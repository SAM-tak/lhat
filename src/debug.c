// L^ (lhat) -- what a debugger reads off a machine (09 章): the frames a run
// left standing or is standing on, and what their registers were called.
//
// Section numbers refer to DesignDocuments/09-debugger.md unless prefixed.
//
// The third file that sees the inside of a machine (machine.h): vm.c runs
// it, gc.c collects it, this one reads it. The hook itself is fired from
// vm.c's loop, which is the only place that knows where the machine is.

#include "lhat/debug.h"

#include <stdio.h>
#include <string.h>

#include "compile.h"
#include "lhat/source.h"
#include "machine.h"
#include "parser.h"

// 04 の 11.6改: which span of frames the walkers read -- the recorded
// fault's, or, when none is recorded, the frames standing right now.
static void frame_span(const LhatMachine *m, size_t *base, size_t *depth,
                       bool *have_at)
{
    if (m->fault_depth > m->fault_base) {
        *base = m->fault_base;
        *depth = m->fault_depth;
        *have_at = true;
        return;
    }
    *base = 0;
    *depth = m->frame_count;
    *have_at = false;
}

// The frame `level` names, and the instruction it stands at. The top frame
// stopped at the recorded instruction; every caller's saved pc points one
// past its call, so its instruction is at pc - 1. SIZE_MAX for a frame that
// has not run an instruction, which has no line to name. NULL past the span.
static const Frame *frame_at(const LhatMachine *m, size_t level, size_t *at)
{
    size_t base, depth;
    bool have_at;
    frame_span(m, &base, &depth, &have_at);
    if (level >= depth - base) {
        return NULL;
    }
    const Frame *frame = &m->frames[depth - 1 - level];
    if (level == 0 && have_at) {
        *at = m->fault_at;
    } else if (frame->pc > 0) {
        *at = frame->pc - 1;
    } else {
        *at = SIZE_MAX;
    }
    return frame;
}

size_t lhat_machine_fault_depth(const LhatMachine *machine)
{
    size_t base, depth;
    bool have_at;
    frame_span(machine, &base, &depth, &have_at);
    return depth - base;
}

bool lhat_machine_fault_frame(const LhatMachine *machine, size_t level,
                              LhatFrameInfo *out)
{
    size_t at = 0;
    const Frame *frame = out != NULL ? frame_at(machine, level, &at) : NULL;
    if (frame == NULL) {
        return false;
    }
    const LhatProto *proto =
        frame->closure != NULL ? frame->closure->proto : NULL;
    out->source = proto != NULL ? proto->source_name : NULL;
    out->name = proto != NULL ? proto->debug_name : NULL;
    out->line = proto != NULL && at < proto->chunk.count
                    ? proto->chunk.lines[at]
                    : 0;
    out->top_level = proto != NULL && proto->is_unit;
    out->coroutine = frame->coroutine != NULL;
    out->disposing = frame->disposing;
    return true;
}

// The bounded writer the renderer fills -- lhat_value_text's shape.
typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} TraceText;

static void trace_put(TraceText *w, const char *text)
{
    size_t length = strlen(text);
    if (w->out != NULL && w->used < w->capacity) {
        size_t room = w->capacity - 1 - w->used;
        size_t take = length < room ? length : room;
        memcpy(w->out + w->used, text, take);
    }
    w->used += length;
}

size_t lhat_machine_traceback(const LhatMachine *machine, char *out,
                              size_t capacity)
{
    TraceText w;
    w.out = capacity > 0 ? out : NULL;
    w.capacity = capacity;
    w.used = 0;
    size_t count = lhat_machine_fault_depth(machine);
    if (count > 0) {
        trace_put(&w, "traceback:");
        for (size_t level = 0; level < count; level++) {
            LhatFrameInfo info;
            if (!lhat_machine_fault_frame(machine, level, &info)) {
                break;
            }
            trace_put(&w, "\n  ");
            trace_put(&w, info.source != NULL ? info.source : "?");
            if (info.line > 0) {
                char spelt[16];
                snprintf(spelt, sizeof spelt, ":%u", info.line);
                trace_put(&w, spelt);
            }
            trace_put(&w, info.name != NULL
                              ? ": in "
                              : (info.top_level ? ": at the top level"
                                                : ": in f^"));
            if (info.name != NULL) {
                trace_put(&w, info.name);
            }
            if (info.coroutine) {
                trace_put(&w, " (coroutine)");
            }
            if (info.disposing) {
                trace_put(&w, " (finally^)");
            }
        }
    }
    if (w.out != NULL) {
        size_t end = w.used < w.capacity - 1 ? w.used : w.capacity - 1;
        w.out[end] = '\0';
    }
    return w.used;
}

// ---------------------------------------------------------------------------
// 09 の 2 章: the hook. Everything from here down is the debugger proper --
// the watcher, the hook, frame inspection and evaluation -- and compiles
// away in a shipping build (LHAT_WITH_DEBUGGER 0). The traceback readers
// above stay: a fault report is not a debugger.
// ---------------------------------------------------------------------------
#if LHAT_WITH_DEBUGGER

// 09 の 5.1. Written here, read by vm.c at the two points every machine
// passes. The install/remove windows debug.h asks for are what stand in for
// a lock: no other thread is making machines at either moment.
LhatMachineWatcher lhat_machine_watcher;

void lhat_debug_watch_machines(const LhatMachineWatcher *watcher)
{
    if (watcher != NULL) {
        lhat_machine_watcher = *watcher;
    } else {
        memset(&lhat_machine_watcher, 0, sizeof lhat_machine_watcher);
    }
}

void lhat_machine_set_debug_hook(LhatMachine *machine, LhatDebugHook hook,
                                 void *context)
{
    machine->hook = hook;
    machine->hook_live = hook;
    machine->hook_context = context;
    // No frame has been looked at yet, so the next instruction counts as
    // one just entered and sounds whatever line it is on.
    machine->hook_depth = SIZE_MAX;
    machine->hook_pc = 0;
}

// ---------------------------------------------------------------------------
// 09 の 3.2: the names of a frame
// ---------------------------------------------------------------------------

// 09 の 4 章: how many names are live at `at` -- declared no later than it
// and not closed by it -- and, when `picked` is asked for, the `index`th of
// them. A frame that has run no instruction (SIZE_MAX) has its parameters
// and whatever else is live at 0.
static size_t live_locals(const LhatProto *proto, size_t at, size_t index,
                          const LhatLocalDesc **picked)
{
    const LhatChunk *chunk = &proto->chunk;
    size_t pc = at == SIZE_MAX ? 0 : at;
    size_t count = 0;
    for (size_t i = 0; i < chunk->local_count; i++) {
        const LhatLocalDesc *local = &chunk->locals[i];
        if (local->from <= pc && pc < local->to) {
            if (picked != NULL && count == index) {
                *picked = local;
            }
            count++;
        }
    }
    return count;
}

size_t lhat_frame_local_count(const LhatMachine *machine, size_t level)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    if (frame == NULL || frame->closure == NULL) {
        return 0;
    }
    return live_locals(frame->closure->proto, at, 0, NULL);
}

bool lhat_frame_local(const LhatMachine *machine, size_t level, size_t index,
                      LhatBindingInfo *out)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    const LhatLocalDesc *local = NULL;
    if (frame == NULL || frame->closure == NULL || out == NULL ||
        live_locals(frame->closure->proto, at, index, &local) <= index) {
        return false;
    }
    size_t slot = frame->base + local->reg;
    LhatValue value = lhat_slots_get(machine->slots, slot);
    out->name = local->name;
    // 05 の 8.9: a host value is handed out the way an argument is -- as a
    // pointer aimed at its slots, whatever its width.
    out->value = lhat_is_hostvalue(value)
                     ? hostvalue_argument(machine->slots, slot)
                     : value;
    return true;
}

size_t lhat_frame_upvalue_count(const LhatMachine *machine, size_t level)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    return frame != NULL && frame->closure != NULL
               ? frame->closure->upvalue_count
               : 0;
}

bool lhat_frame_upvalue(const LhatMachine *machine, size_t level,
                        size_t index, LhatBindingInfo *out)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    if (frame == NULL || frame->closure == NULL || out == NULL ||
        index >= frame->closure->upvalue_count) {
        return false;
    }
    // 03 の 5.4: open or closed, the place is read through its reference --
    // and a suspended coroutine's slot the same way (5.11).
    const LhatUpvalue *place = frame->closure->upvalues[index];
    out->name = frame->closure->proto->upvalues[index].name;
    out->value = place != NULL ? lhat_ref_get(place->location) : lhat_nil();
    return true;
}

bool lhat_frame_set_local(LhatMachine *machine, size_t level, size_t index,
                          LhatValue value)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    const LhatLocalDesc *local = NULL;
    if (frame == NULL || frame->closure == NULL || lhat_is_hostvalue(value) ||
        live_locals(frame->closure->proto, at, index, &local) <= index ||
        local->width > 1) {
        return false;
    }
    size_t slot = frame->base + local->reg;
    if (lhat_is_hostvalue(lhat_slots_get(machine->slots, slot))) {
        return false;  // a one-slot head of a wider layout, all the same
    }
    // No barrier: a register is a root the collector reads again from
    // scratch before it sweeps (gc.c's atomic), exactly because the program
    // writes registers without one.
    lhat_slots_set(machine->slots, slot, value);
    return true;
}

bool lhat_frame_set_upvalue(LhatMachine *machine, size_t level, size_t index,
                            LhatValue value)
{
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    if (frame == NULL || frame->closure == NULL || lhat_is_hostvalue(value) ||
        index >= frame->closure->upvalue_count ||
        frame->closure->upvalues[index] == NULL) {
        return false;
    }
    LhatUpvalue *place = frame->closure->upvalues[index];
    lhat_ref_set(place->location, value);
    // As at SETUPVAL: the place may be an object a closed cycle's marking
    // already looked at, or one it has not seen.
    lhat_gc_barrier(machine, (LhatObject *)place, value);
    return true;
}

// ---------------------------------------------------------------------------
// 09 の 3.5: evaluation
// ---------------------------------------------------------------------------

static void say(char *error, size_t capacity, const char *what, uint32_t line)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    if (line > 0) {
        snprintf(error, capacity, "line %u: %s", line, what);
    } else {
        snprintf(error, capacity, "%s", what);
    }
}

bool lhat_machine_evaluate(LhatMachine *machine, size_t level,
                           const char *text, size_t length, LhatValue *answer,
                           char *error, size_t error_capacity)
{
    if (answer != NULL) {
        *answer = lhat_nil();
    }
    size_t at = 0;
    const Frame *frame = frame_at(machine, level, &at);
    if (frame == NULL || frame->closure == NULL) {
        say(error, error_capacity, "no frame at that level", 0);
        return false;
    }

    // The frame's names, as copies for the evaluation's own first registers:
    // captures first, then the live locals -- so a local shadows the capture
    // it may have been made from, and of two locals alike the later (inner)
    // wins, the way the compiler's own search reads them.
    LhatValue seeds[LHAT_MAX_LOCALS];
    size_t seeded = 0;
    LhatCompileSession *session = lhat_compile_session_new();
    if (session == NULL) {
        say(error, error_capacity, "out of memory", 0);
        return false;
    }
    bool overfull = false;
    size_t captures = lhat_frame_upvalue_count(machine, level);
    size_t locals = lhat_frame_local_count(machine, level);
    for (size_t i = 0; i < captures + locals; i++) {
        LhatBindingInfo binding;
        bool read = i < captures
                        ? lhat_frame_upvalue(machine, level, i, &binding)
                        : lhat_frame_local(machine, level, i - captures,
                                           &binding);
        if (!read || lhat_is_hostvalue(binding.value)) {
            continue;  // a host value's slots cannot be copied one-for-one
        }
        if (seeded >= LHAT_MAX_LOCALS ||
            !lhat_compile_session_seed(session, binding.name,
                                       strlen(binding.name),
                                       (uint8_t)seeded)) {
            overfull = true;
            break;
        }
        seeds[seeded++] = binding.value;
    }
    if (overfull) {
        lhat_compile_session_dispose(session);
        say(error, error_capacity, "too many names in scope", 0);
        return false;
    }

    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    if (!lhat_source_init_from_string(&source, "<debugger>", text, length)) {
        lhat_compile_session_dispose(session);
        say(error, error_capacity, "out of memory", 0);
        return false;
    }
    lhat_lexer_init(&lexer, &source);
    lhat_parse_interactive(&lexer, &parsed);

    LhatProto *proto = NULL;
    LhatCompileResult compiled;
    compiled.status = LHAT_COMPILE_UNSUPPORTED;
    compiled.line = parsed.diagnostic_count > 0 ? parsed.diagnostics[0].line
                                                : 0;
    bool ok = false;
    if (parsed.root == NULL || parsed.diagnostic_count > 0) {
        say(error, error_capacity, "the input did not parse", compiled.line);
    } else {
        compiled = lhat_compile_next(session, parsed.root, &lexer, &proto);
        if (compiled.status != LHAT_COMPILE_OK) {
            say(error, error_capacity,
                lhat_compile_status_message(compiled.status), compiled.line);
            lhat_proto_free(proto);
        } else {
            // The machine takes the tree: a closure the evaluation leaves
            // behind (in a written global, say) keeps it alive (05 の 5.6).
            LhatValue script = lhat_nil();
            if (!lhat_machine_adopt_script(machine, proto, &script)) {
                lhat_proto_free(proto);
                say(error, error_capacity, "out of memory", 0);
            } else {
                // 2.4's silence, by hand: the evaluation's own lines are not
                // the hook's to hear -- and the hook is usually what called.
                LhatDebugHook live = machine->hook_live;
                machine->hook_live = NULL;
                LhatRunResult ran = lhat_machine_run_seeded(
                    machine,
                    (const LhatClosure *)lhat_as_object(script), seeds,
                    seeded);
                machine->hook_live = live;
                if (ran.status != LHAT_RUN_OK) {
                    say(error, error_capacity,
                        lhat_run_status_message(ran.status), ran.line);
                } else {
                    if (answer != NULL) {
                        *answer = ran.value;
                    }
                    ok = true;
                }
            }
        }
    }

    lhat_compile_session_dispose(session);
    lhat_parse_result_dispose(&parsed);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return ok;
}

#endif  // LHAT_WITH_DEBUGGER
