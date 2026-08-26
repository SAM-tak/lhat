// L^ (lhat) -- the heap values: strings and tables.

#include "lhat/object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "code.h"  // 14.4: a proto says whether its body takes a receiver
#include "grow.h"
#include "lhat/port.h"

// 04 の 2.6 and 6.1: whether `value` is an error of `kind`. A kind object
// standing for a whole errordef^ answers yes for any of its kinds, since 2.3
// makes the declaration the union of them.
static bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind);

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

void *lhat_object_alloc(LhatHeap *heap, size_t size, LhatObjectKind kind)
{
    LhatObject *object = (LhatObject *)lhat_calloc(1, size);
    if (object == NULL) {
        return NULL;
    }
    object->kind = kind;
    object->color = heap->white;
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

// 02 の 14.18: code points, counted by skipping the continuation bytes
// 01 の 1 章's UTF-8 spells them with. A byte sequence that is not UTF-8 at
// all still answers -- every stray byte counts as one -- since the count is a
// reading of the bytes and not a judgement on them.
static uint32_t count_characters(const char *text, size_t length)
{
    size_t characters = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            characters++;
        }
    }
    return (uint32_t)characters;
}

LhatString *lhat_string_new(LhatHeap *heap, const char *text, size_t length)
{
    LhatString *string =
        (LhatString *)lhat_object_alloc(
            heap, sizeof *string + length + 1, LHAT_OBJECT_STRING);
    if (string == NULL) {
        return NULL;
    }
    if (length > 0) {
        memcpy(string->text, text, length);
    }
    string->text[length] = '\0';
    string->length = length;
    string->hash = lhat_string_hash(text, length);
    string->characters = count_characters(text, length);
    return string;
}

LhatString *lhat_string_concat(LhatHeap *heap, const LhatString *left,
                               const LhatString *right)
{
    size_t length = left->length + right->length;
    LhatString *joined =
        (LhatString *)lhat_object_alloc(
            heap, sizeof *joined + length + 1, LHAT_OBJECT_STRING);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined->text, left->text, left->length);
    memcpy(joined->text + left->length, right->text, right->length);
    joined->text[length] = '\0';
    joined->length = length;
    joined->hash = lhat_string_hash(joined->text, length);
    // Joining bytes joins code points: a continuation byte cannot begin the
    // right half without having been one there too, so no walk is needed.
    joined->characters = left->characters + right->characters;
    return joined;
}

LhatTable *lhat_table_new(LhatHeap *heap)
{
    return (LhatTable *)lhat_object_alloc(heap, sizeof(LhatTable),
                                          LHAT_OBJECT_TABLE);
}

LhatErrorKind *lhat_error_kind_new(LhatHeap *heap,
                                   const LhatErrorKind *group, bool local,
                                   const LhatString *name)
{
    LhatErrorKind *kind = (LhatErrorKind *)lhat_object_alloc(
        heap, sizeof(LhatErrorKind), LHAT_OBJECT_ERROR_KIND);
    if (kind == NULL) {
        return NULL;
    }
    kind->group = group;
    kind->name = name;
    // 04 の 2.7: a kind is of the family its declaration is, so the group
    // decides for every kind under it and nothing can be told two things.
    kind->local = group != NULL ? group->local : local;
    return kind;
}

LhatError *lhat_error_new(LhatHeap *heap, const LhatErrorKind *kind)
{
    LhatTable *fields = lhat_table_new(heap);
    if (fields == NULL) {
        return NULL;
    }
    LhatError *error =
        (LhatError *)lhat_object_alloc(heap, sizeof(LhatError),
                                       LHAT_OBJECT_ERROR);
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
        (LhatCoroutine *)lhat_object_alloc(
            heap, sizeof(LhatCoroutine), LHAT_OBJECT_COROUTINE);
    if (coroutine == NULL) {
        return NULL;
    }
    // 2.2: the saved frame in the stack's own two-run shape. calloc gives
    // all-zero payloads and tags, and tag zero is LHAT_VALUE_NIL -- so the
    // slots start as nil^ without a pass of their own.
    size_t width = registers ? registers : 1;
    coroutine->registers.values = (LhatValueUnion *)lhat_calloc(
        width, sizeof *coroutine->registers.values);
    coroutine->registers.tags =
        (uint8_t *)lhat_calloc(width, sizeof *coroutine->registers.tags);
    if (coroutine->registers.values == NULL ||
        coroutine->registers.tags == NULL) {
        return NULL;
    }
    coroutine->closure = closure;
    coroutine->state = LHAT_COROUTINE_FRESH;
    coroutine->register_count = registers;
    return coroutine;
}

LhatCoroutine *lhat_table_iterator(LhatHeap *heap, const LhatTable *table,
                                   LhatWalkPart part)
{
    LhatCoroutine *walk =
        (LhatCoroutine *)lhat_object_alloc(
            heap, sizeof(LhatCoroutine), LHAT_OBJECT_COROUTINE);
    if (walk == NULL) {
        return NULL;
    }
    walk->state = LHAT_COROUTINE_FRESH;
    walk->source = LHAT_COROUTINE_TABLE;
    walk->part = part;
    walk->walking = table;
    return walk;
}

LhatCoroutine *lhat_host_iterator(LhatHeap *heap, LhatHostStepFn step,
                                  void *context, LhatHostFn release,
                                  LhatValue held)
{
    LhatCoroutine *walk =
        (LhatCoroutine *)lhat_object_alloc(
            heap, sizeof(LhatCoroutine), LHAT_OBJECT_COROUTINE);
    if (walk == NULL) {
        return NULL;
    }
    walk->state = LHAT_COROUTINE_FRESH;
    walk->source = LHAT_COROUTINE_HOST;
    walk->step = step;
    walk->host_state = context;
    walk->host_release = release;
    walk->held = held;
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
        *value = lhat_slots_get(table->array, walk->at_array++);
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
        (LhatNative *)lhat_object_alloc(heap, sizeof(LhatNative),
                                        LHAT_OBJECT_NATIVE);
    if (native == NULL) {
        return NULL;
    }
    native->kind = kind;
    native->bound = bound;
    return native;
}

LhatHost *lhat_host_new(LhatHeap *heap, LhatHostFn call, void *context,
                        uint8_t parameters, bool has_variadic, bool takes_self)
{
    LhatHost *host =
        (LhatHost *)lhat_object_alloc(heap, sizeof(LhatHost), LHAT_OBJECT_HOST);
    if (host == NULL) {
        return NULL;
    }
    host->call = call;
    host->context = context;
    host->bound = lhat_nil();
    host->parameters = parameters;
    host->has_variadic = has_variadic;
    host->takes_self = takes_self;
    host->self_last = false;
    host->parameter_types = NULL;
    return host;
}

LhatHostData *lhat_hostdata_new(LhatHeap *heap, const LhatHostDataTag *tag,
                                void *pointer, LhatTable *members)
{
    LhatHostData *data =
        (LhatHostData *)lhat_object_alloc(heap, sizeof(LhatHostData),
                                          LHAT_OBJECT_HOSTDATA);
    if (data == NULL) {
        return NULL;
    }
    data->tag = tag;
    data->pointer = pointer;
    data->members = members;
    return data;
}

LhatHostValueBox *lhat_hostvalue_box_new(LhatHeap *heap,
                                         const LhatHostValueTag *tag)
{
    if (tag == NULL) {
        return NULL;
    }
    LhatHostValueBox *box = (LhatHostValueBox *)lhat_object_alloc(
        heap, sizeof(LhatHostValueBox) + tag->width * sizeof(LhatValueUnion),
        LHAT_OBJECT_HOSTVALUE_BOX);
    if (box != NULL) {
        // lhat_object_alloc zeroes, so the tail padding byte equality relies
        // on is already in place; only the head needs its tag.
        box->run[0].hostvalue = tag;
    }
    return box;
}

LhatRuntimeType *lhat_type_rt_new(LhatHeap *heap, LhatRuntimeTypeKind kind)
{
    LhatRuntimeType *type =
        (LhatRuntimeType *)lhat_object_alloc(
            heap, sizeof(LhatRuntimeType), LHAT_OBJECT_TYPE);
    if (type != NULL) {
        type->kind = kind;
    }
    return type;
}

bool lhat_type_rt_add_part(LhatRuntimeType *type, LhatRuntimeType *part)
{
    LhatRuntimeType **grown =
        (LhatRuntimeType **)lhat_realloc(type->parts,
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
    void *grown = lhat_realloc(type->members,
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
        // 03 の 3.4: inference did not decide, so nothing is being asked --
        // the same answer as a place nobody wrote. Only 14.16's writing tells
        // the two apart.
        case LHAT_TYPE_RT_UNKNOWN:
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
        case LHAT_TYPE_RT_TABLE: {
            // 14.10: at least these members, which is what makes the judgement
            // structural rather than a question about where it came from.
            const LhatTable *table = NULL;
            if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
                table = (const LhatTable *)lhat_as_object(value);
            }
            else if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
                table = ((const LhatError *)lhat_as_object(value))->fields;
            } else {
                return false;
            }
            for (size_t i = 0; i < type->member_count; i++) {
                LhatValue held = lhat_table_get(
                    table, lhat_object((LhatObject *)(void *)
                                           type->members[i]
                                               .name));
                if (lhat_is_nil(held) ||
                    !lhat_value_satisfies(held, type->members[i].type)) {
                    return false;
                }
            }
            return true;
        case LHAT_TYPE_RT_SUBROUTINE:
            return lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE);
        case LHAT_TYPE_RT_COROUTINE:
            return lhat_is_object_kind(value, LHAT_OBJECT_COROUTINE);
        // 04 の 2.7: a family, not every error. The two tops are disjoint, so
        // asking error^ of a localerror^ answers false.
        case LHAT_TYPE_RT_ERROR: {
            if (!lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
                return false;
            }
            const LhatErrorKind *kind =
                ((const LhatError *)lhat_as_object(value))->kind;
            return kind != NULL && kind->local == type->error_local;
        }
        case LHAT_TYPE_RT_ERROR_KIND:
            return lhat_error_is_kind(value, type->error_kind);
        // 05 の 8.8: identity is the tag alone, so the value has to actually
        // be hostdata -- otherwise there is no tag to compare and the answer
        // is about two different kinds of value.
        case LHAT_TYPE_RT_HOSTDATA:
            return lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA) &&
                   ((const LhatHostData *)lhat_as_object(value))->tag ==
                       type->hostdata_tag;
        // 05 の 8.9: the same rule over the head slot's own tag. A head
        // travels as the first slot of its width, so asking the head is
        // asking the value.
        case LHAT_TYPE_RT_HOSTVALUE:
            return lhat_is_hostvalue(value) &&
                   lhat_as_hostvalue_tag(value) == type->hostvalue_tag;
        // 05 の 8.9: the same tag, read off the box's own head slot.
        case LHAT_TYPE_RT_HOSTVALUE_BOX:
            return lhat_is_object_kind(value, LHAT_OBJECT_HOSTVALUE_BOX) &&
                   lhat_hostvalue_box_tag(
                       (const LhatHostValueBox *)lhat_as_object(value)) ==
                       type->hostvalue_tag;
        // 13.8改: no value satisfies a tuple. One is never a value in hand --
        // it lives in the slots a call reserved and is taken apart there,
        // which is why the checker refuses it everywhere a name could hold
        // one. Reaching here means a written type got past that, and false is
        // the honest answer: there is nothing to hold up against it.
        case LHAT_TYPE_RT_TUPLE:
            return false;
        case LHAT_TYPE_RT_UNION:
            for (size_t i = 0; i < type->part_count; i++) {
                if (lhat_value_satisfies(value, type->parts[i])) {
                    return true;
                }
            }
            return false;
        // 14.5, 14.12: an overload^ed member has to fit every arm at once --
        // it is one value callable every way the intersection lists.
        case LHAT_TYPE_RT_INTERSECT:
            for (size_t i = 0; i < type->part_count; i++) {
                if (!lhat_value_satisfies(value, type->parts[i])) {
                    return false;
                }
            }
            return true;
        // 02 の 13.13: the structure this one is written inside, which the
        // walk has already been through. Asked of a value it answers what the
        // cycle used to be flattened to -- that it is a table -- since telling
        // more would mean walking a value that may hold itself as well. The
        // checker is where the shape is judged (03 の 5.11c); this is 11.6's
        // own check, and it says no less than it did.
        case LHAT_TYPE_RT_SELF:
            return lhat_is_object_kind(value, LHAT_OBJECT_TABLE);
        }
    }
    return false;
}

static int compare_members(const void *a, const void *b)
{
    const LhatString *left = ((const LhatRuntimeTypeMember *)a)->name;
    const LhatString *right = ((const LhatRuntimeTypeMember *)b)->name;
    size_t shorter = left->length < right->length ? left->length : right->length;
    int cmp = memcmp(left->text, right->text, shorter);
    if (cmp != 0) {
        return cmp;
    }
    return left->length < right->length ? -1
                                        : (left->length > right->length ? 1 : 0);
}

void lhat_type_rt_sort_members(LhatRuntimeType *type)
{
    qsort(type->members, type->member_count, sizeof *type->members,
         compare_members);
}

// ---------------------------------------------------------------------------
// Printing a runtime type down (02 の 14.10, 14.16)
// ---------------------------------------------------------------------------

// Mirrors value.c's Writer -- each file that writes text down keeps its own
// copy of the idiom rather than sharing one across a header.
typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} TypeWriter;

static void type_put(TypeWriter *w, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (w->out != NULL && w->used + 1 < w->capacity) {
            w->out[w->used] = text[i];
        }
        w->used++;
    }
}

static void type_put_text(TypeWriter *w, const char *text)
{
    type_put(w, text, strlen(text));
}

static void write_runtime_type(TypeWriter *w, const LhatRuntimeType *type);

// 14.10's brace half, shared by the two things that wear it: a structure
// after its 't^', and 14.7改's self^{ … } section inside a definition.
static bool has_member_named(const LhatRuntimeType *type,
                             const LhatString *name)
{
    for (size_t i = 0; i < type->member_count; i++) {
        const LhatString *had = type->members[i].name;
        if (had->length == name->length &&
            memcmp(had->text, name->text, name->length) == 0) {
            return true;
        }
    }
    return false;
}

static void write_structure_body(TypeWriter *w, const LhatRuntimeType *type)
{
    type_put_text(w, "{");
    bool first = true;
    // 02 の 14.7改: what the instances carry comes before the definition's
    // own members -- new and the static ones -- the way the def^ was written.
    if (type->instance != NULL) {
        type_put_text(w, " self^");
        write_structure_body(w, type->instance);
        first = false;
    }
    // 14 章: the sequence half first, in order, the way a value written down
    // puts it (value.c's write_table) -- then the named half, sorted into a
    // canonical order already (this is read-only from here).
    for (size_t i = 0; i < type->part_count; i++) {
        type_put_text(w, first ? " " : ", ");
        first = false;
        write_runtime_type(w, type->parts[i]);
    }
    for (size_t i = 0; i < type->member_count; i++) {
        // What the section already showed is not written twice: an instance's
        // member is reachable through the definition as well (14.4's
        // `let^ f = A.m`), which is what the section is saying.
        if (type->instance != NULL &&
            has_member_named(type->instance, type->members[i].name)) {
            continue;
        }
        type_put_text(w, first ? " " : ", ");
        first = false;
        type_put(w, type->members[i].name->text, type->members[i].name->length);
        type_put_text(w, " : ");
        write_runtime_type(w, type->members[i].type);
    }
    type_put_text(w, first ? "}" : " }");
}

// 13.8改2: a result position takes a tuple bare -- a top-level ',' there is
// the tuple's, and the parentheses are left to say "the union covers the
// whole of this" ('(A, B)|SomeError'). This text is read back as a type
// (02 の 14.16's `.signature`), so it spells what the parser accepts:
// a signature's result, closed by 13.3's ';', and a coroutine's slots.
static void write_runtime_result(TypeWriter *w, const LhatRuntimeType *type)
{
    if (type != NULL && type->kind == LHAT_TYPE_RT_TUPLE) {
        for (size_t i = 0; i < type->part_count; i++) {
            if (i > 0) {
                type_put_text(w, ", ");
            }
            write_runtime_type(w, type->parts[i]);
        }
        return;
    }
    write_runtime_type(w, type);
}

static void write_runtime_type(TypeWriter *w, const LhatRuntimeType *type)
{
    if (type == NULL) {
        // 13.7: nothing written asks for the top type, not nothing.
        type_put_text(w, "any^");
        return;
    }
    switch (type->kind) {
        case LHAT_TYPE_RT_ANY:
            type_put_text(w, "any^");
            return;
        // 03 の 3.4: not a type. 05 の 8.7 has a signature read back as a type
        // expression, and one with this in it will not -- which is the point.
        // A place inference did not decide is where an annotation is wanted,
        // and writing any^ there would hide both facts.
        case LHAT_TYPE_RT_UNKNOWN:
            type_put_text(w, "UNKNOWN");
            return;
        case LHAT_TYPE_RT_NIL:
            type_put_text(w, "nil^");
            return;
        case LHAT_TYPE_RT_BOOL:
            type_put_text(w, "bool^");
            return;
        case LHAT_TYPE_RT_NUMBER:
            type_put_text(w, "number^");
            return;
        case LHAT_TYPE_RT_STRING:
            type_put_text(w, "string^");
            return;
        // 04 の 2.7: two tops, and 14.16 wants what it writes to read back.
        case LHAT_TYPE_RT_ERROR:
            type_put_text(w, type->error_local ? "localerror^" : "error^");
            return;
        // 04 の 2.4: identity is the declaration, and the declaration's own
        // name is already qualified (05 の 7.3) -- the same text that names
        // it in an is^ or an annotation.
        case LHAT_TYPE_RT_ERROR_KIND:
            if (type->error_kind != NULL && type->error_kind->name != NULL) {
                type_put(w, type->error_kind->name->text,
                        type->error_kind->name->length);
            } else {
                type_put_text(w, "error^");
            }
            return;
        // 05 の 8.7: a signature reads back as a type expression, and 8.8
        // names a host type the same way a program writes it -- qualified by
        // the module it was registered under.
        case LHAT_TYPE_RT_HOSTDATA:
            if (type->hostdata_tag == NULL) {
                type_put_text(w, "UNKNOWN");
                return;
            }
            if (type->hostdata_tag->module != NULL) {
                type_put_text(w, type->hostdata_tag->module);
                type_put_text(w, ".");
            }
            type_put_text(w, type->hostdata_tag->name);
            return;

        // 05 の 8.9: named the same qualified way; the box adds its own word.
        case LHAT_TYPE_RT_HOSTVALUE:
        case LHAT_TYPE_RT_HOSTVALUE_BOX:
            if (type->hostvalue_tag == NULL) {
                type_put_text(w, "UNKNOWN");
                return;
            }
            if (type->hostvalue_tag->module != NULL) {
                type_put_text(w, type->hostvalue_tag->module);
                type_put_text(w, ".");
            }
            type_put_text(w, type->hostvalue_tag->name);
            if (type->kind == LHAT_TYPE_RT_HOSTVALUE_BOX) {
                type_put_text(w, ".Box^");
            }
            return;
        // 13.9's three slots. NULL still prints any^ -- not a guess, the same
        // "nothing written asks for the top type" convention as everywhere
        // else in this function.
        // 15.3改: the front half is written as the signature it always
        // described -- one resume takes R and answers Y -- which is where the
        // kind of the body goes, both kinds being possible (15.3改).
        case LHAT_TYPE_RT_COROUTINE: {
            // 13.9改: 'c^{f^R -> Y -> T}'. An empty slot is written by
            // leaving it out, so a NULL is not the "nothing written asks for
            // any^" of every other position here -- it is the slot saying
            // nothing is sent in, or that the body ends without a value. How
            // many arrows are written is how many slots were, so a third
            // slot needs the second arrow even where Y is empty. '-' is the
            // third slot's other absence, a body that cannot end.
            type_put_text(w, "c^{");
            type_put_text(w, type->is_function ? "f^" : "p^");
            if (type->receive != NULL) {
                write_runtime_result(w, type->receive);
            }
            bool ends = type->endless || type->result != NULL;
            if (type->produce != NULL || ends) {
                type_put_text(w, " -> ");
                if (type->produce != NULL) {
                    write_runtime_result(w, type->produce);
                }
            }
            if (ends) {
                type_put_text(w, " -> ");
                if (type->endless) {
                    type_put_text(w, "-");
                } else {
                    write_runtime_result(w, type->result);
                }
            }
            type_put_text(w, "}");
            return;
        }
        // 13.8改: '(A, B)'. The parentheses the type grammar already used for
        // grouping, which is why there is no one-position form to write.
        // 13.8改2: reached here the tuple stands where a bare ',' would be
        // read as something else's -- a result goes through
        // write_runtime_result instead.
        case LHAT_TYPE_RT_TUPLE:
            type_put_text(w, "(");
            for (size_t i = 0; i < type->part_count; i++) {
                if (i > 0) {
                    type_put_text(w, ", ");
                }
                write_runtime_type(w, type->parts[i]);
            }
            type_put_text(w, ")");
            return;
        case LHAT_TYPE_RT_UNION:
            for (size_t i = 0; i < type->part_count; i++) {
                if (i > 0) {
                    type_put_text(w, "|");
                }
                write_runtime_type(w, type->parts[i]);
            }
            return;
        // 14.5: '&' is intersection, the same symbol 14.12's overload^ shows
        // a multi-dispatched member's type with.
        case LHAT_TYPE_RT_INTERSECT:
            for (size_t i = 0; i < type->part_count; i++) {
                if (i > 0) {
                    type_put_text(w, " & ");
                }
                // 11.5: '&' binds tighter than '|', so a union arm needs the
                // grouping parentheses back -- without them the text reads
                // as '(A & B) | C', which is a different type.
                bool wrap = type->parts[i] != NULL &&
                            type->parts[i]->kind == LHAT_TYPE_RT_UNION;
                if (wrap) {
                    type_put_text(w, "(");
                }
                write_runtime_type(w, type->parts[i]);
                if (wrap) {
                    type_put_text(w, ")");
                }
            }
            return;
        case LHAT_TYPE_RT_SUBROUTINE: {
            if (type->closed) {  // 15.13, before the kind as it is written
                type_put_text(w, "closed^");
            }
            type_put_text(w, type->is_function ? "f^" : "p^");
            // 14.4: in a type the receiver is a parameter, written as the
            // word itself -- and 11.3改 has it trail on a binary operator,
            // where it is the right operand.
            bool others = type->part_count > 0 || type->variadic != NULL;
            if (type->takes_self && !type->self_last) {
                type_put_text(w, "self^");
                if (others) {
                    type_put_text(w, ", ");
                }
            }
            for (size_t i = 0; i < type->part_count; i++) {
                if (i > 0) {
                    type_put_text(w, ", ");
                }
                write_runtime_type(w, type->parts[i]);
            }
            // 13.7: the same spelling a parameter list ends in.
            if (type->variadic != NULL) {
                if (type->part_count > 0) {
                    type_put_text(w, ", ");
                }
                type_put_text(w, "...:");
                write_runtime_type(w, type->variadic);
            }
            if (type->takes_self && type->self_last) {
                if (others) {
                    type_put_text(w, ", ");
                }
                type_put_text(w, "self^");
            }
            if (type->result != NULL) {
                type_put_text(w, " -> ");
                write_runtime_result(w, type->result);
            }
            type_put_text(w, ";");
            return;
        }
        // 02 の 13.13: the structure this one sits inside, a hat per step out.
        case LHAT_TYPE_RT_SELF:
            type_put_text(w, "Self");
            for (unsigned i = 0; i < (type->levels > 0 ? type->levels : 1);
                 i++) {
                type_put_text(w, "^");
            }
            return;
        case LHAT_TYPE_RT_TABLE:
            type_put_text(w, "t^");
            write_structure_body(w, type);
            return;
    }
}

size_t lhat_runtime_type_write(const LhatRuntimeType *type, char *out,
                               size_t capacity)
{
    TypeWriter w;
    w.out = out;
    w.capacity = capacity;
    w.used = 0;
    write_runtime_type(&w, type);
    if (out != NULL && capacity > 0) {
        out[w.used < capacity ? w.used : capacity - 1] = '\0';
    }
    return w.used;
}

// ---------------------------------------------------------------------------
// Comparing two runtime types (02 の 2811, 11.3, 14.9)
// ---------------------------------------------------------------------------

bool lhat_runtime_type_equal(const LhatRuntimeType *a, const LhatRuntimeType *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        // NULL is "nothing written", which 13.7 makes any^ -- the same
        // question lhat_value_satisfies answers by treating NULL as ANY.
        LhatRuntimeType blank;
        memset(&blank, 0, sizeof blank);
        blank.kind = LHAT_TYPE_RT_ANY;
        return lhat_runtime_type_equal(a == NULL ? &blank : a,
                                       b == NULL ? &blank : b);
    }
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case LHAT_TYPE_RT_ANY:
        case LHAT_TYPE_RT_NIL:
        case LHAT_TYPE_RT_BOOL:
        case LHAT_TYPE_RT_NUMBER:
        case LHAT_TYPE_RT_STRING:
        // 03 の 3.4: two undecided places are the same undecided place. Not
        // equal to any^ though -- the kinds differ above, and NULL normalises
        // to any^ rather than to this, which keeps "nobody wrote one" apart
        // from "inference could not say".
        case LHAT_TYPE_RT_UNKNOWN:
            return true;
        // 04 の 2.7: error^ and localerror^ are two types, not one.
        case LHAT_TYPE_RT_ERROR:
            return a->error_local == b->error_local;
        // 13.9's three slots now carry real answers, so two coroutine
        // types are equal only when R, Y and T line up -- not just by kind.
        // 15.3改 adds the body's own kind to that: what may be done with an
        // f^ coroutine is not what may be done with a p^ one.
        case LHAT_TYPE_RT_COROUTINE:
            // 13.9: an empty slot is a statement of its own, so it is told
            // apart from every type rather than normalised to any^ the way a
            // NULL is everywhere else here.
            if (a->endless != b->endless ||
                (a->receive == NULL) != (b->receive == NULL) ||
                (a->result == NULL) != (b->result == NULL)) {
                return false;
            }
            return a->is_function == b->is_function &&
                   lhat_runtime_type_equal(a->receive, b->receive) &&
                   lhat_runtime_type_equal(a->produce, b->produce) &&
                   lhat_runtime_type_equal(a->result, b->result);
        case LHAT_TYPE_RT_ERROR_KIND:
            // 04 の 2.4: identity is the declaration.
            return a->error_kind == b->error_kind;
        // 05 の 8.8: identity is the tag, which is the whole of what a
        // registered type is.
        case LHAT_TYPE_RT_HOSTDATA:
            return a->hostdata_tag == b->hostdata_tag;
        // 05 の 8.9: the same.
        case LHAT_TYPE_RT_HOSTVALUE:
        case LHAT_TYPE_RT_HOSTVALUE_BOX:
            return a->hostvalue_tag == b->hostvalue_tag;
        // 11.3's structural identity for a union or an intersection asks the
        // same of both sides without caring about the order the arms were
        // written in.
        case LHAT_TYPE_RT_UNION:
        case LHAT_TYPE_RT_INTERSECT: {
            // Each of one side has to be matched by some arm of the other,
            // both ways, and each arm of the other used at most once (so
            // A|A is not equal to A|B).
            if (a->part_count != b->part_count) {
                return false;
            }
            bool *matched = (bool *)lhat_calloc(b->part_count, sizeof *matched);
            if (matched == NULL) {
                return false;
            }
            bool equal = true;
            for (size_t i = 0; equal && i < a->part_count; i++) {
                bool found = false;
                for (size_t j = 0; j < b->part_count; j++) {
                    if (matched[j]) {
                        continue;
                    }
                    if (lhat_runtime_type_equal(a->parts[i], b->parts[j])) {
                        matched[j] = true;
                        found = true;
                        break;
                    }
                }
                equal = found;
            }
            lhat_free(matched);
            return equal;
        }
        // 13.8改: the order and the count are what a tuple means, so this is
        // the positional walk the SUBROUTINE arm below does rather than the
        // order-blind matching a union gets above.
        case LHAT_TYPE_RT_TUPLE:
            if (a->part_count != b->part_count) {
                return false;
            }
            for (size_t i = 0; i < a->part_count; i++) {
                if (!lhat_runtime_type_equal(a->parts[i], b->parts[i])) {
                    return false;
                }
            }
            return true;
        case LHAT_TYPE_RT_SUBROUTINE:
            if (a->is_function != b->is_function ||
                a->takes_self != b->takes_self ||
                // 11.3改: which operand the receiver is, is part of the shape
                // -- the checker refuses one where the other is written
                // (type.c's conforms_func), and the two are written apart.
                a->self_last != b->self_last ||
                a->closed != b->closed ||  // 15.13, and for the same reason
                a->part_count != b->part_count) {
                return false;
            }
            for (size_t i = 0; i < a->part_count; i++) {
                if (!lhat_runtime_type_equal(a->parts[i], b->parts[i])) {
                    return false;
                }
            }
            // Whether there is a tail at all is part of the shape -- unlike
            // NULL elsewhere (13.7 makes an absent one any^), one side
            // having no tail is not the same as a tail of any^.
            if ((a->variadic == NULL) != (b->variadic == NULL)) {
                return false;
            }
            if (a->variadic != NULL &&
                !lhat_runtime_type_equal(a->variadic, b->variadic)) {
                return false;
            }
            return lhat_runtime_type_equal(a->result, b->result);
        case LHAT_TYPE_RT_TABLE:
            // Both sides were sorted once when reflect_type built them
            // (lhat_type_rt_sort_members), so the same shape lines up
            // member-by-member without a search.
            if (a->part_count != b->part_count ||
                a->member_count != b->member_count) {
                return false;
            }
            for (size_t i = 0; i < a->part_count; i++) {
                if (!lhat_runtime_type_equal(a->parts[i], b->parts[i])) {
                    return false;
                }
            }
            for (size_t i = 0; i < a->member_count; i++) {
                const LhatString *an = a->members[i].name;
                const LhatString *bn = b->members[i].name;
                if (an->length != bn->length ||
                    memcmp(an->text, bn->text, an->length) != 0 ||
                    !lhat_runtime_type_equal(a->members[i].type,
                                             b->members[i].type)) {
                    return false;
                }
            }
            // 14.7改: two definitions are the same only if what they make is.
            return lhat_runtime_type_equal(a->instance, b->instance);
        // 13.13: the same word naming the same step out.
        case LHAT_TYPE_RT_SELF:
            return a->levels == b->levels;
    }
    return false;
}

LhatOverload *lhat_overload_new(LhatHeap *heap)
{
    return (LhatOverload *)lhat_object_alloc(heap, sizeof(LhatOverload),
                                             LHAT_OBJECT_OVERLOAD);
}

bool lhat_overload_add(LhatOverload *overload, LhatValue candidate)
{
    LHAT_GROW(overload->candidates, overload->count, overload->capacity, 4,
              return false);
    overload->candidates[overload->count++] = candidate;
    return true;
}

// 02 の 14.12: an override^ over an overloaded name replaces the one arm its
// signature overlaps. Which arm that is was settled by the checker, and the
// values here carry no signature to settle it again -- but they do not have
// to. The replacement is usable wherever that arm was and overlaps no other,
// so putting it first is enough: every call the arm would have taken now
// stops here, and the calls the other arms take never reach it.
//
// A fresh group rather than a write into the old one, because 14.12改 hands
// the old one to super^ -- prepending in place would make super^ reach the
// replacement and call itself.
LhatOverload *lhat_overload_with_first(LhatHeap *heap,
                                       const LhatOverload *existing,
                                       LhatValue candidate)
{
    LhatOverload *made = lhat_overload_new(heap);
    if (made == NULL || !lhat_overload_add(made, candidate)) {
        return NULL;
    }
    for (size_t i = 0; existing != NULL && i < existing->count; i++) {
        if (!lhat_overload_add(made, existing->candidates[i])) {
            return NULL;
        }
    }
    return made;
}

// 03 の 5.11c: the same replacement, once the checker has named the arm. The
// group then holds exactly the arms the name's type says it does -- the one
// replaced is gone rather than kept behind the replacement -- so an index
// into the arms means the same thing on both sides, which is what lets a
// call be settled at compile time.
//
// A fresh group for the reason above: super^ holds the old one.
LhatOverload *lhat_overload_replacing(LhatHeap *heap,
                                      const LhatOverload *existing, size_t arm,
                                      LhatValue candidate)
{
    if (existing == NULL || arm >= existing->count) {
        return NULL;
    }
    LhatOverload *made = lhat_overload_new(heap);
    if (made == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < existing->count; i++) {
        LhatValue part = i == arm ? candidate : existing->candidates[i];
        if (!lhat_overload_add(made, part)) {
            return NULL;
        }
    }
    return made;
}

static bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_ERROR) || kind == NULL) {
        return false;
    }
    const LhatErrorKind *held = ((const LhatError *)lhat_as_object(value))->kind;
    if (held == NULL) {
        return false;
    }
    // 2.3: a declaration is the union of its kinds, so naming it asks whether
    // the error is any of them. 2.7's built-in kinds stand under a top with
    // no declaration around them, so one of those is its own group and the
    // question is about the object itself -- which is why both readings are
    // taken. They cannot both hold: a group object is a type and 2.3 makes
    // no value of it, so an error's own kind is never one.
    return kind->group == NULL ? (held == kind || held->group == kind)
                               : held == kind;
}

// ---------------------------------------------------------------------------
// Giving back what the host made
// ---------------------------------------------------------------------------

bool lhat_hostdata_release(LhatObject *object, struct LhatMachine *machine)
{
    if (object == NULL || object->kind != LHAT_OBJECT_HOSTDATA) {
        return false;
    }
    LhatHostData *data = (LhatHostData *)object;
    if (data->released || data->tag == NULL || data->tag->release == NULL) {
        return false;
    }
    // Marked before the call, so a release that somehow arrives here twice
    // gives the pointer back once (02 の 10.7).
    data->released = true;

    // The value is still whole, so the release reads its pointer out of it
    // the way any other member of the type would.
    LhatValue self = lhat_object(object);
    data->tag->release(machine, data->tag->release_context, &self, 1);
    return true;
}

bool lhat_coroutine_release(LhatObject *object, struct LhatMachine *machine)
{
    if (object == NULL || object->kind != LHAT_OBJECT_COROUTINE) {
        return false;
    }
    LhatCoroutine *walk = (LhatCoroutine *)object;
    if (walk->source != LHAT_COROUTINE_HOST || walk->host_released ||
        walk->host_release == NULL) {
        return false;
    }
    // Marked before the call, so a release that somehow arrives here twice
    // gives the state back once (02 の 10.7) -- lhat_hostdata_release's rule.
    walk->host_released = true;

    LhatValue self = lhat_object(object);
    walk->host_release(machine, walk->host_state, &self, 1);
    return true;
}

void lhat_object_free(LhatObject *object)
{
    if (object == NULL) {
        return;
    }
    switch (object->kind) {
        case LHAT_OBJECT_TABLE: {
            LhatTable *table = (LhatTable *)object;
            lhat_free(table->array.values);
            lhat_free(table->array.tags);
            lhat_free(table->entries);
            break;
        }
        case LHAT_OBJECT_SUBROUTINE:
            lhat_free(((LhatClosure *)object)->upvalues);
            break;
        case LHAT_OBJECT_SCRIPT:
            lhat_proto_free(((LhatLoadedScript *)object)->root);
            break;
        case LHAT_OBJECT_COROUTINE:
            lhat_free(((LhatCoroutine *)object)->registers.values);
            lhat_free(((LhatCoroutine *)object)->registers.tags);
            break;
        case LHAT_OBJECT_TYPE:
            lhat_free(((LhatRuntimeType *)object)->parts);
            lhat_free(((LhatRuntimeType *)object)->members);
            break;
        case LHAT_OBJECT_OVERLOAD:
            lhat_free(((LhatOverload *)object)->candidates);
            break;
        // 02 の 14.12: the array belongs to the host; the types in it are the
        // heap's and are freed with everything else.
        case LHAT_OBJECT_HOST:
            lhat_free(((LhatHost *)object)->parameter_types);
            break;
        default:
            break;
    }
    lhat_free(object);
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
    // 05 の 8.9改: a bare host value is never a stored key -- its width does
    // not fit the entry's one slot -- and the generic get cannot read one
    // either (a head slot alone names no bytes); lookups by value go through
    // lhat_table_get_by_value instead.
    if (key.tag == LHAT_VALUE_HOSTVALUE) {
        return false;
    }
    return true;
}

// 05 の 8.9改: what a box's bytes hash to, shared with the lookup that asks
// with a bare value -- the two have to collide for content keying to work.
static uint32_t content_key_hash(const LhatHostValueTag *tag,
                                 const LhatValueUnion *data)
{
    uint32_t hash = lhat_string_hash((const char *)data,
                                     (tag->width - 1) * sizeof(LhatValueUnion));
    return hash ^ (uint32_t)((uintptr_t)tag >> 4);
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
            // 05 の 8.9改: a sealed box keys by its bytes and its tag --
            // computed on demand, since a sealed box never changes and the
            // runs are a few slots. The same formula answers for a bare
            // value asking at a lookup (lhat_table_get_by_value).
            if (object != NULL && object->kind == LHAT_OBJECT_HOSTVALUE_BOX) {
                const LhatHostValueBox *box = (const LhatHostValueBox *)object;
                const LhatHostValueTag *tag = lhat_hostvalue_box_tag(box);
                if (tag != NULL) {
                    return content_key_hash(tag, box->run + 1);
                }
            }
            // 05 の 8.8: two wrappers of one host object are equal, so they
            // have to hash alike -- off the tag and the host's pointer, the
            // pair equality reads. Not switched on `released`: a key stored
            // in a table has to keep its hash, and the pointer is only ever
            // compared as a value, never followed.
            if (object != NULL && object->kind == LHAT_OBJECT_HOSTDATA) {
                const LhatHostData *data = (const LhatHostData *)object;
                uintptr_t bits = (uintptr_t)data->pointer ^
                                 ((uintptr_t)data->tag << 16);
                return (uint32_t)(bits ^ (bits >> 32));
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
        (LhatTableEntry *)lhat_calloc(capacity, sizeof *entries);
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

    lhat_free(table->entries);
    table->entries = entries;
    table->entry_capacity = capacity;
    table->entry_count = moved;
    return true;
}

// ---------------------------------------------------------------------------
// The array part
// ---------------------------------------------------------------------------

// 02 の 14.14改: which slot of the dense half a key names, if any.
//
// A real names one too, rounded, the way 14.19's at reads an ordinal that came
// out of a division -- both are a number^ used as a position, and 14.8 makes
// the two representations one type, so the position should not depend on which
// one the arithmetic happened to leave. The rounding is at's, floor(x + 0.5),
// so that one spelling of "round a position" holds across the two.
//
// Only inside the run that is already there. Past its end a real stays an
// ordinary key: a table meaning to be read by real keys is one whose keys were
// never a run to begin with, and the dense half is what tells the two apart.
static bool array_index(const LhatTable *table, LhatValue key, size_t *index)
{
    int64_t i;
    if (lhat_is_integer(key)) {
        i = lhat_as_integer(key);
    } else if (lhat_is_real(key)) {
        double d = lhat_as_real(key);
        if (!(d > -9.0e15 && d < 9.0e15)) {
            return false;  // past what a position could name either way
        }
        i = (int64_t)floor(d + 0.5);
    } else {
        return false;
    }
    if (i < 1 || (uint64_t)i > table->array_count) {
        return false;
    }
    *index = (size_t)i - 1;
    return true;
}

static bool grow_array(LhatTable *table)
{
    size_t capacity = table->array_capacity ? table->array_capacity * 2 : 8;
    LhatValueUnion *values = (LhatValueUnion *)lhat_realloc(
        table->array.values, capacity * sizeof *values);
    if (values == NULL) {
        return false;
    }
    table->array.values = values;
    uint8_t *tags = (uint8_t *)lhat_realloc(table->array.tags,
                                            capacity * sizeof *tags);
    if (tags == NULL) {
        // The payload run grew and the capacity did not -- wasted room, not
        // an inconsistency, since count and capacity still hold.
        return false;
    }
    table->array.tags = tags;
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
        lhat_slots_set(table->array, table->array_count++, entry->value);
        entry->key = lhat_nil();
        entry->value = lhat_bool(true);  // a tombstone
        table->entry_count--;
    }
}

// ---------------------------------------------------------------------------
// Reading and writing
// ---------------------------------------------------------------------------

// What object.h publishes. The search it speaks of is vm.c's fits_call,
// which refuses a candidate taking no receiver where one was handed over.
bool lhat_takes_receiver(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        const LhatProto *proto =
            ((const LhatClosure *)lhat_as_object(value))->proto;
        return proto != NULL && proto->takes_self;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOST)) {
        return ((const LhatHost *)lhat_as_object(value))->takes_self;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *group =
            (const LhatOverload *)lhat_as_object(value);
        for (size_t i = 0; i < group->count; i++) {
            if (lhat_takes_receiver(group->candidates[i])) {
                return true;
            }
        }
    }
    return false;
}

LhatValue lhat_table_get(const LhatTable *table, LhatValue key)
{
    if (!usable_key(key)) {
        return lhat_nil();
    }
    key = normalise_key(key);

    // 14.7改: what an instance reaches through its definition is what takes a
    // receiver. 14.11's new and a static member are the definition's own, and
    // calling one with an instance before the dot would hand it a receiver it
    // never asked for. A walk that starts at a definition is the other case
    // -- 'A.new()', 'class^.somestatic()', and 14.5's walk up to a base --
    // and passes everything.
    bool restricted = table != NULL && !table->is_definition;

    // 14.7: an instance's own fields first, then the members its definition
    // holds. 14.2 fixes the chain when the instance is made, so this walk is
    // the one Lua's __index performs -- but written into the structure rather
    // than left to a hook that can be swapped (14.1).
    for (bool inherited = false; table != NULL;
         table = table->definition, inherited = true) {
        size_t index;
        if (array_index(table, key, &index)) {
            LhatValue value = lhat_slots_get(table->array, index);
            // 04 の 11.3: nothing is there, which is what a member that was
            // never meant to be reached this way amounts to. Calling it lands
            // on NOT_CALLABLE rather than on a receiver going somewhere odd.
            if (restricted && inherited && !lhat_takes_receiver(value)) {
                return lhat_nil();
            }
            return value;
        }
        if (table->entry_capacity == 0) {
            continue;
        }
        LhatTableEntry *entry =
            probe(table->entries, table->entry_capacity, key, hash_key(key));
        if (!lhat_is_nil(entry->key)) {
            if (restricted && inherited && !lhat_takes_receiver(entry->value)) {
                return lhat_nil();
            }
            return entry->value;
        }
    }
    return lhat_nil();
}

// 05 の 8.9改: a lookup asking with the bare value -- `t[vec]`. Everything
// a table stores under a box key is sealed, so comparing the bytes of the
// moment against them is exact; only the hash part can hold one, since a
// box never normalises to an array index.
LhatValue lhat_table_get_by_value(const LhatTable *table,
                                  const struct LhatHostValueTag *tag,
                                  const LhatValueUnion *data)
{
    if (table == NULL || tag == NULL || table->entry_capacity == 0) {
        return lhat_nil();
    }
    size_t width = (tag->width - 1) * sizeof(LhatValueUnion);
    size_t index = (size_t)content_key_hash(tag, data) &
                   (table->entry_capacity - 1);
    for (;;) {
        const LhatTableEntry *entry = &table->entries[index];
        if (entry_is_free(entry)) {
            return lhat_nil();
        }
        if (!entry_is_tombstone(entry) &&
            lhat_is_object_kind(entry->key, LHAT_OBJECT_HOSTVALUE_BOX)) {
            const LhatHostValueBox *box =
                (const LhatHostValueBox *)lhat_as_object(entry->key);
            if (lhat_hostvalue_box_tag(box) == tag &&
                memcmp(box->run + 1, data, width) == 0) {
                return entry->value;
            }
        }
        index = (index + 1) & (table->entry_capacity - 1);
    }
}

bool lhat_table_set(LhatTable *table, LhatValue key, LhatValue value,
                    bool *refused)
{
    *refused = false;
    if (table == NULL || !usable_key(key)) {
        *refused = true;
        return true;
    }
    // 05 の 8.9改: a box keys by its bytes, so only a sealed one -- whose
    // bytes hold still -- may be *stored* as a key. constbox^ is how one is
    // made; the checker says so first (MUTABLE_KEY), and this is the
    // unchecked run's backstop. A lookup is free to ask with a live box:
    // it reads the bytes of the moment, and everything the table holds is
    // sealed, so nothing can go stale.
    if (lhat_is_object_kind(key, LHAT_OBJECT_HOSTVALUE_BOX) &&
        !((const LhatHostValueBox *)lhat_as_object(key))->sealed) {
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
        lhat_slots_set(table->array, index, value);
        return true;
    }

    // The next key along extends the dense part rather than starting a hash
    // entry, which is what makes a table built up in order stay an array.
    if (lhat_is_integer(key) && !lhat_is_nil(value) &&
        (uint64_t)lhat_as_integer(key) == (uint64_t)table->array_count + 1) {
        if (table->array_count == table->array_capacity && !grow_array(table)) {
            return false;
        }
        lhat_slots_set(table->array, table->array_count++, value);
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

size_t lhat_table_count(const LhatTable *table)
{
    // 02 の 14.18: both halves, and only this table's own. What an instance
    // reads through `definition` (14.7) is shared rather than held here, so
    // it is not one of this table's elements.
    return table == NULL ? 0 : table->array_count + table->entry_count;
}

size_t lhat_string_characters(const LhatString *string)
{
    // Counted when the string was made (count_characters above), so this is
    // a read. 14.18 puts length^ beside size^ as a property of the value,
    // and a property that walked the bytes on every read would not be one.
    return string == NULL ? 0 : string->characters;
}

size_t lhat_string_byte_at(const LhatString *string, size_t character)
{
    // 02 の 14.19: where the nth character begins, counting from 0 -- the
    // one place a run of characters has to be turned back into bytes.
    // Walked rather than stored: 14.19 is a call, so the work is where the
    // parentheses are, and a table of offsets per string would cost every
    // string for the few that are cut.
    if (string == NULL) {
        return 0;
    }
    if (character >= string->characters) {
        return string->length;  // one past the last, which is the end
    }
    size_t seen = 0;
    for (size_t i = 0; i < string->length; i++) {
        if (((unsigned char)string->text[i] & 0xC0) != 0x80) {
            if (seen == character) {
                return i;
            }
            seen++;
        }
    }
    return string->length;
}
