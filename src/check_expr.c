// L^ (lhat) -- the type checking stage: expressions.

#include "check_internal.h"
#include "instantiation_internal.h"
#include "rttype.h"

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

typedef struct RuntimeArgumentSeen {
    const LhatType *type;
    const struct RuntimeArgumentSeen *outer;
} RuntimeArgumentSeen;

static bool unresolved_within(const LhatType *type, const RuntimeArgumentSeen *seen)
{
    if (type == NULL) return false;
    for (const RuntimeArgumentSeen *s = seen; s != NULL; s = s->outer) {
        if (s->type == type) return false;
    }
    RuntimeArgumentSeen here = {type, seen};
    seen = &here;
    if (type->kind == LHAT_TYPE_ARGUMENT) return true;
    if (type->specialization_base != NULL) {
        for (const LhatTypeList *a = type->specialization_arguments; a; a = a->next) {
            if (unresolved_within(a->type, seen)) return true;
        }
        return false;
    }
    if (type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT ||
        type->kind == LHAT_TYPE_TUPLE) {
        for (const LhatTypeList *a = type->v.composite.arms; a; a = a->next) {
            if (unresolved_within(a->type, seen)) return true;
        }
    } else if (type->kind == LHAT_TYPE_TABLE && !type->v.table.nominal) {
        for (const LhatTypeMember *m = type->v.table.members; m; m = m->next) {
            if (unresolved_within(m->type, seen)) return true;
        }
        return unresolved_within(type->v.table.variadic, seen) ||
               unresolved_within(type->v.table.index_value, seen);
    } else if (type->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = type->v.func.params; p; p = p->next) {
            if (unresolved_within(p->type, seen)) return true;
        }
        return unresolved_within(type->v.func.result, seen) ||
               unresolved_within(type->v.func.variadic, seen);
    } else if (type->kind == LHAT_TYPE_CORO) {
        return unresolved_within(type->v.coroutine.receive, seen) ||
               unresolved_within(type->v.coroutine.produce, seen) ||
               unresolved_within(type->v.coroutine.result, seen);
    }
    return false;
}

bool chk_runtime_has_unresolved_arguments(const LhatType *type)
{
    return unresolved_within(type, NULL);
}

// A type written out for a message's hole, frozen in the semantic arena.
static const char *type_text(Checker *c, const LhatType *type)
{
    size_t length = lhat_type_write(type, NULL, 0);
    char *text = lhat_type_semantic_alloc(c->result->types, length + 1);
    if (text != NULL) lhat_type_write(type, text, length + 1);
    return text;
}

// Texts joined into one, each followed by `between` but the last, in parentheses
// when `wrap` -- an argument list, or the parameters an arm asks for.
static const char *joined_text(Checker *c, const char *const *parts, size_t count,
                               const char *between, bool wrap)
{
    size_t gap = strlen(between), length = wrap ? 2 : 0;
    for (size_t i = 0; i < count; i++) {
        length += (parts[i] != NULL ? strlen(parts[i]) : 0) + gap;
    }
    char *text = lhat_type_semantic_alloc(c->result->types, length + 1);
    if (text == NULL) return NULL;
    size_t at = 0;
    if (wrap) text[at++] = '(';
    for (size_t i = 0; i < count; i++) {
        if (i > 0) { memcpy(text + at, between, gap); at += gap; }
        size_t part = parts[i] != NULL ? strlen(parts[i]) : 0;
        memcpy(text + at, parts[i], part);
        at += part;
    }
    if (wrap) text[at++] = ')';
    text[at] = '\0';
    return text;
}

void chk_expect(Checker *c, const LhatNode *at, LhatType *value,
                LhatType *target, LhatCheckErrorCode code)
{
    // [pipeline.infer]: a parameter whose type the body is deciding is not being
    // checked here -- this position is one of the things deciding it. Most of
    // what a body demands arrives through this one call.
    if (chk_param_var_for(c, value) != NULL) {
        chk_constrain(c, value, target);
        return;
    }

    // Always report the same conformance result. A failure caused only by
    // incomplete information is advisory under relaxed diagnostic policy.
    bool ok = lhat_type_conforms_strict(value, target);
    if (!ok) {
        size_t before = c->result->diagnostic_count;
        chk_report(c, at, code);
        if (c->result->diagnostic_count > before) {
            LhatCheckDiagnostic *d = &c->result->diagnostics[before];
            d->relaxed_ok = d->relaxed_ok || lhat_type_conforms(value, target);
            // An undecided value was reported where it went wrong; writing
            // it as '?' here says nothing more.
            if (value != NULL && target != NULL && !lhat_type_has_gap(value)) {
                d->left_type = type_text(c, value);
                d->right_type = type_text(c, target);
            }
        }
    }
    // Another view may write entries the literal did not originally contain.
    if (value != NULL && target != NULL && value != target &&
        value->kind == LHAT_TYPE_TABLE &&
        (target->kind == LHAT_TYPE_ANY || target->kind == LHAT_TYPE_TABLE)) {
        value->v.table.entries_known = false;
    }
}

// [errors.runtime.index]: a table answers nil^ for a key that is not there, so what a key
// reads out of one is 'T|nil^' -- and no operator is on the nil^ side. Saying
// [syntax.op.right]'s rule there sends the writer looking for an operator to write, when
// what is missing is a narrowing.
//
// True when this is a union carrying a nil^ arm, with `bare` set to what it is
// without one. Each caller then asks its own question again of `bare`: only
// where dropping the nil^ is what would have made the operator answer is that
// advice the right advice, and 'nil^|string^ + 1' is not such a place.
//
// A bare nil^ is not one of these. What stands there is nil^ rather than may
// be, and no narrowing turns it into anything else.
static bool nil_arm_apart(Checker *c, LhatType *type, LhatType **bare)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) {
        return false;
    }
    bool carries = false;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (arm->type != NULL && arm->type->kind == LHAT_TYPE_NIL) {
            carries = true;
            break;
        }
    }
    if (!carries) {
        return false;
    }
    // chk_without rather than chk_without_nil_arm: that one gives a union of
    // three arms back unchanged, and 'number^|string^|nil^' is exactly the
    // shape this is here to see through.
    LhatType *without = chk_without(c, type, chk_simple(c, LHAT_TYPE_NIL));
    if (without == NULL) {
        return false;
    }
    *bare = without;
    return true;
}

// Analyze the successful access identically in both modes, retaining a
// diagnostic for the runtime failure path. Policy only sets its severity.
LhatType *chk_access_target(Checker *c, const LhatNode *node, LhatType *type)
{
    LhatType *bare = NULL;
    if (type != NULL && type->kind == LHAT_TYPE_NIL) {
        bare = chk_simple(c, LHAT_TYPE_UNKNOWN);
    } else if (!nil_arm_apart(c, type, &bare)) {
        return type;
    }
    if (!node->v.access.nil_safe) {
        chk_report(c, node, LHAT_CHECK_ERR_ACCESS_ON_MAYBE_NIL);
    }
    return bare;
}

// Return the success arms only when a union actually contains both success
// and error values. A plain error cannot be repaired by narrowing it.
static LhatType *without_error_arms(Checker *c, LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) return NULL;
    LhatType *bare = NULL;
    bool has_error = false;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        LhatType *t = arm->type;
        if (t != NULL && (t->kind == LHAT_TYPE_ERROR ||
                         t->kind == LHAT_TYPE_ERROR_SET ||
                         t->kind == LHAT_TYPE_ERROR_KIND)) {
            has_error = true;
        } else {
            bare = lhat_type_union(c->result->types, bare, t);
        }
    }
    return has_error ? bare : NULL;
}

// [syntax.op.right]: the one shape every operator is judged with. The left operand is
// asked for the member [syntax.op.define] names, [syntax.op.functions] makes that a function, [syntax.obj.self] puts the
// left operand in its self^ -- so the right operand is its argument and the
// expression is worth what it answers.
//
// NULL means nothing was decided: the operand types said too little (
// [pipeline.strict.relaxed]), or the answer was already reported. The caller says what to fall back
// on, since that differs by operator.
// The arm of an operator member that takes these arguments, or NULL. [syntax.obj.override]
// forbids overlapping signatures, so at most one ever does. `self_last` picks
// which side the arm was written for ([syntax.op.right]): one written the other way round
// describes the other order and is passed over.
//
// [syntax.op.define]: `count` is 1 for a binary operator and 0 for the unary '-', which
// is the whole of what tells the two apart -- chk_signature_accepts compares
// it against the declared parameters, so a binary arm cannot answer a unary
// use and neither can the reverse.
const LhatType *chk_operator_arm(const LhatType *carrier,
                                    LhatType *const *args, size_t count,
                                    bool self_last)
{
    if (carrier == NULL) {
        return NULL;
    }
    if (carrier->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = carrier->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC &&
                arm->type->v.func.self_last == self_last &&
                chk_signature_accepts(arm->type, args, count, true)) {
                return arm->type;
            }
        }
        return NULL;
    }
    if (carrier->kind != LHAT_TYPE_FUNC ||
        carrier->v.func.self_last != self_last ||
        !chk_signature_accepts(carrier, args, count, true)) {
        return NULL;
    }
    return carrier;
}

// [syntax.op.compare]: whether one of the two carries an operator of this name taking the
// other, which is what the comparison is read off. [pipeline.strict.relaxed]: wherever inference
// has not decided, this says nothing rather than reporting -- the machine
// asks the same question of the actual values.
static bool compares_by(Checker *c, LhatType *left, LhatType *right,
                        const char *name, size_t length)
{
    LhatType *args[1];
    args[0] = right;
    if (chk_operator_arm(chk_operator_member(c, left, name, length), args, 1,
                     false) != NULL) {
        return true;
    }
    // [syntax.op.right]: or the right operand carries one written as the receiver, which
    // is how a value joins a comparison whose left side is a built-in.
    args[0] = left;
    return chk_operator_arm(chk_operator_member(c, right, name, length), args, 1,
                        true) != NULL;
}

// Diagnose missing error handling only if removing the error arms really
// makes the operation valid. Do not hide an unrelated operand mismatch, or
// reject an operator explicitly accepting error values.
static unsigned operator_error_sides(Checker *c, LhatType *left, LhatType *right,
                                     const char *name, size_t length)
{
    if (chk_operator_undecided(left) || chk_operator_undecided(right) ||
        chk_param_var_for(c, left) != NULL || chk_param_var_for(c, right) != NULL) {
        return 0;
    }
    LhatType *l = without_error_arms(c, left);
    LhatType *r = without_error_arms(c, right);
    if ((l == NULL && r == NULL) || compares_by(c, left, right, name, length) ||
        !compares_by(c, l != NULL ? l : left, r != NULL ? r : right, name, length)) {
        return 0;
    }
    return (l != NULL ? 1u : 0u) | (r != NULL ? 2u : 0u);
}

// [syntax.op.compare] with [syntax.op.equal]: what says how these two compare. An ordering has only
// '<=>' to read; '=' and '≠' read an op^= first and fall back on '<=>',
// which is the same order the machine looks them up in.
static bool related_pair(Checker *c, LhatOpKind op, LhatType *left,
                         LhatType *right)
{
    if (chk_operator_undecided(left) || chk_operator_undecided(right)) {
        return true;
    }
    if ((op == LHAT_OP_EQ || op == LHAT_OP_NE) &&
        compares_by(c, left, right, "=", 1)) {
        return true;
    }
    return compares_by(c, left, right, "<=>", 3);
}

// Answers false when it has said everything there is to say, which is the one
// case where the ordinary judgement below would only repeat it.
static bool demand_ordering(Checker *c, const LhatNode *at, LhatType *left,
                            LhatType *right);

// A known value-type arm cannot participate in object identity. Unknown and
// any^ operands are checked by the VM; unions must be narrowed first.
static bool lacks_identity(const LhatType *type)
{
    if (type == NULL) return false;
    switch (type->kind) {
        case LHAT_TYPE_NUMBER:
        case LHAT_TYPE_BOOL:
        case LHAT_TYPE_HOSTVALUE:
            return true;
        case LHAT_TYPE_ARGUMENT:
            return lacks_identity(lhat_type_argument_bound(type));
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms;
                 arm != NULL; arm = arm->next) {
                if (lacks_identity(arm->type)) return true;
            }
            return false;
        default:
            return false;
    }
}

// [syntax.op.precedence] の (5) with [syntax.op.compare]: one link of a comparison, asked the same way whether
// it stands alone or in a chain. Always answers bool^ -- what may be wrong is
// the pair, never the shape of the answer.
static LhatType *check_comparison(Checker *c, const LhatNode *at, LhatOpKind op,
                                  LhatType *left, LhatType *right)
{
    // A nil^ operand asks about absence, even for an optional value type.
    if (op == LHAT_OP_IS &&
        !(left != NULL && left->kind == LHAT_TYPE_NIL) &&
        !(right != NULL && right->kind == LHAT_TYPE_NIL) &&
        (lacks_identity(left) || lacks_identity(right))) {
        chk_report(c, at, LHAT_CHECK_ERR_NO_IDENTITY);
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // [pipeline.infer.operators]: an ordering asks the pair to be related by a '<=>', so a
    // parameter standing in one is demanded what could carry it. [syntax.op.compare] leaves
    // '=' and '≠' out: equality answers without a '<=>' at all ([syntax.obj.chain] gives
    // every table its identity), so what they ask is that the two are not
    // disjoint -- and that is not a type to demand.
    if (op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS &&
        !demand_ordering(c, at, left, right)) {
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // [syntax.op.compare]: an operator taking the two is what says how they compare, and
    // it is asked first. One written across two types -- [syntax.op.right] lets the
    // right operand carry it -- relates a pair that [syntax.obj.override] would otherwise
    // call separate, so the judgement below has to come second.
    bool related = related_pair(c, op, left, right);
    if (!related && op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS &&
        operator_error_sides(c, left, right, "<=>", 3) != 0) {
        chk_report(c, at, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR);
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // [syntax.obj.override]'s disjointness says whether any value inhabits both. If none
    // does, and nothing says how they compare either, the answer is fixed
    // before the program runs -- a mistake rather than a comparison.
    if (!related && lhat_type_disjoint(left, right)) {
        chk_report(c, at, LHAT_CHECK_ERR_INCOMPARABLE);
        return chk_simple(c, LHAT_TYPE_BOOL);
    }
    // An ordering has nothing but a '<=>' to read. Equality is a different
    // matter and is left alone -- every value is the same as itself or not,
    // whatever it is ([syntax.obj.chain]), and an op^= or a '<=>' only refines that for a
    // type that writes one.
    if (!related && op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS) {
        // Either side may be the one carrying the nil^, so both are asked --
        // 't[0] < 3' and '3 < t[0]' are the same mistake read from the two
        // ends. Whichever it is, what is missing is the narrowing.
        LhatType *bare = NULL;
        bool by_nil =
            (nil_arm_apart(c, left, &bare) && related_pair(c, op, bare, right)) ||
            (nil_arm_apart(c, right, &bare) && related_pair(c, op, left, bare));
        chk_report(c, at,
                   by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                          : LHAT_CHECK_ERR_NOT_ORDERED);
    }
    return chk_simple(c, LHAT_TYPE_BOOL);
}

// [syntax.op.right]: what the RIGHT operand answers, when it was written as the
// receiver ('op^+ = f^lhs:number^, self^'). Reached only once the left has
// been asked and has no arm taking this right operand, which is what keeps
// [syntax.op.semantic]'s left-first rule intact -- and is the case that matters, since a
// built-in on the left carries the arithmetic but takes only its own kind.
//
// `answered` tells a result of NULL (an operator answering nothing) from no
// answer at all; the caller reports only in the second case.
static LhatType *right_operator(Checker *c, const char *name, size_t length,
                                LhatType *left, LhatType *right,
                                bool *answered)
{
    *answered = false;
    // [pipeline.strict.relaxed]: nothing decided about the right operand is nothing to look a member
    // up on. The demand the left side would have made has already been made.
    if (chk_operator_undecided(right) || chk_param_var_for(c, right) != NULL) {
        return NULL;
    }
    LhatType *carrier = chk_operator_member(c, right, name, length);
    if (carrier == NULL) {
        return NULL;
    }

    // The left operand is the single argument here, so an arm is asked exactly
    // as a member call asks -- the same judgement the left side uses, with the
    // operands the other way round. A left-receiver one on this side answers
    // 'right op left', which is not what is written, so only a self^-last arm
    // will do.
    LhatType *args[1] = {left};
    const LhatType *arm = chk_operator_arm(carrier, args, 1, true);
    if (arm == NULL) {
        return NULL;
    }
    *answered = true;
    return lhat_type_call_answer(arm);
}

// [syntax.op.define]: what '-x' is worth when x is not a number^. The operand carries the
// member [syntax.op.define] names, and [syntax.obj.self] puts it in self^ -- with nothing else written
// there is no argument, which is the whole difference from the binary one.
//
// NULL means nothing was decided, the same as infer_operator: the caller falls
// back on [syntax.obj.number]'s number^ and reports against that.
static LhatType *infer_unary_operator(Checker *c, LhatType *operand)
{
    // [pipeline.strict.relaxed]: a parameter says too little to look a member up on, and [syntax.obj.number]
    // already demanded number^ of it before this is asked.
    if (chk_operator_undecided(operand) || chk_param_var_for(c, operand) != NULL) {
        return NULL;
    }
    // [syntax.op.right] is about which operand is the receiver, and a unary one has only
    // the one -- so the arm is asked for the leading self^ and no other.
    const LhatType *arm =
        chk_operator_arm(chk_operator_member(c, operand, "-", 1), NULL, 0, false);
    return lhat_type_call_answer(arm);
}

// [syntax.op.right]: the operator member this side answers with as the LEFT operand. One
// written with its self^ last describes the other order -- it answers when its
// owner stands on the right, so carrying it is not answering here and it reads
// exactly as though it were absent.
// [syntax.op.right]: the arms of a group written for this order -- the self^-last ones
// describe the other and are never candidates here -- when there is exactly
// one. A group may hold several signatures and still leave one choice, which
// is what lets [pipeline.infer] read a demand off it.
static LhatType *sole_forward_arm(const LhatType *group)
{
    LhatType *only = NULL;
    for (const LhatTypeList *arm = group->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC ||
            arm->type->v.func.self_last) {
            continue;
        }
        if (only != NULL) {
            return NULL;  // two to choose between, and nothing to choose by
        }
        only = arm->type;
    }
    return only;
}

static LhatType *left_operator_member(Checker *c, LhatType *type,
                                      const char *name, size_t length)
{
    LhatType *carrier = chk_operator_member(c, type, name, length);
    if (carrier != NULL && carrier->kind == LHAT_TYPE_FUNC &&
        carrier->v.func.self_last) {
        return NULL;
    }
    return carrier;
}

// [pipeline.infer.operators]: what an operator of this name may be written on, here. Each
// position's union is what a parameter standing there may be.
typedef struct {
    LhatType *left;    // what may stand on the left
    LhatType *right;   // what may stand on the right
    LhatType *answer;  // what any of them answers
    size_t count;      // how many arms survived, over every carrier
} Candidates;

// What the walk knows about the operand that is not the parameter, when it
// knows anything. An arm that could not take it is no candidate, which is
// what keeps 'x < 1' from demanding string^ as well ([syntax.op.compare] gives '<=>' to
// both built-ins) -- and what makes it demand number^ alone.
typedef struct {
    LhatType *type;   // NULL when that side is undecided too
    bool on_right;    // which side it stands on
} Known;

// One arm folded into the three unions. [syntax.op.right]: an arm written with the self^
// last describes the other order, so its receiver is what may stand on the
// RIGHT and its parameter what may stand on the left.
static void fold_arm(Checker *c, Candidates *into, const Known *known,
                     const LhatType *arm, LhatType *receiver)
{
    if (arm == NULL || arm->kind != LHAT_TYPE_FUNC) {
        return;
    }
    LhatType *operand =
        arm->v.func.params != NULL ? arm->v.func.params->type : NULL;
    LhatType *on_left = arm->v.func.self_last ? operand : receiver;
    LhatType *on_right = arm->v.func.self_last ? receiver : operand;

    if (known->type != NULL) {
        LhatType *wanted = known->on_right ? on_right : on_left;
        if (wanted == NULL || !lhat_type_conforms(known->type, wanted)) {
            return;
        }
    }

    LhatTypeArena *types = c->result->types;
    into->count++;
    if (on_left != NULL) {
        into->left = lhat_type_union(types, into->left, on_left);
    }
    if (on_right != NULL) {
        into->right = lhat_type_union(types, into->right, on_right);
    }
    LhatType *answered = lhat_type_call_answer((LhatType *)arm);
    if (answered != NULL) {
        into->answer = lhat_type_union(types, into->answer, answered);
    }
}

static void fold_member(Checker *c, Candidates *into, const Known *known,
                        LhatType *member, LhatType *receiver)
{
    if (member == NULL) {
        return;
    }
    if (member->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = member->v.composite.arms; arm != NULL;
             arm = arm->next) {
            fold_arm(c, into, known, arm->type, receiver);
        }
        return;
    }
    fold_arm(c, into, known, member, receiver);
}

// [pipeline.infer.operators]: the candidates. [syntax.op.define]'s built-ins carry what the checker knows
// they carry -- number^ the arithmetic, string^ the '..', both the one
// comparison ([syntax.op.compare]) -- and every def^ this unit binds to a name and writes an
// op^ of the name on is another.
//
// The carriers are held by name (check.c), so the type is read through the
// scope on every walk -- a def^'s is remade on each of them ([pipeline.infer.rounds]).
// Reading one this walk has not settled yet asks for another round, which is
// what lets a body written above the def^ answer as one written below it does
// ([modules.host.import]).
static Candidates operator_candidates(Checker *c, const char *name,
                                      size_t length, const Known *known)
{
    Candidates found;
    found.left = NULL;
    found.right = NULL;
    found.answer = NULL;
    found.count = 0;

    // [syntax.op.define]: what the checker carries for the built-in types, asked of each so
    // that the answer is the operator's rather than one name's.
    static const LhatTypeKind builtins[] = {
        LHAT_TYPE_NUMBER, LHAT_TYPE_STRING, LHAT_TYPE_BOOL};
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        LhatType *carried = chk_builtin_operator(c, builtins[i], name, length);
        fold_member(c, &found, known, carried, chk_simple(c, builtins[i]));
    }

    uint16_t bit = chk_operator_bit(name, length);
    if (bit == 0 || (c->unit_operators & bit) == 0) {
        return found;
    }
    for (size_t i = 0; i < c->operator_carrier_count; i++) {
        const OperatorCarrier *carrier = &c->operator_carriers[i];
        if ((carrier->operators & bit) == 0) {
            continue;
        }
        // [modules.host.import]: a registered type is the program's, built once and never
        // remade, so it is the carrier itself -- no name to look up, no
        // round to ask for, and no instance half to reach through
        // (chk_instance_of answers NULL for both a host value and a
        // nominal table, which is what a registered type is).
        if (carrier->type != NULL) {
            fold_member(c, &found, known,
                        chk_operator_member(c, carrier->type, name, length),
                        (LhatType *)carrier->type);
            continue;
        }
        Binding *b = chk_scope_find(c->scope, carrier->name,
                                    carrier->name_length, NULL);
        if (b == NULL) {
            continue;
        }
        if (!b->reached) {
            // [pipeline.infer.rounds]: answered from the seed, so this walk read ahead of
            // itself and another one may answer better.
            c->read_provisional = true;
        }
        LhatType *instance = chk_instance_of(b->type);
        if (instance == NULL) {
            continue;
        }
        // The seed has no members yet; the next round will have them.
        fold_member(c, &found, known,
                    chk_operator_member(c, instance, name, length), instance);
    }
    return found;
}

// [pipeline.infer.operators]: what an ordering demands of a parameter standing in it. [syntax.op.compare]
// makes '<=>' the one comparison a type writes, so the candidates are its --
// which is also what keeps [syntax.op.equal]'s op^=-only type out of an ordering: it
// carries '=' and not '<=>', and the carriers are held per name.
static bool demand_ordering(Checker *c, const LhatNode *at, LhatType *left,
                            LhatType *right)
{
    ParamVar *left_var = chk_param_var_for(c, left);
    ParamVar *right_var = chk_param_var_for(c, right);
    if (left_var == NULL && right_var == NULL) {
        return true;  // nothing here to decide; the judgement below stands
    }

    Known known;
    known.on_right = left_var != NULL;
    LhatType *other = left_var != NULL ? right : left;
    known.type = (chk_operator_undecided(other) ||
                  chk_param_var_for(c, other) != NULL)
                     ? NULL
                     : other;

    Candidates found = operator_candidates(c, "<=>", 3, &known);
    if (found.count == 0) {
        return true;  // nothing orders these; the report below says so
    }
    if (left_var != NULL) {
        chk_constrain(c, left, found.left);
    }
    if (right_var != NULL) {
        chk_constrain(c, right, found.right);
    }
    if (found.count > 1) {
        // As an arithmetic use of the same shape: the demands name the
        // candidates, and the writer picks one by annotating or narrowing.
        chk_report(c, at, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
        return false;
    }
    return true;
}

static LhatType *infer_operator_inner(Checker *c, const LhatNode *node, LhatOpKind op,
                                LhatType *left, LhatType *right)
{
    size_t length = 0;
    const char *name = chk_operator_name(op, &length);
    if (name == NULL) {
        return NULL;
    }

    unsigned errors = operator_error_sides(c, left, right, name, length);
    if (errors != 0) {
        chk_report(c, (errors & 1u) != 0 ? node->v.binary.left : node->v.binary.right,
                   LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR);
        return NULL;
    }

    // [pipeline.infer]: the left operand is the receiver ([syntax.obj.self]), so an operator used
    // on a parameter is a demand on it -- and what it demands is what may
    // carry the operator here. '..' is the one name that demands nothing
    // ([errors.runtime.div-zero] gives it composition as well as concatenation, and only one of
    // the two is an op^ to count).
    //
    // [pipeline.infer.expected]: with [syntax.op.define]'s built-in the only candidate the demand is number^,
    // which is what it has always been. Where the unit writes an op^ of this
    // name the demand is the union, and the body below then says the operator
    // is not settled -- but the signature names the candidates, so a writer
    // can annotate one or narrow to it ([syntax.type.fits]).
    if (chk_param_var_for(c, left) != NULL) {
        if (name[0] == '.') {
            return NULL;
        }
        // [pipeline.infer.operators]: the right operand narrows the field where it is settled --
        // 'x * 2' asks only for arms that take a number^.
        Known known;
        known.on_right = true;
        known.type = (chk_operator_undecided(right) ||
                      chk_param_var_for(c, right) != NULL)
                         ? NULL
                         : right;
        Candidates found = operator_candidates(c, name, length, &known);
        if (found.count == 0) {
            return NULL;  // nothing takes it; the reports below say so
        }
        chk_constrain(c, left, found.left);
        if (found.count > 1) {
            // Several could be meant and nothing here says which. The demands
            // still go on, so the signature names them and a writer can pick
            // one -- by annotating, or by narrowing to it in the body ([syntax.type.fits])
            // -- and the answer is what any of them would give.
            chk_constrain(c, right, found.right);
            chk_report(c, node, LHAT_CHECK_ERR_OPERATOR_UNSETTLED);
            return found.answer;
        }
        left = found.left;
    }

    if (chk_operator_undecided(left)) {
        return NULL;
    }

    LhatType *carrier = left_operator_member(c, left, name, length);
    if (carrier == NULL) {
        // [syntax.op.right]: nothing on the left, so the right operand gets the
        // question -- it may have been written as the receiver.
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        // [errors.runtime.index]: a nil^ arm is why the member was not found, rather than
        // an operator nobody wrote -- the same lookup answers without it.
        LhatType *bare = NULL;
        bool by_nil = nil_arm_apart(c, left, &bare) &&
                      left_operator_member(c, bare, name, length) != NULL;
        chk_report(c, node->v.binary.left,
                   by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                          : LHAT_CHECK_ERR_NO_OPERATOR);
        return NULL;
    }

    // [syntax.op.define] with [syntax.obj.override]: one type may answer an operator for several right-hand
    // types, and then the member is the intersection of those signatures.
    // Which arm a use means is settled the way a call's is -- by what is
    // handed over -- and the ban on overlapping signatures leaves at most one.
    // [syntax.obj.self] puts the left operand in self^, so the right one is the single
    // argument and each arm is asked the way a member call asks.
    //
    // Answering NULL here instead would fall back on [syntax.obj.number]'s number^ (the
    // caller's), which is not what the arm says and not what the machine
    // returns -- call_operator has always searched these.
    // [pipeline.infer]: with nothing decided about the right operand there is nothing
    // to choose an arm by -- but a group may still leave only one choice, and
    // then what that arm takes is a demand on this side exactly as a lone
    // signature's is. 'f^ x:A, y { x * y }' is the case: A writes both orders,
    // and only one of them is this one.
    if (carrier->kind == LHAT_TYPE_INTERSECT &&
        (chk_operator_undecided(right) || chk_param_var_for(c, right) != NULL)) {
        LhatType *only = sole_forward_arm(carrier);
        if (only == NULL) {
            return NULL;
        }
        carrier = only;
    }

    if (carrier->kind == LHAT_TYPE_INTERSECT) {
        LhatType *args[1] = { right };
        for (const LhatTypeList *arm = carrier->v.composite.arms; arm != NULL;
             arm = arm->next) {
            // [syntax.op.right]: the self^-last arms of this same group answer the
            // other order, so they are not candidates here.
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC &&
                arm->type->v.func.self_last) {
                continue;
            }
            if (chk_signature_accepts(arm->type, args, 1, true)) {
                return lhat_type_call_answer(arm->type);
            }
        }
        // [syntax.op.right]: no arm here takes it, so the right operand is asked
        // whether it was written as the receiver instead.
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        // [errors.runtime.index]: an arm does take it once the nil^ is off, so the
        // narrowing is what is missing rather than an arm for this type.
        LhatType *bare = NULL;
        bool by_nil = false;
        if (nil_arm_apart(c, right, &bare)) {
            args[0] = bare;
            by_nil = chk_operator_arm(carrier, args, 1, false) != NULL;
        }
        // Otherwise the same report the single arm makes below: what is wrong
        // is that nothing here takes this right operand.
        chk_report(c, node->v.binary.right,
                   by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                          : LHAT_CHECK_ERR_NO_OPERATOR);
        return NULL;
    }

    if (carrier->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    // [syntax.op.define]: a '-' written with self^ alone is the unary one, and it takes no
    // right operand at all. Asked here the same way an arm of the wrong shape
    // is: the right side is offered to [syntax.op.right] first, and refused after.
    if (carrier->v.func.params == NULL) {
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        chk_report(c, node->v.binary.right, LHAT_CHECK_ERR_NO_OPERATOR);
        return NULL;
    }
    LhatType *wanted = carrier->v.func.params->type;
    // [syntax.op.right]: this one does not take the right operand, so that side is
    // asked whether it was written as the receiver. Tried before expect so
    // that the answer is taken rather than reported -- and only when the left
    // has already failed, which is [syntax.op.semantic]'s order kept intact. A built-in on
    // the left lands here: number^ carries the arithmetic but takes only a
    // number^, so '1 + v' is exactly this case.
    if (wanted != NULL && !lhat_type_conforms(right, wanted)) {
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
    }
    // [pipeline.infer]: a parameter on this side is undecided in the sense above, but
    // what the operator takes is a demand on it rather than a gap to wave
    // through -- expect is what tells the two apart.
    if (wanted != NULL &&
        (chk_param_var_for(c, right) != NULL || !chk_operator_undecided(right))) {
        // [errors.runtime.index]: '1 + t[i]' reaches here, and what the operator takes is
        // met once the nil^ is off. Decided before expect so that the one
        // report it may make is the one that says so.
        LhatType *bare = NULL;
        bool by_nil = nil_arm_apart(c, right, &bare) &&
                      lhat_type_conforms(bare, wanted);
        chk_expect(c, node->v.binary.right, right, wanted,
                   by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                          : LHAT_CHECK_ERR_NO_OPERATOR);
    }
    return lhat_type_call_answer(carrier);
}

static LhatType *infer_operator(Checker *c, const LhatNode *node, LhatOpKind op,
                                LhatType *left, LhatType *right)
{
    size_t before = c->result->diagnostic_count;
    LhatType *answer = infer_operator_inner(c, node, op, left, right);
    for (size_t i = before; i < c->result->diagnostic_count; i++) {
        LhatCheckDiagnostic *d = &c->result->diagnostics[i];
        if (d->code != LHAT_CHECK_ERR_NO_OPERATOR) continue;
        size_t length = 0;
        d->name = chk_operator_name(op, &length);
        d->name_length = (uint32_t)length;
        d->left_type = type_text(c, left);
        d->right_type = type_text(c, right);
    }
    return answer;
}

// [modules.host.initial]: the member of L^ a host-bound name reaches, or NULL when the
// host bound no such name. Nothing is added to any scope for these -- they
// are asked for only where a scope answered nothing, which is what lets a
// let^ of the same spelling shadow one without anything being removed.
static const LhatTypeMember *initial_binding_member(Checker *c, const char *name,
                                      size_t length)
{
    for (size_t i = 0; i < c->require.initial_count; i++) {
        const char *bound = c->require.initial_names[i];
        if (bound == NULL || strlen(bound) != length ||
            memcmp(bound, name, length) != 0) {
            continue;
        }
        const char *member = c->require.initial_members[i];
        LhatType *env = chk_environment_type(c);
        if (member == NULL || env == NULL) {
            return NULL;
        }
        const LhatTypeMember *m = chk_find_member(env, member, strlen(member));
        return m;
    }
    return NULL;
}

void chk_nearest_in_scope(Checker *c, ChkNearest *near)
{
    for (const Scope *s = c->scope; s != NULL; s = s->parent) {
        for (const Binding *b = s->bindings; b != NULL; b = b->next) {
            // [syntax.stmt.old-world]: unreadable from its own initialiser, so offering it
            // there would offer the next diagnostic. A name that is not an
            // identifier ('...') is no one's misspelling.
            if (b->being_defined || b->name_length == 0 || b->name[0] == '.') {
                continue;
            }
            chk_nearest_offer(near, b->name, b->name_length);
        }
    }
    for (size_t i = 0; i < c->require.initial_count; i++) {
        const char *bound = c->require.initial_names[i];
        if (bound != NULL &&
            initial_binding_member(c, bound, strlen(bound)) != NULL) {
            chk_nearest_offer(near, bound, strlen(bound));
        }
    }
}

// [lsp.next]: a member the receiver does not hold, offered the nearest one it
// does -- written or built in, the two lists a '.' completes from. A probe
// (`node` NULL) looks for nothing, which is also what keeps the built-in
// walk below from recursing: it asks this function's caller quietly.
void chk_report_no_member(Checker *c, const LhatNode *node,
                             LhatType *target, const char *name,
                             size_t length)
{
    ChkNearest near;
    bool looking = chk_nearest_start(c, &near, name, length);
    if (node != NULL && looking) {
        lhat_check_written_members(target, chk_nearest_member, &near);
        lhat_check_builtin_members(c->result, target, chk_nearest_builtin,
                                   &near);
    }
    chk_report_near(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length,
                    node != NULL ? node->v.access.argument : NULL, &near);
}

// [syntax.obj.new]: what a template holds as a field. A method is a member of the
// definition and not something a construction writes.
static void offer_field(void *context, const LhatTypeMember *member)
{
    if (!chk_takes_receiver(member->type)) {
        chk_nearest_member(context, member);
    }
}

// [errors.try.result] with [pipeline.infer]: an error on its way out of the body being checked --
// through a try^, or through catch^ arms that did not take it. Where the
// result was written, this is where it is asked whether that admits it; where
// none was, this is one of the exits the result is the union of.
void chk_error_leaves(Checker *c, const LhatNode *at, LhatType *escaping)
{
    if (escaping == NULL) {
        return;
    }
    // [errors.kind.local-no-propagate]: before asking whether this subroutine's result admits the
    // error, ask whether any result could. A localerror^ has to be resolved
    // in the frame that raised it, so widening the signature is not the fix
    // it is for [pipeline.bc.call] -- [errors.kind.local-no-propagate] refuses that spelling as well. Reported here
    // rather than at the caller, for [pipeline.bc.call]'s reason: the try^ is what is
    // trying to return it.
    //
    // This is also the inference path (below), which is where a body with no
    // written result would otherwise have laundered it into one.
    if (chk_type_touches_local(escaping, 0)) {
        chk_report(c, at, LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES);
        return;
    }
    if (c->declared_result != NULL) {
        if (!lhat_type_conforms(escaping, c->declared_result)) {
            chk_report(c, at, LHAT_CHECK_ERR_TRY_OUTSIDE);
        }
        return;
    }
    // [syntax.type.tuple]'s width check is the return^ side's business: a union of a tuple
    // with an error kind is what [errors.fallible.union] admits, so the arm added here is
    // not one to measure.
    c->inferred_result =
        lhat_type_union(c->result->types, c->inferred_result, escaping);
}

// One binding selection for both ordinary and flow-narrowed reads. Narrowing
// changes the value type, never the declaration denoted by the source name.
static Binding *read_binding_from(Scope *from, const char *name, size_t length,
                                   Scope **found_in)
{
    Binding *b = chk_scope_find(from, name, length, found_in);
    if (b != NULL && b->being_defined && *found_in != NULL) {
        Scope *outer = NULL;
        Binding *shadowed = chk_scope_find((*found_in)->parent, name, length, &outer);
        if (shadowed != NULL) {
            b = shadowed;
            *found_in = outer;
        }
    }
    return b;
}

LhatType *chk_infer_name(Checker *c, const LhatNode *node,
                         LhatType **named_type)
{
    ((LhatNode *)node)->checked_binding = NULL;
    ((LhatNode *)node)->checked_scope_invalid = false;
    ((LhatNode *)node)->checked_this_body = NULL;
    ((LhatNode *)node)->checked_host_member = NULL;
    ((LhatNode *)node)->checked_module_root = NULL;
    ((LhatNode *)node)->checked_import_global = false;
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node, &name, &length)) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // [lex.hat.repeat]: the stacked reach. this^^ walks the chain of
    // enclosing bodies; it^^/self^^/def^^ walk past inner bindings of the
    // same name. Retain the chosen identity so emission does not count the
    // bindings or bodies again. The parser admits no other word here.
    if (node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats > 1) {
        size_t levels = node->v.name.hats - 1;
        if (chk_name_is(name, length, "this^")) {
            struct ThisLink *link = c->this_link;
            for (size_t i = 0; i < levels && link != NULL; i++) {
                link = link->outer;
            }
            if (link == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            ((LhatNode *)node)->checked_this_body = link->body;
            return link->type;
        }
        Binding *outer = chk_scope_find_skipping(c->scope, name, length, levels);
        if (outer == NULL) {
            chk_report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        outer->reached = true;
        ((LhatNode *)node)->checked_binding = outer->declaration;
#if LHAT_WITH_RESOLUTIONS
        chk_record_resolution(c, node, outer);
#endif
        return outer->type;
    }

    // A few hat identifiers are values rather than names ([lex.hat.one-use]).
    if (node->kind == LHAT_NODE_HAT_IDENT) {
        // [syntax.type.discard]: '_^' is written where a name would go and binds nothing, so
        // reaching it as a value asks for what was thrown away. Every place
        // that may write it takes it before inference is asked; arriving here
        // means it stood in an expression.
        if (chk_name_is(name, length, "_^")) {
            chk_report(c, node, LHAT_CHECK_ERR_DISCARD_READ);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        // [lsp.hover]: these three are the ones deliberately left unrecorded.
        // What a resolution feeds is a reader asking what a name means, and
        // here the spelling is already the answer -- recording it would grow
        // the array without telling anyone anything. Every other early
        // answer below does record, since none of them can be read off the
        // word.
        if (chk_name_is(name, length, "true^") || chk_name_is(name, length, "false^")) {
            return chk_simple(c, LHAT_TYPE_BOOL);
        }
        if (chk_name_is(name, length, "nil^")) {
            return chk_simple(c, LHAT_TYPE_NIL);
        }
        // [syntax.sub.this]: this^ names the subroutine running, which is what lets a
        // body with no name recurse. Only the hatted spelling means it, so
        // an ordinary name `this` is untouched.
        if (chk_name_is(name, length, "this^")) {
            if (c->this_type == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_THIS_OUTSIDE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            // [pipeline.infer] counts it the same way a call by name is counted.
            c->saw_self_call = true;
            ((LhatNode *)node)->checked_this_body = c->this_link->body;
#if LHAT_WITH_RESOLUTIONS
            chk_record_typed_resolution(c, node, c->this_type);
#endif
            return c->this_type;
        }
        // [syntax.obj.super]: super^ is the member an override^ is replacing. Named on
        // its own it is an ordinary value, so [syntax.obj.self] applies and the receiver
        // is written out; called directly it is the bound form, which
        // infer_call takes.
        if (chk_name_is(name, length, "super^")) {
            if (c->super_type == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_SUPER_OUTSIDE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            ((LhatNode *)node)->checked_binding = c->super_binding;
#if LHAT_WITH_RESOLUTIONS
            chk_record_typed_resolution(c, node, c->super_type);
#endif
            return c->super_type;
        }
        // [modules.host.machine]: the machine's own table, there without being imported.
        if (chk_name_is(name, length, "L^")) {
            LhatType *env = chk_environment_type(c);
            if (env == NULL) {
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
#if LHAT_WITH_RESOLUTIONS
            chk_record_typed_resolution(c, node, env);
#endif
            return env;
        }
    }

    // [lex.scope]: a specifier says which scope to start the search in.
    // Without one it starts here, which is every other name.
    Scope *from = c->scope;
    if (node->kind == LHAT_NODE_SCOPE) {
        from = chk_scope_from(c->scope, node);
        if (from == NULL) {
            ((LhatNode *)node)->checked_scope_invalid = true;
            chk_report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
    }

    // Which scope answered, which three of the rules below have to know: [syntax.stmt.visible]
    // tells a name this body bound from one still waiting further out, and
    // 15.13 and [modules.hostvalue] each measure it against a boundary of their own.
    Scope *found_in = NULL;
    Binding *b = read_binding_from(from, name, length, &found_in);
    // [syntax.stmt.old-world]: a binding does not stand in its own initialiser -- anywhere in
    // it, a deferred body included. What it holds is being worked out right
    // here, so the name still means what it meant outside; recursion by the
    // bound name went with this ([syntax.sub.this]: this^ is the one spelling, and the
    // stronger one -- it carries the literal's own signature where the name
    // held only a seed). Where nothing outside answers, the read falls
    // through to the report below, which is the same one it always got.
    if (b == NULL) {
        // [modules.host.initial]: what the host bound before anything ran. Asked after
        // every scope, so a let^ of the same spelling shadows it -- and what
        // it reaches stays readable as L^.<member>, since [modules.host.nothing] keeps the hat
        // identifier out of the spellings a let^ can make.
        const LhatTypeMember *bound = initial_binding_member(c, name, length);
        if (bound != NULL) {
            ((LhatNode *)node)->checked_host_member = bound;
#if LHAT_WITH_RESOLUTIONS
            // [lsp.hover]: nothing here declared it -- the host did, in C --
            // so there is no place to point at, only the type it registered.
            // Which is the whole of what a reader meeting `print` wants.
            chk_record_typed_resolution(c, node, bound->type);
#endif
            return bound->type;
        }
        ChkNearest near;
        if (chk_nearest_start(c, &near, name, length)) {
            chk_nearest_in_scope(c, &near);
        }
        chk_report_near(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length, node,
                        &near);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // [syntax.stmt.visible]: the name is visible throughout the scope, which is what makes
    // mutual recursion work, but its value only exists once its let^ has run.
    // A subroutine body does not run where it is written, so a name bound
    // outside one may still be waiting when the body reads it -- which is the
    // whole of what "outside a deferred body" exempts.
    //
    // A name the body itself bound is not that. Once the body is called its
    // own statements run in order, so 'let^ t = t[i]' standing in one reads a
    // place holding nothing exactly as it does at the top level. Asking
    // `deferred` alone put every body's own bindings under the exemption, and
    // what came of it was a run-time type error where the rule already said
    // there was a mistake to report.
    if (!b->reached) {
        // [syntax.stmt.old-world]: a being-defined binding nothing outer shadows reports
        // outright wherever it is read -- its own value cannot answer, and
        // the ring could never improve on that.
        if (b->being_defined || c->deferred == 0 ||
            chk_scope_within_body(c, found_in)) {
            chk_report(c, node, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
        } else {
            // [pipeline.infer.rounds]: what answered is the seed collect_bindings put
            // there, or what an earlier walk left. Either way this walk read
            // ahead of itself, so walking the statements again from what it
            // learned may answer better -- the same fact a def^ member's seed
            // reports when it is the one answering.
            c->read_provisional = true;
        }
    }
    // [modules.hostvalue]: a name bound outside this body reaches the value through a
    // capture, and a capture outlives the frame the slots belong to. The
    // same boundary test 15.1 uses for writes, asked of a read here because
    // for a host value the read is what would build the upvalue.
    if (chk_is_hostvalue(b->type) && c->body_scope != NULL) {
        if (!chk_scope_within_body(c, found_in)) {
            chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
    }
    ((LhatNode *)node)->checked_binding = b->declaration;
    ((LhatNode *)node)->checked_definition = b->definition;
    ((LhatNode *)node)->checked_module_root = b->module_root;
    ((LhatNode *)node)->checked_import_global = b->import_root &&
        c->body_scope != NULL && !chk_scope_within_body(c, found_in);
#if LHAT_WITH_RESOLUTIONS
    chk_record_resolution(c, node, b);
#endif
    if (named_type != NULL && b->names_type) {
        *named_type = b->named_type;
    }
    return b->type;
}

// [syntax.op.concat-table]: the table [syntax.obj] does not reach -- a plain one, made by a
// literal rather than a def^ and registered by no host. The kinds a
// definition made keep [syntax.obj.compose]'s composition and [syntax.op.define]'s op^.. instead.
static bool concatenable_table(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_TABLE &&
           !type->v.table.is_definition && !type->v.table.from_definition &&
           !type->v.table.nominal && type->v.table.hostvalue_tag == NULL;
}

void chk_join_table_entry_types(Checker *c, const LhatType *table,
                                   LhatType **keys, LhatType **values)
{
    for (const LhatTypeMember *m = table->v.table.members; m != NULL;
         m = m->next) {
        *keys = lhat_type_union(c->result->types, *keys,
                                chk_simple(c, lhat_type_member_key_kind(m)));
        *values = lhat_type_union(c->result->types, *values, m->type);
    }
    if (table->v.table.variadic != NULL) {
        *keys = lhat_type_union(c->result->types, *keys,
                                chk_simple(c, LHAT_TYPE_NUMBER));
        *values = lhat_type_union(c->result->types, *values, table->v.table.variadic);
    }
    *keys = lhat_type_union(c->result->types, *keys, table->v.table.index_key);
    *values = lhat_type_union(c->result->types, *values, table->v.table.index_value);
}

// [syntax.op.concat-table]: what '..' between two plain tables answers. The sequence
// halves in order -- the left's positions, then the right's renumbered after
// them -- and the named keys from both sides, a name both carry reported
// ([syntax.obj.compose-ambiguous]'s rule, without the qualified spelling that saves a method there).
// An unbounded left ('t^{ T[] }') leaves the right's positions nowhere to
// be counted, so everything past it folds into one unbounded element type.
static LhatType *concat_table_types(Checker *c, const LhatNode *node,
                                    const LhatType *left,
                                    const LhatType *right)
{
    LhatType *joined = lhat_type_table(c->result->types);
    bool fold = left->v.table.variadic != NULL;
    LhatType *element = NULL;
    size_t position = 0;
    const LhatType *sides[2] = { left, right };
    for (size_t s = 0; s < 2; s++) {
        size_t own = 0;
        for (const LhatTypeMember *m = sides[s]->v.table.members; m != NULL;
             m = m->next) {
            if (lhat_type_member_position(m, own)) {
                own++;
                if (fold) {
                    element = element == NULL
                                  ? m->type
                                  : lhat_type_union(c->result->types, element,
                                                    m->type);
                } else {
                    lhat_type_add_index_member(c->result->types, joined,
                                               position++, m->type);
                }
                continue;
            }
            // A named key, or a digits name standing off its position --
            // carried as it is, the way the machine carries a sparse index.
            if (chk_find_member(joined, m->name, m->name_length) != NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_CONCAT_COLLIDES);
                continue;
            }
            lhat_type_add_member(c->result->types, joined, m->name,
                                 m->name_length, m->type);
        }
        if (sides[s]->v.table.variadic != NULL) {
            fold = true;
            element = element == NULL
                          ? sides[s]->v.table.variadic
                          : lhat_type_union(c->result->types, element,
                                            sides[s]->v.table.variadic);
        }
    }
    joined->v.table.variadic = element;
    joined->v.table.entries_known = true;
    for (size_t s = 0; s < 2; s++) {
        const LhatType *side = sides[s];
        if (!side->v.table.entries_known &&
            (side->v.table.index_key == NULL || side->v.table.inferred_index)) {
            joined->v.table.entries_known = false;
        }
        if (left->v.table.index_key != NULL || right->v.table.index_key != NULL) {
            chk_join_table_entry_types(c, side, &joined->v.table.index_key,
                                    &joined->v.table.index_value);
        }
    }
    joined->v.table.inferred_index = !joined->v.table.entries_known;
    return joined;
}

static LhatType *successful_error_result(Checker *c, LhatType *type)
{
    if (type == NULL) return NULL;
    if (type->kind == LHAT_TYPE_UNION) {
        LhatType *result = NULL;
        for (const LhatTypeList *arm = type->v.composite.arms; arm; arm = arm->next) {
            result = lhat_type_union(c->result->types, result,
                                     successful_error_result(c, arm->type));
        }
        return result;
    }
    if (type->kind == LHAT_TYPE_UNKNOWN || type->kind == LHAT_TYPE_PENDING ||
        type->kind == LHAT_TYPE_ANY) {
        LhatType *result = chk_simple(c, type->kind == LHAT_TYPE_UNKNOWN
                                           ? LHAT_TYPE_PENDING : type->kind);
        result->excludes_error = true;
        return result;
    }
    return type;
}

// [syntax.obj.bits]: a bitwise operand is a number^ and nothing else -- no op^ reaches
// these. Whether it is a whole number within 64 bits is the machine's to ask.
static LhatType *expect_bitwise(Checker *c, const LhatNode *at,
                                LhatType *operand)
{
    LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);
    LhatType *bare = NULL;
    bool by_nil = nil_arm_apart(c, operand, &bare) &&
                  lhat_type_conforms(bare, number);
    chk_expect(c, at, operand, number,
               by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                      : LHAT_CHECK_ERR_NOT_NUMBER);
    return number;
}

// [syntax.match.other]: whether what a match's arms left of its subject is nothing, once the
// values they named are counted. A bool^ is spent by both of its literals,
// and an enum by every member of its own declaration -- a member of another
// enum spends nothing. Any other type left over is still a way to miss.
static bool match_covers(const LhatType *remaining, bool bool_spent,
                         const LhatType *const *members, size_t count)
{
    if (remaining == NULL) {
        return true;
    }
    if (remaining->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = remaining->v.composite.arms;
             arm != NULL; arm = arm->next) {
            if (!match_covers(arm->type, bool_spent, members, count)) {
                return false;
            }
        }
        return true;
    }
    if (remaining->kind == LHAT_TYPE_BOOL) {
        return bool_spent;
    }
    if (remaining->kind != LHAT_TYPE_ENUM) {
        return false;
    }
    size_t total = 0;
    size_t named = 0;
    for (const LhatTypeList *k = remaining->v.error.kinds; k != NULL;
         k = k->next) {
        bool seen = false;
        for (size_t i = 0; !seen && i < count; i++) {
            seen = members[i] == k->type;
        }
        total++;
        named += seen;
    }
    return named == total;
}

LhatType *chk_infer_binary(Checker *c, const LhatNode *node)
{
    LhatOpKind op = node->v.binary.op;

    // [errors.catch.form] and [syntax.op.default]: both drop one arm and put a value in its place.
    if (op == LHAT_OP_CATCH || op == LHAT_OP_NIL_ELSE) {
        LhatType *left = chk_infer(c, node->v.binary.left);
        // [errors.kind.local]: "an error" is either family here. catch^ resolves one
        // where it stands, which is exactly what a localerror^ asks for.
        LhatType *unwanted = op == LHAT_OP_CATCH ? chk_any_error(c)
                                                 : chk_simple(c, LHAT_TYPE_NIL);

        // [errors.catch.it]: catch^ names the error it^ inside its right side, so the
        // right side is inferred under a scope holding it -- typed as the
        // error half of the left, which is exactly what the machine puts in
        // that register (compile_catch). ?? has no it^: what it replaces is
        // nil^, and there is nothing in a nil^ to name.
        // [errors.catch.panic]: the panic^ arm answers nothing, because it does not
        // come back. Told apart here rather than in chk_infer, which would
        // be saying panic^ is an expression -- it is not, and [syntax.with.no-fail] stands.
        bool panicking = op == LHAT_OP_CATCH &&
                         node->v.binary.right != NULL &&
                         node->v.binary.right->kind == LHAT_NODE_PANIC;

        LhatType *right = NULL;
        if (op == LHAT_OP_CATCH) {
            Scope scope;
            chk_scope_open(&scope, c->scope, node, false);

            Scope *outer = c->scope;
            c->scope = &scope;
            Binding *caught = chk_scope_add(&scope, "it^", 3,
                                            chk_only(c, left, unwanted), node->offset);
            if (caught != NULL) {
                caught->reached = true;
                caught->declaration = node;
            }
            // Either way the right side is read under it^ -- 'panic^ it^'
            // is the form this exists for, and the value panic^ carries is
            // checked for its own sake the way the statement's is.
            right = panicking ? (chk_infer(c, node->v.binary.right->v.jump.value),
                                 (LhatType *)NULL)
                              : chk_infer(c, node->v.binary.right);
            c->scope = outer;
            chk_scope_close(c, &scope);
        } else {
            right = chk_infer(c, node->v.binary.right);
        }

        if (!chk_can_be(left, unwanted)) {
            chk_report(c, node, op == LHAT_OP_CATCH ? LHAT_CHECK_ERR_CANNOT_FAIL
                                                : LHAT_CHECK_ERR_CANNOT_BE_NIL);
        }
        LhatType *kept = chk_without(c, left, unwanted);
        if (op == LHAT_OP_CATCH) kept = successful_error_result(c, kept);
        // An untyped lookup can succeed with a value we know nothing about.
        // Handling its failure does not infer that value from the fallback.
        // Mark this result as an inference gap so strict bindings diagnose it.
        if (kept != NULL && kept->kind == LHAT_TYPE_UNKNOWN) {
            kept = chk_simple(c, LHAT_TYPE_PENDING);
        }
        // [errors.catch.panic]: nothing joins what the left is worth without its error
        // arm, so the whole is exactly that. 'x as^ T catch^ panic^ it^' is
        // T, which is what makes this the assertion. [syntax.type.tuple]'s width question
        // is skipped for the same reason -- there is no right-hand value to
        // measure against the left's.
        if (panicking) {
            return kept;
        }
        // [syntax.type.tuple] with [errors.catch.form]: catch^'s right side is one expression, and
        // there is no expression that is a tuple -- so a call answering
        // several values has no replacement that could stand for them. Said
        // here rather than left to the binding, which would only see that a
        // union is not a tuple and report the count. 'catch^ nil^' stays
        // writable ([errors.catch.ignore]'s deliberate ignoring): nil^ may stand beside
        // a tuple, and the result is discarded or discriminated, not bound.
        if (lhat_type_tuple_width(kept) > 0 &&
            lhat_type_tuple_width(right) != lhat_type_tuple_width(kept) &&
            !chk_may_stand_beside_tuple(right)) {
            chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
        }
        return lhat_type_union(c->result->types, kept, right);
    }

    LhatType *left = chk_infer(c, node->v.binary.left);

    // [syntax.obj.compose]: composition is written with '..' because the order matters. The
    // right side is not a value here -- it is a definition read against what
    // the left already provides, which is what [syntax.obj.override] needs to see.
    if (op == LHAT_OP_CONCAT && node->v.binary.right != NULL &&
        node->v.binary.right->kind == LHAT_NODE_DEF) {
        LhatType *result = chk_infer_def(c, node->v.binary.right, left);
        const LhatDefinition *a = node->v.binary.left->checked_definition;
        const LhatDefinition *b = node->v.binary.right->checked_definition;
        if (a != NULL && b != NULL) {
            LhatDefinition *joined = lhat_definition_new(c->result->types, NULL);
            if (joined != NULL) {
                joined->left = a;
                joined->right = b;
            }
            ((LhatNode *)node)->checked_definition = joined;
        }
        return result;
    }

    // [syntax.type.fits]: fits^ takes a type on the right, so the right side is not a
    // value. [errors.runtime.traceback] moved the type-fit question here from is^, which now
    // asks identity and reads an ordinary value on both sides (below, with
    // the rest of the comparisons).
    if (op == LHAT_OP_FITS) {
        ((LhatNode *)node)->checked_fits_type = left;
        LhatType *asked = chk_resolve_type(c, node->v.binary.right);
        if (chk_runtime_has_unresolved_arguments(asked)) {
            chk_report(c, node->v.binary.right, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
        }
        // [syntax.type.variadic]: any^ is the top of every value, so this holds of whatever is
        // on the left and the question is empty. [syntax.type.fits] refuses to read the
        // left's inferred type against the right -- that would narrow the
        // escape hatch [syntax.type.variadic] exists to provide -- but this one is decided by
        // the right side alone, whatever the left turns out to be.
        if (asked != NULL && asked->kind == LHAT_TYPE_ANY) {
            chk_report(c, node, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
        }
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // [syntax.type.fits]: and^'s right side runs only where the left held, and or^'s only
    // where it did not -- so what the left established about a path is known
    // inside the right, which is what makes 'x fits^ number^ and^ x <= 0.5'
    // read. Popped straight after: what the whole condition tells a branch is
    // the branch's own question, and chk_narrow_from answers it from the top.
    //
    // [syntax.op.precedence] の (5)'s chain does not come through here -- it is its own node --
    // so "a chain does not narrow" stays as it was.
    if (op == LHAT_OP_AND || op == LHAT_OP_OR) {
        LhatType *boolean = chk_simple(c, LHAT_TYPE_BOOL);
        chk_expect(c, node->v.binary.left, left, boolean,
                   LHAT_CHECK_ERR_NOT_BOOL);

        Narrowing *before = c->narrowings;
        chk_narrow_from(c, node->v.binary.left, op == LHAT_OP_AND);
        LhatType *other = chk_infer(c, node->v.binary.right);
        chk_expect(c, node->v.binary.right, other, boolean,
                   LHAT_CHECK_ERR_NOT_BOOL);
        chk_pop_narrowings(c, before);
        return boolean;
    }

    LhatType *right = chk_infer(c, node->v.binary.right);
    // [spec.number.operands]: written on every pass, so the last one is what stands.
    // The three left out reach call_operator even between numbers.
    ((LhatNode *)node)->checked_numeric =
        op != LHAT_OP_CONCAT && op != LHAT_OP_CROSS &&
        op != LHAT_OP_DOT_PRODUCT && left != NULL &&
        left->kind == LHAT_TYPE_NUMBER && right != NULL &&
        right->kind == LHAT_TYPE_NUMBER;

    switch (op) {
        case LHAT_OP_ADD:
        case LHAT_OP_SUB:
        case LHAT_OP_MUL:
        case LHAT_OP_CROSS:
        case LHAT_OP_DOT_PRODUCT:
        case LHAT_OP_DIV:
        case LHAT_OP_FLOORDIV:
        case LHAT_OP_MOD:
        case LHAT_OP_POW: {
            // [syntax.op.framework]: arithmetic asks [syntax.op.semantic]'s question the way '..' does, so a
            // written op^ answers it. [syntax.obj.number]'s number^ carries all seven built
            // in, which leaves ordinary arithmetic exactly as it was.
            LhatType *answer = infer_operator(c, node, op, left, right);
            return answer != NULL ? answer : chk_simple(c, LHAT_TYPE_NUMBER);
        }

        case LHAT_OP_BITAND:
        case LHAT_OP_BITOR:
        case LHAT_OP_BITXOR:
        case LHAT_OP_BITSHIFT:
            expect_bitwise(c, node->v.binary.left, left);
            return expect_bitwise(c, node->v.binary.right, right);

        // [syntax.op.compare]: the one comparison a type writes. Asked exactly the way
        // '..' and the arithmetic are, so an op^<=> on either side answers it
        // ([syntax.op.right]), and number^ and string^ carry their own.
        case LHAT_OP_SPACESHIP: {
            left = chk_require_value(c, node->v.binary.left, left);
            right = chk_require_value(c, node->v.binary.right, right);
            LhatType *answer = infer_operator(c, node, op, left, right);
            return answer != NULL ? answer : chk_simple(c, LHAT_TYPE_NUMBER);
        }

        case LHAT_OP_EQ:
        case LHAT_OP_IS:
        case LHAT_OP_NE:
        case LHAT_OP_LT:
        case LHAT_OP_GT:
        case LHAT_OP_LE:
        case LHAT_OP_GE:
            return check_comparison(c, node, op, left, right);

        case LHAT_OP_CONCAT: {
            // [syntax.op.concat]: '..' is concatenation in general. [syntax.op.semantic] asks the left
            // operand for it -- [syntax.obj.self] makes that the receiver -- and the right
            // one is the argument, which is what lets one type answer several
            // right-hand types through [syntax.obj.override]'s overload^.
            left = chk_require_value(c, node->v.binary.left, left);
            right = chk_require_value(c, node->v.binary.right, right);

            // [syntax.obj.compose]: between two definitions '..' composes, and never calls an
            // op^.. either of them carries. The literal form was taken above;
            // this is the one written with names.
            if (left != NULL && left->kind == LHAT_TYPE_TABLE &&
                left->v.table.is_definition && right != NULL &&
                right->kind == LHAT_TYPE_TABLE &&
                right->v.table.is_definition) {
                return chk_compose_definitions(c, node, left, right);
            }

            // [syntax.op.concat-table]: two plain tables concatenate, built in the way two
            // strings are -- the machine's own answer, never an op^.. .
            if (concatenable_table(left) && concatenable_table(right)) {
                return concat_table_types(c, node, left, right);
            }

            LhatType *joined = infer_operator(c, node, op, left, right);
            return joined != NULL ? joined : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        default:
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
}

// Whether a signature would take these arguments, asked without reporting.
// [syntax.obj.override] forbids overlapping signatures precisely so that at most one arm of
// an overloaded member can answer yes, which makes resolution a search rather
// than a ranking.
bool chk_signature_accepts(const LhatType *func, LhatType *const *args,
                           size_t count, bool through_member)
{
    if (func == NULL || func->kind != LHAT_TYPE_FUNC) {
        return false;
    }

    size_t declared = 0;
    for (const LhatTypeList *p = func->v.func.params; p != NULL; p = p->next) {
        declared++;
    }
    if (func->v.func.takes_self && !through_member) {
        declared++;  // written out at the call ([syntax.obj.self])
    }
    if (count < declared ||
        (count > declared && func->v.func.variadic == NULL)) {
        return false;
    }

    size_t skip = (func->v.func.takes_self && !through_member) ? 1 : 0;
    const LhatTypeList *param = func->v.func.params;
    for (size_t i = 0; i < count; i++) {
        if (skip > 0) {
            skip--;
            continue;
        }
        LhatType *wanted = param != NULL ? param->type : func->v.func.variadic;
        if (wanted != NULL && !lhat_type_conforms(args[i], wanted)) {
            return false;
        }
        if (param != NULL) {
            param = param->next;
        }
    }
    return true;
}

// [pipeline.infer.expected]: a subroutine written where it is called. The call is part of
// the same expression, so reading it leaves nothing. The ones elsewhere are
// [pipeline.infer.call-shapes]'s to read -- as shapes, one walk of the body per shape, never as
// demands folded into one signature -- and only for a unit-local let^: a
// published name's callers stay unread, which is what keeps a signature
// from depending on who happens to use it ([modules.public.types] rests on that).
//
// Refused for the two shapes whose arguments do not line up with the
// parameters one for one: a receiver ([syntax.obj.self]) is written first and belongs to
// no position, and a spread ([syntax.type.variadic]) stands for any number of them.
static bool immediately_called(Checker *c, const LhatNode *node)
{
    const LhatNode *literal = node->v.access.target;
    if (literal == NULL || literal->kind != LHAT_NODE_FUNC) {
        return false;
    }
    for (const LhatNode *param = literal->v.func.params; param != NULL;
         param = param->next) {
        if (chk_self_marker_at(c, literal->v.func.params, param) != 0) {
            return false;
        }
    }
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        if (arg->kind == LHAT_NODE_SPREAD) {
            return false;
        }
    }
    return true;
}

// [syntax.sub.no-contagion] with [syntax.type.coroutine]: the coroutine a call to this signature makes. The middle
// two types are whatever the body's yield^/await^ sites agreed on ([syntax.sub.yieldable]);
// a body with no yield^ at all -- only a await^ that never ran, or none
// reached -- leaves them NULL, which nil^ fills the same way an unwritten
// result does.
LhatType *chk_coroutine_made_by(Checker *c, const LhatType *func)
{
    // [syntax.type.coroutine]: the third type is what the last resume receives. Where the body
    // ends without a value it is left empty and [syntax.sub.coroutine-members]'s nil^ joins Y|T at
    // the resume instead -- so the nil^ stands where it is really received
    // rather than in the coroutine's own type. A body that cannot end has no
    // last resume at all, which `endless` says: nothing joins Y there, since
    // putting anything in would make every consumer narrow away a value that
    // never arrives.
    bool endless =
        func->v.func.result == NULL && !func->v.func.ends_without_value;
    // [syntax.sub.yieldable-f]: the coroutine carries the kind of the body it came from, which
    // is what decides who may advance it ([syntax.sub.coroutine-members]).
    //
    // R is left empty when nothing in the body received a yield^: no var^ took
    // one, so there is no value being sent in and a resume takes no argument.
    // Nothing unions R with anything, so there is nothing for a nil^ to be an
    // arm of -- filling one in would be inventing a parameter. Y is not the
    // same: a yield^ with no value really does hand nil^ to the resumer.
    LhatType *coroutine = lhat_type_coro(c->result->types, func->v.func.yield_receive,
                          func->v.func.yield_produce != NULL
                              ? func->v.func.yield_produce
                              : chk_simple(c, LHAT_TYPE_NIL),
                          func->v.func.result, endless,
                           func->v.func.is_function);
    if (coroutine != NULL) coroutine->source_body = func->source_body;
    return coroutine;
}

// [syntax.type.coroutine]: what one turn of a coroutine answers -- Y|T, which done() is what
// tells apart ([syntax.sub.coroutine-members]). The third slot has two ways of being empty and they
// answer differently: a body that cannot end never reaches a last resume, so
// nothing joins Y; one that ends without a value reaches it and is handed
// nil^ there, so that is where the nil^ comes in rather than in the type.
LhatType *chk_coroutine_answer(Checker *c, const LhatType *coro)
{
    LhatType *produce = coro->v.coroutine.produce;
    if (coro->v.coroutine.endless) {
        return produce;
    }
    LhatType *ends_with = coro->v.coroutine.result != NULL
                              ? coro->v.coroutine.result
                              : chk_simple(c, LHAT_TYPE_NIL);
    return lhat_type_union(c->result->types, produce, ends_with);
}

// The receiver of a call: what stands before the dot, or -- [syntax.obj.self], written
// out -- the first argument.
static const LhatNode *call_receiver(const LhatNode *node, const LhatType *callee)
{
    if (node->v.access.target->kind == LHAT_NODE_MEMBER) {
        return node->v.access.target->v.access.target;
    }
    return callee->v.func.takes_self && !callee->v.func.self_last
               ? node->v.access.argument : NULL;
}

void chk_hostvalue_temporary_write(Checker *c, const LhatNode *at,
                                            const LhatNode *receiver,
                                            const LhatType *type)
{
    // [modules.hostvalue]: an indexed or returned value is a copy, with no
    // location in its source to write back to. A reference keeps its identity.
    if (receiver != NULL && receiver->checked_binding == NULL && chk_is_hostvalue(type)) {
        chk_report(c, at, LHAT_CHECK_ERR_HOSTVALUE_TEMPORARY_WRITE);
    }
}

// [syntax.sub.mutable-self]: an f^ whose receiver seat is mutable writes through it, so an f^
// body may call one only on what it made itself -- [syntax.sub.yieldable-f]'s rule for a
// coroutine, reached through a call instead of a resume. [syntax.sub.table-write]: nothing says
// how far below its receiver such a body writes, so all of it has to be the
// body's -- except for [syntax.obj.table-ops]'s push^ and its kin, which change the receiver
// itself and nothing under it.
static void check_mutable_receiver(Checker *c, const LhatNode *node,
                                   const LhatType *callee)
{
    if (callee != NULL && callee->kind == LHAT_TYPE_FUNC && callee->v.func.takes_self &&
        (!callee->v.func.is_function || callee->v.func.mutable_self)) {
        const LhatNode *receiver = call_receiver(node, callee);
        chk_hostvalue_temporary_write(c, node, receiver,
                                         receiver != NULL ? receiver->checked_type : NULL);
    }
    if (!c->in_function || callee == NULL ||
        callee->kind != LHAT_TYPE_FUNC || !callee->v.func.is_function ||
        !callee->v.func.mutable_self) {
        return;
    }
    if (!chk_lean_on(c, call_receiver(node, callee), !callee->v.func.mutable_shallow)) {
        chk_report(c, node, LHAT_CHECK_ERR_MUTATES_OUTSIDE);
    }
}

// [syntax.sub.table-write]: once the arguments are read. A call counts as a table built around
// them -- what a new() answers holds what it was given -- and a mutable^self^
// call may put any of them under its receiver, one level down at the least.
static void own_after_call(Checker *c, const LhatNode *node,
                           const LhatType *callee,
                           const LhatCheckType *const *types, size_t count)
{
    const LhatNode *receiver = call_receiver(node, callee);
    uint8_t made = LHAT_OWN_DEEP;
    size_t i = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next, i++) {
        if (arg->kind == LHAT_NODE_SPREAD) {
            made = 0;
            break;
        }
        if (arg == receiver) {
            continue;
        }
        uint8_t brought = chk_made_depth(c, arg, i < count ? (const LhatType *)types[i] : NULL);
        made = brought < made ? brought : made;
    }
    ((LhatNode *)node)->checked_own = made == LHAT_OWN_DEEP ? made : (uint8_t)(made + 1);
    if (c->in_function && callee->v.func.is_function && callee->v.func.mutable_self) {
        chk_store_into(c, node, receiver, made);
    }
}

// [pipeline.infer.call-shapes]: the callee whose call shapes are collected -- a name a
// let^ bound to a literal, some parameter of which nothing was written on,
// with no receiver seat and no variadic tail (those two are the shapes [pipeline.infer.expected]
// already refuses to line up positionally). NULL for every other callee,
// which keeps today's reading.
void chk_retain_instance_failure(Checker *c, Instantiation *instance, size_t start)
{
    instance->failure = NULL;
    instance->failure_path = NULL;
    instance->advisory = true;
    size_t selected = start;
    for (size_t k = start; k < c->result->diagnostic_count; k++) {
        if (!c->result->diagnostics[k].relaxed_ok) {
            // An earlier inference gap must not hide a definite body error.
            if (instance->advisory) selected = k;
            instance->advisory = false;
        }
    }
    if (c->result->diagnostic_count <= start) return;
    LhatCheckDiagnostic *kept = lhat_type_semantic_alloc(c->result->types, sizeof *kept);
    if (kept != NULL) {
        *kept = c->result->diagnostics[selected];
        memset(kept->fixes, 0, sizeof kept->fixes);
        instance->failure = kept;
        instance->failure_path = c->lexer->source->name;
    }
}

typedef struct {
    Scope scope;
    const Scope *source;
} TemplateScope;

static Scope *template_scope(Checker *c, const Scope *source)
{
    if (source == NULL) return NULL;
    TemplateScope *kept_scope = lhat_type_semantic_alloc(c->result->types, sizeof *kept_scope);
    if (kept_scope == NULL) return NULL;
    kept_scope->source = source;
    Scope *copy = &kept_scope->scope;
    *copy = *source;
    copy->parent = template_scope(c, source->parent);
    copy->bindings = copy->tail = NULL;
    for (const Binding *b = source->bindings; b != NULL; b = b->next) {
        Binding *kept = lhat_type_semantic_alloc(c->result->types, sizeof *kept);
        if (kept == NULL) return NULL;
        *kept = *b;
        kept->next = NULL;
        if (copy->tail != NULL) copy->tail->next = kept;
        else copy->bindings = kept;
        copy->tail = kept;
    }
    return copy;
}

void chk_freeze_templates(Checker *c, const Scope *scope)
{
    for (LhatTemplate *t = c->templates; t != NULL; t = t->next) {
        for (Scope *kept = t->environment.scope; kept != NULL; kept = kept->parent) {
            TemplateScope *snapshot = (TemplateScope *)kept;
            if (snapshot->source != scope) continue;
            Scope *final = template_scope(c, scope);
            if (final == NULL) continue;
            // The initializer's own binding stays hidden, including when
            // its deferred body is instantiated after the scope has closed.
            for (const Binding *old = kept->bindings; old != NULL; old = old->next) {
                if (!old->being_defined) continue;
                for (Binding *b = final->bindings; b != NULL; b = b->next) {
                    if (b->declaration == old->declaration) b->being_defined = true;
                }
            }
            kept->bindings = final->bindings;
            kept->tail = final->tail;
            snapshot->source = NULL;
        }
    }
}

LhatType *chk_infer_template(Checker *c, const LhatNode *node)
{
    if (c->session) chk_report(c, node, LHAT_CHECK_ERR_TEMPLATE_CONTEXT);
    LhatTemplate *definition = c->templates;
    while (definition != NULL && definition->node != node) definition = definition->next;
    if (definition == NULL) {
        definition = lhat_type_semantic_alloc(c->result->types, sizeof *definition);
        if (definition == NULL) return chk_simple(c, LHAT_TYPE_UNKNOWN);
        definition->node = node;
        definition->next = c->templates;
        c->templates = definition;
        ((LhatNode *)node)->checked_instances = NULL;
    }
    Checker *saved = &definition->environment;
    LhatTemplate *nested = saved->templates;
    memset(saved, 0, sizeof *saved);
    saved->templates = nested;
    saved->lexer = c->lexer;
    saved->unit = c->unit;
    definition->module_name = c->result->module_name;
    saved->require = c->require;
    saved->scope = template_scope(c, c->scope);
    saved->environment = c->environment;
    saved->typeinfo_type = c->typeinfo_type;
    saved->cast_failure_type = c->cast_failure_type;
    saved->deferred = c->deferred;
    struct ThisLink **this_tail = &saved->this_link;
    for (const struct ThisLink *link = c->this_link; link != NULL; link = link->outer) {
        *this_tail = lhat_type_semantic_alloc(c->result->types, sizeof **this_tail);
        if (*this_tail == NULL) break;
        **this_tail = *link;
        this_tail = &(*this_tail)->outer;
        *this_tail = NULL;
    }
    struct SelfLink **self_tail = &saved->self_link;
    for (const struct SelfLink *link = c->self_link; link != NULL; link = link->outer) {
        *self_tail = lhat_type_semantic_alloc(c->result->types, sizeof **self_tail);
        if (*self_tail == NULL) break;
        **self_tail = *link;
        self_tail = &(*self_tail)->outer;
        *self_tail = NULL;
    }
    LhatType *signature = lhat_type_func(c->result->types, node->v.func.is_function);
    for (const LhatNode *p = node->v.func.params; p != NULL; p = p->next) {
        if (p->v.param.variadic || chk_self_marker_at(c, node->v.func.params, p) != 0) {
            chk_report(c, p, LHAT_CHECK_ERR_TEMPLATE_CONTEXT);
        }
        LhatType *type = p->v.param.type != NULL
                            ? chk_resolve_type(c, p->v.param.type)
                            : chk_simple(c, LHAT_TYPE_PENDING);
        lhat_type_add_param(c->result->types, signature, type);
    }
    if (node->v.func.return_type != NULL) {
        c->tuple_allowed = true;
        signature->v.func.result = chk_resolve_type(c, node->v.func.return_type);
    } else {
        signature->v.func.result = chk_simple(c, LHAT_TYPE_PENDING);
    }
    signature->template_definition = definition;
    definition->signature = signature;
    ((LhatNode *)node)->checked_type = signature;
    return signature;
}

static Instantiation *instantiate_template(Checker *c, LhatTemplate *definition,
                                          LhatType *const *args, size_t count)
{
    // Expanding recursive type shapes must not exhaust the native stack.
    if (c->template_depth >= LHAT_MAX_PROTOTYPE_DEPTH) return NULL;
    Checker concrete = definition->environment;
    concrete.template_depth = c->template_depth + 1;
    LhatCheckResult result;
    memset(&result, 0, sizeof result);
    result.types = c->result->types;
    result.strict = c->result->strict;
    result.module_name = (char *)definition->module_name;
    concrete.result = &result;
    concrete.instantiations = definition->instances;
    Instantiation *instance = chk_instantiation_add(&concrete, definition->node, args, count);
    definition->instances = concrete.instantiations;
    if (instance != NULL && !instance->checking) {
        instance->checking = true;
        LhatType *shape = lhat_type_func(result.types, definition->node->v.func.is_function);
        for (size_t k = 0; k < count; k++) lhat_type_add_param(result.types, shape, args[k]);
        concrete.expected_func = shape;
        instance->concrete->body->v.func.is_template = false;
        instance->signature = chk_infer_func(&concrete, instance->concrete->body);
        instance->concrete->signature = instance->signature;
        lhat_type_instance_contract(result.types, definition->node->checked_instances);
        instance->failed = result.diagnostic_count != 0;
        c->read_provisional = c->read_provisional || concrete.read_provisional;
        chk_retain_instance_failure(&concrete, instance, 0);
        instance->checking = false;
        definition->instances = concrete.instantiations;
        definition->environment.templates = concrete.templates;
    }
    result.module_name = NULL; // Borrowed from the defining unit.
    lhat_check_result_dispose(&result);
    return instance;
}

static const LhatNode *instantiable_callee(Checker *c, const LhatNode *node,
                                           const LhatType *callee)
{
    const LhatNode *target = node->v.access.target;
    const char *name = NULL;
    size_t length = 0;
    if (target == NULL || target->kind != LHAT_NODE_IDENT ||
        (callee->kind == LHAT_TYPE_FUNC &&
         (callee->v.func.takes_self || callee->v.func.variadic != NULL)) ||
        !chk_node_name(c, target, &name, &length)) {
        return NULL;
    }
    Binding *b = chk_scope_find(c->scope, name, length, NULL);
    if (b == NULL || !b->immutable || b->being_defined ||
        b->value_node == NULL || b->value_node->kind != LHAT_NODE_FUNC) {
        return NULL;
    }
    const LhatNode *literal = b->value_node;
    bool unwritten = false;
    for (const LhatNode *param = literal->v.func.params; param != NULL;
         param = param->next) {
        if (chk_self_marker_at(c, literal->v.func.params, param) != 0 ||
            param->v.param.variadic) {
            return NULL;
        }
        if (param->v.param.type == NULL) {
            unwritten = true;
        }
    }
    return unwritten ? literal : NULL;
}

// [syntax.obj.abstract] with [syntax.obj.delegate]: whether what this definition's delegate^ lends
// carries the name. A delegate lends a name without giving it: the link hangs
// off the instance (link_delegate), and what it reaches is the value in a
// field, which the program may swap between one lookup and the next. So
// nothing lent answers an abstract^, and nothing lent stands under an
// override^ for super^ to reach -- a declaration is asked of the definition,
// and a lent member is not the definition's. Asked only when new is refused,
// to say which of those it was.
static bool lent_by_delegate(const LhatType *definition, const char *name,
                             size_t length)
{
    const LhatType *instance = chk_instance_of(definition);
    if (instance == NULL || instance->kind != LHAT_TYPE_TABLE ||
        instance->v.table.delegate == NULL) {
        return false;
    }
    const LhatTypeMember *m =
        lhat_type_find_member(instance->v.table.delegate, name, length);
    return m != NULL && !m->abstract && !m->pending && !m->ambiguous &&
           lhat_type_takes_receiver(m->type);
}

// One parameter position of a call, filled by `actual` -- a written argument
// or one position of a spread tuple, which are the same thing here. The
// receiver's goes unasked ([syntax.obj.self]: its type is the call site's own); the rest
// are matched against what they fill. Answers the parameter after it.
static const LhatTypeList *fill_position(Checker *c, const LhatNode *at,
                                         const LhatType *callee,
                                         const LhatTypeList *param,
                                         size_t *skip, LhatType *actual)
{
    if (*skip > 0) {
        (*skip)--;
        return param;
    }
    // [modules.hostvalue]: the variadic tail collects into a table for an L^ body
    // and erases the width for a host's -- either way the seat cannot
    // say a host value's type, so one is boxed to ride it.
    if (param == NULL && chk_is_hostvalue(actual)) {
        chk_report(c, at, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
    }
    LhatType *wanted = param != NULL ? param->type : callee->v.func.variadic;
    if (wanted != NULL) {
        chk_expect(c, at, actual, wanted, LHAT_CHECK_ERR_MISMATCH);
    }
    return param != NULL ? param->next : NULL;
}

static LhatType *host_call_answer(Checker *c, const LhatNode *node,
                                  const LhatType *signature,
                                  const LhatCheckType *const *arguments, size_t count)
{
    bool dependent = lhat_type_has_arguments(signature->v.func.result);
    if (signature->v.func.instantiation_handler == NULL && !dependent) {
        return chk_call_answer(c, signature);
    }
    const LhatType *receiver = NULL;
    if (signature->v.func.takes_self && node->v.access.target->kind == LHAT_NODE_MEMBER) {
        receiver = chk_infer(c, node->v.access.target->v.access.target);
    }
    if (signature->v.func.takes_self && receiver == NULL && count > 0) {
        receiver = arguments[0];
        arguments++;
        count--;
    }
    LhatInstantiationContext context = { c->result->types, c->require.hosted, 0 };
    if (dependent) {
        LhatType *answer = lhat_type_instantiate_result(c->result->types,
            signature->v.func.result, arguments, count);
        if (answer == NULL ||
            lhat_type_tuple_arm_width(answer) != lhat_type_tuple_arm_width(signature->v.func.result) ||
            lhat_type_hostvalue_arm(answer) != lhat_type_hostvalue_arm(signature->v.func.result)) {
            chk_report(c, node, LHAT_CHECK_ERR_SHAPE_REFUSED);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        signature = lhat_check_signature_result(&context, signature, answer);
        if (signature == NULL) return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    const LhatCheckType *resolved = NULL;
    LhatInstantiationStatus status = LHAT_INSTANTIATION_DEFAULT;
    if (signature->v.func.instantiation_handler != NULL) {
        status = signature->v.func.instantiation_handler(
            &context, signature->v.func.instantiation_context, signature, receiver,
            arguments, count, &resolved);
    }
    ((LhatNode *)node)->checked_transfer_arguments = context.transfer_arguments;
    if (status == LHAT_INSTANTIATION_DEFAULT) {
        if (!dependent) return chk_call_answer(c, signature);
        resolved = signature;
        status = LHAT_INSTANTIATION_RESOLVED;
    }
    if (status == LHAT_INSTANTIATION_PENDING) return chk_simple(c, LHAT_TYPE_UNKNOWN);
    // A handler refines types, never the ABI or the procedure/function effect.
    if (status != LHAT_INSTANTIATION_RESOLVED || resolved == NULL ||
        resolved->kind != LHAT_TYPE_FUNC ||
        resolved->v.func.params != signature->v.func.params ||
        resolved->v.func.variadic != signature->v.func.variadic ||
        resolved->v.func.is_function != signature->v.func.is_function ||
        resolved->v.func.takes_self != signature->v.func.takes_self ||
        resolved->v.func.self_last != signature->v.func.self_last ||
        resolved->v.func.yields != signature->v.func.yields ||
        lhat_type_tuple_arm_width(resolved->v.func.result) !=
            lhat_type_tuple_arm_width(signature->v.func.result) ||
        !lhat_type_conforms(resolved->v.func.result, signature->v.func.result)) {
        chk_report(c, node, LHAT_CHECK_ERR_SHAPE_REFUSED);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
#if LHAT_WITH_RESOLUTIONS
    // Record what this call actually used, without changing the registered
    // function type or the type shown at its declaration.
    const LhatNode *name = node->v.access.target;
    if (name->kind == LHAT_NODE_MEMBER) name = name->v.access.argument;
    if (name != NULL) {
        for (size_t i = c->result->resolution_count; i > 0; i--) {
            LhatResolution *at = &c->result->resolutions[i - 1];
            if (at->use == name->offset && at->use_end == name->end) {
                at->call_signature = resolved;
                break;
            }
        }
    }
#endif
    return chk_call_answer(c, resolved);
}

// Only effect differs: keep the value's union type, but check a call against
// its procedure arm. Do not generalize this to overload selection or yields.
static LhatType *effect_union_signature(LhatType *type, LhatType **pure)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) return NULL;
    LhatType *procedure = NULL;
    *pure = NULL;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        LhatType *signature = arm->type;
        if (signature == NULL || signature->kind != LHAT_TYPE_FUNC ||
            signature->v.func.yields || signature->template_definition != NULL ||
            signature->v.func.instantiation_handler != NULL) return NULL;
        if (signature->v.func.is_function) {
            if (*pure != NULL) return NULL;
            *pure = signature;
        } else {
            if (procedure != NULL) return NULL;
            procedure = signature;
        }
    }
    if (*pure == NULL || procedure == NULL) return NULL;
    LhatType comparable = **pure;
    comparable.v.func.is_function = false;
    return lhat_type_equal(&comparable, procedure) ? procedure : NULL;
}

LhatType *chk_infer_call(Checker *c, const LhatNode *node)
{
    ((LhatNode *)node)->checked_transfer_arguments = 0;
    ((LhatNode *)node)->checked_instance = NULL;
    ((LhatNode *)node)->checked_arm = 0;
    ((LhatNode *)node)->checked_receiver = NULL;
    ((LhatNode *)node)->checked_super_call = chk_is_super_name(c, node->v.access.target);
    if (node->checked_super_call) {
        Binding *receiver = chk_scope_find(c->scope, "self^", 5, NULL);
        ((LhatNode *)node)->checked_receiver = receiver != NULL ? receiver->declaration : NULL;
    }
    // [pipeline.infer.expected]: the arguments first where the callee is a literal, so what they
    // are is known before the body that takes them is read. Inferred once --
    // the loop below reads them back rather than asking again, since asking
    // twice would report twice (the same reason [syntax.obj.override]'s arm search keeps
    // them).
    LhatType *given_types[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t given_count = 0;
    bool seeded = immediately_called(c, node);
    if (seeded) {
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            if (given_count >= LHAT_CHECK_MAX_TRACKED_ARGS) {
                seeded = false;  // more than worth keeping; read them as ever
                break;
            }
            given_types[given_count++] = chk_infer(c, arg);
        }
    }
    if (seeded) {
        LhatType *expected = lhat_type_func(
            c->result->types, node->v.access.target->v.func.is_function);
        for (size_t i = 0; i < given_count; i++) {
            lhat_type_add_param(c->result->types, expected, given_types[i]);
        }
        c->expected_func = expected;
    }

    LhatType *callee = chk_infer(c, node->v.access.target);
    c->expected_func = NULL;
    // Optional calls guard absence; ordinary calls retain a nil diagnostic.
    callee = chk_access_target(c, node, callee);

    // [pipeline.infer.call-shapes]: the other place a call site is read. [pipeline.infer.expected] reads the one
    // immediate call; this reads a call of a let^-bound literal elsewhere in
    // the unit, recording its argument types as one shape for infer_func to
    // walk the body under. [modules.public.types] still holds at the unit's edge: an
    // exported name's callers live in units checked after this one, so no
    // shape ever reaches them and a published signature still does not
    // depend on who uses it.
    if (!seeded && callee != NULL &&
        (callee->kind == LHAT_TYPE_FUNC || callee->kind == LHAT_TYPE_INTERSECT)) {
        LhatTemplate *definition = callee->template_definition;
        const LhatNode *literal = definition != NULL ? definition->node
                                  : instantiable_callee(c, node, callee);
        if (literal != NULL) {
            size_t count = 0;
            for (const LhatNode *arg = node->v.access.argument; arg != NULL;
                 arg = arg->next) {
                // A literal argument with an unwritten parameter reads its
                // expectation off the position it is written into ([pipeline.infer.expected]),
                // which a shape still being collected cannot give -- the
                // loop below infers those as ever. One written out in full
                // asks nothing of its position, so it rides along.
                bool defers = false;
                if (arg->kind == LHAT_NODE_FUNC) {
                    for (const LhatNode *p = arg->v.func.params; p != NULL;
                         p = p->next) {
                        if (p->v.param.type == NULL &&
                            chk_self_marker_at(c, arg->v.func.params, p) == 0) {
                            defers = true;
                        }
                    }
                }
                if (count >= LHAT_CHECK_MAX_TRACKED_ARGS ||
                    arg->kind == LHAT_NODE_SPREAD || defers) {
                    break;
                }
                given_types[count++] = chk_infer(c, arg);
            }
            // The loop below reads back what was inferred here whether or
            // not a shape comes of it -- asking twice would report twice.
            seeded = true;
            given_count = count;
            // Only a whole, settled shape is worth keeping: a gap or a tuple
            // in one is a reading of something not yet decided, and [pipeline.infer.rounds]'s
            // rounds bring this walk back once it is.
            size_t declared_here = 0;
            for (const LhatNode *p = literal->v.func.params; p != NULL;
                 p = p->next) {
                declared_here++;
            }
            bool takeable = count == declared_here;
            for (size_t i = 0; i < count && takeable; i++) {
                takeable = given_types[i] != NULL &&
                           !lhat_type_has_gap(given_types[i]) &&
                           lhat_type_tuple_width(given_types[i]) == 0;
            }
            Instantiation *inst =
                takeable ? (definition != NULL
                                ? instantiate_template(c, definition, given_types, count)
                                : chk_instantiation_add(c, literal, given_types, count))
                         : NULL;
            if (definition != NULL && inst == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_SHAPE_REFUSED);
            }
            if (inst != NULL && inst->signature != NULL) {
                // The body has been walked under this very shape, so what it
                // settled to there is this call's signature -- which is what
                // keeps an answer tied to what was handed in.
                callee = inst->signature;
                ((LhatNode *)node)->checked_arm = inst->arm;
                ((LhatNode *)node)->checked_instance = inst->concrete;
                if (inst->failed) {
                    size_t before = c->result->diagnostic_count;
                    chk_report(c, node, LHAT_CHECK_ERR_SHAPE_REFUSED);
                    if (c->result->diagnostic_count > before) {
                        c->result->diagnostics[before].cause = inst->failure;
                        c->result->diagnostics[before].cause_path = inst->failure_path;
                        c->result->diagnostics[before].relaxed_ok = inst->advisory;
                    }
                }
            } else if (inst != NULL) {
                // Recorded and not yet walked: ask for the round that walks
                // it, and let this round's answer stand as provisional.
                c->read_provisional = true;
            }
        }
    }

    size_t given = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        given++;
    }

    if (callee == NULL || callee->kind == LHAT_TYPE_UNKNOWN ||
        callee->kind == LHAT_TYPE_PENDING) {
        for (const LhatNode *arg = node->v.access.argument; arg != NULL && !seeded;
             arg = arg->next) {
            chk_infer(c, arg);
        }
        // [pipeline.strict.per-unit], [pipeline.strict.relaxed]、P6: a callee still pending^ (a mutually recursive
        // partner not yet checked) makes this call's result pending^ too,
        // not merely unknown^ -- strict needs to keep seeing the gap if it
        // survives to where a concrete type is wanted.
        return chk_simple(c, callee != NULL && callee->kind == LHAT_TYPE_PENDING
                              ? LHAT_TYPE_PENDING
                              : LHAT_TYPE_UNKNOWN);
    }

    // [syntax.obj.abstract]: an instance carries a value under every name its definition
    // holds, so one still only declared has nothing to make. Reported where
    // the construction is written, which is what the writer has to change.
    //
    // Asked before the callee's kind is looked at, so that a definition whose
    // new was written with an overload^ -- an intersection, which the branch
    // below answers and returns from -- is asked the same question.
    if (node->v.access.target != NULL &&
        node->v.access.target->kind == LHAT_NODE_MEMBER) {
        const char *called = NULL;
        size_t called_length = 0;
        if (chk_node_name(c, node->v.access.target->v.access.argument, &called,
                          &called_length) &&
                chk_name_is(called, called_length, "new")) {
            LhatType *owner = chk_infer(c, node->v.access.target->v.access.target);
            bool field = false;
            const LhatTypeMember *hole = chk_hole_of(owner, &field);
            if (hole != NULL) {
                // [syntax.obj.abstract-new]: a field has two ways to be given a value and a
                // member has one, so the two are not told the same thing.
                LhatCheckErrorCode code = field
                                              ? LHAT_CHECK_ERR_FIELD_UNPROVIDED
                                              : LHAT_CHECK_ERR_STILL_ABSTRACT;
                // [syntax.obj.abstract] and [syntax.obj.abstract-override]: a member the delegate lends is missing
                // for a reason the writer cannot see from where they stand --
                // the name is right there through the delegate -- so the
                // reason is what they are told.
                if (!field &&
                    lent_by_delegate(owner, hole->name, hole->name_length)) {
                    code = hole->abstract ? LHAT_CHECK_ERR_LENT_DOES_NOT_PROVIDE
                                          : LHAT_CHECK_ERR_LENT_IS_NOT_REPLACED;
                }
                // The missing member is not written at this call. Mark new,
                // independently of the member name included in the message.
                chk_report_named_span(c, node->v.access.target->v.access.argument,
                                      code, hole->name, hole->name_length);
            }
        }
    }

    // [syntax.obj.override]: an overloaded member is the intersection of its signatures, so
    // calling one means finding the arm that fits. Arguments are inferred
    // once here, since inferring them again would report twice.
    if (callee->kind == LHAT_TYPE_INTERSECT) {
        bool through_member =
            node->v.access.target->kind == LHAT_NODE_MEMBER;

        // [pipeline.infer.expected] with [syntax.obj.override]: which arm is meant is settled by the arguments'
        // own types, so in general there is no one position to read an
        // expectation off of. But when one arm alone takes this many
        // arguments, the count has already settled it -- and then a literal
        // argument takes its parameters from that arm the way it would from
        // an unoverloaded signature. The full answer is still searched for
        // below; this only decides what a literal with nothing written may
        // read.
        // [syntax.type.tuple]: a spread tuple counts as its positions, which are not known
        // until it is read -- so a call with one leaves this guess unmade.
        bool spreads = false;
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            spreads = spreads || arg->kind == LHAT_NODE_SPREAD;
        }
        const LhatType *by_arity = NULL;
        for (const LhatTypeList *arm = callee->v.composite.arms;
             arm != NULL && !spreads; arm = arm->next) {
            if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC) {
                continue;
            }
            size_t declared = 0;
            for (const LhatTypeList *p = arm->type->v.func.params; p != NULL;
                 p = p->next) {
                declared++;
            }
            if (arm->type->v.func.takes_self && !through_member) {
                declared++;
            }
            if (given == declared ||
                (given > declared && arm->type->v.func.variadic != NULL)) {
                if (by_arity != NULL) {
                    by_arity = NULL;
                    break;
                }
                by_arity = arm->type;
            }
        }
        const LhatTypeList *expect_param =
            by_arity != NULL ? by_arity->v.func.params : NULL;
        size_t expect_skip =
            by_arity != NULL && by_arity->v.func.takes_self && !through_member
                ? 1
                : 0;

        LhatType *args[LHAT_CHECK_MAX_TRACKED_ARGS];
        size_t tracked = 0;
        bool overflow = false;
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            // [syntax.type.tuple]: a spread tuple is its positions, each an argument the
            // arm is asked about. A table spread stays the one unknown it
            // always was here.
            if (arg->kind == LHAT_NODE_SPREAD) {
                LhatType *spread = chk_infer(c, arg->v.jump.value);
                size_t positions = lhat_type_tuple_width(spread);
                for (size_t i = 0; i < (positions > 0 ? positions : 1); i++) {
                    if (tracked == LHAT_CHECK_MAX_TRACKED_ARGS) {
                        overflow = true;
                        break;
                    }
                    args[tracked++] = positions > 0
                                          ? lhat_type_tuple_at(spread, i)
                                          : chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
                continue;
            }
            LhatType *outer_expected = c->expected_func;
            if (expect_skip > 0) {
                expect_skip--;
            } else if (by_arity != NULL) {
                c->expected_func = expect_param != NULL
                                       ? expect_param->type
                                       : by_arity->v.func.variadic;
                if (expect_param != NULL) {
                    expect_param = expect_param->next;
                }
            }
            LhatType *type = chk_infer(c, arg);
            c->expected_func = outer_expected;
            if (tracked < LHAT_CHECK_MAX_TRACKED_ARGS) {
                args[tracked++] = type;
            } else {
                overflow = true;
            }
        }
        if (overflow) {
            return chk_simple(c, LHAT_TYPE_UNKNOWN);  // more than worth tracking
        }
        size_t position = 0;
        for (const LhatTypeList *arm = callee->v.composite.arms; arm != NULL;
             arm = arm->next, position++) {
            if (chk_signature_accepts(arm->type, args, tracked, through_member)) {
                // [spec.arms]: once every argument's type is settled this is
                // the answer, not a guess -- a call fitting no arm is the
                // MISMATCH below. So the compiler may bake the arm in and let
                // the run skip [pipeline.bc.overload]'s search, under either strictness. An
                // argument that is any^, unknown or has a gap leaves nothing
                // written, and the search decides the call at run time.
                bool settled = true;
                for (size_t i = 0; i < tracked; i++) {
                    if (args[i] == NULL || args[i]->kind == LHAT_TYPE_UNKNOWN ||
                        args[i]->kind == LHAT_TYPE_ANY || lhat_type_has_gap(args[i])) {
                        settled = false;
                    }
                }
                if (settled && position + 1 <= UINT16_MAX) {
                    ((LhatNode *)node)->checked_arm = (uint16_t)(position + 1);
                }
                // [syntax.sub.kinds], [syntax.sub.no-contagion]: f^ may call only f^, whichever arm [syntax.obj.override]
                // resolved to -- except a yieldable p^, whose call alone
                // stays referentially transparent (see the plain-call site
                // above for why).
                if (c->in_function && !arm->type->v.func.is_function &&
                    !arm->type->v.func.yields) {
                    chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
                }
                check_mutable_receiver(c, node, arm->type);  // [syntax.sub.mutable-self]
                own_after_call(c, node, arm->type,
                               (const LhatCheckType *const *)args, tracked);
                if (arm->type->v.func.answers_fresh) {
                    ((LhatNode *)node)->call_answers_fresh = true;  // [syntax.sub.fresh]
                }
                // [syntax.sub.no-contagion]: a yielding arm answers the coroutine it makes, the
                // same as the plain call below.
                return host_call_answer(c, node, arm->type,
                                         (const LhatCheckType *const *)args, tracked);
            }
        }
        // No arm takes these: the arguments, against every arm at once.
        size_t before = c->result->diagnostic_count;
        chk_report(c, node, LHAT_CHECK_ERR_MISMATCH);
        if (c->result->diagnostic_count > before) {
            const char *parts[LHAT_CHECK_MAX_TRACKED_ARGS];
            for (size_t i = 0; i < tracked; i++) parts[i] = type_text(c, args[i]);
            const char *given_text = joined_text(c, parts, tracked, ", ", true);
            // Each arm by the parameters it asks for; the rest of its
            // signature is not what refused the call.
            const char *arms[LHAT_CHECK_MAX_TRACKED_ARGS];
            size_t arm_count = 0;
            for (const LhatTypeList *arm = callee->v.composite.arms;
                 arm != NULL && arm_count < LHAT_CHECK_MAX_TRACKED_ARGS; arm = arm->next) {
                size_t n = 0;
                for (const LhatTypeList *p = arm->type->kind == LHAT_TYPE_FUNC ? arm->type->v.func.params : NULL;
                     p != NULL && n < LHAT_CHECK_MAX_TRACKED_ARGS; p = p->next) {
                    parts[n++] = type_text(c, p->type);
                }
                arms[arm_count++] = joined_text(c, parts, n, ", ", true);
            }
            LhatCheckDiagnostic *d = &c->result->diagnostics[before];
            d->left_type = given_text;
            d->right_type = joined_text(c, arms, arm_count, " or ", false);
        }
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    bool effect_union = false;
    if (callee->kind == LHAT_TYPE_UNION) {
        LhatType *pure = NULL;
        LhatType *common = effect_union_signature(callee, &pure);
        if (common != NULL) {
            effect_union = true;
            if (c->in_function) {
                const char *signature = type_text(c, pure);
                chk_report_named_span(c, node->v.access.target,
                    LHAT_CHECK_ERR_FUNCTION_CALLS_UNION, signature,
                    signature != NULL ? strlen(signature) : 0);
            }
            callee = common;
        } else {
            chk_report(c, node, LHAT_CHECK_ERR_CALL_UNION_NARROW);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
    }
    if (callee->kind != LHAT_TYPE_FUNC) {
        chk_report(c, node, LHAT_CHECK_ERR_NOT_CALLABLE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // [syntax.sub.kinds]: f^ may call only f^, never p^ -- a p^ may run side effects an f^
    // is not allowed to. [syntax.sub.no-contagion] carves out one exception: calling a yieldable
    // p^ does not run its body at all, only makes a coroutine (start()
    // is what runs it, and that is p^ itself, caught the same way any other
    // p^ call is) -- so that call alone stays referentially transparent and
    // is not what this rule exists to catch. Reported here rather than
    // refused earlier so arguments still get checked, the same as an
    // ordinary mismatch.
    if (c->in_function && !effect_union && !callee->v.func.is_function &&
        !callee->v.func.yields) {
        chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    }
    check_mutable_receiver(c, node, callee);  // [syntax.sub.mutable-self]
    if (callee->v.func.answers_fresh) {
        ((LhatNode *)node)->call_answers_fresh = true;  // [syntax.sub.fresh]
    }

    const LhatTypeList *param = callee->v.func.params;

    size_t declared = 0;
    for (const LhatTypeList *p = param; p != NULL; p = p->next) {
        declared++;
    }

    // [syntax.obj.self]: 'x.m()' hands x to the receiver without writing it, while a
    // method taken as a value is called with the receiver spelled out. The
    // receiver is kept out of `params`, so only the second form adjusts.
    //
    // [syntax.obj.super]: 'super^(…)' is the first of the two. What it replaces is a
    // member of the same definition, so the receiver it wants is the one the
    // body already has -- writing it would be noise, and every use of super^
    // would carry it.
    size_t skip = 0;
    if (callee->v.func.takes_self &&
        node->v.access.target->kind != LHAT_NODE_MEMBER &&
        !chk_is_super_name(c, node->v.access.target)) {
        declared++;
        skip = 1;
    }

    // [syntax.type.variadic] with [syntax.type.tuple]: 'expr...' spreads. A tuple's width is its type's, so
    // it stands for that many arguments wherever it is written -- the fixed
    // parameters and the variadic tail alike, as if each position had been
    // written out. Anything else -- a table, or a value whose type is not
    // settled -- is as long as it turns out to be when it runs, so it can
    // only continue the variadic tail, and nothing may follow it. 'declared'
    // does not count the variadic one (v.func.variadic is kept apart from
    // `params`, the same way self^ is).
    size_t filled = 0;  // positions given, the receiver's included
    bool other_spread = false;
    bool tuple_spread = false;
    size_t taken = 0;   // arguments read, which given_types is indexed by
    const LhatCheckType *host_arguments[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t host_count = 0;
    bool host_overflow = false;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        if (arg->kind == LHAT_NODE_SPREAD) {
            LhatType *spread = chk_infer(c, arg->v.jump.value);
            size_t positions = lhat_type_tuple_width(spread);
            for (size_t i = 0; i < positions; i++) {
                if (host_count < LHAT_CHECK_MAX_TRACKED_ARGS) {
                    host_arguments[host_count++] = lhat_type_tuple_at(spread, i);
                } else {
                    host_overflow = true;
                }
                param = fill_position(c, arg, callee, param, &skip,
                                      lhat_type_tuple_at(spread, i));
                filled++;
            }
            if (positions > 0) {
                tuple_spread = true;
                continue;
            }
            other_spread = true;
            if (arg->next != NULL) {
                chk_report(c, arg, LHAT_CHECK_ERR_SPREAD_NOT_LAST);
            }
            if (callee->v.func.variadic == NULL) {
                chk_report(c, arg, LHAT_CHECK_ERR_NOT_VARIADIC);
            } else if (spread != NULL && spread->kind == LHAT_TYPE_TABLE &&
                       spread->v.table.variadic != NULL) {
                chk_expect(c, arg, spread->v.table.variadic,
                           callee->v.func.variadic, LHAT_CHECK_ERR_MISMATCH);
            } else if (spread == NULL || !lhat_type_has_gap(spread)) {
                chk_report(c, arg, LHAT_CHECK_ERR_SPREAD_NOT_SEQUENCE);
            }
            continue;
        }
        // [pipeline.infer.expected]: what this position takes, read before the argument rather
        // than after -- a subroutine literal written here has the callee's
        // written signature standing beside it, and takes the parameters it
        // has none written on from there. The receiver's position is not one
        // of these ([syntax.obj.self]), so nothing is expected there.
        LhatType *wanted =
            skip > 0 ? NULL
                     : (param != NULL ? param->type : callee->v.func.variadic);
        // [pipeline.infer.expected]: read back where the seeding above already inferred it.
        LhatType *actual;
        if (seeded && taken < given_count &&
            !(wanted != NULL && wanted->kind == LHAT_TYPE_TABLE && wanted->v.table.is_typeinfo)) {
            actual = given_types[taken];
        } else {
            LhatType *outer_expected = c->expected_func;
            c->expected_func = wanted;
            actual = chk_infer(c, arg);
            c->expected_func = outer_expected;
        }
        taken++;
        if (host_count < LHAT_CHECK_MAX_TRACKED_ARGS) {
            host_arguments[host_count++] = actual;
        } else {
            host_overflow = true;
        }
        filled++;
        // [syntax.type.tuple]: an argument is one value. A tuple in one is what would
        // bring back [syntax.type.variadic]'s expansion rule (Lua's truncate-except-in-tail-
        // position), and refusing it here is what keeps that rule from
        // arising. Said by name rather than left to the mismatch below,
        // which would report the position's type against the parameter's.
        if (skip == 0 && lhat_type_tuple_width(actual) > 0) {
            chk_report(c, arg, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        }
        param = fill_position(c, arg, callee, param, &skip, actual);
    }

    // [syntax.type.param-names]: a written default does not make a parameter optional. What it
    // fills in is a call site being written by an editor, so a call that
    // reaches here still owes every declared argument. [syntax.type.variadic]: a spread that
    // is not a tuple takes any number beyond them, so only falling short is
    // wrong there -- and into a callee with no tail to continue it was
    // reported above, the count following from that. A spread tuple is said
    // by name, since its positions are what the count is short or over by
    // and nothing written shows them.
    bool miscounted = filled < declared ||
                      (filled > declared && callee->v.func.variadic == NULL);
    if (other_spread) {
        miscounted = callee->v.func.variadic != NULL && filled < declared;
    }
    if (miscounted) {
        chk_report(c, node,
                   tuple_spread ? LHAT_CHECK_ERR_ARITY_SPREAD
                                : LHAT_CHECK_ERR_ARITY);
    }
    own_after_call(c, node, callee, host_arguments, host_count);

    // [syntax.sub.this]: when the body being checked is calling itself, its result is
    // still being worked out, and [pipeline.infer] reads the NULL to mean exactly
    // that -- so a self-call has to keep handing the NULL back.
    if (!callee->v.func.yields && callee->v.func.result == NULL &&
        callee == c->this_type) {
        return NULL;
    }
    if (!miscounted && !other_spread && !host_overflow) {
        return host_call_answer(c, node, callee, host_arguments, host_count);
    }
    return chk_call_answer(c, callee);
}

LhatType *chk_call_answer(Checker *c, const LhatType *callee)
{
    // [syntax.sub.no-contagion]: calling a yieldable procedure answers a coroutine rather than
    // running it. infer_func puts that on the signature as it settles the
    // three types, and every other reader takes it from there; a signature a
    // first pass seeded has none yet, and is assembled here instead.
    if (callee->v.func.yields) {
        return callee->v.func.answers != NULL ? callee->v.func.answers
                                              : chk_coroutine_made_by(c, callee);
    }
    // [syntax.type.empty]: a signature with no result answers no value. That is not a gap in
    // inference, so it is not spelled with a NULL -- [pipeline.infer] kept it apart
    // from nil^, and this is where that stays observable.
    if (callee->v.func.result == NULL) {
        return chk_simple(c, LHAT_TYPE_NONE);
    }
    return callee->v.func.result;
}

// [modules.hostvalue]: the one question every escape rule asks. [modules.hostvalue.union]: through a
// union's arms as well -- every one of those rules is about the width a
// place has room for, and 'Vector3|nil^' is as wide as the Vector3 in it
// whichever arm is live. The narrower question (is this type itself a host
// value) is asked by kind where it is really about identity: what members
// hang off it, what box^ takes, what a field write reaches.
bool chk_is_hostvalue(const LhatType *type)
{
    return lhat_type_hostvalue_arm(type) != NULL;
}

static LhatType *infer_error_construction(Checker *c, const LhatNode *node,
                                           LhatType *kind)
{
    for (const LhatNode *entry = node->v.named.members; entry != NULL;
         entry = entry->next) {
        LhatType *given = chk_infer(c, entry->v.entry.value);
        const char *name = NULL;
        size_t length = 0;
        if (kind == NULL || kind->kind != LHAT_TYPE_ERROR_KIND ||
            !chk_node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        // message and cause exist on every kind without being
        // declared ([errors.kind.made]); the rest have to have been.
        if (chk_name_is(name, length, "message")) {
            chk_expect(c, entry, given, chk_simple(c, LHAT_TYPE_STRING),
                       LHAT_CHECK_ERR_MISMATCH);
            continue;
        }
        if (chk_name_is(name, length, "cause")) {
            continue;
        }
        bool found = false;
        for (const LhatTypeMember *m = kind->v.error.fields; m != NULL;
             m = m->next) {
            if (m->name_length == length &&
                memcmp(m->name, name, length) == 0) {
                chk_expect(c, entry, given, m->type, LHAT_CHECK_ERR_MISMATCH);
                found = true;
                break;
            }
        }
        if (!found) {
            // [syntax.juxta.when]'s two are on every kind, so they are offered too.
            ChkNearest near;
            if (chk_nearest_start(c, &near, name, length)) {
                lhat_check_written_members(kind, chk_nearest_member,
                                           &near);
                chk_nearest_offer(&near, "message", 7);
                chk_nearest_offer(&near, "cause", 5);
            }
            chk_report_near(c, entry, LHAT_CHECK_ERR_NO_MEMBER, name,
                            length, entry->v.entry.key, &near);
        }
    }

    // [errors.kind.new]: a field without a default has to be written, since
    // there is nothing for it to fall back to.
    if (kind != NULL && kind->kind == LHAT_TYPE_ERROR_KIND) {
        for (const LhatTypeMember *m = kind->v.error.fields; m != NULL;
             m = m->next) {
            if (m->optional) {
                continue;
            }
            bool given = false;
            for (const LhatNode *entry = node->v.named.members;
                 entry != NULL && !given; entry = entry->next) {
                const char *name = NULL;
                size_t length = 0;
                given = chk_node_name(c, entry->v.entry.key, &name, &length) &&
                        length == m->name_length &&
                        memcmp(name, m->name, length) == 0;
            }
            if (!given) {
                chk_report(c, node, LHAT_CHECK_ERR_MISSING_FIELD);
            }
        }
    }
    return kind;
}

static LhatType *infer_node(Checker *c, const LhatNode *node,
                            LhatType **named_type);

// The one door every expression's type leaves through. [modules.hostvalue] with
// [pipeline.bc.checked-answers]: the compiler is otherwise type-blind, and a host value changes what
// it has to emit -- how many slots to reserve, how wide a move is -- so every
// expression whose type is one gets that type stamped on its node, the same
// checked_type channel infer_func already uses for a signature. Stamping only
// host values keeps the FUNC and TYPEOF stamps (whose kinds are never
// HOSTVALUE) untouched.
LhatType *chk_infer_with_named_type(Checker *c, const LhatNode *node,
                                    LhatType **named_type)
{
    if (node != NULL) ((LhatNode *)node)->checked_definition = NULL;
    if (named_type != NULL) {
        *named_type = NULL;
        // Only a spelling or a name path carries an alias. Nested calls to
        // chk_infer do not request this output, so a child cannot leak one.
        if (node == NULL || (node->kind != LHAT_NODE_TYPE_VALUE &&
                             !chk_run_of_names(node, true))) {
            named_type = NULL;
        }
    }
    LhatType *context_named = NULL;
    bool expects_descriptor = c->expected_func != NULL &&
        c->expected_func->kind == LHAT_TYPE_TABLE && c->expected_func->v.table.is_typeinfo;
    if (node != NULL) ((LhatNode *)node)->descriptor_type = NULL;
    LhatType **capture = named_type;
    if (capture == NULL && expects_descriptor && node != NULL && chk_run_of_names(node, true)) {
        capture = &context_named;
    }
    LhatType *outer_expected = c->expected_func;
    if (expects_descriptor) c->expected_func = NULL;
    LhatType *type = infer_node(c, node, capture);
    if (node != NULL) {
        ((LhatNode *)node)->checked_callable_type = type != NULL &&
            (type->kind == LHAT_TYPE_FUNC || type->kind == LHAT_TYPE_CORO ||
             type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT)
            ? type : NULL;
    }
    c->expected_func = outer_expected;
    if (expects_descriptor && capture != NULL && *capture == NULL &&
        node != NULL && chk_run_of_names(node, true) &&
        type != NULL && type->kind == LHAT_TYPE_TABLE && !type->v.table.is_typeinfo) {
        // Host declarations and def^ values carry their type through their
        // declaration shape rather than through an explicit alias marker.
        *capture = chk_resolve_type(c, node);
    }
    if (expects_descriptor && capture != NULL && *capture != NULL &&
        !(type != NULL && type->kind == LHAT_TYPE_TABLE && type->v.table.is_typeinfo)) {
        if (chk_runtime_has_unresolved_arguments(*capture)) {
            chk_report(c, node, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
        }
        ((LhatNode *)node)->descriptor_type = *capture;
        type = chk_typeinfo_of(c, *capture);
    }
#if LHAT_WITH_RESOLUTIONS
    if (node != NULL) ((LhatNode *)node)->display_type = type;
#endif
    // [syntax.stmt.nil-assign]: the place a '?op=' reads answers what is there. [errors.runtime.index] puts
    // a nil^ arm on everything a key reaches, and the '?' spelling is how a
    // writer says the write is skipped rather than that the arm is gone --
    // so the operator above this is asked of the rest, and the compiler emits
    // the test that makes that true.
    if (node != NULL && node == c->nil_safe_place) {
        type = chk_without_nil_arm(c, type);
    }
    // [syntax.type.tuple]: a tuple is the other type the compiler has to size slots by --
    // a call answering one writes a run rather than a slot -- so it is
    // stamped for exactly the reason a host value is. A union carrying a
    // tuple arm ('(K, V)|nil^', a walk's resume) is stamped too: even a
    // discarded call has to reserve the arm's width for the run to land in.
    // [modules.hostvalue.union]: and a union carrying a host value arm for the same reason --
    // 'Vector3|nil^' takes the Vector3's slots whichever arm turns up, since
    // the room has to be there before anyone knows which did. Its kind is no
    // more a FUNC or a TYPEOF than HOSTVALUE is, so those stamps stay
    // untouched.
    if (node != NULL &&
        (node->kind == LHAT_NODE_ERROR_NEW || node->kind == LHAT_NODE_NEW ||
         chk_is_hostvalue(type) || lhat_type_tuple_arm_width(type) > 0)) {
        ((LhatNode *)node)->checked_type = type;
    } else if (node != NULL && node->kind != LHAT_NODE_FUNC &&
               node->kind != LHAT_NODE_TYPEOF && node->kind != LHAT_NODE_TYPE_VALUE) {
        // Rechecking after inference narrows an overload may turn a wide
        // result into a scalar. The previous width stamp must not survive
        // that pass. These three node kinds own a different checked_type:
        // the function signature, typeof operand, or represented type.
        const LhatType *previous = (const LhatType *)node->checked_type;
        if (chk_is_hostvalue(previous) || lhat_type_tuple_arm_width(previous) > 0) {
            ((LhatNode *)node)->checked_type = NULL;
        }
    }
    return type;
}

// [syntax.sub.yieldable] with [modules.unit.optional]: a script's top level is 'p^...', and a yield^ or an
// await^ there makes it yieldable like any other body -- running it answers
// the coroutine. A module^ unit's top level runs once under require^, which
// wants its exports and not a coroutine, and a session's input runs at the
// prompt; neither has anyone to resume it, so the suspension is refused
// here rather than left to fault at run time.
static void refuse_top_level_suspension(Checker *c, const LhatNode *node)
{
    if (c->body_scope == NULL &&
        (c->session || c->result->module_name != NULL)) {
        chk_report(c, node, LHAT_CHECK_ERR_YIELD_AT_TOP);
    }
}

LhatType *chk_infer(Checker *c, const LhatNode *node)
{
    return chk_infer_with_named_type(c, node, NULL);
}

static LhatType *infer_node(Checker *c, const LhatNode *node,
                           LhatType **named_type)
{
    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
        case LHAT_NODE_INT:
        case LHAT_NODE_FLOAT:
            return chk_simple(c, LHAT_TYPE_NUMBER);

        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
            return chk_simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_INTERP:
            // [lex.string.interp]: a hole holds an ordinary expression, and lands in a
            // string whatever it turns out to be -- which is why the answer
            // here says nothing about it. What goes wrong inside one is still
            // wrong, so each is checked on its own.
            for (const LhatNode *part = node->v.list.items; part != NULL;
                 part = part->next) {
                if (part->kind == LHAT_NODE_INTERP_HOLE) {
                    // [modules.hostvalue]: a host value stands here like any other.
                    // The hole reads it and keeps nothing, which is what
                    // every other place [syntax.stmt.let] refuses does -- and [syntax.obj.tostring]'s
                    // tostring reaches one the same as it reaches a number^:
                    // the library's registered one, or the built-in writing
                    // the type's name where none was registered.
                    LhatType *held = chk_require_value(
                        c, part->v.hole.value, chk_infer(c, part->v.hole.value));
                    // [syntax.type.tuple]: a hole is one value, as an argument is. A
                    // tuple here has nowhere to put its positions, and the
                    // machine would only refuse it as it ran.
                    if (lhat_type_tuple_width(held) > 0) {
                        chk_report(c, part->v.hole.value,
                                   LHAT_CHECK_ERR_TUPLE_MISPLACED);
                    }
                }
            }
            return chk_simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_TYPE_NAME:
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_SCOPE:
        case LHAT_NODE_FOCUS: {
            // [syntax.type.fits]: a branch may know more about this path than the binding.
            LhatType *narrowed = chk_narrowed_type(c, node);
            if (narrowed != NULL &&
                (node->kind == LHAT_NODE_HAT_IDENT || node->kind == LHAT_NODE_FOCUS)) {
                chk_infer_name(c, node, named_type);
#if LHAT_WITH_RESOLUTIONS
                const char *name = NULL;
                size_t length = 0;
                Binding *binding = NULL;
                if (chk_node_name(c, node, &name, &length)) {
                    binding = node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats > 1
                        ? chk_scope_find_skipping(c->scope, name, length, node->v.name.hats - 1)
                        : read_binding_from(c->scope, name, length, NULL);
                }
                if (binding != NULL) {
                    chk_record_narrowed_resolution(c, node, binding, narrowed);
                } else {
                    chk_record_typed_resolution(c, node, narrowed);
                }
#endif
                return narrowed;
            }
            if (narrowed == NULL || named_type != NULL) {
                LhatType *held = chk_infer_name(c, node, named_type);
                if (narrowed == NULL) {
                    return held;
                }
            }
            // Record the narrowed value type at the same declaration. An
            // alias lookup above may also have recorded the original value
            // type; settling the records keeps this more precise answer.
            //
            // The binding is looked up rather than reported on: an
            // undefined name has no narrowing to reach this with, since
            // what put one here read the name first. A spelling with no
            // binding to find -- [syntax.stmt.no-bare-expr]'s $^, [syntax.for.forms]'s focus -- keeps the type
            // and nothing else, which is what a member already does.
            const char *name = NULL;
            size_t length = 0;
            Scope *from = node->kind == LHAT_NODE_SCOPE
                              ? chk_scope_from(c->scope, node) : c->scope;
            Scope *found = NULL;
            Binding *b = (node->kind == LHAT_NODE_IDENT || node->kind == LHAT_NODE_SCOPE) &&
                                 chk_node_name(c, node, &name, &length)
                             ? read_binding_from(from, name, length, &found) : NULL;
            ((LhatNode *)node)->checked_binding = NULL;
            ((LhatNode *)node)->checked_host_member =
                b == NULL && name != NULL ? initial_binding_member(c, name, length) : NULL;
            ((LhatNode *)node)->checked_module_root = b != NULL ? b->module_root : NULL;
            ((LhatNode *)node)->checked_import_global = b != NULL && b->import_root &&
                c->body_scope != NULL && !chk_scope_within_body(c, found);
            if (b != NULL) {
                ((LhatNode *)node)->checked_binding = b->declaration;
                ((LhatNode *)node)->checked_definition = b->definition;
            }
#if LHAT_WITH_RESOLUTIONS
            if (b != NULL) {
                chk_record_narrowed_resolution(c, node, b, narrowed);
            } else {
                chk_record_typed_resolution(c, node, narrowed);
            }
#endif
            return narrowed;
        }

        case LHAT_NODE_TABLE:
            return chk_infer_table(c, node);

        case LHAT_NODE_DEF:
            return chk_infer_def(c, node, NULL);

        case LHAT_NODE_SELF_TABLE: {
            // [syntax.obj.new]: reached as an expression this is the construction
            // notation, and construction is a written new's alone -- the
            // machine has just made the copy and nothing else holds it yet.
            // A method that wants to change its receiver writes the fields
            // one at a time, where [syntax.obj.self]'s rules can see each write.
            if (!c->in_new_body) {
                chk_report(c, node, LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW);
            }
            // The fields are checked against the instance either way. new
            // is bound over self^ (chk_infer_def does this for the
            // constructor entry); the self_link arm keeps a refused stray
            // measured against the definition it sits in.
            Binding *receiver = chk_scope_find(c->scope, "self^", 5, NULL);
            ((LhatNode *)node)->checked_receiver = receiver != NULL ? receiver->declaration : NULL;
            LhatType *instance = receiver != NULL      ? receiver->type
                                 : c->self_link != NULL ? c->self_link->type
                                                        : NULL;
            uint8_t made = LHAT_OWN_DEEP;
            for (const LhatNode *field = node->v.list.items; field != NULL;
                 field = field->next) {
                LhatType *value = chk_infer(c, field->v.entry.value);
                uint8_t brought = chk_made_depth(c, field->v.entry.value, value);
                made = brought < made ? brought : made;
                // [modules.hostvalue]: an instance is a table, so its fields refuse a
                // host value the way any table member does.
                if (chk_is_hostvalue(value)) {
                    chk_report(c, field->v.entry.value,
                               LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
                }
                // [syntax.obj.new]: the template says what a field holds, so what is
                // written here is held to it -- which is also where
                // [pipeline.infer] reads the type of an unannotated parameter handed
                // straight into a field. A name the template does not have
                // is not a field at all: [syntax.obj.instance] closes an instance to members
                // added afterwards.
                const char *name = NULL;
                size_t length = 0;
                if (instance == NULL || field->v.entry.key == NULL ||
                    !chk_node_name(c, field->v.entry.key, &name, &length)) {
                    continue;
                }
                const LhatTypeMember *held =
                    chk_find_member(instance, name, length);
                if (held == NULL) {
                    ChkNearest near;
                    if (chk_nearest_start(c, &near, name, length)) {
                        lhat_check_written_members(instance, offer_field,
                                                   &near);
                    }
                    chk_report_near(c, field->v.entry.key,
                                    LHAT_CHECK_ERR_NO_MEMBER, name, length,
                                    field->v.entry.key, &near);
                    continue;
                }
                chk_expect(c, field->v.entry.value, value, held->type,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            // [syntax.sub.table-write]: the fields land in self^ itself.
            ((LhatNode *)node)->checked_own =
                made == LHAT_OWN_DEEP ? made : (uint8_t)(made + 1);
            chk_store_into_self(c, node, made);
            return instance != NULL ? instance
                                    : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_UNARY: {
            LhatType *operand = chk_infer(c, node->v.unary.operand);
            // [syntax.op.present]: 'x?' is '!(x fits^ nil^)' written short, so it
            // asks nothing of its operand -- every value either is nil^ or
            // is not. A type with no nil^ arm answers true^ always, which is
            // let through rather than reported: [syntax.op.nil-safe] takes the same posture
            // for a '?.' that cannot find an absent target.
            if (node->v.unary.op == LHAT_OP_PRESENT) {
                chk_require_value(c, node->v.unary.operand, operand);
                return chk_simple(c, LHAT_TYPE_BOOL);
            }
            if (node->v.unary.op == LHAT_OP_NOT) {
                chk_expect(c, node, operand, chk_simple(c, LHAT_TYPE_BOOL),
                           LHAT_CHECK_ERR_NOT_BOOL);
                return chk_simple(c, LHAT_TYPE_BOOL);
            }
            if (node->v.unary.op == LHAT_OP_BITNOT) {
                return expect_bitwise(c, node->v.unary.operand, operand);
            }
            // [syntax.op.define]: [syntax.obj.number] gives number^ the negation, and a type that wrote
            // its own is asked only where that does not reach -- the order the
            // machine takes, where NEG handles its own type and comes to the
            // member for everything else.
            LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);
            LhatType *success = without_error_arms(c, operand);
            if (success != NULL && infer_unary_operator(c, operand) == NULL) {
                LhatType *answer = lhat_type_conforms(success, number)
                                       ? number : infer_unary_operator(c, success);
                if (answer != NULL) {
                    chk_report(c, node->v.unary.operand, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR);
                    return answer;
                }
            }
            if (!lhat_type_conforms(operand, number)) {
                LhatType *own = infer_unary_operator(c, operand);
                if (own != NULL) {
                    return own;
                }
            }
            // [errors.runtime.index]: '-t[i]' is the same mistake the binary ones make,
            // and "arithmetic needs number^" says as little about it.
            LhatType *bare = NULL;
            bool by_nil = nil_arm_apart(c, operand, &bare) &&
                          lhat_type_conforms(bare, number);
            chk_expect(c, node, operand, number,
                       by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                              : LHAT_CHECK_ERR_NOT_NUMBER);
            return number;
        }

        case LHAT_NODE_BINARY:
            return chk_infer_binary(c, node);

        // [syntax.op.precedence] の (5): 'a < b < c' is '(a < b) and^ (b < c)', so every link
        // is asked what a comparison written on its own is asked. The operand
        // two links share is inferred once, the way it is evaluated once.
        //
        // [syntax.type.fits]: an fits^ link takes a type on the right, and what it tests is
        // the operand standing to its left -- a type is not a value the next
        // link could compare against, so it does not take that place. 'a < b
        // fits^ number^ < c' asks about b three times over.
        case LHAT_NODE_COMPARE_CHAIN: {
            const LhatNode *operand = node->v.chain.operands;
            LhatType *left = chk_infer(c, operand);
            for (const LhatNode *marker = node->v.chain.operators;
                 marker != NULL && operand != NULL; marker = marker->next) {
                LhatOpKind op = marker->v.unary.op;
                operand = operand->next;
                if (operand == NULL) {
                    break;
                }
                if (op == LHAT_OP_FITS) {
                    ((LhatNode *)marker)->checked_fits_type = left;
                    LhatType *asked = chk_resolve_type(c, operand);
                    if (chk_runtime_has_unresolved_arguments(asked)) {
                        chk_report(c, operand, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
                    }
                    if (asked != NULL && asked->kind == LHAT_TYPE_ANY) {
                        chk_report(c, operand, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
                    }
                    continue;  // the left of the next link is still `left`
                }
                LhatType *right = chk_infer(c, operand);
                check_comparison(c, operand, op, left, right);
                left = right;
            }
            return chk_simple(c, LHAT_TYPE_BOOL);
        }

        // [errors.runtime.nil]: the second half of the '?' forms. Each of the
        // three reads its target with the nil^ arm set aside; what comes back
        // gains one here, since an absent target answers nil^ rather than a
        // member, a position or a call. A narrowed path never reaches
        // chk_nil_propagated: [syntax.type.fits]'s `narrowable` refuses a '?.' path outright.
        case LHAT_NODE_CALL:
            return chk_nil_propagated(c, node, chk_infer_call(c, node));

        case LHAT_NODE_MEMBER: {
            LhatType *narrowed = chk_narrowed_type(c, node);
            // Narrowing changes the value type, not the identity of the read.
            // Walk the member and its target even when flow already knows the
            // answer, so codegen receives bindings and member provenance.
            LhatType *held = chk_infer_member(c, node, named_type);
            LhatType *answer = narrowed != NULL ? narrowed : chk_nil_propagated(c, node, held);
#if LHAT_WITH_RESOLUTIONS
            // [lsp.hover]: recorded here rather than inside chk_infer_member,
            // which answers from a dozen places -- the built-in members of a
            // coroutine ([syntax.sub.coroutine-members]), an error's own two ([errors.kind.made]), a
            // definition's, a table's. What every one of them has in common
            // is that it came back through here.
            const LhatTypeMember *found = c->resolved_member;
            // [syntax.enum]: an enum's member read in value position answers
            // the wide type, and what the name reached is left the same way
            // a member is -- the place a reader is sent to is the member's.
            const LhatType *kind = c->resolved_kind;
            // [syntax.obj.substring], [syntax.obj.tostring-hat], [syntax.sub.coroutine-members]: an answer with no member record behind
            // it is the language's own -- a host registration leaves a real
            // member (with nothing written for declared_in), and a def^ or a
            // t^{ … } leaves one that says where it stands. A miss answers
            // unknown^ and is nobody's.
            bool builtin = found == NULL && kind == NULL && held != NULL &&
                           held->kind != LHAT_TYPE_UNKNOWN &&
                           held->kind != LHAT_TYPE_PENDING;
            if (kind != NULL) {
                chk_record_kind_resolution(c, node->v.access.argument, answer,
                                           kind);
            } else {
                chk_record_member_resolution(c, node->v.access.argument,
                                             answer, found, builtin);
            }
#endif
            return answer;
        }

        case LHAT_NODE_INDEX:
            return chk_nil_propagated(c, node, chk_infer_index(c, node));

        // [errors.runtime.panic]: sound rather than a bare relabelling -- [syntax.obj.override]'s
        // disjointness rules out what could never succeed at compile time
        // (a mistake, the same reasoning EQ/NE/... already use just above),
        // and compile_expression checks what disjointness cannot rule out
        // against the actual value at run time, panicking if it does not
        // hold. Either way the expression's own type becomes what was
        // written, since that is what a fitting value goes on to satisfy.
        case LHAT_NODE_AS: {
            LhatType *actual = chk_require_value(
                c, node->v.ascription.value, chk_infer(c, node->v.ascription.value));
            LhatType *wanted = chk_resolve_type(c, node->v.ascription.type);
            if (chk_runtime_has_unresolved_arguments(wanted)) {
                chk_report(c, node->v.ascription.type, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
            }
            if (lhat_type_disjoint(actual, wanted)) {
                // [syntax.op.as-error]: a pair no value inhabits both of is settled at the
                // type level, not a narrowing [syntax.type.fits] keeps out of the
                // checker's reach -- so the cast could only ever fail, and
                // saying so beats handing back a union whose value arm is
                // unreachable.
                chk_report(c, node, LHAT_CHECK_ERR_AS_IMPOSSIBLE);
            }
            // [syntax.op.as-error]: the failure arm is added always, rather than only
            // where the cast could fail. A rule that read the left's
            // inferred type would move the answer's type with how much
            // inference happened to settle.
            //
            // [errors.kind.local-no-propagate] is what makes this affordable: the arm cannot be
            // let past with try^, so the union stops at whoever wrote the
            // as^ instead of spreading into every signature above it --
            // which is the contamination [errors.runtime.div-zero] withdrew a design over.
            return lhat_type_union(c->result->types, wanted,
                                   chk_cast_failure_type(c));
        }

        case LHAT_NODE_FUNC:
            return chk_infer_func(c, node);

        // [syntax.sub.yieldable]: what a yield^ answers (R) has to be fixed by whatever binds
        // it directly, since the value it carries out (Y) is only half of
        // what the expression is. check_define and the bare-statement case
        // in check_statement are the only two places that set yield_context
        // to anything other than NONE, and only for the yield^ they
        // themselves are looking at -- chk_infer() clears it immediately below
        // so it can never leak into node->v.jump.value.
        case LHAT_NODE_YIELD: {
            refuse_top_level_suspension(c, node);
            enum YieldContext ctx = c->yield_context;
            LhatType *bound = c->yield_bound_type;
            c->yield_context = YIELD_CTX_NONE;
            c->yield_bound_type = NULL;

            // [syntax.type.tuple]: 'yield^ a, b' answers a tuple, exactly as a return^ of
            // several values does. What comes back is R, sized on its own
            // ([syntax.type.coroutine] seats R and Y apart), so the binding below reads it the
            // same way it reads a one-value yield^'s.
            LhatType *produced;
            if (node->v.jump.level > 1) {
                LhatType *tuple = lhat_type_tuple(c->result->types);
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    LhatType *position =
                        chk_require_value(c, item, chk_infer(c, item));
                    chk_check_tuple_position(c, item, position);
                    lhat_type_add_position(c->result->types, tuple, position);
                }
                produced = tuple;
            } else {
                produced = chk_require_value(c, node,
                                             chk_infer(c, node->v.jump.value));
            }
            chk_unify_yield(c, node, &c->coroutine_produce, produced);

            if (ctx == YIELD_CTX_DISCARD) {
                // Nothing receives this one, so it says nothing about R.
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (ctx == YIELD_CTX_BOUND && bound != NULL) {
                chk_unify_yield(c, node, &c->coroutine_receive, bound);
                return bound;
            }
            // Either bound with no annotation to read R off of, or reached
            // some other way (buried inside a larger expression) where there
            // is nowhere to write one.
            chk_report(c, node, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        // [syntax.sub.await]: the value is the inner coroutine's return value, and the
        // right side has to be a coroutine -- there is nothing else to
        // delegate to. [syntax.sub.yieldable]: whatever it yields passes through as this
        // body's own Y/R, same as a yield^ written directly here would.
        case LHAT_NODE_AWAIT: {
            refuse_top_level_suspension(c, node);
            LhatType *inner = chk_infer(c, node->v.jump.value);
            if (inner == NULL || inner->kind == LHAT_TYPE_UNKNOWN ||
                inner->kind == LHAT_TYPE_PENDING) {
                // [pipeline.strict.per-unit], [pipeline.strict.relaxed]、P6: delegating to a still-pending^ inner
                // expression makes this await^ pending^ too.
                return chk_simple(c, inner != NULL &&
                                          inner->kind == LHAT_TYPE_PENDING
                                      ? LHAT_TYPE_PENDING
                                      : LHAT_TYPE_UNKNOWN);
            }
            if (inner->kind != LHAT_TYPE_CORO) {
                chk_report(c, node, LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            // [syntax.sub.await] with [syntax.sub.yieldable-f]: delegating runs the inner body -- that is what
            // makes its yield^ reach out here -- so it advances the coroutine
            // as surely as resume() does, and the same two questions apply.
            // Asked here rather than left to [syntax.sub.kinds], since no call of start()
            // or resume() is written for the rule to catch.
            if (c->in_function && !inner->v.coroutine.is_function) {
                chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
            } else if (c->in_function &&
                       !chk_receiver_is_own_coroutine(c, node->v.jump.value)) {
                chk_report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
            }
            chk_unify_yield(c, node, &c->coroutine_produce, inner->v.coroutine.produce);
            chk_unify_yield(c, node, &c->coroutine_receive, inner->v.coroutine.receive);
            // [syntax.type.coroutine]: a coroutine that cannot end never reaches a last resume,
            // so a delegation to one never produces a value either. One that
            // ends without a value does reach it, and is handed nil^ there --
            // the same nil^ a resume of it would read.
            if (inner->v.coroutine.endless) {
                return chk_simple(c, LHAT_TYPE_NONE);
            }
            return inner->v.coroutine.result != NULL
                       ? inner->v.coroutine.result
                       : chk_simple(c, LHAT_TYPE_NIL);
        }

        case LHAT_NODE_TRY: {
            // [errors.try.result]: the errors this lets through have to be ones the
            // enclosing subroutine may return, and the report belongs here
            // rather than at the caller.
            LhatType *value = chk_infer(c, node->v.jump.value);
            // [errors.kind.local]: either family is an error to try^. Telling them apart is
            // chk_error_leaves's job, and only to refuse one.
            LhatType *error = chk_any_error(c);
            if (!chk_can_be(value, error)) {
                // try^ is an identity on an expression that cannot fail.
                return value;
            } else if (c->catch_frame != NULL) {
                // [errors.catch.clause]: catch^ arms stand between this and the
                // caller. What leaves here reaches them, and [errors.try.result] is asked of
                // whatever they do not take, where they are.
                LhatType *escaping = chk_only(c, value, error);
                if (escaping != NULL) {
                    c->catch_frame->caught =
                        lhat_type_union(c->result->types,
                                        c->catch_frame->caught, escaping);
                }
            } else {
                chk_error_leaves(c, node, chk_only(c, value, error));
            }
            LhatType *kept = chk_without(c, value, error);
            return successful_error_result(c, kept);
        }

        // [syntax.obj.typeof]: preserve the checked type in both the descriptor value
        // and type^<T>. Match the compiler's error/tag fallback: those answers
        // have erased type^, since their exact descriptor is known only at run.
        case LHAT_NODE_TYPEOF: {
            LhatType *operand = chk_infer(c, node->v.jump.value);
            ((LhatNode *)node)->checked_type = operand;
            if (operand != NULL && !lhat_rt_mentions_error(operand) &&
                !chk_runtime_has_unresolved_arguments(operand)) return chk_typeinfo_of(c, operand);
            return chk_typeinfo_type(c);
        }

        // [syntax.type.as-expr]: the spelling resolved, stamped for the compiler to
        // fold into a descriptor constant -- and the expression itself is
        // worth a typeinfo to the program, exactly what typeof^ answers.
        case LHAT_NODE_TYPE_VALUE: {
            bool outer_in_type_value = c->in_type_value;
            c->in_type_value = true;
            LhatType *named = chk_resolve_type(c, node->v.jump.value);
            c->in_type_value = outer_in_type_value;
            if (chk_runtime_has_unresolved_arguments(named)) {
                chk_report(c, node, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
            }
            ((LhatNode *)node)->checked_type = named;
            if (named_type != NULL) {
                *named_type = named;
            }
            return chk_typeinfo_of(c, named);
        }

        case LHAT_NODE_IF_EXPR: {
            // The same shape as the statement form: each clause sees what the
            // earlier conditions ruled out, which is what makes a chain over
            // a union exhaustive ([errors.exhaustive]).
            Narrowing *outer = c->narrowings;
            LhatType *result = NULL;
            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    result = lhat_type_union(c->result->types, result,
                                             chk_infer(c, clause->v.clause.body));
                    continue;
                }
                chk_expect(c, condition, chk_infer(c, condition),
                           chk_simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);

                Narrowing *before = c->narrowings;
                chk_narrow_from(c, condition, true);
                result = lhat_type_union(c->result->types, result,
                                         chk_infer(c, clause->v.clause.body));
                chk_pop_narrowings(c, before);

                chk_narrow_from(c, condition, false);
            }
            chk_pop_narrowings(c, outer);
            return result;
        }

        case LHAT_NODE_FOR: {
            // [syntax.match.form]: the expression form of a match. The subject is a binding
            // like any other focus, so it needs the scope [syntax.for.not-loop] implies, and
            // the body is already the if-chain of [syntax.match.expand].
            Scope scope;
            chk_scope_open(&scope, c->scope, node, false);

            Scope *outer = c->scope;
            c->scope = &scope;
            chk_check_statements(c, node->v.loop.focus);
            LhatType *result = chk_infer(c, node->v.loop.body);

            // [syntax.match.other]: without other^ the arms must provably exhaust the
            // subject. Every arm only ever takes something out of what the
            // subject may still be, so an arm that proves nothing (a range,
            // another literal) is simply left out of the count. `fits^` takes
            // its type out the way [syntax.type.fits] does on the false side of a chain;
            // `= true^` / `= false^` / `= nil^` and `= E.Member` are what
            // [syntax.match.expand] lowered a literal pattern to, possibly or^-ed together,
            // and take a value out of the type that has only those.
            // relaxed leaves the doubt to the run, where the arm nothing fits
            // is a panic ([pipeline.strict.per-unit]; compile.c's tail).
            const LhatNode *chain = node->v.loop.body;
            const LhatNode *clause =
                chain != NULL && chain->kind == LHAT_NODE_IF_EXPR
                    ? chain->v.list.items
                    : NULL;
            if (node->v.loop.kind == LHAT_FOR_WHEN &&
                clause != NULL) {
                bool defaulted = false;
                bool saw_true = false;
                bool saw_false = false;
                const LhatType *seen_members[256];
                size_t seen_count = 0;
                LhatType *remaining =
                    scope.bindings != NULL ? scope.bindings->type : NULL;
                bool known_subject = remaining != NULL;
                for (; clause != NULL && !defaulted; clause = clause->next) {
                    const LhatNode *condition = clause->v.clause.condition;
                    if (condition == NULL) {
                        defaulted = true;
                        break;
                    }
                    const LhatNode *stack[16];
                    size_t top = 0;
                    stack[top++] = condition;
                    while (top > 0) {
                        const LhatNode *cond = stack[--top];
                        if (cond->kind != LHAT_NODE_BINARY) {
                            continue;
                        }
                        if (cond->v.binary.op == LHAT_OP_OR && top + 2 <= 16) {
                            stack[top++] = cond->v.binary.left;
                            stack[top++] = cond->v.binary.right;
                            continue;
                        }
                        const LhatNode *right = cond->v.binary.right;
                        if (cond->v.binary.op == LHAT_OP_FITS) {
                            remaining = chk_without(c, remaining,
                                                    chk_resolve_type(c, right));
                            continue;
                        }
                        if (cond->v.binary.op != LHAT_OP_EQ || right == NULL) {
                            continue;
                        }
                        const char *name = NULL;
                        size_t length = 0;
                        if (chk_node_name(c, right, &name, &length)) {
                            if (lhat_name_is(name, length, "nil^")) {
                                remaining = chk_without(
                                    c, remaining,
                                    chk_simple(c, LHAT_TYPE_NIL));
                            }
                            saw_true = saw_true ||
                                       lhat_name_is(name, length, "true^");
                            saw_false = saw_false ||
                                        lhat_name_is(name, length, "false^");
                        } else if (right->kind == LHAT_NODE_MEMBER &&
                                   seen_count < sizeof seen_members /
                                                    sizeof seen_members[0]) {
                            // [syntax.enum]: resolved from the written path -- value
                            // position reads wide on purpose, so the
                            // singleton is asked for here.
                            LhatType *holder =
                                chk_infer(c, right->v.access.target);
                            LhatType *arm = NULL;
                            if (holder != NULL &&
                                holder->kind == LHAT_TYPE_ENUM &&
                                chk_node_name(c, right->v.access.argument,
                                              &name, &length)) {
                                arm = chk_kind_of_set(holder, name, length);
                            }
                            if (arm != NULL &&
                                arm->kind == LHAT_TYPE_ENUM_MEMBER) {
                                seen_members[seen_count++] = arm;
                            }
                        }
                    }
                }
                if (!defaulted &&
                    !(known_subject &&
                      match_covers(remaining, saw_true && saw_false,
                                   seen_members, seen_count))) {
                    chk_report(c, node, LHAT_CHECK_ERR_MATCH_NOT_EXHAUSTIVE);
                }
            }

            c->scope = outer;
            chk_scope_close(c, &scope);
            return result;
        }

        case LHAT_NODE_REQUIRE: {
            // [modules.deps.static]: the checker follows this. [l10n.compose.hole-kinds] already had the parser
            // insist the path be written out, so there is text here to hand
            // the resolver rather than an expression to evaluate.
            const LhatNode *path = node->v.jump.value;
            if (c->in_function) {
                chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_REQUIRES);
            }
            if (path == NULL || c->require.resolve == NULL) {
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            const char *module_name = NULL;
            const char *unit_path = NULL;
            LhatType *exports = c->require.resolve(
                c->require.context, c->lexer->strings + path->v.string.offset,
                path->v.string.length, &module_name, &unit_path);
            if (exports == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
#if LHAT_WITH_RESOLUTIONS
            // [lsp.hover]: the written path names a unit the way a name names
            // a binding, and the resolver has just found it.
            chk_record_unit_resolution(c, path, exports, unit_path);
#endif
            // [syntax.if.spellings]: running the unit is what registers it, and this is where
            // it runs -- so L^.modules can be said to hold it from here on.
            chk_register_module_type(c, module_name, exports);
            return exports;
        }

        // [modules.host.import]: names a module the host registered, rather than a file.
        case LHAT_NODE_IMPORT: {
            LhatType *module = chk_hosted_module(c, node->v.jump.value);
            if (module == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_NOT_HOSTED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return module;
        }

        case LHAT_NODE_ERROR_NEW:
            return infer_error_construction(c, node,
                chk_resolve_type(c, node->v.named.name));

        case LHAT_NODE_NEW: {
            LhatType *target = chk_resolve_type(c, node->v.named.name);
            if (target != NULL && target->kind == LHAT_TYPE_ERROR_KIND) {
                return infer_error_construction(c, node, target);
            }
            if (target == NULL || target->kind != LHAT_TYPE_TABLE ||
                target->v.table.nominal || target->v.table.is_typeinfo ||
                target->v.table.is_definition) {
                chk_report(c, node, LHAT_CHECK_ERR_NEW_TARGET);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (!target->v.table.from_definition) {
                LhatType *made = chk_infer_table(c, node);
                chk_expect(c, node, made, target, LHAT_CHECK_ERR_MISMATCH);
                return made;
            }
            LhatType *owner = chk_infer(c, node->v.named.name);
            if (owner == NULL || owner->kind != LHAT_TYPE_TABLE ||
                !owner->v.table.is_definition) {
                chk_report(c, node, LHAT_CHECK_ERR_NEW_TARGET);
                return target;
            }
            if (owner->v.table.new_overridden) {
                chk_report(c, node, LHAT_CHECK_ERR_NEW_CONSTRUCTOR);
            }
            bool field = false;
            const LhatTypeMember *hole = chk_hole_of(owner, &field);
            if (hole != NULL) {
                chk_report_named_span(c, node, field ? LHAT_CHECK_ERR_FIELD_UNPROVIDED
                                                     : LHAT_CHECK_ERR_STILL_ABSTRACT,
                                      hole->name, hole->name_length);
            }
            for (const LhatNode *entry = node->v.named.members; entry != NULL;
                 entry = entry->next) {
                LhatType *given = chk_require_value(c, entry->v.entry.value,
                                                   chk_infer(c, entry->v.entry.value));
                const char *name = NULL;
                size_t length = 0;
                if (entry->v.entry.computed || entry->v.entry.key == NULL ||
                    !chk_node_name(c, entry->v.entry.key, &name, &length)) {
                    chk_report(c, entry, LHAT_CHECK_ERR_NEW_TARGET);
                    continue;
                }
                const LhatTypeMember *held = chk_find_member(target, name, length);
                if (held == NULL || !held->instance_field) {
                    chk_report_named_span(c, entry->v.entry.key,
                                          LHAT_CHECK_ERR_NO_MEMBER, name, length);
                    continue;
                }
                if (chk_is_hostvalue(given)) {
                    chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
                }
                chk_expect(c, entry->v.entry.value, given, held->type,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            return target;
        }

        // [syntax.type.tuple]: '(a, b)' written as a value. Where the tuple came from
        // makes no difference downstream -- this is the same type a
        // 'return^ a, b' builds, so every rule about where a tuple may
        // stand applies to a literal without being restated. The parser
        // folds one written as a jump's value into the jump itself, so what
        // reaches here stands in expression position: a catch^'s right side,
        // or somewhere a tuple may not be, which the callers refuse.
        case LHAT_NODE_TUPLE: {
            LhatType *tuple = lhat_type_tuple(c->result->types);
            for (const LhatNode *item = node->v.list.items; item != NULL;
                 item = item->next) {
                LhatType *position =
                    chk_require_value(c, item, chk_infer(c, item));
                chk_check_tuple_position(c, item, position);
                lhat_type_add_position(c->result->types, tuple, position);
            }
            return tuple;
        }

        // [syntax.type.tuple]: pack^ turns the several values a call answered with into a
        // table a name can hold -- [syntax.obj.structure]'s positional members, numbered from
        // 0, which is what 't[0]' and a destructuring both read.
        case LHAT_NODE_PACK: {
            LhatType *source = chk_infer(c, node->v.jump.value);
            size_t width = lhat_type_tuple_width(source);
            if (width == 0) {
                // Nothing to pack. Saying so by name is better than answering
                // a table that was never built.
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_MISPLACED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *packed = lhat_type_table(c->result->types);
            for (size_t i = 0; i < width; i++) {
                lhat_type_add_index_member(c->result->types, packed, i,
                                           lhat_type_tuple_at(source, i));
            }
            return packed;
        }

        // [modules.hostvalue]: box^ puts a host value in the box the heap can hold.
        // What it takes is exactly a host value -- everything else already
        // lives on the heap and needs no box.
        case LHAT_NODE_BOX: {
            LhatType *held = chk_infer(c, node->v.jump.value);
            // [modules.hostvalue]: 'constbox^' off a box of either kind is a sealed
            // copy -- the way a keyable box is made from a live one.
            if (node->v.jump.sealing && held != NULL &&
                held->kind == LHAT_TYPE_HOSTVALUE_BOX) {
                LhatType *inner =
                    held->v.table.instance != NULL
                        ? held->v.table.instance
                        : lhat_type_hostvalue(c->result->types,
                                              held->v.table.hostvalue_tag);
                return lhat_type_hostvalue_box(c->result->types, inner, true);
            }
            if (held == NULL || held->kind != LHAT_TYPE_HOSTVALUE) {
                // A gap stays quiet -- the mistake was reported where the
                // value came from, and one report per mistake is the rule.
                if (held != NULL && held->kind != LHAT_TYPE_UNKNOWN &&
                    held->kind != LHAT_TYPE_PENDING) {
                    chk_report(c, node, LHAT_CHECK_ERR_NOT_BOXABLE);
                }
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return lhat_type_hostvalue_box(c->result->types, held,
                                           node->v.jump.sealing);
        }

        default:
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
}
