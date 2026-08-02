// L^ (lhat) -- the heap values: strings and tables.

#include "object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

static void *allocate(LhatHeap *heap, size_t size, LhatObjectKind kind)
{
    LhatObject *object = (LhatObject *)calloc(1, size);
    if (object == NULL) {
        return NULL;
    }
    object->kind = kind;
    object->next = heap->objects;
    heap->objects = object;
    heap->count++;
    return object;
}

uint32_t lhat_string_hash(const char *text, size_t length)
{
    // FNV-1a. Cheap, and good enough for keys that are mostly member names.
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

LhatString *lhat_string_new(LhatHeap *heap, const char *text, size_t length)
{
    LhatString *string =
        (LhatString *)allocate(heap, sizeof *string + length + 1,
                               LHAT_OBJECT_STRING);
    if (string == NULL) {
        return NULL;
    }
    if (length > 0) {
        memcpy(string->text, text, length);
    }
    string->text[length] = '\0';
    string->length = length;
    string->hash = lhat_string_hash(text, length);
    return string;
}

LhatString *lhat_string_concat(LhatHeap *heap, const LhatString *left,
                               const LhatString *right)
{
    size_t length = left->length + right->length;
    LhatString *joined =
        (LhatString *)allocate(heap, sizeof *joined + length + 1,
                               LHAT_OBJECT_STRING);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined->text, left->text, left->length);
    memcpy(joined->text + left->length, right->text, right->length);
    joined->text[length] = '\0';
    joined->length = length;
    joined->hash = lhat_string_hash(joined->text, length);
    return joined;
}

LhatTable *lhat_table_new(LhatHeap *heap)
{
    return (LhatTable *)allocate(heap, sizeof(LhatTable), LHAT_OBJECT_TABLE);
}

LhatErrorKind *lhat_error_kind_new(LhatHeap *heap,
                                   const LhatErrorKind *group,
                                   const LhatString *name)
{
    LhatErrorKind *kind = (LhatErrorKind *)allocate(heap, sizeof(LhatErrorKind),
                                                    LHAT_OBJECT_ERROR_KIND);
    if (kind == NULL) {
        return NULL;
    }
    kind->group = group;
    kind->name = name;
    return kind;
}

LhatError *lhat_error_new(LhatHeap *heap, const LhatErrorKind *kind)
{
    LhatTable *fields = lhat_table_new(heap);
    if (fields == NULL) {
        return NULL;
    }
    LhatError *error =
        (LhatError *)allocate(heap, sizeof(LhatError), LHAT_OBJECT_ERROR);
    if (error == NULL) {
        return NULL;
    }
    error->kind = kind;
    error->fields = fields;
    return error;
}

LhatCoroutine *lhat_coroutine_new(LhatHeap *heap, const LhatClosure *closure,
                                  size_t registers)
{
    LhatCoroutine *coroutine =
        (LhatCoroutine *)allocate(heap, sizeof(LhatCoroutine),
                                  LHAT_OBJECT_COROUTINE);
    if (coroutine == NULL) {
        return NULL;
    }
    coroutine->registers =
        (LhatValue *)calloc(registers ? registers : 1, sizeof *coroutine->registers);
    if (coroutine->registers == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < registers; i++) {
        coroutine->registers[i] = lhat_nil();
    }
    coroutine->closure = closure;
    coroutine->state = LHAT_COROUTINE_FRESH;
    coroutine->register_count = registers;
    return coroutine;
}

LhatCoroutine *lhat_table_iterator(LhatHeap *heap, const LhatTable *table)
{
    LhatCoroutine *walk =
        (LhatCoroutine *)allocate(heap, sizeof(LhatCoroutine),
                                  LHAT_OBJECT_COROUTINE);
    if (walk == NULL) {
        return NULL;
    }
    walk->state = LHAT_COROUTINE_FRESH;
    walk->source = LHAT_COROUTINE_TABLE;
    walk->walking = table;
    return walk;
}

bool lhat_table_walk(LhatCoroutine *walk, LhatValue *key, LhatValue *value)
{
    const LhatTable *table = walk->walking;
    if (table == NULL) {
        return false;
    }
    // The dense part first, in index order: 14 章 makes a table both a
    // sequence and a mapping, and the sequence half has an order worth
    // keeping. What order the rest comes in is not promised.
    if (walk->at_array < table->array_count) {
        *key = lhat_integer((int64_t)walk->at_array + 1);
        *value = table->array[walk->at_array++];
        return true;
    }
    while (walk->at_entry < table->entry_capacity) {
        const LhatTableEntry *entry = &table->entries[walk->at_entry++];
        if (!lhat_is_nil(entry->key)) {
            *key = entry->key;
            *value = entry->value;
            return true;
        }
    }
    return false;
}

LhatNative *lhat_native_new(LhatHeap *heap, LhatNativeKind kind,
                            LhatValue bound)
{
    LhatNative *native =
        (LhatNative *)allocate(heap, sizeof(LhatNative), LHAT_OBJECT_NATIVE);
    if (native == NULL) {
        return NULL;
    }
    native->kind = kind;
    native->bound = bound;
    return native;
}

LhatHost *lhat_host_new(LhatHeap *heap, LhatHostFn call, void *context,
                        uint8_t parameters, bool takes_self)
{
    LhatHost *host =
        (LhatHost *)allocate(heap, sizeof(LhatHost), LHAT_OBJECT_HOST);
    if (host == NULL) {
        return NULL;
    }
    host->call = call;
    host->context = context;
    host->bound = lhat_nil();
    host->parameters = parameters;
    host->takes_self = takes_self;
    return host;
}

LhatRuntimeType *lhat_type_rt_new(LhatHeap *heap, LhatRuntimeTypeKind kind)
{
    LhatRuntimeType *type =
        (LhatRuntimeType *)allocate(heap, sizeof(LhatRuntimeType),
                                    LHAT_OBJECT_TYPE);
    if (type != NULL) {
        type->kind = kind;
    }
    return type;
}

bool lhat_type_rt_add_part(LhatRuntimeType *type, LhatRuntimeType *part)
{
    LhatRuntimeType **grown =
        (LhatRuntimeType **)realloc(type->parts,
                                    (type->part_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    type->parts = grown;
    type->parts[type->part_count++] = part;
    return true;
}

bool lhat_type_rt_add_member(LhatRuntimeType *type, const LhatString *name,
                             LhatRuntimeType *member)
{
    void *grown = realloc(type->members,
                          (type->member_count + 1) * sizeof *type->members);
    if (grown == NULL) {
        return false;
    }
    type->members = grown;
    type->members[type->member_count].name = name;
    type->members[type->member_count].type = member;
    type->member_count++;
    return true;
}

bool lhat_value_satisfies(LhatValue value, const LhatRuntimeType *type)
{
    if (type == NULL) {
        return true;  // nothing was written, so nothing is asked
    }
    switch (type->kind) {
        case LHAT_TYPE_RT_ANY:
            return true;
        case LHAT_TYPE_RT_NIL:
            return lhat_is_nil(value);
        case LHAT_TYPE_RT_BOOL:
            return lhat_is_bool(value);
        case LHAT_TYPE_RT_NUMBER:
            // 14.8: one type, so either representation answers yes.
            return lhat_is_number(value);
        case LHAT_TYPE_RT_STRING:
            return lhat_is_object_kind(value, LHAT_OBJECT_STRING);
        case LHAT_TYPE_RT_TABLE:
            return lhat_is_object_kind(value, LHAT_OBJECT_TABLE);
        case LHAT_TYPE_RT_SUBROUTINE:
            return lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE);
        case LHAT_TYPE_RT_COROUTINE:
            return lhat_is_object_kind(value, LHAT_OBJECT_COROUTINE);
        case LHAT_TYPE_RT_ERROR:
            return lhat_is_object_kind(value, LHAT_OBJECT_ERROR);
        case LHAT_TYPE_RT_ERROR_KIND:
            return lhat_error_is_kind(value, type->error_kind);
        case LHAT_TYPE_RT_UNION:
            for (size_t i = 0; i < type->part_count; i++) {
                if (lhat_value_satisfies(value, type->parts[i])) {
                    return true;
                }
            }
            return false;
        case LHAT_TYPE_RT_STRUCTURE: {
            // 14.10: at least these members, which is what makes the judgement
            // structural rather than a question about where it came from.
            const LhatTable *table = NULL;
            if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
                table = (const LhatTable *)lhat_as_object(value);
            } else if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
                table = ((const LhatError *)lhat_as_object(value))->fields;
            } else {
                return false;
            }
            for (size_t i = 0; i < type->member_count; i++) {
                LhatValue held = lhat_table_get(
                    table, lhat_object((LhatObject *)(void *)
                                       type->members[i].name));
                if (lhat_is_nil(held) ||
                    !lhat_value_satisfies(held, type->members[i].type)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

LhatOverload *lhat_overload_new(LhatHeap *heap)
{
    return (LhatOverload *)allocate(heap, sizeof(LhatOverload),
                                    LHAT_OBJECT_OVERLOAD);
}

bool lhat_overload_add(LhatOverload *overload, LhatValue candidate)
{
    if (overload->count == overload->capacity) {
        size_t grown = overload->capacity ? overload->capacity * 2 : 4;
        LhatValue *bigger =
            (LhatValue *)realloc(overload->candidates, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        overload->candidates = bigger;
        overload->capacity = grown;
    }
    overload->candidates[overload->count++] = candidate;
    return true;
}

bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_ERROR) || kind == NULL) {
        return false;
    }
    const LhatErrorKind *held = ((const LhatError *)lhat_as_object(value))->kind;
    if (held == NULL) {
        return false;
    }
    // 2.3: a declaration is the union of its kinds, so naming it asks whether
    // the error is any of them.
    return kind->group == NULL ? held->group == kind : held == kind;
}

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

void lhat_gray_dispose(LhatGray *gray)
{
    free(gray->items);
    gray->items = NULL;
    gray->count = 0;
    gray->capacity = 0;
}

static bool gray_push(LhatGray *gray, LhatObject *object)
{
    if (gray->count == gray->capacity) {
        size_t grown = gray->capacity ? gray->capacity * 2 : 64;
        LhatObject **bigger =
            (LhatObject **)realloc(gray->items, grown * sizeof *bigger);
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

size_t lhat_gc_sweep(LhatHeap *heap)
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
        lhat_object_free(object);
        heap->count--;
        freed++;
    }
    return freed;
}

void lhat_object_free(LhatObject *object)
{
    if (object == NULL) {
        return;
    }
    switch (object->kind) {
        case LHAT_OBJECT_TABLE: {
            LhatTable *table = (LhatTable *)object;
            free(table->array);
            free(table->entries);
            break;
        }
        case LHAT_OBJECT_SUBROUTINE:
            free(((LhatClosure *)object)->upvalues);
            break;
        case LHAT_OBJECT_COROUTINE:
            free(((LhatCoroutine *)object)->registers);
            break;
        case LHAT_OBJECT_TYPE:
            free(((LhatRuntimeType *)object)->parts);
            free(((LhatRuntimeType *)object)->members);
            break;
        case LHAT_OBJECT_OVERLOAD:
            free(((LhatOverload *)object)->candidates);
            break;
        default:
            break;
    }
    free(object);
}

void lhat_object_free_all(LhatHeap *heap)
{
    LhatObject *object = heap->objects;
    while (object != NULL) {
        LhatObject *next = object->next;
        lhat_object_free(object);
        object = next;
    }
    heap->objects = NULL;
    heap->count = 0;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

bool lhat_string_equal(const LhatString *a, const LhatString *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL || a->length != b->length ||
        a->hash != b->hash) {
        return false;
    }
    return memcmp(a->text, b->text, a->length) == 0;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

// 02 の 14.8 makes number^ one type with two representations, so 2 and 2.0
// have to be the same key. The real form folds into the integer one whenever
// it names the same number.
static LhatValue normalise_key(LhatValue key)
{
    if (!lhat_is_real(key)) {
        return key;
    }
    double d = lhat_as_real(key);
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
        d == (double)(int64_t)d) {
        return lhat_integer((int64_t)d);
    }
    return key;
}

// A key has to answer the same hash every time it is asked, which a NaN
// cannot: it is not equal to itself. nil^ is refused because 11.3 uses it for
// "not there".
static bool usable_key(LhatValue key)
{
    if (lhat_is_nil(key)) {
        return false;
    }
    if (lhat_is_real(key) && lhat_as_real(key) != lhat_as_real(key)) {
        return false;
    }
    return true;
}

static uint32_t hash_key(LhatValue key)
{
    switch (key.tag) {
        case LHAT_VALUE_BOOL:
            return lhat_as_bool(key) ? 1u : 0u;
        case LHAT_VALUE_INTEGER: {
            uint64_t bits = (uint64_t)lhat_as_integer(key);
            return (uint32_t)(bits ^ (bits >> 32));
        }
        case LHAT_VALUE_REAL: {
            double d = lhat_as_real(key);
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            return (uint32_t)(bits ^ (bits >> 32));
        }
        case LHAT_VALUE_OBJECT: {
            const LhatObject *object = lhat_as_object(key);
            if (object != NULL && object->kind == LHAT_OBJECT_STRING) {
                return ((const LhatString *)object)->hash;
            }
            uintptr_t address = (uintptr_t)object;
            return (uint32_t)(address ^ (address >> 32));
        }
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// The hash part
// ---------------------------------------------------------------------------

// An entry is free when both halves are nil^, and a tombstone when the key is
// nil^ but the value is not. A tombstone keeps a probe running past a removed
// key that something after it collided with.
static bool entry_is_free(const LhatTableEntry *entry)
{
    return lhat_is_nil(entry->key) && lhat_is_nil(entry->value);
}

static bool entry_is_tombstone(const LhatTableEntry *entry)
{
    return lhat_is_nil(entry->key) && !lhat_is_nil(entry->value);
}

// The place `key` belongs in `entries`: the entry holding it, or the first
// place it could be put. Never returns NULL when capacity is non-zero, since
// the load factor keeps a free entry available.
static LhatTableEntry *probe(LhatTableEntry *entries, size_t capacity,
                             LhatValue key, uint32_t hash)
{
    size_t index = (size_t)hash & (capacity - 1);
    LhatTableEntry *tombstone = NULL;
    for (;;) {
        LhatTableEntry *entry = &entries[index];
        if (entry_is_free(entry)) {
            return tombstone != NULL ? tombstone : entry;
        }
        if (entry_is_tombstone(entry)) {
            if (tombstone == NULL) {
                tombstone = entry;
            }
        } else if (lhat_value_equal(entry->key, key)) {
            return entry;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static bool grow_entries(LhatTable *table)
{
    size_t capacity = table->entry_capacity ? table->entry_capacity * 2 : 8;
    LhatTableEntry *entries =
        (LhatTableEntry *)calloc(capacity, sizeof *entries);
    if (entries == NULL) {
        return false;
    }
    // calloc gives zeroed bytes, which is not necessarily a nil^ value.
    for (size_t i = 0; i < capacity; i++) {
        entries[i].key = lhat_nil();
        entries[i].value = lhat_nil();
    }

    size_t moved = 0;
    for (size_t i = 0; i < table->entry_capacity; i++) {
        LhatTableEntry *old = &table->entries[i];
        if (lhat_is_nil(old->key)) {
            continue;  // free or a tombstone; neither survives a rehash
        }
        LhatTableEntry *entry =
            probe(entries, capacity, old->key, hash_key(old->key));
        *entry = *old;
        moved++;
    }

    free(table->entries);
    table->entries = entries;
    table->entry_capacity = capacity;
    table->entry_count = moved;
    return true;
}

// ---------------------------------------------------------------------------
// The array part
// ---------------------------------------------------------------------------

static bool array_index(const LhatTable *table, LhatValue key, size_t *index)
{
    if (!lhat_is_integer(key)) {
        return false;
    }
    int64_t i = lhat_as_integer(key);
    if (i < 1 || (uint64_t)i > table->array_count) {
        return false;
    }
    *index = (size_t)i - 1;
    return true;
}

static bool grow_array(LhatTable *table)
{
    size_t capacity = table->array_capacity ? table->array_capacity * 2 : 8;
    LhatValue *array = (LhatValue *)realloc(table->array, capacity * sizeof *array);
    if (array == NULL) {
        return false;
    }
    table->array = array;
    table->array_capacity = capacity;
    return true;
}

// Once key n has been appended, n+1 may be waiting in the hash part from
// before the array reached it. Without this a table filled in from the end
// would keep everything in the hash part for ever.
static void drain_into_array(LhatTable *table)
{
    while (table->entry_capacity > 0) {
        LhatValue key = lhat_integer((int64_t)table->array_count + 1);
        LhatTableEntry *entry =
            probe(table->entries, table->entry_capacity, key, hash_key(key));
        if (lhat_is_nil(entry->key)) {
            return;
        }
        if (table->array_count == table->array_capacity && !grow_array(table)) {
            return;
        }
        table->array[table->array_count++] = entry->value;
        entry->key = lhat_nil();
        entry->value = lhat_bool(true);  // a tombstone
        table->entry_count--;
    }
}

// ---------------------------------------------------------------------------
// Reading and writing
// ---------------------------------------------------------------------------

LhatValue lhat_table_get(const LhatTable *table, LhatValue key)
{
    if (!usable_key(key)) {
        return lhat_nil();
    }
    key = normalise_key(key);

    // 14.7: an instance's own fields first, then the members its definition
    // holds. 14.2 fixes the chain when the instance is made, so this walk is
    // the one Lua's __index performs -- but written into the structure rather
    // than left to a hook that can be swapped (14.1).
    for (; table != NULL; table = table->definition) {
        size_t index;
        if (array_index(table, key, &index)) {
            return table->array[index];
        }
        if (table->entry_capacity == 0) {
            continue;
        }
        LhatTableEntry *entry =
            probe(table->entries, table->entry_capacity, key, hash_key(key));
        if (!lhat_is_nil(entry->key)) {
            return entry->value;
        }
    }
    return lhat_nil();
}

bool lhat_table_set(LhatTable *table, LhatValue key, LhatValue value,
                    bool *refused)
{
    *refused = false;
    if (table == NULL || !usable_key(key)) {
        *refused = true;
        return true;
    }
    key = normalise_key(key);

    size_t index;
    if (array_index(table, key, &index)) {
        if (lhat_is_nil(value) && index + 1 == table->array_count) {
            table->array_count--;  // removing the last one shortens the run
            return true;
        }
        table->array[index] = value;
        return true;
    }

    // The next key along extends the dense part rather than starting a hash
    // entry, which is what makes a table built up in order stay an array.
    if (lhat_is_integer(key) && !lhat_is_nil(value) &&
        (uint64_t)lhat_as_integer(key) == (uint64_t)table->array_count + 1) {
        if (table->array_count == table->array_capacity && !grow_array(table)) {
            return false;
        }
        table->array[table->array_count++] = value;
        drain_into_array(table);
        return true;
    }

    if (lhat_is_nil(value)) {
        if (table->entry_capacity == 0) {
            return true;
        }
        LhatTableEntry *entry =
            probe(table->entries, table->entry_capacity, key, hash_key(key));
        if (!lhat_is_nil(entry->key)) {
            entry->key = lhat_nil();
            entry->value = lhat_bool(true);  // a tombstone
            table->entry_count--;
        }
        return true;
    }

    // Kept under three quarters full: linear probing degrades badly past that,
    // and tombstones make the real occupancy higher than the count says.
    if (table->entry_count + 1 > table->entry_capacity - table->entry_capacity / 4) {
        if (!grow_entries(table)) {
            return false;
        }
    }
    LhatTableEntry *entry =
        probe(table->entries, table->entry_capacity, key, hash_key(key));
    if (lhat_is_nil(entry->key)) {
        table->entry_count++;
    }
    entry->key = key;
    entry->value = value;
    return true;
}

size_t lhat_table_length(const LhatTable *table)
{
    return table == NULL ? 0 : table->array_count;
}
