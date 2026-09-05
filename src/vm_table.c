// L^ (lhat) -- table operations and instance defaults.

#include "vm_internal.h"
#include <string.h>
#include "lhat/port.h"

// 5.12: everything in this file that writes into a table goes through here.
// A table is written over and over, so the barrier it takes is the backward
// one -- the table goes back on the collector's list, to be looked at once
// the writing has settled, rather than every value being marked as it
// arrives. Both the key and the value are stored, so both are asked about.
//
// One place for the barrier to be missing from rather than nine. A table
// made in this same instruction is white and the barrier does nothing, so
// there is no call site that has to know which kind it has.
bool vm_set_key(Machine *m, LhatTable *table, LhatValue key,
                    LhatValue value, bool *refused)
{
    // 02 の 14.15: writing nil^ over a field its definition declared puts
    // the seat back rather than taking the key. The prototype keeps its own
    // seat for ever (sealed, never filled), which is what says the name was
    // declared -- no other ledger exists.
    if (lhat_is_nil(value) && table->definition != NULL) {
        const LhatTable *proto = vm_table_of(lhat_table_get(
            table->definition, lhat_object((LhatObject *)m->self_key)));
        if (proto != NULL && lhat_table_reserved(proto, key)) {
            *refused = false;
            lhat_table_vacate(table, key);
            return true;
        }
    }
    if (!lhat_table_set(table, key, value, refused)) {
        return false;
    }
    lhat_gc_barrier_back(m, (LhatObject *)table, key);
    lhat_gc_barrier_back(m, (LhatObject *)table, value);
    return true;
}

// 02 の 14.11: the leaves that may sit in a definition's prototype as they
// are. Immutable values -- a subroutine's captured places are its own
// affair, an error kind and a type are identities -- which sharing cannot
// betray. A table is not one: vm_bake_default below copies it into the
// prototype's own sealed tree, and a definition among the values stays
// shared on purpose (a public identity, not per-instance data).
static bool immutable_default(LhatValue value)
{
    if (!lhat_is_object(value)) {
        // nil^, bool^, number^. A host value is bytes on the stack and
        // never a table's to hold, but the answer here is no either way.
        return !lhat_is_hostvalue(value) && value.tag != LHAT_VALUE_CONT &&
               value.tag != LHAT_VALUE_RUN;
    }
    switch (lhat_as_object(value)->kind) {
        case LHAT_OBJECT_STRING:
        case LHAT_OBJECT_SUBROUTINE:
        case LHAT_OBJECT_HOST:
        case LHAT_OBJECT_NATIVE:
        case LHAT_OBJECT_OVERLOAD:
        case LHAT_OBJECT_TYPE:
        case LHAT_OBJECT_ERROR_KIND:
        case LHAT_OBJECT_ENUM:
        case LHAT_OBJECT_ENUMERATOR:
            return true;
        default:
            return false;
    }
}

// 02 の 14.11: a field's value on its way onto the prototype (SETPROTO).
// A table that is not a definition becomes the prototype's own: a sealed
// structural copy, so what every instance starts from belongs to the
// definition alone, whatever expression the initialiser was. Leaves pass
// through; a value nothing may share is refused. The depth cap is the
// C-stack guard config.h describes -- a cycle cannot be a literal tree, so
// it lands here and is refused rather than followed.
bool vm_bake_default(Machine *m, LhatValue held, LhatValue *out,
                         size_t depth, bool *refused_value)
{
    // 05 の 8.9: a box is a copyable node of depth nought -- bytes, no
    // references -- so the prototype takes a sealed copy of its own.
    if (lhat_is_object_kind(held, LHAT_OBJECT_HOSTVALUE_BOX)) {
        const LhatHostValueBox *source =
            (const LhatHostValueBox *)lhat_as_object(held);
        const LhatHostValueTag *tag = lhat_hostvalue_box_tag(source);
        LhatHostValueBox *copy = lhat_hostvalue_box_new(&m->objects, tag);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy->run, source->run, tag->width * sizeof(LhatValueUnion));
        copy->sealed = true;
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
        const LhatTable *table = (const LhatTable *)lhat_as_object(held);
        if (table->is_definition) {
            *out = held;
            return true;
        }
        if (depth > LHAT_MAX_PROTOTYPE_DEPTH) {
            *refused_value = true;
            return false;
        }
        LhatTable *copy = lhat_table_new(&m->objects);
        if (copy == NULL) {
            return false;
        }
        bool key_refused = false;
        for (size_t i = 0; i < table->array_count; i++) {
            LhatValue baked = lhat_nil();
            if (!vm_bake_default(m, lhat_slots_get(table->array, i), &baked,
                              depth + 1, refused_value) ||
                !vm_set_key(m, copy, lhat_integer((int64_t)i + 1), baked,
                         &key_refused)) {
                return false;
            }
        }
        for (size_t i = 0; i < table->entry_capacity; i++) {
            const LhatTableEntry *entry = &table->entries[i];
            if (lhat_is_nil(entry->key)) {
                continue;  // free, or a tombstone
            }
            LhatValue baked = lhat_nil();
            if (!vm_bake_default(m, entry->value, &baked, depth + 1,
                              refused_value) ||
                !vm_set_key(m, copy, entry->key, baked, &key_refused)) {
                return false;
            }
        }
        copy->sealed = true;
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    if (!immutable_default(held)) {
        *refused_value = true;
        return false;
    }
    *out = held;
    return true;
}

LhatTable *vm_clone_table(Machine *m, const LhatTable *source,
                              size_t depth, bool *too_deep);

// A field's value on its way into a copy. A table that is not a definition
// is a tree of the instance's own (14.11's literal trees), so it is copied
// too; everything else -- the immutable leaves, and a definition, which is a
// shared identity -- travels as it is.
static bool clone_default(Machine *m, LhatValue held, LhatValue *out,
                          size_t depth, bool *too_deep)
{
    if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
        const LhatTable *table = (const LhatTable *)lhat_as_object(held);
        if (!table->is_definition) {
            LhatTable *copy = vm_clone_table(m, table, depth + 1, too_deep);
            if (copy == NULL) {
                return false;
            }
            *out = lhat_object((LhatObject *)copy);
            return true;
        }
    }
    // 05 の 8.9: a box is copied by its bytes, unsealed -- each instance's
    // own to set().
    if (lhat_is_object_kind(held, LHAT_OBJECT_HOSTVALUE_BOX)) {
        const LhatHostValueBox *source =
            (const LhatHostValueBox *)lhat_as_object(held);
        const LhatHostValueTag *tag = lhat_hostvalue_box_tag(source);
        LhatHostValueBox *copy = lhat_hostvalue_box_new(&m->objects, tag);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy->run, source->run, tag->width * sizeof(LhatValueUnion));
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    *out = held;
    return true;
}

// 02 の 14.11: the copy construction makes -- the table's own two halves,
// value by value, a held table copied as its own tree. The link and the seal
// are the caller's to set; a fresh table has neither. The baking (SETPROTO)
// already refused a cycle, so the depth cap only keeps the recursion off the
// C stack's limits for a table that never went through it. NULL when out of
// memory, or with *too_deep set when the cap refused it.
LhatTable *vm_clone_table(Machine *m, const LhatTable *source,
                              size_t depth, bool *too_deep)
{
    if (depth > LHAT_MAX_PROTOTYPE_DEPTH) {
        *too_deep = true;
        return NULL;
    }
    LhatTable *clone = lhat_table_new(&m->objects);
    if (clone == NULL) {
        return NULL;
    }
    bool refused = false;
    for (size_t i = 0; i < source->array_count; i++) {
        LhatValue held = lhat_nil();
        if (!clone_default(m, lhat_slots_get(source->array, i), &held, depth,
                           too_deep) ||
            !vm_set_key(m, clone, lhat_integer((int64_t)i + 1), held, &refused)) {
            return NULL;
        }
    }
    for (size_t i = 0; i < source->entry_capacity; i++) {
        const LhatTableEntry *entry = &source->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;  // free, or a tombstone
        }
        // 02 の 14.15: a seat travels as the seat it is -- vm_set_key would
        // read its nil as a removal, and the clone is born with the very
        // keys the prototype holds.
        if (lhat_is_nil(entry->value)) {
            if (!lhat_table_reserve(clone, entry->key)) {
                return NULL;
            }
            lhat_gc_barrier_back(m, (LhatObject *)clone, entry->key);
            continue;
        }
        LhatValue held = lhat_nil();
        if (!clone_default(m, entry->value, &held, depth, too_deep) ||
            !vm_set_key(m, clone, entry->key, held, &refused)) {
            return NULL;
        }
    }
    // 03 の 5.1改: the building above counted as layout changes, but the
    // clone is only now born -- no cache can remember a table that did not
    // exist. Version zero is what lets a from_definition answer stand for
    // an instance that only ever filled its declared seats.
    clone->version = 0;
    return clone;
}

// 02 の 11.2改: '..' between two plain tables concatenates into a new table.
// The sequence halves go in order -- the left's positions and then the
// right's, renumbered after them -- and the named keys come from both sides.
// The copy is shallow: the elements are shared, only the holder is new.
//
// A key both sides carry answers NULL through *collided (14.5改's rule read
// for values: neither side was written against the other, so neither is the
// answer -- and unlike a composed definition's method there is no qualified
// spelling left to reach one through, so it is refused outright).
LhatTable *vm_concat_tables(Machine *m, const LhatTable *left,
                                const LhatTable *right, bool *collided)
{
    *collided = false;
    LhatTable *joined = lhat_table_new(&m->objects);
    if (joined == NULL) {
        return NULL;
    }
    bool refused = false;
    size_t position = 0;
    const LhatTable *sides[2] = { left, right };
    for (size_t s = 0; s < 2; s++) {
        for (size_t i = 0; i < sides[s]->array_count; i++) {
            if (!vm_set_key(m, joined, lhat_integer((int64_t)++position),
                         lhat_slots_get(sides[s]->array, i), &refused)) {
                return NULL;
            }
        }
    }
    for (size_t s = 0; s < 2; s++) {
        for (size_t i = 0; i < sides[s]->entry_capacity; i++) {
            const LhatTableEntry *entry = &sides[s]->entries[i];
            if (lhat_is_nil(entry->key)) {
                continue;  // free, or a tombstone
            }
            // An integer key past the dense part stays what it was: the two
            // sequences were renumbered above, and a sparse index was never
            // part of either sequence, so it is carried as a named key is.
            // Asked of both sides, not just the right: a sparse index may
            // land where the renumbering put someone's position, and that
            // is a collision like any other.
            if (!lhat_is_nil(lhat_table_get(joined, entry->key))) {
                *collided = true;
                return NULL;
            }
            if (!vm_set_key(m, joined, entry->key, entry->value, &refused)) {
                return NULL;
            }
        }
    }
    return joined;
}

// ---------------------------------------------------------------------------
// 02 の 14.22: the table's own operations
// ---------------------------------------------------------------------------

// One comparison, three-way. `cmp` is the written comparator or nil^ for the
// built-in ordering -- 11.9's own, numbers and strings.
static LhatRunStatus sort_compare(Machine *m, LhatValue cmp, LhatValue x,
                                  LhatValue y, int *out)
{
    if (lhat_is_nil(cmp)) {
        return vm_three_way(x, y, out) ? LHAT_RUN_OK : LHAT_RUN_TYPE_ERROR;
    }
    LhatValue pair[2];
    pair[0] = x;
    pair[1] = y;
    // Nested on purpose -- vm.h says a call may be made inside a running
    // machine, and the comparator is arbitrary L^ code.
    LhatRunResult ran = lhat_machine_call(m, cmp, pair, 2);
    if (ran.status != LHAT_RUN_OK) {
        return ran.status;
    }
    if (!lhat_is_number(ran.value)) {
        return LHAT_RUN_TYPE_ERROR;
    }
    double answer = lhat_number_as_real(ran.value);
    *out = answer < 0 ? -1 : answer > 0 ? 1 : 0;
    return LHAT_RUN_OK;
}

// Merge sort, bottom up, for both spellings -- stable, which sort^ simply
// does not promise. Every pass reads the whole of `from` and writes the
// whole of `into`, so at any moment every element is in one rooted table:
// t is a register's, and the aux rides m->native_hold while the comparator
// (which may allocate and collect) runs. A comparator that faults leaves
// the order unspecified, but every element still present.
static LhatRunStatus table_sort(Machine *m, LhatTable *t, LhatValue cmp)
{
    size_t count = t->array_count;
    if (count < 2) {
        return LHAT_RUN_OK;
    }
    LhatTable *aux = lhat_table_new(&m->objects);
    if (aux == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    bool refused = false;
    for (size_t i = 0; i < count; i++) {
        if (!vm_set_key(m, aux, lhat_integer((int64_t)i + 1),
                     lhat_slots_get(t->array, i), &refused)) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    LhatNativeHold hold;
    hold.held = lhat_object((LhatObject *)aux);
    hold.outer = m->native_hold;
    m->native_hold = &hold;
    LhatTable *from = aux;
    LhatTable *into = t;
    LhatRunStatus status = LHAT_RUN_OK;
    for (size_t width = 1; width < count && status == LHAT_RUN_OK;
         width *= 2) {
        for (size_t lo = 0; lo < count && status == LHAT_RUN_OK;
             lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo;
            size_t j = mid;
            for (size_t k = lo; k < hi; k++) {
                bool take_left;
                if (i >= mid) {
                    take_left = false;
                } else if (j >= hi) {
                    take_left = true;
                } else {
                    int outcome = 0;
                    status = sort_compare(m, cmp,
                                          lhat_slots_get(from->array, i),
                                          lhat_slots_get(from->array, j),
                                          &outcome);
                    if (status != LHAT_RUN_OK) {
                        break;
                    }
                    take_left = outcome <= 0;  // equals keep their order
                }
                LhatValue moved =
                    lhat_slots_get(from->array, take_left ? i++ : j++);
                lhat_slots_set(into->array, k, moved);
                lhat_gc_barrier_back(m, (LhatObject *)into, moved);
            }
        }
        LhatTable *turned = from;
        from = into;
        into = turned;
    }
    // The passes end with the ordered run in `from`.
    if (status == LHAT_RUN_OK && from != t) {
        for (size_t i = 0; i < count; i++) {
            LhatValue moved = lhat_slots_get(from->array, i);
            lhat_slots_set(t->array, i, moved);
            lhat_gc_barrier_back(m, (LhatObject *)t, moved);
        }
    }
    m->native_hold = hold.outer;
    return status;
}

// One string out of the sequence half. Lua's line: a string is itself, a
// number is written the way tostring writes it, anything else is refused.
static LhatRunStatus table_join(Machine *m, const LhatTable *t,
                                const char *sep, size_t sep_length,
                                LhatValue *answer)
{
    size_t count = t->array_count;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
            total += ((const LhatString *)lhat_as_object(held))->length;
        } else if (lhat_is_number(held)) {
            total += lhat_value_text(held, NULL, 0);
        } else {
            return LHAT_RUN_TYPE_ERROR;
        }
        if (i + 1 < count) {
            total += sep_length;
        }
    }
    char *text = (char *)lhat_alloc(total + 1);
    if (text == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
            const LhatString *piece =
                (const LhatString *)lhat_as_object(held);
            memcpy(text + at, piece->text, piece->length);
            at += piece->length;
        } else {
            at += lhat_value_text(held, text + at, total + 1 - at);
        }
        if (i + 1 < count && sep_length > 0) {
            memcpy(text + at, sep, sep_length);
            at += sep_length;
        }
    }
    LhatString *joined = lhat_string_new(&m->objects, text, at);
    lhat_free(text);
    if (joined == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    *answer = lhat_object((LhatObject *)joined);
    return LHAT_RUN_OK;
}

// The block copy Lua's table.move makes: dst[to .. to+(last-from)] =
// src[from .. last], through get/set so a sparse landing works, backwards
// when the ranges overlap that way round. The caller checked the bounds.
static LhatRunStatus table_blockmove(Machine *m, LhatTable *dst,
                                     const LhatTable *src, int64_t from,
                                     int64_t last, int64_t to)
{
    if (last < from) {
        return LHAT_RUN_OK;  // an empty block moves nothing
    }
    bool refused = false;
    int64_t span = last - from;
    if (dst == src && to > from) {
        for (int64_t k = span; k >= 0; k--) {
            if (!vm_set_key(m, dst, lhat_integer(to + k),
                         lhat_table_get(src, lhat_integer(from + k)),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
        }
    } else {
        for (int64_t k = 0; k <= span; k++) {
            if (!vm_set_key(m, dst, lhat_integer(to + k),
                         lhat_table_get(src, lhat_integer(from + k)),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
        }
    }
    return LHAT_RUN_OK;
}
// 14.22: one value through the clone's written policy -- arbitrary L^ code,
// nested the way a sort's comparator is.
static LhatRunStatus clone_policy(Machine *m, LhatValue policy,
                                  LhatValue held, LhatValue *out)
{
    LhatRunResult ran = lhat_machine_call(m, policy, &held, 1);
    if (ran.status != LHAT_RUN_OK) {
        return ran.status;
    }
    *out = ran.value;
    return LHAT_RUN_OK;
}

// 14.22: the shallow copy, or each value through the policy. The copy under
// construction rides m->native_hold while the policy runs, the way a sort's
// aux table does -- the policy may allocate and collect. The keys are
// carried as they are; only the values are asked about.
static LhatRunStatus table_clone(Machine *m, const LhatTable *t,
                                 LhatValue policy, LhatValue *answer)
{
    LhatTable *copy = lhat_table_new(&m->objects);
    if (copy == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    LhatNativeHold hold;
    hold.held = lhat_object((LhatObject *)copy);
    hold.outer = m->native_hold;
    m->native_hold = &hold;
    LhatRunStatus status = LHAT_RUN_OK;
    bool refused = false;
    for (size_t i = 0; i < t->array_count && status == LHAT_RUN_OK; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (!lhat_is_nil(policy)) {
            status = clone_policy(m, policy, held, &held);
        }
        if (status == LHAT_RUN_OK &&
            !vm_set_key(m, copy, lhat_integer((int64_t)i + 1), held, &refused)) {
            status = LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    for (size_t i = 0; i < t->entry_capacity && status == LHAT_RUN_OK; i++) {
        const LhatTableEntry *entry = &t->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;  // free, or a tombstone
        }
        LhatValue held = entry->value;
        if (!lhat_is_nil(policy)) {
            status = clone_policy(m, policy, held, &held);
        }
        if (status == LHAT_RUN_OK &&
            !vm_set_key(m, copy, entry->key, held, &refused)) {
            status = LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    m->native_hold = hold.outer;
    if (status == LHAT_RUN_OK) {
        *answer = lhat_object((LhatObject *)copy);
    }
    return status;
}

// The one door for all of 14.22. `args` were copied out of the caller's
// window, so a nested comparator call cannot disturb them; what lands over
// the call goes through *answer.
LhatRunStatus vm_table_native(Machine *m, const LhatNative *native,
                                  const LhatValue *args, size_t count,
                                  LhatValue *answer)
{
    LhatTable *t = (LhatTable *)lhat_as_object(native->bound);
    size_t n = t->array_count;
    bool refused = false;
    // The mutating half answer the receiver, for chaining; the readers
    // write their own answer over this.
    *answer = native->bound;

    // 05 の 8.6: the machine's own tables are read-only, natives included.
    bool mutates = native->kind == LHAT_NATIVE_INSERT ||
                   native->kind == LHAT_NATIVE_PUSH ||
                   native->kind == LHAT_NATIVE_EXTEND ||
                   native->kind == LHAT_NATIVE_REMOVE ||
                   native->kind == LHAT_NATIVE_POP ||
                   native->kind == LHAT_NATIVE_SORT ||
                   native->kind == LHAT_NATIVE_STABLESORT ||
                   native->kind == LHAT_NATIVE_MOVE ||
                   native->kind == LHAT_NATIVE_REVERSE ||
                   native->kind == LHAT_NATIVE_CLEAR;
    if (mutates && t->sealed) {
        return LHAT_RUN_SEALED;
    }

    switch (native->kind) {
        case LHAT_NATIVE_JOIN: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            const char *sep = "";
            size_t sep_length = 0;
            if (count == 1) {
                if (!lhat_is_object_kind(args[0], LHAT_OBJECT_STRING)) {
                    return LHAT_RUN_TYPE_ERROR;
                }
                const LhatString *written =
                    (const LhatString *)lhat_as_object(args[0]);
                sep = written->text;
                sep_length = written->length;
            }
            return table_join(m, t, sep, sep_length, answer);
        }

        case LHAT_NATIVE_INDEXOF:
        case LHAT_NATIVE_CONTAINS: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            bool asking = native->kind == LHAT_NATIVE_CONTAINS;
            *answer = asking ? lhat_bool(false) : lhat_nil();
            for (size_t i = 0; i < n; i++) {
                if (lhat_value_equal(args[0], lhat_slots_get(t->array, i))) {
                    *answer = asking ? lhat_bool(true)
                                     : lhat_integer((int64_t)i + 1);
                    break;
                }
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_SLICE: {
            if (count < 1 || count > 2) {
                return LHAT_RUN_ARITY;
            }
            int64_t from = 0;
            int64_t to = 0;
            if (!vm_ordinal_of(args[0], &from) ||
                (count == 2 && !vm_ordinal_of(args[1], &to))) {
                return LHAT_RUN_TYPE_ERROR;
            }
            // 14.19's reading: one ordinal runs to the end, a negative one
            // counts from it, and a range that does not stand answers empty.
            int64_t start = vm_resolve_ordinal(from, n);
            int64_t end = vm_resolve_ordinal(count == 2 ? to : (int64_t)n, n);
            LhatTable *cut = lhat_table_new(&m->objects);
            if (cut == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            if (start >= 1 && start <= end && end <= (int64_t)n) {
                for (int64_t k = start; k <= end; k++) {
                    if (!vm_set_key(m, cut, lhat_integer(k - start + 1),
                                 lhat_slots_get(t->array, (size_t)k - 1),
                                 &refused)) {
                        return LHAT_RUN_OUT_OF_MEMORY;
                    }
                }
            }
            *answer = lhat_object((LhatObject *)cut);
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_INSERT: {
            if (count != 2) {
                return LHAT_RUN_ARITY;
            }
            int64_t at_pos = 0;
            if (!vm_ordinal_of(args[0], &at_pos)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            if (at_pos < 1 || at_pos > (int64_t)n + 1) {
                return LHAT_RUN_BAD_KEY;
            }
            for (int64_t k = (int64_t)n; k >= at_pos; k--) {
                if (!vm_set_key(m, t, lhat_integer(k + 1),
                             lhat_slots_get(t->array, (size_t)k - 1),
                             &refused)) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
            }
            if (!vm_set_key(m, t, lhat_integer(at_pos), args[1], &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_PUSH: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            if (!vm_set_key(m, t, lhat_integer((int64_t)n + 1), args[0],
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_EXTEND: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            if (!lhat_is_object_kind(args[0], LHAT_OBJECT_TABLE)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            const LhatTable *more =
                (const LhatTable *)lhat_as_object(args[0]);
            // The count is read once, so extending a table with itself
            // appends what it held when the call was made.
            size_t held = more->array_count;
            for (size_t i = 0; i < held; i++) {
                if (!vm_set_key(m, t, lhat_integer((int64_t)(n + i) + 1),
                             lhat_slots_get(more->array, i), &refused)) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_REMOVE:
        case LHAT_NATIVE_POP: {
            int64_t at_pos = (int64_t)n;
            if (native->kind == LHAT_NATIVE_REMOVE) {
                if (count != 1) {
                    return LHAT_RUN_ARITY;
                }
                if (!vm_ordinal_of(args[0], &at_pos)) {
                    return LHAT_RUN_TYPE_ERROR;
                }
                at_pos = vm_resolve_ordinal(at_pos, n);
            } else if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            // 04 の 11.3's line: what is not there is not an error. An empty
            // table's pop and an out-of-range remove both answer nil^.
            if (at_pos < 1 || at_pos > (int64_t)n) {
                *answer = lhat_nil();
                return LHAT_RUN_OK;
            }
            *answer = lhat_slots_get(t->array, (size_t)at_pos - 1);
            for (int64_t k = at_pos; k < (int64_t)n; k++) {
                lhat_slots_set(t->array, (size_t)k - 1,
                               lhat_slots_get(t->array, (size_t)k));
            }
            if (!vm_set_key(m, t, lhat_integer((int64_t)n), lhat_nil(),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_CLONE: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            return table_clone(m, t, count == 1 ? args[0] : lhat_nil(),
                               answer);
        }

        case LHAT_NATIVE_SORT:
        case LHAT_NATIVE_STABLESORT: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            return table_sort(m, t, count == 1 ? args[0] : lhat_nil());
        }

        case LHAT_NATIVE_MOVE: {
            bool cross = count >= 1 &&
                         lhat_is_object_kind(args[0], LHAT_OBJECT_TABLE);
            const LhatTable *source =
                cross ? (const LhatTable *)lhat_as_object(args[0]) : t;
            size_t shift = cross ? 1 : 0;
            if (count - shift < 2 || count - shift > 3) {
                return LHAT_RUN_ARITY;
            }
            int64_t from = 0;
            int64_t last = 0;
            int64_t to = 0;
            bool block = count - shift == 3;
            if (!vm_ordinal_of(args[shift], &from) ||
                (block && !vm_ordinal_of(args[shift + 1], &last)) ||
                !vm_ordinal_of(args[count - 1], &to)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            if (!block) {
                last = from;
            }
            if (!cross && !block) {
                // 14.22: the two-ordinal form of self relocates one element,
                // the others shifting to close and open the gap.
                if (from < 1 || from > (int64_t)n || to < 1 ||
                    to > (int64_t)n) {
                    return LHAT_RUN_BAD_KEY;
                }
                LhatValue moved =
                    lhat_slots_get(t->array, (size_t)from - 1);
                if (from < to) {
                    for (int64_t k = from; k < to; k++) {
                        lhat_slots_set(t->array, (size_t)k - 1,
                                       lhat_slots_get(t->array, (size_t)k));
                    }
                } else {
                    for (int64_t k = from; k > to; k--) {
                        lhat_slots_set(
                            t->array, (size_t)k - 1,
                            lhat_slots_get(t->array, (size_t)k - 2));
                    }
                }
                lhat_slots_set(t->array, (size_t)to - 1, moved);
                return LHAT_RUN_OK;
            }
            if (last >= from &&
                (from < 1 || last > (int64_t)source->array_count || to < 1)) {
                return LHAT_RUN_BAD_KEY;
            }
            return table_blockmove(m, t, source, from, last, to);
        }

        case LHAT_NATIVE_REVERSE: {
            if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            for (size_t i = 0; i * 2 + 1 < n; i++) {
                LhatValue held = lhat_slots_get(t->array, i);
                lhat_slots_set(t->array, i,
                               lhat_slots_get(t->array, n - 1 - i));
                lhat_slots_set(t->array, n - 1 - i, held);
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_CLEAR: {
            if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            for (size_t i = 0; i < t->array_count; i++) {
                lhat_slots_set(t->array, i, lhat_nil());
            }
            t->array_count = 0;
            for (size_t i = 0; i < t->entry_capacity; i++) {
                t->entries[i].key = lhat_nil();
                t->entries[i].value = lhat_nil();
            }
            t->entry_count = 0;
            return LHAT_RUN_OK;
        }

        default:
            return LHAT_RUN_TYPE_ERROR;  // never reached; the range was asked
    }
}
