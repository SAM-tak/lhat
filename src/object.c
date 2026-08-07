// L^ (lhat) -- the heap values: strings and tables.

#include "object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"

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
    return joined;
}

LhatTable *lhat_table_new(LhatHeap *heap)
{
    return (LhatTable *)lhat_object_alloc(heap, sizeof(LhatTable),
                                          LHAT_OBJECT_TABLE);
}

LhatErrorKind *lhat_error_kind_new(LhatHeap *heap,
                                   const LhatErrorKind *group,
                                   const LhatString *name)
{
    LhatErrorKind *kind = (LhatErrorKind *)lhat_object_alloc(
        heap, sizeof(LhatErrorKind), LHAT_OBJECT_ERROR_KIND);
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
    coroutine->registers =
        (LhatValue *)lhat_calloc(registers ? registers : 1, sizeof *coroutine->registers);
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
        (LhatCoroutine *)lhat_object_alloc(
            heap, sizeof(LhatCoroutine), LHAT_OBJECT_COROUTINE);
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
                        uint8_t parameters, bool takes_self)
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
    host->takes_self = takes_self;
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

LhatHostDataTagRef *lhat_hostdata_tag_ref_new(LhatHeap *heap,
                                              const LhatHostDataTag *tag)
{
    LhatHostDataTagRef *ref = (LhatHostDataTagRef *)lhat_object_alloc(
        heap, sizeof(LhatHostDataTagRef), LHAT_OBJECT_HOSTDATA_TAG_REF);
    if (ref == NULL) {
        return NULL;
    }
    ref->tag = tag;
    return ref;
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
        // 14.5, 14.12: an overload^ed member has to fit every arm at once --
        // it is one value callable every way the intersection lists.
        case LHAT_TYPE_RT_INTERSECT:
            for (size_t i = 0; i < type->part_count; i++) {
                if (!lhat_value_satisfies(value, type->parts[i])) {
                    return false;
                }
            }
            return true;
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
        case LHAT_TYPE_RT_TABLE:
            type_put_text(w, "table^");
            return;
        case LHAT_TYPE_RT_ERROR:
            type_put_text(w, "error^");
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
        // 13.9's three slots. NULL still prints any^ (S28's residual: a
        // coroutine value or a yielding subroutine reflected before this
        // was wired up carries none of the three) -- not a guess, the same
        // "nothing written asks for the top type" convention as everywhere
        // else in this function.
        // 15.3改: the front half is written as the signature it always
        // described -- one resume takes R and answers Y -- which is where the
        // kind of the body goes now that both kinds exist.
        case LHAT_TYPE_RT_COROUTINE:
            type_put_text(w, "c^{");
            type_put_text(w, type->is_function ? "f^" : "p^");
            write_runtime_type(w, type->receive);
            type_put_text(w, " -> ");
            write_runtime_type(w, type->produce);
            type_put_text(w, ";, ");
            write_runtime_type(w, type->result);
            type_put_text(w, "}");
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
                write_runtime_type(w, type->parts[i]);
            }
            return;
        case LHAT_TYPE_RT_SUBROUTINE:
            type_put_text(w, type->is_function ? "f^" : "p^");
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
            if (type->result != NULL) {
                type_put_text(w, " -> ");
                write_runtime_type(w, type->result);
            }
            type_put_text(w, ";");
            return;
        case LHAT_TYPE_RT_STRUCTURE:
            type_put_text(w, "t^{");
            {
                bool first = true;
                // 14 章: the sequence half first, in order, the way a value
                // written down puts it (value.c's write_table) -- then the
                // named half, sorted into a canonical order already (this is
                // read-only from here).
                for (size_t i = 0; i < type->part_count; i++) {
                    type_put_text(w, first ? " " : ", ");
                    first = false;
                    write_runtime_type(w, type->parts[i]);
                }
                for (size_t i = 0; i < type->member_count; i++) {
                    type_put_text(w, first ? " " : ", ");
                    first = false;
                    type_put(w, type->members[i].name->text,
                            type->members[i].name->length);
                    type_put_text(w, " : ");
                    write_runtime_type(w, type->members[i].type);
                }
                type_put_text(w, first ? "}" : " }");
            }
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
        case LHAT_TYPE_RT_TABLE:
        case LHAT_TYPE_RT_ERROR:
        // 03 の 3.4: two undecided places are the same undecided place. Not
        // equal to any^ though -- the kinds differ above, and NULL normalises
        // to any^ rather than to this, which keeps "nobody wrote one" apart
        // from "inference could not say".
        case LHAT_TYPE_RT_UNKNOWN:
            return true;
        // 13.9's three slots now carry real answers (S28), so two coroutine
        // types are equal only when R, Y and T line up -- not just by kind.
        // 15.3改 adds the body's own kind to that: what may be done with an
        // f^ coroutine is not what may be done with a p^ one.
        case LHAT_TYPE_RT_COROUTINE:
            return a->is_function == b->is_function &&
                   lhat_runtime_type_equal(a->receive, b->receive) &&
                   lhat_runtime_type_equal(a->produce, b->produce) &&
                   lhat_runtime_type_equal(a->result, b->result);
        case LHAT_TYPE_RT_ERROR_KIND:
            // 04 の 2.4: identity is the declaration.
            return a->error_kind == b->error_kind;
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
        case LHAT_TYPE_RT_SUBROUTINE:
            if (a->is_function != b->is_function ||
                a->takes_self != b->takes_self ||
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
        case LHAT_TYPE_RT_STRUCTURE:
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
            return true;
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
    if (overload->count == overload->capacity) {
        size_t grown = overload->capacity ? overload->capacity * 2 : 4;
        LhatValue *bigger =
            (LhatValue *)lhat_realloc(overload->candidates, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        overload->candidates = bigger;
        overload->capacity = grown;
    }
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

void lhat_object_free(LhatObject *object)
{
    if (object == NULL) {
        return;
    }
    switch (object->kind) {
        case LHAT_OBJECT_TABLE: {
            LhatTable *table = (LhatTable *)object;
            lhat_free(table->array);
            lhat_free(table->entries);
            break;
        }
        case LHAT_OBJECT_SUBROUTINE:
            lhat_free(((LhatClosure *)object)->upvalues);
            break;
        case LHAT_OBJECT_COROUTINE:
            lhat_free(((LhatCoroutine *)object)->registers);
            break;
        case LHAT_OBJECT_TYPE:
            lhat_free(((LhatRuntimeType *)object)->parts);
            lhat_free(((LhatRuntimeType *)object)->members);
            break;
        case LHAT_OBJECT_OVERLOAD:
            lhat_free(((LhatOverload *)object)->candidates);
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
    LhatValue *array = (LhatValue *)lhat_realloc(table->array, capacity * sizeof *array);
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
