// L^ (lhat) -- types: arena, construction, conformance and disjointness.

#include "type.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/object.h"  // 05 の 8.9: LhatHostValueTag, for naming a host value
#include "lhat/port.h"
#include "lhat/config.h"

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

LhatType *lhat_type_hostvalue(LhatTypeArena *arena,
                              const struct LhatHostValueTag *tag)
{
    LhatType *type = new_type(arena, LHAT_TYPE_HOSTVALUE);
    if (type != NULL) {
        // 05 の 8.9: the payload shares v.table so member registration and
        // lookup read it unchanged; nominal is set for the readers that key
        // off it rather than off the kind.
        type->v.table.nominal = true;
        type->v.table.from_definition = true;
        type->v.table.hostvalue_tag = tag;
    }
    return type;
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
                         LhatType *produce, LhatType *result,
                         bool is_function)
{
    LhatType *type = new_type(arena, LHAT_TYPE_CORO);
    if (type != NULL) {
        type->v.coroutine.receive = receive;
        type->v.coroutine.produce = produce;
        type->v.coroutine.result = result;
        type->v.coroutine.is_function = is_function;
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

LhatType *lhat_type_tuple(LhatTypeArena *arena)
{
    return new_type(arena, LHAT_TYPE_TUPLE);
}

bool lhat_type_add_position(LhatTypeArena *arena, LhatType *tuple,
                            LhatType *position)
{
    if (tuple == NULL) {
        return false;
    }
    LhatTypeList *link = (LhatTypeList *)arena_alloc(arena, sizeof *link);
    if (link == NULL) {
        return false;
    }
    link->type = position;

    // Appended, never merged: two positions of the same type are two
    // positions. lhat_type_union's arm folding would lose one.
    LhatTypeList **slot = &tuple->v.composite.arms;
    while (*slot != NULL) {
        slot = &(*slot)->next;
    }
    *slot = link;
    return true;
}

size_t lhat_type_tuple_width(const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_TUPLE) {
        return 0;
    }
    size_t width = 0;
    for (const LhatTypeList *p = type->v.composite.arms; p != NULL; p = p->next) {
        width++;
    }
    return width;
}

size_t lhat_type_tuple_arm_width(const LhatType *type)
{
    if (type == NULL) {
        return 0;
    }
    if (type->kind == LHAT_TYPE_TUPLE) {
        return lhat_type_tuple_width(type);
    }
    if (type->kind == LHAT_TYPE_UNION) {
        // 02 の 13.8改: at most one arm can be a tuple -- two could not be
        // told apart by the head slot -- so the first found is the answer.
        for (const LhatTypeList *p = type->v.composite.arms; p != NULL;
             p = p->next) {
            size_t width = lhat_type_tuple_arm_width(p->type);
            if (width > 0) {
                return width;
            }
        }
    }
    return 0;
}

LhatType *lhat_type_tuple_at(const LhatType *type, size_t index)
{
    if (type == NULL || type->kind != LHAT_TYPE_TUPLE) {
        return NULL;
    }
    const LhatTypeList *p = type->v.composite.arms;
    for (size_t i = 0; i < index && p != NULL; i++) {
        p = p->next;
    }
    return p != NULL ? p->type : NULL;
}

// 15.5: a call does not run a yielding body -- it makes the coroutine 13.9
// describes, and that is what the caller is handed. Assembled by infer_func,
// where the three slots settle; NULL until then and on every signature that
// does not yield, where the written result is the whole answer.
LhatType *lhat_type_call_answer(const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    return type->v.func.answers != NULL ? type->v.func.answers
                                        : type->v.func.result;
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

// 13.13: a written Self^ makes a type hold itself, so a walk over two of them
// has to be given a way to end. The pairs already being asked about are kept
// on the C stack, and meeting one again answers yes.
//
// That is not a shortcut but the rule for recursive structures: a value fails
// to conform only if some *finite* path through it disagrees, and a finite
// path would have been walked before the question came back around. Assuming
// yes and looking for a disagreement elsewhere is what decides the rest. The
// same convention rt_from_checked(vm.c) uses to end its own walk.
typedef struct Assumed {
    const LhatType *value;
    const LhatType *target;
    const struct Assumed *outer;
} Assumed;

static bool conforms_in(const LhatType *value, const LhatType *target,
                        const Assumed *seen);

static bool conforms_func(const LhatType *value, const LhatType *target,
                          const Assumed *seen)
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
    // 11.3改: and which operand the receiver is decides which way round
    // the operator may be written, so the two are not each other.
    if (value->v.func.self_last != target->v.func.self_last) {
        return false;
    }
    // 15.13: a promise, so it goes only one way. A body that names nothing
    // outside itself stands wherever an ordinary one is written; an ordinary
    // one where a closed^ is asked for would be a promise nobody made.
    if (target->v.func.closed && !value->v.func.closed) {
        return false;
    }

    const LhatTypeList *a = value->v.func.params;
    const LhatTypeList *b = target->v.func.params;
    while (a != NULL && b != NULL) {
        if (!conforms_in(b->type, a->type, seen)) {  // contravariant
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
        !conforms_in(target->v.func.variadic, value->v.func.variadic, seen)) {
        return false;
    }

    // 13.2: no result at all is not the same as returning something.
    //
    // 15.5: and what a call answers is the coroutine where the body yields
    // (13.9), not 13.9's T -- so a signature written 'p^number^;' does not
    // take one that makes a coroutine, and one written with the c^{ … } does.
    // Reading `result` here would compare the two on what their bodies return,
    // which is not what either caller receives.
    LhatType *from = lhat_type_call_answer(value);
    LhatType *into = lhat_type_call_answer(target);
    if ((from == NULL) != (into == NULL)) {
        return false;
    }
    if (from != NULL && !conforms_in(from, into, seen)) {
        return false;
    }
    return true;
}

static bool conforms_in(const LhatType *value, const LhatType *target,
                        const Assumed *seen)
{
    if (value == NULL || target == NULL) {
        return true;  // nothing was inferred here; do not cascade
    }

    // 13.13: this pair is already open further up the walk, so a Self^ has
    // brought the question back to itself.
    for (const Assumed *s = seen; s != NULL; s = s->outer) {
        if (s->value == value && s->target == target) {
            return true;
        }
    }
    Assumed here = { value, target, seen };
    seen = &here;

    // A gap in inference is not a mismatch. Under relaxed it becomes a runtime
    // check (03 の 3.5); under strict, lhat_type_conforms_strict is the one
    // that refuses pending^ -- this function stays lenient for every caller
    // that has not asked for that.
    if (value->kind == LHAT_TYPE_UNKNOWN || target->kind == LHAT_TYPE_UNKNOWN ||
        value->kind == LHAT_TYPE_PENDING || target->kind == LHAT_TYPE_PENDING) {
        return true;
    }

    // 13.2: nothing inhabits "no value", so it fits nowhere a value is wanted
    // -- not even any^, which is the top of every *value* (13.7). And nothing
    // fits where no value is wanted either.
    if (value->kind == LHAT_TYPE_NONE || target->kind == LHAT_TYPE_NONE) {
        return value->kind == target->kind;
    }

    // 05 の 8.9: a host value fits exactly its own type and nothing wider.
    // Checked before any^'s shortcut on purpose: any^ is the top of every
    // value that can live anywhere a value lives, and a host value cannot
    // leave its frame -- letting it under any^ (or into a union, checked
    // below by falling out of this early return) would be the first step of
    // every escape the kind exists to refuse. Identity is the tag, for
    // 8.8's reason sharpened by value semantics.
    if (value->kind == LHAT_TYPE_HOSTVALUE ||
        target->kind == LHAT_TYPE_HOSTVALUE) {
        return value->kind == target->kind &&
               value->v.table.hostvalue_tag == target->v.table.hostvalue_tag;
    }

    // 13.7: any^ is the top of every value, not just of tables.
    if (target->kind == LHAT_TYPE_ANY) {
        // 13.8改: except a tuple. any^ is the top of every value a name can
        // hold, and a tuple is not one -- it lives in the slots a caller
        // reserved and is taken apart there. Refusing it here is what confines
        // it to a result position, the same way the HOSTVALUE arm above
        // confines a host value to its frame. The one place a tuple still
        // reaches past itself is a union with an error kind (04 の 3.1),
        // which the UNION arms below take, since this only refuses any^.
        return value->kind != LHAT_TYPE_TUPLE;
    }

    // Every arm has to fit, since the value may be any of them. This is what
    // makes 04 の 8.1 work: a number^|error^ cannot stand where number^ is.
    if (value->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = value->v.composite.arms; arm != NULL;
             arm = arm->next) {
            // 03 の 7 章、P6: an arm left pending^ here is not the gap the
            // leniency above forgives -- it is a mutually recursive call
            // whose own inference had not run yet, so this union was never
            // actually checked against anything. Letting it through the way
            // a bare pending^ passes everything would hide the very
            // mismatch this function exists to catch. unknown^ arms (table
            // subtyping's silence and the like) are not this -- they stay
            // forgiven and fall through to the ordinary per-arm check below.
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_PENDING) {
                return false;
            }
            if (!conforms_in(arm->type, target, seen)) {
                return false;
            }
        }
        return true;
    }

    // An intersection has every structure, so fitting through one is enough.
    if (value->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = value->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (conforms_in(arm->type, target, seen)) {
                return true;
            }
        }
        // Fall through: a table may still satisfy a structure by combining
        // members drawn from several arms, which the loop above cannot see.
    }

    if (target->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = target->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (conforms_in(value, arm->type, seen)) {
                return true;
            }
        }
        return false;
    }

    if (target->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = target->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (!conforms_in(value, arm->type, seen)) {
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
            // 14.7改: what a definition's instances carry is part of what the
            // definition is, so a written self^{ … } is asked of them the way
            // the members are -- and only a definition has any to ask.
            if (target->v.table.instance != NULL &&
                (value->v.table.instance == NULL ||
                 !conforms_in(value->v.table.instance,
                              target->v.table.instance, seen))) {
                return false;
            }
            // 14.10: at least the listed members. Extra members are fine,
            // which is what lets a value with many members satisfy a small
            // structure at all.
            for (const LhatTypeMember *want = target->v.table.members;
                 want != NULL; want = want->next) {
                const LhatTypeMember *have =
                    find_member(value->v.table.members, want);
                if (have == NULL ||
                    !conforms_in(have->type, want->type, seen)) {
                    return false;
                }
            }
            // 13.7, 14.10: an unbounded tail, checked by walking the
            // positions after the named ones the way 14.10 counts them --
            // from 1, since a variadic type here is not written mixed with
            // fixed positions in practice. Stops at the first position
            // value does not have; 13.7 asks for zero or more, not a count.
            if (target->v.table.variadic != NULL) {
                for (size_t i = 1;; i++) {
                    char digits[24];
                    size_t length = index_digits(i, digits, sizeof digits);
                    LhatTypeMember probe;
                    probe.name = digits;
                    probe.name_length = length;
                    const LhatTypeMember *have =
                        find_member(value->v.table.members, &probe);
                    if (have == NULL) {
                        break;
                    }
                    if (!conforms_in(have->type, target->v.table.variadic,
                                     seen)) {
                        return false;
                    }
                }
            }
            return true;

        case LHAT_TYPE_TUPLE: {
            // 13.8改: the width is exact. 14.10's width subtyping is a rule
            // about tables and does not reach here -- every position of a
            // tuple is a slot the caller reserved, so one more or one fewer
            // is a different call, not a wider value.
            const LhatTypeList *have = value->v.composite.arms;
            const LhatTypeList *want = target->v.composite.arms;
            while (have != NULL && want != NULL) {
                if (!conforms_in(have->type, want->type, seen)) {
                    return false;
                }
                have = have->next;
                want = want->next;
            }
            return have == NULL && want == NULL;
        }

        case LHAT_TYPE_FUNC:
            return conforms_func(value, target, seen);

        case LHAT_TYPE_CORO:
            // 15.3改: advancing one runs its body, so start()/resume() carry
            // the body's kind (15.6改). One kind cannot stand where the other
            // is written -- an f^ holding a p^ coroutine could not advance it,
            // and a p^ one is not subject to 15.3改's containment either.
            if (value->v.coroutine.is_function !=
                target->v.coroutine.is_function) {
                return false;
            }
            // 13.9. What the coroutine receives is an input, so it varies the
            // other way round from what it produces and returns.
            return conforms_in(target->v.coroutine.receive,
                               value->v.coroutine.receive, seen) &&
                   conforms_in(value->v.coroutine.produce,
                               target->v.coroutine.produce, seen) &&
                   conforms_in(value->v.coroutine.result,
                               target->v.coroutine.result, seen);

        default:
            return true;  // the primitives, matched by kind above
    }
}

bool lhat_type_conforms(const LhatType *value, const LhatType *target)
{
    return conforms_in(value, target, NULL);
}

// 03 の 3.1: how deep to look for a gap. A type built out of unions and
// tuples this far down is past anything a diagnostic could point at usefully,
// and the bound is what keeps a type holding itself from being walked forever
// -- the same depth chk_mentions_function_coroutine stops at.
#define GAP_MAX_DEPTH 8

static bool has_gap_in(const LhatType *type, unsigned depth)
{
    if (type == NULL || depth > GAP_MAX_DEPTH) {
        return false;
    }
    if (type->kind == LHAT_TYPE_PENDING) {
        return true;
    }
    // A union promising 'bool^|pending^' promises its callers a type it does
    // not have; a tuple with a gap in one position is the same thing said of
    // one of several values. Nothing else is descended into -- see the header.
    if (type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (has_gap_in(arm->type, depth + 1)) {
                return true;
            }
        }
        return false;
    }
    if (type->kind == LHAT_TYPE_TUPLE) {
        for (const LhatTypeList *at = type->v.composite.arms; at != NULL;
             at = at->next) {
            if (has_gap_in(at->type, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

LhatType *lhat_type_without_gaps(LhatTypeArena *arena, LhatType *type)
{
    if (type == NULL) {
        return NULL;
    }
    // A subroutine is what a mutually recursive name usually holds, and the
    // gap is in what it answers rather than in the name's own type. Written
    // through the signature the last walk built: the walk about to run builds
    // its own from the body, so what is changed here is only ever the seed.
    if (type->kind == LHAT_TYPE_FUNC) {
        type->v.func.result =
            lhat_type_without_gaps(arena, type->v.func.result);
        return type;
    }
    if (type->kind != LHAT_TYPE_UNION) {
        return type;  // nothing to take an arm out of
    }
    LhatType *kept = NULL;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (has_gap_in(arm->type, 0)) {
            continue;
        }
        kept = kept == NULL ? arm->type : lhat_type_union(arena, kept, arm->type);
    }
    // Every arm was a gap, so there is nothing this says about the value.
    // Answering with what was already there keeps the walk from narrowing on
    // the strength of knowing nothing.
    return kept != NULL ? kept : type;
}

bool lhat_type_has_gap(const LhatType *type)
{
    return has_gap_in(type, 0);
}

bool lhat_type_conforms_strict(const LhatType *value, const LhatType *target)
{
    // NULL means nothing was inferred (a cascade to avoid, not a gap to
    // report) -- lhat_type_conforms already treats it that way, and strict
    // has no argument against that reading of it.
    if (value == NULL || target == NULL) {
        return true;
    }
    // 13.2: NONE is judged by kind alone -- neither any^ nor a gap forgives
    // it, so it has to be settled before either check below runs.
    if (value->kind == LHAT_TYPE_NONE || target->kind == LHAT_TYPE_NONE) {
        return lhat_type_conforms(value, target);
    }
    // 13.7: any^ is the top of every value, pending^ included -- a value
    // whose own type is still unresolved still fits where anything does.
    // 03 の 3.1・3.5: a *target* left pending^ is a constraint that was
    // never written -- an omitted parameter or return annotation, a
    // binding a multiple-assignment left short -- not a gap strict has any
    // business closing. That is a different question from whether the
    // *value* itself is pending^, which the check below still asks.
    if (target->kind == LHAT_TYPE_ANY || target->kind == LHAT_TYPE_PENDING) {
        return true;
    }
    // 03 の 3.1・3.5、P6: a value with a gap in it -- inference that had
    // somewhere left to run but did not -- is exactly what strict exists to
    // report, unlike unknown^ (lhat_type_conforms's leniency for that one
    // stands even here). Buried in a union as readily as on its own: what a
    // mutually recursive pair answers is 'bool^|pending^', and a value whose
    // type is partly undecided is undecided where a concrete one is wanted.
    if (lhat_type_has_gap(value)) {
        return false;
    }
    return lhat_type_conforms(value, target);
}

bool lhat_type_equal(const LhatType *a, const LhatType *b)
{
    // 03 の 7 章、P6: lhat_type_conforms treats unknown^ and pending^ as
    // fitting anywhere -- a gap in inference, not a claim about sameness.
    // Two-way conforms would call either one "equal" to whatever it is
    // compared against, which is what let append_arms (build_composite)
    // mistake a gap arm for a duplicate of an arm already in a union and
    // drop it silently. The two kinds of gap are also not each other --
    // "nothing known" and "not checked yet" collapsing together would
    // reintroduce the same silent drop one level up.
    bool a_gap = a != NULL &&
                 (a->kind == LHAT_TYPE_UNKNOWN || a->kind == LHAT_TYPE_PENDING);
    bool b_gap = b != NULL &&
                 (b->kind == LHAT_TYPE_UNKNOWN || b->kind == LHAT_TYPE_PENDING);
    if (a_gap != b_gap) {
        return false;
    }
    if (a_gap && b_gap && a->kind != b->kind) {
        return false;
    }
    return lhat_type_conforms(a, b) && lhat_type_conforms(b, a);
}

// ---------------------------------------------------------------------------
// Disjointness (14.12)
// ---------------------------------------------------------------------------

static bool disjoint_in(const LhatType *a, const LhatType *b,
                        const Assumed *seen)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    // 13.13: the question has come back to itself through a Self^. Nothing on
    // this path tells the two apart, which is what false says here -- the
    // dual of the assumption conforms_in makes, and for the same reason: only
    // a finite path can be a witness, and one would already have been walked.
    for (const Assumed *s = seen; s != NULL; s = s->outer) {
        if (s->value == a && s->target == b) {
            return false;
        }
    }
    Assumed here = { a, b, seen };
    seen = &here;

    // Neither a gap in inference nor the top rules anything out.
    if (a->kind == LHAT_TYPE_UNKNOWN || b->kind == LHAT_TYPE_UNKNOWN ||
        a->kind == LHAT_TYPE_PENDING || b->kind == LHAT_TYPE_PENDING ||
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
            if (!disjoint_in(arm->type, b, seen)) {
                return false;
            }
        }
        return true;
    }
    if (b->kind == LHAT_TYPE_UNION) {
        return disjoint_in(b, a, seen);
    }

    // An intersection has to satisfy every arm, so one separate arm is enough
    // to leave nothing inhabiting both.
    if (a->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = a->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (disjoint_in(arm->type, b, seen)) {
                return true;
            }
        }
        return false;
    }
    if (b->kind == LHAT_TYPE_INTERSECT) {
        return disjoint_in(b, a, seen);
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

    // 05 の 8.9: identity is the tag, so two host value types overlap exactly
    // when they are the same registration. There is no structure to combine
    // the way two tables below might.
    if (a->kind == LHAT_TYPE_HOSTVALUE) {
        return a->v.table.hostvalue_tag != b->v.table.hostvalue_tag;
    }

    // 13.8改: two tuples overlap only if they have the same positions and
    // something inhabits every pair of them. Differing widths leave nothing in
    // both, unlike two tables, which may always carry each other's members.
    if (a->kind == LHAT_TYPE_TUPLE) {
        const LhatTypeList *left = a->v.composite.arms;
        const LhatTypeList *right = b->v.composite.arms;
        while (left != NULL && right != NULL) {
            if (disjoint_in(left->type, right->type, seen)) {
                return true;
            }
            left = left->next;
            right = right->next;
        }
        return left != NULL || right != NULL;
    }

    if (a->kind == LHAT_TYPE_TABLE) {
        // Two structures are separate only when a name they share is declared
        // with types that nothing satisfies at once. Sharing no name at all
        // leaves them overlapping, since a value may carry both sets of
        // members -- which is why 14.12 forbids overloading on shape.
        for (const LhatTypeMember *m = a->v.table.members; m != NULL;
             m = m->next) {
            const LhatTypeMember *other = find_member(b->v.table.members, m);
            if (other != NULL && disjoint_in(m->type, other->type, seen)) {
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

bool lhat_type_disjoint(const LhatType *a, const LhatType *b)
{
    return disjoint_in(a, b, NULL);
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
    // 13.8改: two tuples of the same width fold position by position --
    // '(A, B)|(C, D)' is '(A|C, B|D)'. The answer is two values either way,
    // and each position is one side's or the other's; leaving the union
    // outside would say the answer is a run OR a run, which is not a shape
    // anything can reserve slots for.
    //
    // This is what lets 'f() catch^ (0, 1)' and 'f() ?? (nil^, nil^)' be
    // taken apart. Where the two sides are the same type the arm-folding
    // below already collapsed them, which is why the first tuple literals
    // worked without this -- widths that agree but positions that do not
    // were the case it missed.
    size_t width = lhat_type_tuple_width(a);
    if (width > 0 && width == lhat_type_tuple_width(b)) {
        LhatType *folded = lhat_type_tuple(arena);
        for (size_t i = 0; i < width; i++) {
            lhat_type_add_position(
                arena, folded,
                build_composite(arena, LHAT_TYPE_UNION,
                                lhat_type_tuple_at(a, i),
                                lhat_type_tuple_at(b, i)));
        }
        return folded;
    }
    return build_composite(arena, LHAT_TYPE_UNION, a, b);
}

LhatType *lhat_type_intersect(LhatTypeArena *arena, LhatType *a, LhatType *b)
{
    return build_composite(arena, LHAT_TYPE_INTERSECT, a, b);
}

// ---------------------------------------------------------------------------
// Writing a type out (07 の 4 章)
// ---------------------------------------------------------------------------

// Keeps writing past the end of the buffer without touching it, so the caller
// learns how long the whole thing was -- and so nothing has to check the room
// left at every step.
// 13.13: the structures this walk is already inside, innermost first. A type
// that holds itself is reached again through one of them, and what stands
// there in the source is a Self^ -- so that is what is written, with a second
// hat for each further one out, the way the annotation counts them.
typedef struct WriteSeen {
    const LhatType *type;
    const struct WriteSeen *outer;
} WriteSeen;

typedef struct {
    char *at;
    size_t left;
    size_t written;
    // Pushed and popped around a structure as write_type descends into it.
    const WriteSeen *seen;
} TypeSink;

static void put(TypeSink *sink, const char *text, size_t length)
{
    size_t room = length < sink->left ? length : sink->left;
    if (room > 0) {
        memcpy(sink->at, text, room);
        sink->at += room;
        sink->left -= room;
    }
    sink->written += length;
}

static void put_text(TypeSink *sink, const char *text)
{
    put(sink, text, strlen(text));
}

static void write_type(TypeSink *sink, const LhatType *type, int depth);

static void write_members(TypeSink *sink, const LhatTypeMember *members,
                          int depth)
{
    int count = 0;
    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (count == LHAT_TYPE_WRITE_MAX_ITEMS) {
            put_text(sink, ", …");
            return;
        }
        if (count > 0) {
            put_text(sink, ", ");
        }
        put(sink, m->name, m->name_length);
        put_text(sink, " : ");
        write_type(sink, m->type, depth + 1);
        count++;
    }
}

// A definition's own members: the ones the self^{ … } section did not already
// show. An instance's member is reachable through the definition too (14.4's
// `let^ f = A.m`), so writing it twice would only say the same thing again.
// Each is written after a ", " of its own, since a section came before.
static void write_own_members(TypeSink *sink, const LhatType *definition,
                              int depth)
{
    const LhatType *held = definition->v.table.instance;
    int count = 0;
    for (const LhatTypeMember *m = definition->v.table.members; m != NULL;
         m = m->next) {
        if (find_member(held->v.table.members, m) != NULL) {
            continue;
        }
        if (count == LHAT_TYPE_WRITE_MAX_ITEMS) {
            put_text(sink, ", …");
            return;
        }
        put_text(sink, ", ");
        put(sink, m->name, m->name_length);
        put_text(sink, " : ");
        write_type(sink, m->type, depth + 1);
        count++;
    }
}

static void write_list(TypeSink *sink, const LhatTypeList *list, int depth,
                       const char *separator)
{
    int count = 0;
    for (const LhatTypeList *item = list; item != NULL; item = item->next) {
        if (count == LHAT_TYPE_WRITE_MAX_ITEMS) {
            put_text(sink, separator);
            put_text(sink, "…");
            return;
        }
        if (count > 0) {
            put_text(sink, separator);
        }
        write_type(sink, item->type, depth + 1);
        count++;
    }
}

static void write_type(TypeSink *sink, const LhatType *type, int depth)
{
    if (type == NULL) {
        put_text(sink, "?");
        return;
    }
    // 14 章 lets a table hold itself, so the walk needs a floor of its own.
    if (depth > LHAT_TYPE_WRITE_MAX_DEPTH) {
        put_text(sink, "…");
        return;
    }

    switch (type->kind) {
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
            // Inference has not decided. Saying so is better than naming a
            // type the checker never settled on.
            put_text(sink, "?");
            return;
        case LHAT_TYPE_NONE:
            // 13.2: nothing is produced, which is not nil^ (03 の 3.4).
            put_text(sink, "no value");
            return;
        case LHAT_TYPE_ANY:    put_text(sink, "any^"); return;
        case LHAT_TYPE_NIL:    put_text(sink, "nil^"); return;
        case LHAT_TYPE_BOOL:   put_text(sink, "bool^"); return;
        case LHAT_TYPE_NUMBER: put_text(sink, "number^"); return;
        case LHAT_TYPE_STRING: put_text(sink, "string^"); return;
        case LHAT_TYPE_ERROR:  put_text(sink, "error^"); return;

        case LHAT_TYPE_TABLE: {
            // 13.13: already inside this one, so the source said Self^ here.
            unsigned level = 1;
            for (const WriteSeen *s = sink->seen; s != NULL; s = s->outer) {
                if (s->type == type) {
                    put_text(sink, "Self");
                    for (unsigned i = 0; i < level; i++) {
                        put_text(sink, "^");
                    }
                    return;
                }
                level++;
            }

            // 14.10: bare t^ asks for nothing in particular.
            if (type->v.table.members == NULL &&
                type->v.table.variadic == NULL &&
                type->v.table.instance == NULL) {
                put_text(sink, "t^");
                return;
            }
            WriteSeen here = { type, sink->seen };
            sink->seen = &here;
            // 13.13: inside a definition Self^ is the instance, so that is
            // what the innermost link names here -- and the definition itself
            // becomes Self^^, one structure further out.
            WriteSeen within = { type->v.table.instance, sink->seen };
            if (type->v.table.instance != NULL) {
                sink->seen = &within;
            }
            put_text(sink, "t^{ ");
            // 14.7改: a definition says what its instances carry in the same
            // self^{ … } section it was written with. Its own members follow
            // -- new and the static ones. What the section already showed is
            // not written twice: an instance's member is reachable through
            // the definition (14.4's 'let^ f = A.m'), which is what the
            // section is saying.
            if (type->v.table.instance != NULL) {
                const LhatType *held = type->v.table.instance;
                put_text(sink, "self^{ ");
                write_members(sink, held->v.table.members, depth + 1);
                put_text(sink, " }");
                write_own_members(sink, type, depth);
            } else {
                write_members(sink, type->v.table.members, depth);
            }
            if (type->v.table.variadic != NULL) {
                // 14.10: the sequence half, unbounded.
                if (type->v.table.members != NULL) {
                    put_text(sink, ", ");
                }
                put_text(sink, "...:");
                write_type(sink, type->v.table.variadic, depth + 1);
            }
            put_text(sink, " }");
            sink->seen = here.outer;
            return;
        }

        case LHAT_TYPE_HOSTVALUE:
            // 05 の 8.9: named the way the host registered it. The members
            // are not written out -- like a hostdata type, what it is is the
            // declaration, not the shape.
            if (type->v.table.hostvalue_tag != NULL) {
                put_text(sink, type->v.table.hostvalue_tag->module);
                put_text(sink, ".");
                put_text(sink, type->v.table.hostvalue_tag->name);
            } else {
                put_text(sink, "hostvalue");
            }
            return;

        case LHAT_TYPE_TUPLE:
            // 13.8改: '(A, B)'. The grouping parentheses 13.1's grammar
            // already had are what leave a one-position form unwritable, so
            // there is no '(T,)' to spell here.
            put_text(sink, "(");
            write_list(sink, type->v.composite.arms, depth, ", ");
            put_text(sink, ")");
            return;

        case LHAT_TYPE_FUNC: {
            // 15.13: the mark stands before the kind, the way it is written.
            if (type->v.func.closed) {
                put_text(sink, "closed^");
            }
            // 13.1's form. 13.2 writes '->' only when something is returned.
            put_text(sink, type->v.func.is_function ? "f^" : "p^");
            // 14.4: in a type the receiver is a parameter, written as the
            // word itself. Saying so is what tells a member apart from a
            // plain subroutine of the same shape -- and 11.3改 has it trail
            // on a binary operator, where it is the right operand.
            bool others = type->v.func.params != NULL ||
                          type->v.func.variadic != NULL;
            if (type->v.func.takes_self && !type->v.func.self_last) {
                put_text(sink, "self^");
                if (others) {
                    put_text(sink, ", ");
                }
            }
            write_list(sink, type->v.func.params, depth, ", ");
            if (type->v.func.variadic != NULL) {
                if (type->v.func.params != NULL) {
                    put_text(sink, ", ");
                }
                put_text(sink, "...:");
                write_type(sink, type->v.func.variadic, depth + 1);
            }
            if (type->v.func.takes_self && type->v.func.self_last) {
                if (others) {
                    put_text(sink, ", ");
                }
                put_text(sink, "self^");
            }
            // 15.5: what a call answers, which for a yielding body is the
            // coroutine it makes (13.9) rather than what the body returns.
            // 05 の 8.7 has this read back as a type expression, and a caller
            // reading it wants to know what arrives.
            LhatType *answer = lhat_type_call_answer(type);
            if (answer != NULL && answer->kind != LHAT_TYPE_NONE) {
                put_text(sink, " -> ");
                write_type(sink, answer, depth + 1);
            }
            put_text(sink, ";");
            return;
        }

        case LHAT_TYPE_CORO:
            // 13.9 with 15.3改: 'c^{ f^R -> Y;, T }'.
            put_text(sink, "c^{ ");
            put_text(sink, type->v.coroutine.is_function ? "f^" : "p^");
            if (type->v.coroutine.receive != NULL) {
                write_type(sink, type->v.coroutine.receive, depth + 1);
            }
            if (type->v.coroutine.produce != NULL) {
                put_text(sink, " -> ");
                write_type(sink, type->v.coroutine.produce, depth + 1);
            }
            put_text(sink, ";, ");
            write_type(sink, type->v.coroutine.result, depth + 1);
            put_text(sink, " }");
            return;

        case LHAT_TYPE_ERROR_SET:
            put(sink, type->v.error.name, type->v.error.name_length);
            return;

        case LHAT_TYPE_ERROR_KIND:
            // 04 の 2.4: a kind is named through the set that declared it.
            if (type->v.error.set != NULL) {
                put(sink, type->v.error.set->v.error.name,
                    type->v.error.set->v.error.name_length);
                put_text(sink, ".");
            }
            put(sink, type->v.error.name, type->v.error.name_length);
            return;

        case LHAT_TYPE_UNION:
            write_list(sink, type->v.composite.arms, depth, " | ");
            return;
        case LHAT_TYPE_INTERSECT:
            write_list(sink, type->v.composite.arms, depth, " & ");
            return;

        case LHAT_TYPE_KIND_COUNT:
            break;
    }
    put_text(sink, "?");
}

size_t lhat_type_write(const LhatType *type, char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) {
        return 0;
    }
    // One byte held back for the terminator, so `left` running out and the
    // string being closed are the same condition.
    TypeSink sink = {buffer, size - 1, 0, NULL};
    write_type(&sink, type, 0);
    *sink.at = '\0';

    if (sink.written > size - 1) {
        // Cut. Say so rather than leaving a type that reads as complete.
        size_t room = size - 1;
        const char *mark = "…";  // three bytes in UTF-8
        size_t mark_length = strlen(mark);
        if (room > mark_length) {
            memcpy(buffer + room - mark_length, mark, mark_length);
        }
        return room;
    }
    return sink.written;
}
