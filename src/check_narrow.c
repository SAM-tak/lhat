// L^ (lhat) -- the type checking stage: narrowing ([syntax.type.fits]).

#include "check_internal.h"
#include "instantiation_internal.h"
#include "rttype.h"

// ---------------------------------------------------------------------------
// Narrowing helpers ([errors.catch.form], [syntax.op.default])
// ---------------------------------------------------------------------------

// [errors.kind.made]: a set stands for the union of its kinds, so a test against one
// kind has something to take apart. Without this, narrowing a value typed as
// IOError by IOError.NotFound would find nothing to remove.
static LhatType *expand_set(Checker *c, LhatType *type)
{
    // [syntax.enum]: an enum stands for the union of its members the same way.
    if (type == NULL || (type->kind != LHAT_TYPE_ERROR_SET &&
                         type->kind != LHAT_TYPE_ENUM)) {
        return type;
    }
    LhatType *expanded = NULL;
    for (const LhatTypeList *k = type->v.error.kinds; k != NULL; k = k->next) {
        expanded = lhat_type_union(c->result->types, expanded, k->type);
    }
    return expanded != NULL ? expanded : type;
}

// The part of `type` that could still be `wanted`. This is the true side of
// [syntax.type.fits]'s narrowing, and [errors.try.result] uses it to name the errors a try^ lets
// past.
//
// An arm wider than the test narrows down to the test: a value typed as the
// whole set, once known to be one kind, is that kind.
LhatType *chk_only(Checker *c, LhatType *type, LhatType *wanted)
{
    if (type == NULL) {
        return NULL;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        LhatType *kept = NULL;
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            kept = lhat_type_union(c->result->types, kept,
                                   chk_only(c, arm->type, wanted));
        }
        return kept;
    }
    if (lhat_type_conforms(type, wanted)) {
        return type;
    }
    if (lhat_type_conforms(wanted, type)) {
        return wanted;
    }
    return NULL;
}

// The complement: what remains once everything that could be `unwanted` is
// gone. The false side of [syntax.type.fits], and what catch^ and ?? do to their left.
LhatType *chk_without(Checker *c, LhatType *type, LhatType *unwanted)
{
    if (type == NULL) {
        return NULL;
    }
    // Inference gaps conform permissively, but that does not prove every
    // possible value belongs to the removed arm. Keep the gap so a fallback
    // cannot supply the entire result type of an unresolved successful path.
    if (type->kind == LHAT_TYPE_UNKNOWN || type->kind == LHAT_TYPE_PENDING) {
        return type;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        LhatType *kept = NULL;
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            kept = lhat_type_union(c->result->types, kept,
                                   chk_without(c, arm->type, unwanted));
        }
        return kept;
    }
    if (lhat_type_conforms(type, unwanted)) {
        return NULL;
    }
    if (lhat_type_disjoint(type, unwanted)) {
        return type;
    }
    // Overlapping but wider. A set can be taken apart; anything else stays
    // whole, since narrowing may only ever remove what it can name.
    if (type->kind == LHAT_TYPE_ERROR_SET) {
        return chk_without(c, expand_set(c, type), unwanted);
    }
    return type;
}

bool chk_can_be(const LhatType *type, const LhatType *wanted)
{
    return type == NULL || !lhat_type_disjoint(type, wanted);
}

// [syntax.type.empty]: nothing inhabits "no value", so it cannot stand where one is wanted.
// The positions that want a value ask here, rather than infer_call reporting
// it -- [syntax.stmt.no-bare-expr] makes a call on its own a statement, and that is exactly where a
// subroutine with no result belongs. Answers unknown once it has reported, so
// the one mistake does not cascade.
LhatType *chk_require_value(Checker *c, const LhatNode *at, LhatType *type)
{
    if (type != NULL && type->kind == LHAT_TYPE_NONE) {
        chk_report(c, at, LHAT_CHECK_ERR_MISMATCH);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    return type;
}

// [syntax.op.define]: an operator is a member whose name is the operator itself. NULL for
// the ones no op^ may define -- [syntax.op.define] keeps and^, or^ and '!' built in, and
// [syntax.op.precedence]'s comparisons decide by [syntax.obj.override]'s disjointness rather than by asking.
// The spellings are token.h's one list, which is what keeps this the same
// string vm.c looks a candidate up by.
const char *chk_operator_name(LhatOpKind op, size_t *length)
{
    switch (op) {
#define LHAT_OPERATOR_CASE(opk, bc, spelling, len) \
    case LHAT_OP_##opk:                            \
        *length = (len);                           \
        return spelling;
        LHAT_OPERATOR_MEMBERS(LHAT_OPERATOR_CASE)
#undef LHAT_OPERATOR_CASE
        default:
            *length = 0;
            return NULL;
    }
}

// [syntax.op.define]: whether this member name is an operator. These spellings are the
// ones op^ writes, and [lex.comment] keeps a program from writing any of them
// as an ordinary name -- so a member of one of these names got there
// through op^ and nowhere else.
bool chk_is_operator_name(const char *name, size_t length)
{
#define LHAT_OPERATOR_TEST(opk, bc, spelling, len)                   \
    if (length == (len) && memcmp(name, spelling, (len)) == 0) {     \
        return true;                                                 \
    }
    LHAT_OPERATOR_MEMBERS(LHAT_OPERATOR_TEST)
#undef LHAT_OPERATOR_TEST
    return false;
}

// [pipeline.infer.expected]: the same list read as a bit each, so "does this unit write an
// op^ of this name" is one test. Zero for a name no operator wears.
uint16_t chk_operator_bit(const char *name, size_t length)
{
    uint16_t at = 0;
#define LHAT_OPERATOR_BIT(opk, bc, spelling, len)                    \
    if (length == (len) && memcmp(name, spelling, (len)) == 0) {     \
        return (uint16_t)(1u << at);                                 \
    }                                                                \
    at++;
    LHAT_OPERATOR_MEMBERS(LHAT_OPERATOR_BIT)
#undef LHAT_OPERATOR_BIT
    (void)at;
    return 0;
}

// [syntax.op.define]: an operator is an f^ that takes self^ and one argument. [syntax.op.functions] makes it
// a function -- a p^ could carry side effects into an operator -- and [syntax.obj.self]
// puts the left operand in self^, which leaves the right one as the single
// parameter. Nothing later checks this: the call site reads the signature and
// believes it, and the machine hands over a receiver and one argument
// whatever the body declared.
// [syntax.op.compare]: and a '<=>' answers a number^, since the four orderings are
// read off it by asking which side of zero the answer falls.
//
// [syntax.op.define]: '-' is the one operator written both ways, so it also takes the
// shape with no argument at all -- that is the unary one. Told apart by the
// count, which is what lets [syntax.obj.override] hold both under the one name. The other
// spellings keep refusing it: there is no unary '..' for it to mean.
void chk_check_operator_shape(Checker *c, const LhatNode *at,
                              const LhatType *type, const char *name,
                              size_t length)
{
    bool compares = length == 3 && memcmp(name, "<=>", 3) == 0;
    bool equals = length == 1 && name[0] == '=';  // [syntax.op.equal]
    bool may_be_unary = length == 1 && name[0] == '-';
    if (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING) {
        return;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        // [syntax.obj.override]: an overloaded one is every arm at once, and each has to be
        // an operator on its own.
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            chk_check_operator_shape(c, at, arm->type, name, length);
        }
        return;
    }
    size_t params = 0;
    for (const LhatTypeList *p =
             type->kind == LHAT_TYPE_FUNC ? type->v.func.params : NULL;
         p != NULL; p = p->next) {
        params++;
    }
    // [syntax.sub.op-not-yieldable]: and it may not be yieldable. Not because it is an f^ -- [syntax.sub.yieldable-f]
    // lets one of those suspend -- but because [syntax.sub.no-contagion] has a yieldable call
    // answer a coroutine, where the signature above says it answers T.
    //
    // [syntax.op.right]: which side the self^ was written on is not judged here.
    // Either way the receiver is out of `params`, so the count is the same,
    // and both spellings are operators -- the position only says which
    // operand the receiver is.
    // [syntax.op.define]: a unary one takes self^ and nothing else. [syntax.op.right]'s trailing
    // self^ cannot arise there -- with no other parameter, the self^ is the
    // first, and chk_self_marker_at reads that as the leading one.
    bool shaped = params == 1 || (may_be_unary && params == 0);
    if (type->kind != LHAT_TYPE_FUNC || !type->v.func.is_function ||
        !type->v.func.takes_self || !shaped || type->v.func.yields) {
        chk_report(c, at, LHAT_CHECK_ERR_BAD_OPERATOR);
        return;
    }
    // [syntax.op.compare]: '(a <=> b) < 0' is what an ordering reads, so the answer has
    // to be a thing zero can be on one side of.
    if (compares && (type->v.func.result == NULL ||
                     !lhat_type_conforms(type->v.func.result,
                                         chk_simple(c, LHAT_TYPE_NUMBER)))) {
        chk_report(c, at, LHAT_CHECK_ERR_COMPARE_NOT_NUMBER);
    }
    // [syntax.op.equal]: an '=' is read as it stands rather than against zero, and
    // [syntax.if.bool] makes bool^ the one thing a condition may be -- so that is what
    // the answer has to be.
    if (equals && (type->v.func.result == NULL ||
                   !lhat_type_conforms(type->v.func.result,
                                       chk_simple(c, LHAT_TYPE_BOOL)))) {
        chk_report(c, at, LHAT_CHECK_ERR_EQUAL_NOT_BOOL);
    }
}

// [syntax.op.right]: and nothing but an op^ has any use for a trailing self^. A
// call written 'x.m(y)' takes its receiver from what stands before the dot,
// so a member saying the receiver is its last argument says nothing [syntax.obj.self] can
// act on. Reads an overloaded member arm by arm, the way the shape rule does.
void chk_refuse_self_last(Checker *c, const LhatNode *at,
                          const LhatType *type)
{
    if (type == NULL) {
        return;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            chk_refuse_self_last(c, at, arm->type);
        }
        return;
    }
    if (type->kind == LHAT_TYPE_FUNC && type->v.func.self_last) {
        chk_report(c, at, LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR);
    }
}

// [syntax.obj.tostring]: tostring is 'f^self^ -> string^'. A hole of [lex.string.interp] calls it on the
// value's behalf with no call written, so [syntax.sub.kinds] has nothing to catch there --
// what stands under the name is held to that shape where it is written
// instead. `both` says whether the bare spelling is the same member here
// (everything but a plain table, [syntax.obj.tostring-hat]); every arm of an overload is a
// candidate a hole may reach, so each has to be an f^.
static bool tostring_fits(const LhatType *type, const LhatType *shape)
{
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type->kind != LHAT_TYPE_FUNC || !arm->type->v.func.is_function) {
                return false;
            }
        }
    }
    return lhat_type_conforms(type, shape);
}

void chk_check_tostring_shape(Checker *c, const LhatNode *at, const LhatType *type,
                              const char *name, size_t length, bool both)
{
    if (!chk_name_is(name, both ? lhat_member_held_as(name, length) : length,
                     both ? "tostring" : "tostring^") ||
        type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING) {
        return;
    }
    LhatType *shape = lhat_type_func(c->result->types, true);
    shape->v.func.takes_self = true;
    shape->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    if (!tostring_fits(type, shape)) {
        chk_report(c, at, LHAT_CHECK_ERR_BAD_TOSTRING);
    }
}

// [syntax.op.define]: what a built-in type answers. The checker knows these rather than any
// L^ writing them out, the way [syntax.sub.coroutine-make] gives a coroutine start(). [syntax.obj.self] puts the
// left operand in self^, so the right one is the only parameter.
LhatType *chk_builtin_operator(Checker *c, LhatTypeKind carrier,
                               const char *name, size_t length)
{
    // [syntax.op.compare]: number^ and string^ each order their own, so both carry the
    // one comparison -- and it answers a number^ whatever it was asked of.
    // Without this a written '"a" < "b"' had nothing to reach at all.
    if (chk_name_is(name, length, "<=>") &&
        (carrier == LHAT_TYPE_NUMBER || carrier == LHAT_TYPE_STRING)) {
        LhatType *compare = lhat_type_func(c->result->types, true);
        compare->v.func.takes_self = true;
        lhat_type_add_param(c->result->types, compare, chk_simple(c, carrier));
        compare->v.func.result = chk_simple(c, LHAT_TYPE_NUMBER);
        return compare;
    }

    LhatTypeKind takes;
    if (carrier == LHAT_TYPE_STRING && chk_name_is(name, length, "..")) {
        takes = LHAT_TYPE_STRING;  // [syntax.op.concat]: joining two strings answers one
    } else if (carrier == LHAT_TYPE_NUMBER && length > 0 && name[0] != '.' &&
               !chk_name_is(name, length, "cross") &&
               !chk_name_is(name, length, "dot")) {
        // [syntax.obj.number] makes number^ one type, and its arithmetic operators take
        // and answer one. Products are supplied by types, not by number^;
        // '..' likewise is not numeric arithmetic ([errors.runtime.div-zero]).
        takes = LHAT_TYPE_NUMBER;
    } else {
        // [syntax.op.define]: bool^ carries none. and^, or^ and '!' are the built-in
        // logic and nothing writes over them.
        return NULL;
    }
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, signature, chk_simple(c, takes));
    signature->v.func.result = chk_simple(c, takes);
    return signature;
}

// [syntax.op.functions]: an operator is a function, and [syntax.op.semantic] asks structurally whether a type
// carries it. Symbolic operators cannot be ordinary member names. The named
// products cross/dot are introduced through op^cross^ and op^dot^, and share
// their member names with host registrations.
LhatType *chk_operator_member(Checker *c, const LhatType *type,
                              const char *name, size_t length)
{
    if (type == NULL || name == NULL) {
        return NULL;
    }
    // [modules.hostvalue]: a host value carries its registered operators in the same
    // member list a table carries [syntax.op.define]'s.
    if (type->kind == LHAT_TYPE_TABLE || type->kind == LHAT_TYPE_HOSTVALUE) {
        const LhatTypeMember *m = chk_find_member(type, name, length);
        return m != NULL ? m->type : NULL;
    }
    return chk_builtin_operator(c, type->kind, name, length);
}

// An inference gap may settle in a later round. any^ is a known top type,
// not a gap: an operator requiring a narrower type must reject it until the
// source narrows it, just as an ordinary function parameter does.
bool chk_operator_undecided(const LhatType *type)
{
    return type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
           type->kind == LHAT_TYPE_PENDING;
}

// ---------------------------------------------------------------------------
// Narrowing ([syntax.type.fits])
// ---------------------------------------------------------------------------

// [syntax.type.fits] limits narrowing to a name and to a dot path from one. A call or an
// index is excluded because there is no guarantee the second evaluation
// yields the same value, and a narrowing that cannot be relied on afterwards
// is worse than none.
bool chk_narrowable(const LhatNode *node)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_SCOPE:
        // [syntax.match.form]: a match's subject with no name of its own is the focus, and
        // the desugared arms test exactly that node -- 'when^ fits^ T:' has
        // to narrow what it^ names or the arm knows nothing.
        case LHAT_NODE_FOCUS:
            return true;
        case LHAT_NODE_MEMBER:
            return !node->v.access.nil_safe && chk_narrowable(node->v.access.target);
        default:
            return false;
    }
}

// [syntax.type.fits]: the nil^ written down, which is what makes a comparison against it a
// narrowing. The literal only -- a narrowing needs somewhere in the source to
// anchor to, and nothing is gained by following an expression that merely
// happens to be typed nil^. The same test infer_name makes for the value.
static bool is_nil_literal(const Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           chk_node_name(c, node, &name, &length) &&
           chk_name_is(name, length, "nil^");
}

// [syntax.type.fits-range]: a whole number written down, which is what makes a comparison
// against it a bound. The literal only, for the reason the nil^ one above is
// the literal only -- and a whole one, since a position is counted in whole
// numbers and [syntax.obj.number]'s one type admits reals as readily. '-1' is the unary it
// is written as and still a number written down -- positions count from 0,
// so the end just before the first is spelt with it.
bool chk_whole_literal(const LhatNode *node, LhatInteger *value)
{
    bool negated = node != NULL && node->kind == LHAT_NODE_UNARY &&
                   node->v.unary.op == LHAT_OP_SUB;
    if (negated) {
        node = node->v.unary.operand;
    }
    if (node == NULL || node->kind != LHAT_NODE_INT) {
        return false;
    }
    LhatInteger written = (LhatInteger)node->v.integer.value;
    *value = negated ? -written : written;
    return true;
}

// [syntax.op.precedence]: which way a comparison faces, and whether it takes its own limit.
// `strict` is the '<' of '<=' -- the limit is one step further in.
static bool ordering_op(LhatOpKind op, bool *upward, bool *strict)
{
    switch (op) {
        case LHAT_OP_LT: *upward = true;  *strict = true;  return true;
        case LHAT_OP_LE: *upward = true;  *strict = false; return true;
        case LHAT_OP_GT: *upward = false; *strict = true;  return true;
        case LHAT_OP_GE: *upward = false; *strict = false; return true;
        default: return false;
    }
}

// [syntax.type.fits-range]: 'path < limit' and the three like it. `path_leads` says which side
// the path stood on, since 'd <= 9' and '9 >= d' bound the same end; `holds`
// says which branch is being read, since what an ordering denies is another
// ordering -- '!(d < 1)' is 'd >= 1', and that is what makes the guard
// written as an early exit bound the rest of the body.
static void bound_from_comparison(Checker *c, const LhatNode *path,
                                  LhatOpKind op, LhatInteger limit,
                                  bool path_leads, bool holds)
{
    bool upward = false;
    bool strict = false;
    if (!ordering_op(op, &upward, &strict) || !chk_narrowable(path)) {
        return;
    }
    if (!holds) {
        upward = !upward;
        strict = !strict;
    }
    if (!path_leads) {
        upward = !upward;
    }
    if (upward) {
        chk_push_bounds(c, path, INT64_MIN, strict ? limit - 1 : limit);
    } else {
        chk_push_bounds(c, path, strict ? limit + 1 : limit, INT64_MAX);
    }
}

// [lex.hat.repeat]: the canonical name cuts after the first hat, so it^ and
// it^^ spell the same name reaching two different bindings -- a narrowing
// recorded for one must not apply to the other, which is what comparing the
// counts guards.
static size_t name_hats(const LhatNode *node)
{
    if (node == NULL) {
        return 0;
    }
    // [syntax.for.it]: the focus with no name written is it^ -- one hat, so a written
    // it^ still names the same binding it does.
    if (node->kind == LHAT_NODE_FOCUS) {
        return 1;
    }
    if (node->kind == LHAT_NODE_IDENT || node->kind == LHAT_NODE_HAT_IDENT ||
        node->kind == LHAT_NODE_TYPE_NAME) {
        return node->v.name.hats;
    }
    return 0;
}

static bool same_name(const Checker *c, const LhatNode *a, const LhatNode *b)
{
    const char *na = NULL;
    const char *nb = NULL;
    size_t la = 0;
    size_t lb = 0;
    return chk_node_name(c, a, &na, &la) && chk_node_name(c, b, &nb, &lb) &&
           la == lb && memcmp(na, nb, la) == 0 &&
           name_hats(a) == name_hats(b);
}

static bool same_path(const Checker *c, const LhatNode *a, const LhatNode *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    // [syntax.match.form]: the focus node is it^ without the word appearing, so a written
    // it^ read in an arm is the same path as the FOCUS the desugar tested.
    // same_name reads "it^" off both, so only the kind gate has to give.
    if (a->kind != b->kind &&
        a->kind != LHAT_NODE_FOCUS && b->kind != LHAT_NODE_FOCUS) {
        return false;
    }
    if (a->kind != b->kind &&
        (a->kind == LHAT_NODE_MEMBER || b->kind == LHAT_NODE_MEMBER)) {
        return false;
    }
    if (a->kind == LHAT_NODE_MEMBER) {
        return same_path(c, a->v.access.target, b->v.access.target) &&
               same_name(c, a->v.access.argument, b->v.access.argument);
    }
    return same_name(c, a, b);
}

// Whether `path` is `prefix` or reaches further through it.
static bool path_passes_through(const Checker *c, const LhatNode *path,
                                const LhatNode *prefix)
{
    for (; path != NULL; path = path->kind == LHAT_NODE_MEMBER
                                    ? path->v.access.target
                                    : NULL) {
        if (same_path(c, path, prefix)) {
            return true;
        }
    }
    return false;
}

LhatType *chk_narrowed_type(Checker *c, const LhatNode *path)
{
    // [syntax.type.fits]: a write ends what a branch established about the target, so it
    // does not answer for the target either -- what may be written is what
    // the name holds. Only the path being written to: the prefix of one
    // keeps its own, which is what lets 'a.b := 1' inside 'if^ a fits^ T'
    // reach T's member at all.
    if (c->writing_to != NULL && same_path(c, c->writing_to, path)) {
        return NULL;
    }
    // [syntax.type.fits-range]: a record made of bounds says nothing about the type, so it is
    // passed over rather than answered with -- the two readings share a list
    // and neither hides the other.
    for (const Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        if (n->type != NULL && same_path(c, n->path, path)) {
            return n->type;
        }
    }
    return NULL;
}

bool chk_narrowed_bounds(Checker *c, const LhatNode *path, LhatInteger *lo,
                         LhatInteger *hi)
{
    // A write ends what a branch knew, exactly as it does of a type above.
    if (c->writing_to != NULL && same_path(c, c->writing_to, path)) {
        return false;
    }
    for (const Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        if (n->has_bounds && same_path(c, n->path, path)) {
            *lo = n->lo;
            *hi = n->hi;
            return true;
        }
    }
    return false;
}

// One end moved by a written amount. An end nothing bounded stays unbounded,
// and one that would run past the int64_t range stops at its edge -- which is
// still an end nothing bounded, so it reads the same.
static LhatInteger shift_end(LhatInteger end, LhatInteger by)
{
    if (end == INT64_MIN || end == INT64_MAX) {
        return end;
    }
    if (by > 0 && end > INT64_MAX - by) {
        return INT64_MAX;
    }
    if (by < 0 && end < INT64_MIN - by) {
        return INT64_MIN;
    }
    return end + by;
}

// [syntax.type.fits-range]: the whole numbers an expression may hold here -- a number written
// down, a bounded path, or one of those with a written number added or taken
// away. Nothing wider: a write is what would let a range grow round after
// round in [pipeline.infer.rounds], and every form here is read where it stands and kept by
// nothing but an index or a let^.
bool chk_bounds_of(Checker *c, const LhatNode *node, LhatInteger *lo, LhatInteger *hi)
{
    LhatInteger k = 0;
    if (chk_whole_literal(node, &k)) {
        *lo = *hi = k;
        return true;
    }
    if (node == NULL) {
        return false;
    }
    if (chk_narrowable(node)) {
        return chk_narrowed_bounds(c, node, lo, hi);
    }
    if (node->kind != LHAT_NODE_BINARY ||
        (node->v.binary.op != LHAT_OP_ADD && node->v.binary.op != LHAT_OP_SUB)) {
        return false;
    }
    bool subtract = node->v.binary.op == LHAT_OP_SUB;
    LhatInteger a = 0;
    LhatInteger b = 0;
    if (chk_whole_literal(node->v.binary.right, &k) &&
        chk_bounds_of(c, node->v.binary.left, &a, &b)) {
        // 'd + k' and 'd - k'.
        if (subtract) {
            k = k == INT64_MIN ? INT64_MAX : -k;
        }
        *lo = shift_end(a, k);
        *hi = shift_end(b, k);
        return true;
    }
    if (chk_whole_literal(node->v.binary.left, &k) &&
        chk_bounds_of(c, node->v.binary.right, &a, &b)) {
        if (!subtract) {
            *lo = shift_end(a, k);  // 'k + d'
            *hi = shift_end(b, k);
            return true;
        }
        // 'k - d' faces the other way: the far end of d is the near end here.
        *lo = b == INT64_MAX ? INT64_MIN : shift_end(k, b == INT64_MIN ? INT64_MAX : -b);
        *hi = a == INT64_MIN ? INT64_MAX : shift_end(k, -a);
        return true;
    }
    return false;
}

static void push_narrowing(Checker *c, const LhatNode *path, LhatType *type)
{
    if (type == NULL) {
        return;  // nothing survives; leave the wider type rather than none
    }
    Narrowing *n = (Narrowing *)lhat_calloc(1, sizeof *n);
    if (n == NULL) {
        return;
    }
    n->path = path;
    n->type = type;
    n->next = c->narrowings;
    c->narrowings = n;
}

void chk_push_bounds(Checker *c, const LhatNode *path, LhatInteger lo, LhatInteger hi)
{
    // [syntax.type.fits-range]: 'd >= 1 and^ d <= 9' pushes one side and then the other, so
    // what is known here is what both sides said. The and^ of a condition
    // narrows from each half in turn (chk_narrow_from), which is what leaves
    // the earlier record standing to meet this one.
    LhatInteger known_lo = 0;
    LhatInteger known_hi = 0;
    if (chk_narrowed_bounds(c, path, &known_lo, &known_hi)) {
        if (known_lo > lo) {
            lo = known_lo;
        }
        if (known_hi < hi) {
            hi = known_hi;
        }
    }
    if (lo > hi) {
        return;  // nothing is both, so the branch is left as it was
    }
    Narrowing *n = (Narrowing *)lhat_calloc(1, sizeof *n);
    if (n == NULL) {
        return;
    }
    n->path = path;
    n->lo = lo;
    n->hi = hi;
    n->has_bounds = true;
    n->next = c->narrowings;
    c->narrowings = n;
}

void chk_pop_narrowings(Checker *c, Narrowing *mark)
{
    while (c->narrowings != mark) {
        Narrowing *next = c->narrowings->next;
        lhat_free(c->narrowings);
        c->narrowings = next;
    }
}

// [syntax.type.fits]: reassigning ends it, since the claim was about the value that was
// examined. A let^ in the branch does the same by introducing a new name.
//
// Emptied rather than unlinked: a branch remembers where its narrowings begin
// so it can pop them, and removing a node from under that mark would leave it
// pointing at freed memory. An emptied entry reports nothing, which sends the
// caller back to the binding -- the answer wanted here anyway.
//
// Only the written path and what is reached through it: 'a.b := v' leaves
// what was established of 'a' ([syntax.type.fits]'s own example). An index names no
// member statically, so 't[k] := v' ends everything reached through t, but
// not what was established of t itself.
void chk_drop_narrowings_for(Checker *c, const LhatNode *target)
{
    bool indexed = target->kind == LHAT_NODE_INDEX;
    while (target->kind == LHAT_NODE_INDEX) {
        target = target->v.access.target;
    }
    for (Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        const LhatNode *path = n->path;
        if (indexed) {
            path = path->kind == LHAT_NODE_MEMBER ? path->v.access.target : NULL;
        }
        if (path_passes_through(c, path, target)) {
            n->type = NULL;
            // [syntax.type.fits-word]: what the branch knew of the number is ended by the
            // write as surely as what it knew of the type.
            n->has_bounds = false;
        }
    }
}

// [syntax.loop.exit]: a break^ leaves as many loops as its level names, counting the
// innermost as 1. Answers whether this loop has a way out written in it --
// `depth` is how many loops stand between the statements being read and the
// one being asked about, so a break^ leaves it when its level reaches past
// them. Whether it is that loop's normal end or one it is only passed
// through does not matter here: either way control leaves.
static bool breaks_out_from(const LhatNode *node, uint32_t depth);

// [errors.catch.clause]: an arm stands where the block's statements do, so a break^ in
// one counts the same loops theirs would.
static bool arms_break_out(const LhatNode *arms, uint32_t depth)
{
    for (const LhatNode *arm = arms; arm != NULL; arm = arm->next) {
        if (breaks_out_from(arm->v.clause.body, depth)) {
            return true;
        }
    }
    return false;
}

static bool breaks_out_from(const LhatNode *node, uint32_t depth)
{
    for (; node != NULL; node = node->next) {
        switch (node->kind) {
            case LHAT_NODE_BREAK:
                if (node->v.jump.level > depth) {
                    return true;
                }
                break;

            case LHAT_NODE_BLOCK:
                if (breaks_out_from(node->v.list.items, depth)) {
                    return true;
                }
                // [syntax.loop]: the clauses of a loop body are statements of it too.
                for (const LhatNode *clause = node->v.list.extra;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out_from(clause->v.loop_clause.body, depth)) {
                        return true;
                    }
                }
                if (arms_break_out(node->v.list.arms, depth)) {
                    return true;
                }
                break;

            case LHAT_NODE_IF_STMT:
                for (const LhatNode *clause = node->v.list.items;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out_from(clause->v.clause.body, depth)) {
                        return true;
                    }
                }
                if (arms_break_out(node->v.list.arms, depth)) {
                    return true;
                }
                break;

            case LHAT_NODE_WITH:
                if (breaks_out_from(node->v.list.extra, depth)) {
                    return true;
                }
                break;

            case LHAT_NODE_FOR:
                // [syntax.for.forms] and [syntax.match]: the if^, when^ and do^ forms do not iterate,
                // so a break^ inside one still leaves the loop out here. Every
                // other form is a loop, and standing inside it is one more
                // loop for a level to count past.
                if (node->v.loop.kind == LHAT_FOR_IF ||
                    node->v.loop.kind == LHAT_FOR_WHEN ||
                    node->v.loop.kind == LHAT_FOR_ONCE) {
                    if (breaks_out_from(node->v.loop.body, depth)) {
                        return true;
                    }
                } else if (breaks_out_from(node->v.loop.body, depth + 1)) {
                    return true;
                }
                break;

            case LHAT_NODE_REPEAT:
                if (breaks_out_from(node->v.repeat.body, depth + 1)) {
                    return true;
                }
                break;

            // A nested subroutine body is not this loop's, whatever it
            // writes -- a break^ there leaves that body's own loops.
            default:
                break;
        }
    }
    return false;
}

static bool breaks_out(const LhatNode *node)
{
    return breaks_out_from(node, 0);
}

static bool arms_always_exit(const LhatNode *arms)
{
    for (const LhatNode *arm = arms; arm != NULL; arm = arm->next) {
        if (!chk_always_exits(arm->v.clause.body)) {
            return false;
        }
    }
    return true;
}

// Whether control cannot reach the end of this statement. [errors.fits.form] is
// written in the early-return style -- handle the error, leave, and carry on
// below knowing it did not happen -- so the narrowing a branch established
// has to outlive a branch that never falls through.
bool chk_always_exits(const LhatNode *node)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        case LHAT_NODE_RETURN:
        case LHAT_NODE_BREAK:
        // [syntax.loop.next]: a next^ leaves no more of the body to run either. It is not a
        // way out of the loop -- breaks_out_from is where that is asked, and
        // this one is not counted there -- but nothing after it is reached.
        case LHAT_NODE_NEXT:
        case LHAT_NODE_PANIC:
            return true;

        // [errors.catch.clause]: the statements finishing with nothing raised is one way
        // past a block, and each arm is another -- what no arm took leaves
        // the frame -- so every arm has to leave as well.
        case LHAT_NODE_BLOCK:
            for (const LhatNode *s = node->v.list.items; s != NULL;
                 s = s->next) {
                if (chk_always_exits(s)) {
                    return arms_always_exit(node->v.list.arms);
                }
            }
            return false;

        case LHAT_NODE_IF_STMT: {
            bool has_else = false;
            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                if (!chk_always_exits(clause->v.clause.body)) {
                    return false;
                }
                if (clause->v.clause.condition == NULL) {
                    has_else = true;
                }
            }
            return has_else && arms_always_exit(node->v.list.arms);
        }

        // [syntax.for.repeat]: a repeat^ with no bound runs until something leaves it. With
        // no break^ of its own, nothing after it is ever reached, so it ends
        // the statements around it the way a return^ does. [pipeline.infer] counts
        // exits to infer a result, and this is one that produces no value
        // and never happens -- which keeps it out of the result entirely.
        case LHAT_NODE_REPEAT:
            return node->v.repeat.kind == LHAT_REPEAT_FOREVER &&
                   !breaks_out(node->v.repeat.body);

        // [syntax.with.form]: a with^ runs its block once and always. Nothing stands
        // between the bindings and the body, which is why the checker walks
        // it outside c->conditional where a loop's or an if^'s body is
        // inside. So what the block does is what the with^ does -- and
        // [errors.try.form]'s 'with^ h = try^ open(p) { return^ … }' is a body that answers
        // on every path, which is what it was written to be.
        case LHAT_NODE_WITH:
            return chk_always_exits(node->v.list.extra);

        default:
            return false;
    }
}

// The narrowings a condition implies when it holds, or when it does not.
//
// [syntax.type.fits]: and^ tells us both sides held when it is true, or^ tells us both
// failed when it is false, and neither says anything in the other direction
// -- one of the two could have decided it alone.
void chk_narrow_from(Checker *c, const LhatNode *condition, bool truth)
{
    if (condition == NULL) {
        return;
    }

    // [syntax.type.fits] with [errors.runtime.index]: three shapes narrow. 'x fits^ T' asks about a
    // type; '=' / '!=' / 'is^' against the nil^ literal asks about absence,
    // which [errors.runtime.index] spells with nil^ and is the first thing anyone writes; and
    // [syntax.op.present]'s 'x?' is the second of those written short. Without the nil^
    // ones, 'if^ t != nil^ { t[0] }' passed the condition and then reported
    // against t[0] -- a diagnostic nowhere near its cause.
    const LhatNode *path = NULL;
    LhatType *tested = NULL;
    // Whether the branch has the path *being* what was tested for. '!=' and
    // '?' are the same question the other way round, exactly as '!' is.
    bool holds = truth;

    if (condition->kind == LHAT_NODE_UNARY) {
        if (condition->v.unary.op == LHAT_OP_NOT) {
            chk_narrow_from(c, condition->v.unary.operand, !truth);
            return;
        }
        // [syntax.op.present]: 'x?' is '!(x fits^ nil^)', so it narrows to exactly
        // what that would -- the true side without the nil^ arm.
        if (condition->v.unary.op != LHAT_OP_PRESENT) {
            return;
        }
        path = condition->v.unary.operand;
        tested = chk_simple(c, LHAT_TYPE_NIL);
        holds = !truth;
    } else if (condition->kind == LHAT_NODE_COMPARE_CHAIN) {
        // [syntax.op.precedence] の (5): '1 <= d <= 9' is the two links written as one, and the
        // operand they share is bounded on both sides at once. Each link is
        // read the way the binary form below reads it, so a chain of any
        // length bounds whatever whole numbers stand beside a path.
        //
        // The true side only. [syntax.op.compare] makes a chain the and^ of its links, so
        // denying it denies one link without saying which -- there is no
        // single bound on the other side to record.
        if (!truth) {
            return;
        }
        const LhatNode *operand = condition->v.chain.operands;
        for (const LhatNode *marker = condition->v.chain.operators;
             marker != NULL && operand != NULL; marker = marker->next) {
            const LhatNode *right = operand->next;
            if (right == NULL) {
                break;
            }
            LhatInteger limit = 0;
            LhatOpKind op = marker->v.unary.op;
            if (chk_whole_literal(right, &limit)) {
                bound_from_comparison(c, operand, op, limit, true, true);
            } else if (chk_whole_literal(operand, &limit)) {
                bound_from_comparison(c, right, op, limit, false, true);
            }
            operand = right;
        }
        return;
    } else if (condition->kind == LHAT_NODE_BINARY) {
        LhatOpKind op = condition->v.binary.op;
        if ((op == LHAT_OP_AND && truth) || (op == LHAT_OP_OR && !truth)) {
            chk_narrow_from(c, condition->v.binary.left, truth);
            chk_narrow_from(c, condition->v.binary.right, truth);
            return;
        }
        // [syntax.type.fits-range]: an ordering against a whole number written down bounds the
        // path on one side. Both branches bound it -- what an ordering denies
        // is the ordering the other way -- so a guard written as an early
        // exit leaves the rest of the body knowing what it let through.
        LhatInteger limit = 0;
        bool upward = false;
        bool strict = false;
        if (ordering_op(op, &upward, &strict)) {
            if (chk_whole_literal(condition->v.binary.right, &limit)) {
                bound_from_comparison(c, condition->v.binary.left, op, limit,
                                      true, truth);
            } else if (chk_whole_literal(condition->v.binary.left, &limit)) {
                bound_from_comparison(c, condition->v.binary.right, op, limit,
                                      false, truth);
            }
            return;
        }
        if (op == LHAT_OP_FITS) {
            path = condition->v.binary.left;
            tested = chk_resolve_type(c, condition->v.binary.right);
        } else if (op == LHAT_OP_EQ || op == LHAT_OP_NE || op == LHAT_OP_IS) {
            // Either side may carry the literal; the other is the path.
            // 'is^' rides with '=' because nil^ has one value, so [syntax.type.fits]'s
            // closing rule makes the two always agree -- leaving it out
            // would be a gap with no reason behind it.
            if (is_nil_literal(c, condition->v.binary.right)) {
                path = condition->v.binary.left;
            } else if (is_nil_literal(c, condition->v.binary.left)) {
                path = condition->v.binary.right;
            } else {
                return;
            }
            tested = chk_simple(c, LHAT_TYPE_NIL);
            if (op == LHAT_OP_NE) {
                holds = !truth;
            }
        } else {
            return;
        }
    } else {
        return;
    }

    if (path == NULL || !chk_narrowable(path)) {
        return;
    }

    // A second reading of something the caller inferred already: every call
    // site below narrows a condition it has just walked in full, so nothing
    // here is reached for the first time and nothing here reports.
    c->rereading++;
    LhatType *current = chk_infer(c, path);
    c->rereading--;

    LhatType *inside =
        holds ? chk_only(c, current, tested) : chk_without(c, current, tested);

    // A successful runtime test supplies the shape an undecided arm lacked.
    if (holds && inside != NULL &&
        (inside->kind == LHAT_TYPE_UNKNOWN || inside->kind == LHAT_TYPE_PENDING)) {
        inside = tested;
    }

    // [pipeline.infer]: a parameter still being decided says nothing to narrow, so
    // both sides hand its own object straight back. Inside the branch that
    // object would be read as the parameter itself and every use of it as a
    // demand -- which is exactly what a narrowing is written to prevent. The
    // true side knows what was tested for; the false side knows only what it
    // is not, which is not a type here.
    if (chk_param_var_for(c, inside) != NULL) {
        inside = truth ? tested : chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    push_narrowing(c, path, inside);
}
