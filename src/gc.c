// L^ (lhat) -- the collector. gc.h says what it is and why it is this shape;
// this is the working form of it.
//
// Two halves meet here. The first is the object graph -- what one object
// refers to -- which used to sit beside the objects themselves. The second is
// the roots, which only the machine knows, and 02 の 10.7's holding back of a
// coroutine that still owes its cleanups. Neither half collects anything
// without the other, so both are here.

#include "gc.h"

#include "lhatconfig.h"
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
                lhat_gc_reach(gray, table->array[i]);
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
            lhat_gc_reach(gray, upvalue->closed);
            if (upvalue->location != NULL) {
                lhat_gc_reach(gray, *upvalue->location);
            }
            return;
        }

        case LHAT_OBJECT_COROUTINE: {
            const LhatCoroutine *co = (const LhatCoroutine *)object;
            for (size_t i = 0; i < co->register_count; i++) {
                lhat_gc_reach(gray, co->registers[i]);
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
        case LHAT_OBJECT_HOST:
            lhat_gc_reach(gray, ((const LhatHost *)object)->bound);
            return;

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

size_t lhat_gc_sweep(LhatHeap *heap, Machine *machine)
{
    // The caller has already swapped, so this is the white the marking was
    // handing out -- and so the colour of everything it did not reach.
    uint8_t dead = lhat_gc_other_white(heap->white);

    size_t freed = 0;
    LhatObject **link = &heap->objects;
    while (*link != NULL) {
        LhatObject *object = *link;
        if (object->color != dead) {
            // Black, or one born since the swap. Either way it lives, and
            // goes back to white for the collection after this one.
            object->color = heap->white;
            link = &object->next;
            continue;
        }
        *link = object->next;
        // 05 の 8.8: what the host made is the host's to free, and this is
        // 10.7's last resort for one nothing disposed of by hand.
        lhat_hostdata_release(object, machine);
        lhat_object_free(object);
        heap->count--;
        freed++;
    }
    return freed;
}

// ---------------------------------------------------------------------------
// The roots, and one whole cycle
// ---------------------------------------------------------------------------

// The roots: everything the program can still reach. Collection happens
// between instructions, so every live value is in a register, a frame or the
// open list -- there is no half-built object to miss.
static void mark_roots(Machine *m)
{
    // 05 の 8.6: L^ is reachable from anywhere without being held anywhere,
    // so the machine is what keeps it and what it carries alive.
    lhat_gc_reach(&m->gray, lhat_object((LhatObject *)m->environment));
    for (size_t i = 0; i < m->frame_count; i++) {
        Frame *frame = &m->frames[i];
        lhat_gc_reach(&m->gray,
                      lhat_object((LhatObject *)(void *)frame->closure));
        lhat_gc_reach(&m->gray, lhat_object((LhatObject *)frame->coroutine));
        lhat_gc_reach(&m->gray, frame->answer);
        // 5.2 fixed the frame's width at compile time, and the scratch above
        // the names is inside it, so this covers the values a half-finished
        // expression is holding.
        const LhatProto *proto = frame->closure->proto;
        size_t width = proto != NULL ? proto->chunk.registers : 0;
        for (size_t r = 0; r < width; r++) {
            lhat_gc_reach(&m->gray, frame->base[r]);
        }
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

// Follows what the roots reached until nothing grey is left.
static void follow_gray(Machine *m)
{
    while (m->gray != NULL) {
        LhatObject *object = m->gray;
        m->gray = object->gclist;
        object->gclist = NULL;
        // Black before the children rather than after: one that refers to
        // itself would otherwise be put back on the list it was just taken
        // off, and go round for as long as the loop cared to.
        object->color = LHAT_GC_BLACK;
        lhat_gc_children(&m->gray, object);
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

// 03 の 1.2 keeps Lua's incremental collector as something to borrow later.
// This is the working form 5.1's order asks for first.
void lhat_gc_collect(Machine *m)
{
    mark_roots(m);
    follow_gray(m);
    // 02 の 10.7: held back before the sweep, and what they reach followed
    // after -- a coroutine kept for its cleanups keeps its registers too.
    hold_pending_disposals(m);
    follow_gray(m);

    // The swap: from here on a new object is born the other white, so the
    // only things still wearing this one are what the marking did not reach.
    m->objects.white = lhat_gc_other_white(m->objects.white);
    m->collected += lhat_gc_sweep(&m->objects, m);

    // What survived is the new baseline, so a program holding a lot does not
    // collect on every allocation. The floor keeps a nearly-empty heap from
    // triggering the next collection almost immediately.
    m->threshold = m->objects.count * LHAT_GC_GROWTH_FACTOR + LHAT_GC_MIN_THRESHOLD;
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
