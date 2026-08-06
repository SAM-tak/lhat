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
#include "port.h"

// ---------------------------------------------------------------------------
// The object graph
// ---------------------------------------------------------------------------

void lhat_gray_dispose(LhatGray *gray)
{
    lhat_free(gray->items);
    gray->items = NULL;
    gray->count = 0;
    gray->capacity = 0;
}

static bool gray_push(LhatGray *gray, LhatObject *object)
{
    if (gray->count == gray->capacity) {
        size_t grown = gray->capacity ? gray->capacity * 2 : 64;
        LhatObject **bigger =
            (LhatObject **)lhat_realloc(gray->items, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        gray->items = bigger;
        gray->capacity = grown;
    }
    gray->items[gray->count++] = object;
    return true;
}

static bool reach(LhatGray *gray, LhatObject *object)
{
    if (object == NULL || object->marked) {
        return true;
    }
    object->marked = true;
    return gray_push(gray, object);
}

bool lhat_gc_reach(LhatGray *gray, LhatValue value)
{
    return !lhat_is_object(value) || reach(gray, lhat_as_object(value));
}

bool lhat_gc_children(LhatGray *gray, LhatObject *object)
{
    switch (object->kind) {
        case LHAT_OBJECT_STRING:
            return true;

        case LHAT_OBJECT_TABLE: {
            const LhatTable *table = (const LhatTable *)object;
            for (size_t i = 0; i < table->array_count; i++) {
                if (!lhat_gc_reach(gray, table->array[i])) {
                    return false;
                }
            }
            for (size_t i = 0; i < table->entry_capacity; i++) {
                if (!lhat_gc_reach(gray, table->entries[i].key) ||
                    !lhat_gc_reach(gray, table->entries[i].value)) {
                    return false;
                }
            }
            // 14.2 fixes this when the instance is made, so it is a plain
            // reference like any other.
            return reach(gray, (LhatObject *)(void *)table->definition);
        }

        case LHAT_OBJECT_ERROR: {
            const LhatError *error = (const LhatError *)object;
            return reach(gray, (LhatObject *)(void *)error->kind) &&
                   reach(gray, (LhatObject *)error->fields);
        }

        case LHAT_OBJECT_ERROR_KIND: {
            const LhatErrorKind *kind = (const LhatErrorKind *)object;
            return reach(gray, (LhatObject *)(void *)kind->group) &&
                   reach(gray, (LhatObject *)(void *)kind->name);
        }

        case LHAT_OBJECT_SUBROUTINE: {
            const LhatClosure *closure = (const LhatClosure *)object;
            for (size_t i = 0; i < closure->upvalue_count; i++) {
                if (!reach(gray, (LhatObject *)closure->upvalues[i])) {
                    return false;
                }
            }
            return true;
        }

        case LHAT_OBJECT_UPVALUE: {
            // 5.4: while it is open the place is a stack slot, and the stack
            // is a root of its own. Closed, the value is here.
            const LhatUpvalue *upvalue = (const LhatUpvalue *)object;
            return lhat_gc_reach(gray, upvalue->closed) &&
                   (upvalue->location == NULL ||
                    lhat_gc_reach(gray, *upvalue->location));
        }

        case LHAT_OBJECT_COROUTINE: {
            const LhatCoroutine *co = (const LhatCoroutine *)object;
            for (size_t i = 0; i < co->register_count; i++) {
                if (!lhat_gc_reach(gray, co->registers[i])) {
                    return false;
                }
            }
            return reach(gray, (LhatObject *)(void *)co->closure) &&
                   reach(gray, (LhatObject *)(void *)co->walking);
        }

        case LHAT_OBJECT_NATIVE:
            return lhat_gc_reach(gray, ((const LhatNative *)object)->bound);

        // 05 の 8.7: `context` is the host's, and the collector cannot see
        // into it. What is reachable from here is the receiver alone.
        case LHAT_OBJECT_HOST:
            return lhat_gc_reach(gray, ((const LhatHost *)object)->bound);

        // 05 の 8.8: the same for the pointer. The members are the registered
        // type's table, which the registry holds anyway -- reached here so
        // that a value handed to L^ keeps what answers its calls.
        case LHAT_OBJECT_HOSTDATA:
            return reach(gray, (LhatObject *)((const LhatHostData *)object)->members);

        case LHAT_OBJECT_TYPE: {
            const LhatRuntimeType *type = (const LhatRuntimeType *)object;
            for (size_t i = 0; i < type->part_count; i++) {
                if (!reach(gray, (LhatObject *)type->parts[i])) {
                    return false;
                }
            }
            for (size_t i = 0; i < type->member_count; i++) {
                if (!reach(gray, (LhatObject *)(void *)type->members[i].name) ||
                    !reach(gray, (LhatObject *)type->members[i].type)) {
                    return false;
                }
            }
            if (!reach(gray, (LhatObject *)type->result) ||
                !reach(gray, (LhatObject *)type->variadic)) {
                return false;
            }
            return reach(gray, (LhatObject *)(void *)type->error_kind);
        }

        case LHAT_OBJECT_OVERLOAD: {
            const LhatOverload *overload = (const LhatOverload *)object;
            for (size_t i = 0; i < overload->count; i++) {
                if (!lhat_gc_reach(gray, overload->candidates[i])) {
                    return false;
                }
            }
            return true;
        }
    }
    return true;
}

size_t lhat_gc_sweep(LhatHeap *heap, Machine *machine)
{
    size_t freed = 0;
    LhatObject **link = &heap->objects;
    while (*link != NULL) {
        LhatObject *object = *link;
        if (object->marked) {
            object->marked = false;  // ready for the next collection
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
static bool mark_roots(Machine *m)
{
    // 05 の 8.6: L^ is reachable from anywhere without being held anywhere,
    // so the machine is what keeps it and what it carries alive.
    if (!lhat_gc_reach(&m->gray, lhat_object((LhatObject *)m->environment))) {
        return false;
    }
    for (size_t i = 0; i < m->frame_count; i++) {
        Frame *frame = &m->frames[i];
        if (!lhat_gc_reach(&m->gray,
                           lhat_object((LhatObject *)(void *)frame->closure)) ||
            !lhat_gc_reach(&m->gray,
                           lhat_object((LhatObject *)frame->coroutine)) ||
            !lhat_gc_reach(&m->gray, frame->answer)) {
            return false;
        }
        // 5.2 fixed the frame's width at compile time, and the scratch above
        // the names is inside it, so this covers the values a half-finished
        // expression is holding.
        const LhatProto *proto = frame->closure->proto;
        size_t width = proto != NULL ? proto->chunk.registers : 0;
        for (size_t r = 0; r < width; r++) {
            if (!lhat_gc_reach(&m->gray, frame->base[r])) {
                return false;
            }
        }
    }
    for (LhatUpvalue *open = m->open; open != NULL; open = open->next_open) {
        if (!lhat_gc_reach(&m->gray, lhat_object((LhatObject *)open))) {
            return false;
        }
    }
    // 02 の 10.7: one already waiting to have its cleanups run is alive
    // until they have -- it was unreachable to the program when it was put
    // here, and the sweep would take it back if this did not hold it.
    for (LhatCoroutine *co = m->pending_dispose; co != NULL;
         co = co->next_pending) {
        if (!lhat_gc_reach(&m->gray, lhat_object((LhatObject *)co))) {
            return false;
        }
    }
    return true;
}

// Follows what the roots reached until nothing grey is left.
static bool follow_gray(Machine *m)
{
    while (m->gray.count > 0) {
        LhatObject *object = m->gray.items[--m->gray.count];
        if (!lhat_gc_children(&m->gray, object)) {
            return false;
        }
    }
    return true;
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
// reaches it, so the `marked` test above is what keeps this pass from
// finding it twice; and once its cleanups have run its count is 0 and its
// state is DONE, so it never answers here again and a later cycle takes it
// like anything else. What a cleanup did in between -- including storing
// what it reached somewhere still live -- is asked about from scratch then.
static bool hold_pending_disposals(Machine *m)
{
    for (LhatObject *object = m->objects.objects; object != NULL;
         object = object->next) {
        if (object->marked || object->kind != LHAT_OBJECT_COROUTINE) {
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
        if (!lhat_gc_reach(&m->gray, lhat_object(object))) {
            return false;
        }
        co->next_pending = m->pending_dispose;
        m->pending_dispose = co;
    }
    return true;
}

// 03 の 1.2 keeps Lua's incremental collector as something to borrow later.
// This is the working form 5.1's order asks for first.
void lhat_gc_collect(Machine *m)
{
    if (!mark_roots(m) || !follow_gray(m)) {
        m->threshold = SIZE_MAX;  // out of memory; stop trying to collect
        return;
    }
    // 02 の 10.7: held back before the sweep, and what they reach followed
    // after -- a coroutine kept for its cleanups keeps its registers too.
    if (!hold_pending_disposals(m) || !follow_gray(m)) {
        m->threshold = SIZE_MAX;
        return;
    }
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
