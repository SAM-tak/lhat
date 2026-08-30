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

#include <stdlib.h>
#include <string.h>

#include "lhat/debug.h"
#include "lhat/object.h"
#include "port/socket.h"
#include "port/thread.h"
#include "protocol.h"

#ifdef _WIN32
#include <string.h>
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
    char *source;  // normalized absolute path, owned
    int line;
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

static char *normalize(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
    return full != NULL ? full : _strdup(path);
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
        s->cached_normal = normalize(source);
        s->cached_source = source;
    }
    return s->cached_normal;
}

static bool at_breakpoint(DapSession *s, const LhatFrameInfo *where)
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
        if (s->breaks[i].line == (int)where->line &&
            (mapped ? strcmp(s->breaks[i].source, here) == 0
                    : path_equal(s->breaks[i].source, here))) {
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
            cJSON_AddNumberToObject(frame, "column", 1);
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
static void send_stopped(DapSession *s, int thread_id, const char *reason)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "threadId", thread_id);
    cJSON_AddBoolToObject(body, "allThreadsStopped", true);
    lhat_mutex_lock(&s->write_lock);
    dap_event(&s->peer, "stopped", body);
    lhat_mutex_unlock(&s->write_lock);
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

static void set_breakpoints(DapSession *s, const cJSON *arguments, cJSON *body)
{
    for (size_t i = 0; i < s->break_count; i++) {
        free(s->breaks[i].source);
    }
    s->break_count = 0;

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
            key = normalize(path->valuestring);
        }
    }

    cJSON *verified = cJSON_CreateArray();
    const cJSON *lines = cJSON_GetObjectItem(arguments, "breakpoints");
    const cJSON *one = NULL;
    cJSON_ArrayForEach(one, lines) {
        const cJSON *line = cJSON_GetObjectItem(one, "line");
        if (!cJSON_IsNumber(line) || key == NULL) {
            continue;
        }
        if (s->break_count == s->break_capacity) {
            size_t grown = s->break_capacity ? s->break_capacity * 2 : 8;
            DapBreak *bigger =
                (DapBreak *)realloc(s->breaks, grown * sizeof *bigger);
            if (bigger == NULL) {
                break;
            }
            s->breaks = bigger;
            s->break_capacity = grown;
        }
        s->breaks[s->break_count].source = own_text(key);
        s->breaks[s->break_count].line = (int)line->valuedouble;
        s->break_count++;

        // v1 verifies every line it was given (09 の 5; D8 leaves checking
        // the line against the code for later).
        cJSON *mark = cJSON_CreateObject();
        cJSON_AddBoolToObject(mark, "verified", true);
        cJSON_AddNumberToObject(mark, "line", (int)line->valuedouble);
        cJSON_AddItemToArray(verified, mark);
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
// The hook: every machine's thread comes through here at each new line.

static void dap_hook(LhatMachine *machine, void *context, LhatDebugEvent event,
                     const LhatFrameInfo *where)
{
    (void)event;
    DapThread *t = (DapThread *)context;
    DapSession *s = t->session;

    lhat_mutex_lock(&s->lock);
    if (s->ended) {
        lhat_mutex_unlock(&s->lock);
        lhat_machine_panic_text(machine, "stopped by the debugger");
        return;
    }

    size_t depth = lhat_machine_fault_depth(machine);
    bool stop =
        s->stopping || s->pause_all || t->mode == DAP_STEP_IN ||
        (t->mode == DAP_STEP_OVER && depth <= t->step_depth) ||
        (t->mode == DAP_STEP_OUT && depth < t->step_depth) ||
        at_breakpoint(s, where);
    if (!stop) {
        lhat_mutex_unlock(&s->lock);
        return;
    }

    // The first to park is the stop the debugger hears about; the rest park
    // silently -- allThreadsStopped said it all.
    const char *reason = s->pause_all ? "pause"
                         : t->mode != DAP_RUN ? "step"
                                              : "breakpoint";
    t->mode = DAP_RUN;
    if (!s->stopping) {
        s->stopping = true;
        s->pause_all = false;
        clear_vars(s);
        send_stopped(s, t->id, reason);
    }
    t->parked = true;
    while (s->stopping && !s->ended) {
        lhat_condition_wait(&s->changed, &s->lock);
    }
    t->parked = false;
    bool over = s->ended;
    lhat_mutex_unlock(&s->lock);
    if (over) {
        lhat_machine_panic_text(machine, "stopped by the debugger");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle

bool dap_session_begin(DapSession **out, LhatMachine *machine, uint16_t port,
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
    s->peer.seq = 1;
    s->next_id = 1;
    lhat_mutex_init(&s->lock);
    lhat_mutex_init(&s->write_lock);
    lhat_condition_init(&s->changed);

    if (!lhat_socket_listen(&s->listener, port) ||
        !lhat_socket_accept(s->listener, &s->socket)) {
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
        free(s->breaks[i].source);
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
