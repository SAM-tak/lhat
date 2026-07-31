// L^ (lhat) -- the heap values: strings and tables.

#include "object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

static void *allocate(LhatObject **owner, size_t size, LhatObjectKind kind)
{
    LhatObject *object = (LhatObject *)calloc(1, size);
    if (object == NULL) {
        return NULL;
    }
    object->kind = kind;
    object->next = *owner;
    *owner = object;
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

LhatString *lhat_string_new(LhatObject **owner, const char *text, size_t length)
{
    LhatString *string =
        (LhatString *)allocate(owner, sizeof *string + length + 1,
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

LhatTable *lhat_table_new(LhatObject **owner)
{
    return (LhatTable *)allocate(owner, sizeof(LhatTable), LHAT_OBJECT_TABLE);
}

LhatErrorKind *lhat_error_kind_new(LhatObject **owner,
                                   const LhatErrorKind *group,
                                   const LhatString *name)
{
    LhatErrorKind *kind = (LhatErrorKind *)allocate(owner, sizeof(LhatErrorKind),
                                                    LHAT_OBJECT_ERROR_KIND);
    if (kind == NULL) {
        return NULL;
    }
    kind->group = group;
    kind->name = name;
    return kind;
}

LhatError *lhat_error_new(LhatObject **owner, const LhatErrorKind *kind)
{
    LhatTable *fields = lhat_table_new(owner);
    if (fields == NULL) {
        return NULL;
    }
    LhatError *error =
        (LhatError *)allocate(owner, sizeof(LhatError), LHAT_OBJECT_ERROR);
    if (error == NULL) {
        return NULL;
    }
    error->kind = kind;
    error->fields = fields;
    return error;
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
        default:
            break;
    }
    free(object);
}

void lhat_object_free_all(LhatObject **owner)
{
    LhatObject *object = *owner;
    while (object != NULL) {
        LhatObject *next = object->next;
        lhat_object_free(object);
        object = next;
    }
    *owner = NULL;
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
    if (table == NULL || !usable_key(key)) {
        return lhat_nil();
    }
    key = normalise_key(key);

    size_t index;
    if (array_index(table, key, &index)) {
        return table->array[index];
    }
    if (table->entry_capacity == 0) {
        return lhat_nil();
    }
    LhatTableEntry *entry =
        probe(table->entries, table->entry_capacity, key, hash_key(key));
    return lhat_is_nil(entry->key) ? lhat_nil() : entry->value;
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
