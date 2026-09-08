// L^ (lhat) -- the debug adapter: every machine's line hook on one side, DAP
// over a socket on the other.
//
// The shape is all-stop with one reader (09 の 5.1). A single thread owns
// the socket's reading and answers every request; a machine that decides to
// stop parks its own thread on the session's condition and stays frozen
// until the debugger resumes -- which is what makes it safe for the reader
// to walk a parked machine's frames, and to evaluate on one, from its own
// thread: nothing else is touching it, and the lock hands the writes over.
//
// Machines are not registered by anyone: the session watches machine birth
// (lhat_debug_watch_machines), hooks each newborn and tells the debugger a
// thread started. A DAP "thread" is a machine, whatever OS thread machinery
// the host runs it on.

#include "adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/debug.h"
#include "lhat/object.h"
#include "lhat/source.h"
#include "port/socket.h"
#include "port/thread.h"
#include "protocol.h"

#ifdef _WIN32
#include <string.h>
// CreateFileA and GetFinalPathNameByHandleA, for D6's link resolution.
#include <windows.h>
#define path_equal(a, b) (_stricmp((a), (b)) == 0)
#else
#include <limits.h>
#define path_equal(a, b) (strcmp((a), (b)) == 0)
#endif

// The socket shims transport.c reads and writes through (defined at the end).
static size_t socket_read(void *context, char *buffer, size_t size);
static bool socket_write(void *context, const char *bytes, size_t size);

// How many machines a session will follow. One past the cap runs unhooked
// and undebugged rather than failing anything.
#define DAP_MAX_THREADS 64

// variablesReference space: below this is a frame scope (frame_ref * 2 + 2
// for locals, + 3 for captures, where frame_ref = threadId * 1000 + level);
// from here up, a handed-out table.
#define DAP_TABLE_REFS 100000

// How a source line and the file it is in name one breakpoint.
typedef struct {
    char *source;  // path-map unit spelling or normalized path, owned
    uint32_t line;
    // D8: NULL is unconditional. A condition is evaluated in the frame
    // which reached this line, and only true^ lets the breakpoint stop it.
    char *condition;
} DapBreak;

typedef enum {
    DAP_RUN,
    DAP_STEP_IN,
    DAP_STEP_OVER,
    DAP_STEP_OUT
} DapMode;

// One machine the session follows. The hook's context, so it lives until
// the machine dies or the session ends -- never freed while hooked.
typedef struct {
    struct DapSession *session;
    LhatMachine *machine;
    int id;  // the DAP threadId
    DapMode mode;
    size_t step_depth;
    bool parked;  // its thread waits on the session's condition
    // D5: a fault is terminal, but the debugger parks here first so its
    // frames and bindings can be read. Keep its text: evaluating while
    // stopped is a nested run and may clear the machine's transient record.
    bool faulted;
    char fault_text[256];
} DapThread;

// A value a variables request handed a reference out for, with the machine
// it lives on (a parked one -- its registers root it until the resume).
typedef struct {
    LhatValue value;
    LhatMachine *machine;
} DapVar;

struct DapSession {
    // The socket, and the one lock every write to it takes. Innermost:
    // taken while `lock` is held, never the other way around.
    DapPeer peer;
    LhatMutex write_lock;
    LhatSocket listener;
    LhatSocket socket;
    // 09 の 5.2: the host's spelling map, zeroed when none was given --
    // both sides are filesystem paths then, and are normalized to compare.
    DapPathMap paths;
    // D8: compiled, before the DAP handshake starts, and alive until after
    // the run. Its line tables tell setBreakpoints where code can run.
    const LhatProgram *program;

    // Everything below is under `lock`; `changed` is broadcast whenever
    // stopping/ended/thread_count moves.
    LhatMutex lock;
    LhatCondition changed;

    DapThread *threads[DAP_MAX_THREADS];
    size_t thread_count;
    int next_id;

    DapBreak *breaks;
    size_t break_count;
    size_t break_capacity;

    bool stopping;   // someone parked; every machine parks at its next line
    bool pause_all;  // a pause was asked for; the next line is a stop
    bool configured;
    bool ended;       // the session is over; every hook panics its machine
    bool peer_ended;  // and it was the debugger's own doing (or its exit)

    DapVar *vars;
    size_t var_count;
    size_t var_capacity;

    // The last (source pointer, its normalized form) pair, so a run does
    // not realpath the same unit on every line. Under `lock`.
    const char *cached_source;
    char *cached_normal;

    LhatThread reader;
    bool reader_started;
};

// ---------------------------------------------------------------------------
// Threads (machines)

static DapThread *thread_by_id(DapSession *s, int id)
{
    for (size_t i = 0; i < s->thread_count; i++) {
        if (s->threads[i]->id == id) {
            return s->threads[i];
        }
    }
    return NULL;
}

static DapThread *thread_of(DapSession *s, LhatMachine *machine)
{
    for (size_t i = 0; i < s->thread_count; i++) {
        if (s->threads[i]->machine == machine) {
            return s->threads[i];
        }
    }
    return NULL;
}

static int frame_ref(const DapThread *t, int level)
{
    return t->id * 1000 + level;
}

static void send_thread_event(DapSession *s, int id, const char *reason)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "threadId", id);
    lhat_mutex_lock(&s->write_lock);
    dap_event(&s->peer, "thread", body);
    lhat_mutex_unlock(&s->write_lock);
}

static void dap_hook(LhatMachine *machine, void *context, LhatDebugEvent event,
                     const LhatFrameInfo *where);

// The hook goes on before the machine has run anything, and the debugger
// hears of the thread -- the whole of what a host would otherwise wire.
static DapThread *add_thread(DapSession *s, LhatMachine *machine, bool announce)
{
    if (s->thread_count >= DAP_MAX_THREADS) {
        return NULL;  // it runs, undebugged
    }
    DapThread *t = (DapThread *)calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->session = s;
    t->machine = machine;
    t->id = s->next_id++;
    s->threads[s->thread_count++] = t;
    lhat_machine_set_debug_hook(machine, dap_hook, t);
    if (announce) {
        send_thread_event(s, t->id, "started");
    }
    return t;
}

static void remove_thread(DapSession *s, DapThread *t, bool announce)
{
    lhat_machine_set_debug_hook(t->machine, NULL, NULL);
    for (size_t i = 0; i < s->thread_count; i++) {
        if (s->threads[i] == t) {
            s->threads[i] = s->threads[--s->thread_count];
            break;
        }
    }
    if (announce) {
        send_thread_event(s, t->id, "exited");
    }
    free(t);
    lhat_condition_broadcast(&s->changed);
}

static void machine_born(void *context, LhatMachine *machine)
{
    DapSession *s = (DapSession *)context;
    lhat_mutex_lock(&s->lock);
    if (!s->ended) {
        add_thread(s, machine, true);
    }
    lhat_mutex_unlock(&s->lock);
}

static void machine_dying(void *context, LhatMachine *machine)
{
    DapSession *s = (DapSession *)context;
    lhat_mutex_lock(&s->lock);
    DapThread *t = thread_of(s, machine);
    if (t != NULL) {
        remove_thread(s, t, true);
    }
    lhat_mutex_unlock(&s->lock);
}

// ---------------------------------------------------------------------------
// Paths

static char *own_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

#ifdef _WIN32
// 09 の D6: _fullpath canonicalises a spelling -- '.', '..', a relative
// start, the separators -- but walks no reparse point, so a junction or a
// symbolic link keeps its own name and two spellings of one file compare
// unequal. A breakpoint set through the link then binds to nothing. POSIX
// has had this for free all along: realpath resolves links.
//
// Opening the file is what resolves them, since the filesystem does the walk
// on the way in. No elevation is involved -- reading where a link points is
// not a privileged act, only making one is -- and a desired access of 0 asks
// for metadata alone, so no read permission is needed either.
// FILE_FLAG_BACKUP_SEMANTICS is what lets a directory be opened at all.
//
// Narrow on purpose. Every other path in the tree goes through fopen
// (src/source.c, lsp/workspace.c), so this reads a path the same way the
// rest of the program does; a wide call here would resolve names nothing
// else could then open.
//
// NULL when the path names nothing that can be opened -- an editor may well
// ask about a file that has since been moved -- and the caller keeps
// _fullpath's spelling for it.
static char *resolve_links(const char *path)
{
    HANDLE handle = CreateFileA(
        path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    // The documented two-call shape: a short answer fits, and a long one
    // reports the room it needs (that count includes the terminator, the
    // successful one does not).
    const DWORD kind = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    char small[MAX_PATH];
    char *answer = small;
    DWORD room = (DWORD)sizeof small;
    DWORD wrote = GetFinalPathNameByHandleA(handle, small, room, kind);
    if (wrote >= room) {
        answer = (char *)malloc(wrote);
        room = answer != NULL ? wrote : 0;
        wrote = answer != NULL
                    ? GetFinalPathNameByHandleA(handle, answer, room, kind)
                    : 0;
    }
    CloseHandle(handle);
    if (wrote == 0 || wrote >= room) {
        if (answer != small) {
            free(answer);
        }
        return NULL;
    }
    // What comes back wears the extended prefix. Every other path here is
    // written the way a person writes one, and the two must compare equal.
    const char *shown = answer;
    char unc[MAX_PATH * 4];
    if (strncmp(answer, "\\\\?\\UNC\\", 8) == 0) {
        // \\?\UNC\server\share -> \\server\share
        if (snprintf(unc, sizeof unc, "\\\\%s", answer + 8) < (int)sizeof unc) {
            shown = unc;
        }
    } else if (strncmp(answer, "\\\\?\\", 4) == 0) {
        shown = answer + 4;
    }
    char *copy = _strdup(shown);
    if (answer != small) {
        free(answer);
    }
    return copy;
}
#endif

char *dap_normalize_path(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
    if (full == NULL) {
        return _strdup(path);
    }
    char *resolved = resolve_links(full);
    if (resolved == NULL) {
        return full;
    }
    free(full);
    return resolved;
#else
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        return strdup(resolved);
    }
    return strdup(path);
#endif
}

// The normalized form of `source`, cached against its (stable) pointer.
static const char *normal_of(DapSession *s, const char *source)
{
    if (source == NULL) {
        return NULL;
    }
    if (source != s->cached_source) {
        free(s->cached_normal);
        s->cached_normal = dap_normalize_path(source);
        s->cached_source = source;
    }
    return s->cached_normal;
}

static bool source_equal(const DapSession *s, const char *left,
                         const char *right)
{
    return s->paths.to_unit != NULL ? strcmp(left, right) == 0
                                    : path_equal(left, right);
}

uint32_t dap_column_of_line(const char *text, size_t length, uint32_t line)
{
    if (text == NULL || line == 0) {
        return 1;
    }
    // To the start of the line the frame is on. Lines are 1-based, and the
    // text has been normalised to LF, so one '\n' is one line boundary.
    size_t at = 0;
    for (uint32_t seen = 1; seen < line; seen++) {
        while (at < length && text[at] != '\n') {
            at++;
        }
        if (at == length) {
            return 1;  // past the end: no line to read a column off
        }
        at++;
    }
    // Past the blanks. A line that is blank or all blanks answers 1 rather
    // than the column of its own end, which would put the mark past what a
    // reader can see.
    size_t first = at;
    while (first < length && (text[first] == ' ' || text[first] == '\t')) {
        first++;
    }
    if (first == length || text[first] == '\n') {
        return 1;
    }
    return (uint32_t)(first - at) + 1;
}

// The text of the unit `path` names. `path` is the runtime's own spelling --
// proto->source_name, stamped from the unit's path by program.c -- so the
// two compare directly, and 5.2's map does not come into it: that one is for
// showing a file to the editor. NULL when the program holds no such unit, or
// it came from bytecode with no text behind it.
static const LhatSource *text_of(const DapSession *s, const char *path)
{
    if (s->program == NULL || path == NULL) {
        return NULL;
    }
    for (const LhatUnit *unit = lhat_program_units(s->program); unit != NULL;
         unit = lhat_unit_next(unit)) {
        const char *held = lhat_unit_path(unit);
        if (held == NULL || strcmp(held, path) != 0) {
            continue;
        }
        const LhatSource *source = lhat_unit_source(unit);
        return source != NULL && source->text != NULL ? source : NULL;
    }
    return NULL;
}

// The compiled unit tree is the authority on what an editor line can mean.
// A path-map's key is already the unit spelling; without one both unit and
// editor paths are normalised before this comparison.
static uint32_t next_executable_line(DapSession *s, const char *key,
                                     uint32_t requested)
{
    if (s->program == NULL) {
        return 0;
    }
    for (const LhatUnit *unit = lhat_program_units(s->program); unit != NULL;
         unit = lhat_unit_next(unit)) {
        const char *path = lhat_unit_path(unit);
        if (path == NULL) {
            continue;
        }
        bool matches = false;
        if (s->paths.to_unit != NULL) {
            matches = strcmp(path, key) == 0;
        } else {
            // This runs only during setBreakpoints, never at every VM
            // instruction, so a per-unit normalization needs no cache.
            char *normal = dap_normalize_path(path);
            matches = normal != NULL && path_equal(normal, key);
            free(normal);
        }
        if (matches) {
            return lhat_proto_next_instruction_line(lhat_unit_proto(unit),
                                                    requested);
        }
    }
    return 0;
}

static bool condition_is_true(LhatMachine *machine, const char *condition)
{
    if (condition == NULL) {
        return true;
    }
    LhatValue answer = lhat_nil();
    char error[256];
    // Evaluating from a hook is what lhat_machine_evaluate is designed for:
    // it suppresses nested line/fault events and restores its own failures.
    // A DAP condition is a boolean; bad or non-boolean expressions simply do
    // not match, rather than turning a filtered breakpoint unconditional.
    return lhat_machine_evaluate(machine, 0, condition, strlen(condition),
                                 &answer, error, sizeof error) &&
           lhat_is_bool(answer) && lhat_as_bool(answer);
}

static bool at_breakpoint(DapSession *s, LhatMachine *machine,
                          const LhatFrameInfo *where)
{
    if (s->break_count == 0 || where->source == NULL) {
        return false;
    }
    // 09 の 5.2: with a path map the breakpoint keys are unit spellings --
    // the very thing the frame's source is -- and match exactly. With none
    // they are normalized filesystem paths, so the source is normalized
    // (cached) to meet them.
    bool mapped = s->paths.to_unit != NULL;
    const char *here = mapped ? where->source : normal_of(s, where->source);
    if (here == NULL) {
        return false;
    }
    for (size_t i = 0; i < s->break_count; i++) {
        if (s->breaks[i].line == where->line &&
            source_equal(s, s->breaks[i].source, here) &&
            condition_is_true(machine, s->breaks[i].condition)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Variable references

static int add_var(DapSession *s, LhatMachine *machine, LhatValue value)
{
    if (s->var_count == s->var_capacity) {
        size_t grown = s->var_capacity ? s->var_capacity * 2 : 16;
        DapVar *bigger = (DapVar *)realloc(s->vars, grown * sizeof *bigger);
        if (bigger == NULL) {
            return 0;
        }
        s->vars = bigger;
        s->var_capacity = grown;
    }
    s->vars[s->var_count].value = value;
    s->vars[s->var_count].machine = machine;
    return DAP_TABLE_REFS + (int)s->var_count++;
}

static void clear_vars(DapSession *s)
{
    s->var_count = 0;
}

// One variable, expandable when it is a table.
static cJSON *variable_json(DapSession *s, LhatMachine *machine,
                            const char *name, LhatValue value)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "name", name);

    char text[256];
    lhat_value_text(value, text, sizeof text);
    cJSON_AddStringToObject(out, "value", text);

    const LhatRuntimeType *type = lhat_value_type(machine, value);
    if (type != NULL) {
        char spelt[128];
        lhat_runtime_type_write(type, spelt, sizeof spelt);
        cJSON_AddStringToObject(out, "type", spelt);
    }

    int reference = 0;
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        reference = add_var(s, machine, value);
    }
    cJSON_AddNumberToObject(out, "variablesReference", reference);
    return out;
}

// The members of a table, dense part first then the map part.
static cJSON *expand_table(DapSession *s, LhatMachine *machine,
                           LhatValue value)
{
    cJSON *out = cJSON_CreateArray();
    const LhatTable *table = (const LhatTable *)lhat_as_object(value);
    for (size_t i = 0; i < table->array_count; i++) {
        char name[32];
        snprintf(name, sizeof name, "%zu", i + 1);
        cJSON_AddItemToArray(
            out,
            variable_json(s, machine, name, lhat_slots_get(table->array, i)));
    }
    for (size_t i = 0; i < table->entry_capacity; i++) {
        LhatValue key = table->entries[i].key;
        if (lhat_is_nil(key)) {
            continue;
        }
        char name[128];
        lhat_value_text(key, name, sizeof name);
        cJSON_AddItemToArray(
            out, variable_json(s, machine, name, table->entries[i].value));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reading and writing a frame

// The DAP name of a frame -- what the traceback calls it.
static const char *frame_name(const LhatFrameInfo *info)
{
    if (info->name != NULL) {
        return info->name;
    }
    return info->top_level ? "(top level)" : "f^";
}

static void stack_trace(DapSession *s, DapThread *t, cJSON *body)
{
    cJSON *frames = cJSON_CreateArray();
    int depth = 0;
    // A machine that is not parked is running (or blocked in a host call)
    // and its frames are not this thread's to walk: it answers empty.
    if (t != NULL && t->parked) {
        depth = (int)lhat_machine_fault_depth(t->machine);
        for (int level = 0; level < depth; level++) {
            LhatFrameInfo info;
            if (!lhat_machine_fault_frame(t->machine, (size_t)level, &info)) {
                break;
            }
            cJSON *frame = cJSON_CreateObject();
            cJSON_AddNumberToObject(frame, "id", frame_ref(t, level));
            cJSON_AddStringToObject(frame, "name", frame_name(&info));
            if (info.source != NULL) {
                // 09 の 5.2: reported in the editor's spelling when the
                // host's map has one, so the debugger can open the file.
                const char *shown = info.source;
                char mapped[512];
                if (s->paths.to_editor != NULL &&
                    s->paths.to_editor(s->paths.context, info.source, mapped,
                                       sizeof mapped)) {
                    shown = mapped;
                }
                cJSON *source = cJSON_CreateObject();
                cJSON_AddStringToObject(source, "path", shown);
                cJSON_AddItemToObject(frame, "source", source);
            }
            cJSON_AddNumberToObject(frame, "line", info.line);
            // 09 の 5.3: the column is read off the source rather than kept
            // per instruction. One pass over the text per frame -- this runs
            // when a person asked where they are, never at an instruction,
            // so the scan is not worth a cache.
            const LhatSource *text = text_of(s, info.source);
            cJSON_AddNumberToObject(
                frame, "column",
                text != NULL
                    ? dap_column_of_line(text->text, text->length, info.line)
                    : 1);
            cJSON_AddItemToArray(frames, frame);
        }
    }
    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", depth);
}

// What a debugger typed as a new value, read the way L^ spells values:
// nil^, true^, false^, a number, or a quoted string (no escapes -- the
// panel edits short values; an expression is evaluate's).
static bool parse_value(LhatMachine *machine, const char *text, LhatValue *out)
{
    while (*text == ' ') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == ' ') {
        length--;
    }
    if (length == 4 && strncmp(text, "nil^", 4) == 0) {
        *out = lhat_nil();
        return true;
    }
    if (length == 5 && strncmp(text, "true^", 5) == 0) {
        *out = lhat_bool(true);
        return true;
    }
    if (length == 6 && strncmp(text, "false^", 6) == 0) {
        *out = lhat_bool(false);
        return true;
    }
    if (length >= 2 && text[0] == '"' && text[length - 1] == '"') {
        return lhat_machine_make_string(machine, text + 1, length - 2, out);
    }
    char *end = NULL;
    long long integer = strtoll(text, &end, 10);
    if (end == text + length) {
        *out = lhat_integer(integer);
        return true;
    }
    double real = strtod(text, &end);
    if (end == text + length && end != text) {
        *out = lhat_real(real);
        return true;
    }
    return false;
}

// A table member's key, back from the name a variables request rendered it
// as: a whole number is the sequence key it was, anything else the string.
static bool parse_key(LhatMachine *machine, const char *name, LhatValue *out)
{
    char *end = NULL;
    long long index = strtoll(name, &end, 10);
    if (end != name && *end == '\0') {
        *out = lhat_integer(index);
        return true;
    }
    return lhat_machine_make_string(machine, name, strlen(name), out);
}

// A scope reference decoded: which machine, which level, which half.
static DapThread *decode_scope(DapSession *s, int reference, int *level,
                               bool *captures)
{
    if (reference < 2 || reference >= DAP_TABLE_REFS) {
        return NULL;
    }
    int frame = (reference - 2) / 2;
    *captures = ((reference - 2) % 2) != 0;
    *level = frame % 1000;
    DapThread *t = thread_by_id(s, frame / 1000);
    return t != NULL && t->parked ? t : NULL;
}

// Writes `value` where `reference` and `name` point: a member of a handed-out
// table, or a frame scope's binding -- by name, the innermost when shadowed,
// the same rule the read gave the panel its list under. `machine` answers
// which machine took the write, for the caller to parse the value against
// first (chicken and egg: the value needs the machine, so this is called
// twice -- once with NULL `value_text` semantics avoided by splitting).
static LhatMachine *write_target(DapSession *s, int reference)
{
    if (reference >= DAP_TABLE_REFS) {
        int index = reference - DAP_TABLE_REFS;
        return index >= 0 && (size_t)index < s->var_count
                   ? s->vars[index].machine
                   : NULL;
    }
    int level = 0;
    bool captures = false;
    DapThread *t = decode_scope(s, reference, &level, &captures);
    return t != NULL ? t->machine : NULL;
}

static bool write_variable(DapSession *s, int reference, const char *name,
                           LhatValue value)
{
    if (reference >= DAP_TABLE_REFS) {
        int index = reference - DAP_TABLE_REFS;
        if (index < 0 || (size_t)index >= s->var_count) {
            return false;
        }
        DapVar *var = &s->vars[index];
        LhatValue key;
        bool refused = false;
        return parse_key(var->machine, name, &key) &&
               lhat_machine_table_set(var->machine,
                                      (LhatTable *)lhat_as_object(var->value),
                                      key, value, &refused) &&
               !refused;
    }
    int level = 0;
    bool captures = false;
    DapThread *t = decode_scope(s, reference, &level, &captures);
    if (t == NULL) {
        return false;
    }
    size_t count =
        captures ? lhat_frame_upvalue_count(t->machine, (size_t)level)
                 : lhat_frame_local_count(t->machine, (size_t)level);
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        LhatBindingInfo binding;
        bool read = captures
                        ? lhat_frame_upvalue(t->machine, (size_t)level, i,
                                             &binding)
                        : lhat_frame_local(t->machine, (size_t)level, i,
                                           &binding);
        if (read && strcmp(binding.name, name) == 0) {
            found = i;  // the later of two under one name is the inner
        }
    }
    if (found == SIZE_MAX) {
        return false;
    }
    return captures ? lhat_frame_set_upvalue(t->machine, (size_t)level, found,
                                             value)
                    : lhat_frame_set_local(t->machine, (size_t)level, found,
                                           value);
}

// ---------------------------------------------------------------------------
// Events

// Under `lock`; takes the write lock inside, which is the one order.
static void send_stopped(DapSession *s, int thread_id, const char *reason,
                         const char *description)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", reason);
    if (description != NULL && description[0] != '\0') {
        cJSON_AddStringToObject(body, "description", description);
        cJSON_AddStringToObject(body, "text", description);
    }
    cJSON_AddNumberToObject(body, "threadId", thread_id);
    cJSON_AddBoolToObject(body, "allThreadsStopped", true);
    lhat_mutex_lock(&s->write_lock);
    dap_event(&s->peer, "stopped", body);
    lhat_mutex_unlock(&s->write_lock);
}

static void fault_text(LhatMachine *machine, char *out, size_t capacity)
{
    LhatRunStatus status = lhat_machine_fault_status(machine);
    if (status == LHAT_RUN_PANIC) {
        char value[192];
        lhat_value_text(lhat_machine_fault_value(machine), value,
                        sizeof value);
        snprintf(out, capacity, "panic^ %s", value);
    } else {
        snprintf(out, capacity, "%s", lhat_run_status_message(status));
    }
}

// ---------------------------------------------------------------------------
// Requests. All under `lock` (the handshake and the reader both hold it);
// responses take the write lock inside dap_respond via the locked wrappers.

static bool respond(DapSession *s, const cJSON *request, bool success,
                    cJSON *body)
{
    lhat_mutex_lock(&s->write_lock);
    bool ok = dap_respond(&s->peer, request, success, body);
    lhat_mutex_unlock(&s->write_lock);
    return ok;
}

static bool refuse(DapSession *s, const cJSON *request, const char *message)
{
    lhat_mutex_lock(&s->write_lock);
    bool ok = dap_fail(&s->peer, request, message);
    lhat_mutex_unlock(&s->write_lock);
    return ok;
}

static void dispose_break(DapBreak *point)
{
    free(point->source);
    free(point->condition);
}

// DAP replaces a source's breakpoints, not every breakpoint in a session.
static void clear_breakpoints_for(DapSession *s, const char *source)
{
    if (source == NULL) {
        return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < s->break_count; i++) {
        if (source_equal(s, s->breaks[i].source, source)) {
            dispose_break(&s->breaks[i]);
        } else {
            if (kept != i) {
                s->breaks[kept] = s->breaks[i];
            }
            kept++;
        }
    }
    s->break_count = kept;
}

static bool add_breakpoint(DapSession *s, const char *source, uint32_t line,
                           const char *condition)
{
    if (s->break_count == s->break_capacity) {
        size_t grown = s->break_capacity ? s->break_capacity * 2 : 8;
        DapBreak *bigger =
            (DapBreak *)realloc(s->breaks, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        s->breaks = bigger;
        s->break_capacity = grown;
    }
    DapBreak point = {0};
    point.source = own_text(source);
    point.condition = condition != NULL && condition[0] != '\0'
                          ? own_text(condition)
                          : NULL;
    if (point.source == NULL ||
        (condition != NULL && condition[0] != '\0' && point.condition == NULL)) {
        dispose_break(&point);
        return false;
    }
    point.line = line;
    s->breaks[s->break_count++] = point;
    return true;
}

static void add_breakpoint_response(cJSON *list, bool verified, uint32_t line,
                                    const char *message)
{
    cJSON *mark = cJSON_CreateObject();
    cJSON_AddBoolToObject(mark, "verified", verified);
    if (verified) {
        cJSON_AddNumberToObject(mark, "line", line);
    }
    if (message != NULL) {
        cJSON_AddStringToObject(mark, "message", message);
    }
    cJSON_AddItemToArray(list, mark);
}

static void set_breakpoints(DapSession *s, const cJSON *arguments, cJSON *body)
{
    const cJSON *source = cJSON_GetObjectItem(arguments, "source");
    const cJSON *path = cJSON_GetObjectItem(source, "path");
    // The key a line event will be matched against: the unit spelling the
    // host's map answers (09 の 5.2), or the normalized filesystem path
    // with no map. A file the map does not know binds no breakpoints.
    char *key = NULL;
    if (cJSON_IsString(path)) {
        if (s->paths.to_unit != NULL) {
            char unit[512];
            if (s->paths.to_unit(s->paths.context, path->valuestring, unit,
                                 sizeof unit)) {
                key = own_text(unit);
            }
        } else {
            key = dap_normalize_path(path->valuestring);
        }
    }
    clear_breakpoints_for(s, key);

    cJSON *verified = cJSON_CreateArray();
    const cJSON *lines = cJSON_GetObjectItem(arguments, "breakpoints");
    const cJSON *one = NULL;
    cJSON_ArrayForEach(one, lines) {
        const cJSON *line = cJSON_GetObjectItem(one, "line");
        const cJSON *condition = cJSON_GetObjectItem(one, "condition");
        const char *why = NULL;
        const char *condition_text = NULL;
        uint32_t requested = 0;
        uint32_t actual = 0;
        if (!cJSON_IsNumber(line) || line->valuedouble < 1 ||
            line->valuedouble > UINT32_MAX ||
            (uint32_t)line->valuedouble != line->valuedouble) {
            why = "a breakpoint line must be a positive whole number";
        } else if (key == NULL) {
            why = "the source is not part of this program";
        } else if (condition != NULL && !cJSON_IsString(condition)) {
            why = "a breakpoint condition must be an expression";
        } else {
            requested = (uint32_t)line->valuedouble;
            actual = next_executable_line(s, key, requested);
            condition_text = cJSON_IsString(condition) ? condition->valuestring
                                                        : NULL;
            if (actual == 0) {
                why = "there is no executable line at or after this line";
            } else if (!add_breakpoint(s, key, actual, condition_text)) {
                why = "out of memory while setting the breakpoint";
            }
        }
        add_breakpoint_response(verified, why == NULL, actual, why);
    }
    free(key);
    cJSON_AddItemToObject(body, "breakpoints", verified);
}

// Lets every parked machine go. Modes were set by the step requests first.
static void resume_all(DapSession *s)
{
    s->stopping = false;
    s->pause_all = false;
    clear_vars(s);
    lhat_condition_broadcast(&s->changed);
}

static void dispatch(DapSession *s, const cJSON *request)
{
    const char *command = dap_command(request);
    const cJSON *arguments = dap_arguments(request);

    if (strcmp(command, "initialize") == 0) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", true);
        cJSON_AddBoolToObject(body, "supportsSetVariable", true);
        cJSON_AddBoolToObject(body, "supportsEvaluateForHovers", true);
        cJSON_AddBoolToObject(body, "supportsConditionalBreakpoints", true);
        cJSON_AddBoolToObject(body, "supportsExceptionInfoRequest", true);
        respond(s, request, true, body);
        lhat_mutex_lock(&s->write_lock);
        dap_event(&s->peer, "initialized", NULL);
        lhat_mutex_unlock(&s->write_lock);
        return;
    }
    if (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0) {
        const cJSON *stop = cJSON_GetObjectItem(arguments, "stopOnEntry");
        DapThread *main = thread_by_id(s, 1);
        if (cJSON_IsTrue(stop) && main != NULL) {
            main->mode = DAP_STEP_IN;  // stop at the first line
        }
        respond(s, request, true, NULL);
        return;
    }
    if (strcmp(command, "setBreakpoints") == 0) {
        cJSON *body = cJSON_CreateObject();
        set_breakpoints(s, arguments, body);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "setExceptionBreakpoints") == 0) {
        // D5: an L^ runtime fault is always terminal, so every one stops for
        // inspection. There are no separate caught/uncaught classes for the
        // client to select; accepting this standard request says exactly that.
        respond(s, request, true, NULL);
        return;
    }
    if (strcmp(command, "configurationDone") == 0) {
        s->configured = true;
        respond(s, request, true, NULL);
        return;
    }
    if (strcmp(command, "threads") == 0) {
        cJSON *body = cJSON_CreateObject();
        cJSON *threads = cJSON_CreateArray();
        for (size_t i = 0; i < s->thread_count; i++) {
            cJSON *one = cJSON_CreateObject();
            cJSON_AddNumberToObject(one, "id", s->threads[i]->id);
            char name[32];
            if (s->threads[i]->id == 1) {
                snprintf(name, sizeof name, "main");
            } else {
                snprintf(name, sizeof name, "machine %d", s->threads[i]->id);
            }
            cJSON_AddStringToObject(one, "name", name);
            cJSON_AddItemToArray(threads, one);
        }
        cJSON_AddItemToObject(body, "threads", threads);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "stackTrace") == 0) {
        const cJSON *thread_id = cJSON_GetObjectItem(arguments, "threadId");
        DapThread *t = thread_by_id(
            s, cJSON_IsNumber(thread_id) ? (int)thread_id->valuedouble : 1);
        cJSON *body = cJSON_CreateObject();
        stack_trace(s, t, body);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "exceptionInfo") == 0) {
        const cJSON *thread_id = cJSON_GetObjectItem(arguments, "threadId");
        DapThread *t = thread_by_id(
            s, cJSON_IsNumber(thread_id) ? (int)thread_id->valuedouble : 1);
        if (t == NULL || !t->parked || !t->faulted) {
            refuse(s, request, "that machine did not stop on a runtime fault");
            return;
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "exceptionId", "lhat.runtimeFault");
        cJSON_AddStringToObject(body, "description", t->fault_text);
        cJSON_AddStringToObject(body, "breakMode", "always");
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "scopes") == 0) {
        const cJSON *frame_id = cJSON_GetObjectItem(arguments, "frameId");
        int frame = cJSON_IsNumber(frame_id) ? (int)frame_id->valuedouble : 0;
        DapThread *t = thread_by_id(s, frame / 1000);
        int level = frame % 1000;
        cJSON *list = cJSON_CreateArray();
        if (t != NULL && t->parked) {
            cJSON *locals = cJSON_CreateObject();
            cJSON_AddStringToObject(locals, "name", "Locals");
            cJSON_AddNumberToObject(locals, "variablesReference",
                                    frame * 2 + 2);
            cJSON_AddBoolToObject(locals, "expensive", false);
            cJSON_AddItemToArray(list, locals);
            if (lhat_frame_upvalue_count(t->machine, (size_t)level) > 0) {
                cJSON *captures = cJSON_CreateObject();
                cJSON_AddStringToObject(captures, "name", "Captures");
                cJSON_AddNumberToObject(captures, "variablesReference",
                                        frame * 2 + 3);
                cJSON_AddBoolToObject(captures, "expensive", false);
                cJSON_AddItemToArray(list, captures);
            }
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "scopes", list);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "variables") == 0) {
        const cJSON *reference =
            cJSON_GetObjectItem(arguments, "variablesReference");
        int ref = cJSON_IsNumber(reference) ? (int)reference->valuedouble : 0;
        cJSON *list = NULL;
        if (ref >= DAP_TABLE_REFS) {
            int index = ref - DAP_TABLE_REFS;
            list = index >= 0 && (size_t)index < s->var_count
                       ? expand_table(s, s->vars[index].machine,
                                      s->vars[index].value)
                       : cJSON_CreateArray();
        } else {
            list = cJSON_CreateArray();
            int level = 0;
            bool captures = false;
            DapThread *t = decode_scope(s, ref, &level, &captures);
            size_t count =
                t == NULL ? 0
                : captures
                    ? lhat_frame_upvalue_count(t->machine, (size_t)level)
                    : lhat_frame_local_count(t->machine, (size_t)level);
            for (size_t i = 0; i < count; i++) {
                LhatBindingInfo binding;
                bool read =
                    captures ? lhat_frame_upvalue(t->machine, (size_t)level,
                                                  i, &binding)
                             : lhat_frame_local(t->machine, (size_t)level, i,
                                                &binding);
                if (read) {
                    cJSON_AddItemToArray(
                        list, variable_json(s, t->machine, binding.name,
                                            binding.value));
                }
            }
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "variables", list);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "evaluate") == 0) {
        const cJSON *expression = cJSON_GetObjectItem(arguments, "expression");
        const cJSON *frame_id = cJSON_GetObjectItem(arguments, "frameId");
        int frame = cJSON_IsNumber(frame_id) ? (int)frame_id->valuedouble
                                             : 1000;  // main, level 0
        DapThread *t = thread_by_id(s, frame / 1000);
        if (!cJSON_IsString(expression)) {
            refuse(s, request, "no expression");
            return;
        }
        if (t == NULL || !t->parked) {
            refuse(s, request, "that machine is not stopped");
            return;
        }
        char why[256];
        LhatValue value = lhat_nil();
        if (!lhat_machine_evaluate(t->machine, (size_t)(frame % 1000),
                                   expression->valuestring,
                                   strlen(expression->valuestring), &value,
                                   why, sizeof why)) {
            refuse(s, request, why);
            return;
        }
        cJSON *body = cJSON_CreateObject();
        char rendered[256];
        lhat_value_text(value, rendered, sizeof rendered);
        cJSON_AddStringToObject(body, "result", rendered);
        // Rendered, not handed out for expansion: nothing roots what an
        // evaluation answered once its frame is gone, and a reference read
        // later -- after another evaluation's collection -- would be stale.
        cJSON_AddNumberToObject(body, "variablesReference", 0);
        respond(s, request, true, body);
        return;
    }
    if (strcmp(command, "setVariable") == 0) {
        const cJSON *reference =
            cJSON_GetObjectItem(arguments, "variablesReference");
        const cJSON *name = cJSON_GetObjectItem(arguments, "name");
        const cJSON *text = cJSON_GetObjectItem(arguments, "value");
        LhatValue value = lhat_nil();
        LhatMachine *machine =
            cJSON_IsNumber(reference)
                ? write_target(s, (int)reference->valuedouble)
                : NULL;
        bool wrote = machine != NULL && cJSON_IsString(name) &&
                     cJSON_IsString(text) &&
                     parse_value(machine, text->valuestring, &value) &&
                     write_variable(s, (int)reference->valuedouble,
                                    name->valuestring, value);
        cJSON *body = NULL;
        if (wrote) {
            body = cJSON_CreateObject();
            char rendered[256];
            lhat_value_text(value, rendered, sizeof rendered);
            cJSON_AddStringToObject(body, "value", rendered);
        }
        respond(s, request, wrote, body);
        return;
    }
    if (strcmp(command, "continue") == 0) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "allThreadsContinued", true);
        respond(s, request, true, body);
        resume_all(s);
        return;
    }
    if (strcmp(command, "next") == 0 || strcmp(command, "stepIn") == 0 ||
        strcmp(command, "stepOut") == 0) {
        const cJSON *thread_id = cJSON_GetObjectItem(arguments, "threadId");
        DapThread *t = thread_by_id(
            s, cJSON_IsNumber(thread_id) ? (int)thread_id->valuedouble : 1);
        if (t != NULL) {
            t->mode = command[0] == 'n' ? DAP_STEP_OVER
                      : strcmp(command, "stepIn") == 0 ? DAP_STEP_IN
                                                       : DAP_STEP_OUT;
            if (t->parked) {
                t->step_depth = lhat_machine_fault_depth(t->machine);
            }
        }
        respond(s, request, true, NULL);
        resume_all(s);
        return;
    }
    if (strcmp(command, "pause") == 0) {
        s->pause_all = true;
        respond(s, request, true, NULL);
        return;
    }
    if (strcmp(command, "disconnect") == 0 ||
        strcmp(command, "terminate") == 0) {
        respond(s, request, true, NULL);
        s->ended = true;
        s->peer_ended = true;
        lhat_condition_broadcast(&s->changed);
        return;
    }
    // Anything else is answered, unsupported, so the client is not left
    // waiting on a request this adapter does not know.
    respond(s, request, false, NULL);
}

// ---------------------------------------------------------------------------
// The reader: the one thread that owns the socket's reading. Every parked
// machine is passive data under the lock while it answers.

static int reader_main(void *argument)
{
    DapSession *s = (DapSession *)argument;
    for (;;) {
        cJSON *request = dap_read(&s->peer);
        if (request == NULL) {
            lhat_mutex_lock(&s->lock);
            if (!s->ended) {
                s->ended = true;
                s->peer_ended = true;  // the peer is gone
            }
            lhat_condition_broadcast(&s->changed);
            lhat_mutex_unlock(&s->lock);
            return 0;
        }
        lhat_mutex_lock(&s->lock);
        bool over = s->ended;
        if (!over) {
            dispatch(s, request);
            over = s->ended;
        }
        lhat_mutex_unlock(&s->lock);
        cJSON_Delete(request);
        if (over) {
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
// The hook: every machine's thread comes through here at each new line and
// at a host function's explicitly reported cooperative boundary.

static void dap_hook(LhatMachine *machine, void *context, LhatDebugEvent event,
                     const LhatFrameInfo *where)
{
    DapThread *t = (DapThread *)context;
    DapSession *s = t->session;

    lhat_mutex_lock(&s->lock);
    if (s->ended) {
        lhat_mutex_unlock(&s->lock);
        if (!lhat_machine_panic_text(machine, "stopped by the debugger")) {
            lhat_machine_panic(machine, lhat_nil());
        }
        return;
    }

    size_t depth = lhat_machine_fault_depth(machine);
    bool fault = event == LHAT_DEBUG_FAULT;
    bool host_point = event == LHAT_DEBUG_HOST_PAUSE_POINT;
    if (fault) {
        t->faulted = true;
        fault_text(machine, t->fault_text, sizeof t->fault_text);
    }
    // A host point is a boundary a host selected, not a source instruction:
    // it completes a requested/all-thread pause and observes session end,
    // but cannot consume a step or hit a source breakpoint.
    bool stop = fault || s->stopping || s->pause_all ||
                (!host_point &&
                 (t->mode == DAP_STEP_IN ||
                  (t->mode == DAP_STEP_OVER && depth <= t->step_depth) ||
                  (t->mode == DAP_STEP_OUT && depth < t->step_depth) ||
                  at_breakpoint(s, machine, where)));
    if (!stop) {
        lhat_mutex_unlock(&s->lock);
        return;
    }

    // The first to park is the stop the debugger hears about; the rest park
    // silently -- allThreadsStopped said it all.
    const char *reason = fault ? "exception"
                         : s->pause_all ? "pause"
                         : t->mode != DAP_RUN ? "step"
                                              : "breakpoint";
    t->mode = DAP_RUN;
    if (!s->stopping) {
        s->stopping = true;
        s->pause_all = false;
        clear_vars(s);
        send_stopped(s, t->id, reason, fault ? t->fault_text : NULL);
    }
    t->parked = true;
    while (s->stopping && !s->ended) {
        lhat_condition_wait(&s->changed, &s->lock);
    }
    t->parked = false;
    bool over = s->ended;
    lhat_mutex_unlock(&s->lock);
    if (over) {
        if (!lhat_machine_panic_text(machine, "stopped by the debugger")) {
            lhat_machine_panic(machine, lhat_nil());
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle

bool dap_session_begin(DapSession **out, LhatMachine *machine,
                       const LhatProgram *program, uint16_t port,
                       const DapPathMap *paths)
{
    *out = NULL;
    if (!lhat_socket_startup()) {
        return false;
    }
    DapSession *s = (DapSession *)calloc(1, sizeof *s);
    if (s == NULL) {
        lhat_socket_cleanup();
        return false;
    }
    if (paths != NULL) {
        s->paths = *paths;
    }
    s->program = program;
    s->peer.seq = 1;
    s->next_id = 1;
    lhat_mutex_init(&s->lock);
    lhat_mutex_init(&s->write_lock);
    lhat_condition_init(&s->changed);

    if (lhat_socket_listen(&s->listener, port)) {
        // 09 の 7 章: whoever started this process has no other way to know
        // when to connect. The socket goes up after the program is loaded
        // and checked, which takes as long as the program is large, and the
        // accept below then blocks until a debugger arrives -- so an editor
        // that guessed the moment would guess wrong on a big program and
        // race on a small one. One line, on the stream the protocol does not
        // use, before anything can block.
        fprintf(stderr, "lhat: dap listening on %u\n", (unsigned)port);
        fflush(stderr);
    }
    if (s->listener.handle == 0 || !lhat_socket_accept(s->listener, &s->socket)) {
        if (s->listener.handle != 0) {
            lhat_socket_close(s->listener);
        }
        free(s);
        lhat_socket_cleanup();
        return false;
    }

    // The stream is the socket, through the two shims below.
    s->peer.stream.context = s;
    s->peer.stream.read = socket_read;
    s->peer.stream.write = socket_write;

    // The machine in hand is thread 1; every machine born from here on is
    // followed the same way, whoever makes it and on whatever thread.
    lhat_mutex_lock(&s->lock);
    add_thread(s, machine, false);
    lhat_mutex_unlock(&s->lock);
    LhatMachineWatcher watcher;
    watcher.context = s;
    watcher.born = machine_born;
    watcher.dying = machine_dying;
    lhat_debug_watch_machines(&watcher);

    // The handshake: answer requests until configurationDone, on this
    // thread -- the reader takes over from there.
    while (!s->configured) {
        cJSON *request = dap_read(&s->peer);
        if (request == NULL) {
            break;  // the debugger left before it started
        }
        lhat_mutex_lock(&s->lock);
        dispatch(s, request);
        lhat_mutex_unlock(&s->lock);
        cJSON_Delete(request);
    }

    s->reader_started = lhat_thread_start(&s->reader, reader_main, s);
    *out = s;
    return true;
}

void dap_session_end(DapSession *session, int exit_code)
{
    if (session == NULL) {
        return;
    }
    DapSession *s = session;

    lhat_mutex_lock(&s->lock);
    bool peer_ended = s->peer_ended;
    s->ended = true;
    lhat_condition_broadcast(&s->changed);
    // The workers' machines go when their runs end -- the broadcast has
    // every parked one wake and panic, and every running one panics at its
    // next line. Each dying machine takes itself off the list; the main
    // machine (still the caller's) is the one left.
    while (s->thread_count > 1) {
        lhat_condition_wait(&s->changed, &s->lock);
    }
    if (s->thread_count == 1) {
        remove_thread(s, s->threads[0], false);
    }
    lhat_mutex_unlock(&s->lock);
    lhat_debug_watch_machines(NULL);

    if (!peer_ended) {
        lhat_mutex_lock(&s->write_lock);
        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "exitCode", exit_code);
        dap_event(&s->peer, "exited", body);
        dap_event(&s->peer, "terminated", NULL);
        lhat_mutex_unlock(&s->write_lock);
    }

    // Closing the socket is what unblocks a reader waiting in recv.
    lhat_socket_close(s->socket);
    lhat_socket_close(s->listener);
    if (s->reader_started) {
        lhat_thread_join(&s->reader);
    }
    lhat_socket_cleanup();

    for (size_t i = 0; i < s->break_count; i++) {
        dispose_break(&s->breaks[i]);
    }
    free(s->breaks);
    free(s->vars);
    free(s->cached_normal);
    lhat_condition_destroy(&s->changed);
    lhat_mutex_destroy(&s->write_lock);
    lhat_mutex_destroy(&s->lock);
    free(s);
}

bool dap_session_ended_run(const DapSession *session)
{
    return session != NULL && session->peer_ended;
}

// The socket shims transport.c reads and writes through.
static size_t socket_read(void *context, char *buffer, size_t size)
{
    DapSession *s = (DapSession *)context;
    long got = lhat_socket_recv(s->socket, buffer, size);
    return got > 0 ? (size_t)got : 0;
}

static bool socket_write(void *context, const char *bytes, size_t size)
{
    DapSession *s = (DapSession *)context;
    return lhat_socket_send_all(s->socket, bytes, size);
}
