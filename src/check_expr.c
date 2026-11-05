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

static bool runtime_has_unresolved_arguments(const LhatType *type, const RuntimeArgumentSeen *seen)
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
            if (runtime_has_unresolved_arguments(a->type, seen)) return true;
        }
        return false;
    }
    if (type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT ||
        type->kind == LHAT_TYPE_TUPLE) {
        for (const LhatTypeList *a = type->v.composite.arms; a; a = a->next) {
            if (runtime_has_unresolved_arguments(a->type, seen)) return true;
        }
    } else if (type->kind == LHAT_TYPE_TABLE && !type->v.table.nominal) {
        for (const LhatTypeMember *m = type->v.table.members; m; m = m->next) {
            if (runtime_has_unresolved_arguments(m->type, seen)) return true;
        }
        return runtime_has_unresolved_arguments(type->v.table.variadic, seen) ||
               runtime_has_unresolved_arguments(type->v.table.index_value, seen);
    } else if (type->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = type->v.func.params; p; p = p->next) {
            if (runtime_has_unresolved_arguments(p->type, seen)) return true;
        }
        return runtime_has_unresolved_arguments(type->v.func.result, seen) ||
               runtime_has_unresolved_arguments(type->v.func.variadic, seen);
    } else if (type->kind == LHAT_TYPE_CORO) {
        return runtime_has_unresolved_arguments(type->v.coroutine.receive, seen) ||
               runtime_has_unresolved_arguments(type->v.coroutine.produce, seen) ||
               runtime_has_unresolved_arguments(type->v.coroutine.result, seen);
    }
    return false;
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
static LhatType *access_target(Checker *c, const LhatNode *node, LhatType *type)
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
static const LhatType *operator_arm(const LhatType *carrier,
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
    if (operator_arm(chk_operator_member(c, left, name, length), args, 1,
                     false) != NULL) {
        return true;
    }
    // [syntax.op.right]: or the right operand carries one written as the receiver, which
    // is how a value joins a comparison whose left side is a built-in.
    args[0] = left;
    return operator_arm(chk_operator_member(c, right, name, length), args, 1,
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
    const LhatType *arm = operator_arm(carrier, args, 1, true);
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
        operator_arm(chk_operator_member(c, operand, "-", 1), NULL, 0, false);
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
            by_nil = operator_arm(carrier, args, 1, false) != NULL;
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
static void report_no_member(Checker *c, const LhatNode *node,
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

static void join_table_entry_types(Checker *c, const LhatType *table,
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
            join_table_entry_types(c, side, &joined->v.table.index_key,
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
        if (runtime_has_unresolved_arguments(asked, 0)) {
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
static LhatType *coroutine_made_by(Checker *c, const LhatType *func)
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
static LhatType *coroutine_answer(Checker *c, const LhatType *coro)
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

static void check_hostvalue_temporary_write(Checker *c, const LhatNode *at,
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
        check_hostvalue_temporary_write(c, node, receiver,
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
static void retain_instance_failure(Checker *c, Instantiation *instance, size_t start)
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

static LhatType *infer_template(Checker *c, const LhatNode *node)
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
        retain_instance_failure(&concrete, instance, 0);
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
    callee = access_target(c, node, callee);

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
                                              : coroutine_made_by(c, callee);
    }
    // [syntax.type.empty]: a signature with no result answers no value. That is not a gap in
    // inference, so it is not spelled with a NULL -- [pipeline.infer] kept it apart
    // from nil^, and this is where that stays observable.
    if (callee->v.func.result == NULL) {
        return chk_simple(c, LHAT_TYPE_NONE);
    }
    return callee->v.func.result;
}

// [syntax.for.forms] with [syntax.type.tuple]: what the built-in walk of a table yields -- the
// tuple (K, V), the key first and the value second. A tuple rather
// than a table: a table would cost an allocation every step. Both halves come
// from what the table holds: [syntax.obj] makes it a sequence and a mapping at
// once, so a walk of one that is both hands over keys of either kind.
// [syntax.for.keys-values]: the halves on their own, which keys^ and values^ answer with and
// the pair below is built from. Written as one walk of the members so the
// three readings cannot drift apart.
// [syntax.obj]: the sequence half is described by members whose names are digits,
// which [lex.comment] keeps a program from writing. Everything else is the keyed
// half, reached by a name -- and by [syntax.obj] that is the same door 't.a' uses.
static bool member_is_positional(const LhatTypeMember *m)
{
    return m != NULL && lhat_type_member_key_kind(m) == LHAT_TYPE_NUMBER;
}

static void table_walk_halves(Checker *c, const LhatType *over,
                              LhatType **out_keys, LhatType **out_values)
{
    LhatType *keys = NULL;
    LhatType *values = NULL;
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        join_table_entry_types(c, over, &keys, &values);
    }
    // An unconstrained table may carry more than its listed members.
    // An explicit index constraint supplies the bound on the entire walk.
    *out_keys = keys != NULL ? keys : chk_simple(c, LHAT_TYPE_UNKNOWN);
    *out_values = values != NULL ? values : chk_simple(c, LHAT_TYPE_UNKNOWN);
}

LhatType *chk_table_walk_tuple(Checker *c, const LhatType *over)
{
    LhatType *keys = NULL;
    LhatType *values = NULL;
    table_walk_halves(c, over, &keys, &values);
    // The width is always two, whatever is known about the halves.
    LhatType *pair = lhat_type_tuple(c->result->types);
    lhat_type_add_position(c->result->types, pair, keys);
    lhat_type_add_position(c->result->types, pair, values);
    return pair;
}

// [syntax.for.forms] with [syntax.type.tuple]: what a single name walking a table receives -- the
// values of the sequence half, in order, the keyed half not visited. The
// same loop as 'for^ i from^ 0 to^ the length - 1 { t[i] }', so the type is the
// union of the positional members and the unbounded tail; named members are
// not part of it.
LhatType *chk_table_element_type(Checker *c, const LhatType *over)
{
    if (over == NULL || over->kind != LHAT_TYPE_TABLE) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    LhatType *values = NULL;
    for (const LhatTypeMember *m = over->v.table.members; m != NULL;
         m = m->next) {
        if (member_is_positional(m)) {
            values = lhat_type_union(c->result->types, values, m->type);
        }
    }
    if (over->v.table.variadic != NULL) {
        values = lhat_type_union(c->result->types, values,
                                 over->v.table.variadic);
    }
    if (over->v.table.index_key != NULL &&
        !lhat_type_disjoint(over->v.table.index_key,
                            chk_simple(c, LHAT_TYPE_NUMBER))) {
        values = lhat_type_union(c->result->types, values,
                                 over->v.table.index_value);
    }
    return values != NULL ? values : chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// [syntax.obj.tostring]: every value carries this, the way [modules.host.coroutine-ops] gives a coroutine
// its operations -- a member of the value rather than a name a unit has to
// import. An f^: writing a value down changes nothing, which is [syntax.op.functions]'s
// reason for keeping an operator pure applied to the same question.
static LhatType *builtin_tostring(Checker *c, const LhatType *target)
{
    LhatType *plain = lhat_type_func(c->result->types, true);
    plain->v.func.takes_self = true;
    plain->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    if (target == NULL || target->kind != LHAT_TYPE_NUMBER) {
        return plain;
    }
    // [syntax.obj.tostring]: a number^ also takes a format. [syntax.obj.override] makes the two an
    // intersection, and the counts they allow -- none and one -- keep them
    // from overlapping without the types being looked at.
    LhatType *formatted = lhat_type_func(c->result->types, true);
    formatted->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, formatted,
                        chk_simple(c, LHAT_TYPE_STRING));
    formatted->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    return lhat_type_intersect(c->result->types, plain, formatted);
}

// [syntax.obj.eq]: the comparison '=' makes, with the error term written down.
// An f^ -- comparing two numbers changes nothing. Both arguments are
// number^: the one to compare against, and how far apart the two may be.
static LhatType *builtin_eq(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, signature,
                        chk_simple(c, LHAT_TYPE_NUMBER));
    lhat_type_add_param(c->result->types, signature,
                        chk_simple(c, LHAT_TYPE_NUMBER));
    signature->v.func.result = chk_simple(c, LHAT_TYPE_BOOL);
    return signature;
}

// [syntax.obj.round]: the whole number below, above or nearest. One type for the
// three -- which of them it is was settled by the name, so nothing is taken
// and a number^ comes back. An f^: rounding reads a value and makes another.
static LhatType *builtin_whole(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    signature->v.func.result = chk_simple(c, LHAT_TYPE_NUMBER);
    return signature;
}

// [syntax.obj.bits]: questions about the bits, answered true^ or false^. bitany,
// bitall and bitat take a number^ (a mask or a position); isinteger nothing.
static LhatType *builtin_bits(Checker *c, bool takes_number)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    if (takes_number) {
        lhat_type_add_param(c->result->types, signature,
                            chk_simple(c, LHAT_TYPE_NUMBER));
    }
    signature->v.func.result = chk_simple(c, LHAT_TYPE_BOOL);
    return signature;
}

// [syntax.obj.tonumber]: tostring read backwards, carried by the one value a number^
// can be read out of. The two signatures are shaped exactly as [syntax.obj.tostring]'s and
// made an intersection for the same reason -- [syntax.obj.override] forbids them overlapping,
// and taking none and taking one keeps them apart without a type being asked
// about.
//
// The answer carries a nil^ arm: a string^ holding no number^ is data, not a
// mistake, so [errors.runtime.nil]'s narrowing or [syntax.op.default]'s '??' is what a caller writes. An f^
// -- reading a value changes nothing.
static LhatType *builtin_tonumber(Checker *c)
{
    LhatType *answer = lhat_type_union(
        c->result->types, chk_simple(c, LHAT_TYPE_NUMBER), chk_simple(c, LHAT_TYPE_NIL));

    LhatType *plain = lhat_type_func(c->result->types, true);
    plain->v.func.takes_self = true;
    plain->v.func.result = answer;

    LhatType *formatted = lhat_type_func(c->result->types, true);
    formatted->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, formatted,
                        chk_simple(c, LHAT_TYPE_STRING));
    formatted->v.func.result = answer;
    return lhat_type_intersect(c->result->types, plain, formatted);
}

// [syntax.obj.substring]: a run of a string^'s characters. The two forms are shaped as
// [syntax.obj.tostring]'s are and made an intersection for the same reason -- [syntax.obj.override] forbids
// them overlapping, and taking one and taking two keeps them apart without a
// type being asked about.
//
// The answer is a plain string^: a range that does not stand answers the
// empty one, so there is no nil^ arm for a caller to take apart.
static LhatType *builtin_substring(Checker *c)
{
    LhatType *from = lhat_type_func(c->result->types, true);
    from->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, from, chk_simple(c, LHAT_TYPE_NUMBER));
    from->v.func.result = chk_simple(c, LHAT_TYPE_STRING);

    LhatType *between = lhat_type_func(c->result->types, true);
    between->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, between,
                        chk_simple(c, LHAT_TYPE_NUMBER));
    lhat_type_add_param(c->result->types, between,
                        chk_simple(c, LHAT_TYPE_NUMBER));
    between->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    return lhat_type_intersect(c->result->types, from, between);
}

// [syntax.obj.at]: one character of a string^, which is [syntax.obj.substring]'s run with both
// ends at the same ordinal. One shape rather than two, so no intersection --
// and a string^ still, since there is no character type for it to answer.
static LhatType *builtin_at(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, signature,
                        chk_simple(c, LHAT_TYPE_NUMBER));
    signature->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    return signature;
}

// [syntax.sub.mutable-self]: what the mutating half of [syntax.obj.table-ops] are -- an f^ that writes through
// its receiver and nothing else. A p^ calls one anywhere; an f^ only on a
// table its own body made.
static LhatType *mutable_method(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.mutable_self = true;
    signature->v.func.mutable_shallow = true;
    return signature;
}

// [syntax.obj.name] with [syntax.obj.tostring-hat]: a table nobody made with a def^. Every name on one is
// the writer's -- vm.c's plain_table asks the same of the value, and the two
// have to answer alike or the checker would allow what the machine refuses.
static bool plain_table_type(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_TABLE &&
           !type->v.table.is_definition && !type->v.table.from_definition;
}

// [syntax.obj.tostring-hat]: the two words that have two spellings at all (keys^ and values^ are
// hat-only, [syntax.obj.count]'s line).
//
// This is a fallback and not what makes the two one member -- lhat_member_held_as
// does that, by taking the hat off at the door wherever the names are not the
// writer's, so that every comparison downstream is an ordinary one. What is
// left here answers for a member still held under the hat: a session's or a
// prompt's type, built by a path that did not go through an owner this could
// be asked of. The machine's member_written keeps the same fallback.
static const char *other_spelling(const char *name, size_t length)
{
    if (chk_name_is(name, length, "iterate")) {
        return "iterate^";
    }
    if (chk_name_is(name, length, "iterate^")) {
        return "iterate";
    }
    if (chk_name_is(name, length, "tostring")) {
        return "tostring^";
    }
    if (chk_name_is(name, length, "tostring^")) {
        return "tostring";
    }
    return NULL;
}

// [syntax.obj.tostring-hat], [syntax.for.reuse]: the hat spelling always reaches the built-in. The bare one
// does too, except where the names are the writer's -- there it is an
// ordinary member like any other, and absent unless something was written
// under it.
static bool builtin_named(const char *name, size_t length, const char *word,
                          bool hatted_only)
{
    size_t n = strlen(word);
    if (length == n + 1 && name[n] == '^' && memcmp(name, word, n) == 0) {
        return true;
    }
    return !hatted_only && length == n && memcmp(name, word, n) == 0;
}

// [errors.runtime.nil] with [lex.op.list]: the '?' forms are one of the two ways to
// reach through a T|nil^ -- the other is [syntax.type.fits]'s narrowing. Two halves make
// one: the target is read without its nil^ arm (the strips below), and what
// comes back gains one, since a nil^ target answers nil^ rather than a
// member, a position or a call.
//
// [syntax.op.nil-safe-chain]: the two halves are not both per link. The strip is -- it belongs
// to the '?' that was written -- but the arm goes on once, at the end of the
// run the parser marked, so the first '?' guards everything after it.
//
// That is also what keeps a nil^ arriving from somewhere else refused. An
// intermediate link gains nothing here, so an arm seen at one came from the
// member's own written type ('a?.b.c' where b is C|nil^), and reaching
// through it still wants a '?' of its own.
//
// A p^ call answers nothing ([syntax.type.empty]), and nothing unions with nil^ into
// something -- a nil-safe call of one produces no value either way.
static LhatType *nil_propagated(Checker *c, const LhatNode *node,
                                LhatType *answer)
{
    if (!node->v.access.nil_chain_end || answer == NULL ||
        answer->kind == LHAT_TYPE_NONE) {
        return answer;
    }
    return lhat_type_union(c->result->types, answer, chk_simple(c, LHAT_TYPE_NIL));
}

// [errors.runtime.nil]: what a target offers once its nil^ arm is set aside, for the
// '?' forms and for relaxed's own stepping aside. Answers `target` unchanged
// when there is no nil^ to remove, or when what is left is still a union of
// real types -- one of those has no single member type to answer with.
LhatType *chk_without_nil_arm(Checker *c, LhatType *target)
{
    if (target == NULL || target->kind != LHAT_TYPE_UNION) {
        return target;
    }
    LhatType *bare = chk_without(c, target, chk_simple(c, LHAT_TYPE_NIL));
    return bare != NULL && bare->kind != LHAT_TYPE_UNION ? bare : target;
}

// [errors.kind.made]: a union every arm of which is an error. What such a union
// answers is what every arm answers, and [errors.kind.made] gives message and cause to
// every kind -- so a call that can fail two ways answers them from the union
// its catch^ hands over, without the writer naming one of the ways first.
//
// Not a case of its own: the same reading a union of anything gets. It is
// spelt out here because the arms are the only values whose members are not
// on a member list to intersect.
static bool all_error_arms(const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) {
        return false;
    }
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (arm->type == NULL || (arm->type->kind != LHAT_TYPE_ERROR &&
                                  arm->type->kind != LHAT_TYPE_ERROR_SET &&
                                  arm->type->kind != LHAT_TYPE_ERROR_KIND)) {
            return false;
        }
    }
    return true;
}

// A name path, excluding computed and nil-safe accesses. Alias rebinding
// also accepts scope specifiers; ReturnType keeps its existing grammar.
static bool run_of_names(const LhatNode *node, bool allow_scope)
{
    while (node != NULL && node->kind == LHAT_NODE_MEMBER &&
           !node->v.access.nil_safe) {
        const LhatNode *argument = node->v.access.argument;
        if (argument == NULL || (argument->kind != LHAT_NODE_IDENT &&
                                 argument->kind != LHAT_NODE_HAT_IDENT)) {
            return false;
        }
        node = node->v.access.target;
    }
    return node != NULL && (node->kind == LHAT_NODE_IDENT ||
                            node->kind == LHAT_NODE_HAT_IDENT ||
                            (allow_scope && node->kind == LHAT_NODE_SCOPE));
}

// [syntax.obj.substring] with [syntax.sub.coroutine-members] and the rest: the words the cascade below answers to.
//
// Spellings to TRY, and nothing more -- no hat, no receiver, no condition.
// Which of them a given type actually answers, and under which spelling, is
// what chk_member_of decides, and asking it is how a tool lists them
// (lhat_check_builtin_members). Writing the conditions out a second time
// here is exactly what this arrangement exists to avoid: they are the hard
// part (plain_table_type, the hat-only families, a written member winning
// first), and a copy of them would disagree where they are hardest.
//
// So a word too many costs nothing -- it simply answers nowhere. A word
// missing costs one completion that is not offered, and never a wrong one.
// Adding a built-in to the cascade means adding its bare word here.
//
// Bare, because the enumeration tries "word" and "word^" for each and keeps
// whichever answers ([lex.hat.repeat]: the hat is part of the name).
const char *const chk_builtin_words[] = {
    "ReturnType", "abs", "assign", "at", "bitall",
    "bitany", "bitat", "cause",
    "ceil", "chars", "clamp", "clear", "clone", "codepoints",
    "contains", "count", "dispose", "done",
    "enum", "eq", "extend", "find",
    "findall", "floor", "get", "indexof",
    "insert", "isinteger", "iterate", "join", "keys",
    "len", "length", "load", "message", "move",
    "pop", "push", "range", "range_downto", "remove", "replace",
    "reverse_iterate", "reverse_range",
    "resize", "resume", "reverse", "round", "save", "self",
    "set", "sign", "size", "slice", "sliceBytes",
    "sort", "split", "stablesort", "start",
    "started", "sub", "substr", "substring",
    "tolower", "tonumber", "tostring", "toupper",
    "value", "values",
};
const size_t chk_builtin_word_count =
    sizeof chk_builtin_words / sizeof chk_builtin_words[0];

LhatType *chk_infer_member(Checker *c, const LhatNode *node,
                           LhatType **named_type)
{
    // [syntax.obj.number-consts]: number^ carries a few static members -- the constants.
    // The parser keeps number^.inf/nan as static member accesses rather
    // than descriptor attributes, so read them before inferring the target.
    const LhatNode *on = node->v.access.target;
    const char *on_name = NULL;
    size_t on_length = 0;
    if (on != NULL && on->kind == LHAT_NODE_HAT_IDENT &&
        chk_node_name(c, on, &on_name, &on_length) &&
        chk_name_is(on_name, on_length, "number^")) {
        const char *constant = NULL;
        size_t constant_length = 0;
#if LHAT_WITH_RESOLUTIONS
        // [lsp.hover]: the dot is recorded here too, and marked as the word's
        // -- a tool asked what may stand after 'number^.' is owed the
        // constants, and this branch answers before any type exists to ask.
        chk_record_member_site(c, node, chk_simple(c, LHAT_TYPE_NUMBER), true);
#endif
        if (chk_node_name(c, node->v.access.argument, &constant,
                          &constant_length) &&
            lhat_number_constant(constant, constant_length) != NULL) {
            return chk_simple(c, LHAT_TYPE_NUMBER);
        }
        chk_report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    LhatType *target = chk_infer(c, node->v.access.target);
    if (c->writing_to == node) {
        check_hostvalue_temporary_write(c, node, node->v.access.target, target);
    }
#if LHAT_WITH_RESOLUTIONS
    // [lsp.hover]: only the two answers below have a member record to point
    // at, so what is left here when this returns is this access's member or
    // nothing. Cleared after the target rather than before: 'a.b.c' infers
    // a.b through this same function, and clearing on the way in would leave
    // b's record standing where c's was asked for.
    c->resolved_member = NULL;
    c->resolved_kind = NULL;
    // [lsp.hover]: what stands to the left of the dot, controlled here rather
    // than where the answer comes back. This one is wanted exactly when the
    // name below is missing -- a dot at the end of what has been typed so
    // far -- so it is taken before that bail, and the nil^ arm is set aside
    // by the same rule the lookup further down applies, so that what a tool
    // offers and what an access would accept are the one answer.
    chk_record_member_site(c, node,
                           node->v.access.nil_safe
                               ? chk_without_nil_arm(c, target)
                               : target,
                           false);
#endif
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node->v.access.argument, &name, &length)) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 04 section [errors.runtime.nil]: analyze the non-nil receiver. Do this
    // before testing for an undecided receiver: ?|nil^ becomes ?.
    // Optional access restores its nil^ arm through nil_propagated.
    if (!builtin_named(name, length, "tostring", false)) {
        target = access_target(c, node, target);
    }

    if (target == NULL || target->kind == LHAT_TYPE_UNKNOWN ||
        target->kind == LHAT_TYPE_PENDING) {
        // [pipeline.infer]: a parameter read for a member has to carry one, and
        // [syntax.obj.structure]'s "at least these" is exactly that demand written as a type.
        chk_constrain_member(c, target, name, length);
        // [pipeline.strict.per-unit], [pipeline.strict.relaxed]、P6: a target still pending^ makes this member's
        // type pending^ too, not merely unknown^ -- the gap has to survive
        // for strict to see it if it reaches somewhere a concrete type was
        // wanted.
        LhatType *answer = chk_simple(
            c, target != NULL && target->kind == LHAT_TYPE_PENDING
                   ? LHAT_TYPE_PENDING : LHAT_TYPE_UNKNOWN);
        // A non-nil receiver does not prove that this key is present.
        // Pending inference keeps its provisional answer until it settles.
        return c->writing_to == node || answer->kind == LHAT_TYPE_PENDING
                   ? answer
                   : lhat_type_union(c->result->types, answer,
                                     chk_simple(c, LHAT_TYPE_NIL));
    }

    return chk_member_of(c, target, name, length, node, named_type);
}

// [syntax.obj.structure] with [syntax.obj.substring]: what `target` answers for a member of this name. Split
// out of chk_infer_member so that it can be ASKED rather than only walked
// into -- a tool listing what may stand after a '.' has a type and a spelling
// and nothing else, and working the answer out for itself would be a second
// reading of everything below ([lsp.hover] refuses that for the same reason it
// refuses re-deriving [lton.host]'s scoping).
//
// `node` is where a diagnostic would stand, and NULL asks the question
// quietly: chk_report already bails on a NULL position (check.c) and so does
// every report_named through it, so a probe reports nothing and mutates
// nothing. The three places that read the node for something other than a
// position guard on it themselves.
//
// What the caller must have done first, because the prologue does it and a
// probe cannot: settled the target (this refuses UNKNOWN and PENDING above),
// and set aside the nil^ arm under [errors.runtime.nil]'s rule -- which for a probe means
// handing in the receiver LhatMemberSite already recorded.
LhatType *chk_member_of(Checker *c, LhatType *target, const char *name,
                        size_t length, const LhatNode *node,
                        LhatType **named_type)
{
    // [syntax.type.return-type]: X.ReturnType -- what a call of X answers, as the
    // descriptor a written type is ([syntax.type.as-expr]). Only a run of names may stand
    // for X: the compiler folds the whole access into a constant, so a call
    // written there would never run. The answer is stamped on the name the
    // access ends in, for that fold and for the let^ that binds an alias to
    // it; the flag is what says the stamp is there.
    if (target->kind == LHAT_TYPE_FUNC &&
        chk_name_is(name, length, "ReturnType")) {
        // A probe has no tree to read the run off, and no name to stamp the
        // answer on -- what it wants to know is only whether the member is
        // there, which for ReturnType it is whenever the receiver is an f^.
        if (node != NULL && !run_of_names(node->v.access.target, false)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        LhatType *answer = chk_call_answer(c, target);
        if (answer == NULL || answer->kind == LHAT_TYPE_NONE) {
            chk_report(c, node, LHAT_CHECK_ERR_NO_RESULT_TYPE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        if (runtime_has_unresolved_arguments(answer, NULL)) {
            chk_report(c, node, LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME);
        }
        if (node != NULL) {
            ((LhatNode *)node->v.access.argument)->checked_type = answer;
            ((LhatNode *)node)->v.access.type_spelling = true;
        }
        if (named_type != NULL) {
            *named_type = answer;
        }
        return chk_typeinfo_of(c, answer);
    }

    // [errors.kind.made]: every kind carries message and cause without declaring them
    // -- so they answer however much of the kind is known: a leaf, a
    // declaration's whole set, error^ alone, or a union of any of those
    // ([l10n.id.spelling]'s it^ is the last one wherever a call can fail two ways). A
    // declared field is the leaf's own and wants the narrowing.
    // [syntax.obj.new]: the prototype a definition hangs under self^ -- one canonical
    // instance every construction copies, so its type is the instance type
    // itself. The definition's alone: an instance reading the name finds
    // nothing, exactly as [syntax.obj.members-order] keeps a value member out of its reach.
    if (target->kind == LHAT_TYPE_TABLE && target->v.table.is_definition &&
        target->v.table.instance != NULL && chk_name_is(name, length, "self^")) {
        return target->v.table.instance;
    }

    // [modules.hostvalue]: the box's two members, the language's own. get answers the
    // value whole -- the registered type, fields and members included, which
    // rides on the box type as `instance`. set writes one of the same tag
    // over the bytes: an effect, so it is a p^ and an f^ body cannot call it
    // ([syntax.sub.kinds]).
    if (target->kind == LHAT_TYPE_HOSTVALUE_BOX) {
        LhatType *held = target->v.table.instance != NULL
                             ? target->v.table.instance
                             : lhat_type_hostvalue(
                                   c->result->types,
                                   target->v.table.hostvalue_tag);
        if (chk_name_is(name, length, "get")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            if (signature != NULL) {
                signature->v.func.result = held;
            }
            return signature;
        }
        // ConstBox^ is the get-only reading, so set is not a member of it
        // at all -- the sealed box's runtime refusal backs the unchecked run.
        if (!target->v.table.sealed && chk_name_is(name, length, "set")) {
            LhatType *signature = lhat_type_func(c->result->types, false);
            if (signature != NULL) {
                lhat_type_add_param(c->result->types, signature, held);
            }
            return signature;
        }
        // [modules.hostvalue.union]: a registered field reads straight off the box's
        // bytes, as it does off a stack value's -- the read only. A write
        // goes through set(), where the sealed box and [syntax.sub.kinds] keep their say.
        const LhatTypeMember *field = chk_find_member(held, name, length);
        if (field != NULL && field->type != NULL &&
            field->type->kind != LHAT_TYPE_FUNC) {
            if (node != NULL && c->writing_to == node) {
                chk_report(c, node, LHAT_CHECK_ERR_BOX_FIELD_WRITE);
            }
#if LHAT_WITH_RESOLUTIONS
            c->resolved_member = field;
#endif
            return field->type;
        }
        // Anything else falls to the built-ins every value answers.
    }

    const LhatTypeMember *members = NULL;
    // [modules.hostdata.base]: the table `members` is the head of, so a search that
    // runs out can go on to what this type is declared under. NULL for the
    // lists that belong to no table -- an error kind's fields ([errors.kind.errordef]).
    const LhatType *member_table = NULL;
    if (target->kind == LHAT_TYPE_TABLE ||
        target->kind == LHAT_TYPE_HOSTVALUE) {
        // [modules.hostvalue]: fields and registered members alike, off the shared
        // member list.
        members = target->v.table.members;
        member_table = target;
    } else if (target->kind == LHAT_TYPE_ERROR_KIND ||
               target->kind == LHAT_TYPE_ERROR_SET ||
               target->kind == LHAT_TYPE_ERROR || all_error_arms(target)) {
        if (chk_name_is(name, length, "message")) {
            return chk_simple(c, LHAT_TYPE_STRING);
        }
        // [errors.kind.local]: either family may be a cause. Wrapping what could not be
        // returned inside one's own error IS resolving it here, and it is the
        // one sanctioned way the thing that failed survives the frame -- so
        // refusing a localerror^ a place in the chain would leave a writer
        // copying the message across by hand.
        if (chk_name_is(name, length, "cause")) {
            return lhat_type_union(c->result->types, chk_any_error(c),
                                   chk_simple(c, LHAT_TYPE_NIL));
        }
        if (target->kind == LHAT_TYPE_ERROR_KIND) {
            members = target->v.error.fields;
        }
        // [errors.kind.made]: the declaration makes types and no values, so a kind of
        // it standing here is a type written where a value was wanted. The
        // name resolves, which is why it reads as a member access at all --
        // and "no such member" would send the writer looking for a spelling
        // mistake instead of for the error^ that [errors.kind.new] puts in front.
        if (target->kind == LHAT_TYPE_ERROR_SET &&
            chk_kind_of_set(target, name, length) != NULL) {
            chk_report_named(c, node, LHAT_CHECK_ERR_KIND_AS_VALUE, name,
                             length);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        // ERROR, ERROR_SET and a union of kinds carry no fields of their own
        // -- [pipeline.value.layout]'s are the leaf's, and reaching one is what [syntax.op.framework]'s narrowing
        // is for. The search below finds nothing and the shared tail still
        // answers tostring.
    } else if (target->kind == LHAT_TYPE_ENUM ||
               target->kind == LHAT_TYPE_ENUM_MEMBER) {
        // [syntax.enum]: in value position E.AAA answers the ENUM type, wide
        // -- a binding made of it takes any member, and two members
        // compare without the checker calling the comparison settled. The
        // member's own singleton type lives in type position
        // (resolve_qualified_type) and in narrowing, where the precision
        // is the point.
        if (target->kind == LHAT_TYPE_ENUM) {
            LhatType *member = chk_kind_of_set(target, name, length);
            if (member != NULL) {
#if LHAT_WITH_RESOLUTIONS
                // [lsp.hover]: the wide type is the answer, but the member is
                // what the name reached and where it was written.
                c->resolved_kind = member;
#endif
                return target;
            }
            if (chk_name_is(name, length, "value")) {
                // An enumerator read through the wide type: any member's.
                LhatType *joined = NULL;
                for (const LhatTypeList *k = target->v.error.kinds;
                     k != NULL; k = k->next) {
                    joined = lhat_type_union(c->result->types, joined,
                                             k->type->v.error.value_type);
                }
                return joined != NULL ? joined
                                      : chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (chk_name_is(name, length, "enum") ||
                chk_name_is(name, length, "enum^")) {
                return target;
            }
        } else {
            // `value` alone: nothing user-written stands here to collide
            // with, and enum^ pairs with the declaring word where value^
            // pairs with nothing.
            if (chk_name_is(name, length, "value")) {
                return target->v.error.value_type != NULL
                           ? target->v.error.value_type
                           : chk_simple(c, LHAT_TYPE_NUMBER);
            }
            if (chk_name_is(name, length, "enum") ||
                chk_name_is(name, length, "enum^")) {
                return target->v.error.set != NULL
                           ? target->v.error.set
                           : chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
        }
    } else if (target->kind == LHAT_TYPE_CORO) {
        // [modules.host.coroutine-ops]: a coroutine carries these without importing anything.
        // [syntax.with.coroutine] spells dispose(), [syntax.sub.coroutine-make] puts resume beside it, and [syntax.for.forms]
        // makes iterate what `in^` asks for.
        //
        // [syntax.sub.coroutine-members]: the three that run the body take the body's own kind, so
        // [syntax.sub.kinds]'s calling rule settles who may advance it -- an f^ reaching for
        // a p^ coroutine is an f^ calling a p^, and nothing about yield^ has
        // to be said again. The three that do not run it (done, started,
        // iterate) read state or hand the coroutine back, so they are f^
        // whatever the body is.
        bool advances = target->v.coroutine.is_function;
        // [syntax.sub.yieldable-f]: the kind alone would let a body advance one it was handed,
        // whose progress the caller can see afterwards. What a body made is
        // its own, exactly as [syntax.sub.table-write] reads it for a table.
        if (advances && c->in_function &&
            (chk_name_is(name, length, "start") ||
             chk_name_is(name, length, "resume") ||
             chk_name_is(name, length, "dispose") ||
             chk_name_is(name, length, "load")) &&
            node != NULL &&
            !chk_receiver_is_own_coroutine(c, node->v.access.target)) {
            chk_report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
        }
        if (chk_name_is(name, length, "start")) {
            // [syntax.sub.yieldable]: runs the body from the top, for a coroutine that has
            // never been resumed. Takes nothing, since nothing has been
            // yield^ed yet to send a value to. Answers what one turn answers.
            LhatType *signature = lhat_type_func(c->result->types, advances);
            signature->v.func.result = coroutine_answer(c, target);
            return signature;
        }
        if (chk_name_is(name, length, "resume")) {
            // [syntax.type.coroutine]: one resume sends R in and answers Y|T. Where R is empty
            // there is nothing to send, so it takes no argument at all --
            // [syntax.type.empty]'s empty argument side, and a caller writing nil^ there is
            // passing an argument the coroutine has no parameter for.
            // [syntax.type.tuple]: a tuple R is sent as that many arguments --
            // resume(a, b) -- and the yield^'s binding takes them apart.
            LhatType *signature = lhat_type_func(c->result->types, advances);
            LhatType *receive = target->v.coroutine.receive;
            size_t sent_width = lhat_type_tuple_width(receive);
            if (sent_width > 0) {
                for (size_t i = 0; i < sent_width; i++) {
                    lhat_type_add_param(c->result->types, signature,
                                        lhat_type_tuple_at(receive, i));
                }
            } else if (receive != NULL) {
                lhat_type_add_param(c->result->types, signature, receive);
            }
            signature->v.func.result = coroutine_answer(c, target);
            return signature;
        }
        if (chk_name_is(name, length, "dispose")) {
            // [syntax.with.no-fail]: it answers nothing, which is what [syntax.with.checked] checks for.
            return lhat_type_func(c->result->types, advances);
        }
        // [syntax.sub.save-load]: save() reads the frame out without running the body, so it
        // is an f^ like done(). load() puts one back, which changes what the
        // next resume does as much as a resume would -- so it takes the
        // body's kind, the rule dispose() follows.
        if (chk_name_is(name, length, "save")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = chk_simple(c, LHAT_TYPE_COSTATE);
            return signature;
        }
        if (chk_name_is(name, length, "load")) {
            LhatType *signature = lhat_type_func(c->result->types, advances);
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_COSTATE));
            return signature;
        }
        if (builtin_named(name, length, "iterate", false)) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = target;  // [syntax.for.forms]: itself
            return signature;
        }
        // [syntax.sub.coroutine-members]: what a resume answers is a union of Y and the result, and
        // nothing keeps those two apart -- a body that yields nil^ and one
        // that has ended answer the same value. So the state is asked for
        // rather than read out of the value.
        if (chk_name_is(name, length, "done")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = chk_simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        // done() alone leaves a fresh coroutine and a suspended one looking
        // alike, so a consumer handed one it did not make could not tell
        // which of start() and resume() this is.
        if (chk_name_is(name, length, "started")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = chk_simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        if (builtin_named(name, length, "tostring", false)) {
            return builtin_tostring(c, target);  // [syntax.obj.tostring]
        }
        report_no_member(c, node, target, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    } else {
        // [syntax.obj.tostring]: nil^, bool^, number^, string^ and the rest hold no members
        // of their own, and this is the one every value answers.
        if (builtin_named(name, length, "tostring", false)) {
            return builtin_tostring(c, target);
        }
        // [syntax.obj.tonumber]: and this is the one only a string^ answers. The bare word
        // alone -- [syntax.obj.count-hat]: a string^ is not a value a writer can add a name
        // to, so there is nothing here for a hat to keep a built-in off, and
        // a second spelling of one member is all it would be.
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "tonumber")) {
            return builtin_tonumber(c);
        }
        // [syntax.obj.eq]: and this is the one only a number^ answers. The bare word
        // alone, for [syntax.obj.count-hat]'s reason -- nothing can be written on a number^.
        if (target->kind == LHAT_TYPE_NUMBER &&
            chk_name_is(name, length, "eq")) {
            return builtin_eq(c);
        }
        // [syntax.obj.round]: and so are these three, for the same reason.
        // [syntax.obj.abs]: abs and sign have the same shape; clamp takes its bounds.
        if (target->kind == LHAT_TYPE_NUMBER &&
            (chk_name_is(name, length, "floor") ||
             chk_name_is(name, length, "ceil") ||
             chk_name_is(name, length, "round") ||
             chk_name_is(name, length, "abs") ||
             chk_name_is(name, length, "sign"))) {
            return builtin_whole(c);
        }
        if (target->kind == LHAT_TYPE_NUMBER &&
            (chk_name_is(name, length, "bitany") ||
             chk_name_is(name, length, "bitall") ||
             chk_name_is(name, length, "bitat") ||
             chk_name_is(name, length, "isinteger"))) {
            return builtin_bits(c, !chk_name_is(name, length, "isinteger"));
        }
        if (target->kind == LHAT_TYPE_NUMBER &&
            chk_name_is(name, length, "clamp")) {
            LhatType *signature = builtin_whole(c);
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_NUMBER));
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_NUMBER));
            return signature;
        }
        // [syntax.obj.count]: how long it is, and how many bytes that is.
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "length") ||
             chk_name_is(name, length, "len") ||
             chk_name_is(name, length, "size"))) {
            return chk_simple(c, LHAT_TYPE_NUMBER);
        }
        // [syntax.obj.substring]: a run of its characters, under any of its three names.
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "substring") ||
             chk_name_is(name, length, "substr") ||
             chk_name_is(name, length, "sub"))) {
            return builtin_substring(c);
        }
        // [syntax.obj.at]: one character of it.
        if (target->kind == LHAT_TYPE_STRING && chk_name_is(name, length, "at")) {
            return builtin_at(c);
        }
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "sliceBytes")) {
            return builtin_substring(c);
        }
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "chars") ||
             chk_name_is(name, length, "codepoints"))) {
            LhatType *item = chk_simple(c, chk_name_is(name, length, "chars")
                                             ? LHAT_TYPE_STRING : LHAT_TYPE_NUMBER);
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result =
                lhat_type_coro(c->result->types, NULL, item, NULL, false, true);
            return signature;
        }
        // [syntax.obj.find]: the plain searches. What is not there answers nil^
        // ([errors.runtime.index]) -- never a sentinel -- and the pattern vocabulary
        // lives in std.regex.
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "find")) {
            LhatType *found = lhat_type_union(
                c->result->types, chk_simple(c, LHAT_TYPE_NUMBER),
                chk_simple(c, LHAT_TYPE_NIL));
            LhatType *bare = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, bare,
                                chk_simple(c, LHAT_TYPE_STRING));
            bare->v.func.result = found;
            LhatType *from = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, from,
                                chk_simple(c, LHAT_TYPE_STRING));
            lhat_type_add_param(c->result->types, from,
                                chk_simple(c, LHAT_TYPE_NUMBER));
            from->v.func.result = found;
            return lhat_type_intersect(c->result->types, bare, from);
        }
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "findall")) {
            // [syntax.sub.yieldable-f]: reading a string changes nothing, so the walk is an
            // f^ coroutine, exactly as a table's keys^ is.
            LhatType *walk = lhat_type_coro(
                c->result->types, NULL, chk_simple(c, LHAT_TYPE_NUMBER),
                NULL, false, true);
            LhatType *signature = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_STRING));
            signature->v.func.result = walk;
            return signature;
        }
        // [syntax.obj.find]: join^'s inverse (with the separator) and the words
        // reading (without). A fresh answer either way -- the caller splits
        // and then mends its own pieces.
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "split")) {
            LhatType *pieces = lhat_type_table(c->result->types);
            pieces->v.table.variadic = chk_simple(c, LHAT_TYPE_STRING);
            LhatType *words = lhat_type_func(c->result->types, true);
            words->v.func.result = pieces;
            words->v.func.answers_fresh = true;
            LhatType *by_sep = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, by_sep,
                                chk_simple(c, LHAT_TYPE_STRING));
            by_sep->v.func.result = pieces;
            by_sep->v.func.answers_fresh = true;
            return lhat_type_intersect(c->result->types, words, by_sep);
        }
        // [syntax.obj.find]: the ASCII case swaps.
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "toupper") ||
             chk_name_is(name, length, "tolower"))) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
            return signature;
        }
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "replace")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_STRING));
            lhat_type_add_param(c->result->types, signature,
                                chk_simple(c, LHAT_TYPE_STRING));
            signature->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
            return signature;
        }
        report_no_member(c, node, target, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (target->kind == LHAT_TYPE_TABLE && node != NULL && c->writing_to == node) {
        target->v.table.entries_known = false;
    }
    if (target->kind == LHAT_TYPE_TABLE && target->v.table.index_key != NULL &&
        !target->v.table.inferred_index && length > 0 && name[length - 1] != '^') {
        chk_expect(c, node, chk_simple(c, LHAT_TYPE_STRING),
                   target->v.table.index_key, LHAT_CHECK_ERR_MISMATCH);
        const LhatTypeMember *known = chk_find_member(target, name, length);
        if (known == NULL) {
            return lhat_type_union(c->result->types, target->v.table.index_value,
                                   chk_simple(c, LHAT_TYPE_NIL));
        }
    }
    // [syntax.obj.tostring-hat]: everywhere but a plain table the two spellings of tostring
    // and iterate are one member, so where the written one misses, the other
    // spelling is searched before any built-in answers -- what was written
    // wins under either spelling.
    const char *look = name;
    size_t look_length = length;
    for (int pass = 0; pass < 2; pass++) {
        // The links go through the one search ([modules.hostdata.base], [syntax.obj.delegate]); the
        // bare list is for what has no table behind it -- an error kind's
        // fields ([errors.kind.errordef]) and a coroutine's built-ins.
        const LhatTypeMember *found =
            member_table != NULL
                ? lhat_type_find_member(member_table, look, look_length)
                : chk_members_search(members, look, look_length);
        {
            const LhatTypeMember *m = found;
            if (m != NULL) {
                // [syntax.obj.compose-ambiguous]: carried by both sides of a composition, so reading
                // it here would be picking one of two for the writer. [syntax.obj.self]'s
                // 'let^ f = A.m' is how the side is named.
                if (m->ambiguous) {
                    chk_report_named(c, node, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER,
                                     name, length);
                    return chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
                // [syntax.obj.delegate]: what answered is a seed, not an inferred type --
                // the body it belongs to has not been walked yet.
                // chk_infer_def walks the entries again when this happened,
                // and the walk after it finds a better one here.
                if (m->provisional) {
                    c->read_provisional = true;
                }
#if LHAT_WITH_RESOLUTIONS
                c->resolved_member = m;
#endif
                if (named_type != NULL && m->names_type) {
                    *named_type = m->named_type;
                }
                if (node != NULL && target->v.table.is_namespace) {
                    ((LhatNode *)node)->checked_definition = m->definition;
                }
                return lhat_type_instantiate_receiver(c->result->types, m->type, target);
            }
        }
        const char *second =
            plain_table_type(target) ? NULL : other_spelling(name, length);
        if (second == NULL) {
            break;
        }
        look = second;
        look_length = strlen(second);
    }

    // [syntax.for.forms]: `in^ e` asks e for the coroutine to walk, and a table answers
    // with one over its keys without anything being written. This comes
    // after the search, not before it, because [syntax.for.forms] lets a written iterate
    // win -- the same order the machine reads it in. A host type ([modules.hostdata])
    // is a table only in how its members are reached -- there is nothing of
    // a table's own behind it to walk, so with none registered there is no
    // built-in to fall back on either.
    if (!(target->kind == LHAT_TYPE_TABLE && target->v.table.nominal) &&
        builtin_named(name, length, "iterate", plain_table_type(target))) {
        // [syntax.type.tuple]: the pair is the tuple (K, V). The walk sends nothing in
        // and ends without a value, which [errors.runtime.index] spells nil^ -- so what
        // resume() answers is (K, V)|nil^, the union [syntax.type.tuple] admits beside
        // an error's, since nil^ is discriminable off the head slot too.
        //
        // [syntax.sub.yieldable-f]: the built-in walk changes nothing, so it is an f^ coroutine
        // -- which is what lets 'for^ k, v in^ t' stand inside an f^ body.
        LhatType *walk =
            lhat_type_coro(c->result->types, NULL,
                           chk_table_walk_tuple(c, target), NULL, false, true);
        LhatType *signature = lhat_type_func(c->result->types, true);
        signature->v.func.result = walk;
        return signature;
    }

    // [syntax.obj.tostring], the same order and for the same reason: a written tostring is
    // the one that answers, and this is what a table falls back to.
    if (builtin_named(name, length, "tostring", plain_table_type(target))) {
        return builtin_tostring(c, target);
    }

    // [syntax.obj.count]: how long the run is, and how much the table holds altogether.
    // The hat is not optional here even where [syntax.obj.tostring-hat] would let the bare word
    // through: [syntax.obj] reserves new, tostring and dispose on a def^ and these
    // are not among them, so on a table the bare words stay the writer's
    // whatever kind of table it is.
    if (target->kind == LHAT_TYPE_TABLE &&
        (builtin_named(name, length, "length", true) ||
         builtin_named(name, length, "len", true) ||
         builtin_named(name, length, "count", true))) {
        return chk_simple(c, LHAT_TYPE_NUMBER);
    }

    // [syntax.for.keys-values]: the two projections of the same walk. Calls, as iterate^ is
    // and for its reason -- each answers a coroutine of its own, so the
    // parentheses are where one is made rather than a name that quietly
    // makes one every time it is read. [syntax.sub.yieldable-f] makes them f^ coroutines
    // because reading a table changes nothing. The hat is not optional, as
    // in [syntax.obj.count] just above.
    if (target->kind == LHAT_TYPE_TABLE &&
        (builtin_named(name, length, "keys", true) ||
         builtin_named(name, length, "values", true))) {
        LhatType *keys = NULL;
        LhatType *values = NULL;
        table_walk_halves(c, target, &keys, &values);
        LhatType *walk =
            lhat_type_coro(c->result->types, NULL,
                           name[0] == 'k' ? keys : values, NULL, false, true);
        LhatType *signature = lhat_type_func(c->result->types, true);
        signature->v.func.result = walk;
        return signature;
    }

    // [syntax.obj.table-ops]: assign^ writes into the container already there, so a def^
    // instance stays one -- the one table operation that reaches past a
    // plain table. Not a host type's: there is nothing of a table behind it.
    if (target->kind == LHAT_TYPE_TABLE && !target->v.table.nominal &&
        builtin_named(name, length, "assign", true)) {
        target->v.table.entries_known = false;
        LhatType *signature = mutable_method(c);
        lhat_type_add_param(c->result->types, signature, target);
        signature->v.func.result = target;
        return signature;
    }

    // [syntax.obj.table-ops]: the table's own operations, on a plain table alone -- a def^'s
    // names are the writer's and a host type's are the library's. The hat is
    // not optional, as in [syntax.obj.count]. E is the sequence half's element type
    // (chk_table_element_type), which is what makes 't.sort^(f^ a, b { … })'
    // read its parameters off the receiver through [pipeline.infer.expected]'s expectation.
    if (plain_table_type(target) && !target->v.table.nominal) {
        LhatType *element = chk_table_element_type(c, target);
        LhatType *self_type = target;
        LhatType *elem_or_nil = lhat_type_union(
            c->result->types, element, chk_simple(c, LHAT_TYPE_NIL));
        LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);

        if (builtin_named(name, length, "range", true) ||
            builtin_named(name, length, "range_downto", true) ||
            builtin_named(name, length, "reverse_range", true) ||
            builtin_named(name, length, "reverse_iterate", true)) {
            LhatType *pair = lhat_type_tuple(c->result->types);
            lhat_type_add_position(c->result->types, pair, number);
            lhat_type_add_position(c->result->types, pair, element);
            LhatType *walk = lhat_type_coro(c->result->types, NULL, pair,
                                           NULL, false, true);
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = walk;
            if (builtin_named(name, length, "reverse_iterate", true)) {
                return signature;
            }
            lhat_type_add_param(c->result->types, signature, number);
            LhatType *bounded = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, bounded, number);
            lhat_type_add_param(c->result->types, bounded, number);
            bounded->v.func.result = walk;
            return lhat_type_intersect(c->result->types, signature, bounded);
        }

        // join^ -- [syntax.op.concat]'s word already means "one string out of pieces".
        if (builtin_named(name, length, "join", true)) {
            LhatType *bare = lhat_type_func(c->result->types, true);
            bare->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
            LhatType *with_sep = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, with_sep,
                                chk_simple(c, LHAT_TYPE_STRING));
            with_sep->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
            return lhat_type_intersect(c->result->types, bare, with_sep);
        }
        if (builtin_named(name, length, "indexof", true)) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, signature, element);
            signature->v.func.result = lhat_type_union(
                c->result->types, number, chk_simple(c, LHAT_TYPE_NIL));
            return signature;
        }
        if (builtin_named(name, length, "contains", true)) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, signature, element);
            signature->v.func.result = chk_simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        // slice^ answers a fresh run of the same elements; the count is the
        // range's, which the type does not say ([syntax.obj.structure]'s unbounded tail).
        if (builtin_named(name, length, "slice", true)) {
            LhatType *cut = lhat_type_table(c->result->types);
            cut->v.table.variadic = element;
            LhatType *from = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, from, number);
            from->v.func.result = cut;
            from->v.func.answers_fresh = true;  // [syntax.sub.fresh]
            LhatType *range = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, range, number);
            lhat_type_add_param(c->result->types, range, number);
            range->v.func.result = cut;
            range->v.func.answers_fresh = true;
            return lhat_type_intersect(c->result->types, from, range);
        }
        // clone^ -- the shallow copy by name, or each value through a
        // written policy ([syntax.obj.table-ops]). The policy is f^any^ -> any^; on purpose,
        // not f^V -> V;: it decides the depth itself, and the V one level
        // down is not the V up here -- 'x.clone^(this^)' has to fit its own
        // signature at every level, which without generics only any^ can
        // say. Inside, [syntax.type.fits]'s narrowing is how a policy tells its cases
        // apart, which is what a policy is. Both arms promise a fresh
        // answer ([syntax.sub.fresh]) -- an f^ clones what arrived, then mends its own
        // copy.
        if (builtin_named(name, length, "clone", true)) {
            LhatType *any = chk_simple(c, LHAT_TYPE_ANY);
            LhatType *bare = lhat_type_func(c->result->types, true);
            bare->v.func.result = self_type;
            bare->v.func.answers_fresh = true;
            LhatType *policy = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, policy, any);
            policy->v.func.result = any;
            LhatType *with_policy = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, with_policy, policy);
            LhatType *mapped = lhat_type_table(c->result->types);
            mapped->v.table.index_key = target->v.table.index_key;
            if (mapped->v.table.index_key != NULL) {
                mapped->v.table.index_value = any;
            }
            with_policy->v.func.result = mapped;
            with_policy->v.func.answers_fresh = true;
            return lhat_type_intersect(c->result->types, bare, with_policy);
        }
        // [syntax.sub.mutable-self]: the mutating half write through the receiver alone, so
        // they are f^mutable^self^ -- an f^ body calls them on what it made
        // itself, a p^ anywhere.
        if (builtin_named(name, length, "push", true) ||
            builtin_named(name, length, "insert", true) ||
            builtin_named(name, length, "extend", true) ||
            builtin_named(name, length, "resize", true) ||
            builtin_named(name, length, "move", true)) {
            target->v.table.entries_known = false;
            if (target->v.table.index_key != NULL && !target->v.table.inferred_index) {
                chk_expect(c, node, number, target->v.table.index_key,
                           LHAT_CHECK_ERR_MISMATCH);
            }
        }
        if (builtin_named(name, length, "push", true)) {
            LhatType *signature = mutable_method(c);
            lhat_type_add_param(c->result->types, signature, element);
            signature->v.func.result = self_type;
            return signature;
        }
        if (builtin_named(name, length, "insert", true)) {
            LhatType *signature = mutable_method(c);
            lhat_type_add_param(c->result->types, signature, number);
            lhat_type_add_param(c->result->types, signature, element);
            signature->v.func.result = self_type;
            return signature;
        }
        if (builtin_named(name, length, "extend", true)) {
            LhatType *more = lhat_type_table(c->result->types);
            more->v.table.variadic = element;
            LhatType *signature = mutable_method(c);
            lhat_type_add_param(c->result->types, signature, more);
            signature->v.func.result = self_type;
            return signature;
        }
        if (builtin_named(name, length, "remove", true)) {
            LhatType *signature = mutable_method(c);
            lhat_type_add_param(c->result->types, signature, number);
            signature->v.func.result = elem_or_nil;
            return signature;
        }
        if (builtin_named(name, length, "pop", true)) {
            LhatType *signature = mutable_method(c);
            signature->v.func.result = elem_or_nil;
            return signature;
        }
        // [syntax.obj.table-ops]: resize^(n, fill) for any element. resize^(n) fills with
        // nil^, so it is there only where the element admits nil^.
        if (builtin_named(name, length, "resize", true)) {
            LhatType *filled = mutable_method(c);
            lhat_type_add_param(c->result->types, filled, number);
            lhat_type_add_param(c->result->types, filled, element);
            filled->v.func.result = self_type;
            if (!lhat_type_conforms(chk_simple(c, LHAT_TYPE_NIL), element)) {
                return filled;
            }
            LhatType *bare = mutable_method(c);
            lhat_type_add_param(c->result->types, bare, number);
            bare->v.func.result = self_type;
            return lhat_type_intersect(c->result->types, bare, filled);
        }
        // [syntax.op.compare]'s three-way answer is the comparison's shape here too.
        if (builtin_named(name, length, "sort", true) ||
            builtin_named(name, length, "stablesort", true)) {
            LhatType *bare = mutable_method(c);
            bare->v.func.result = self_type;
            LhatType *cmp = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, cmp, element);
            lhat_type_add_param(c->result->types, cmp, element);
            cmp->v.func.result = number;
            LhatType *with_cmp = mutable_method(c);
            lhat_type_add_param(c->result->types, with_cmp, cmp);
            with_cmp->v.func.result = self_type;
            return lhat_type_intersect(c->result->types, bare, with_cmp);
        }
        // move^: one element relocated, a block copied within self, or a
        // block copied in from another table -- told apart by arity and by
        // what stands first.
        if (builtin_named(name, length, "move", true)) {
            LhatType *more = lhat_type_table(c->result->types);
            more->v.table.variadic = element;
            LhatType *one = mutable_method(c);
            lhat_type_add_param(c->result->types, one, number);
            lhat_type_add_param(c->result->types, one, number);
            one->v.func.result = self_type;
            LhatType *block = mutable_method(c);
            lhat_type_add_param(c->result->types, block, number);
            lhat_type_add_param(c->result->types, block, number);
            lhat_type_add_param(c->result->types, block, number);
            block->v.func.result = self_type;
            LhatType *from_one = mutable_method(c);
            lhat_type_add_param(c->result->types, from_one, more);
            lhat_type_add_param(c->result->types, from_one, number);
            lhat_type_add_param(c->result->types, from_one, number);
            from_one->v.func.result = self_type;
            LhatType *from_block = mutable_method(c);
            lhat_type_add_param(c->result->types, from_block, more);
            lhat_type_add_param(c->result->types, from_block, number);
            lhat_type_add_param(c->result->types, from_block, number);
            lhat_type_add_param(c->result->types, from_block, number);
            from_block->v.func.result = self_type;
            return lhat_type_intersect(
                c->result->types,
                lhat_type_intersect(c->result->types, one, block),
                lhat_type_intersect(c->result->types, from_one, from_block));
        }
        if (builtin_named(name, length, "reverse", true) ||
            builtin_named(name, length, "clear", true)) {
            LhatType *signature = mutable_method(c);
            signature->v.func.result = self_type;
            return signature;
        }
    }

    report_no_member(c, node, target, name, length);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

static bool string_member_key(Checker *c, const LhatNode *key)
{
    if (key == NULL || key->kind != LHAT_NODE_STRING) {
        return false;
    }
    LhatTypeMember probe = {0};
    probe.name = c->lexer->strings + key->v.string.offset;
    probe.name_length = key->v.string.length;
    return lhat_type_member_key_kind(&probe) == LHAT_TYPE_STRING;
}

// Without an explicit index constraint, collect the declared members a key
// can reach. NULL leaves the result unknown, as ordinary width subtyping does.
static LhatType *reachable_by_key(Checker *c, const LhatType *over,
                                  const LhatType *key)
{
    // chk_operator_undecided rather than conformance: lhat_type_conforms fits
    // unknown^ against anything, so asking it first would drop an undecided
    // key into whichever half was tested first.
    bool any_kind = chk_operator_undecided(key);
    bool by_position =
        any_kind || lhat_type_conforms(key, chk_simple(c, LHAT_TYPE_NUMBER));
    bool by_name =
        any_kind || lhat_type_conforms(key, chk_simple(c, LHAT_TYPE_STRING));

    LhatType *reachable = NULL;
    for (const LhatTypeMember *m = over->v.table.members; m != NULL;
         m = m->next) {
        if (member_is_positional(m) ? by_position : by_name) {
            reachable = lhat_type_union(c->result->types, reachable, m->type);
        }
    }
    // [syntax.type.variadic]: an unbounded tail is more positions beyond any listed, so a key
    // that could name a position could name one of those.
    if (by_position && over->v.table.variadic != NULL) {
        reachable =
            lhat_type_union(c->result->types, reachable, over->v.table.variadic);
    }
    return reachable;
}

// [syntax.type.fits-range] with [syntax.obj.structure]: what the type answers for every position a bounded key
// could name, or NULL where the key is not bounded or could leave them. The
// positions have to run from zero without a gap -- a value fitting the type
// has each one it declares, and nothing is promised past the first it does
// not.
static LhatType *within_declared_positions(Checker *c, const LhatType *over,
                                           const LhatNode *key)
{
    LhatInteger lo = 0;
    LhatInteger hi = 0;
    if (key == NULL || key->next != NULL ||
        !chk_bounds_of(c, key, &lo, &hi) || lo < 0) {
        return NULL;
    }
    LhatType *reached = NULL;
    for (LhatInteger at = lo; at <= hi; at++) {
        const LhatTypeMember *m = lhat_type_member_at(over, (size_t)at);
        if (m == NULL) {
            return NULL;  // past what the type declares
        }
        reached = lhat_type_union(c->result->types, reached, m->type);
    }
    return reached;
}

static LhatType *dictionary_index_type(Checker *c, const LhatNode *node,
                                        LhatType *over, LhatType *key)
{
    if (over == NULL) {
        return NULL;
    }
    if (over->kind == LHAT_TYPE_TABLE &&
        over->v.table.hostdata_tag != NULL) {
        const LhatTypeMember *getter = chk_find_member(over, "[]", 2);
        if (getter != NULL) {
            if (c->writing_to == node) {
                chk_report(c, node, LHAT_CHECK_ERR_TABLE_IS_SEALED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *args[1] = { key };
            const LhatType *arm = operator_arm(getter->type, args, 1, false);
            if (arm == NULL) {
                chk_report(c, node->v.access.argument, LHAT_CHECK_ERR_NO_OPERATOR);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return lhat_type_call_answer(arm);
        }
    }
    if (over->kind == LHAT_TYPE_TABLE && over->v.table.index_key != NULL &&
        !over->v.table.inferred_index) {
        chk_expect(c, node->v.access.argument, key, over->v.table.index_key,
                   LHAT_CHECK_ERR_MISMATCH);
        if (key != NULL && key->kind == LHAT_TYPE_NIL) {
            chk_report(c, node->v.access.argument, LHAT_CHECK_ERR_BAD_KEY);
        }
        return lhat_type_union(c->result->types, over->v.table.index_value,
                               chk_simple(c, LHAT_TYPE_NIL));
    }
    if (over->kind != LHAT_TYPE_UNION && over->kind != LHAT_TYPE_INTERSECT) {
        return NULL;
    }
    LhatType *answer = NULL;
    bool unspecified = false;
    for (const LhatTypeList *arm = over->v.composite.arms; arm != NULL;
         arm = arm->next) {
        LhatType *part = dictionary_index_type(c, node, arm->type, key);
        if (part == NULL) {
            unspecified = true;
            continue;
        }
        bool all = over->kind == LHAT_TYPE_INTERSECT || c->writing_to == node;
        answer = all ? lhat_type_intersect(c->result->types, answer, part)
                     : lhat_type_union(c->result->types, answer, part);
    }
    if (answer != NULL && unspecified && over->kind == LHAT_TYPE_UNION &&
        c->writing_to != node) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    return answer;
}

// The '[' half of the same. Lifted out of infer's switch so that it reads
// beside infer_member, which it now shares its nil^ handling with.
static LhatType *infer_index(Checker *c, const LhatNode *node)
{
    LhatType *over = chk_infer(c, node->v.access.target);
    LhatType *asked = chk_require_value(c, node->v.access.argument,
                                        chk_infer(c, node->v.access.argument));
    // [modules.hostvalue.union]: a box's key hash is its bytes, and only a sealed box
    // keeps them still -- so storing a key asks for constbox^. A lookup may
    // ask with a live box, or with the bare value itself: both read the
    // bytes of the moment, and everything the table holds is sealed. Only
    // storing asks for the sealed box -- a bare value cannot BE a stored
    // key at all, its width not fitting the entry's one slot.
    if (asked != NULL && asked->kind == LHAT_TYPE_HOSTVALUE_BOX &&
        !asked->v.table.sealed && c->writing_to == node) {
        chk_report(c, node->v.access.argument, LHAT_CHECK_ERR_MUTABLE_KEY);
    }
    if (chk_is_hostvalue(asked) && c->writing_to == node) {
        chk_report(c, node->v.access.argument,
                   LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
    }
    // [errors.runtime.nil]: as in infer_member -- relaxed steps past a nil^
    // arm, and '?[' steps past it under strict as well.
    over = access_target(c, node, over);
    LhatType *dictionary = dictionary_index_type(c, node, over, asked);
    if (dictionary != NULL && (over->kind != LHAT_TYPE_TABLE ||
                               over->v.table.hostdata_tag != NULL)) {
        return dictionary;
    }
    // [errors.runtime.index]: only a table holds keys. A settled type that is not one
    // answers a key with nothing -- the machine refuses it, so the checker
    // says so first. Unions and undecided types stay with the machine.
    if (over != NULL) {
        switch (over->kind) {
            case LHAT_TYPE_NIL:
            case LHAT_TYPE_BOOL:
            case LHAT_TYPE_NUMBER:
            case LHAT_TYPE_STRING:
            case LHAT_TYPE_FUNC:
            case LHAT_TYPE_CORO:
            case LHAT_TYPE_TUPLE:
            case LHAT_TYPE_NONE:
            case LHAT_TYPE_ERROR:
            case LHAT_TYPE_ERROR_SET:
            case LHAT_TYPE_ERROR_KIND:
            case LHAT_TYPE_ENUM:
            case LHAT_TYPE_ENUM_MEMBER:
            case LHAT_TYPE_HOSTVALUE:
            case LHAT_TYPE_HOSTVALUE_BOX:
                chk_report(c, node, LHAT_CHECK_ERR_NOT_INDEXABLE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            default:
                break;
        }
    }
    // A key written out names one position or one member, so the
    // table says what is there. [errors.runtime.index]: a key that is not there
    // answers nil^ -- but a written one that the type does not
    // mention says nothing, since [syntax.obj.structure] lets a table carry more than
    // it declares.
    const LhatNode *key = node->v.access.argument;
    if (over != NULL && over->kind == LHAT_TYPE_TABLE && c->writing_to == node) {
        over->v.table.entries_known = false;
    }
    if (over != NULL && over->kind == LHAT_TYPE_TABLE && key != NULL &&
        key->next == NULL) {
        const LhatTypeMember *found = NULL;
        if (key->kind == LHAT_NODE_INT) {
            found = lhat_type_member_at(over, (size_t)key->v.integer.value);
        } else if (string_member_key(c, key)) {
            found = chk_find_member(over, c->lexer->strings + key->v.string.offset,
                                    key->v.string.length);
        }
        if (found != NULL) {
            return found->type;
        }
    }
    if (dictionary != NULL) {
        return dictionary;
    }
    // [syntax.type.fits-range]: a key the branch bounded, standing where the type declares
    // every position it could reach. [syntax.obj.structure] makes a table type "at least
    // these", so a declared position is one a fitting value has -- and a key
    // that cannot leave them cannot find nothing. This is the static half of
    // [syntax.op.semantic] arrived at by another road, so the answer carries no nil^.
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        LhatType *within = within_declared_positions(c, over, key);
        if (within != NULL) {
            return within;
        }
    }
    // [errors.runtime.index]: a key that did not resolve to one member still has an
    // answer -- what the type says a key of this kind reaches, and nil^ for
    // the keys it says nothing about, since absence is an ordinary result
    // rather than a failure. That union is what [errors.runtime.index]'s 'T|nil^' names.
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        LhatType *reachable = reachable_by_key(c, over, asked);
        if (reachable != NULL) {
            return lhat_type_union(c->result->types, reachable,
                                   chk_simple(c, LHAT_TYPE_NIL));
        }
    }
    // Nothing the type says is reachable this way, so there is no T to make
    // 'T|nil^' out of -- [syntax.obj.structure] leaves what an undeclared table holds unsaid,
    // and [pipeline.strict.relaxed] leaves that to the machine.
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

static bool declares_self(Checker *c, const LhatNode *value);
static LhatType *declared_signature(Checker *c, const LhatNode *value);

static LhatType *infer_table(Checker *c, const LhatNode *node)
{
    const LhatNode *items = node->kind == LHAT_NODE_NEW
                                ? node->v.named.members : node->v.list.items;
    LhatType *table = lhat_type_table(c->result->types);
    table->v.table.entries_known = true;

    // [syntax.obj.self]: a method written in the literal reaches the table through its
    // self^ receiver, the same receiver a def^'s method takes -- [syntax.stmt.old-world] took
    // the bound name out of the initialiser, and this is the door that
    // stays. The named signatures go in first, annotations only, so a body
    // walked below can call a method declared after it. (The full fixpoint
    // stays def^'s, [syntax.obj.members-order]: a member whose type only its body knows is not
    // seeded here, and reading it ahead is what a def^ is for.)
    for (const LhatNode *entry = items; entry != NULL;
         entry = entry->next) {
        const char *seeded = NULL;
        size_t seeded_length = 0;
        if (entry->v.entry.computed || entry->v.entry.value == NULL ||
            entry->v.entry.value->kind != LHAT_NODE_FUNC ||
            !chk_node_name(c, entry->v.entry.key, &seeded, &seeded_length)) {
            continue;
        }
        LhatType *seed = entry->v.entry.type != NULL
                             ? chk_resolve_type(c, entry->v.entry.type)
                             : declared_signature(c, entry->v.entry.value);
        if (seed != NULL) {
            lhat_type_add_member(c->result->types, table, seeded,
                                 seeded_length, seed);
        }
    }

    // [syntax.obj] makes a table a sequence as well as a mapping. The keyed
    // half is described by name; the sequence half by position, counted the
    // way the machine lays it out -- from zero, in the order written.
    size_t position = 0;
    // [syntax.sub.table-write]: how much of what the entries hold was made here as well.
    uint8_t made = LHAT_OWN_DEEP;
    for (const LhatNode *entry = items; entry != NULL;
         entry = entry->next) {
        // The receiver scope, exactly as chk_infer_def pushes it: self^ is
        // the literal's own type, which is still growing but is the one
        // object -- by the time anything calls the method, it is whole.
        Scope *outer = c->scope;
        Scope receiver;
        bool method = declares_self(c, entry->v.entry.value);
        if (method) {
            chk_scope_open(&receiver, outer, NULL, false);
            Binding *bound =
                chk_scope_add(&receiver, "self^", 5, table, node->offset);
            if (bound != NULL) {
                bound->reached = true;
                bound->declaration = entry->v.entry.value;
            }
            c->scope = &receiver;
        }
        LhatType *annotation = entry->v.entry.type != NULL
                                   ? chk_resolve_type(c, entry->v.entry.type) : NULL;
        LhatType *outer_expected = c->expected_func;
        c->expected_func = annotation;
        LhatType *value = chk_require_value(c, entry->v.entry.value,
                                            chk_infer(c, entry->v.entry.value));
        c->expected_func = outer_expected;
        uint8_t brought = chk_made_depth(c, entry->v.entry.value, value);
        made = brought < made ? brought : made;
        if (annotation != NULL) {
            chk_expect(c, entry->v.entry.value, value, annotation, LHAT_CHECK_ERR_MISMATCH);
            value = annotation;
        }
#if LHAT_WITH_RESOLUTIONS
        ((LhatNode *)entry)->display_type = value;
#endif
        if (method) {
            c->scope = outer;
            chk_scope_close(c, &receiver);
        }

        // [modules.hostvalue]: a table lives on the heap and a host value does not
        // leave the stack, so no member of one is ever a host value.
        if (chk_is_hostvalue(value)) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        // [syntax.type.tuple]: nor a tuple, for the same reason said of slots -- a member
        // is one value, and a run is several. pack^ is what puts one in a
        // table, and it makes the table itself.
        if (lhat_type_tuple_width(value) > 0) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        }

        // [syntax.obj.keys-equals]: a computed key is an expression, checked like any other.
        // What it names is only known when it is written out -- an integer
        // reaches the sequence half, a string the keyed one, and anything
        // else lands somewhere [syntax.obj.structure] lets the type stay quiet about.
        if (entry->v.entry.computed) {
            const LhatNode *key = entry->v.entry.key;
            LhatType *asked = chk_require_value(c, key, chk_infer(c, key));
            uint8_t keyed = chk_made_depth(c, key, asked);
            made = keyed < made ? keyed : made;
            // [errors.runtime.index]: nil^ is how "not there" is spelled, so it cannot
            // also be a key. The machine reports one that turns out to be
            // nil^; a key that can only ever be one is decided here. A NaN
            // is the other refusal, and stays the machine's -- nothing in
            // [syntax.obj.number]'s one number type tells them apart.
            if (asked != NULL && asked->kind != LHAT_TYPE_UNKNOWN &&
                asked->kind != LHAT_TYPE_PENDING &&
                lhat_type_conforms(asked, chk_simple(c, LHAT_TYPE_NIL))) {
                chk_report(c, key, LHAT_CHECK_ERR_BAD_KEY);
            }
            // [modules.hostvalue.union]: a literal's key is stored, so only a sealed box
            // holds its hash still -- and a bare host value does not fit a
            // key's one slot at all.
            if (asked != NULL && asked->kind == LHAT_TYPE_HOSTVALUE_BOX &&
                !asked->v.table.sealed) {
                chk_report(c, key, LHAT_CHECK_ERR_MUTABLE_KEY);
            }
            if (chk_is_hostvalue(asked)) {
                chk_report(c, key, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            if (value != NULL && value->kind == LHAT_TYPE_NIL) {
                continue;
            }
            table->v.table.inferred_index = true;
            table->v.table.index_key = lhat_type_union(
                c->result->types, table->v.table.index_key, asked);
            table->v.table.index_value = lhat_type_union(
                c->result->types, table->v.table.index_value, value);
            if (key != NULL && key->kind == LHAT_NODE_INT &&
                key->v.integer.value >= 0) {
                lhat_type_add_index_member(c->result->types, table,
                                           (size_t)key->v.integer.value, value);
            } else if (string_member_key(c, key)) {
                lhat_type_add_member(c->result->types, table,
                                     c->lexer->strings + key->v.string.offset,
                                     key->v.string.length, value);
            }
            continue;
        }

        const char *name = NULL;
        size_t length = 0;
        chk_refuse_hat_name(c, entry->v.entry.key, true);
        if (chk_node_name(c, entry->v.entry.key, &name, &length)) {
            chk_check_tostring_shape(c, entry, value, name, length, false);
            // A seeded signature is written over with what the body turned
            // out to be -- one member, never two of one name.
            const LhatTypeMember *already =
                chk_find_member(table, name, length);
            if (already != NULL) {
                ((LhatTypeMember *)already)->type = value;
            } else {
                lhat_type_add_member(c->result->types, table, name, length,
                                     value);
            }
            continue;
        }
        if (entry->v.entry.key == NULL) {
            lhat_type_add_index_member(c->result->types, table, position++,
                                       value);
        }
    }
    ((LhatNode *)node)->checked_own =
        made == LHAT_OWN_DEEP ? made : (uint8_t)(made + 1);
    if (table->v.table.index_key != NULL) {
        join_table_entry_types(c, table, &table->v.table.index_key,
                                &table->v.table.index_value);
    }
    return table;
}

// [syntax.sub.yieldable]: folds one more yield^/await^ site into the body's running Y or R.
// The first site fixes it; every later one has to agree, or the body is
// mixing yields the way [syntax.type.coroutine] does not allow.
void chk_unify_yield(Checker *c, const LhatNode *at, LhatType **slot,
                     LhatType *candidate)
{
    if (candidate == NULL || candidate->kind == LHAT_TYPE_UNKNOWN ||
        candidate->kind == LHAT_TYPE_PENDING) {
        // [syntax.type.fits]: UNKNOWN carries no information, so there is nothing here to
        // agree or disagree with.
        return;
    }
    // [modules.hostvalue.union]: a host value rides a yield^ whole -- one seat, its full
    // width, never mixed into a run (the tuple-position rule keeps that).
    // The machine carries it through the frame's answer room the way a
    // wide return^ travels, so nothing is refused here any more.
    if (*slot == NULL) {
        *slot = candidate;
        return;
    }
    if (!lhat_type_equal(*slot, candidate)) {
        chk_report(c, at, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    }
}

// [pipeline.infer]: the result type is inferred from return^ unless it is written,
// and a subroutine that calls itself has to write it.
// [syntax.obj.abstract-override]: the shape a subroutine literal declares, read off its annotations
// alone. What super^ stands for inside a pending override^ has to be known
// before the body is walked, and the body is what infer_func would walk.
//
// Answers NULL for anything that is not a subroutine literal, and leaves the
// result NULL when none was written -- [syntax.type.empty] makes an f^ declare one, so what
// is missing here belongs to a p^, which answers with nothing anyway.
static LhatType *declared_signature(Checker *c, const LhatNode *node)
{
    if (node == NULL || node->kind != LHAT_NODE_FUNC) {
        return NULL;
    }
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    func->v.func.yields = node->v.func.yields;
    func->v.func.answers_fresh = node->v.func.answers_fresh;  // [syntax.sub.fresh]
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            // [syntax.sub.mutable-self]: the marker travels with the seat. On a p^ it says
            // nothing a p^ could not already do, so it is not recorded.
            if (param->v.param.mutable_receiver && func->v.func.is_function) {
                func->v.func.mutable_self = true;
            }
            continue;
        }
        LhatType *type = param->v.param.type != NULL
                             ? chk_resolve_type(c, param->v.param.type)
                             : chk_simple(c, LHAT_TYPE_PENDING);
        if (param->v.param.variadic) {
            // [modules.hostvalue]: the tail is collected into a table ([syntax.type.variadic]), and a
            // table member is never a host value.
            if (chk_is_hostvalue(type)) {
                chk_report(c, param, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            func->v.func.variadic =
                param->v.param.type != NULL ? type : chk_simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);
    }
    if (node->v.func.return_type != NULL) {
        c->tuple_allowed = true;  // [syntax.type.tuple], as above
        func->v.func.result = chk_resolve_type(c, node->v.func.return_type);
    }
    // [syntax.sub.no-contagion]: a yieldable's call answers a coroutine, and the seed says so
    // too -- a walk written against a member seeded here asks no more.
    // What it yields only the body knows, so with nothing written the
    // answer is a coroutine of unknowns.
    if (node->v.func.yields && func->v.func.answers == NULL) {
        LhatType *written = func->v.func.result;
        func->v.func.answers =
            written != NULL && written->kind == LHAT_TYPE_CORO
                ? written
                : lhat_type_coro(c->result->types, NULL,
                                 chk_simple(c, LHAT_TYPE_UNKNOWN), NULL,
                                 false, node->v.func.is_function);
    }
    return func;
}

// [pipeline.infer.call-shapes]: a literal whose call shapes were recorded is walked once
// per shape -- each walk reads the literal again with the shape standing
// where an annotation would ([pipeline.infer.expected]'s expectation path), so none of them
// collects demands or reports a parameter undecided. What a walk under one
// shape reported is rolled back the way a round's walk is ([pipeline.infer.rounds]): the
// call that handed the shape in reads `failed` and says so at its own
// position. The walk that stays -- the stamp the compiler reads, the type
// the binding holds -- is over the join of the shapes the body took, so the
// one body still serves every caller.
#if LHAT_WITH_RESOLUTIONS
static void merge_instance_resolutions(LhatCheckResult *result, size_t base, size_t first)
{
    size_t write = first;
    for (size_t read = first; read < result->resolution_count; read++) {
        LhatResolution incoming = result->resolutions[read];
        size_t at = base;
        for (; at < write; at++) {
            if (result->resolutions[at].use == incoming.use) break;
        }
        if (at == write) {
            result->resolutions[write++] = incoming;
            continue;
        }
        LhatResolution *kept = &result->resolutions[at];
        kept->type = lhat_type_union(result->types, kept->type, incoming.type);
        if (kept->call_signature != NULL && incoming.call_signature != NULL) {
            kept->call_signature = lhat_type_intersect(result->types,
                (LhatType *)kept->call_signature, (LhatType *)incoming.call_signature);
        }
        if (kept->definition != incoming.definition ||
            kept->definition_path != incoming.definition_path) {
            kept->has_definition = false;
        }
    }
    result->resolution_count = write;
}
#endif

static LhatType *infer_func_instantiated(Checker *c, const LhatNode *node)
{
    const LhatNode *outer = c->instantiating;
    c->instantiating = node;
#if LHAT_WITH_RESOLUTIONS
    size_t resolution_base = c->result->resolution_count;
#endif

    for (Instantiation *i = c->instantiations; i != NULL; i = i->next) {
        if (i->func != node) {
            continue;
        }
        size_t diagnostics = c->result->diagnostic_count;
#if LHAT_WITH_RESOLUTIONS
        size_t resolutions = c->result->resolution_count;
#endif
        LhatType *shape =
            lhat_type_func(c->result->types, node->v.func.is_function);
        for (size_t k = 0; k < i->count; k++) {
            lhat_type_add_param(c->result->types, shape, i->args[k]);
        }
        c->expected_func = shape;
        i->signature = chk_infer_func(c, i->concrete->body);
        i->concrete->signature = i->signature;
        i->failed = c->result->diagnostic_count > diagnostics;
        retain_instance_failure(c, i, diagnostics);
        c->result->diagnostic_count = diagnostics;
#if LHAT_WITH_RESOLUTIONS
        merge_instance_resolutions(c->result, resolution_base, resolutions);
#endif
    }

    // The external contract preserves argument/result correlations. Every
    // arm's body already carries its own complete semantic result.
    LhatType *settled = lhat_type_instance_contract(c->result->types, node->checked_instances);
    ((LhatNode *)node)->checked_type = settled;
    c->instantiating = outer;
    return settled;
}

LhatType *chk_infer_func(Checker *c, const LhatNode *node)
{
    if (node->v.func.is_template) return infer_template(c, node);
    if (!chk_instantiations_exist(c, node)) {
        ((LhatNode *)node)->checked_instances = NULL;
    }
    // [pipeline.infer.expected]: what expects this literal, taken here and cleared at once
    // -- a body written inside this one is expected by nothing, and reading
    // an outer expectation there would put a type on a parameter no one was
    // talking about.
    LhatType *expected_func = c->expected_func;
    c->expected_func = NULL;
    // [pipeline.infer.expected]: a seat written 'f^…;|nil^' -- or gsub's 'string^|f^…;' -- still
    // says which signature a literal there is expected to have. The one
    // func arm is the expectation; the other arms are other values' to fill.
    if (expected_func != NULL && expected_func->kind == LHAT_TYPE_UNION) {
        LhatType *only_func = NULL;
        for (const LhatTypeList *arm = expected_func->v.composite.arms;
             arm != NULL; arm = arm->next) {
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC) {
                if (only_func != NULL) {
                    only_func = NULL;  // two candidates say nothing
                    break;
                }
                only_func = arm->type;
            }
        }
        expected_func = only_func;
    }
    if (expected_func != NULL && (expected_func->kind != LHAT_TYPE_FUNC ||
                                  (expected_func->v.func.is_function &&
                                   !node->v.func.is_function))) {
        expected_func = NULL;  // not a signature for this; nothing to take
    }
    const LhatTypeList *expected_param =
        expected_func != NULL ? expected_func->v.func.params : NULL;

    // [pipeline.infer.call-shapes]: call shapes were recorded for this literal, and nothing
    // else expects it here -- the shape walks are what read the body now.
    if (expected_func == NULL && c->instantiating != node &&
        chk_instantiations_exist(c, node)) {
        return infer_func_instantiated(c, node);
    }

    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    // [syntax.sub.yieldable]: whether the body suspends is read off the body, not written.
    if (func != NULL) func->source_body = node;
    func->v.func.yields = node->v.func.yields;
    // 15.13: and whether it promises to capture nothing is written, not read
    // -- what a caller may rely on is what the writer said.
    // [syntax.sub.fresh]: as is whether the answer is promised new.
    func->v.func.answers_fresh = node->v.func.answers_fresh;

    // [pipeline.infer]: where this body's own parameters begin. A body nested in
    // another leaves the enclosing one's in place behind this mark -- a demand
    // written in here still reaches them, since the value it names came from
    // out there.
    ParamVar *param_mark = c->param_vars;

    Scope body;
    chk_scope_open(&body, c->scope, node, false);

    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        // [syntax.obj.self]: the receiver is not an ordinary parameter. Leaving it unbound
        // lets the self^ that infer_def put in scope show through, which is
        // what the body is actually talking about.
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            // [syntax.sub.mutable-self]: the marker travels with the seat. On a p^ it says
            // nothing a p^ could not already do, so it is not recorded.
            if (param->v.param.mutable_receiver && func->v.func.is_function) {
                func->v.func.mutable_self = true;
            }
            continue;
        }
        // [pipeline.infer.expected]: what the position is expected to take, which stands
        // where a written annotation would. The variadic one is expected by
        // the signature's own tail; the rest walk in step, since neither
        // list counts the receiver ([syntax.obj.self]).
        LhatType *expected = NULL;
        if (expected_func != NULL) {
            if (param->v.param.variadic) {
                expected = expected_func->v.func.variadic;
            } else if (expected_param != NULL) {
                expected = expected_param->type;
                expected_param = expected_param->next;
            }
        }

        LhatType *type = param->v.param.type != NULL
                             ? chk_resolve_type(c, param->v.param.type)
                         : expected != NULL
                             ? expected
                             : chk_simple(c, LHAT_TYPE_PENDING);
#if LHAT_WITH_RESOLUTIONS
        ((LhatNode *)param)->display_type = type;
#endif
        // [syntax.type.param-names]: a default is what completion and the visual editor write into
        // a call site, so it has to fit the position it will be written into.
        // Read out here, before c->scope becomes the body below -- the
        // expression stands at the call, where none of these parameters is in
        // scope, so it may not name them.
        //
        // Only against a written type. With nothing written the slot is still
        // pending^, and [pipeline.infer] leaves what settles it to the body's demands;
        // a default is not one of them (it is a value the call carries, not a
        // use the body makes).
        // [pipeline.infer.expected]: the written type stands beside the default the same way it
        // stands beside an argument, so a literal default takes it too.
        LhatType *outer_expected = c->expected_func;
        c->expected_func = param->v.param.type != NULL ? type : NULL;
        LhatType *fallback = chk_infer(c, param->v.param.fallback);
        c->expected_func = outer_expected;
        if (fallback != NULL && param->v.param.type != NULL) {
            chk_expect(c, param->v.param.fallback, fallback, type,
                       LHAT_CHECK_ERR_MISMATCH);
        }
        if (param->v.param.variadic) {
            LhatType *element =
                param->v.param.type != NULL ? type : chk_simple(c, LHAT_TYPE_ANY);
            func->v.func.variadic = element;
            // [syntax.type.variadic]: '...' inside the body names what was collected. [syntax.obj.structure]'s
            // form for that is a table whose sequence is unbounded, one
            // element type throughout -- the same shape a written
            // 't^{ T[] }' names, since both describe the same thing.
            LhatType *collected = lhat_type_table(c->result->types);
            collected->v.table.variadic = element;
            Binding *b = chk_scope_add(&body, "...", 3, collected, param->offset);
            if (b != NULL) {
                b->reached = true;
                b->is_parameter = true;
                b->declaration = param;
                ((LhatNode *)param)->checked_binding = param;
            }
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);
        // [pipeline.infer]: with nothing written, the body is what decides. The
        // binding below and the signature above hold this same object, so
        // settling it once at the end writes through to both. A position an
        // expectation filled is decided already and collects no demands --
        // the body is checked against it, as against a written one.
        if (param->v.param.type == NULL && expected == NULL) {
            chk_push_param_var(c, type, param);
        }

        const char *name = NULL;
        size_t length = 0;
        if (!chk_is_discard(c, param->v.param.name)) {
            chk_refuse_hat_name(c, param->v.param.name, false);
        }
        if (chk_node_name(c, param->v.param.name, &name, &length)) {
            Binding *b = chk_scope_add(&body, name, length, type,
                                       param->v.param.name->offset);
            if (b != NULL) {
                b->declaration = param->v.param.name;
                ((LhatNode *)b->declaration)->checked_binding = b->declaration;
                b->reached = true;
                b->is_parameter = true;
            }
        }
    }

    LhatType *declared = NULL;
    if (node->v.func.return_type != NULL) {
        c->tuple_allowed = true;  // [syntax.type.tuple], as in resolve_func_type
        declared = chk_resolve_type(c, node->v.func.return_type);
    }
    // [pipeline.infer.expected]: a signature written on the binding says what the result is
    // as plainly as one written here does. The parameters above are filled
    // from it for that reason, and the result is not a different question --
    // leaving it out would have the body infer a result that the very
    // signature it was written under already gave.
    if (declared == NULL && expected_func != NULL) {
        declared = expected_func->v.func.result;
    }
    // [syntax.sub.no-contagion] with [syntax.type.coroutine]: what a call to a yielding body answers is the coroutine,
    // and what the body itself returns is the third of its three types. So a
    // c^{ … } in the result position is unwrapped and the body handed that
    // third type -- the coroutine around it belongs to the caller. Written on
    // the literal or on the binding is the same statement about the same
    // value, so both spellings are read the same way here. Y and R stay
    // [syntax.sub.yieldable]'s to settle from the yield^ sites; what was written of them is
    // checked against those below rather than handed down.
    LhatType *written_coroutine = NULL;
    if (node->v.func.yields && declared != NULL &&
        declared->kind == LHAT_TYPE_CORO) {
        written_coroutine = declared;
        declared = declared->v.coroutine.result;
    }
    func->v.func.result = declared;

    Scope *outer_scope = c->scope;
    LhatType *outer_declared = c->declared_result;
    LhatType *outer_inferred = c->inferred_result;
    bool outer_self_call = c->saw_self_call;
    bool outer_recursive = c->recursive_return;
    bool outer_valueless = c->valueless_return;
    LhatType *outer_this = c->this_type;
    LhatType *outer_coroutine_produce = c->coroutine_produce;
    LhatType *outer_coroutine_receive = c->coroutine_receive;
    enum YieldContext outer_yield_context = c->yield_context;
    LhatType *outer_yield_bound_type = c->yield_bound_type;
    bool outer_in_function = c->in_function;
    Scope *outer_body_scope = c->body_scope;
    // [errors.catch.clause]: a try^ written in this body belongs to this body, whatever
    // block the literal itself was written inside.
    struct CatchFrame *outer_catch_frame = c->catch_frame;
    c->catch_frame = NULL;

    // [syntax.obj.new]: whether this literal is the written new being walked. Only
    // its own statements are a constructor's -- a literal nested inside it is
    // an ordinary body again, which the save/restore below arranges.
    bool constructor = node == c->new_func;
    bool outer_in_new_body = c->in_new_body;
    c->in_new_body = constructor;
    // [syntax.sub.mutable-self]: whether this body's own receiver seat is mutable.
    bool outer_receiver_mutable = c->receiver_mutable;
    c->receiver_mutable = func->v.func.mutable_self;
    ChkOwnership outer_self_owned = c->self_owned;
    c->self_owned = (ChkOwnership){LHAT_OWN_DEEP, 0};
    // [syntax.sub.fresh]: whether every exit of this body owes something new.
    bool outer_must_answer_fresh = c->must_answer_fresh;
    c->must_answer_fresh = func->v.func.answers_fresh;

    c->scope = &body;
    c->declared_result = declared;
    c->inferred_result = NULL;
    c->saw_self_call = false;
    c->recursive_return = false;
    c->valueless_return = false;
    c->this_type = func;
    // [syntax.sub.this]: the chain this^^ walks. Lives here on the C stack, exactly
    // as long as the body is being checked.
    struct ThisLink this_link = { func, c->this_link, node };
    c->this_link = &this_link;
    c->in_function = node->v.func.is_function;
    c->body_scope = &body;
    c->deferred++;
    // [syntax.sub.yieldable]: a nested p^{...} starts collecting its own Y/R from scratch, so
    // its yield^ sites never unify with the ones out here.
    c->coroutine_produce = NULL;
    c->coroutine_receive = NULL;
    c->yield_context = YIELD_CTX_NONE;
    c->yield_bound_type = NULL;

    // [lex.scope]: the body's '{' is the scope its parameters are already in
    // (made just above), so the statements go straight into it rather than
    // opening a second one a '$^' would have to count.
    if (node->v.func.body != NULL &&
        node->v.func.body->kind == LHAT_NODE_BLOCK) {
        chk_check_block_in_scope(c, node->v.func.body);
    } else {
        chk_check_statement(c, node->v.func.body);
    }

    // [pipeline.infer]: every demand this body makes is in. Settled before the result
    // is put together, since a return^ of a parameter carries this very object
    // into it.
    chk_settle_param_vars(c, param_mark);

    if (node->v.func.yields) {
        func->v.func.yield_produce = c->coroutine_produce;
        func->v.func.yield_receive = c->coroutine_receive;
    }

    // Reaching the end of the body is an exit that produces no value, and a
    // bare return^ is the same exit written down. Both hand nil^ back at run
    // time, so [pipeline.infer] counts them together. What that means depends on
    // what the subroutine promised.
    bool falls_through = !chk_always_exits(node->v.func.body);
    bool leaves_without_value = falls_through || c->valueless_return;
    func->v.func.ends_without_value = leaves_without_value;

    // [syntax.type.empty]: a function always has a result. So an f^ with a path that
    // answers nothing has one
    // with nothing to answer with, and no result type would make it right.
    //
    // [syntax.sub.yieldable-f] with [syntax.sub.no-contagion]: a yieldable one answers a coroutine, and it answers
    // one whatever the body goes on to do. Reaching the end of the body ends
    // the coroutine ([syntax.type.coroutine] puts that in the third type); it is not the
    // function failing to answer. So [syntax.type.empty] is already satisfied here.
    if (leaves_without_value && node->v.func.is_function &&
        !node->v.func.yields && !constructor) {
        // [syntax.obj.new]: a new body is exempt -- it never answers for itself, so
        // reaching its end is the ordinary way out.
        chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    } else if (leaves_without_value && declared != NULL &&
               !lhat_type_conforms(chk_simple(c, LHAT_TYPE_NIL), declared)) {
        // A p^ may leave without a value, but then its result has to admit
        // one. [errors.runtime.index] spells that nil^.
        chk_report(c, node, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    }

    if (declared == NULL) {
        // [pipeline.infer]: the result is what the exits that do not go through the
        // subroutine itself agree on. Leaving without a value is one of
        // those exits, and [errors.runtime.index] already spells "no value" nil^.
        //
        // [syntax.type.empty] keeps that apart from a body that returns nothing at
        // all: there the writer never asked for a value, and the signature
        // has a form for it. nil^ joins in only when some other exit does
        // produce one.
        if (leaves_without_value &&
            (c->inferred_result != NULL || c->recursive_return)) {
            // [syntax.type.tuple]: there is no union of a tuple with nil^ to fall into, so
            // every exit of a body answering several values has to carry
            // them. [syntax.type.empty] asks this of an f^ already; the width is what a
            // caller reserves slots by, so it reaches p^ too.
            if (lhat_type_tuple_width(c->inferred_result) > 0) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
            } else {
                c->inferred_result = lhat_type_union(c->result->types,
                                                     c->inferred_result,
                                                     chk_simple(c, LHAT_TYPE_NIL));
            }
        }

        // Every way out goes through the subroutine itself, so no call of it
        // ever produces a value. [syntax.with.errors] and [pipeline.bc.no-unwind] leave no other way
        // out -- no exceptions, no unwinding -- so this is decidable here.
        if (c->recursive_return && c->inferred_result == NULL) {
            chk_report(c, node, LHAT_CHECK_ERR_NEVER_RETURNS);
        }
        func->v.func.result = c->inferred_result;

        // [pipeline.strict.per-unit]: what the body did not decide is reported here rather
        // than left in the signature. A result type is a promise to every
        // caller, so a gap in one hands the gap out at every call -- and
        // where there is no call yet, nobody would ever have asked. The
        // parameters are not read this way (a gap there is a constraint
        // nobody wrote, which is what any^ already means), and relaxed
        // forgives all of it ([pipeline.strict.relaxed]).
        //
        // A result [pipeline.infer.expected]'s expectation gave is written rather than inferred,
        // so this branch is not where it lands at all.
        if (lhat_type_has_gap(func->v.func.result)) {
            chk_report(c, node, LHAT_CHECK_ERR_RESULT_UNDECIDED);
        }
    }

    // [syntax.sub.no-contagion]: what a call to this answers, settled here because this is where
    // the three types [syntax.type.coroutine] needs are all known. Everything that asks what a
    // call is worth reads it from the signature rather than assembling its
    // own -- the writers and conformance among them, which is what lets the
    // written-out form ('p^number^ -> c^{ p^nil^ -> number^ -> nil^ };') be
    // read back as the annotation it looks like ([modules.host.import]).
    if (node->v.func.yields) {
        func->v.func.answers = coroutine_made_by(c, func);
        // [syntax.type.coroutine] with [syntax.sub.yieldable]: R and Y are read off the yield^ sites. Where the
        // writer put them down as well, the two have to be the same
        // coroutine -- a signature saying something else is not a signature,
        // and one of the two has to move. A signature written on the binding
        // meets the same question at the assignment, which conforms_func
        // asks through lhat_type_call_answer; one written here has no
        // assignment to meet, so it is asked here.
        if (written_coroutine != NULL &&
            !lhat_type_conforms(func->v.func.answers, written_coroutine)) {
            chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
        }
    }

    // [syntax.obj.new]: whatever the body does, the member answers the copy the machine
    // made -- so the signature says so, whichever branch above wrote it.
    if (constructor) {
        func->v.func.result = c->self_link != NULL ? c->self_link->type : NULL;
        func->v.func.ends_without_value = false;
    }

    // [syntax.sub.yieldable-f]: an f^ coroutine may not leave the body that made it. Reaching
    // the outside is what would make advancing it observable from there, and
    // it reaches out through a table member or a nested signature as readily
    // as through the result itself -- so the whole result type is read rather
    // than its outermost shape. A p^ coroutine needs nothing here: advancing
    // one is a p^ call, which [syntax.sub.kinds] already refuses inside an f^.
    if (node->v.func.is_function &&
        chk_mentions_function_coroutine(func->v.func.result, 0)) {
        chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
    }

    // [errors.kind.local-no-propagate], and the same shape of rule as the one above: a localerror^
    // may not be what a caller receives. Reading the whole settled signature
    // rather than its outermost shape catches it inside a table or a nested
    // one, and reaches a coroutine's Y as well as its T (chk_type_touches_local
    // asks `answers` first).
    //
    // A body that tried to let one past through try^ was already refused at
    // the try^ itself, and chk_error_leaves keeps it out of what it infers --
    // so what this finds is a result somebody WROTE.
    if (chk_type_touches_local(func, 0)) {
        chk_report(c, node, LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN);
    }

    c->deferred--;
    c->in_new_body = outer_in_new_body;
    c->receiver_mutable = outer_receiver_mutable;
    c->self_owned = outer_self_owned;
    c->must_answer_fresh = outer_must_answer_fresh;
    c->scope = outer_scope;
    c->declared_result = outer_declared;
    c->inferred_result = outer_inferred;
    c->saw_self_call = outer_self_call;
    c->recursive_return = outer_recursive;
    c->valueless_return = outer_valueless;
    c->this_type = outer_this;
    c->this_link = this_link.outer;
    c->coroutine_produce = outer_coroutine_produce;
    c->coroutine_receive = outer_coroutine_receive;
    c->yield_context = outer_yield_context;
    c->yield_bound_type = outer_yield_bound_type;
    c->in_function = outer_in_function;
    c->body_scope = outer_body_scope;
    c->catch_frame = outer_catch_frame;

    chk_scope_close(c, &body);
    // The compiler reads this back instead of re-deriving the signature from
    // written annotations alone, so a result left to inference (no return^
    // type written) still reaches typeof^ and overload dispatch precisely.
    ((LhatNode *)node)->checked_type = func;
    return func;
}

// [syntax.obj]. A definition produces two structures, and [syntax.obj.instance] is what ties them:
// an instance can reach the definition's members, so its type contains them
// as well as the fields the template declares.
//
//   definition : the members, plus a new if none was written ([syntax.obj.new])
//   instance   : those members, plus the template's fields
//
// [syntax.obj.name] keeps the name out of it. Both are ordinary structures, so [syntax.op.semantic]'s
// structural identity applies unchanged and nothing here has to be interned.
// The one walk of a member list, under both of the searches below.
const LhatTypeMember *chk_members_search(const LhatTypeMember *members,
                                         const char *name, size_t length)
{
    for (; members != NULL; members = members->next) {
        if (members->name_length == length &&
            memcmp(members->name, name, length) == 0) {
            return members;
        }
    }
    return NULL;
}

const LhatTypeMember *chk_find_member(const LhatType *table,
                                      const char *name, size_t length)
{
    // [modules.hostvalue]: a host value type keeps its registered members in the same
    // list a table keeps its own, so the one search serves both -- and the
    // links it may hang off are type.c's to follow, since the machine and
    // the relations read them through the same walk.
    return lhat_type_find_member(table, name, length);
}

// [syntax.obj.override]'s overload^ puts several signatures under one name, and [syntax.obj.new]'s new
// is a name like any other -- so what a definition holds there is one
// signature or an intersection of them. Both readings below take that as
// given: every arm answers with the same instance, since every arm is a way
// of constructing this definition.
static const LhatType *constructor_arm(const LhatType *held)
{
    if (held == NULL) {
        return NULL;
    }
    if (held->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = held->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC) {
                return arm->type;
            }
        }
        return NULL;
    }
    return held->kind == LHAT_TYPE_FUNC ? held : NULL;
}

// [syntax.obj.new] makes new return an instance, so a definition's own structure is
// where its instance type can be found again. Composition needs it, to carry
// the base's fields into the derived instance.
LhatType *chk_instance_of(const LhatType *definition)
{
    // [modules.hostdata]: a registered host type is its own written type -- it has
    // no instance half, and a member the host happened to call "new"
    // (std.math.Vector3.new, say) is an ordinary constructor function, not
    // [syntax.obj.new]'s mechanism. Without this, writing the type's name answered
    // with that member's result instead of the type.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.nominal) {
        return NULL;
    }
    // [syntax.obj.members-order]: a def^ says outright what its instances carry. Read before new
    // is asked, since the two must not drift -- what new answers is built
    // from this very structure.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.instance != NULL) {
        return definition->v.table.instance;
    }
    // [syntax.obj.new] is a def^'s mechanism and no one else's. A module -- hosted, or
    // what require^ answers -- may carry an ordinary function under the
    // name (std.regex.new, say), and reading its result as "the instance
    // type" here made the module's own name unresolvable as a path.
    if (definition == NULL || definition->kind != LHAT_TYPE_TABLE ||
        !definition->v.table.is_definition) {
        return NULL;
    }
    const LhatTypeMember *constructor = chk_find_member(definition, "new", 3);
    const LhatType *arm =
        constructor != NULL ? constructor_arm(constructor->type) : NULL;
    return arm != NULL ? arm->v.func.result : NULL;
}

// [syntax.obj.new] with [syntax.obj.compose]: the new a definition gets before anything is written into
// it. The parameters come from the base -- a derived definition is built the
// way the base was -- but the result has to be the derived instance, since
// [syntax.obj.compose] composes to make something new and a constructor answering with the
// base would defeat that. With no base, or a base that carries no signature
// here, it is the one [syntax.obj.new] gives every definition: no arguments, and the
// template fixes every field.
//
// An overloaded new is rebuilt arm by arm, since each arm is one way of
// constructing the same thing.
static LhatType *constructor_from(Checker *c, const LhatType *inherited,
                                  LhatType *instance)
{
    if (inherited != NULL && inherited->kind == LHAT_TYPE_INTERSECT) {
        LhatType *rebuilt = NULL;
        for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC) {
                continue;
            }
            LhatType *one = constructor_from(c, arm->type, instance);
            rebuilt = rebuilt == NULL
                          ? one
                          : lhat_type_intersect(c->result->types, rebuilt, one);
        }
        if (rebuilt != NULL) {
            return rebuilt;
        }
    }

    LhatType *constructor = lhat_type_func(c->result->types, true);
    if (inherited != NULL && inherited->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = inherited->v.func.params; p != NULL;
             p = p->next) {
            lhat_type_add_param(c->result->types, constructor, p->type);
        }
        constructor->v.func.variadic = inherited->v.func.variadic;
    }
    constructor->v.func.result = instance;
    return constructor;
}

// The same, reading the base's new off the base itself.
static LhatType *constructor_for(Checker *c, const LhatType *base,
                                 LhatType *instance)
{
    const LhatTypeMember *inherited = chk_find_member(base, "new", 3);
    return constructor_from(c, inherited != NULL ? inherited->type : NULL,
                            instance);
}

// [syntax.obj.new]: the leaves a template initialiser may leave on the prototype
// by type alone. Construction copies the prototype, so a value anything
// could write through -- a coroutine, a host object -- would be one place
// shared by all of them. Immutable values pass: a subroutine's captured
// places are its own affair, the way a definition's shared members already
// are, and an error kind is an identity. So does a definition -- a public
// identity every instance already shares by name, which a field adds no
// sharing to. any^ could smuggle a mutable one past this, so it is refused;
// what inference left open is let through, since the machine checks the
// values themselves (SETPROTO). A plain table is not a leaf -- what admits
// one is the literal-tree reading below.
static bool immutable_default_type(const LhatType *type)
{
    if (type == NULL) {
        return true;
    }
    switch (type->kind) {
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
        case LHAT_TYPE_NIL:
        case LHAT_TYPE_BOOL:
        case LHAT_TYPE_NUMBER:
        case LHAT_TYPE_STRING:
        case LHAT_TYPE_FUNC:
        case LHAT_TYPE_ERROR_KIND:
            return true;
        case LHAT_TYPE_TABLE:
            return type->v.table.is_definition;
        // [modules.hostvalue]: a box is a copyable node -- bake and construction copy
        // its bytes, so a default shares nothing.
        case LHAT_TYPE_HOSTVALUE_BOX:
            return true;
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (!immutable_default_type(arm->type)) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

// [syntax.obj.new]: whether this initialiser may sit on the prototype. A table
// only as a literal tree -- table literals nested in table literals, each
// level born fresh at this very expression, so the copies construction hands
// out share nothing and cycle nowhere, and the whole of what every instance
// starts with is right there in the source. A table that arrived any other
// way -- a name, a call's answer -- is an identity from somewhere else,
// which is what new is for. The entry types are read off the literal's own
// inferred type, walked in parallel with the tree.
static bool immutable_default_value(Checker *c, const LhatNode *node,
                                    const LhatType *type)
{
    if (node != NULL && (node->kind == LHAT_NODE_TABLE ||
        (node->kind == LHAT_NODE_NEW && type != NULL &&
         type->kind == LHAT_TYPE_TABLE && !type->v.table.from_definition))) {
        const LhatNode *items = node->kind == LHAT_NODE_NEW
                                    ? node->v.named.members : node->v.list.items;
        size_t position = 0;
        for (const LhatNode *entry = items; entry != NULL;
             entry = entry->next) {
            // A computed key is a value of its own; the names and numbers a
            // literal writes out are the keys a default takes.
            if (entry->v.entry.computed) {
                return false;
            }
            const LhatTypeMember *member = NULL;
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key != NULL) {
                if (chk_node_name(c, entry->v.entry.key, &name, &length)) {
                    member = chk_find_member(type, name, length);
                }
            } else {
                member = lhat_type_member_at(type, position++);
            }
            if (!immutable_default_value(c, entry->v.entry.value,
                                         member != NULL ? member->type
                                                        : NULL)) {
                return false;
            }
        }
        return true;
    }
    return immutable_default_type(type);
}

// [syntax.obj.super] with [syntax.obj.new]: what super^ names inside a written new -- the
// hook of the new standing before it, run against the same receiver. It
// takes what that new took and answers nothing: the instance is already in
// the caller's hands, so there is nothing for the chain to pass back.
static LhatType *hook_signature(Checker *c, const LhatType *from)
{
    if (from != NULL && from->kind == LHAT_TYPE_INTERSECT) {
        LhatType *rebuilt = NULL;
        for (const LhatTypeList *arm = from->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC) {
                continue;
            }
            LhatType *one = hook_signature(c, arm->type);
            rebuilt = rebuilt == NULL
                          ? one
                          : lhat_type_intersect(c->result->types, rebuilt, one);
        }
        if (rebuilt != NULL) {
            return rebuilt;
        }
    }
    LhatType *hook = lhat_type_func(c->result->types, true);
    if (from != NULL && from->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = from->v.func.params; p != NULL;
             p = p->next) {
            lhat_type_add_param(c->result->types, hook, p->type);
        }
        hook->v.func.variadic = from->v.func.variadic;
    }
    return hook;
}

// Overwrites rather than appending, so a member the base already had ends up
// replaced by [syntax.obj.override]'s override^ instead of shadowed by position.
//
// [syntax.obj.abstract]: `abstract` travels with the type, so writing over a declaration with
// a real one is what fills the hole -- and writing a declaration over a
// declaration leaves it open.
// Answers the member it settled -- the one already there, or the one it
// added -- so that a caller holding the key it was written under can say
// where that key stands ([lsp.hover], chk_member_declared_at).
static LhatTypeMember *set_member_marked(Checker *c, LhatType *table,
                                         const char *name, size_t length,
                                         LhatType *type, bool abstract,
                                         bool pending)
{
    // The table's own, and no link followed: [modules.hostdata.base]'s base and [syntax.obj.delegate]'s
    // delegate lend a name without giving it, and writing through either
    // would put a member on somebody else's type.
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        m->type = type;
        m->abstract = abstract;
        m->pending = pending;
        // [syntax.obj.members-order]: whatever the first pass put here, this is the real one.
        m->provisional = false;
        return m;
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->abstract = abstract;
        added->pending = pending;
    }
    return added;
}

// [syntax.obj.members-order]: the first pass's seed. Puts the name there with the signature
// it was written with, so a body checked before it can reach it -- and leaves
// alone anything already there, which is the base's or an earlier entry's and
// is the real thing rather than a seed.
static LhatTypeMember *set_member_provisional(Checker *c, LhatType *table,
                                              const char *name, size_t length,
                                              LhatType *type, bool abstract)
{
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        return m;
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->provisional = true;
        added->abstract = abstract;
    }
    return added;
}

// [syntax.obj.delegate]: the next walk's seed. What the last walk inferred stays --
// that is what it learned -- and the mark says it is still not the answer, so
// a body reading it is known to have read ahead again, and [syntax.obj.override]'s "a second
// member of this name wants a marker" does not fire on this def^'s own
// entries the way it would against a member that is really there.
static void mark_provisional(LhatType *table, const char *name, size_t length)
{
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        m->provisional = true;
    }
}

static LhatTypeMember *set_member_as(Checker *c, LhatType *table,
                                     const char *name, size_t length,
                                     LhatType *type, bool abstract)
{
    return set_member_marked(c, table, name, length, type, abstract, false);
}

static LhatTypeMember *set_member(Checker *c, LhatType *table,
                                  const char *name, size_t length,
                                  LhatType *type)
{
    return set_member_marked(c, table, name, length, type, false, false);
}

// [syntax.obj.abstract]: whether the definition is still waiting on a composition. [syntax.obj.new]'s
// new is what this stands in the way of -- an abstract^ would leave a name
// with nothing under it, and [syntax.obj.abstract-override]'s pending override^ would leave a super^
// pointing at nothing.
//
// `field` answers which of the two lists the hole is in, since [syntax.obj.abstract-new] gives
// the two different ways out: a member is a composition's to provide, and a
// field is that or a written new's to write.
const LhatTypeMember *chk_hole_of(const LhatType *definition, bool *field)
{
    if (field != NULL) {
        *field = false;
    }
    if (definition == NULL || definition->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    for (const LhatTypeMember *m = definition->v.table.members; m != NULL;
         m = m->next) {
        if (m->abstract || m->pending) {
            return m;
        }
    }
    const LhatType *instance = chk_instance_of(definition);
    if (instance == NULL || instance->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    for (const LhatTypeMember *m = instance->v.table.members; m != NULL;
         m = m->next) {
        if (m->abstract || m->pending) {
            if (field != NULL) {
                *field = !m->pending;  // a wait is [syntax.obj.abstract-override]'s, not a field's
            }
            return m;
        }
    }
    return NULL;
}

const LhatTypeMember *chk_unimplemented_member(const LhatType *definition)
{
    return chk_hole_of(definition, NULL);
}

// Composition copies availability as well as the signature. In particular,
// extending a definition cannot make an ambiguous name callable again.
static LhatTypeMember *copy_member(Checker *c, LhatType *into,
                                   const LhatTypeMember *from)
{
    LhatTypeMember *copy = set_member_marked(
        c, into, from->name, from->name_length, from->type,
        from->abstract, from->pending);
    if (copy != NULL) {
        copy->ambiguous = from->ambiguous;
        copy->demanded = from->demanded;
        copy->instance_field = from->instance_field;
    }
    return copy;
}

static void copy_members(Checker *c, LhatType *into, const LhatType *from)
{
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = from->v.table.members; m != NULL;
         m = m->next) {
        copy_member(c, into, m);
    }
}

// [syntax.obj.delegate]: does this member take a receiver? That is [syntax.obj.instance]'s whole rule for
// what an instance sees, and delegation does not widen it -- a static member
// of the delegate is the delegate's, the way a static member of a definition
// is the definition's.
//
// [syntax.obj.override]'s groups answer yes when any arm does: the call picks an arm, and one
// that takes a receiver is reachable through the dot.
static bool member_takes_receiver(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_FUNC) {
        return type->v.func.takes_self;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (member_takes_receiver(arm->type)) {
                return true;
            }
        }
    }
    return false;
}

// What this def^ delegates to, as it was written. NULL when it declares
// none. [syntax.obj.methods]'s two spellings: 'self^.x' reads the receiver's field, a bare
// 'x' the definition's member, and `through_self` is which was written --
// they are not one form with an optional prefix, since [syntax.obj.methods] declares fields
// and members apart and a name may be both. The same split decides where
// the answer is looked for at run time.
static const LhatNode *delegate_of(const LhatNode *node, bool *through_self)
{
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.modifier != LHAT_DEF_DELEGATE ||
            entry->v.entry.value == NULL) {
            continue;
        }
        if (through_self != NULL) {
            *through_self = entry->v.entry.value->kind == LHAT_NODE_MEMBER;
        }
        return entry->v.entry.value;  // the parser allows one
    }
    return NULL;
}

// [syntax.obj.delegate]: hangs what this definition delegates to off the section its
// instances carry, as a link. Nothing is copied: chk_find_member follows it
// where a name is not the wrapper's own, which is [syntax.obj.instance]'s order, and the
// machine answers the same name by growing a leg on the same walk
// (object.c). One shape, asked twice.
//
// Answers whether it settled. A delegate naming a member whose type only its
// own body knows cannot settle before that body is walked, so this is asked
// once per round of the walk ([pipeline.infer.rounds]) and reports only on the last --
// `report` is what tells the two apart, and a walk that did not settle says
// so through read_provisional, which is what buys the next round.
static bool link_delegate(Checker *c, const LhatNode *node,
                          LhatType *definition, LhatType *instance,
                          bool report)
{
    bool through_self = false;
    const LhatNode *named = delegate_of(node, &through_self);
    if (named == NULL || instance == NULL) {
        return true;  // nothing to settle
    }

    const LhatNode *spelt = through_self ? named->v.access.argument : named;
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, spelt, &name, &length)) {
        return true;  // not a name; the walk of it says what is wrong
    }
    // Looked for where the spelling said, and nowhere else. Finding it in the
    // other place would be answering a question that was not asked.
    const LhatTypeMember *held =
        lhat_type_own_member(through_self ? instance : definition, name, length);
    LhatType *from = held != NULL ? held->type : NULL;
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        // [syntax.obj.def-form] gives a member no way to carry a written type, so a static
        // one holding what a call answered is known only once that entry has
        // been walked. Saying it was read ahead is what asks for the round
        // that walks it; on the last round `report` is set and this is the
        // answer.
        if (!report) {
            c->read_provisional = true;
            return false;
        }
        if (held == NULL) {
            chk_report_named(c, spelt, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        } else {
            chk_report(c, spelt, LHAT_CHECK_ERR_NO_MEMBER);
        }
        return true;
    }
    // [syntax.obj.instance]: an instance sees what the delegate's own instances would see. A
    // host type ([modules.hostdata]) has no instance section of its own -- its members
    // ARE what one reaches -- so the type itself is what is linked, and its
    // base link carries the rest of the chain.
#if LHAT_WITH_RESOLUTIONS
    // [lsp.hover]: what the spelling named is a member, found here and nowhere
    // else -- [syntax.obj.delegate]'s target is not walked as an expression, so nothing
    // else records it and a reader standing on it was told nothing. What was
    // found is the member itself, so where it was written comes with it.
    chk_record_member_resolution(c, spelt, from, held, false);
#endif
    instance->v.table.delegate =
        from->v.table.instance != NULL ? from->v.table.instance : from;
    return true;
}

// [syntax.obj.abstract-new]: which fields a written new gives a value to.
//
// [syntax.obj.new] builds an instance with `self^{ … }`, and [syntax.obj.abstract]'s declaration on a
// template field is a field with no initialiser -- so a new that writes one is
// what an instance of that definition gets it from. Reading which names it
// writes is what tells the hole from the filled field, and the tree is where
// that is: the walk over the fields (the SELF_TABLE case) asks of each written
// field whether the template has it, which is the other direction.
//
// Every self^{ … } in the body has to write the name, since any of them may be
// the one that runs -- so several are intersected rather than unioned.
typedef struct {
    Checker *c;
    const char *names[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t lengths[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t count;
    bool seen_one;   // whether a first self^{ … } has been met to intersect with
    bool overflowed; // more written fields than there is room to track
} WrittenFields;

static void collect_written_fields(WrittenFields *w, const LhatNode *node);

static void written_fields_child(void *context, const char *field, bool in_list,
                                 const LhatNode *child)
{
    (void)field;
    (void)in_list;
    collect_written_fields((WrittenFields *)context, child);
}

// Keeps only the names this one also writes, which is the intersection two
// branches of a new agree on.
static void intersect_written(WrittenFields *w, const LhatNode *table)
{
    size_t kept = 0;
    for (size_t i = 0; i < w->count; i++) {
        bool here = false;
        for (const LhatNode *field = table->v.list.items;
             field != NULL && !here; field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (chk_node_name(w->c, field->v.entry.key, &name, &length)) {
                here = length == w->lengths[i] &&
                       memcmp(name, w->names[i], length) == 0;
            }
        }
        if (here) {
            w->names[kept] = w->names[i];
            w->lengths[kept] = w->lengths[i];
            kept++;
        }
    }
    w->count = kept;
}

static void collect_written_fields(WrittenFields *w, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    // A body written inside this one builds some other definition's instance,
    // and its self^{ … } says nothing about this one's fields.
    if (node->kind == LHAT_NODE_FUNC || node->kind == LHAT_NODE_DEF) {
        return;
    }
    if (node->kind == LHAT_NODE_SELF_TABLE) {
        if (w->seen_one) {
            intersect_written(w, node);
            return;
        }
        w->seen_one = true;
        for (const LhatNode *field = node->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(w->c, field->v.entry.key, &name, &length)) {
                continue;
            }
            if (w->count == LHAT_CHECK_MAX_TRACKED_ARGS) {
                w->overflowed = true;
                break;
            }
            w->names[w->count] = name;
            w->lengths[w->count] = length;
            w->count++;
        }
        return;
    }
    lhat_node_visit_children(node, written_fields_child, w);
}

// [lsp.next]: [syntax.obj.override]'s marker leads the member it applies to, and the parser starts
// the entry at it -- so the marker is the stretch between the entry's start
// and its key, the space after the word included. Empty length when no marker
// was written, which is how a fix knows there is nothing to take off.
static size_t marker_removal(const LhatNode *entry, LhatFixSlot *out)
{
    const LhatNode *key = entry->v.entry.key;
    if (key == NULL || key->offset <= entry->offset) {
        return 0;
    }
    *out = lhat_fix_slot(LHAT_FIX_REMOVE_MARKER, LHAT_FIX_SUGGESTED,
                         entry->offset, key->offset - entry->offset, "");
    return 1;
}

// And the other way: a name that is already a member takes one of the two
// markers, written in front of it. Which of them is the writer's to say --
// one replaces what is there, the other adds a way of calling it -- so both
// are offered and neither is machine-appliable. Fills LHAT_FIX_SLOTS slots.
static size_t marker_choices(const LhatNode *entry, LhatFixSlot *out)
{
    static const struct {
        LhatFixTitle title;
        const char *text;
    } WORDS[] = {
        {LHAT_FIX_WRITE_OVERRIDE, "override^ "},
        {LHAT_FIX_WRITE_OVERLOAD, "overload^ "},
    };
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        out[i] = lhat_fix_slot(WORDS[i].title, LHAT_FIX_SUGGESTED,
                               entry->offset, 0, WORDS[i].text);
    }
    return sizeof WORDS / sizeof WORDS[0];
}

// [syntax.obj.override]'s overlap test, applied to two signatures: is there an argument count
// admissible by both at which no position is separate? A separate position is
// enough to keep them apart, since a call has to satisfy every one.
//
// [syntax.type.param-names] keeps defaults out of a type, so a signature admits one count, or a
// range once it has a '...'. Checking the smallest count they share settles
// it: beyond that at least one side is variadic, and its element type does
// not change with position.
static bool signatures_overlap(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL || a->kind != LHAT_TYPE_FUNC ||
        b->kind != LHAT_TYPE_FUNC) {
        return true;  // nothing here can tell them apart
    }

    // [syntax.op.right]: which side the receiver stands on is a position too, and the
    // one a call fixes first. An operator written 'f^self^, rhs' answers a
    // use with its owner on the left and 'f^lhs, self^' one with it on the
    // right; [syntax.op.right]'s order asks the left operand first and only then the
    // right, so no use ever reaches both. The two are different types
    // (type.h) and operator_arm already picks between them by this alone.
    if (a->v.func.self_last != b->v.func.self_last) {
        return false;
    }

    size_t count_a = 0;
    size_t count_b = 0;
    for (const LhatTypeList *p = a->v.func.params; p != NULL; p = p->next) {
        count_a++;
    }
    for (const LhatTypeList *p = b->v.func.params; p != NULL; p = p->next) {
        count_b++;
    }

    size_t shared = count_a > count_b ? count_a : count_b;
    if ((shared > count_a && a->v.func.variadic == NULL) ||
        (shared > count_b && b->v.func.variadic == NULL)) {
        return false;
    }

    const LhatTypeList *pa = a->v.func.params;
    const LhatTypeList *pb = b->v.func.params;
    for (size_t i = 0; i < shared; i++) {
        LhatType *ta = pa != NULL ? pa->type : a->v.func.variadic;
        LhatType *tb = pb != NULL ? pb->type : b->v.func.variadic;
        if (lhat_type_disjoint(ta, tb)) {
            return false;
        }
        if (pa != NULL) {
            pa = pa->next;
        }
        if (pb != NULL) {
            pb = pb->next;
        }
    }
    return true;
}

// [syntax.obj.override]. A member of a name the base already uses is an error unless it says
// which of the two things it means, and each says something checkable.
// [syntax.obj.override]: an override^ replaces the one candidate its signature overlaps.
// What a name carries is a single signature or an intersection of them
// ([syntax.obj.super]), so this walks the arms, checks substitutability against the one
// that overlaps, and answers the intersection with that arm swapped out.
//
// Refuses two overlaps -- [syntax.obj.override] says which one was meant is then undecidable
// -- and none, since there was nothing at that name to replace.
static LhatType *override_one(Checker *c, const LhatNode *entry,
                              LhatType *inherited, LhatType *replacement)
{
    if (inherited == NULL || inherited->kind != LHAT_TYPE_INTERSECT) {
        // One candidate, so it is the one: ordinary conformance, arguments
        // wider and result narrower. The receiver is not in the parameter
        // list ([syntax.obj.self]), so [syntax.obj.override]'s exemption of self^ needs nothing of its own.
        if (!lhat_type_conforms(replacement, inherited)) {
            chk_report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        }
        return replacement;
    }

    LhatType *overlapped = NULL;
    size_t overlaps = 0;
    size_t position = 0;
    size_t at = 0;
    for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
         arm = arm->next, position++) {
        if (signatures_overlap(replacement, arm->type)) {
            overlapped = arm->type;
            at = position;
            overlaps++;
        }
    }

    if (overlaps == 0) {
        LhatFixSlot away;
        chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL, 0,
                       &away, marker_removal(entry, &away));
        return replacement;
    }
    if (overlaps > 1) {
        // [syntax.obj.override]: widening the arguments may reach two of them, and then
        // which is being replaced is not decidable. Reported as the overlap
        // it is, since that is what the writer has to take apart.
        chk_report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
        return replacement;
    }
    if (!lhat_type_conforms(replacement, overlapped)) {
        chk_report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        return replacement;
    }

    // [spec.arms]: which arm this is settles the shape of the group the run
    // builds, so the compiler is told rather than left to put the replacement
    // in front of a group that keeps the arm it replaced. Not a strict-only
    // matter: [pipeline.repl.no-fork] asks that what runs be the same either way, and the
    // arms a name carries are read by typeof^ ([syntax.obj.typeof]) whichever it is.
    if (at + 1 <= UINT16_MAX) {
        ((LhatNode *)entry)->checked_arm = (uint16_t)(at + 1);
    }

    // The others are untouched, so the name goes on carrying them.
    LhatType *rebuilt = NULL;
    for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
         arm = arm->next) {
        LhatType *part = arm->type == overlapped ? replacement : arm->type;
        rebuilt = rebuilt == NULL
                      ? part
                      : lhat_type_intersect(c->result->types, rebuilt, part);
    }
    return rebuilt != NULL ? rebuilt : replacement;
}

// `constructor` says this entry is [syntax.obj.new]'s new, which [syntax.obj.override-new] exempts from
// the substitutability an override^ otherwise owes. Nothing can be written
// that takes a definition where another definition is expected -- [modules.env.one]
// and [syntax.obj.instance] make a definition's name mean its instance -- so a new that
// narrows what it accepts leaves no caller behind. A method is the other way
// round: an instance does stand where the base's instance is written, and
// there the rule is holding something up. So an override^ new replaces what
// the name held, whole.
static LhatType *check_same_name(Checker *c, const LhatNode *entry,
                                 const LhatTypeMember *inherited,
                                 LhatType *replacement, bool constructor)
{
    LhatDefModifier modifier = entry->v.entry.modifier;

    if (inherited == NULL) {
        // [syntax.obj.abstract-override]: an override^ with nothing yet under the name is a mixin
        // written against a base it has not met. It is not an error -- it
        // says what the composition has to bring, and the member stays
        // pending until something does. [syntax.obj.new]'s new is what it stands in
        // the way of; overload^ has no such reading, since adding a way to
        // call something that is not there says nothing.
        if (modifier == LHAT_DEF_OVERLOAD) {
            LhatFixSlot away;
            chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL,
                           0, &away, marker_removal(entry, &away));
        }
        return replacement;
    }

    if (inherited->ambiguous) {
        chk_report(c, entry, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER);
        return replacement;
    }

    // [syntax.obj.abstract]: a declaration is not a definition of the member, so filling it
    // in is not the collision [syntax.obj.override] is about -- no marker is wanted. What the
    // declaration does ask is that the value fit the type it wrote.
    if (inherited->abstract) {
        if (modifier != LHAT_DEF_PLAIN) {
            LhatFixSlot away;
            chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL,
                           0, &away, marker_removal(entry, &away));
        } else if (!lhat_type_conforms(replacement, inherited->type)) {
            chk_report(c, entry, LHAT_CHECK_ERR_MISMATCH);
        }
        return replacement;
    }

    switch (modifier) {
        case LHAT_DEF_PLAIN: {
            LhatFixSlot markers[LHAT_FIX_SLOTS];
            chk_report_fix(c, entry, LHAT_CHECK_ERR_MEMBER_EXISTS, NULL, 0,
                           markers, marker_choices(entry, markers));
            return replacement;
        }

        case LHAT_DEF_OVERRIDE:
            // [syntax.obj.override-new]: the exemption above. The whole member goes, so there
            // is no arm to name and nothing to compare against.
            if (constructor) {
                return replacement;
            }
            // [syntax.obj.override]: what is replaced is the one existing candidate the new
            // signature overlaps. A name that was overloaded carries several,
            // and comparing against all of them at once would refuse every
            // replacement -- no one signature is usable where an intersection
            // of them was.
            return override_one(c, entry, inherited->type, replacement);

        case LHAT_DEF_OVERLOAD: {
            // [syntax.obj.override]: the ban is on one signature overlapping another, so a
            // name already overloaded is asked arm by arm -- the same reason
            // override^ above walks them. An intersection is not a signature,
            // and signatures_overlap answers "cannot tell them apart" for
            // anything that is not one, so asking it as a whole would refuse
            // every arm past the second.
            bool overlaps = false;
            if (inherited->type != NULL &&
                inherited->type->kind == LHAT_TYPE_INTERSECT) {
                for (const LhatTypeList *arm = inherited->type->v.composite.arms;
                     arm != NULL && !overlaps; arm = arm->next) {
                    overlaps = signatures_overlap(replacement, arm->type);
                }
            } else {
                overlaps = signatures_overlap(replacement, inherited->type);
            }
            if (overlaps) {
                chk_report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
                return replacement;
            }
            // [syntax.obj.override]: an overloaded member is callable both ways, which is
            // what '&' means ([syntax.obj.compose]).
            return lhat_type_intersect(c->result->types, inherited->type,
                                       replacement);
        }

        // [syntax.obj.abstract]'s declaration and [syntax.obj.delegate]'s delegate^ define no member, so
        // [syntax.obj.override] has nothing to ask of either.
        case LHAT_DEF_ABSTRACT:
        case LHAT_DEF_DELEGATE:
            break;
    }
    return replacement;
}

// Merge one name from an independently written definition. A field has no
// qualified access to fall back to; a shared member does, so only the field
// collision rejects the composition itself.
static void compose_member(Checker *c, const LhatNode *node, LhatType *into,
                           const LhatTypeMember *right, bool field)
{
    LhatTypeMember *left = (LhatTypeMember *)lhat_type_own_member(
        into, right->name, right->name_length);
    if (left == NULL) {
        copy_member(c, into, right);
        return;
    }
    if (left->instance_field != right->instance_field) {
        chk_report_named(c, node, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                         right->name, right->name_length);
        return;
    }
    if (left->ambiguous || right->ambiguous) {
        // Neither an abstract requirement nor a pending override can choose
        // a provider from an ambiguous name. Keep any unmet requirement open.
        if (right->abstract) {
            left->type = left->abstract
                ? lhat_type_intersect(c->result->types, left->type, right->type)
                : right->type;
        }
        // A requirement a side had already met is open again: what met it is
        // one of the two answers that reach no one.
        left->abstract = left->abstract || right->abstract ||
                         left->demanded || right->demanded;
        left->demanded = left->demanded || right->demanded;
        left->pending = left->pending || right->pending;
        left->ambiguous = true;
        return;
    }
    if (left->abstract && right->abstract) {
        // Both requirements survive, independent of the order of the parts.
        left->type = lhat_type_intersect(c->result->types, left->type,
                                        right->type);
        return;
    }
    if (left->abstract || right->abstract) {
        const LhatTypeMember *need = left->abstract ? left : right;
        const LhatTypeMember *give = left->abstract ? right : left;
        if (!lhat_type_conforms(give->type, need->type)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_MISMATCH,
                             right->name, right->name_length);
        }
        LhatTypeMember *met = copy_member(c, into, give);
        if (met != NULL) {
            met->demanded = true;
        }
        return;
    }
    if (right->pending) {
        if (!lhat_type_conforms(right->type, left->type)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,
                             right->name, right->name_length);
        }
        bool pending = left->pending;
        bool demanded = left->demanded || right->demanded;
        LhatTypeMember *written = copy_member(c, into, right);
        if (written != NULL) {
            written->pending = pending;
            written->demanded = demanded;
        }
    } else if (field) {
        chk_report_named(c, node, LHAT_CHECK_ERR_COMPOSE_COLLIDES,
                         right->name, right->name_length);
    } else {
        // [syntax.obj.compose-ambiguous]: carried by both sides, so it reaches no one answer. Marked
        // rather than dropped, so an access can say what went wrong rather
        // than that the name is not there -- and a requirement either side
        // had met is open again, as above.
        left->ambiguous = true;
        if (left->demanded || right->demanded) {
            left->abstract = true;
            left->demanded = true;
        }
    }
}

// [syntax.obj.compose]: a name on the right brings its type and any pending requirements.
// Unlike a literal, it was not written against this left side. The compiler
// flattens the same parts, so the type must preserve every unresolved name.
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right)
{
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
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    definition->v.table.is_definition = true;
    definition->v.table.instance = instance;  // [syntax.obj.instance]
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    copy_members(c, definition, left);
    copy_members(c, instance, chk_instance_of(left));
    // [syntax.obj.delegate]: and what the parts delegate to, which is a link and so carries
    // over whole. The last part to declare one wins, the way a member written
    // later does -- which is how the compiler reads a composition (it walks
    // every part of the chain for a delegate^), and how the literal form of
    // '..' below already carried its base's. This form carried none, so a
    // member the machine lends through the composition was one the checker
    // called missing.
    const LhatType *left_instance = chk_instance_of(left);
    const LhatType *lent_right = chk_instance_of(right);
    if (lent_right != NULL && lent_right->kind == LHAT_TYPE_TABLE &&
        lent_right->v.table.delegate != NULL) {
        instance->v.table.delegate = lent_right->v.table.delegate;
    } else if (left_instance != NULL &&
               left_instance->kind == LHAT_TYPE_TABLE) {
        instance->v.table.delegate = left_instance->v.table.delegate;
    }
    for (const LhatTypeMember *m = right->v.table.members; m != NULL;
         m = m->next) {
        // Every definition has new; it is rebuilt for the composed instance.
        if (!chk_name_is(m->name, m->name_length, "new")) {
            compose_member(c, node, definition, m, false);
        }
    }
    const LhatType *right_instance = chk_instance_of(right);
    if (right_instance != NULL) {
        for (const LhatTypeMember *m = right_instance->v.table.members;
             m != NULL; m = m->next) {
            const LhatTypeMember *shared =
                lhat_type_own_member(definition, m->name, m->name_length);
            const LhatTypeMember *held =
                lhat_type_own_member(instance, m->name, m->name_length);
            // A method is in both tables. Mirror the result rather than
            // checking it twice and reporting the same mismatch twice -- but
            // only over the method's own mirror. A name the left instance
            // holds that is not a method is a field, declared or given, and
            // only the instance holds a field: the definition's side never
            // met it, so nothing has asked whether a method may stand there.
            if (shared != NULL && !m->instance_field &&
                member_takes_receiver(m->type) &&
                (held == NULL || !held->instance_field)) {
                copy_member(c, instance, shared);
            } else {
                compose_member(c, node, instance, m, true);
            }
        }
    }

    definition->v.table.new_overridden = left->v.table.new_overridden ||
                                         right->v.table.new_overridden;
    instance->v.table.new_overridden = definition->v.table.new_overridden;

    // [syntax.obj.compose] composes to make something new, so the constructor has to build
    // the composed instance. The parameters come from the right, which is the
    // more derived of the two -- every way of constructing it ([syntax.obj.override]'s
    // overload^ may have put several there) rebuilt to answer with what this
    // composition makes.
    const LhatTypeMember *inherited = chk_find_member(right, "new", 3);
    if (inherited == NULL) {
        inherited = chk_find_member(left, "new", 3);
    }
    set_member(c, definition, "new", 3,
               constructor_from(c, inherited != NULL ? inherited->type : NULL,
                                instance));
    return definition;
}

// [syntax.obj.abstract-same]: whether this def^ also writes a value under the name, which
// is what makes a declaration of it pointless. Read off the entries rather
// than kept as a mark, since the question is asked only where an abstract^
// stands and the list is a handful of members long.
static bool defined_in_def(Checker *c, const LhatNode *node, const char *name,
                           size_t length)
{
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key == NULL || entry->v.entry.declared) {
            continue;
        }
        const char *other = NULL;
        size_t other_length = 0;
        if (!chk_node_name(c, entry->v.entry.key, &other, &other_length)) {
            continue;
        }
        if (other_length == length && memcmp(other, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// [syntax.obj.self]: whether this member is a method -- one that wrote self^ among
// its parameters, and so is handed a receiver. A member without it is
// static, and the name self^ means nothing inside one.
static bool declares_self(Checker *c, const LhatNode *value)
{
    if (value == NULL || value->kind != LHAT_NODE_FUNC) {
        return false;
    }
    for (const LhatNode *param = value->v.func.params; param != NULL;
         param = param->next) {
        if (chk_self_marker_at(c, value->v.func.params, param) != 0) {
            return true;
        }
    }
    return false;
}

// [syntax.obj.members-order] with [syntax.obj.self]: whether an instance carries this member. What may be
// reached through one is what is handed a receiver, and a signature says so --
// [syntax.op.right]'s operator form writes self^ second, which takes_self covers either
// way. Everything else (new, a static member, a value) is the definition's.
//
// [syntax.obj.override]'s overloaded name is carried when any arm takes a receiver: which arm
// a call means is settled at the call, and an arm that takes none is not
// reachable through an instance whatever the type says here.
bool chk_takes_receiver(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (chk_takes_receiver(arm->type)) {
                return true;
            }
        }
        return false;
    }
    return type->kind == LHAT_TYPE_FUNC && type->v.func.takes_self;
}

// [syntax.obj.delegate]: what one entry of a def^ left behind, kept from one walk of
// the entries to the next.
//
// `found` and `member` are what the first walk found under the entry's own
// name. A later walk asks [syntax.obj.override] the same question, but by then the answer has
// been written over -- by the walk itself, and by the re-seeding that hands
// the next walk what the last one learned -- so it is kept here rather than
// looked up again. A copy: the member it was taken from is the one being
// written over.
//
// `inferred` is the type the last walk gave the entry, which is how
// [pipeline.infer.rounds] tells a walk that learned something from one that only said the same
// again.
typedef struct {
    bool found;
    LhatTypeMember member;
    LhatType *inferred;
} SeenMember;

LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base)
{
    LhatDefinition *origin = lhat_definition_new(c->result->types, c->result->module_name);
    if (origin != NULL) {
        origin->literal = node;
        origin->lexer = c->lexer;
        origin->statements = c->unit != NULL ? c->unit->v.list.items : NULL;
    }
    ((LhatNode *)node)->checked_definition = origin;
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    // [syntax.obj.compose]: '..' between two definitions is composition, never a call of an
    // op^.. one of them carries -- this is what tells a definition from an
    // instance of it.
    definition->v.table.is_definition = true;
    // [syntax.obj.members-order]: and this is what its instances carry. Held on the definition so
    // that [syntax.type.self]'s Self^ and the writer's self^{ … } section both read one
    // place, rather than going through what new answers.
    definition->v.table.instance = instance;
    // [syntax.stmt.member]: and both sides are closed to a member being added afterwards.
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    // [syntax.obj.compose]: composition is ordered, and the derived side is written against
    // what the base already provides.
    definition->v.table.new_overridden = base != NULL && base->v.table.new_overridden;
    instance->v.table.new_overridden = definition->v.table.new_overridden;
    copy_members(c, definition, base);
    copy_members(c, instance, chk_instance_of(base));
    // [syntax.obj.delegate]: and what the base delegates to, which is a link and so
    // carries over whole. A part declaring its own writes over it below
    // -- that is the "last part wins" rule, and with one link to hold it
    // there is nothing of an earlier part's to undo.
    LhatType *base_instance = chk_instance_of(base);
    if (base_instance != NULL) {
        instance->v.table.delegate = base_instance->v.table.delegate;
    }

    // [syntax.obj.new-default]: the new every definition has is put here, before anything is
    // written -- so a written one is a second member of that name and [syntax.obj.override]
    // asks for its marker. It also replaces the base's, which answered with
    // the base's instance. What it is at run time has not changed: the
    // compiler has always written a real closure under this name.
    set_member(c, definition, "new", 3, constructor_for(c, base, instance));

    // [syntax.type.self] with [syntax.obj.instance]: writing the name of a definition asks for an instance
    // of it, so Self^ inside one names the instance as well -- the same object
    // self^ is bound to below. A definition written with no binding to take a
    // name from ([syntax.obj.name]) has this and nothing else to say its own type with.
    struct SelfLink self_here = { instance, c->self_link };
    c->self_link = &self_here;

    // [syntax.obj.name]: and the name this one is landing in, if check_define found one,
    // means the same instance from here on.
    if (c->def_link != NULL && c->def_link->node == node) {
        c->def_link->instance = instance;
    }

    const LhatNode *template = NULL;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        // [syntax.obj.delegate]: the delegate entry carries no key either, so what marks
        // the template is the absence of both.
        if (entry->v.entry.key == NULL &&
            entry->v.entry.modifier != LHAT_DEF_DELEGATE) {
            template = entry->v.entry.value;
        }
    }

    // [syntax.obj.self]: def^ and Self^ name the same definition in value position,
    // and every member reaches it, including static members: the definition
    // exists before any instance. self^ is bound per member below: only
    // one that wrote it among its parameters is handed a receiver, so only
    // there does the name mean anything.
    Scope members;
    chk_scope_open(&members, c->scope, NULL, false);
    const char *owner_names[] = { "def^", "Self^" };
    for (size_t i = 0; i < 2; i++) {
        Binding *owner = chk_scope_add(&members, owner_names[i], strlen(owner_names[i]),
                                       definition, node->offset);
        if (owner != NULL) {
            owner->reached = true;
            owner->declaration = node;
        }
    }

    Scope *outer = c->scope;
    c->scope = &members;

    // [syntax.obj.members-order]: the names first, so the members can reach each other however
    // they are ordered -- 'expr calls term calls factor calls expr' is a ring
    // and no ordering unties it. What can be put down here is what can be
    // read without walking a body: a declaration's written type, and a
    // subroutine's written signature. A member whose type only its value
    // knows is not seeded, which is [pipeline.infer]'s line -- what is read before
    // it is inferred has to have been written.
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        // [syntax.annot.where]: a member of a def^ takes one.
        chk_check_annotations(c, entry->v.entry.annotations,
                              LHAT_ANNOTATION_MEMBER);
        chk_refuse_hat_name(c, entry->v.entry.key, true);
        if (entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        // A declaration's type is read here and nowhere else -- reading it
        // twice would report whatever is wrong with it twice.
        LhatType *seed = entry->v.entry.declared
                             ? chk_resolve_type(c, entry->v.entry.value)
                             : entry->v.entry.type != NULL
                                   ? chk_resolve_type(c, entry->v.entry.type)
                             : declared_signature(c, entry->v.entry.value);
        if (seed == NULL) {
            continue;
        }
        chk_member_declared_at(
            c,
            set_member_provisional(c, definition, name, length, seed,
                                   entry->v.entry.declared),
            entry->v.entry.key);
        // [syntax.obj.members-order]: an instance carries what may be reached through one, which
        // the signature says. A declaration ([syntax.obj.abstract]) is read the same way --
        // what fills it in is held to the type written here.
        if (chk_takes_receiver(seed)) {
            chk_member_declared_at(
                c,
                set_member_provisional(c, instance, name, length, seed,
                                       entry->v.entry.declared),
                entry->v.entry.key);
        }
    }

    // The fields next, so a member body sees them through self^. Inside the
    // members scope: [syntax.obj.new] evaluates an initialiser once, as the definition
    // is built, where def^ and every member are in place -- self^ is not,
    // since no instance exists yet, and the scope holds no such name.
    if (template != NULL) {
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            // [syntax.annot.where]: a field of a self^{ … } takes one, which is where
            // an @export-shaped annotation goes.
            chk_check_annotations(c, field->v.entry.annotations,
                                  LHAT_ANNOTATION_FIELD);
            chk_refuse_hat_name(c, field->v.entry.key, true);
            if (!chk_node_name(c, field->v.entry.key, &name, &length)) {
                continue;
            }
            const LhatTypeMember *required =
                lhat_type_own_member(instance, name, length);
            if (required != NULL && !required->instance_field) {
                chk_report_named(c, field, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                 name, length);
                continue;
            }
            // [syntax.obj.abstract]: a field the composition has to provide carries its type
            // and no value -- and so no key on the prototype.
            if (field->v.entry.declared) {
                LhatType *declared =
                    chk_resolve_type(c, field->v.entry.value);
                if (required != NULL && required->abstract) {
                    declared = lhat_type_intersect(c->result->types,
                                                  required->type, declared);
                }
                chk_check_tostring_shape(c, field, declared, name, length, true);
                LhatTypeMember *added =
                    set_member_as(c, instance, name, length, declared, true);
                if (added != NULL) { added->instance_field = true; }
                chk_member_declared_at(c, added, field->v.entry.key);
                continue;
            }
            // [syntax.obj.template]: a type written alongside the value is what the field
            // holds, and the value is measured against it -- the same reading
            // [modules.host.machine]'s 'let^ x : T = v' has, and for the same reason: what a
            // reader may rely on is what was written, not what the first
            // value happened to be.
            LhatType *written = chk_resolve_type(c, field->v.entry.type);
            LhatType *outer_expected = c->expected_func;
            c->expected_func = written;
            LhatType *actual = chk_infer(c, field->v.entry.value);
            c->expected_func = outer_expected;
            if (written != NULL) {
                chk_expect(c, field->v.entry.value, actual, written,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            if (required != NULL && required->abstract) {
                chk_expect(c, field, written != NULL ? written : actual,
                           required->type, LHAT_CHECK_ERR_MISMATCH);
            }
            // [syntax.obj.new]: the value goes on the prototype and every instance
            // starts as a copy of it, so it has to be a leaf nothing can
            // write through or a literal tree the copy owns. The value's own
            // type and spelling are what is judged -- 'slot : any^ = 1'
            // shares a number, however wide the field is. The machine asks
            // the same question of the values themselves (SETPROTO), for the
            // code checking never saw.
            if (!immutable_default_value(c, field->v.entry.value, actual)) {
                chk_report_named(c, field->v.entry.value,
                                 LHAT_CHECK_ERR_MUTABLE_DEFAULT, name, length);
            }
            chk_check_tostring_shape(c, field, written != NULL ? written : actual,
                                     name, length, true);
            LhatTypeMember *added = set_member(
                c, instance, name, length, written != NULL ? written : actual);
            if (added != NULL) { added->instance_field = true; }
            chk_member_declared_at(c, added, field->v.entry.key);
        }
    }

    // [pipeline.infer.rounds]: the walk below is one iteration of a least fixpoint. A
    // member read before its own body was walked answers with a seed, and a
    // ring of them -- expr calls term calls factor calls expr -- has at least
    // one such read whatever the order. Walking the entries again from what
    // the last walk inferred is what closes the ring, and it is walked again
    // only while that keeps changing what they answer. Rounds holds the
    // bookkeeping; what is seeded and what is put back before a walk is here.
    size_t entry_count = 0;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        entry_count++;
    }
    SeenMember *seen =
        entry_count > 0 ? calloc(entry_count, sizeof(*seen)) : NULL;
    Rounds rounds;
    chk_rounds_begin(c, &rounds, seen != NULL ? entry_count : 0);

    do {
        // [syntax.obj.delegate]: before the bodies, so a member reaches through self^ what
        // this definition delegates to -- the whole point of writing one is
        // that the name is the wrapper's own, and that has to hold inside it
        // as much as outside. Silent here: the round it settles in is not
        // known ahead, and what a round says is thrown away when another
        // follows (chk_rounds_next), so the last one is the one that reports.
        link_delegate(c, node, definition, instance, false);

        size_t round = rounds.round;
        size_t index = 0;
        for (const LhatNode *entry = node->v.list.items; entry != NULL;
             entry = entry->next, index++) {
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key == NULL ||
                !chk_node_name(c, entry->v.entry.key, &name, &length)) {
                continue;
            }
            // [syntax.obj.override] asks what was already under this name, and a seed from
            // the pass above was not -- it is this very entry, read ahead.
            const LhatTypeMember *hidden = NULL;
            if (round == 0) {
                hidden = chk_find_member(definition, name, length);
                if (hidden != NULL && hidden->provisional) {
                    hidden = NULL;
                }
                if (seen != NULL) {
                    seen[index].found = hidden != NULL;
                    if (hidden != NULL) {
                        seen[index].member = *hidden;
                    }
                }
            } else if (seen != NULL && seen[index].found) {
                // [syntax.obj.delegate]: asking again would answer with what this very walk
                // wrote over the name, so the first walk's answer is kept
                // above and handed back here.
                hidden = &seen[index].member;
            }

            // [syntax.obj.abstract]: a declaration carries a type and no value, and says the
            // composition has to provide the member. It is not a definition
            // of it, so [syntax.obj.override] has nothing to check here.
            if (entry->v.entry.declared) {
                const LhatTypeMember *seeded =
                    chk_find_member(definition, name, length);
                LhatType *declared =
                    seeded != NULL && seeded->provisional
                        ? seeded->type
                        : chk_resolve_type(c, entry->v.entry.value);
                const LhatTypeMember *held =
                    lhat_type_own_member(instance, name, length);
                if (chk_takes_receiver(declared) && held != NULL &&
                    held->instance_field) {
                    chk_report_named(c, entry, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                     name, length);
                    continue;
                }
                // [syntax.op.functions]: an op^ is a member with an operator for a name, and
                // declaring one asks the same shape of it that giving one does.
                if (chk_is_operator_name(name, length)) {
                    chk_check_operator_shape(c, entry, declared, name, length);
                } else {
                    chk_refuse_self_last(c, entry, declared);
                }
                chk_check_tostring_shape(c, entry, declared, name, length, true);
                if (hidden != NULL && !hidden->abstract) {
                    // Already provided, so the declaration asks for nothing.
                    chk_report(c, entry, LHAT_CHECK_ERR_ALREADY_PROVIDED);
                }
                if (hidden != NULL && hidden->abstract) {
                    declared = lhat_type_intersect(c->result->types,
                                                  hidden->type, declared);
                }
                // [syntax.obj.abstract-same]: and the composition is where it is provided.
                // Writing the value here as well leaves the declaration
                // saying nothing -- [syntax.obj.members-order] already lets the members reach
                // each other whatever the order, so there is no forward
                // reference left to declare for.
                if (defined_in_def(c, node, name, length)) {
                    chk_report(c, entry, LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE);
                }
                chk_member_declared_at(
                    c,
                    set_member_as(c, definition, name, length, declared, true),
                    entry->v.entry.key);
                if (chk_takes_receiver(declared)) {  // [syntax.obj.members-order], as above
                    chk_member_declared_at(
                        c,
                        set_member_as(c, instance, name, length, declared,
                                      true),
                        entry->v.entry.key);
                }
                continue;
            }

            // [syntax.obj.new]: a member spelled new and written as a body is the
            // constructor. The machine makes the instance and the body only
            // adjusts it -- so self^ is bound around it the way a method's
            // is, and chk_infer_func reads c->new_func to open the body as
            // one.
            if (chk_name_is(name, length, "new") &&
                entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                definition->v.table.new_overridden = true;
                instance->v.table.new_overridden = true;
            }
            bool constructor_entry =
                chk_name_is(name, length, "new") &&
                entry->v.entry.value != NULL &&
                entry->v.entry.value->kind == LHAT_NODE_FUNC;

            // [syntax.obj.super]: super^ is a name only inside an override^, and what it
            // names is everything that was under this name -- the whole
            // intersection when [syntax.obj.override]'s overload^ put several there, since
            // the write replaces the member as a whole.
            LhatType *outer_super = c->super_type;
            const LhatNode *outer_super_binding = c->super_binding;
            c->super_type = NULL;
            c->super_binding = NULL;
            if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                // [syntax.obj.abstract-override]: with nothing under the name yet, the shape super^
                // will have is the one written here. [syntax.obj.override] has the
                // replacement usable where the original was -- arguments
                // wider, result narrower -- so what is written is admissible
                // wherever the original is, and taking it for super^ cannot
                // promise more than the base gives.
                c->super_binding = entry;
                c->super_type =
                    hidden != NULL
                        ? hidden->type
                        : declared_signature(c, entry->v.entry.value);
                // [syntax.obj.new]: inside a new the name means the hook of the new
                // before it -- the same arguments run against the same
                // receiver, with nothing to answer.
                if (constructor_entry) {
                    c->super_type = hook_signature(c, c->super_type);
                }
            }
            // [syntax.obj.self]: the receiver is this member's, so it is bound around this
            // body and nowhere else. A subroutine written inside the body
            // still reaches it, the way any name in scope is reached ([syntax.if.bool]'s
            // capture).
            Scope receiver;
            bool method = declares_self(c, entry->v.entry.value) ||
                          constructor_entry;
            if (method) {
                chk_scope_open(&receiver, c->scope, NULL, false);
                Binding *bound = chk_scope_add(&receiver, "self^", 5, instance,
                                               node->offset);
                if (bound != NULL) {
                    bound->reached = true;
                    bound->declaration = entry->v.entry.value;
                }
                c->scope = &receiver;
            }
            const LhatNode *outer_new = c->new_func;
            if (constructor_entry) {
                c->new_func = entry->v.entry.value;
            }
            LhatType *annotation = entry->v.entry.type != NULL
                                       ? chk_resolve_type(c, entry->v.entry.type) : NULL;
            LhatType *outer_expected = c->expected_func;
            c->expected_func = annotation;
            LhatType *type = chk_infer(c, entry->v.entry.value);
            c->expected_func = outer_expected;
            if (annotation != NULL) {
                chk_expect(c, entry->v.entry.value, type, annotation, LHAT_CHECK_ERR_MISMATCH);
                type = annotation;
            }
            c->new_func = outer_new;
            if (method) {
                c->scope = &members;
                chk_scope_close(c, &receiver);
            }
            c->super_type = outer_super;
            c->super_binding = outer_super_binding;
            const LhatTypeMember *held =
                lhat_type_own_member(instance, name, length);
            if (chk_takes_receiver(type) && held != NULL && held->instance_field) {
                chk_report_named(c, entry, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                 name, length);
                continue;
            }
            // [syntax.obj.override]: two members of one name in a single def^ need a marker
            // too, so what is already there has to include this def^'s
            // earlier entries and not only what the base brought.
            // `definition` is both -- it was copied from the base above and
            // has been accumulating since.
            type = check_same_name(c, entry, hidden, type,
                                   chk_name_is(name, length, "new"));
            if (chk_is_operator_name(name, length)) {
                chk_check_operator_shape(c, entry, type, name, length);
            } else {
                chk_refuse_self_last(c, entry, type);
            }
            chk_check_tostring_shape(c, entry, type, name, length, true);
            // [syntax.obj.abstract-override]: an override^ that found nothing waits for a
            // composition to bring what it replaces. Until then super^ inside
            // it points at nothing, so [syntax.obj.new]'s new has to stay out of reach.
            // Landing on another one that is waiting settles neither.
            bool pending = entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                           (hidden == NULL || hidden->pending);
            // [syntax.obj.abstract]: what answers an abstract^ keeps saying so, for the
            // composition that might make the name ambiguous later.
            bool demanded =
                hidden != NULL && (hidden->abstract || hidden->demanded);
            LhatTypeMember *written = set_member_marked(
                c, definition, name, length, type, false, pending);
            if (written != NULL) {
                written->demanded = written->demanded || demanded;
            }
            chk_member_declared_at(c, written, entry->v.entry.key);
            // [syntax.obj.members-order]: only what is handed a receiver is reachable through an
            // instance. new and a static member stay the definition's.
            if (chk_takes_receiver(type)) {
                LhatTypeMember *mirrored = set_member_marked(
                    c, instance, name, length, type, false, pending);
                if (mirrored != NULL) {
                    mirrored->demanded = mirrored->demanded || demanded;
                }
                chk_member_declared_at(c, mirrored, entry->v.entry.key);
            }
            if (seen != NULL) {
                if (round == 0 || !lhat_type_equal(seen[index].inferred, type)) {
                    rounds.changed = true;
                }
                seen[index].inferred = type;
            }
        }

        if (!chk_rounds_next(c, &rounds)) {
            break;
        }
        // What this def^ wrote goes back to being a seed, carrying the type
        // the walk just inferred -- that is what starting from what was
        // learned amounts to. A member the base brought keeps what the walk
        // made of it: the entry hiding it was answered from `seen` above.
        for (const LhatNode *entry = node->v.list.items; entry != NULL;
             entry = entry->next) {
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key == NULL ||
                !chk_node_name(c, entry->v.entry.key, &name, &length)) {
                continue;
            }
            mark_provisional(definition, name, length);
            mark_provisional(instance, name, length);
        }
    } while (true);
    free(seen);
    chk_rounds_end(c, &rounds);

    c->scope = outer;
    chk_scope_close(c, &members);

    // [syntax.obj.abstract-new]: a template field the written new gives a value to is provided,
    // the same as one a composition fills. [syntax.obj.override-new] makes an override^ new
    // replace the default whole, so that new is the only way to build one and
    // what it writes is what every instance has. An overload^ leaves the
    // default arm standing, and that one writes nothing -- so it settles
    // nothing here.
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.modifier != LHAT_DEF_OVERRIDE ||
            entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length) ||
            !chk_name_is(name, length, "new")) {
            continue;
        }
        // The body, not the f^ itself: collect_written_fields stops at a
        // subroutine, which is what keeps a nested one's self^{ … } out.
        const LhatNode *made = entry->v.entry.value;
        if (made == NULL || made->kind != LHAT_NODE_FUNC) {
            break;
        }
        WrittenFields written;
        written.c = c;
        written.count = 0;
        written.seen_one = false;
        written.overflowed = false;
        collect_written_fields(&written, made->v.func.body);
        if (written.overflowed) {
            break;  // more than can be tracked; leave every hole as it was
        }
        for (size_t i = 0; i < written.count; i++) {
            for (LhatTypeMember *m = instance->v.table.members; m != NULL;
                 m = m->next) {
                if (m->abstract && m->name_length == written.lengths[i] &&
                    memcmp(m->name, written.names[i], m->name_length) == 0) {
                    m->abstract = false;
                    break;
                }
            }
        }
        break;  // [syntax.obj.override] allows one entry of the name, so there is one to read
    }

    // [syntax.obj.delegate]: and again once every round is over, which is where a delegate
    // that never settled is reported. Writing the same link twice is
    // writing it once.
    link_delegate(c, node, definition, instance, true);

    c->self_link = self_here.outer;
    return definition;
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
                             !run_of_names(node, true))) {
            named_type = NULL;
        }
    }
    LhatType *context_named = NULL;
    bool expects_descriptor = c->expected_func != NULL &&
        c->expected_func->kind == LHAT_TYPE_TABLE && c->expected_func->v.table.is_typeinfo;
    if (node != NULL) ((LhatNode *)node)->descriptor_type = NULL;
    LhatType **capture = named_type;
    if (capture == NULL && expects_descriptor && node != NULL && run_of_names(node, true)) {
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
        node != NULL && run_of_names(node, true) &&
        type != NULL && type->kind == LHAT_TYPE_TABLE && !type->v.table.is_typeinfo) {
        // Host declarations and def^ values carry their type through their
        // declaration shape rather than through an explicit alias marker.
        *capture = chk_resolve_type(c, node);
    }
    if (expects_descriptor && capture != NULL && *capture != NULL &&
        !(type != NULL && type->kind == LHAT_TYPE_TABLE && type->v.table.is_typeinfo)) {
        if (runtime_has_unresolved_arguments(*capture, NULL)) {
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
            return infer_table(c, node);

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
                    if (runtime_has_unresolved_arguments(asked, 0)) {
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
        // nil_propagated: [syntax.type.fits]'s `narrowable` refuses a '?.' path outright.
        case LHAT_NODE_CALL:
            return nil_propagated(c, node, chk_infer_call(c, node));

        case LHAT_NODE_MEMBER: {
            LhatType *narrowed = chk_narrowed_type(c, node);
            // Narrowing changes the value type, not the identity of the read.
            // Walk the member and its target even when flow already knows the
            // answer, so codegen receives bindings and member provenance.
            LhatType *held = chk_infer_member(c, node, named_type);
            LhatType *answer = narrowed != NULL ? narrowed : nil_propagated(c, node, held);
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
            return nil_propagated(c, node, infer_index(c, node));

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
            if (runtime_has_unresolved_arguments(wanted, 0)) {
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
                !runtime_has_unresolved_arguments(operand, NULL)) return chk_typeinfo_of(c, operand);
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
            if (runtime_has_unresolved_arguments(named, NULL)) {
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
                LhatType *made = infer_table(c, node);
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
