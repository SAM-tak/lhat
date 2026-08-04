// L^ (lhat) -- types: arena, construction, conformance and disjointness.

#include "type.h"

#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "lhatconfig.h"

// MSVC does not provide max_align_t in C mode, so the arena carries its own
// worst-case alignment rather than depending on it.
typedef union {
    long double ld;
    void *pointer;
    long long integer;
} LhatTypeAlign;

struct LhatTypeArenaBlock {
    LhatTypeArenaBlock *next;
    size_t used;
    // Kept aligned for anything allocated out of it.
    _Alignas(LhatTypeAlign) unsigned char bytes[LHAT_TYPE_BLOCK_BYTES];
};

void lhat_type_arena_init(LhatTypeArena *arena)
{
    arena->blocks = NULL;
    arena->type_count = 0;
}

void lhat_type_arena_dispose(LhatTypeArena *arena)
{
    LhatTypeArenaBlock *block = arena->blocks;
    while (block != NULL) {
        LhatTypeArenaBlock *next = block->next;
        lhat_free(block);
        block = next;
    }
    arena->blocks = NULL;
    arena->type_count = 0;
}

static void *arena_alloc(LhatTypeArena *arena, size_t size)
{
    size_t aligned = (size + sizeof(LhatTypeAlign) - 1) &
                     ~(sizeof(LhatTypeAlign) - 1);
    if (aligned > LHAT_TYPE_BLOCK_BYTES) {
        return NULL;
    }

    if (arena->blocks == NULL ||
        arena->blocks->used + aligned > LHAT_TYPE_BLOCK_BYTES) {
        LhatTypeArenaBlock *block = (LhatTypeArenaBlock *)lhat_alloc(sizeof *block);
        if (block == NULL) {
            return NULL;
        }
        block->next = arena->blocks;
        block->used = 0;
        arena->blocks = block;
    }

    void *p = arena->blocks->bytes + arena->blocks->used;
    arena->blocks->used += aligned;
    memset(p, 0, aligned);
    return p;
}

static LhatType *new_type(LhatTypeArena *arena, LhatTypeKind kind)
{
    LhatType *type = (LhatType *)arena_alloc(arena, sizeof *type);
    if (type == NULL) {
        return NULL;
    }
    type->kind = kind;
    arena->type_count++;
    return type;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LhatType *lhat_type_simple(LhatTypeArena *arena, LhatTypeKind kind)
{
    return new_type(arena, kind);
}

LhatType *lhat_type_table(LhatTypeArena *arena)
{
    return new_type(arena, LHAT_TYPE_TABLE);
}

LhatType *lhat_type_func(LhatTypeArena *arena, bool is_function)
{
    LhatType *type = new_type(arena, LHAT_TYPE_FUNC);
    if (type != NULL) {
        type->v.func.is_function = is_function;
    }
    return type;
}

LhatType *lhat_type_coro(LhatTypeArena *arena, LhatType *receive,
                         LhatType *produce, LhatType *result)
{
    LhatType *type = new_type(arena, LHAT_TYPE_CORO);
    if (type != NULL) {
        type->v.coroutine.receive = receive;
        type->v.coroutine.produce = produce;
        type->v.coroutine.result = result;
    }
    return type;
}

LhatType *lhat_type_error_set(LhatTypeArena *arena, const char *name,
                              size_t name_length)
{
    LhatType *type = new_type(arena, LHAT_TYPE_ERROR_SET);
    if (type != NULL) {
        type->v.error.name = name;
        type->v.error.name_length = name_length;
    }
    return type;
}

LhatType *lhat_type_error_kind(LhatTypeArena *arena, LhatType *set,
                               const char *name, size_t name_length)
{
    LhatType *type = new_type(arena, LHAT_TYPE_ERROR_KIND);
    if (type == NULL) {
        return NULL;
    }
    type->v.error.set = set;
    type->v.error.name = name;
    type->v.error.name_length = name_length;

    if (set != NULL) {
        LhatTypeList *link = (LhatTypeList *)arena_alloc(arena, sizeof *link);
        if (link != NULL) {
            link->type = type;
            LhatTypeList **slot = &set->v.error.kinds;
            while (*slot != NULL) {
                slot = &(*slot)->next;
            }
            *slot = link;
        }
    }
    return type;
}

LhatTypeMember *lhat_type_add_member(LhatTypeArena *arena, LhatType *owner,
                                     const char *name, size_t name_length,
                                     LhatType *type)
{
    if (owner == NULL) {
        return NULL;
    }
    LhatTypeMember **slot = owner->kind == LHAT_TYPE_ERROR_KIND
                                ? &owner->v.error.fields
                                : &owner->v.table.members;

    LhatTypeMember *member = (LhatTypeMember *)arena_alloc(arena, sizeof *member);
    if (member == NULL) {
        return NULL;
    }
    member->name = name;
    member->name_length = name_length;
    member->type = type;

    while (*slot != NULL) {
        slot = &(*slot)->next;
    }
    *slot = member;
    return member;
}

// The digits of a one-based index, written into the arena so the member can
// borrow them the way an ordinary one borrows the source text.
static size_t index_digits(size_t index, char *out, size_t capacity)
{
    size_t length = 0;
    size_t rest = index;
    do {
        length++;
        rest /= 10;
    } while (rest > 0);
    if (length > capacity) {
        return 0;
    }
    rest = index;
    for (size_t i = length; i > 0; i--) {
        out[i - 1] = (char)('0' + (rest % 10));
        rest /= 10;
    }
    return length;
}

LhatTypeMember *lhat_type_add_index_member(LhatTypeArena *arena,
                                           LhatType *owner, size_t index,
                                           LhatType *type)
{
    char digits[24];
    size_t length = index_digits(index, digits, sizeof digits);
    if (length == 0) {
        return NULL;
    }
    char *name = (char *)arena_alloc(arena, length);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, digits, length);
    return lhat_type_add_member(arena, owner, name, length, type);
}

const LhatTypeMember *lhat_type_member_at(const LhatType *table, size_t index)
{
    if (table == NULL || table->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    char digits[24];
    size_t length = index_digits(index, digits, sizeof digits);
    if (length == 0) {
        return NULL;
    }
    for (const LhatTypeMember *m = table->v.table.members; m != NULL;
         m = m->next) {
        if (m->name_length == length && memcmp(m->name, digits, length) == 0) {
            return m;
        }
    }
    return NULL;
}

bool lhat_type_add_param(LhatTypeArena *arena, LhatType *func, LhatType *param)
{
    if (func == NULL) {
        return false;
    }
    LhatTypeList *link = (LhatTypeList *)arena_alloc(arena, sizeof *link);
    if (link == NULL) {
        return false;
    }
    link->type = param;

    LhatTypeList **slot = &func->v.func.params;
    while (*slot != NULL) {
        slot = &(*slot)->next;
    }
    *slot = link;
    return true;
}

// ---------------------------------------------------------------------------
// Conformance (13.11)
// ---------------------------------------------------------------------------

static bool member_names_equal(const LhatTypeMember *a, const LhatTypeMember *b)
{
    return a->name_length == b->name_length &&
           memcmp(a->name, b->name, a->name_length) == 0;
}

static const LhatTypeMember *find_member(const LhatTypeMember *members,
                                         const LhatTypeMember *wanted)
{
    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (member_names_equal(m, wanted)) {
            return m;
        }
    }
    return NULL;
}

// The root of an error kind's identity: the declaration it came from.
static const LhatType *error_set_of(const LhatType *type)
{
    if (type->kind == LHAT_TYPE_ERROR_KIND) {
        return type->v.error.set;
    }
    if (type->kind == LHAT_TYPE_ERROR_SET) {
        return type;
    }
    return NULL;
}

static bool is_error_type(const LhatType *type)
{
    return type->kind == LHAT_TYPE_ERROR ||
           type->kind == LHAT_TYPE_ERROR_SET ||
           type->kind == LHAT_TYPE_ERROR_KIND;
}

static bool conforms_func(const LhatType *value, const LhatType *target)
{
    // 14.12 already states the rule for override^, and it is the ordinary one
    // for functions: arguments may be wider, results may be narrower.
    if (value->v.func.is_function != target->v.func.is_function) {
        return false;
    }
    // 14.4: an instance method and a plain subroutine are called differently,
    // so one cannot stand where the other is written.
    if (value->v.func.takes_self != target->v.func.takes_self) {
        return false;
    }

    const LhatTypeList *a = value->v.func.params;
    const LhatTypeList *b = target->v.func.params;
    while (a != NULL && b != NULL) {
        if (!lhat_type_conforms(b->type, a->type)) {  // contravariant
            return false;
        }
        a = a->next;
        b = b->next;
    }
    if (a != NULL || b != NULL) {
        return false;
    }

    // 13.7: a trailing '...' is part of how the signature may be called, so
    // one that has it cannot stand for one that does not, or the other way.
    if ((value->v.func.variadic == NULL) != (target->v.func.variadic == NULL)) {
        return false;
    }
    if (value->v.func.variadic != NULL &&
        !lhat_type_conforms(target->v.func.variadic, value->v.func.variadic)) {
        return false;
    }

    // 13.2: no result at all is not the same as returning something.
    if ((value->v.func.result == NULL) != (target->v.func.result == NULL)) {
        return false;
    }
    if (value->v.func.result != NULL &&
        !lhat_type_conforms(value->v.func.result, target->v.func.result)) {
        return false;
    }
    return true;
}

bool lhat_type_conforms(const LhatType *value, const LhatType *target)
{
    if (value == NULL || target == NULL) {
        return true;  // nothing was inferred here; do not cascade
    }

    // A gap in inference is not a mismatch. Under relaxed it becomes a runtime
    // check (03 の 3.5); under strict the failure was already reported where
    // it happened.
    if (value->kind == LHAT_TYPE_UNKNOWN || target->kind == LHAT_TYPE_UNKNOWN) {
        return true;
    }

    // 13.2: nothing inhabits "no value", so it fits nowhere a value is wanted
    // -- not even any^, which is the top of every *value* (13.7). And nothing
    // fits where no value is wanted either.
    if (value->kind == LHAT_TYPE_NONE || target->kind == LHAT_TYPE_NONE) {
        return value->kind == target->kind;
    }

    // 13.7: any^ is the top of every value, not just of tables.
    if (target->kind == LHAT_TYPE_ANY) {
        return true;
    }

    // Every arm has to fit, since the value may be any of them. This is what
    // makes 04 の 8.1 work: a number^|error^ cannot stand where number^ is.
    if (value->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = value->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (!lhat_type_conforms(arm->type, target)) {
                return false;
            }
        }
        return true;
    }

    // An intersection has every structure, so fitting through one is enough.
    if (value->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = value->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (lhat_type_conforms(arm->type, target)) {
                return true;
            }
        }
        // Fall through: a table may still satisfy a structure by combining
        // members drawn from several arms, which the loop above cannot see.
    }

    if (target->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = target->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (lhat_type_conforms(value, arm->type)) {
                return true;
            }
        }
        return false;
    }

    if (target->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = target->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (!lhat_type_conforms(value, arm->type)) {
                return false;
            }
        }
        return true;
    }

    if (value->kind == LHAT_TYPE_INTERSECT) {
        return false;  // the fall-through above found nothing
    }

    // 04 の 2.3: every kind is below error^, and a kind is below its set.
    if (is_error_type(value) && is_error_type(target)) {
        if (target->kind == LHAT_TYPE_ERROR) {
            return true;
        }
        if (value->kind == LHAT_TYPE_ERROR) {
            return false;  // the supertype does not stand for a particular set
        }
        if (target->kind == LHAT_TYPE_ERROR_SET) {
            return error_set_of(value) == target;
        }
        return value == target;  // 2.4: identity is the declaration
    }

    if (value->kind != target->kind) {
        return false;
    }

    switch (target->kind) {
        case LHAT_TYPE_TABLE:
            // 05 の 8.8: a host type is the one thing 11.3 does not judge by
            // shape. Asked for one, only that one will do -- there is nothing
            // to compare structurally, and being wrong means a pointer read
            // as something it is not.
            if (target->v.table.nominal) {
                return value == target;
            }
            // 14.10: at least the listed members. Extra members are fine,
            // which is what lets a value with many members satisfy a small
            // structure at all.
            for (const LhatTypeMember *want = target->v.table.members;
                 want != NULL; want = want->next) {
                const LhatTypeMember *have =
                    find_member(value->v.table.members, want);
                if (have == NULL ||
                    !lhat_type_conforms(have->type, want->type)) {
                    return false;
                }
            }
            return true;

        case LHAT_TYPE_FUNC:
            return conforms_func(value, target);

        case LHAT_TYPE_CORO:
            // 13.9. What the coroutine receives is an input, so it varies the
            // other way round from what it produces and returns.
            return lhat_type_conforms(target->v.coroutine.receive,
                                      value->v.coroutine.receive) &&
                   lhat_type_conforms(value->v.coroutine.produce,
                                      target->v.coroutine.produce) &&
                   lhat_type_conforms(value->v.coroutine.result,
                                      target->v.coroutine.result);

        default:
            return true;  // the primitives, matched by kind above
    }
}

bool lhat_type_equal(const LhatType *a, const LhatType *b)
{
    return lhat_type_conforms(a, b) && lhat_type_conforms(b, a);
}

// ---------------------------------------------------------------------------
// Disjointness (14.12)
// ---------------------------------------------------------------------------

bool lhat_type_disjoint(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    // Neither a gap in inference nor the top rules anything out.
    if (a->kind == LHAT_TYPE_UNKNOWN || b->kind == LHAT_TYPE_UNKNOWN ||
        a->kind == LHAT_TYPE_ANY || b->kind == LHAT_TYPE_ANY) {
        return false;
    }

    // 13.2: nothing inhabits "no value", so nothing inhabits it and something
    // else either. This is what makes 11.7's `??` and 04 の 4.1's catch^
    // report on a call that produces nothing.
    if (a->kind == LHAT_TYPE_NONE || b->kind == LHAT_TYPE_NONE) {
        return true;
    }

    // A union is separate from something only when all of it is.
    if (a->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = a->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (!lhat_type_disjoint(arm->type, b)) {
                return false;
            }
        }
        return true;
    }
    if (b->kind == LHAT_TYPE_UNION) {
        return lhat_type_disjoint(b, a);
    }

    // An intersection has to satisfy every arm, so one separate arm is enough
    // to leave nothing inhabiting both.
    if (a->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = a->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (lhat_type_disjoint(arm->type, b)) {
                return true;
            }
        }
        return false;
    }
    if (b->kind == LHAT_TYPE_INTERSECT) {
        return lhat_type_disjoint(b, a);
    }

    // 04 の 2.6: an error is separate from everything that is not one. Within
    // errors, error^ overlaps them all and two kinds overlap only if they are
    // the same declaration.
    if (is_error_type(a) || is_error_type(b)) {
        if (!is_error_type(a) || !is_error_type(b)) {
            return true;
        }
        if (a->kind == LHAT_TYPE_ERROR || b->kind == LHAT_TYPE_ERROR) {
            return false;
        }
        if (a->kind == LHAT_TYPE_ERROR_SET || b->kind == LHAT_TYPE_ERROR_SET) {
            return error_set_of(a) != error_set_of(b);
        }
        return a != b;
    }

    if (a->kind != b->kind) {
        return true;  // different primitives, or a primitive and a structure
    }

    if (a->kind == LHAT_TYPE_TABLE) {
        // Two structures are separate only when a name they share is declared
        // with types that nothing satisfies at once. Sharing no name at all
        // leaves them overlapping, since a value may carry both sets of
        // members -- which is why 14.12 forbids overloading on shape.
        for (const LhatTypeMember *m = a->v.table.members; m != NULL;
             m = m->next) {
            const LhatTypeMember *other = find_member(b->v.table.members, m);
            if (other != NULL && lhat_type_disjoint(m->type, other->type)) {
                return true;
            }
        }
        return false;
    }

    // A function value has one signature, but a signature accepting a wider
    // argument inhabits both of two narrower ones, so nothing here is decided
    // by the parameter types alone. Treat them as overlapping.
    return false;
}

// ---------------------------------------------------------------------------
// Composites
// ---------------------------------------------------------------------------

static bool list_contains(const LhatTypeList *list, const LhatType *type)
{
    for (const LhatTypeList *link = list; link != NULL; link = link->next) {
        if (lhat_type_equal(link->type, type)) {
            return true;
        }
    }
    return false;
}

static bool append_arms(LhatTypeArena *arena, LhatType *into,
                        LhatTypeKind composite, LhatType *type)
{
    // Flatten, so 'a|b|c' is one list rather than a nest of pairs and the
    // arms can be walked once when narrowing drops some of them.
    if (type != NULL && type->kind == composite) {
        for (LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (!append_arms(arena, into, composite, arm->type)) {
                return false;
            }
        }
        return true;
    }

    if (list_contains(into->v.composite.arms, type)) {
        return true;
    }

    LhatTypeList *link = (LhatTypeList *)arena_alloc(arena, sizeof *link);
    if (link == NULL) {
        return false;
    }
    link->type = type;

    LhatTypeList **slot = &into->v.composite.arms;
    while (*slot != NULL) {
        slot = &(*slot)->next;
    }
    *slot = link;
    return true;
}

static LhatType *build_composite(LhatTypeArena *arena, LhatTypeKind kind,
                                 LhatType *a, LhatType *b)
{
    if (a == NULL) {
        return b;
    }
    if (b == NULL) {
        return a;
    }

    // 13.7: any^ admits every value, so a union with it is it. An
    // intersection with it adds no requirement, so it is the other side.
    if (a->kind == LHAT_TYPE_ANY || b->kind == LHAT_TYPE_ANY) {
        if (kind == LHAT_TYPE_UNION) {
            return a->kind == LHAT_TYPE_ANY ? a : b;
        }
        return a->kind == LHAT_TYPE_ANY ? b : a;
    }

    LhatType *node = new_type(arena, kind);
    if (node == NULL) {
        return a;
    }
    if (!append_arms(arena, node, kind, a) ||
        !append_arms(arena, node, kind, b)) {
        return a;
    }

    // Nothing was added by the second side, so there is no composite left.
    if (node->v.composite.arms != NULL && node->v.composite.arms->next == NULL) {
        return node->v.composite.arms->type;
    }
    return node;
}

LhatType *lhat_type_union(LhatTypeArena *arena, LhatType *a, LhatType *b)
{
    return build_composite(arena, LHAT_TYPE_UNION, a, b);
}

LhatType *lhat_type_intersect(LhatTypeArena *arena, LhatType *a, LhatType *b)
{
    return build_composite(arena, LHAT_TYPE_INTERSECT, a, b);
}

const char *lhat_type_kind_name(LhatTypeKind kind)
{
    switch (kind) {
        case LHAT_TYPE_UNKNOWN:     return "unknown";
        case LHAT_TYPE_NONE:        return "no value";
        case LHAT_TYPE_ANY:         return "any^";
        case LHAT_TYPE_NIL:         return "nil^";
        case LHAT_TYPE_BOOL:        return "bool^";
        case LHAT_TYPE_NUMBER:      return "number^";
        case LHAT_TYPE_STRING:      return "string^";
        case LHAT_TYPE_TABLE:       return "t^{...}";
        case LHAT_TYPE_FUNC:        return "f^";
        case LHAT_TYPE_CORO:        return "c^{...}";
        case LHAT_TYPE_ERROR:       return "error^";
        case LHAT_TYPE_ERROR_SET:   return "error set";
        case LHAT_TYPE_ERROR_KIND:  return "error kind";
        case LHAT_TYPE_UNION:       return "|";
        case LHAT_TYPE_INTERSECT:   return "&";
        case LHAT_TYPE_KIND_COUNT:  break;
    }
    return "?";
}
