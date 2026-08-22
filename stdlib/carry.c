// L^ (lhat) -- sample standard library: values carried between machines
// (see carry.h).
//
// The carried form is a flat array of nodes indexed by position; a table's
// entries and a closure's cells refer to other nodes by index, which is how
// a cycle and a shared sub-object survive the trip -- every object on the
// way in is memoised by its address, every node on the way out by its index.

#include "carry.h"

#include <string.h>

typedef enum {
    NODE_NIL,
    NODE_BOOL,
    NODE_INT,
    NODE_REAL,
    NODE_STRING,
    NODE_TABLE,
    NODE_CLOSURE,
    NODE_CELL      // a captured place; held by one or more closures
} NodeKind;

typedef struct {
    size_t key;    // node index
    size_t value;  // node index
} Entry;

typedef struct {
    NodeKind kind;
    bool boolean;
    int64_t integer;
    double real;
    char *bytes;               // STRING, owned
    size_t length;
    Entry *entries;            // TABLE, owned
    size_t entry_count;
    const LhatProto *proto;    // CLOSURE, borrowed from the program
    size_t *cells;             // CLOSURE: node indexes of NODE_CELLs, owned
    size_t cell_count;
    size_t held;               // CELL: the node index of what it holds
} Node;

struct LhatCarried {
    Node *nodes;
    size_t count;
    size_t capacity;
    size_t root;
};

// ---------------------------------------------------------------------------
// Memo: address -> node index, open addressing
// ---------------------------------------------------------------------------

typedef struct {
    const void **keys;
    size_t *values;
    size_t count;
    size_t capacity;  // a power of two, or 0
} Memo;

static size_t hash_pointer(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    return (size_t)x;
}

static bool memo_grow(Memo *memo)
{
    size_t wider = memo->capacity ? memo->capacity * 2 : 64;
    const void **keys = (const void **)lhat_calloc(wider, sizeof *keys);
    size_t *values = (size_t *)lhat_calloc(wider, sizeof *values);
    if (keys == NULL || values == NULL) {
        lhat_free(keys);
        lhat_free(values);
        return false;
    }
    for (size_t i = 0; i < memo->capacity; i++) {
        if (memo->keys[i] == NULL) {
            continue;
        }
        size_t at = hash_pointer(memo->keys[i]) & (wider - 1);
        while (keys[at] != NULL) {
            at = (at + 1) & (wider - 1);
        }
        keys[at] = memo->keys[i];
        values[at] = memo->values[i];
    }
    lhat_free(memo->keys);
    lhat_free(memo->values);
    memo->keys = keys;
    memo->values = values;
    memo->capacity = wider;
    return true;
}

static bool memo_find(const Memo *memo, const void *key, size_t *index)
{
    if (memo->capacity == 0) {
        return false;
    }
    size_t at = hash_pointer(key) & (memo->capacity - 1);
    while (memo->keys[at] != NULL) {
        if (memo->keys[at] == key) {
            *index = memo->values[at];
            return true;
        }
        at = (at + 1) & (memo->capacity - 1);
    }
    return false;
}

static bool memo_put(Memo *memo, const void *key, size_t index)
{
    if ((memo->count + 1) * 2 > memo->capacity && !memo_grow(memo)) {
        return false;
    }
    size_t at = hash_pointer(key) & (memo->capacity - 1);
    while (memo->keys[at] != NULL) {
        at = (at + 1) & (memo->capacity - 1);
    }
    memo->keys[at] = key;
    memo->values[at] = index;
    memo->count++;
    return true;
}

static void memo_dispose(Memo *memo)
{
    lhat_free(memo->keys);
    lhat_free(memo->values);
}

// ---------------------------------------------------------------------------
// Taking apart
// ---------------------------------------------------------------------------

typedef struct {
    LhatCarried *out;
    Memo memo;
    const char *refused;
} Carrier;

static bool refuse(Carrier *c, const char *why)
{
    if (c->refused == NULL) {
        c->refused = why;
    }
    return false;
}

// A new node, its index answered. The array may move, so a caller keeps
// indexes rather than pointers across any further allocation.
static bool add_node(Carrier *c, NodeKind kind, size_t *index)
{
    LhatCarried *carried = c->out;
    if (carried->count == carried->capacity) {
        size_t wider = carried->capacity ? carried->capacity * 2 : 32;
        Node *grown = (Node *)lhat_alloc(wider * sizeof *grown);
        if (grown == NULL) {
            return refuse(c, "out of memory");
        }
        memcpy(grown, carried->nodes, carried->count * sizeof *grown);
        lhat_free(carried->nodes);
        carried->nodes = grown;
        carried->capacity = wider;
    }
    Node *node = &carried->nodes[carried->count];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    *index = carried->count++;
    return true;
}

static bool carry_value(Carrier *c, LhatValue value, size_t *index);

static bool carry_table(Carrier *c, const LhatTable *table, size_t *index)
{
    if (memo_find(&c->memo, table, index)) {
        return true;
    }
    if (table->definition != NULL || table->is_definition) {
        return refuse(c, "a def^ and its instances stay on their machine");
    }
    if (table->sealed) {
        return refuse(c, "a module stays on its machine");
    }
    if (!add_node(c, NODE_TABLE, index) ||
        !memo_put(&c->memo, table, *index)) {
        return refuse(c, "out of memory");
    }
    size_t total = table->array_count + table->entry_count;
    Entry *entries = total > 0 ? (Entry *)lhat_alloc(total * sizeof *entries)
                               : NULL;
    if (total > 0 && entries == NULL) {
        return refuse(c, "out of memory");
    }
    size_t filled = 0;
    for (size_t i = 0; i < table->array_count; i++) {
        size_t key = 0;
        size_t held = 0;
        if (!carry_value(c, lhat_integer((int64_t)i + 1), &key) ||
            !carry_value(c, lhat_slots_get(table->array, i), &held)) {
            lhat_free(entries);
            return false;
        }
        entries[filled].key = key;
        entries[filled].value = held;
        filled++;
    }
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;
        }
        // A key is a string or a number in what crosses -- a table as a
        // key would be identity on this machine and nothing on the other.
        if (!lhat_is_number(entry->key) &&
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING) &&
            !lhat_is_bool(entry->key)) {
            lhat_free(entries);
            return refuse(c, "a key that is not a string, number or bool");
        }
        size_t key = 0;
        size_t held = 0;
        if (!carry_value(c, entry->key, &key) ||
            !carry_value(c, entry->value, &held)) {
            lhat_free(entries);
            return false;
        }
        entries[filled].key = key;
        entries[filled].value = held;
        filled++;
    }
    // The node array may have moved while the entries were carried.
    Node *node = &c->out->nodes[*index];
    node->entries = entries;
    node->entry_count = filled;
    return true;
}

static bool carry_closure(Carrier *c, LhatValue closure, size_t *index)
{
    const void *identity = lhat_as_object(closure);
    if (memo_find(&c->memo, identity, index)) {
        return true;
    }
    const LhatProto *proto = lhat_closure_proto(closure);
    if (proto == NULL) {
        return refuse(c, "not a closure");
    }
    size_t count = lhat_closure_capture_count(closure);
    if (!add_node(c, NODE_CLOSURE, index) ||
        !memo_put(&c->memo, identity, *index)) {
        return refuse(c, "out of memory");
    }
    size_t *cells = count > 0 ? (size_t *)lhat_alloc(count * sizeof *cells)
                              : NULL;
    if (count > 0 && cells == NULL) {
        return refuse(c, "out of memory");
    }
    for (size_t i = 0; i < count; i++) {
        const void *place = lhat_closure_capture_id(closure, i);
        size_t cell = 0;
        if (!memo_find(&c->memo, place, &cell)) {
            // A new place: the cell node first, memoised, then what it
            // holds -- which may be this very closure (15.10's this^).
            if (!add_node(c, NODE_CELL, &cell) ||
                !memo_put(&c->memo, place, cell)) {
                lhat_free(cells);
                return refuse(c, "out of memory");
            }
            size_t held = 0;
            if (!carry_value(c, lhat_closure_capture(closure, i), &held)) {
                lhat_free(cells);
                return false;
            }
            c->out->nodes[cell].held = held;
        }
        cells[i] = cell;
    }
    Node *node = &c->out->nodes[*index];
    node->proto = proto;
    node->cells = cells;
    node->cell_count = count;
    return true;
}

static bool carry_value(Carrier *c, LhatValue value, size_t *index)
{
    if (lhat_is_nil(value)) {
        return add_node(c, NODE_NIL, index);
    }
    if (lhat_is_bool(value)) {
        if (!add_node(c, NODE_BOOL, index)) {
            return false;
        }
        c->out->nodes[*index].boolean = lhat_as_bool(value);
        return true;
    }
    if (lhat_is_integer(value)) {
        if (!add_node(c, NODE_INT, index)) {
            return false;
        }
        c->out->nodes[*index].integer = lhat_as_integer(value);
        return true;
    }
    if (lhat_is_real(value)) {
        if (!add_node(c, NODE_REAL, index)) {
            return false;
        }
        c->out->nodes[*index].real = lhat_as_real(value);
        return true;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(value);
        char *copy =
            string->length > 0 ? (char *)lhat_alloc(string->length) : NULL;
        if (string->length > 0 && copy == NULL) {
            return refuse(c, "out of memory");
        }
        if (!add_node(c, NODE_STRING, index)) {
            lhat_free(copy);
            return false;
        }
        if (string->length > 0) {
            memcpy(copy, string->text, string->length);
        }
        c->out->nodes[*index].bytes = copy;
        c->out->nodes[*index].length = string->length;
        return true;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return carry_table(c, (const LhatTable *)lhat_as_object(value),
                           index);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        return carry_closure(c, value, index);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_COROUTINE)) {
        return refuse(c, "a coroutine stays on its machine");
    }
    if (lhat_is_hostvalue(value) ||
        lhat_is_object_kind(value, LHAT_OBJECT_HOSTVALUE_BOX) ||
        lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return refuse(c, "a host's value stays on its machine");
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOST) ||
        lhat_is_object_kind(value, LHAT_OBJECT_NATIVE)) {
        return refuse(c, "a host subroutine stays on its machine");
    }
    return refuse(c, "a value of a kind that does not cross");
}

bool lhat_carry(LhatValue value, LhatCarried **out, const char **refused)
{
    if (refused != NULL) {
        *refused = NULL;
    }
    if (out == NULL) {
        return false;
    }
    LhatCarried *carried = (LhatCarried *)lhat_calloc(1, sizeof *carried);
    if (carried == NULL) {
        if (refused != NULL) {
            *refused = "out of memory";
        }
        return false;
    }
    Carrier c;
    memset(&c, 0, sizeof c);
    c.out = carried;
    bool ok = carry_value(&c, value, &carried->root);
    memo_dispose(&c.memo);
    if (!ok) {
        if (refused != NULL) {
            *refused = c.refused != NULL ? c.refused : "out of memory";
        }
        lhat_carried_free(carried);
        return false;
    }
    *out = carried;
    return true;
}

void lhat_carried_free(LhatCarried *carried)
{
    if (carried == NULL) {
        return;
    }
    for (size_t i = 0; i < carried->count; i++) {
        lhat_free(carried->nodes[i].bytes);
        lhat_free(carried->nodes[i].entries);
        lhat_free(carried->nodes[i].cells);
    }
    lhat_free(carried->nodes);
    lhat_free(carried);
}

// ---------------------------------------------------------------------------
// Putting back together
// ---------------------------------------------------------------------------

typedef struct {
    LhatMachine *machine;
    const LhatCarried *carried;
    LhatValue *made;        // per node, once built (nil^ until then)
    bool *built;
    LhatUpvalue **cells;    // per node, for NODE_CELLs
} Builder;

static bool build(Builder *b, size_t index, LhatValue *out);

static bool build_cell(Builder *b, size_t index)
{
    if (b->cells[index] != NULL) {
        return true;
    }
    // The cell first, holding nil^, so that what it holds may reach back
    // to it through a closure; its value is written in once built.
    if (!lhat_machine_make_cell(b->machine, lhat_nil(), &b->cells[index])) {
        return false;
    }
    LhatValue held = lhat_nil();
    if (!build(b, b->carried->nodes[index].held, &held)) {
        return false;
    }
    lhat_ref_set(lhat_upvalue_closed_ref(b->cells[index]), held);
    return true;
}

static bool build(Builder *b, size_t index, LhatValue *out)
{
    const Node *node = &b->carried->nodes[index];
    if (b->built[index]) {
        *out = b->made[index];
        return true;
    }
    switch (node->kind) {
        case NODE_NIL:
            *out = lhat_nil();
            break;
        case NODE_BOOL:
            *out = lhat_bool(node->boolean);
            break;
        case NODE_INT:
            *out = lhat_integer(node->integer);
            break;
        case NODE_REAL:
            *out = lhat_real(node->real);
            break;
        case NODE_STRING:
            if (!lhat_machine_make_string(b->machine, node->bytes,
                                          node->length, out)) {
                return false;
            }
            break;
        case NODE_TABLE: {
            if (!lhat_machine_make_table(b->machine, out)) {
                return false;
            }
            // Memoised before its entries are built, so a cycle finds it.
            b->made[index] = *out;
            b->built[index] = true;
            LhatTable *table = (LhatTable *)lhat_as_object(*out);
            for (size_t i = 0; i < node->entry_count; i++) {
                LhatValue key = lhat_nil();
                LhatValue held = lhat_nil();
                if (!build(b, node->entries[i].key, &key) ||
                    !build(b, node->entries[i].value, &held) ||
                    !lhat_machine_table_set(b->machine, table, key, held,
                                            NULL)) {
                    return false;
                }
            }
            return true;
        }
        case NODE_CLOSURE: {
            // Every cell exists (holding nil^) before the closure is made,
            // the closure is memoised, and only then are the cells filled
            // -- a cell holding this closure (15.10's this^) finds it.
            LhatUpvalue **cells =
                node->cell_count > 0
                    ? (LhatUpvalue **)lhat_calloc(node->cell_count,
                                                  sizeof *cells)
                    : NULL;
            if (node->cell_count > 0 && cells == NULL) {
                return false;
            }
            for (size_t i = 0; i < node->cell_count; i++) {
                size_t cell = node->cells[i];
                if (b->cells[cell] == NULL &&
                    !lhat_machine_make_cell(b->machine, lhat_nil(),
                                            &b->cells[cell])) {
                    lhat_free(cells);
                    return false;
                }
                cells[i] = b->cells[cell];
            }
            bool made = lhat_machine_make_closure_with(
                b->machine, node->proto, cells, node->cell_count, out);
            lhat_free(cells);
            if (!made) {
                return false;
            }
            b->made[index] = *out;
            b->built[index] = true;
            for (size_t i = 0; i < node->cell_count; i++) {
                size_t cell = node->cells[i];
                // A cell already filled by an earlier closure keeps it.
                if (b->built[cell]) {
                    continue;
                }
                b->built[cell] = true;
                LhatValue held = lhat_nil();
                if (!build(b, b->carried->nodes[cell].held, &held)) {
                    return false;
                }
                lhat_ref_set(lhat_upvalue_closed_ref(b->cells[cell]), held);
            }
            return true;
        }
        case NODE_CELL:
            // Reached only through a closure above; a cell is never a value.
            return build_cell(b, index);
    }
    b->made[index] = *out;
    b->built[index] = true;
    return true;
}

bool lhat_uncarry(LhatMachine *machine, const LhatCarried *carried,
                  LhatValue *out)
{
    if (machine == NULL || carried == NULL || out == NULL) {
        return false;
    }
    Builder b;
    b.machine = machine;
    b.carried = carried;
    b.made = (LhatValue *)lhat_calloc(carried->count ? carried->count : 1,
                                      sizeof *b.made);
    b.built = (bool *)lhat_calloc(carried->count ? carried->count : 1,
                                  sizeof *b.built);
    b.cells = (LhatUpvalue **)lhat_calloc(carried->count ? carried->count : 1,
                                          sizeof *b.cells);
    bool ok = b.made != NULL && b.built != NULL && b.cells != NULL &&
              build(&b, carried->root, out);
    lhat_free(b.made);
    lhat_free(b.built);
    lhat_free(b.cells);
    return ok;
}
