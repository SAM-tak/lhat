// L^ (lhat) -- the debug adapter: the machine's line hook on one side, DAP
// over a socket on the other.

#include "adapter.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/debug.h"
#include "lhat/object.h"
#include "port/socket.h"
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

typedef enum {
    DAP_ACT_NONE,       // answered; keep reading
    DAP_ACT_RESUME,     // let the program run on
    DAP_ACT_TERMINATE   // end the run
} DapAction;

struct DapSession {
    DapPeer peer;
    LhatSocket listener;
    LhatSocket socket;
    LhatMachine *machine;
    char *program_path;  // normalized, for a breakpoint with no source

    DapBreak *breaks;
    size_t break_count;
    size_t break_capacity;

    DapMode mode;
    size_t step_depth;
    bool pause_requested;
    bool configured;
    bool ended_run;  // the debugger disconnected or terminated

    // The tables a variables request handed out a reference for, so a later
    // request for that reference can expand them. Reset at every stop.
    LhatValue *vars;
    size_t var_count;
    size_t var_capacity;

    // The last (source pointer, its normalized form) pair, so a run does not
    // realpath the same unit on every line.
    const char *cached_source;
    char *cached_normal;

    int poll_left;  // lines until the next non-blocking socket poll
};

// ---------------------------------------------------------------------------
// Paths

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

// ---------------------------------------------------------------------------
// Variable references

static int add_var(DapSession *s, LhatValue value)
{
    if (s->var_count == s->var_capacity) {
        size_t grown = s->var_capacity ? s->var_capacity * 2 : 16;
        LhatValue *bigger =
            (LhatValue *)realloc(s->vars, grown * sizeof *bigger);
        if (bigger == NULL) {
            return 0;
        }
        s->vars = bigger;
        s->var_capacity = grown;
    }
    s->vars[s->var_count] = value;
    // Table references start at 1000; scope references are the small numbers
    // below it (2*level + 2 for locals, +3 for captures).
    return 1000 + (int)s->var_count++;
}

static void clear_vars(DapSession *s)
{
    s->var_count = 0;
}

// One variable, expandable when it is a table.
static cJSON *variable_json(DapSession *s, const char *name, LhatValue value)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "name", name);

    char text[256];
    lhat_value_text(value, text, sizeof text);
    cJSON_AddStringToObject(out, "value", text);

    const LhatRuntimeType *type = lhat_value_type(s->machine, value);
    if (type != NULL) {
        char spelt[128];
        lhat_runtime_type_write(type, spelt, sizeof spelt);
        cJSON_AddStringToObject(out, "type", spelt);
    }

    int reference = 0;
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        reference = add_var(s, value);
    }
    cJSON_AddNumberToObject(out, "variablesReference", reference);
    return out;
}

// The members of a table, dense part first then the map part.
static cJSON *expand_table(DapSession *s, LhatValue value)
{
    cJSON *out = cJSON_CreateArray();
    const LhatTable *table = (const LhatTable *)lhat_as_object(value);
    for (size_t i = 0; i < table->array_count; i++) {
        char name[32];
        snprintf(name, sizeof name, "%zu", i + 1);
        cJSON_AddItemToArray(
            out, variable_json(s, name, lhat_slots_get(table->array, i)));
    }
    for (size_t i = 0; i < table->entry_capacity; i++) {
        LhatValue key = table->entries[i].key;
        if (lhat_is_nil(key)) {
            continue;
        }
        char name[128];
        lhat_value_text(key, name, sizeof name);
        cJSON_AddItemToArray(out,
                             variable_json(s, name, table->entries[i].value));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Where the machine is

static int current_depth(DapSession *s)
{
    return (int)lhat_machine_fault_depth(s->machine);
}

// The DAP name of a frame -- what the traceback calls it.
static const char *frame_name(const LhatFrameInfo *info)
{
    if (info->name != NULL) {
        return info->name;
    }
    return info->top_level ? "(top level)" : "f^";
}

static bool at_breakpoint(DapSession *s, const LhatFrameInfo *where)
{
    if (s->break_count == 0) {
        return false;
    }
    const char *here = normal_of(s, where->source);
    if (here == NULL) {
        return false;
    }
    for (size_t i = 0; i < s->break_count; i++) {
        if (s->breaks[i].line == (int)where->line &&
            path_equal(s->breaks[i].source, here)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Requests

static void clear_breaks(DapSession *s)
{
    for (size_t i = 0; i < s->break_count; i++) {
        free(s->breaks[i].source);
    }
    s->break_count = 0;
}

static void set_breakpoints(DapSession *s, const cJSON *arguments, cJSON *body)
{
    clear_breaks(s);
    const cJSON *source = cJSON_GetObjectItem(arguments, "source");
    const cJSON *path = cJSON_GetObjectItem(source, "path");
    char *normal =
        cJSON_IsString(path) ? normalize(path->valuestring) : NULL;

    cJSON *verified = cJSON_CreateArray();
    const cJSON *lines = cJSON_GetObjectItem(arguments, "breakpoints");
    const cJSON *one = NULL;
    cJSON_ArrayForEach(one, lines) {
        const cJSON *line = cJSON_GetObjectItem(one, "line");
        if (!cJSON_IsNumber(line) || normal == NULL) {
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
        s->breaks[s->break_count].source = normalize(normal);
        s->breaks[s->break_count].line = (int)line->valuedouble;
        s->break_count++;

        // v1 verifies every line it was given (09 の 5, D8 leaves checking
        // the line against the code for later).
        cJSON *mark = cJSON_CreateObject();
        cJSON_AddBoolToObject(mark, "verified", true);
        cJSON_AddNumberToObject(mark, "line", (int)line->valuedouble);
        cJSON_AddItemToArray(verified, mark);
    }
    free(normal);
    cJSON_AddItemToObject(body, "breakpoints", verified);
}

static void stack_trace(DapSession *s, cJSON *body)
{
    cJSON *frames = cJSON_CreateArray();
    int depth = current_depth(s);
    for (int level = 0; level < depth; level++) {
        LhatFrameInfo info;
        if (!lhat_machine_fault_frame(s->machine, (size_t)level, &info)) {
            break;
        }
        cJSON *frame = cJSON_CreateObject();
        cJSON_AddNumberToObject(frame, "id", level);
        cJSON_AddStringToObject(frame, "name", frame_name(&info));
        if (info.source != NULL) {
            cJSON *source = cJSON_CreateObject();
            cJSON_AddStringToObject(source, "path", info.source);
            cJSON_AddItemToObject(frame, "source", source);
        }
        cJSON_AddNumberToObject(frame, "line", info.line);
        cJSON_AddNumberToObject(frame, "column", 1);
        cJSON_AddItemToArray(frames, frame);
    }
    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", depth);
}

static void scopes(DapSession *s, const cJSON *arguments, cJSON *body)
{
    const cJSON *frame_id = cJSON_GetObjectItem(arguments, "frameId");
    int level = cJSON_IsNumber(frame_id) ? (int)frame_id->valuedouble : 0;
    cJSON *list = cJSON_CreateArray();

    cJSON *locals = cJSON_CreateObject();
    cJSON_AddStringToObject(locals, "name", "Locals");
    cJSON_AddNumberToObject(locals, "variablesReference", 2 * level + 2);
    cJSON_AddBoolToObject(locals, "expensive", false);
    cJSON_AddItemToArray(list, locals);

    if (lhat_frame_upvalue_count(s->machine, (size_t)level) > 0) {
        cJSON *captures = cJSON_CreateObject();
        cJSON_AddStringToObject(captures, "name", "Captures");
        cJSON_AddNumberToObject(captures, "variablesReference", 2 * level + 3);
        cJSON_AddBoolToObject(captures, "expensive", false);
        cJSON_AddItemToArray(list, captures);
    }
    cJSON_AddItemToObject(body, "scopes", list);
    (void)s;
}

static void variables(DapSession *s, const cJSON *arguments, cJSON *body)
{
    const cJSON *reference = cJSON_GetObjectItem(arguments, "variablesReference");
    int ref = cJSON_IsNumber(reference) ? (int)reference->valuedouble : 0;
    cJSON *list = NULL;

    if (ref >= 1000) {
        int index = ref - 1000;
        list = index >= 0 && (size_t)index < s->var_count
                   ? expand_table(s, s->vars[index])
                   : cJSON_CreateArray();
    } else {
        list = cJSON_CreateArray();
        int level = (ref - 2) / 2;
        bool captures = ((ref - 2) % 2) != 0;
        if (captures) {
            size_t count = lhat_frame_upvalue_count(s->machine, (size_t)level);
            for (size_t i = 0; i < count; i++) {
                LhatBindingInfo binding;
                if (lhat_frame_upvalue(s->machine, (size_t)level, i, &binding)) {
                    cJSON_AddItemToArray(
                        list, variable_json(s, binding.name, binding.value));
                }
            }
        } else {
            size_t count = lhat_frame_local_count(s->machine, (size_t)level);
            for (size_t i = 0; i < count; i++) {
                LhatBindingInfo binding;
                if (lhat_frame_local(s->machine, (size_t)level, i, &binding)) {
                    cJSON_AddItemToArray(
                        list, variable_json(s, binding.name, binding.value));
                }
            }
        }
    }
    cJSON_AddItemToObject(body, "variables", list);
}

// What a debugger typed as a new value, read the way L^ spells values:
// nil^, true^, false^, a number, or a quoted string (no escapes -- what the
// panel edits are short values, D1 keeps expressions for later).
static bool parse_value(DapSession *s, const char *text, LhatValue *out)
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
        return lhat_machine_make_string(s->machine, text + 1, length - 2, out);
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
static bool parse_key(DapSession *s, const char *name, LhatValue *out)
{
    char *end = NULL;
    long long index = strtoll(name, &end, 10);
    if (end != name && *end == '\0') {
        *out = lhat_integer(index);
        return true;
    }
    return lhat_machine_make_string(s->machine, name, strlen(name), out);
}

// Writes `value` where `reference` and `name` point: a member of a handed-out
// table, or a frame scope's binding -- by name, the innermost when shadowed,
// the same rule the read gave the panel its list under.
static bool write_variable(DapSession *s, int reference, const char *name,
                           LhatValue value)
{
    if (reference >= 1000) {
        int index = reference - 1000;
        if (index < 0 || (size_t)index >= s->var_count) {
            return false;
        }
        LhatValue key;
        bool refused = false;
        return parse_key(s, name, &key) &&
               lhat_machine_table_set(s->machine,
                                      (LhatTable *)lhat_as_object(
                                          s->vars[index]),
                                      key, value, &refused) &&
               !refused;
    }
    int level = (reference - 2) / 2;
    bool captures = ((reference - 2) % 2) != 0;
    size_t count =
        captures ? lhat_frame_upvalue_count(s->machine, (size_t)level)
                 : lhat_frame_local_count(s->machine, (size_t)level);
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        LhatBindingInfo binding;
        bool read = captures
                        ? lhat_frame_upvalue(s->machine, (size_t)level, i,
                                             &binding)
                        : lhat_frame_local(s->machine, (size_t)level, i,
                                           &binding);
        if (read && strcmp(binding.name, name) == 0) {
            found = i;  // the later of two under one name is the inner
        }
    }
    if (found == SIZE_MAX) {
        return false;
    }
    return captures ? lhat_frame_set_upvalue(s->machine, (size_t)level, found,
                                             value)
                    : lhat_frame_set_local(s->machine, (size_t)level, found,
                                           value);
}

// ---------------------------------------------------------------------------
// Events

static void send_stopped(DapSession *s, const char *reason)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsStopped", true);
    dap_event(&s->peer, "stopped", body);
}

// ---------------------------------------------------------------------------
// Dispatch

static DapAction dispatch(DapSession *s, const cJSON *request)
{
    const char *command = dap_command(request);
    const cJSON *arguments = dap_arguments(request);

    if (strcmp(command, "initialize") == 0) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", true);
        cJSON_AddBoolToObject(body, "supportsSetVariable", true);
        dap_respond(&s->peer, request, true, body);
        dap_event(&s->peer, "initialized", NULL);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0) {
        const cJSON *stop = cJSON_GetObjectItem(arguments, "stopOnEntry");
        if (cJSON_IsTrue(stop)) {
            s->mode = DAP_STEP_IN;  // stop at the first line
        }
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "setBreakpoints") == 0) {
        cJSON *body = cJSON_CreateObject();
        set_breakpoints(s, arguments, body);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "setExceptionBreakpoints") == 0) {
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "configurationDone") == 0) {
        s->configured = true;
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "threads") == 0) {
        cJSON *body = cJSON_CreateObject();
        cJSON *threads = cJSON_CreateArray();
        cJSON *main = cJSON_CreateObject();
        cJSON_AddNumberToObject(main, "id", 1);
        cJSON_AddStringToObject(main, "name", "main");
        cJSON_AddItemToArray(threads, main);
        cJSON_AddItemToObject(body, "threads", threads);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "stackTrace") == 0) {
        cJSON *body = cJSON_CreateObject();
        stack_trace(s, body);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "scopes") == 0) {
        cJSON *body = cJSON_CreateObject();
        scopes(s, arguments, body);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "variables") == 0) {
        cJSON *body = cJSON_CreateObject();
        variables(s, arguments, body);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "setVariable") == 0) {
        const cJSON *reference =
            cJSON_GetObjectItem(arguments, "variablesReference");
        const cJSON *name = cJSON_GetObjectItem(arguments, "name");
        const cJSON *text = cJSON_GetObjectItem(arguments, "value");
        LhatValue value = lhat_nil();
        bool wrote = cJSON_IsNumber(reference) && cJSON_IsString(name) &&
                     cJSON_IsString(text) &&
                     parse_value(s, text->valuestring, &value) &&
                     write_variable(s, (int)reference->valuedouble,
                                    name->valuestring, value);
        cJSON *body = NULL;
        if (wrote) {
            body = cJSON_CreateObject();
            char rendered[256];
            lhat_value_text(value, rendered, sizeof rendered);
            cJSON_AddStringToObject(body, "value", rendered);
        }
        dap_respond(&s->peer, request, wrote, body);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "continue") == 0) {
        s->mode = DAP_RUN;
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "allThreadsContinued", true);
        dap_respond(&s->peer, request, true, body);
        return DAP_ACT_RESUME;
    }
    if (strcmp(command, "next") == 0) {
        s->mode = DAP_STEP_OVER;
        s->step_depth = (size_t)current_depth(s);
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_RESUME;
    }
    if (strcmp(command, "stepIn") == 0) {
        s->mode = DAP_STEP_IN;
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_RESUME;
    }
    if (strcmp(command, "stepOut") == 0) {
        s->mode = DAP_STEP_OUT;
        s->step_depth = (size_t)current_depth(s);
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_RESUME;
    }
    if (strcmp(command, "pause") == 0) {
        s->pause_requested = true;
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_NONE;
    }
    if (strcmp(command, "disconnect") == 0 ||
        strcmp(command, "terminate") == 0) {
        dap_respond(&s->peer, request, true, NULL);
        return DAP_ACT_TERMINATE;
    }
    // Anything else is answered, unsupported, so the client is not left
    // waiting on a request this adapter does not know.
    dap_respond(&s->peer, request, false, NULL);
    return DAP_ACT_NONE;
}

// Reads and answers requests until the program should run on, or the
// debugger ends it, or the socket closes.
static void stop_loop(DapSession *s)
{
    for (;;) {
        cJSON *request = dap_read(&s->peer);
        if (request == NULL) {
            s->ended_run = true;  // the peer is gone
            return;
        }
        DapAction action = dispatch(s, request);
        cJSON_Delete(request);
        if (action == DAP_ACT_RESUME) {
            return;
        }
        if (action == DAP_ACT_TERMINATE) {
            s->ended_run = true;
            return;
        }
    }
}

// While running, drains whatever the debugger sent without stopping -- a
// pause to raise the flag, a disconnect to end, a breakpoint change. A
// request that would resume is a no-op here (the program is already going).
static void poll(DapSession *s)
{
    while (lhat_socket_readable(s->socket, 0)) {
        cJSON *request = dap_read(&s->peer);
        if (request == NULL) {
            s->ended_run = true;
            return;
        }
        if (dispatch(s, request) == DAP_ACT_TERMINATE) {
            s->ended_run = true;
        }
        cJSON_Delete(request);
    }
}

// ---------------------------------------------------------------------------
// The hook

static void dap_hook(LhatMachine *machine, void *context, LhatDebugEvent event,
                     const LhatFrameInfo *where)
{
    (void)event;
    DapSession *s = (DapSession *)context;

    if (--s->poll_left <= 0) {
        s->poll_left = 256;
        poll(s);
    }
    if (s->ended_run) {
        lhat_machine_panic_text(machine, "stopped by the debugger");
        return;
    }

    int depth = current_depth(s);
    bool stop = s->pause_requested || s->mode == DAP_STEP_IN ||
                (s->mode == DAP_STEP_OVER && depth <= (int)s->step_depth) ||
                (s->mode == DAP_STEP_OUT && depth < (int)s->step_depth) ||
                at_breakpoint(s, where);
    if (!stop) {
        return;
    }

    const char *reason = s->pause_requested
                             ? "pause"
                             : (s->mode != DAP_RUN ? "step" : "breakpoint");
    s->pause_requested = false;
    s->mode = DAP_RUN;
    clear_vars(s);
    send_stopped(s, reason);
    stop_loop(s);
    if (s->ended_run) {
        lhat_machine_panic_text(machine, "stopped by the debugger");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle

bool dap_session_begin(DapSession **out, LhatMachine *machine, uint16_t port,
                       const char *program_path)
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
    s->machine = machine;
    s->program_path = normalize(program_path);
    s->peer.seq = 1;
    s->poll_left = 256;

    if (!lhat_socket_listen(&s->listener, port) ||
        !lhat_socket_accept(s->listener, &s->socket)) {
        if (s->listener.handle != 0) {
            lhat_socket_close(s->listener);
        }
        free(s->program_path);
        free(s);
        lhat_socket_cleanup();
        return false;
    }

    // The stream is the socket, through the two shims below.
    s->peer.stream.context = s;
    s->peer.stream.read = socket_read;
    s->peer.stream.write = socket_write;

    // The handshake: answer requests until configurationDone. initialize
    // sends the `initialized` event, then the client sets breakpoints and
    // says it is done.
    while (!s->configured) {
        cJSON *request = dap_read(&s->peer);
        if (request == NULL) {
            break;  // the debugger left before it started
        }
        dispatch(s, request);
        cJSON_Delete(request);
    }

    lhat_machine_set_debug_hook(machine, dap_hook, s);
    *out = s;
    return true;
}

void dap_session_end(DapSession *session, int exit_code)
{
    if (session == NULL) {
        return;
    }
    lhat_machine_set_debug_hook(session->machine, NULL, NULL);
    if (!session->ended_run) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "exitCode", exit_code);
        dap_event(&session->peer, "exited", body);
        dap_event(&session->peer, "terminated", NULL);
    }
    lhat_socket_close(session->socket);
    lhat_socket_close(session->listener);
    lhat_socket_cleanup();
    clear_breaks(session);
    free(session->breaks);
    free(session->vars);
    free(session->cached_normal);
    free(session->program_path);
    free(session);
}

bool dap_session_ended_run(const DapSession *session)
{
    return session != NULL && session->ended_run;
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
