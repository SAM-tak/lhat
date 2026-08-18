// L^ (lhat) -- the collector. gc.h says what it is and why it is this shape;
// this is the working form of it.
//
// Two halves meet here. The first is the object graph -- what one object
// refers to. The second is
// the roots, which only the machine knows, and 02 の 10.7's holding back of a
// coroutine that still owes its cleanups. Neither half collects anything
// without the other, so both are here.

#include "gc.h"

#ifdef LHAT_GC_PARANOID
#include <stdio.h>
#include <stdlib.h>
#endif

#include "lhat/config.h"
#include "machine.h"

// ---------------------------------------------------------------------------
// The object graph
// ---------------------------------------------------------------------------

// A push that cannot fail: the link is the object's own. Only a white object
// goes on the list -- one already gray is on it, and a black one has been
// taken off it and looked into.
static void reach(LhatObject **gray, LhatObject *object)
{
    if (object == NULL || !lhat_gc_is_white(object)) {
        return;
    }
    object->color = LHAT_GC_GRAY;
    object->gclist = *gray;
    *gray = object;
}

void lhat_gc_reach(LhatObject **gray, LhatValue value)
{
    if (lhat_is_object(value)) {
        reach(gray, lhat_as_object(value));
    }
}

void lhat_gc_children(LhatObject **gray, LhatObject *object)
{
    switch (object->kind) {
        case LHAT_OBJECT_STRING:
            return;

        case LHAT_OBJECT_TABLE: {
            const LhatTable *table = (const LhatTable *)object;
            for (size_t i = 0; i < table->array_count; i++) {
                lhat_gc_reach(gray, lhat_slots_get(table->array, i));
            }
            for (size_t i = 0; i < table->entry_capacity; i++) {
                lhat_gc_reach(gray, table->entries[i].key);
                lhat_gc_reach(gray, table->entries[i].value);
            }
            // 14.2 fixes this when the instance is made, so it is a plain
            // reference like any other.
            reach(gray, (LhatObject *)(void *)table->definition);
            return;
        }

        case LHAT_OBJECT_ERROR: {
            const LhatError *error = (const LhatError *)object;
            reach(gray, (LhatObject *)(void *)error->kind);
            reach(gray, (LhatObject *)error->fields);
            return;
        }

        case LHAT_OBJECT_ERROR_KIND: {
            const LhatErrorKind *kind = (const LhatErrorKind *)object;
            reach(gray, (LhatObject *)(void *)kind->group);
            reach(gray, (LhatObject *)(void *)kind->name);
            return;
        }

        case LHAT_OBJECT_SUBROUTINE: {
            const LhatClosure *closure = (const LhatClosure *)object;
            for (size_t i = 0; i < closure->upvalue_count; i++) {
                reach(gray, (LhatObject *)closure->upvalues[i]);
            }
            return;
        }

        case LHAT_OBJECT_UPVALUE: {
            // 5.4: while it is open the place is a stack slot, and the stack
            // is a root of its own. Closed, the value is here.
            const LhatUpvalue *upvalue = (const LhatUpvalue *)object;
            if (upvalue->location.value != NULL) {
                lhat_gc_reach(gray, lhat_ref_get(upvalue->location));
            }
            // 15.4: while the owning frame is suspended, the place is a slot
            // of that coroutine's saved registers -- so the coroutine has to
            // live as long as this capture does, even once nothing else
            // holds it.
            reach(gray, (LhatObject *)(void *)upvalue->suspended_in);
            return;
        }

        case LHAT_OBJECT_COROUTINE: {
            const LhatCoroutine *co = (const LhatCoroutine *)object;
            for (size_t i = 0; i < co->register_count; i++) {
                lhat_gc_reach(gray, lhat_slots_get(co->registers, i));
            }
            reach(gray, (LhatObject *)(void *)co->closure);
            reach(gray, (LhatObject *)(void *)co->walking);
            return;
        }

        case LHAT_OBJECT_NATIVE:
            lhat_gc_reach(gray, ((const LhatNative *)object)->bound);
            return;

        // 05 の 8.7: `context` is the host's, and the collector cannot see
        // into it. What is reachable from here is the receiver alone.
        case LHAT_OBJECT_HOST: {
            const LhatHost *host = (const LhatHost *)object;
            lhat_gc_reach(gray, host->bound);
            // 02 の 14.12: the parameter types a registration built. Nothing
            // else holds them -- a proto's live in its chunk, but a host has
            // no chunk, so these are on the heap and reached from here.
            for (size_t i = 0; host->parameter_types != NULL &&
                               i < (size_t)host->parameters;
                 i++) {
                reach(gray, (LhatObject *)host->parameter_types[i]);
            }
            return;
        }

        // 05 の 8.8: the same for the pointer. The members are the registered
        // type's table, which the registry holds anyway -- reached here so
        // that a value handed to L^ keeps what answers its calls.
        case LHAT_OBJECT_HOSTDATA:
            reach(gray, (LhatObject *)((const LhatHostData *)object)->members);
            return;

        case LHAT_OBJECT_TYPE: {
            const LhatRuntimeType *type = (const LhatRuntimeType *)object;
            for (size_t i = 0; i < type->part_count; i++) {
                reach(gray, (LhatObject *)type->parts[i]);
            }
            for (size_t i = 0; i < type->member_count; i++) {
                reach(gray, (LhatObject *)(void *)type->members[i].name);
                reach(gray, (LhatObject *)type->members[i].type);
            }
            reach(gray, (LhatObject *)type->result);
            reach(gray, (LhatObject *)type->variadic);
            reach(gray, (LhatObject *)type->instance);  // 02 の 14.7改
            reach(gray, (LhatObject *)(void *)type->error_kind);
            return;
        }

        case LHAT_OBJECT_OVERLOAD: {
            const LhatOverload *overload = (const LhatOverload *)object;
            for (size_t i = 0; i < overload->count; i++) {
                lhat_gc_reach(gray, overload->candidates[i]);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// The roots, and running a cycle
// ---------------------------------------------------------------------------

// The roots: everything the program can still reach. Collection happens
// between instructions, so every live value is in a register, a frame or the
// open list -- there is no half-built object to miss.
static void mark_roots(Machine *m)
{
    // 05 の 8.6: L^ is reachable from anywhere without being held anywhere,
    // so the machine is what keeps it and what it carries alive.
    lhat_gc_reach(&m->gray, lhat_object((LhatObject *)m->environment));
    // 02 の 14.11: the machine's own key to a definition's prototype. No
    // frame holds it, so the machine is its root too.
    lhat_gc_reach(&m->gray, lhat_object((LhatObject *)m->self_key));
    for (size_t i = 0; i < m->frame_count; i++) {
        Frame *frame = &m->frames[i];
        lhat_gc_reach(&m->gray,
                      lhat_object((LhatObject *)(void *)frame->closure));
        lhat_gc_reach(&m->gray, lhat_object((LhatObject *)frame->coroutine));
        lhat_gc_reach(&m->gray, frame->answer);
        // 02 の 13.8改: a tuple answer rides the frame's own room through the
        // drain, so its positions are roots the way a register is -- and they
        // are invisible to the line above, which reaches one value. A host
        // value in the same room needs nothing here: its continuation slots
        // are raw bytes and hold no reference. This matters because cleanups
        // run arbitrary L^ code between the return^ and the pop, with a
        // collection in the pop path itself.
        if (lhat_is_run(frame->answer)) {
            size_t positions = lhat_run_width(frame->answer);
            for (size_t at = 1; at <= positions; at++) {
                LhatValue held;
                held.as = frame->answer_run[at];
                held.tag = (LhatValueTag)frame->answer_tags[at];
                lhat_gc_reach(&m->gray, held);
            }
        }
        // 5.2 fixed the frame's width at compile time, and the scratch above
        // the names is inside it, so this covers the values a half-finished
        // expression is holding.
        const LhatProto *proto = frame->closure->proto;
        size_t width = proto != NULL ? proto->chunk.registers : 0;
        for (size_t r = 0; r < width; r++) {
            lhat_gc_reach(&m->gray, lhat_slots_get(m->slots, frame->base + r));
        }
    }
    // 02 の 13.8改: the several values a host is answering with, held in the
    // machine's own room until the call returns and the run is laid down.
    // A host may allocate in between -- build a table, call back into L^ --
    // so these are roots for as long as the count says they are there. The
    // host-value scratch beside them needs nothing: it is bytes.
    for (size_t at = 0; at < m->tuple_scratch_count; at++) {
        lhat_gc_reach(&m->gray, m->tuple_scratch[at]);
    }
    for (LhatUpvalue *open = m->open; open != NULL; open = open->next_open) {
        lhat_gc_reach(&m->gray, lhat_object((LhatObject *)open));
    }
    // 02 の 10.7: one already waiting to have its cleanups run is alive
    // until they have -- it was unreachable to the program when it was put
    // here, and the sweep would take it back if this did not hold it.
    for (LhatCoroutine *co = m->pending_dispose; co != NULL;
         co = co->next_pending) {
        lhat_gc_reach(&m->gray, lhat_object((LhatObject *)co));
    }
}

// Takes the object at the head of the gray list and looks into it. The list
// is the whole of the collector's progress, so one of these is the smallest
// piece of marking there is.
static void propagate_one(Machine *m)
{
    LhatObject *object = m->gray;
    m->gray = object->gclist;
    object->gclist = NULL;
    // Black before the children rather than after: one that refers to itself
    // would otherwise be put back on the list it was just taken off, and go
    // round for as long as the loop cared to.
    object->color = LHAT_GC_BLACK;
    lhat_gc_children(&m->gray, object);
}

// Follows what the roots reached until nothing gray is left.
static void follow_gray(Machine *m)
{
    while (m->gray != NULL) {
        propagate_one(m);
    }
}

// 02 の 10.7: a coroutine the program can no longer reach may still owe its
// pending finally^ bodies. Running them here is out of the question -- they
// are L^ code and the heap is about to be swept -- so this holds one back
// instead: it is reached (which keeps the sweep off it for this cycle) and
// put on the machine's list, to be run at an instruction boundary and
// collected by a later cycle. Lua takes the same two-cycle route for the
// same reason.
//
// Nothing marks one as "already held". While it is on the list mark_roots
// reaches it, so the white test below is what keeps this pass from finding
// it twice; and once its cleanups have run its count is 0 and its state is
// DONE, so it never answers here again and a later cycle takes it like
// anything else. What a cleanup did in between -- including storing what it
// reached somewhere still live -- is asked about from scratch then.
static void hold_pending_disposals(Machine *m)
{
    for (LhatObject *object = m->objects.objects; object != NULL;
         object = object->next) {
        if (!lhat_gc_is_white(object) || object->kind != LHAT_OBJECT_COROUTINE) {
            continue;
        }
        LhatCoroutine *co = (LhatCoroutine *)object;
        // 16.3's walk of a table has no body and so no cleanups, and holds
        // neither closure nor registers -- entering a frame for one would
        // read through NULL. Said outright rather than left to the count.
        if (co->source != LHAT_COROUTINE_BODY ||
            co->state != LHAT_COROUTINE_SUSPENDED || co->cleanup_count == 0) {
            continue;
        }
        lhat_gc_reach(&m->gray, lhat_object(object));
        co->next_pending = m->pending_dispose;
        m->pending_dispose = co;
    }
}

#ifdef LHAT_GC_PARANOID
// The whole of 5.12's invariant, asked outright: no black object refers to a
// white one. A missing barrier breaks exactly this and breaks it silently --
// the object is freed while live and the program reads freed memory some
// time later, somewhere else. So it is worth a pass over the heap in a build
// that can afford one.
//
// lhat_gc_children does the asking. Given a black object it grays every
// white child, so an empty list afterwards is the invariant holding, and one
// that is not empty names the object that broke it.
static void check_invariant(Machine *m)
{
    LhatObject *offenders = NULL;
    for (LhatObject *object = m->objects.objects; object != NULL;
         object = object->next) {
        if (!lhat_gc_is_black(object)) {
            continue;
        }
        lhat_gc_children(&offenders, object);
        if (offenders != NULL) {
            fprintf(stderr,
                    "lhat: a black object of kind %d refers to a white one "
                    "of kind %d -- a write to it went without a barrier\n",
                    (int)object->kind, (int)offenders->kind);
            fflush(stderr);
#ifdef _MSC_VER
            // Left alone, abort() stops at a dialog with nobody there to
            // answer it, and a test run reads that as a hang rather than as
            // the failure it is.
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
            abort();
        }
    }
}
#endif

// The one part of a cycle the program may not run across. Everything the
// marking could have missed is settled here.
//
// The roots are read a second time, from scratch. They are the one place the
// program writes to without a barrier -- registers, frames, open upvalues --
// and L^ has one set of them rather than a thread apiece, so re-reading them
// is the whole of what Lua does by re-traversing every thread.
static void atomic(Machine *m)
{
    mark_roots(m);
    follow_gray(m);
    // 02 の 10.7: held back before the sweep, and what they reach followed
    // after -- a coroutine kept for its cleanups keeps its registers too.
    hold_pending_disposals(m);
    follow_gray(m);

#ifdef LHAT_GC_PARANOID
    check_invariant(m);
#endif

    // The swap: from here on a new object is born the other white, so the
    // only things still wearing this one are what the marking did not reach.
    m->objects.white = lhat_gc_other_white(m->objects.white);
    m->sweep = &m->objects.objects;
    m->gcstate = LHAT_GC_SWEEP;
}

// Frees up to `budget` objects' worth of the heap, picking up where the last
// step left off. Answers how many it looked at.
static size_t sweep_some(Machine *m, size_t budget)
{
    uint8_t dead = lhat_gc_other_white(m->objects.white);
    size_t looked = 0;
    while (looked < budget && *m->sweep != NULL) {
        LhatObject *object = *m->sweep;
        looked++;
        if (object->color != dead) {
            // Black, or born since the swap. Either way it lives, and goes
            // back to white for the cycle after this one.
            object->color = m->objects.white;
            m->sweep = &object->next;
            continue;
        }
        *m->sweep = object->next;
        // 05 の 8.8: what the host made is the host's to free, and this is
        // 10.7's last resort for one nothing disposed of by hand.
        lhat_hostdata_release(object, m);
        lhat_object_free(object);
        m->objects.count--;
        m->collected++;
    }
    if (*m->sweep == NULL) {
        m->sweep = NULL;
        m->gcstate = LHAT_GC_PAUSE;
    }
    return looked;
}

// Reads the roots and leaves the collector following them.
static void start_cycle(Machine *m)
{
    mark_roots(m);
    m->gcstate = LHAT_GC_PROPAGATE;
}

// The smallest piece of work there is in whatever phase the cycle is in.
// Answers how much it did, in objects looked at. `atomic` is the exception
// and is charged what it is: the pause a step-at-a-time collector still has.
static size_t single_step(Machine *m)
{
    switch (m->gcstate) {
        case LHAT_GC_PAUSE:
            start_cycle(m);
            return 1;

        case LHAT_GC_PROPAGATE:
            if (m->gray != NULL) {
                propagate_one(m);
                return 1;
            }
            atomic(m);
            return LHAT_GC_STEP_WORK;

        case LHAT_GC_SWEEP:
            return sweep_some(m, LHAT_GC_STEP_WORK);
    }
    return 0;
}

// 03 の 1.2: Lua's incremental collector, borrowed. A step's worth of
// whichever phase the cycle is in, and then back to the program.
void lhat_gc_step(Machine *m)
{
    size_t done = 0;
    do {
        done += single_step(m);
    } while (done < LHAT_GC_STEP_WORK && m->gcstate != LHAT_GC_PAUSE);

    if (m->gcstate == LHAT_GC_PAUSE) {
        // A cycle just ended. What survived is the new baseline, so a program
        // holding a lot does not collect on every allocation, and the floor
        // keeps a nearly-empty heap from starting the next one at once.
        m->threshold =
            m->objects.count * LHAT_GC_GROWTH_FACTOR + LHAT_GC_MIN_THRESHOLD;
    } else {
        // In the middle of one, so the next step is due after a fixed number
        // of allocations rather than at a size of heap.
        m->threshold = m->objects.count + LHAT_GC_STEP_SIZE;
    }
#ifdef LHAT_GC_PARANOID
    // A step at every instruction, so that a cycle is nearly always half
    // done and every barrier is on the path something takes. Without this a
    // small heap finishes its marking inside one step and the barriers are
    // never reached at all -- which reads as "the tests pass" and means
    // "the tests do not go there". Lua's HARDMEMTESTS is the same idea.
    m->threshold = 0;
#endif
}

void lhat_gc_collect(Machine *m)
{
    // Whatever is under way first: its marking began before the program
    // dropped whatever it dropped since, so finishing it is not the same as
    // asking the question now. Then one cycle from a standing start, which
    // is. Lua's luaC_fullgc takes the same two.
    while (m->gcstate != LHAT_GC_PAUSE) {
        single_step(m);
    }
    start_cycle(m);
    while (m->gcstate != LHAT_GC_PAUSE) {
        single_step(m);
    }
    m->threshold =
        m->objects.count * LHAT_GC_GROWTH_FACTOR + LHAT_GC_MIN_THRESHOLD;
}

// ---------------------------------------------------------------------------
// The barriers
// ---------------------------------------------------------------------------

void lhat_gc_barrier(Machine *m, LhatObject *parent, LhatValue value)
{
    if (m->gcstate != LHAT_GC_PROPAGATE || !lhat_gc_is_black(parent)) {
        return;
    }
    lhat_gc_reach(&m->gray, value);
}

void lhat_gc_barrier_back(Machine *m, LhatObject *parent, LhatValue value)
{
    if (m->gcstate != LHAT_GC_PROPAGATE || !lhat_gc_is_black(parent) ||
        !lhat_is_object(value) || !lhat_gc_is_white(lhat_as_object(value))) {
        return;
    }
    // Back on the list, to be looked at again once the writing has settled.
    parent->color = LHAT_GC_GRAY;
    parent->gclist = m->gray;
    m->gray = parent;
}

// 02 の 10.7: the next coroutine waiting to have its cleanups run, or NULL.
LhatCoroutine *lhat_gc_take_pending(Machine *m)
{
    LhatCoroutine *co = m->pending_dispose;
    if (co != NULL) {
        m->pending_dispose = co->next_pending;
        co->next_pending = NULL;
    }
    return co;
}
