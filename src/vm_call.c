// L^ (lhat) -- call boundaries, frames, coroutines and cleanup.

#include "vm_internal.h"
#include <string.h>
#include "lhat/config.h"
#include "lhat/port.h"

// 15.4 with 5.4: the inverse of the move a yield^ makes. The frame is back
// on the stack at `base` -- always as the new top frame, so its slots are
// the highest addresses in use -- and every vm_capture that traveled with the
// saved registers points back into the live slots and rejoins the machine's
// open list at its head. The coroutine's own list is kept in ascending slot
// order, so prepending one by one lands them in the descending order the
// machine's list keeps.
static void reattach_upvalues(Machine *m, LhatCoroutine *co, size_t base)
{
    while (co->open != NULL) {
        LhatUpvalue *up = co->open;
        co->open = up->next_open;
        size_t offset = (size_t)(up->location.value - co->registers.values);
        up->location = lhat_slots_ref(m->slots, base + offset);
        up->suspended_in = NULL;
        up->next_open = m->open;
        m->open = up;
    }
}

// 5.11 with 02 の 10.7: puts a suspended coroutine's one saved frame back on
// the stack so that what it still owes can be run. The caller has already
// made room (the frame array and the stack's own end) and picked where the
// frame goes: `next_base` is its first register and `result` the slot in the
// caller's frame the answer lands in.
//
// 5.2 fixes a frame's width at compile time and gc.c's mark_roots walks all
// of it -- it has to, since the scratch above the names is where a
// half-finished expression keeps what it is holding. So every slot inside the
// width is read by the collector whether or not this frame ever wrote it, and
// one the frame below left behind still holds that frame's value.
//
// Which is a use-after-free waiting to happen: the value was unreachable when
// its own frame went away and was collected, and the slot still points at it.
// Emptying the scratch here is what keeps the walk seeing nil^ instead. Lua
// clears a new frame's stack for the same reason.
//
// 05 の 8.9: where the parameters end is `parameter_slots` and not the count
// -- a host value parameter is one parameter and several slots. 13.7's
// collector is a parameter of its own and is inside it too, so everything the
// caller laid down is below this and only the scratch is emptied.
void vm_clear_scratch(Machine *m, size_t base, const LhatProto *proto)
{
    if (proto == NULL) {
        return;
    }
    for (size_t r = proto->parameter_slots; r < proto->chunk.registers; r++) {
        lhat_slots_set(m->slots, base + r, lhat_nil());
    }
}

// The frame comes back marked `disposing`, which 10.7 needs -- a yield^ from
// inside a cleanup has nothing to suspend into -- and set to drain rather
// than to run: every cleanup, innermost first, and then the coroutine is
// finished. What the caller does with `frame`/`registers`/`chunk`/`pc` after
// this is jump to the drain.
void vm_enter_disposal_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result,
                                 Frame **frame, size_t *rbase,
                                 const LhatChunk **chunk, size_t *pc)
{
    for (size_t i = 0; i < co->register_count; i++) {
        lhat_slots_set(m->slots, next_base + i,
                       lhat_slots_get(co->registers, i));
    }
    reattach_upvalues(m, co, next_base);
    Frame *called = &m->frames[m->frame_count++];
    called->closure = co->closure;
    called->pc = co->pc;
    called->base = next_base;
    called->result = result;
    called->prepared = 1;  // 13.8改: a dispose answers nothing to take apart
    called->coroutine = co;
    called->drop_answer = false;  // 5.3
    called->disposing = true;
    called->returning = true;
    called->drain_target = 0;
    called->answer = lhat_nil();
    called->cleanup_count = co->cleanup_count;
    for (size_t i = 0; i < co->cleanup_count; i++) {
        called->cleanups[i] = co->cleanups[i];
    }
    if (co->state == LHAT_COROUTINE_SUSPENDED && co->cleanup_count > 0) {
        m->cleanup_carriers--;  // 10.7: its cleanups are the frame's now
    }
    co->state = LHAT_COROUTINE_RUNNING;

    *frame = called;
    *rbase = called->base;
    *chunk = &co->closure->proto->chunk;
    *pc = called->pc;
}

// 5.4: one place per slot, so two closures capturing the same name share it.
// The machine's list only ever holds captures of live stack slots, so the
// payload pointer alone orders it -- the tag pointer travels alongside.
LhatUpvalue *vm_capture(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL && (*link)->location.value > place) {
        link = &(*link)->next_open;
    }
    if (*link != NULL && (*link)->location.value == place) {
        return *link;
    }

    LhatUpvalue *upvalue =
        (LhatUpvalue *)lhat_object_alloc(&m->objects, sizeof *upvalue,
                                        LHAT_OBJECT_UPVALUE);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = lhat_slots_ref(m->slots, slot);
    upvalue->next_open = *link;
    *link = upvalue;
    return upvalue;
}
// Carrying a shared place into the upvalue itself: the value moves into the
// closed cell and location aims there, the same dereference as before.
static void close_into_cell(Machine *m, LhatUpvalue *upvalue)
{
    LhatValue held = lhat_ref_get(upvalue->location);
    upvalue->closed_value = held.as;
    upvalue->closed_tag = (uint8_t)held.tag;
    upvalue->location = lhat_upvalue_closed_ref(upvalue);
    // 5.12: what was a stack slot the roots covered is now a place the
    // upvalue itself holds, so a black upvalue has just gained a reference
    // the collector has not seen.
    lhat_gc_barrier(m, (LhatObject *)upvalue, held);
}

// The frame is going, so anything still pointing into it carries its value
// away. Without this a closure returned from a subroutine would read a slot
// that has been reused.
void vm_close_upvalues(Machine *m, size_t above)
{
    const LhatValueUnion *floor = m->slots.values + above;
    while (m->open != NULL && m->open->location.value >= floor) {
        LhatUpvalue *upvalue = m->open;
        m->open = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

// The same for one place rather than a frame's worth. 03 の 4.3: a session's
// top level puts every let^ of a name back in the one slot, so severing that
// binding's sharing must leave the names above it -- other bindings, still
// live -- shared as they were.
void vm_close_one_upvalue(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL) {
        LhatUpvalue *upvalue = *link;
        if (upvalue->location.value != place) {
            link = &upvalue->next_open;
            continue;
        }
        *link = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

// What a host function left behind when it returned, read by the three
// places one is called from. 05 の 8.7改: a nested lhat_machine_call that
// faulted leaves its frames standing (nothing unwinds), so frames past
// `frames_before` mean the outer run ends with the same fault. 8.7改2: and
// lhat_machine_panic asks for the same end with a value of the host's own.
// True when the run is over, with what to end it with.
bool vm_host_faulted(Machine *m, size_t frames_before,
                         LhatRunStatus *status, LhatValue *value)
{
    if (m->host_panicked) {
        m->host_panicked = false;
        *status = LHAT_RUN_PANIC;
        *value = m->fault_value;
        return true;
    }
    if (m->frame_count > frames_before) {
        *status = m->fault_status != LHAT_RUN_OK ? m->fault_status
                                                 : LHAT_RUN_TYPE_ERROR;
        *value = m->fault_value;
        return true;
    }
    return false;
}
LhatRunResult vm_finish(Machine *m, const LhatChunk *chunk,
                            LhatRunStatus status, LhatValue value, size_t at)
{
    // 03 の 4.3: what the program allocated belongs to the machine, not to
    // the run -- the answer may be a table or a string, and a REPL's next
    // input has to be able to reach it. So a result owns nothing and the
    // answer is good for as long as the machine is.
    LhatRunResult result;
    result.status = status;
    result.value = value;
    // 02 の 13.8改: the positions of a tuple answer, which the pop copied
    // into the machine's room before coming here. Everything else leaves the
    // count at zero, so an ordinary answer reads as it always did.
    result.positions = m->tuple_scratch_count > 0 ? m->tuple_scratch : NULL;
    result.position_count = m->tuple_scratch_count;
    result.at = at;
    // 04 の 11.6改: the frames of this run are still standing (nothing
    // here unwinds), so remember which they were and where the top one
    // stopped -- lhat_machine_fault_* walks them until the next run.
    if (status != LHAT_RUN_OK && status != LHAT_RUN_SUSPENDED) {
        m->fault_base = m->run_base;
        m->fault_depth = m->frame_count;
        m->fault_at = at;
        m->fault_status = status;
        m->fault_value = value;
    } else {
        m->fault_depth = m->fault_base = 0;
        m->fault_status = LHAT_RUN_OK;
        m->fault_value = lhat_nil();
    }
    // 04 の 11 章: named from the chunk's own line table (03 の 5.11a-style
    // parallel array) and 02 の 11.8's operator names -- both silently
    // answer nothing usable when `at` is out of range, which only happens
    // for the one fault raised before the first instruction runs.
    result.line = at < chunk->count ? chunk->lines[at] : 0;
    result.op_name =
        at < chunk->count ? vm_operator_name(lhat_op(chunk->code[at]),
                                          &result.op_name_length)
                          : NULL;
    if (result.op_name == NULL) {
        result.op_name_length = 0;
    }
    result.collected = m->collected;
    result.live = m->objects.count;

    // 5.4: an upvalue points into the stack, and the frames are about to go
    // -- but the list is not emptied here. 03 の 4.3: a session's top-level
    // slots outlive the run, and what points into them has to stay listed as
    // open, or the next input could neither find it to close (a let^ writing
    // that name again) nor find it to share (a second closure over the same
    // name). What the run above left pointing into slots that do not survive
    // is closed by the next lhat_run, before it clears them.
    return result;
}
// Puts one step down at slot `at` (WALK_AS_RUN fills at..at+2). The caller
// reserved the slots; the bounds check is the caller's, since the loop's
// reservation and a hand-driven call site reserve different widths.
WalkStep vm_step_table_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                                size_t at, Frame *frame)
{
    // 16.3改2: a projection walks the whole table and hands over one half of
    // each step, so the mode says nothing here -- there is one slot to fill
    // whichever way the caller asked. A caller that reserved a run is refused
    // above; this is only ever reached with a slot to write.
    if (co->part != LHAT_WALK_PAIR) {
        LhatValue key, value;
        if (!lhat_table_walk(co, &key, &value)) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
        co->state = LHAT_COROUTINE_SUSPENDED;
        lhat_slots_set(m->slots, at,
                       co->part == LHAT_WALK_KEYS ? key : value);
        return WALK_TOOK;
    }

    if (mode == WALK_AS_VALUE) {
        // Only the dense half, by index -- lhat_table_walk would go on into
        // the keyed half, which this form does not visit.
        LhatValue value = lhat_table_get(
            co->walking, lhat_integer((int64_t)co->at_array + 1));
        if (lhat_is_nil(value)) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
        co->at_array++;
        co->state = LHAT_COROUTINE_SUSPENDED;
        lhat_slots_set(m->slots, at, value);
        return WALK_TOOK;
    }

    LhatValue key, value;
    if (!lhat_table_walk(co, &key, &value)) {
        co->state = LHAT_COROUTINE_DONE;
        lhat_slots_set(m->slots, at, lhat_nil());
        return WALK_ENDED;
    }
    co->state = LHAT_COROUTINE_SUSPENDED;

    if (mode == WALK_AS_RUN) {
        // 13.8改: the pair as a tuple, laid into the caller's reserved slots.
        lhat_slots_set(m->slots, at, lhat_run_head(2));
        lhat_slots_set(m->slots, at + 1, key);
        lhat_slots_set(m->slots, at + 2, value);
        return WALK_TOOK;
    }

    // WALK_AS_ANSWER: the positions ride the frame's answer room and only
    // the head sits in the slot, which is what the YIELD of 15.8's
    // delegation loop forwards. They stay reachable while they sit there
    // through the table being walked, which the walk holds and the loop
    // holds in a register -- not through the room itself, which gc.c walks
    // only for a frame whose answer is a run.
    frame->answer_run[1] = key.as;
    frame->answer_tags[1] = (uint8_t)key.tag;
    frame->answer_run[2] = value.as;
    frame->answer_tags[2] = (uint8_t)value.tag;
    lhat_slots_set(m->slots, at, lhat_run_head(2));
    return WALK_TOOK;
}

// 05 の 8.8: the same step for a walk the host wrote -- its body is one C
// call rather than a frame, so this is vm_step_table_walk's twin with the value
// already picked by the host. `sent` is what the resume handed in (nil^ from
// the loops, which send nothing). `expected` is the positions a WALK_AS_RUN
// caller reserved, or the slots a WALK_AS_VALUE caller reserved for a wide
// value (0 when it could only be one). A step that answers a shape
// the call site did not reserve puts the mismatch in *fault, LHAT_RUN_OK
// otherwise -- the same refusals a yield^'s placement makes.
// 02 の 13.8改: the boundary's answer, in the shape the machine carries one
// in. Defined below, where the room itself is.
static bool boundary_answer(Machine *m, int written, LhatValue *answered);
bool vm_call_host_fn(Machine *m, LhatHostFn call, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answered);

WalkStep vm_step_host_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                               size_t at, size_t expected, Frame *frame,
                               const LhatValue *sent, size_t sent_count,
                               LhatRunStatus *fault)
{
    *fault = LHAT_RUN_OK;
    LhatValue out = lhat_nil();
    int written = 0;
    co->state = LHAT_COROUTINE_RUNNING;
    m->tuple_scratch_count = 0;
    bool more = co->step((LhatMachine *)m, co->host_state, sent, sent_count,
                         m->tuple_scratch, &written);
    if (!boundary_answer(m, written, &out)) {
        *fault = LHAT_RUN_TUPLE_ARITY;
        return WALK_ENDED;
    }
    if (!more) {
        // 02 の 10.7: the walk is over, so its state goes back now rather
        // than waiting on a collection.
        co->state = LHAT_COROUTINE_DONE;
        lhat_coroutine_release((LhatObject *)co, m);
        // 13.9: what a walk wrote as it ended is T -- the slot a body
        // coroutine fills by writing return^. It is placed like any
        // other answer, so a hand-written resume() reads it; `for^`
        // does not, because the loop asks ISDONE after the resume and
        // leaves without binding what came back (compile.c).
        if (written == 0) {
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
    } else {
        co->state = LHAT_COROUTINE_SUSPENDED;
        if (written == 0) {
            // A walk that goes on has to hand something over: `for^`
            // binds what came back, and there would be nothing to bind.
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
    }

    if (!lhat_is_run(out)) {
        // 05 の 8.9: a host value is written out whole on the spot, exactly
        // as a host call's answer is -- the scratch it lives in belongs to
        // whoever crosses the boundary next, and the next step is exactly
        // such a crossing. Only into room its width fits, whichever mode
        // said it: a run reservation is expected+1 slots, a wide value
        // reservation rides `expected` too, and a site that could not know
        // the type reserved one slot and is refused rather than overwritten
        // past (the delegation loop's slot among them).
        if (lhat_is_hostvalue(out)) {
            const LhatValueUnion *orun = out.as.hostvalue_run;
            size_t room = mode == WALK_AS_RUN ? expected + 1
                          : expected > 0      ? expected
                                              : 1;
            if (orun == NULL || orun[0].hostvalue == NULL ||
                room < orun[0].hostvalue->width ||
                !vm_place_hostvalue_answer(m, at, out)) {
                *fault = LHAT_RUN_TYPE_ERROR;
                return WALK_ENDED;
            }
            return more ? WALK_TOOK : WALK_ENDED;
        }
        if (mode == WALK_AS_RUN) {
            // The loop reserved a run and one value came out.
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
        lhat_slots_set(m->slots, at, out);
        return more ? WALK_TOOK : WALK_ENDED;
    }

    // Several answers: the positions sit in the machine's own room,
    // put there by the host before it returned.
    size_t positions = lhat_run_width(out);
    if (positions != m->tuple_scratch_count) {
        *fault = LHAT_RUN_TUPLE_ARITY;
        return WALK_ENDED;
    }
    if (mode == WALK_AS_VALUE) {
        // One slot reserved and a tuple came out -- 13.8改's refusal.
        *fault = LHAT_RUN_TUPLE_UNEXPECTED;
        return WALK_ENDED;
    }
    if (mode == WALK_AS_RUN) {
        if (positions != expected) {
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
        lhat_slots_set(m->slots, at, out);
        for (size_t i = 0; i < positions; i++) {
            lhat_slots_set(m->slots, at + 1 + i, m->tuple_scratch[i]);
        }
        return more ? WALK_TOOK : WALK_ENDED;
    }

    // WALK_AS_ANSWER, as above: head in the slot, positions in the frame's
    // answer room for the YIELD forwarding them. They stay reachable through
    // the machine's tuple room, whose count stands until the next host
    // boundary -- and the delegation loop reaches its YIELD without one.
    for (size_t i = 0; i < positions; i++) {
        frame->answer_run[i + 1] = m->tuple_scratch[i].as;
        frame->answer_tags[i + 1] = (uint8_t)m->tuple_scratch[i].tag;
    }
    lhat_slots_set(m->slots, at, out);
    return more ? WALK_TOOK : WALK_ENDED;
}

// 5.11: one frame, put back where it left off -- shared by the natives'
// resume, LHAT_BC_RESUME and the host boundary (lhat_machine_resume), which
// all restore a BODY coroutine the same way. `prepared` is what the call
// site reserved for the answer (13.8改) and `result_slot` is where it lands
// in the resumer's window. `sent`/`sent_count` is what the resume sends --
// none, one, or 13.8改's several. The caller has checked the frame and slot
// bounds (a several-send against sent_into included) and saved its own pc.
Frame *vm_enter_resume_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result_slot,
                                 uint8_t prepared, const LhatValue *sent,
                                 size_t sent_count)
{
    bool resuming = co->state == LHAT_COROUTINE_SUSPENDED;
    for (size_t i = 0; i < co->register_count; i++) {
        lhat_slots_set(m->slots, next_base + (i),
                       lhat_slots_get(co->registers, i));
    }
    reattach_upvalues(m, co, next_base);
    Frame *called = &m->frames[m->frame_count++];
    called->closure = co->closure;
    called->pc = co->pc;
    called->base = next_base;
    called->result = result_slot;
    called->prepared = prepared;
    called->coroutine = co;
    called->disposing = false;
    called->drop_answer = false;  // 5.3
    // The room is a root while the frame lives (mark_roots), so it starts
    // empty rather than as whatever the slot held before.
    called->answer = lhat_nil();
    called->derive = LHAT_FRAME_NO_DERIVE;
    called->derive_equal = false;
    called->returning = false;
    called->cleanup_count = co->cleanup_count;
    for (size_t i = 0; i < co->cleanup_count; i++) {
        called->cleanups[i] = co->cleanups[i];
    }
    if (resuming && co->cleanup_count > 0) {
        m->cleanup_carriers--;  // 10.7: its cleanups are the frame's now
    }
    co->state = LHAT_COROUTINE_RUNNING;
    if (resuming) {
        // 15.4: what the resume sent arrives where the yield^ put out what
        // it sent. 13.8改: several arrive as a run -- head in that slot and
        // the positions after it, which the yield^'s own binding reserved.
        if (sent_count > 1) {
            lhat_slots_set(m->slots, next_base + co->sent_into,
                           lhat_run_head(sent_count));
            for (size_t i = 0; i < sent_count; i++) {
                lhat_slots_set(m->slots, next_base + co->sent_into + 1 + i,
                               sent[i]);
            }
        } else if (sent_count == 1 && lhat_is_hostvalue(sent[0])) {
            // 05 の 8.9: the pointer form the gather built, written out
            // whole where the yield^ receives -- the callers checked the
            // width against the frame already.
            vm_place_hostvalue_answer(m, next_base + co->sent_into, sent[0]);
        } else {
            lhat_slots_set(m->slots, next_base + co->sent_into,
                           sent_count == 1 ? sent[0] : lhat_nil());
        }
    }
    return called;
}
// 02 の 13.8改 with 05 の 8.7: the boundary's answer, put into the shape the
// machine already carries one in. A host writes its values into the room
// and says how many; from here on a single value travels as itself and
// several travel as a run head with the positions in tuple_scratch --
// which is what every site downstream already reads.
//
// Answers false when the count is out of range, which is a host saying
// something the machine has no room for. The room is LHAT_MAX_TUPLE wide
// and a signature wider than that is refused at registration, so a host
// writing what its own signature says never lands here.
static bool boundary_answer(Machine *m, int written, LhatValue *answered)
{
    if (written < 0 || written > LHAT_MAX_TUPLE) {
        return false;
    }
    if (written == 0) {
        m->tuple_scratch_count = 0;
        *answered = lhat_nil();
        return true;
    }
    if (written == 1) {
        m->tuple_scratch_count = 0;
        *answered = m->tuple_scratch[0];
        return true;
    }
    // Held until the call returns and the machine lays the run down. The
    // count is what tells the collector how much of the room to read.
    m->tuple_scratch_count = (size_t)written;
    *answered = lhat_run_head((size_t)written);
    return true;
}

// One crossing: the arguments over, the answers back. The room is the
// machine's tuple_scratch, which the collector walks -- so a host may
// build a table or call back into L^ between writing an answer and
// returning, and what it wrote stays reachable.
bool vm_call_host_fn(Machine *m, LhatHostFn call, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answered)
{
    int written = 0;
    m->tuple_scratch_count = 0;
    call((LhatMachine *)m, context, arguments, count, m->tuple_scratch,
         &written);
    return boundary_answer(m, written, answered);
}
LhatRunResult lhat_run_arguments(LhatMachine *m, const LhatProto *proto,
                                 const LhatValue *arguments, size_t count)
{
    const LhatChunk *chunk = &proto->chunk;

    // 02 の 13.8改: whatever the last answer left in the room is not this
    // run.s, and holding it would keep those values from being collected.
    // 8.7改2: a panic asked for outside any host call belongs to no run.
    m->host_panicked = false;
    m->tuple_scratch_count = 0;

    // 5.4 and 5.2: the frames and the registers belong to the run, so each
    // starts with neither. What the heap holds is the machine's and stays.
    //
    // 03 の 4.3: except the registers the unit says are already spoken for.
    // A REPL's second input finds the top-level names of the first there,
    // and clearing them would be clearing the session.
    // 5.4: what an earlier run left sharing a slot this one is about to
    // clear takes its value away first. The slots below `reserved` are the
    // session's and survive, so what points into them stays open and shared
    // -- that is what lets a closure an earlier input made go on naming the
    // very place a later one reads and writes.
    m->frame_count = 0;
    // 04 の 11.6改: and what the last run's fault left readable goes with
    // them -- a host asked "where am I" in this run must be answered with
    // this run's frames, not the span the last one recorded.
    m->fault_depth = m->fault_base = 0;
    vm_close_upvalues(m, proto->reserved);
    for (size_t i = proto->reserved; i < m->slot_capacity; i++) {
        lhat_slots_set(m->slots, i, lhat_nil());
    }

    LhatClosure *entry =
        (LhatClosure *)lhat_object_alloc(&m->objects, sizeof *entry,
                                        LHAT_OBJECT_SUBROUTINE);
    if (entry == NULL) {
        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
    }
    entry->proto = proto;

    vm_push_frame(m, entry, 0, 0, 1);

    // 02 の 13.7 with 05 の 3.2: a script's '...' is register 0, and what was
    // handed over is collected into it the way a CALL collects. A module^
    // unit (or a session's input) has no '...' to take anything.
    if (proto->has_variadic) {
        LhatTable *collected = lhat_table_new(&m->objects);
        if (collected == NULL) {
            return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
        }
        for (size_t i = 0; i < count; i++) {
            bool refused = false;
            if (lhat_is_hostvalue(arguments[i]) ||
                !vm_set_key(m, collected, lhat_integer((int64_t)i + 1),
                         arguments[i], &refused)) {
                return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), 0);
            }
        }
        lhat_slots_set(m->slots, proto->reserved,
                       lhat_object((LhatObject *)collected));
    } else if (count > 0) {
        return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), 0);
    }

    return vm_run_frames(m, 0, false);
}

LhatRunResult lhat_run(LhatMachine *m, const LhatProto *proto)
{
    return lhat_run_arguments(m, proto, NULL, 0);
}

// A LhatRunResult for a call that never got as far as pushing a frame -- no
// chunk exists yet to name a line or an operator from, so both come back
// empty the way vm_finish() already leaves them for `at` out of range.
static LhatRunResult call_fault(Machine *m, LhatRunStatus status)
{
    // 04 の 11.6改: a boundary fault happened before any frame went on, so
    // the recorded span is empty -- lhat_machine_fault_depth answers 0.
    if (status != LHAT_RUN_OK) {
        m->fault_base = m->fault_depth = m->frame_count;
        m->fault_at = 0;
    }
    LhatRunResult result;
    result.status = status;
    result.value = lhat_nil();
    result.positions = NULL;
    result.position_count = 0;
    result.at = 0;
    result.line = 0;
    result.op_name = NULL;
    result.op_name_length = 0;
    result.collected = m->collected;
    result.live = m->objects.count;
    return result;
}

// The body of both host entry points below. `receiver` means something only
// when `as_method`, and then the callee is a member reached through it
// (14.4) -- which is the whole of the difference between them.
// 05 の 8.9改: one boundary value into consecutive slots. A host value
// widens to its head and continuation slots -- the in-frame shape every
// instruction reads -- and anything else takes one slot. Answers how many
// slots it took.
static size_t place_boundary_value(LhatSlots slots, size_t at, LhatValue v)
{
    if (!lhat_is_hostvalue(v)) {
        lhat_slots_set(slots, at, v);
        return 1;
    }
    const LhatValueUnion *run = v.as.hostvalue_run;
    size_t width = run[0].hostvalue->width;
    slots.values[at] = run[0];
    slots.tags[at] = (uint8_t)LHAT_VALUE_HOSTVALUE;
    for (size_t k = 1; k < width; k++) {
        slots.values[at + k] = run[k];
        slots.tags[at + k] = (uint8_t)LHAT_VALUE_CONT;
    }
    return width;
}

static LhatRunResult host_call(Machine *m, LhatValue callee, LhatValue receiver,
                               bool as_method, const LhatValue *arguments,
                               size_t count)
{
    size_t base = m->frame_count;
    m->tuple_scratch_count = 0;  // 13.8改, as in lhat_run

    // 05 の 8.7: a member the host registered in C, called back the way an
    // instruction calls one -- flat arguments, no spread. Without this a
    // host holding a hostdata value could call nothing on it, since 8.8
    // registers every member of one this way.
    if (lhat_is_object_kind(callee, LHAT_OBJECT_HOST)) {
        const LhatHost *host = (const LhatHost *)lhat_as_object(callee);
        // 13.4 keeps self^ out of the count, as at a compiled call site.
        if (host->has_variadic ? count < host->parameters
                               : count != host->parameters) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        bool receiver_first = host->takes_self && as_method;
        size_t given = count + (receiver_first ? 1 : 0);
        LhatValue gathered[LHAT_MAX_REGISTERS + 2];
        if (given > LHAT_MAX_REGISTERS + 2) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        size_t into = 0;
        if (receiver_first) {
            gathered[into++] = receiver;
        }
        for (size_t i = 0; i < count; i++) {
            gathered[into++] = arguments[i];
        }
        // 05 の 8.8: a dispose^ called by hand is the same giving-back the
        // collection would do, so it is marked here and 10.7 keeps the
        // sweep from doing it again -- as at a compiled call site.
        if (receiver_first &&
            lhat_is_object_kind(gathered[0], LHAT_OBJECT_HOSTDATA)) {
            LhatHostData *data = (LhatHostData *)lhat_as_object(gathered[0]);
            const LhatHostDataTag *by = lhat_hostdata_releaser(data->tag);
            if (by != NULL && by->release == host->call) {
                data->released = true;
            }
        }
        size_t frames_before = m->frame_count;
        LhatValue answered = lhat_nil();
        if (!vm_call_host_fn(m, host->call, host->context, gathered, given,
                          &answered)) {
            return call_fault(m, LHAT_RUN_TUPLE_ARITY);
        }
        // 05 の 8.7改2: the host's own fault comes back to the host that
        // called it, as it would to an instruction. The span is empty
        // (call_fault) -- no frame of this machine's was involved.
        LhatRunStatus left = LHAT_RUN_OK;
        LhatValue left_with = lhat_nil();
        if (vm_host_faulted(m, frames_before, &left, &left_with)) {
            LhatRunResult faulted = call_fault(m, left);
            faulted.value = left_with;
            return faulted;
        }
        // 05 の 8.9: a host value cannot cross this boundary -- its bytes
        // live in slots the boundary does not have -- so it goes the way a
        // return's base already sends one.
        if (lhat_is_hostvalue(answered)) {
            answered = lhat_nil();
        }
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (lhat_is_run(answered)) {
            // 02 の 13.8改: the positions already sit in the machine's room
            // (the boundary's own room), which is where the result aims anyway.
            size_t positions = lhat_run_width(answered);
            if (positions != m->tuple_scratch_count) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
            made.positions = m->tuple_scratch;
            made.position_count = positions;
            made.value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
        } else {
            m->tuple_scratch_count = 0;
            made.value = answered;
        }
        return made;
    }

    // 05 の 8.7's LhatHostFn has no shape for a spread; a host calling back in
    // already has its arguments as a flat array, so this is the plain-call
    // subset of what LHAT_BC_CALL does (vm.c's CALL case).
    if (!lhat_is_object_kind(callee, LHAT_OBJECT_SUBROUTINE)) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    const LhatClosure *closure = (const LhatClosure *)lhat_as_object(callee);
    if (closure->proto == NULL) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }

    // 14.4: a member that takes self^ is handed the receiver in a slot of its
    // own, and one that does not is a static member -- the receiver is then
    // simply not passed, the same way LHAT_BC_CALLMETHOD steps over it.
    bool pass_self = as_method && closure->proto->takes_self;

    size_t declared_slots = closure->proto->parameters;
    size_t required =
        closure->proto->has_variadic ? declared_slots - 1 : declared_slots;
    // 13.4 keeps self^ out of the parameter list a call writes, and
    // proto->parameters counts the slot it occupies -- so what the host
    // handed over is compared after the receiver is added back in.
    size_t written = count + (pass_self ? 1 : 0);
    if (closure->proto->has_variadic ? written < required
                                     : written != declared_slots) {
        return call_fault(m, LHAT_RUN_ARITY);
    }

    // 02 の 11.3改: an op^ may write self^ last instead, which says the right
    // operand is the receiver. The slot it occupies moves with it, so where
    // the receiver lands is read off the proto rather than assumed.
    size_t self_at =
        pass_self && closure->proto->self_last && required > 0 ? required - 1
                                                               : 0;

    const LhatProto *proto = closure->proto;
    LhatCoroutine *co = NULL;
    size_t next_base = 0;
    LhatSlots destination = m->slots;
    if (proto->yields) {
        co = lhat_coroutine_new(&m->objects, closure, proto->chunk.registers);
        if (co == NULL) {
            return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
        }
        destination = co->registers;
    } else {
        if (base >= m->frame_capacity) {
            return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
        }
        next_base = base == 0 ? 0 : m->frames[base - 1].base +
                                       m->frames[base - 1].closure->proto->chunk.registers;
        if (next_base + proto->chunk.registers >= m->slot_capacity) {
            return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
        }
    }

    size_t taken = 0;
    size_t laid = next_base;
    for (size_t i = 0; i < required; i++) {
        LhatValue value = pass_self && i == self_at ? receiver : arguments[taken++];
        laid += place_boundary_value(destination, laid, value);
    }
    // Host values occupy multiple slots. Their widths must agree with the
    // compiled parameter layout before a variadic table or scratch is placed.
    if (laid != next_base + (size_t)proto->parameter_slots -
                    (proto->has_variadic ? 1 : 0)) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }
    if (proto->has_variadic) {
        LhatTable *collected = lhat_table_new(&m->objects);
        if (collected == NULL) {
            return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
        }
        for (size_t i = taken; i < count; i++) {
            bool refused = false;
            if (lhat_is_hostvalue(arguments[i])) {
                return call_fault(m, LHAT_RUN_TYPE_ERROR);
            }
            if (!vm_set_key(m, collected, lhat_integer((int64_t)(i - taken + 1)),
                            arguments[i], &refused)) {
                return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
            }
        }
        lhat_slots_set(destination, laid, lhat_object((LhatObject *)collected));
    }
    if (co != NULL) {
        // Calling a yieldable body constructs it; only resume runs its code.
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        made.value = lhat_object((LhatObject *)co);
        return made;
    }

    vm_clear_scratch(m, next_base, closure->proto);

    vm_push_frame(m, closure, next_base, 0, 1);

    return vm_run_frames(m, base, false);
}

LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count)
{
    return host_call((Machine *)machine, callee, lhat_nil(), false, arguments,
                     count);
}

LhatRunResult lhat_machine_run_seeded(Machine *m, const LhatClosure *closure,
                                      const LhatValue *seed, size_t count)
{
    size_t base = m->frame_count;
    m->tuple_scratch_count = 0;  // 13.8改, as in lhat_run
    if (closure == NULL || closure->proto == NULL ||
        count > closure->proto->chunk.registers) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    if (base >= m->frame_capacity) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + closure->proto->chunk.registers >=
        m->slot_capacity) {  // 4.3改
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    for (size_t i = 0; i < count; i++) {
        lhat_slots_set(m->slots, next_base + i, seed[i]);
    }
    // The rest of the window is emptied: the collector walks a frame's whole
    // width, and whatever an earlier run left there is not this one's.
    for (size_t i = count; i < closure->proto->chunk.registers; i++) {
        lhat_slots_set(m->slots, next_base + i, lhat_nil());
    }

    vm_push_frame(m, closure, next_base, 0, 1);

    LhatRunResult ran = vm_run_frames(m, base, false);
    if (ran.status != LHAT_RUN_OK) {
        // As run_one_disposal abandons a failed cleanup: closed first, or a
        // place captured inside the evaluation would be left pointing at
        // slots the next call reuses.
        vm_close_upvalues(m, next_base);
        m->frame_count = base;
        m->fault_base = 0;
        m->fault_depth = 0;
    }
    return ran;
}

static size_t waiting_disposals(const Machine *m)
{
    size_t waiting = 0;
    for (const LhatCoroutine *co = m->pending_dispose; co != NULL;
         co = co->next_pending) {
        waiting++;
    }
    return waiting;
}

// 02 の 10.7: runs the cleanups of one dropped coroutine, from C. The frame
// goes just past whatever is on top -- the same measure host_call takes, and
// base == 0 (nothing running) is as good a case as any -- and the loop is
// entered at the drain, since a disposal frame has no instructions of its
// own to run.
//
// False when there was nothing waiting or nowhere to put the frame. What the
// cleanups answered comes back in `status`.
static bool run_one_disposal(Machine *m, LhatRunStatus *status)
{
    size_t base = m->frame_count;
    if (m->pending_dispose == NULL || base >= m->frame_capacity) {
        return false;
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + m->pending_dispose->closure->proto->chunk.registers >=
        m->slot_capacity) {  // 4.3改
        return false;
    }

    // gc.c only ever holds back a suspended BODY coroutine with cleanups
    // left, so the closure and the proto this reads through are there.
    LhatCoroutine *co = lhat_gc_take_pending(m);
    Frame *frame = NULL;
    size_t rbase = 0;
    const LhatChunk *chunk = NULL;
    size_t pc = 0;
    vm_enter_disposal_frame(m, co, next_base, 0, &frame, &rbase, &chunk, &pc);
    // 15.4: same as an explicit dispose -- the slot a yield^ answers into
    // gets nil^, since nothing sent anything in.
    lhat_slots_set(m->slots, rbase + co->sent_into, lhat_nil());
    *status = vm_run_frames(m, base, true).status;

    // 04 の 11.6改: a fault leaves its frames standing so that a traceback
    // can be read off them. There is no caller here to read one -- the
    // machine was tidying up after itself, and the run that dropped this
    // coroutine is long over -- and leaving them would be worse than
    // useless. The frames hold the closure this coroutine was suspended in,
    // and so a body a host may be about to free, while
    // lhat_machine_pending_disposals -- which is what it asks before freeing
    // -- would read zero. So the cleanup is abandoned and its frames go.
    //
    // 5.4: closed first, or a place captured inside the cleanup would be
    // left pointing at slots the next call reuses.
    if (*status != LHAT_RUN_OK) {
        vm_close_upvalues(m, next_base);
        m->frame_count = base;
        m->fault_base = 0;
        m->fault_depth = 0;
    }
    return true;
}

size_t lhat_machine_collectgarbage(LhatMachine *machine)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return 0;
    }
    lhat_gc_collect(m);

    // 02 の 10.7: and here the cleanups the collection held back get to run,
    // which inside a run is what an instruction boundary does for them. A
    // host calling this has no loop under it to do that, and the difference
    // matters: until they have run, a dropped coroutine still holds the
    // closure it was suspended in.
    //
    // Never on top of a disposal already under way -- two unwindings that
    // have nothing to do with each other must not interleave, which is the
    // same refusal the run loop makes.
    if (m->frame_count > 0 && m->frames[m->frame_count - 1].disposing) {
        return m->objects.count;
    }

    // Only what is queued now. A cleanup that drops another coroutine leaves
    // it for the next call -- the bound the run loop's `end_swept` puts on
    // the end of a run, for the same reason: this must not be something a
    // program can go on extending.
    size_t waiting = waiting_disposals(m);
    bool ran_any = false;
    for (size_t i = 0; i < waiting; i++) {
        LhatRunStatus status = LHAT_RUN_OK;
        if (!run_one_disposal(m, &status)) {
            break;
        }
        ran_any = true;
        // A cleanup that faulted stops the drain: the rest keep, and
        // lhat_machine_fault_* says what happened to this one.
        if (status != LHAT_RUN_OK) {
            break;
        }
    }

    // Their bodies have finished now, so they are DONE with no cleanups left
    // and gc.c will not hold them back again -- this is the cycle that
    // actually reclaims them.
    if (ran_any) {
        lhat_gc_collect(m);
    }
    return m->objects.count;
}
size_t lhat_machine_pending_disposals(const LhatMachine *machine)
{
    return machine != NULL ? waiting_disposals((const Machine *)machine) : 0;
}
LhatRunResult lhat_machine_call_member(LhatMachine *machine,
                                       LhatValue receiver, const char *name,
                                       size_t length,
                                       const LhatValue *arguments,
                                       size_t count)
{
    Machine *m = (Machine *)machine;
    if (name == NULL || count > LHAT_MAX_REGISTERS) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }
    // 14.4 reaches a member through a table; a value that is not one has no
    // members to reach, which is the same refusal an instruction makes.
    // 05 の 8.8: a hostdata value reads its members through the registered
    // type's table, exactly as an instruction's read does -- which is why
    // this is vm_readable_table rather than vm_table_of.
    const LhatTable *table = vm_readable_table(receiver);
    if (table == NULL) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }

    LhatString *key = lhat_string_new(&m->objects, name, length);
    if (key == NULL) {
        return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
    }
    // 14.7: an instance sees its definition's members too, and lhat_table_get
    // is what already walks that.
    LhatValue member = lhat_table_get(table, lhat_object((LhatObject *)key));

    // 14.12: at most one candidate fits, so this is a search and not a
    // choice. The lineup is what an instruction has lying in its registers:
    // the callee, the receiver, then what the call wrote.
    if (lhat_is_object_kind(member, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *group = (const LhatOverload *)lhat_as_object(member);
        LhatValue lineup[LHAT_MAX_REGISTERS + 2];
        lineup[0] = member;
        lineup[1] = receiver;
        for (size_t i = 0; i < count; i++) {
            lineup[i + 2] = arguments[i];
        }
        size_t picked_skip = 1;
        LhatValue chosen = lhat_nil();
        for (size_t i = 0; i < group->count; i++) {
            if (vm_fits_call(group->candidates[i], lineup, (uint8_t)count, true,
                          &picked_skip)) {
                chosen = group->candidates[i];
                break;
            }
        }
        if (lhat_is_nil(chosen)) {
            return call_fault(m, LHAT_RUN_NO_CANDIDATE);
        }
        member = chosen;
    }

    return host_call(m, member, receiver, true, arguments, count);
}

bool lhat_machine_make_coroutine(LhatMachine *machine, LhatHostStepFn step,
                                 void *context, LhatHostFn release,
                                 LhatValue held, LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || step == NULL || out == NULL) {
        return false;
    }
    LhatCoroutine *walk =
        lhat_host_iterator(&m->objects, step, context, release, held);
    if (walk == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)walk);
    return true;
}

LhatRunResult lhat_machine_resume(LhatMachine *machine, LhatValue coroutine,
                                  const LhatValue *sent, size_t sent_count)
{
    Machine *m = (Machine *)machine;
    m->tuple_scratch_count = 0;  // 13.8改, as in host_call
    if (sent == NULL) {
        sent_count = 0;
    }
    if (!lhat_is_object_kind(coroutine, LHAT_OBJECT_COROUTINE) ||
        sent_count > LHAT_MAX_TUPLE) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }
    LhatCoroutine *co = (LhatCoroutine *)lhat_as_object(coroutine);
    // 15.2's guards as the instructions make them -- resume subsumes start
    // here (lua_resume's shape): a fresh body runs from the top, and that
    // first `sent` is discarded, since no yield^ awaits it yet.
    if (co->state == LHAT_COROUTINE_DONE ||
        co->state == LHAT_COROUTINE_RUNNING) {
        return call_fault(m, LHAT_RUN_DEAD_COROUTINE);
    }

    // 16.3: a table's walk has no body, so one step is the whole of the
    // resume -- no frame entered. The pair crosses the boundary as the
    // positions of a tuple (13.8改), the way any tuple crosses it.
    if (co->source == LHAT_COROUTINE_TABLE) {
        LhatValue key, value;
        if (!lhat_table_walk(co, &key, &value)) {
            co->state = LHAT_COROUTINE_DONE;
            return call_fault(m, LHAT_RUN_OK);
        }
        co->state = LHAT_COROUTINE_SUSPENDED;
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (co->part != LHAT_WALK_PAIR) {
            made.value = co->part == LHAT_WALK_KEYS ? key : value;
            return made;
        }
        m->tuple_scratch[0] = key;
        m->tuple_scratch[1] = value;
        m->tuple_scratch_count = 2;
        made.value = key;
        made.positions = m->tuple_scratch;
        made.position_count = 2;
        return made;
    }

    // 05 の 8.8: and a host's walk is one step of its own C body. RUNNING
    // above already refuses a step resuming its own coroutine. 13.8改:
    // everything the resume sent goes over, the way it does to a body.
    if (co->source == LHAT_COROUTINE_HOST) {
        LhatValue out = lhat_nil();
        int written = 0;
        co->state = LHAT_COROUTINE_RUNNING;
        m->tuple_scratch_count = 0;
        bool more = co->step(machine, co->host_state, sent, sent_count,
                             m->tuple_scratch, &written);
        if (!boundary_answer(m, written, &out)) {
            return call_fault(m, LHAT_RUN_TUPLE_ARITY);
        }
        if (!more) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_coroutine_release((LhatObject *)co, machine);
            // 13.9: what it wrote as it ended is T, and a resume from C
            // reads it the way a written one does. Nothing written is a
            // walk that ended with nothing.
        } else {
            co->state = LHAT_COROUTINE_SUSPENDED;
            if (written == 0) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
        }
        // 05 の 8.9改2: a host value passes through whole -- the step wrote
        // it into the machine's scratch itself (lhat_make_hostvalue), which
        // is exactly where the caller reads the pointer form.
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (lhat_is_run(out)) {
            size_t positions = lhat_run_width(out);
            if (positions != m->tuple_scratch_count) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
            made.positions = m->tuple_scratch;
            made.position_count = positions;
            made.value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
        } else {
            made.value = out;
        }
        return made;
    }

    // A body: the frame goes back on at the base of a run of its own --
    // host_call's placement, re-entered where the yield^ left off, or at
    // the top when the body has not started. The yield that suspends it
    // again leaves through LHAT_BC_YIELD's base case, the way a return
    // leaves through its own.
    if (co->closure == NULL || co->closure->proto == NULL) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    // 13.8改: the count a resume sends is the R's -- checked here the way
    // the natives check it, with the same tolerance for a proto the checker
    // never reached, and the same stop for a run that would not fit the
    // suspended frame. A fresh body's first send is discarded, so any count
    // passes there.
    // 05 の 8.9改2: a host value rides the send whole -- the pointer form
    // the host already holds (lhat_make_hostvalue's scratch, or an argument
    // echoed back), one seat, never mixed into a run. The lay-down side
    // expands it (vm_enter_resume_frame), and unlike a compiled resume the
    // pointer never aims into the registers about to be restored over.
    for (size_t i = 0; i < sent_count; i++) {
        if (lhat_is_hostvalue(sent[i]) && sent_count > 1) {
            return call_fault(m, LHAT_RUN_TYPE_ERROR);
        }
    }
    if (co->state == LHAT_COROUTINE_SUSPENDED) {
        const LhatProto *from = co->closure->proto;
        // As at the natives' resume: an unknown proto keeps the old ceiling
        // of one, since only a checked body has reserved the run's slots.
        if (from->yield_receives_known
                ? sent_count != from->yield_receive_count
                : sent_count > 1) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        if (sent_count > 1 &&
            (size_t)co->sent_into + sent_count >= co->register_count) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        if (sent_count == 1 && lhat_is_hostvalue(sent[0])) {
            const LhatValueUnion *srun = sent[0].as.hostvalue_run;
            const LhatHostValueTag *stag =
                srun != NULL ? srun[0].hostvalue : NULL;
            // The lowered R is what the body reserved its slots by, so a
            // checked body holds the send to its own tag -- stronger than
            // the count check alone. An unchecked one still gets the room
            // checked.
            const struct LhatRuntimeType *wants = from->yield_receive_type;
            if (stag == NULL ||
                (wants != NULL && (wants->kind != LHAT_TYPE_RT_HOSTVALUE ||
                                   wants->hostvalue_tag != stag)) ||
                (size_t)co->sent_into + stag->width > co->register_count) {
                return call_fault(m, LHAT_RUN_TYPE_ERROR);
            }
        }
    }
    size_t base = m->frame_count;
    if (base >= m->frame_capacity) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + co->register_count >= m->slot_capacity) {  // 4.3改
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    // prepared = 1: the host boundary takes one value, as host_call says.
    // The result slot is never read -- the base cases return instead.
    vm_enter_resume_frame(m, co, next_base, 0, 1, sent, sent_count);
    return vm_run_frames(m, base, false);
}

// 05 の 8.8改3: what a carry reads of a coroutine that has not started, and
// the maker that puts one back together.
static const LhatCoroutine *fresh_body(LhatValue coroutine)
{
    if (!lhat_is_object_kind(coroutine, LHAT_OBJECT_COROUTINE)) {
        return NULL;
    }
    const LhatCoroutine *co =
        (const LhatCoroutine *)lhat_as_object(coroutine);
    // Every other field of a FRESH body is still what lhat_coroutine_new
    // left: only vm_enter_resume_frame and YIELD write pc, sent_into, the
    // cleanups and the open list, and neither has run.
    return co->state == LHAT_COROUTINE_FRESH &&
                   co->source == LHAT_COROUTINE_BODY && co->closure != NULL
               ? co
               : NULL;
}

bool lhat_coroutine_is_fresh_body(LhatValue coroutine)
{
    return fresh_body(coroutine) != NULL;
}

size_t lhat_coroutine_fresh_width(LhatValue coroutine)
{
    const LhatCoroutine *co = fresh_body(coroutine);
    return co != NULL ? co->register_count : 0;
}

LhatValue lhat_coroutine_fresh_slot(LhatValue coroutine, size_t index)
{
    const LhatCoroutine *co = fresh_body(coroutine);
    return co != NULL && index < co->register_count
               ? lhat_slots_get(co->registers, index)
               : lhat_nil();
}

LhatValue lhat_coroutine_fresh_closure(LhatValue coroutine)
{
    const LhatCoroutine *co = fresh_body(coroutine);
    return co != NULL ? lhat_object((LhatObject *)(void *)co->closure)
                      : lhat_nil();
}

bool lhat_machine_make_coroutine_from(LhatMachine *machine, LhatValue closure,
                                      const LhatValue *slots, size_t count,
                                      LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || out == NULL ||
        !lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return false;
    }
    const LhatClosure *made = (const LhatClosure *)lhat_as_object(closure);
    if (made->proto == NULL || !made->proto->yields) {
        return false;
    }
    LhatCoroutine *co =
        lhat_coroutine_new(&m->objects, made, made->proto->chunk.registers);
    if (co == NULL) {
        return false;
    }
    // The image as it stands: the same slot-blind copy the interpreter makes
    // where a call answers a coroutine, so a wide host value's continuation
    // slots travel beside their head.
    for (size_t i = 0; i < count && i < co->register_count; i++) {
        lhat_slots_set(co->registers, i, slots[i]);
        lhat_gc_barrier_back(m, (LhatObject *)co, slots[i]);
    }
    *out = lhat_object((LhatObject *)co);
    return true;
}
LhatRunResult lhat_machine_continue(LhatMachine *machine)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || !m->suspended) {
        LhatRunResult nothing;
        memset(&nothing, 0, sizeof nothing);
        nothing.status = LHAT_RUN_OK;
        nothing.value = lhat_nil();
        return nothing;
    }
    m->suspended = false;
    return vm_run_frames(m, m->suspended_base, false);
}

bool lhat_machine_coroutine_done(LhatValue coroutine)
{
    return lhat_is_object_kind(coroutine, LHAT_OBJECT_COROUTINE) &&
           ((const LhatCoroutine *)lhat_as_object(coroutine))->state ==
               LHAT_COROUTINE_DONE;
}
