// L^ (lhat) -- the type checking stage: expressions.

#include "check_internal.h"

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

void chk_expect(Checker *c, const LhatNode *at, LhatType *value,
                LhatType *target, LhatCheckErrorCode code)
{
    // 03 の 3.4: a parameter whose type the body is deciding is not being
    // checked here -- this position is one of the things deciding it. Most of
    // what a body demands arrives through this one call.
    if (chk_param_var_for(c, value) != NULL) {
        chk_constrain(c, value, target);
        return;
    }

    // 03 の 3.1・3.5: strict reports a lingering gap in inference here;
    // relaxed waves unknown^ through on purpose. 3.5 withdrew the inserted
    // runtime check that was once meant to stand behind the waving -- what a
    // waved-through mismatch meets now is the machine's own instruction
    // check, which panics where it lands (04 の 11.6).
    bool ok = c->strict ? lhat_type_conforms_strict(value, target)
                        : lhat_type_conforms(value, target);
    if (!ok) {
        chk_report(c, at, code);
    }
}

// 04 の 11.3: a table answers nil^ for a key that is not there, so what a key
// reads out of one is 'T|nil^' -- and no operator is on the nil^ side. Saying
// 11.3改's rule there sends the writer looking for an operator to write, when
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

// 11.3改: the one shape every operator is judged with. The left operand is
// asked for the member 11.8 names, 11.1 makes that a function, 14.4 puts the
// left operand in its self^ -- so the right operand is its argument and the
// expression is worth what it answers.
//
// NULL means nothing was decided: the operand types said too little (03 の
// 3.5), or the answer was already reported. The caller says what to fall back
// on, since that differs by operator.
// The arm of an operator member that takes these arguments, or NULL. 14.12
// forbids overlapping signatures, so at most one ever does. `self_last` picks
// which side the arm was written for (11.3改): one written the other way round
// describes the other order and is passed over.
//
// 11.8改: `count` is 1 for a binary operator and 0 for the unary '-', which
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

// 11.9: whether one of the two carries an operator of this name taking the
// other, which is what the comparison is read off. 3.5: wherever inference
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
    // 11.3改: or the right operand carries one written as the receiver, which
    // is how a value joins a comparison whose left side is a built-in.
    args[0] = left;
    return operator_arm(chk_operator_member(c, right, name, length), args, 1,
                        true) != NULL;
}

// 11.9 with 11.9改: what says how these two compare. An ordering has only
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

// 11.5 の (5) with 11.9: one link of a comparison, asked the same way whether
// it stands alone or in a chain. Always answers bool^ -- what may be wrong is
// the pair, never the shape of the answer.
static LhatType *check_comparison(Checker *c, const LhatNode *at, LhatOpKind op,
                                  LhatType *left, LhatType *right)
{
    // 03 の 3.4改3: an ordering asks the pair to be related by a '<=>', so a
    // parameter standing in one is demanded what could carry it. 11.9 leaves
    // '=' and '≠' out: equality answers without a '<=>' at all (14.2 gives
    // every table its identity), so what they ask is that the two are not
    // disjoint -- and that is not a type to demand.
    if (op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS &&
        !demand_ordering(c, at, left, right)) {
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // 11.9: an operator taking the two is what says how they compare, and
    // it is asked first. One written across two types -- 11.3改 lets the
    // right operand carry it -- relates a pair that 14.12 would otherwise
    // call separate, so the judgement below has to come second.
    bool related = related_pair(c, op, left, right);

    // 14.12's disjointness says whether any value inhabits both. If none
    // does, and nothing says how they compare either, the answer is fixed
    // before the program runs -- a mistake rather than a comparison.
    if (!related && lhat_type_disjoint(left, right)) {
        chk_report(c, at, LHAT_CHECK_ERR_INCOMPARABLE);
        return chk_simple(c, LHAT_TYPE_BOOL);
    }
    // An ordering has nothing but a '<=>' to read. Equality is a different
    // matter and is left alone -- every value is the same as itself or not,
    // whatever it is (14.2), and an op^= or a '<=>' only refines that for a
    // type that writes one.
    if (!related && op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS) {
        // Either side may be the one carrying the nil^, so both are asked --
        // 't[1] < 3' and '3 < t[1]' are the same mistake read from the two
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

// 11.3改: what the RIGHT operand answers, when it was written as the
// receiver ('op^+ = f^lhs:number^, self^'). Reached only once the left has
// been asked and has no arm taking this right operand, which is what keeps
// 11.3's left-first rule intact -- and is the case that matters, since a
// built-in on the left carries the arithmetic but takes only its own kind.
//
// `answered` tells a result of NULL (an operator answering nothing) from no
// answer at all; the caller reports only in the second case.
static LhatType *right_operator(Checker *c, const char *name, size_t length,
                                LhatType *left, LhatType *right,
                                bool *answered)
{
    *answered = false;
    // 3.5: nothing decided about the right operand is nothing to look a member
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

// 11.8改: what '-x' is worth when x is not a number^. The operand carries the
// member 11.8 names, and 14.4 puts it in self^ -- with nothing else written
// there is no argument, which is the whole difference from the binary one.
//
// NULL means nothing was decided, the same as infer_operator: the caller falls
// back on 14.8's number^ and reports against that.
static LhatType *infer_unary_operator(Checker *c, LhatType *operand)
{
    // 03 の 3.5: a parameter says too little to look a member up on, and 14.8
    // already demanded number^ of it before this is asked.
    if (chk_operator_undecided(operand) || chk_param_var_for(c, operand) != NULL) {
        return NULL;
    }
    // 11.3改 is about which operand is the receiver, and a unary one has only
    // the one -- so the arm is asked for the leading self^ and no other.
    const LhatType *arm =
        operator_arm(chk_operator_member(c, operand, "-", 1), NULL, 0, false);
    return lhat_type_call_answer(arm);
}

// 11.3改: the operator member this side answers with as the LEFT operand. One
// written with its self^ last describes the other order -- it answers when its
// owner stands on the right, so carrying it is not answering here and it reads
// exactly as though it were absent.
// 11.3改: the arms of a group written for this order -- the self^-last ones
// describe the other and are never candidates here -- when there is exactly
// one. A group may hold several signatures and still leave one choice, which
// is what lets 03 の 3.4 read a demand off it.
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

// 03 の 3.4改3: what an operator of this name may be written on, here. Each
// position's union is what a parameter standing there may be.
typedef struct {
    LhatType *left;    // what may stand on the left
    LhatType *right;   // what may stand on the right
    LhatType *answer;  // what any of them answers
    size_t count;      // how many arms survived, over every carrier
} Candidates;

// What the walk knows about the operand that is not the parameter, when it
// knows anything. An arm that could not take it is no candidate, which is
// what keeps 'x < 1' from demanding string^ as well (11.9 gives '<=>' to
// both built-ins) -- and what makes it demand number^ alone.
typedef struct {
    LhatType *type;   // NULL when that side is undecided too
    bool on_right;    // which side it stands on
} Known;

// One arm folded into the three unions. 11.3改: an arm written with the self^
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

// 03 の 3.4改3: the candidates. 11.8's built-ins carry what the checker knows
// they carry -- number^ the arithmetic, string^ the '..', both the one
// comparison (11.9) -- and every def^ this unit binds to a name and writes an
// op^ of the name on is another.
//
// The carriers are held by name (check.c), so the type is read through the
// scope on every walk -- a def^'s is remade on each of them (03 の 3.4改2).
// Reading one this walk has not settled yet asks for another round, which is
// what lets a body written above the def^ answer as one written below it does
// (8.7).
static Candidates operator_candidates(Checker *c, const char *name,
                                      size_t length, const Known *known)
{
    Candidates found;
    found.left = NULL;
    found.right = NULL;
    found.answer = NULL;
    found.count = 0;

    // 11.8: what the checker carries for the built-in types, asked of each so
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
        Binding *b = chk_scope_find(c->scope, carrier->name,
                                    carrier->name_length, NULL);
        if (b == NULL) {
            continue;
        }
        if (!b->reached) {
            // 3.4改2: answered from the seed, so this walk read ahead of
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

// 03 の 3.4改3: what an ordering demands of a parameter standing in it. 11.9
// makes '<=>' the one comparison a type writes, so the candidates are its --
// which is also what keeps 11.9改's op^=-only type out of an ordering: it
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

static LhatType *infer_operator(Checker *c, const LhatNode *node, LhatOpKind op,
                                LhatType *left, LhatType *right)
{
    size_t length = 0;
    const char *name = chk_operator_name(op, &length);
    if (name == NULL) {
        return NULL;
    }

    // 03 の 3.4: the left operand is the receiver (14.4), so an operator used
    // on a parameter is a demand on it -- and what it demands is what may
    // carry the operator here. '..' is the one name that demands nothing
    // (11.2 gives it composition as well as concatenation, and only one of
    // the two is an op^ to count).
    //
    // 3.4改: with 11.8's built-in the only candidate the demand is number^,
    // which is what it has always been. Where the unit writes an op^ of this
    // name the demand is the union, and the body below then says the operator
    // is not settled -- but the signature names the candidates, so a writer
    // can annotate one or narrow to it (13.11).
    if (chk_param_var_for(c, left) != NULL) {
        if (name[0] == '.') {
            return NULL;
        }
        // 3.4改3: the right operand narrows the field where it is settled --
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
            // one -- by annotating, or by narrowing to it in the body (13.11)
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
        // 11.3改: nothing on the left, so the right operand gets the
        // question -- it may have been written as the receiver.
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        // 04 の 11.3: a nil^ arm is why the member was not found, rather than
        // an operator nobody wrote -- the same lookup answers without it.
        LhatType *bare = NULL;
        bool by_nil = nil_arm_apart(c, left, &bare) &&
                      left_operator_member(c, bare, name, length) != NULL;
        chk_report(c, node->v.binary.left,
                   by_nil ? LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL
                          : LHAT_CHECK_ERR_NO_OPERATOR);
        return NULL;
    }

    // 11.8 with 14.12: one type may answer an operator for several right-hand
    // types, and then the member is the intersection of those signatures.
    // Which arm a use means is settled the way a call's is -- by what is
    // handed over -- and the ban on overlapping signatures leaves at most one.
    // 14.4 puts the left operand in self^, so the right one is the single
    // argument and each arm is asked the way a member call asks.
    //
    // Answering NULL here instead would fall back on 14.8's number^ (the
    // caller's), which is not what the arm says and not what the machine
    // returns -- call_operator has always searched these.
    // 03 の 3.4: with nothing decided about the right operand there is nothing
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
            // 11.3改: the self^-last arms of this same group answer the
            // other order, so they are not candidates here.
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC &&
                arm->type->v.func.self_last) {
                continue;
            }
            if (chk_signature_accepts(arm->type, args, 1, true)) {
                return lhat_type_call_answer(arm->type);
            }
        }
        // 11.3改: no arm here takes it, so the right operand is asked
        // whether it was written as the receiver instead.
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        // 04 の 11.3: an arm does take it once the nil^ is off, so the
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
    // 11.8改: a '-' written with self^ alone is the unary one, and it takes no
    // right operand at all. Asked here the same way an arm of the wrong shape
    // is: the right side is offered to 11.3改 first, and refused after.
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
    // 11.3改: this one does not take the right operand, so that side is
    // asked whether it was written as the receiver. Tried before expect so
    // that the answer is taken rather than reported -- and only when the left
    // has already failed, which is 11.3's order kept intact. A built-in on
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
    // 03 の 3.4: a parameter on this side is undecided in the sense above, but
    // what the operator takes is a demand on it rather than a gap to wave
    // through -- expect is what tells the two apart.
    if (wanted != NULL &&
        (chk_param_var_for(c, right) != NULL || !chk_operator_undecided(right))) {
        // 04 の 11.3: '1 + t[i]' reaches here, and what the operator takes is
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

// 05 の 8.2: the member of L^ a host-bound name reaches, or NULL when the
// host bound no such name. Nothing is added to any scope for these -- they
// are asked for only where a scope answered nothing, which is what lets a
// let^ of the same spelling shadow one without anything being removed.
static LhatType *initial_binding_type(Checker *c, const char *name,
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
        return m != NULL ? m->type : NULL;
    }
    return NULL;
}

// 04 の 5.3 with 3.4: an error on its way out of the body being checked --
// through a try^, or through a try^{ } whose arms did not take it. Where the
// result was written, this is where it is asked whether that admits it; where
// none was, this is one of the exits the result is the union of.
void chk_error_leaves(Checker *c, const LhatNode *at, LhatType *escaping)
{
    if (escaping == NULL) {
        return;
    }
    if (c->declared_result != NULL) {
        if (!lhat_type_conforms(escaping, c->declared_result)) {
            chk_report(c, at, LHAT_CHECK_ERR_TRY_OUTSIDE);
        }
        return;
    }
    // 13.8改's width check is the return^ side's business: a union of a tuple
    // with an error kind is what 04 の 3.1 admits, so the arm added here is
    // not one to measure.
    c->inferred_result =
        lhat_type_union(c->result->types, c->inferred_result, escaping);
}

LhatType *chk_infer_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node, &name, &length)) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 01 の 2.3: the stacked reach. this^^ walks the chain of
    // enclosing bodies; it^^/self^^/class^^ walk past inner bindings of the
    // same name -- the same search vm.c makes, so the two agree on which
    // binding a count lands on. The parser admits no other word here.
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
            return link->type;
        }
        Binding *outer = chk_scope_find_skipping(c->scope, name, length, levels);
        if (outer == NULL) {
            chk_report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        outer->reached = true;
        return outer->type;
    }

    // A few hat identifiers are values rather than names (01 の 2.2).
    if (node->kind == LHAT_NODE_HAT_IDENT) {
        // 13.12: '_^' is written where a name would go and binds nothing, so
        // reaching it as a value asks for what was thrown away. Every place
        // that may write it takes it before inference is asked; arriving here
        // means it stood in an expression.
        if (chk_name_is(name, length, "_^")) {
            chk_report(c, node, LHAT_CHECK_ERR_DISCARD_READ);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        // 07 の 4 章: these three are the ones deliberately left unrecorded.
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
        // 15.10: this^ names the subroutine running, which is what lets a
        // body with no name recurse. Only the hatted spelling means it, so
        // an ordinary name `this` is untouched.
        if (chk_name_is(name, length, "this^")) {
            if (c->this_type == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_THIS_OUTSIDE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 03 の 3.4 counts it the same way a call by name is counted.
            c->saw_self_call = true;
#if LHAT_WITH_RESOLUTIONS
            chk_record_typed_resolution(c, node, c->this_type);
#endif
            return c->this_type;
        }
        // 14.12改: super^ is the member an override^ is replacing. Named on
        // its own it is an ordinary value, so 14.4 applies and the receiver
        // is written out; called directly it is the bound form, which
        // infer_call takes.
        if (chk_name_is(name, length, "super^")) {
            if (c->super_type == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_SUPER_OUTSIDE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
#if LHAT_WITH_RESOLUTIONS
            chk_record_typed_resolution(c, node, c->super_type);
#endif
            return c->super_type;
        }
        // 05 の 8.6: the machine's own table, there without being imported.
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

    // 01 の 8 章: a specifier says which scope to start the search in.
    // Without one it starts here, which is every other name.
    Scope *from = c->scope;
    if (node->kind == LHAT_NODE_SCOPE) {
        from = chk_scope_from(c->scope, node);
        if (from == NULL) {
            chk_report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
    }

    // Which scope answered, which three of the rules below have to know: 8.7
    // tells a name this body bound from one still waiting further out, and
    // 15.13 and 05 の 8.9 each measure it against a boundary of their own.
    Scope *found_in = NULL;
    Binding *b = chk_scope_find(from, name, length, &found_in);
    // 8.7改: a binding does not stand in its own initialiser -- anywhere in
    // it, a deferred body included. What it holds is being worked out right
    // here, so the name still means what it meant outside; recursion by the
    // bound name went with this (15.10: this^ is the one spelling, and the
    // stronger one -- it carries the literal's own signature where the name
    // held only a seed). Where nothing outside answers, the read falls
    // through to the report below, which is the same one it always got.
    if (b != NULL && b->being_defined && found_in != NULL) {
        Scope *outer = NULL;
        Binding *shadowed =
            chk_scope_find(found_in->parent, name, length, &outer);
        if (shadowed != NULL) {
            b = shadowed;
            found_in = outer;
        }
    }
    if (b == NULL) {
        // 05 の 8.2: what the host bound before anything ran. Asked after
        // every scope, so a let^ of the same spelling shadows it -- and what
        // it reaches stays readable as L^.<member>, since 8.1 keeps the hat
        // identifier out of the spellings a let^ can make.
        LhatType *bound = initial_binding_type(c, name, length);
        if (bound != NULL) {
#if LHAT_WITH_RESOLUTIONS
            // 07 の 4 章: nothing here declared it -- the host did, in C --
            // so there is no place to point at, only the type it registered.
            // Which is the whole of what a reader meeting `print` wants.
            chk_record_typed_resolution(c, node, bound);
#endif
            return bound;
        }
        chk_report_named(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 8.7: the name is visible throughout the scope, which is what makes
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
        // 8.7改: a being-defined binding nothing outer shadows reports
        // outright wherever it is read -- its own value cannot answer, and
        // the ring could never improve on that.
        if (b->being_defined || c->deferred == 0 ||
            chk_scope_within_body(c, found_in)) {
            chk_report(c, node, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
        } else {
            // 03 の 3.4改2: what answered is the seed collect_bindings put
            // there, or what an earlier walk left. Either way this walk read
            // ahead of itself, so walking the statements again from what it
            // learned may answer better -- the same fact a def^ member's seed
            // reports when it is the one answering.
            c->read_provisional = true;
        }
    }
    // 15.13: a closed^ body promised to name nothing standing outside it, and
    // this is where that is decided -- the same boundary test, asked of every
    // read rather than of a host value's. An import^ root is not a capture
    // (05 の 8.7 reads it off L^.modules wherever it is written), so it is
    // the one name from outside that may be written here.
    if (c->closed_scope != NULL && !b->import_root) {
        if (!chk_scope_within(c, found_in, c->closed_scope)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_CLOSED_CAPTURES, name,
                             length);
        }
    }
    // 05 の 8.9: a name bound outside this body reaches the value through a
    // capture, and a capture outlives the frame the slots belong to. The
    // same boundary test 15.1 uses for writes, asked of a read here because
    // for a host value the read is what would build the upvalue.
    if (chk_is_hostvalue(b->type) && c->body_scope != NULL) {
        if (!chk_scope_within_body(c, found_in)) {
            chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
    }
#if LHAT_WITH_RESOLUTIONS
    chk_record_resolution(c, node, b);
#endif
    return b->type;
}

// 02 の 11.2改: the table 14 章 does not reach -- a plain one, made by a
// literal rather than a def^ and registered by no host. The kinds a
// definition made keep 14.5's composition and 11.8's op^.. instead.
static bool concatenable_table(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_TABLE &&
           !type->v.table.is_definition && !type->v.table.from_definition &&
           !type->v.table.nominal && type->v.table.hostvalue_tag == NULL;
}

// 02 の 11.2改: what '..' between two plain tables answers. The sequence
// halves in order -- the left's positions, then the right's renumbered after
// them -- and the named keys from both sides, a name both carry reported
// (14.5改's rule, without the qualified spelling that saves a method there).
// An unbounded left ('t^{ ...:T }') leaves the right's positions nowhere to
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
            if (lhat_type_member_position(m, own + 1)) {
                own++;
                if (fold) {
                    element = element == NULL
                                  ? m->type
                                  : lhat_type_union(c->result->types, element,
                                                    m->type);
                } else {
                    lhat_type_add_index_member(c->result->types, joined,
                                               ++position, m->type);
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
    return joined;
}

LhatType *chk_infer_binary(Checker *c, const LhatNode *node)
{
    LhatOpKind op = node->v.binary.op;

    // 04 の 4.1 and 11.7: both drop one arm and put a value in its place.
    if (op == LHAT_OP_CATCH || op == LHAT_OP_NIL_ELSE) {
        LhatType *left = chk_infer(c, node->v.binary.left);
        LhatType *unwanted = chk_simple(c, op == LHAT_OP_CATCH ? LHAT_TYPE_ERROR
                                                           : LHAT_TYPE_NIL);

        // 04 の 4.2: catch^ names the error it^ inside its right side, so the
        // right side is inferred under a scope holding it -- typed as the
        // error half of the left, which is exactly what the machine puts in
        // that register (compile_catch). ?? has no it^: what it replaces is
        // nil^, and there is nothing in a nil^ to name.
        LhatType *right = NULL;
        if (op == LHAT_OP_CATCH) {
            Scope scope;
            scope.bindings = NULL;
            scope.tail = NULL;
            scope.parent = c->scope;
            scope.transparent = false;

            Scope *outer = c->scope;
            c->scope = &scope;
            Binding *caught = chk_scope_add(&scope, "it^", 3,
                                            chk_only(c, left, unwanted), node->offset);
            if (caught != NULL) {
                caught->reached = true;
            }
            right = chk_infer(c, node->v.binary.right);
            c->scope = outer;
            chk_scope_dispose(&scope);
        } else {
            right = chk_infer(c, node->v.binary.right);
        }

        if (!chk_can_be(left, unwanted)) {
            chk_report(c, node, op == LHAT_OP_CATCH ? LHAT_CHECK_ERR_CANNOT_FAIL
                                                : LHAT_CHECK_ERR_CANNOT_BE_NIL);
        }
        LhatType *kept = chk_without(c, left, unwanted);
        // 13.8改 with 04 の 4.1: catch^'s right side is one expression, and
        // there is no expression that is a tuple -- so a call answering
        // several values has no replacement that could stand for them. Said
        // here rather than left to the binding, which would only see that a
        // union is not a tuple and report the count. 'catch^ nil^' stays
        // writable (04 の 4.4's deliberate ignoring): nil^ may stand beside
        // a tuple, and the result is discarded or discriminated, not bound.
        if (lhat_type_tuple_width(kept) > 0 &&
            lhat_type_tuple_width(right) != lhat_type_tuple_width(kept) &&
            !chk_may_stand_beside_tuple(right)) {
            chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
        }
        return lhat_type_union(c->result->types, kept, right);
    }

    LhatType *left = chk_infer(c, node->v.binary.left);

    // 14.5: composition is written with '..' because the order matters. The
    // right side is not a value here -- it is a definition read against what
    // the left already provides, which is what 14.12 needs to see.
    if (op == LHAT_OP_CONCAT && node->v.binary.right != NULL &&
        node->v.binary.right->kind == LHAT_NODE_DEF) {
        return chk_infer_def(c, node->v.binary.right, left);
    }

    // 13.11: isa^ takes a type on the right, so the right side is not a
    // value. 11.6改 moved the type-fit question here from is^, which now
    // asks identity and reads an ordinary value on both sides (below, with
    // the rest of the comparisons).
    if (op == LHAT_OP_ISA) {
        LhatType *asked = chk_resolve_type(c, node->v.binary.right);
        // 13.7: any^ is the top of every value, so this holds of whatever is
        // on the left and the question is empty. 13.11 refuses to read the
        // left's inferred type against the right -- that would narrow the
        // escape hatch 13.7 exists to provide -- but this one is decided by
        // the right side alone, whatever the left turns out to be.
        if (asked != NULL && asked->kind == LHAT_TYPE_ANY) {
            chk_report(c, node, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
        }
        return chk_simple(c, LHAT_TYPE_BOOL);
    }

    // 13.11: and^'s right side runs only where the left held, and or^'s only
    // where it did not -- so what the left established about a path is known
    // inside the right, which is what makes 'x isa^ number^ and^ x <= 0.5'
    // read. Popped straight after: what the whole condition tells a branch is
    // the branch's own question, and chk_narrow_from answers it from the top.
    //
    // 11.5 の (5)'s chain does not come through here -- it is its own node --
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

    switch (op) {
        case LHAT_OP_ADD:
        case LHAT_OP_SUB:
        case LHAT_OP_MUL:
        case LHAT_OP_DIV:
        case LHAT_OP_FLOORDIV:
        case LHAT_OP_MOD:
        case LHAT_OP_POW: {
            // 11.4: arithmetic asks 11.3's question the way '..' does, so a
            // written op^ answers it. 14.8's number^ carries all seven built
            // in, which leaves ordinary arithmetic exactly as it was.
            LhatType *answer = infer_operator(c, node, op, left, right);
            return answer != NULL ? answer : chk_simple(c, LHAT_TYPE_NUMBER);
        }

        // 11.9: the one comparison a type writes. Asked exactly the way
        // '..' and the arithmetic are, so an op^<=> on either side answers it
        // (11.3改), and number^ and string^ carry their own.
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
            // 11.2: '..' is concatenation in general. 11.3 asks the left
            // operand for it -- 14.4 makes that the receiver -- and the right
            // one is the argument, which is what lets one type answer several
            // right-hand types through 14.12's overload^.
            left = chk_require_value(c, node->v.binary.left, left);
            right = chk_require_value(c, node->v.binary.right, right);

            // 14.5: between two definitions '..' composes, and never calls an
            // op^.. either of them carries. The literal form was taken above;
            // this is the one written with names.
            if (left != NULL && left->kind == LHAT_TYPE_TABLE &&
                left->v.table.is_definition && right != NULL &&
                right->kind == LHAT_TYPE_TABLE &&
                right->v.table.is_definition) {
                return chk_compose_definitions(c, node, left, right);
            }

            // 11.2改: two plain tables concatenate, built in the way two
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
// 14.12 forbids overlapping signatures precisely so that at most one arm of
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
        declared++;  // written out at the call (14.4)
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

// 03 の 3.4改: a subroutine written where it is called. The call is part of
// the same expression, so reading it leaves nothing -- 3.4's "the call sites
// are not read" is about the ones elsewhere, which are what would make a
// signature depend on who happens to use it (05 の 4.3 rests on that).
//
// Refused for the two shapes whose arguments do not line up with the
// parameters one for one: a receiver (14.4) is written first and belongs to
// no position, and a spread (13.7) stands for any number of them.
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

// 15.5 with 13.9: the coroutine a call to this signature makes. The middle
// two types are whatever the body's yield^/await^ sites agreed on (15.2);
// a body with no yield^ at all -- only a await^ that never ran, or none
// reached -- leaves them NULL, which nil^ fills the same way an unwritten
// result does.
static LhatType *coroutine_made_by(Checker *c, const LhatType *func)
{
    // 13.9: the third type is what the last resume receives. Where the body
    // ends without a value it is left empty and 15.6改's nil^ joins Y|T at
    // the resume instead -- so the nil^ stands where it is really received
    // rather than in the coroutine's own type. A body that cannot end has no
    // last resume at all, which `endless` says: nothing joins Y there, since
    // putting anything in would make every consumer narrow away a value that
    // never arrives.
    bool endless =
        func->v.func.result == NULL && !func->v.func.ends_without_value;
    // 15.3改: the coroutine carries the kind of the body it came from, which
    // is what decides who may advance it (15.6改).
    //
    // R is left empty when nothing in the body received a yield^: no var^ took
    // one, so there is no value being sent in and a resume takes no argument.
    // Nothing unions R with anything, so there is nothing for a nil^ to be an
    // arm of -- filling one in would be inventing a parameter. Y is not the
    // same: a yield^ with no value really does hand nil^ to the resumer.
    return lhat_type_coro(c->result->types, func->v.func.yield_receive,
                          func->v.func.yield_produce != NULL
                              ? func->v.func.yield_produce
                              : chk_simple(c, LHAT_TYPE_NIL),
                          func->v.func.result, endless,
                          func->v.func.is_function);
}

// 13.9: what one turn of a coroutine answers -- Y|T, which done() is what
// tells apart (15.6改). The third slot has two ways of being empty and they
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

// 15.1改2: an f^ whose receiver seat is mutable writes through it, so an f^
// body may call one only on what it made itself -- 15.3改's rule for a
// coroutine, reached through a call instead of a resume. Inside a
// mutable^self^ body, self^ is the one receiver the signature already
// vouches for, so mutable methods chain through it unasked.
static void check_mutable_receiver(Checker *c, const LhatNode *node,
                                   const LhatType *callee)
{
    if (!c->in_function || callee == NULL ||
        callee->kind != LHAT_TYPE_FUNC || !callee->v.func.is_function ||
        !callee->v.func.mutable_self) {
        return;
    }
    const LhatNode *receiver = NULL;
    if (node->v.access.target->kind == LHAT_NODE_MEMBER) {
        receiver = node->v.access.target->v.access.target;
    } else if (callee->v.func.takes_self && !callee->v.func.self_last) {
        // 14.4: written out, the receiver is the first argument.
        receiver = node->v.access.argument;
    }
    if (receiver != NULL && c->receiver_mutable) {
        // The root of the chain, so 'self^.log.push^(v)' is seen the way
        // the write rule sees 'self^.log[k] := v' -- one root, one body.
        const LhatNode *root = receiver;
        while (root != NULL && (root->kind == LHAT_NODE_MEMBER ||
                                root->kind == LHAT_NODE_INDEX)) {
            root = root->v.access.target;
        }
        const char *name = NULL;
        size_t length = 0;
        if (chk_node_name(c, root, &name, &length) &&
            chk_name_is(name, length, "self^")) {
            return;
        }
    }
    if (receiver == NULL || !chk_receiver_is_own_table(c, receiver)) {
        chk_report(c, node, LHAT_CHECK_ERR_MUTATES_OUTSIDE);
    }
}

LhatType *chk_infer_call(Checker *c, const LhatNode *node)
{
    // 3.4改: the arguments first where the callee is a literal, so what they
    // are is known before the body that takes them is read. Inferred once --
    // the loop below reads them back rather than asking again, since asking
    // twice would report twice (the same reason 14.12's arm search keeps
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
    // 04 の 11.4: '?(' reaches through a callee that may be absent --
    // 'f?(x)' where f is (f^…)|nil^. Relaxed steps past a nil^ arm anywhere;
    // this is the written form that does it under strict too.
    if (!c->strict || node->v.access.nil_safe) {
        callee = chk_without_nil_arm(c, callee);
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
        // 03 の 3.1・3.5、P6: a callee still pending^ (a mutually recursive
        // partner not yet checked) makes this call's result pending^ too,
        // not merely unknown^ -- strict needs to keep seeing the gap if it
        // survives to where a concrete type is wanted.
        return chk_simple(c, callee != NULL && callee->kind == LHAT_TYPE_PENDING
                              ? LHAT_TYPE_PENDING
                              : LHAT_TYPE_UNKNOWN);
    }

    // 14.15: an instance carries a value under every name its definition
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
                // 14.15改3: a field has two ways to be given a value and a
                // member has one, so the two are not told the same thing.
                chk_report_named(c, node,
                                 field ? LHAT_CHECK_ERR_FIELD_UNPROVIDED
                                       : LHAT_CHECK_ERR_STILL_ABSTRACT,
                                 hole->name, hole->name_length);
            }
        }
    }

    // 14.12: an overloaded member is the intersection of its signatures, so
    // calling one means finding the arm that fits. Arguments are inferred
    // once here, since inferring them again would report twice.
    if (callee->kind == LHAT_TYPE_INTERSECT) {
        bool through_member =
            node->v.access.target->kind == LHAT_NODE_MEMBER;

        // 3.4改 with 14.12: which arm is meant is settled by the arguments'
        // own types, so in general there is no one position to read an
        // expectation off of. But when one arm alone takes this many
        // arguments, the count has already settled it -- and then a literal
        // argument takes its parameters from that arm the way it would from
        // an unoverloaded signature. The full answer is still searched for
        // below; this only decides what a literal with nothing written may
        // read.
        const LhatType *by_arity = NULL;
        for (const LhatTypeList *arm = callee->v.composite.arms; arm != NULL;
             arm = arm->next) {
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
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
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
            }
        }
        if (tracked < given) {
            return chk_simple(c, LHAT_TYPE_UNKNOWN);  // more than worth tracking
        }
        size_t position = 0;
        for (const LhatTypeList *arm = callee->v.composite.arms; arm != NULL;
             arm = arm->next, position++) {
            if (chk_signature_accepts(arm->type, args, tracked, through_member)) {
                // 03 の 5.11c: under strict this is the answer, not a guess --
                // an argument whose type is not settled is 3.1's gap and is
                // reported where it reaches a place wanting a concrete type,
                // and a call fitting no arm is the MISMATCH below. So the
                // compiler may bake the arm in and let the run skip 5.11's
                // search. Relaxed writes nothing and keeps the search, which
                // is the only thing left there to decide the call.
                if (c->strict && position + 1 <= UINT16_MAX) {
                    ((LhatNode *)node)->checked_arm = (uint16_t)(position + 1);
                }
                // 15.1, 15.5: f^ may call only f^, whichever arm 14.12
                // resolved to -- except a yieldable p^, whose call alone
                // stays referentially transparent (see the plain-call site
                // above for why).
                if (c->in_function && !arm->type->v.func.is_function &&
                    !arm->type->v.func.yields) {
                    chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
                }
                check_mutable_receiver(c, node, arm->type);  // 15.1改2
                if (arm->type->v.func.answers_fresh) {
                    ((LhatNode *)node)->call_answers_fresh = true;  // 15.1改3
                }
                // 15.5: a yielding arm answers the coroutine it makes, the
                // same as the plain call below.
                return lhat_type_call_answer(arm->type);
            }
        }
        chk_report(c, node, LHAT_CHECK_ERR_MISMATCH);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (callee->kind != LHAT_TYPE_FUNC) {
        chk_report(c, node, LHAT_CHECK_ERR_NOT_CALLABLE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 15.1: f^ may call only f^, never p^ -- a p^ may run side effects an f^
    // is not allowed to. 15.5 carves out one exception: calling a yieldable
    // p^ does not run its body at all, only makes a coroutine (start()
    // is what runs it, and that is p^ itself, caught the same way any other
    // p^ call is) -- so that call alone stays referentially transparent and
    // is not what this rule exists to catch. Reported here rather than
    // refused earlier so arguments still get checked, the same as an
    // ordinary mismatch.
    if (c->in_function && !callee->v.func.is_function &&
        !callee->v.func.yields) {
        chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    }
    check_mutable_receiver(c, node, callee);  // 15.1改2
    if (callee->v.func.answers_fresh) {
        ((LhatNode *)node)->call_answers_fresh = true;  // 15.1改3
    }

    const LhatTypeList *param = callee->v.func.params;

    size_t declared = 0;
    for (const LhatTypeList *p = param; p != NULL; p = p->next) {
        declared++;
    }

    // 14.4: 'x.m()' hands x to the receiver without writing it, while a
    // method taken as a value is called with the receiver spelled out. The
    // receiver is kept out of `params`, so only the second form adjusts.
    //
    // 14.12改: 'super^(…)' is the first of the two. What it replaces is a
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

    // 13.7: 'expr...' as the last argument spreads a collected table -- or a
    // tuple -- back into the variadic tail. It stands for zero or more of the
    // callee's own, so what comes before it owes the fixed arguments and may
    // then write as many of the variadic ones as it likes: `print("a", ...)`
    // reads as the tail beginning with "a". 'declared' does not count the
    // variadic one (v.func.variadic is kept apart from `params`, the same way
    // self^ is).
    const LhatNode *last_arg = node->v.access.argument;
    while (last_arg != NULL && last_arg->next != NULL) {
        last_arg = last_arg->next;
    }
    bool has_spread = last_arg != NULL && last_arg->kind == LHAT_NODE_SPREAD;
    if (has_spread) {
        given--;  // the spread node itself is not one fixed argument
    }

    // 13.7: a trailing '...' takes any number beyond the declared ones.
    if (has_spread) {
        if (callee->v.func.variadic == NULL) {
            chk_report(c, last_arg, LHAT_CHECK_ERR_NOT_VARIADIC);
        }
        if (given < declared) {
            chk_report(c, node, LHAT_CHECK_ERR_ARITY);
        }
    } else if (given < declared ||
              (given > declared && callee->v.func.variadic == NULL)) {
        // 13.4: a written default does not make a parameter optional. What it
        // fills in is a call site being written by an editor, so a call that
        // reaches here still owes every declared argument.
        chk_report(c, node, LHAT_CHECK_ERR_ARITY);
    }

    size_t taken = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        // 13.7: checked against the element type directly, rather than
        // through the ordinary per-argument loop below -- there is no
        // parameter position for it to line up with.
        if (arg->kind == LHAT_NODE_SPREAD) {
            LhatType *spread = chk_infer(c, arg->v.jump.value);
            size_t positions = lhat_type_tuple_width(spread);
            if (callee->v.func.variadic != NULL && positions > 0) {
                // 13.8改: a tuple spreads too. Unlike a table, whose tail is
                // one element type, each position carries its own -- so each
                // is asked separately whether it fits the variadic's.
                for (size_t i = 0; i < positions; i++) {
                    chk_expect(c, arg, lhat_type_tuple_at(spread, i),
                               callee->v.func.variadic,
                               LHAT_CHECK_ERR_MISMATCH);
                }
            } else if (callee->v.func.variadic != NULL && spread != NULL &&
                       spread->kind == LHAT_TYPE_TABLE &&
                       spread->v.table.variadic != NULL) {
                chk_expect(c, arg, spread->v.table.variadic,
                           callee->v.func.variadic, LHAT_CHECK_ERR_MISMATCH);
            } else if (callee->v.func.variadic != NULL) {
                chk_report(c, arg, LHAT_CHECK_ERR_NOT_VARIADIC);
            }
            break;
        }
        // 3.4改: what this position takes, read before the argument rather
        // than after -- a subroutine literal written here has the callee's
        // written signature standing beside it, and takes the parameters it
        // has none written on from there. The receiver's position is not one
        // of these (14.4), so nothing is expected there.
        LhatType *wanted =
            skip > 0 ? NULL
                     : (param != NULL ? param->type : callee->v.func.variadic);
        // 3.4改: read back where the seeding above already inferred it.
        LhatType *actual;
        if (seeded && taken < given_count) {
            actual = given_types[taken];
        } else {
            LhatType *outer_expected = c->expected_func;
            c->expected_func = wanted;
            actual = chk_infer(c, arg);
            c->expected_func = outer_expected;
        }
        taken++;
        if (skip > 0) {
            skip--;  // the receiver, whose type the call site already knows
            continue;
        }
        // 13.8改: an argument is one value. A tuple in one is what would
        // bring back 13.7's expansion rule (Lua's truncate-except-in-tail-
        // position), and refusing it here is what keeps that rule from
        // arising. Said by name rather than left to the mismatch below,
        // which would report the position's type against the parameter's.
        if (lhat_type_tuple_width(actual) > 0) {
            chk_report(c, arg, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        }
        // 05 の 8.9: the variadic tail collects into a table for an L^ body
        // and erases the width for a host's -- either way the seat cannot
        // say a host value's type, so one is boxed to ride it.
        if (param == NULL && chk_is_hostvalue(actual)) {
            chk_report(c, arg, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        if (wanted != NULL) {
            chk_expect(c, arg, actual, wanted, LHAT_CHECK_ERR_MISMATCH);
        }
        if (param != NULL) {
            param = param->next;
        }
    }

    // 15.5: calling a yieldable procedure answers a coroutine rather than
    // running it. infer_func puts that on the signature as it settles the
    // three types, and every other reader takes it from there; a signature a
    // first pass seeded has none yet, and is assembled here instead.
    if (callee->v.func.yields) {
        return callee->v.func.answers != NULL ? callee->v.func.answers
                                              : coroutine_made_by(c, callee);
    }
    // 13.2: a signature with no result answers no value. That is not a gap in
    // inference, so it is not spelled with a NULL -- 03 の 3.4 kept it apart
    // from nil^, and this is where that stays observable.
    //
    // 15.10: except when the body being checked is calling itself. Its result
    // is still being worked out, and 03 の 3.4 reads the NULL to mean exactly
    // that -- so a self-call has to keep handing the NULL back.
    if (callee->v.func.result == NULL && callee != c->this_type) {
        return chk_simple(c, LHAT_TYPE_NONE);
    }
    return callee->v.func.result;
}

// 02 § 16.3 with 13.8改: what the built-in walk of a table yields -- the
// tuple (K, V), position 1 the key, position 2 the value. A tuple rather
// than a table: a table would cost an allocation every step. Both halves come
// from what the table holds: 14 章 makes it a sequence and a mapping at
// once, so a walk of one that is both hands over keys of either kind.
// 16.3改2: the halves on their own, which keys^ and values^ answer with and
// the pair below is built from. Written as one walk of the members so the
// three readings cannot drift apart.
// 14 章: the sequence half is described by members whose names are digits,
// which 01 の 6 章 keeps a program from writing. Everything else is the keyed
// half, reached by a name -- and by 14 章 that is the same door 't.a' uses.
static bool member_is_positional(const LhatTypeMember *m)
{
    if (m == NULL || m->name_length == 0) {
        return false;
    }
    for (size_t i = 0; i < m->name_length; i++) {
        if (m->name[i] < '0' || m->name[i] > '9') {
            return false;
        }
    }
    return true;
}

static void table_walk_halves(Checker *c, const LhatType *over,
                              LhatType **out_keys, LhatType **out_values)
{
    LhatType *keys = NULL;
    LhatType *values = NULL;
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        for (const LhatTypeMember *m = over->v.table.members; m != NULL;
             m = m->next) {
            bool positional = member_is_positional(m);
            keys = lhat_type_union(
                c->result->types, keys,
                chk_simple(c, positional ? LHAT_TYPE_NUMBER : LHAT_TYPE_STRING));
            values = lhat_type_union(c->result->types, values, m->type);
        }
        // 13.7: an unbounded tail is walked as more positions beyond any
        // listed, every one of the same element type.
        if (over->v.table.variadic != NULL) {
            keys = lhat_type_union(c->result->types, keys,
                                   chk_simple(c, LHAT_TYPE_NUMBER));
            values = lhat_type_union(c->result->types, values,
                                     over->v.table.variadic);
        }
    }

    // 14.10 lets a table carry more than its type lists, so what is written
    // down only ever adds to what a walk may hand over -- it never bounds it.
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

// 02 § 16.3 with 13.8改: what a single name walking a table receives -- the
// values of the sequence half, in order, the keyed half not visited. The
// same loop as 'for^ i from^ 1 to^ the length { t[i] }', so the type is the
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
    return values != NULL ? values : chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// 02 の 14.17: every value carries this, the way 05 の 8.5 gives a coroutine
// its operations -- a member of the value rather than a name a unit has to
// import. An f^: writing a value down changes nothing, which is 11.1's
// reason for keeping an operator pure applied to the same question.
static LhatType *builtin_tostring(Checker *c, const LhatType *target)
{
    LhatType *plain = lhat_type_func(c->result->types, true);
    plain->v.func.takes_self = true;
    plain->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    if (target == NULL || target->kind != LHAT_TYPE_NUMBER) {
        return plain;
    }
    // 14.17: a number^ also takes a format. 14.12 makes the two an
    // intersection, and the counts they allow -- none and one -- keep them
    // from overlapping without the types being looked at.
    LhatType *formatted = lhat_type_func(c->result->types, true);
    formatted->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, formatted,
                        chk_simple(c, LHAT_TYPE_STRING));
    formatted->v.func.result = chk_simple(c, LHAT_TYPE_STRING);
    return lhat_type_intersect(c->result->types, plain, formatted);
}

// 02 の 14.20: the comparison '=' makes, with the error term written down.
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

// 02 の 14.21: the whole number below, above or nearest. One type for the
// three -- which of them it is was settled by the name, so nothing is taken
// and a number^ comes back. An f^: rounding reads a value and makes another.
static LhatType *builtin_whole(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    signature->v.func.result = chk_simple(c, LHAT_TYPE_NUMBER);
    return signature;
}

// 02 の 14.17改2: tostring read backwards, carried by the one value a number^
// can be read out of. The two signatures are shaped exactly as 14.17's and
// made an intersection for the same reason -- 14.12 forbids them overlapping,
// and taking none and taking one keeps them apart without a type being asked
// about.
//
// The answer carries a nil^ arm: a string^ holding no number^ is data, not a
// mistake, so 04 の 11.4's narrowing or 11.7's '??' is what a caller writes. An f^
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

// 02 の 14.19: a run of a string^'s characters. The two forms are shaped as
// 14.17's are and made an intersection for the same reason -- 14.12 forbids
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

// 02 の 14.19改: one character of a string^, which is 14.19's run with both
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

// 15.1改2: what the mutating half of 14.22 are -- an f^ that writes through
// its receiver and nothing else. A p^ calls one anywhere; an f^ only on a
// table its own body made.
static LhatType *mutable_method(Checker *c)
{
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.mutable_self = true;
    return signature;
}

// 14.9 with 14.17改: a table nobody made with a def^. Every name on one is
// the writer's -- vm.c's plain_table asks the same of the value, and the two
// have to answer alike or the checker would allow what the machine refuses.
static bool plain_table_type(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_TABLE &&
           !type->v.table.is_definition && !type->v.table.from_definition;
}

// 14.17改: everywhere but a plain table the two spellings of tostring and
// iterate name one member -- these are the only two words with two
// spellings at all (keys^ and values^ are hat-only, 14.18's line). The
// machine's member_written makes the same crossover.
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

// 14.17改, 16.3改: the hat spelling always reaches the built-in. The bare one
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

// 04 の 11.4 with 01 の 7.1: the '?' forms are one of the two ways to
// reach through a T|nil^ -- the other is 13.11's narrowing. Two halves make
// one: the target is read without its nil^ arm (the strips below), and what
// comes back gains one, since a nil^ target answers nil^ rather than a
// member, a position or a call.
//
// 11.7改2: the two halves are not both per link. The strip is -- it belongs
// to the '?' that was written -- but the arm goes on once, at the end of the
// run the parser marked, so the first '?' guards everything after it.
//
// That is also what keeps a nil^ arriving from somewhere else refused. An
// intermediate link gains nothing here, so an arm seen at one came from the
// member's own written type ('a?.b.c' where b is C|nil^), and reaching
// through it still wants a '?' of its own.
//
// A p^ call answers nothing (13.2), and nothing unions with nil^ into
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

// 04 の 11.4: what a target offers once its nil^ arm is set aside, for the
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

// 04 の 2.3: a union every arm of which is an error. What such a union
// answers is what every arm answers, and 2.3 gives message and cause to
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

LhatType *chk_infer_member(Checker *c, const LhatNode *node)
{
#if LHAT_WITH_RESOLUTIONS
    // 07 の 4 章: only the two answers below have a member record to point at,
    // and the target's own inference runs before either -- so what is left
    // here when this returns is this access's member, or nothing.
    c->resolved_member = NULL;
#endif
    LhatType *target = chk_infer(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node->v.access.argument, &name, &length)) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (target == NULL || target->kind == LHAT_TYPE_UNKNOWN ||
        target->kind == LHAT_TYPE_PENDING) {
        // 03 の 3.4: a parameter read for a member has to carry one, and
        // 14.10's "at least these" is exactly that demand written as a type.
        chk_constrain_member(c, target, name, length);
        // 03 の 3.1・3.5、P6: a target still pending^ makes this member's
        // type pending^ too, not merely unknown^ -- the gap has to survive
        // for strict to see it if it reaches somewhere a concrete type was
        // wanted.
        return chk_simple(c, target != NULL && target->kind == LHAT_TYPE_PENDING
                              ? LHAT_TYPE_PENDING
                              : LHAT_TYPE_UNKNOWN);
    }

    // 04 の 11.4: under relaxed, a T|nil^ value may be referenced as T.
    // The checker steps aside; a nil^ actually arriving meets the machine's
    // own instruction check and panics where it lands, with 11.6's line.
    // strict keeps refusing -- narrowing (isa^, ??, ?.) is the spelling
    // there. Only nil^ is stepped past: a union of two real types still has
    // no one member type to answer with.
    //
    // '?.' is that spelling, so it steps past under strict too -- and
    // unlike relaxed it says so in the answer (nil_propagated).
    if (!c->strict || node->v.access.nil_safe) {
        target = chk_without_nil_arm(c, target);
    }

    // 04 の 2.3: every kind carries message and cause without declaring them
    // -- so they answer however much of the kind is known: a leaf, a
    // declaration's whole set, error^ alone, or a union of any of those
    // (4.2's it^ is the last one wherever a call can fail two ways). A
    // declared field is the leaf's own and wants the narrowing.
    // 14.11: the prototype a definition hangs under self^ -- one canonical
    // instance every construction copies, so its type is the instance type
    // itself. The definition's alone: an instance reading the name finds
    // nothing, exactly as 14.7改 keeps a value member out of its reach.
    if (target->kind == LHAT_TYPE_TABLE && target->v.table.is_definition &&
        target->v.table.instance != NULL && chk_name_is(name, length, "self^")) {
        return target->v.table.instance;
    }

    // 05 の 8.9: the box's two members, the language's own. get answers the
    // value whole -- the registered type, fields and members included, which
    // rides on the box type as `instance`. set writes one of the same tag
    // over the bytes: an effect, so it is a p^ and an f^ body cannot call it
    // (15.1).
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
        // 05 の 8.9改: a registered field reads straight off the box's
        // bytes, as it does off a stack value's -- the read only. A write
        // goes through set(), where the sealed box and 15.1 keep their say.
        const LhatTypeMember *field = chk_find_member(held, name, length);
        if (field != NULL && field->type != NULL &&
            field->type->kind != LHAT_TYPE_FUNC) {
            if (c->writing_to == node) {
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
    if (target->kind == LHAT_TYPE_TABLE ||
        target->kind == LHAT_TYPE_HOSTVALUE) {
        // 05 の 8.9: fields and registered members alike, off the shared
        // member list.
        members = target->v.table.members;
    } else if (target->kind == LHAT_TYPE_ERROR_KIND ||
               target->kind == LHAT_TYPE_ERROR_SET ||
               target->kind == LHAT_TYPE_ERROR || all_error_arms(target)) {
        if (chk_name_is(name, length, "message")) {
            return chk_simple(c, LHAT_TYPE_STRING);
        }
        if (chk_name_is(name, length, "cause")) {
            return lhat_type_union(c->result->types, chk_simple(c, LHAT_TYPE_ERROR),
                                   chk_simple(c, LHAT_TYPE_NIL));
        }
        if (target->kind == LHAT_TYPE_ERROR_KIND) {
            members = target->v.error.fields;
        }
        // 04 の 2.3: the declaration makes types and no values, so a kind of
        // it standing here is a type written where a value was wanted. The
        // name resolves, which is why it reads as a member access at all --
        // and "no such member" would send the writer looking for a spelling
        // mistake instead of for the error^ that 2.5 puts in front.
        if (target->kind == LHAT_TYPE_ERROR_SET &&
            chk_kind_of_set(target, name, length) != NULL) {
            chk_report_named(c, node, LHAT_CHECK_ERR_KIND_AS_VALUE, name,
                             length);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        // ERROR, ERROR_SET and a union of kinds carry no fields of their own
        // -- 2.2's are the leaf's, and reaching one is what 11.4's narrowing
        // is for. The search below finds nothing and the shared tail still
        // answers tostring.
    } else if (target->kind == LHAT_TYPE_CORO) {
        // 05 の 8.5: a coroutine carries these without importing anything.
        // 02 の 12.6 spells dispose(), 15.6 puts resume beside it, and 16.3
        // makes iterate what `in^` asks for.
        //
        // 15.6改: the three that run the body take the body's own kind, so
        // 15.1's calling rule settles who may advance it -- an f^ reaching for
        // a p^ coroutine is an f^ calling a p^, and nothing about yield^ has
        // to be said again. The three that do not run it (done, started,
        // iterate) read state or hand the coroutine back, so they are f^
        // whatever the body is.
        bool advances = target->v.coroutine.is_function;
        // 15.3改: the kind alone would let a body advance one it was handed,
        // whose progress the caller can see afterwards. What a body made is
        // its own, exactly as 15.1改 reads it for a table.
        if (advances && c->in_function &&
            (chk_name_is(name, length, "start") ||
             chk_name_is(name, length, "resume") ||
             chk_name_is(name, length, "dispose")) &&
            !chk_receiver_is_own_coroutine(c, node->v.access.target)) {
            chk_report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
        }
        if (chk_name_is(name, length, "start")) {
            // 15.2: runs the body from the top, for a coroutine that has
            // never been resumed. Takes nothing, since nothing has been
            // yield^ed yet to send a value to. Answers what one turn answers.
            LhatType *signature = lhat_type_func(c->result->types, advances);
            signature->v.func.result = coroutine_answer(c, target);
            return signature;
        }
        if (chk_name_is(name, length, "resume")) {
            // 13.9: one resume sends R in and answers Y|T. Where R is empty
            // there is nothing to send, so it takes no argument at all --
            // 13.2's empty argument side, and a caller writing nil^ there is
            // passing an argument the coroutine has no parameter for.
            // 13.8改: a tuple R is sent as that many arguments --
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
            // 12.7: it answers nothing, which is what 12.5 checks for.
            return lhat_type_func(c->result->types, advances);
        }
        if (builtin_named(name, length, "iterate", false)) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = target;  // 16.3: itself
            return signature;
        }
        // 15.6改: what a resume answers is a union of Y and the result, and
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
            return builtin_tostring(c, target);  // 14.17
        }
        chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    } else {
        // 14.17: nil^, bool^, number^, string^ and the rest hold no members
        // of their own, and this is the one every value answers.
        if (builtin_named(name, length, "tostring", false)) {
            return builtin_tostring(c, target);
        }
        // 14.17改2: and this is the one only a string^ answers. The bare word
        // alone -- 14.18改: a string^ is not a value a writer can add a name
        // to, so there is nothing here for a hat to keep a built-in off, and
        // a second spelling of one member is all it would be.
        if (target->kind == LHAT_TYPE_STRING &&
            chk_name_is(name, length, "tonumber")) {
            return builtin_tonumber(c);
        }
        // 14.20: and this is the one only a number^ answers. The bare word
        // alone, for 14.18改's reason -- nothing can be written on a number^.
        if (target->kind == LHAT_TYPE_NUMBER &&
            chk_name_is(name, length, "eq")) {
            return builtin_eq(c);
        }
        // 14.21: and so are these three, for the same reason.
        if (target->kind == LHAT_TYPE_NUMBER &&
            (chk_name_is(name, length, "floor") ||
             chk_name_is(name, length, "ceil") ||
             chk_name_is(name, length, "round"))) {
            return builtin_whole(c);
        }
        // 14.18: how long it is, and how many bytes that is.
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "length") ||
             chk_name_is(name, length, "len") ||
             chk_name_is(name, length, "size"))) {
            return chk_simple(c, LHAT_TYPE_NUMBER);
        }
        // 14.19: a run of its characters, under any of its three names.
        if (target->kind == LHAT_TYPE_STRING &&
            (chk_name_is(name, length, "substring") ||
             chk_name_is(name, length, "substr") ||
             chk_name_is(name, length, "sub"))) {
            return builtin_substring(c);
        }
        // 14.19改: one character of it.
        if (target->kind == LHAT_TYPE_STRING && chk_name_is(name, length, "at")) {
            return builtin_at(c);
        }
        // 14.19改3: the plain searches. What is not there answers nil^
        // (04 の 11.3) -- never a sentinel -- and the pattern vocabulary
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
            // 15.3改: reading a string changes nothing, so the walk is an
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
        chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 14.17改: everywhere but a plain table the two spellings of tostring
    // and iterate are one member, so where the written one misses, the other
    // spelling is searched before any built-in answers -- what was written
    // wins under either spelling.
    const char *look = name;
    size_t look_length = length;
    for (int pass = 0; pass < 2; pass++) {
        for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
            if (m->name_length == look_length &&
                memcmp(m->name, look, look_length) == 0) {
                // 14.5改: carried by both sides of a composition, so reading
                // it here would be picking one of two for the writer. 14.4's
                // 'let^ f = A.m' is how the side is named.
                if (m->ambiguous) {
                    chk_report_named(c, node, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER,
                                     name, length);
                    return chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
                // 14.7改2: what answered is a seed, not an inferred type --
                // the body it belongs to has not been walked yet.
                // chk_infer_def walks the entries again when this happened,
                // and the walk after it finds a better one here.
                if (m->provisional) {
                    c->read_provisional = true;
                }
#if LHAT_WITH_RESOLUTIONS
                c->resolved_member = m;
#endif
                return m->type;
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

    // 16.3: `in^ e` asks e for the coroutine to walk, and a table answers
    // with one over its keys without anything being written. This comes
    // after the search, not before it, because 16.3 lets a written iterate
    // win -- the same order the machine reads it in. A host type (05 の 8.8)
    // is a table only in how its members are reached -- there is nothing of
    // a table's own behind it to walk, so with none registered there is no
    // built-in to fall back on either.
    if (!(target->kind == LHAT_TYPE_TABLE && target->v.table.nominal) &&
        builtin_named(name, length, "iterate", plain_table_type(target))) {
        // 13.8改: the pair is the tuple (K, V). The walk sends nothing in
        // and ends without a value, which 04 の 11.3 spells nil^ -- so what
        // resume() answers is (K, V)|nil^, the union 13.8改 admits beside
        // an error's, since nil^ is discriminable off the head slot too.
        //
        // 15.3改: the built-in walk changes nothing, so it is an f^ coroutine
        // -- which is what lets 'for^ k, v in^ t' stand inside an f^ body.
        LhatType *walk =
            lhat_type_coro(c->result->types, NULL,
                           chk_table_walk_tuple(c, target), NULL, false, true);
        LhatType *signature = lhat_type_func(c->result->types, true);
        signature->v.func.result = walk;
        return signature;
    }

    // 14.17, the same order and for the same reason: a written tostring is
    // the one that answers, and this is what a table falls back to.
    if (builtin_named(name, length, "tostring", plain_table_type(target))) {
        return builtin_tostring(c, target);
    }

    // 14.18: how long the run is, and how much the table holds altogether.
    // The hat is not optional here even where 14.17改 would let the bare word
    // through: 14 章 reserves new, tostring and dispose on a def^ and these
    // are not among them, so on a table the bare words stay the writer's
    // whatever kind of table it is.
    if (target->kind == LHAT_TYPE_TABLE &&
        (builtin_named(name, length, "length", true) ||
         builtin_named(name, length, "len", true) ||
         builtin_named(name, length, "count", true))) {
        return chk_simple(c, LHAT_TYPE_NUMBER);
    }

    // 16.3改2: the two projections of the same walk. Calls, as iterate^ is
    // and for its reason -- each answers a coroutine of its own, so the
    // parentheses are where one is made rather than a name that quietly
    // makes one every time it is read. 15.3改 makes them f^ coroutines
    // because reading a table changes nothing. The hat is not optional, as
    // in 14.18 just above.
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

    // 14.22: the table's own operations, on a plain table alone -- a def^'s
    // names are the writer's and a host type's are the library's. The hat is
    // not optional, as in 14.18. E is the sequence half's element type
    // (chk_table_element_type), which is what makes 't.sort^(f^ a, b { … })'
    // read its parameters off the receiver through 3.4改's expectation.
    if (plain_table_type(target) && !target->v.table.nominal) {
        LhatType *element = chk_table_element_type(c, target);
        LhatType *self_type = target;
        LhatType *elem_or_nil = lhat_type_union(
            c->result->types, element, chk_simple(c, LHAT_TYPE_NIL));
        LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);

        // join^ -- 11.2's word already means "one string out of pieces".
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
        // range's, which the type does not say (14.10's unbounded tail).
        if (builtin_named(name, length, "slice", true)) {
            LhatType *cut = lhat_type_table(c->result->types);
            cut->v.table.variadic = element;
            LhatType *from = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, from, number);
            from->v.func.result = cut;
            from->v.func.answers_fresh = true;  // 15.1改3
            LhatType *range = lhat_type_func(c->result->types, true);
            lhat_type_add_param(c->result->types, range, number);
            lhat_type_add_param(c->result->types, range, number);
            range->v.func.result = cut;
            range->v.func.answers_fresh = true;
            return lhat_type_intersect(c->result->types, from, range);
        }
        // clone^ -- the shallow copy by name, or each value through a
        // written policy (14.22). The policy is f^any^ -> any^; on purpose,
        // not f^V -> V;: it decides the depth itself, and the V one level
        // down is not the V up here -- 'x.clone^(this^)' has to fit its own
        // signature at every level, which without generics only any^ can
        // say. Inside, 13.11's narrowing is how a policy tells its cases
        // apart, which is what a policy is. Both arms promise a fresh
        // answer (15.1改3) -- an f^ clones what arrived, then mends its own
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
            with_policy->v.func.result = self_type;
            with_policy->v.func.answers_fresh = true;
            return lhat_type_intersect(c->result->types, bare, with_policy);
        }
        // 15.1改2: the mutating half write through the receiver alone, so
        // they are f^mutable^self^ -- an f^ body calls them on what it made
        // itself, a p^ anywhere.
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
        // 11.9's three-way answer is the comparison's shape here too.
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

    chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// 04 の 11.3 with 14 章: what a key of this kind reaches. A number^ names a
// position, a string^ names a member -- and 14 章 makes that the same door
// 't.a' goes through, so the two halves answer separate questions. A key
// whose own type is not decided may be either, which is the safe reading
// rather than the narrow one: 3.5 leaves a gap to the machine, and picking
// one half here would be claiming to know which.
//
// NULL when the type declares nothing a key of that kind could reach. There
// is no T for 'T|nil^' to be made of, and 14.10 leaves what an undeclared
// table holds unsaid -- so nothing is claimed. A bool^ key lands here too:
// what it reaches is in the hash part, which no table type describes.
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
    // 13.7: an unbounded tail is more positions beyond any listed, so a key
    // that could name a position could name one of those.
    if (by_position && over->v.table.variadic != NULL) {
        reachable =
            lhat_type_union(c->result->types, reachable, over->v.table.variadic);
    }
    return reachable;
}

// 13.11改 with 14.10: what the type answers for every position a bounded key
// could name, or NULL where the key is not bounded or could leave them. The
// positions have to run from one without a gap -- a value fitting the type
// has each one it declares, and nothing is promised past the first it does
// not.
static LhatType *within_declared_positions(Checker *c, const LhatType *over,
                                           const LhatNode *key)
{
    int64_t lo = 0;
    int64_t hi = 0;
    if (key == NULL || key->next != NULL ||
        !chk_narrowed_bounds(c, key, &lo, &hi) || lo < 1) {
        return NULL;
    }
    LhatType *reached = NULL;
    for (int64_t at = lo; at <= hi; at++) {
        const LhatTypeMember *m = lhat_type_member_at(over, (size_t)at);
        if (m == NULL) {
            return NULL;  // past what the type declares
        }
        reached = lhat_type_union(c->result->types, reached, m->type);
    }
    return reached;
}

// The '[' half of the same. Lifted out of infer's switch so that it reads
// beside infer_member, which it now shares its nil^ handling with.
static LhatType *infer_index(Checker *c, const LhatNode *node)
{
    LhatType *over = chk_infer(c, node->v.access.target);
    LhatType *asked = chk_require_value(c, node->v.access.argument,
                                        chk_infer(c, node->v.access.argument));
    // 05 の 8.9改: a box's key hash is its bytes, and only a sealed box
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
    // 04 の 11.4: as in infer_member -- relaxed steps past a nil^
    // arm, and '?[' steps past it under strict as well.
    if (!c->strict || node->v.access.nil_safe) {
        over = chk_without_nil_arm(c, over);
    }
    // 04 の 11.3: only a table holds keys. A settled type that is not one
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
            case LHAT_TYPE_HOSTVALUE:
            case LHAT_TYPE_HOSTVALUE_BOX:
                chk_report(c, node, LHAT_CHECK_ERR_NOT_INDEXABLE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            default:
                break;
        }
    }
    // A key written out names one position or one member, so the
    // table says what is there. 04 の 11.3: a key that is not there
    // answers nil^ -- but a written one that the type does not
    // mention says nothing, since 14.10 lets a table carry more than
    // it declares.
    const LhatNode *key = node->v.access.argument;
    if (over != NULL && over->kind == LHAT_TYPE_TABLE && key != NULL &&
        key->next == NULL) {
        const LhatTypeMember *found = NULL;
        if (key->kind == LHAT_NODE_INT) {
            found = lhat_type_member_at(over, (size_t)key->v.integer.value);
        } else if (key->kind == LHAT_NODE_STRING) {
            found = chk_find_member(over, c->lexer->strings + key->v.string.offset,
                                    key->v.string.length);
        }
        if (found != NULL) {
            return found->type;
        }
    }
    // 13.11改: a key the branch bounded, standing where the type declares
    // every position it could reach. 14.10 makes a table type "at least
    // these", so a declared position is one a fitting value has -- and a key
    // that cannot leave them cannot find nothing. This is the static half of
    // 11.3 arrived at by another road, so the answer carries no nil^.
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        LhatType *within = within_declared_positions(c, over, key);
        if (within != NULL) {
            return within;
        }
    }
    // 04 の 11.3: a key that did not resolve to one member still has an
    // answer -- what the type says a key of this kind reaches, and nil^ for
    // the keys it says nothing about, since absence is an ordinary result
    // rather than a failure. That union is what 11.3's 'T|nil^' names.
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        LhatType *reachable = reachable_by_key(c, over, asked);
        if (reachable != NULL) {
            return lhat_type_union(c->result->types, reachable,
                                   chk_simple(c, LHAT_TYPE_NIL));
        }
    }
    // Nothing the type says is reachable this way, so there is no T to make
    // 'T|nil^' out of -- 14.10 leaves what an undeclared table holds unsaid,
    // and 3.5 leaves that to the machine.
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

static bool declares_self(Checker *c, const LhatNode *value);
static LhatType *declared_signature(Checker *c, const LhatNode *value);

static LhatType *infer_table(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(c->result->types);

    // 14.4: a method written in the literal reaches the table through its
    // self^ receiver, the same receiver a def^'s method takes -- 8.7改 took
    // the bound name out of the initialiser, and this is the door that
    // stays. The named signatures go in first, annotations only, so a body
    // walked below can call a method declared after it. (The full fixpoint
    // stays def^'s, 14.7改: a member whose type only its body knows is not
    // seeded here, and reading it ahead is what a def^ is for.)
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *seeded = NULL;
        size_t seeded_length = 0;
        if (entry->v.entry.computed || entry->v.entry.value == NULL ||
            entry->v.entry.value->kind != LHAT_NODE_FUNC ||
            !chk_node_name(c, entry->v.entry.key, &seeded, &seeded_length)) {
            continue;
        }
        LhatType *seed = declared_signature(c, entry->v.entry.value);
        if (seed != NULL) {
            lhat_type_add_member(c->result->types, table, seeded,
                                 seeded_length, seed);
        }
    }

    // 02 §14 makes a table a sequence as well as a mapping. The keyed
    // half is described by name; the sequence half by position, counted the
    // way the machine lays it out -- one-based, in the order written.
    size_t position = 0;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        // The receiver scope, exactly as chk_infer_def pushes it: self^ is
        // the literal's own type, which is still growing but is the one
        // object -- by the time anything calls the method, it is whole.
        Scope *outer = c->scope;
        Scope receiver;
        bool method = declares_self(c, entry->v.entry.value);
        if (method) {
            receiver.bindings = NULL;
            receiver.tail = NULL;
            receiver.parent = outer;
            receiver.transparent = false;
            Binding *bound =
                chk_scope_add(&receiver, "self^", 5, table, node->offset);
            if (bound != NULL) {
                bound->reached = true;
            }
            c->scope = &receiver;
        }
        LhatType *value = chk_require_value(c, entry->v.entry.value,
                                            chk_infer(c, entry->v.entry.value));
        if (method) {
            c->scope = outer;
            chk_scope_dispose(&receiver);
        }

        // 05 の 8.9: a table lives on the heap and a host value does not
        // leave the stack, so no member of one is ever a host value.
        if (chk_is_hostvalue(value)) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        // 13.8改: nor a tuple, for the same reason said of slots -- a member
        // is one value, and a run is several. pack^ is what puts one in a
        // table, and it makes the table itself.
        if (lhat_type_tuple_width(value) > 0) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        }

        // 14.14改: a computed key is an expression, checked like any other.
        // What it names is only known when it is written out -- an integer
        // reaches the sequence half, a string the keyed one, and anything
        // else lands somewhere 14.10 lets the type stay quiet about.
        if (entry->v.entry.computed) {
            const LhatNode *key = entry->v.entry.key;
            LhatType *asked = chk_require_value(c, key, chk_infer(c, key));
            // 04 の 11.3: nil^ is how "not there" is spelled, so it cannot
            // also be a key. The machine reports one that turns out to be
            // nil^; a key that can only ever be one is decided here. A NaN
            // is the other refusal, and stays the machine's -- nothing in
            // 14.8's one number type tells them apart.
            if (asked != NULL && asked->kind != LHAT_TYPE_UNKNOWN &&
                asked->kind != LHAT_TYPE_PENDING &&
                lhat_type_conforms(asked, chk_simple(c, LHAT_TYPE_NIL))) {
                chk_report(c, key, LHAT_CHECK_ERR_BAD_KEY);
            }
            // 05 の 8.9改: a literal's key is stored, so only a sealed box
            // holds its hash still -- and a bare host value does not fit a
            // key's one slot at all.
            if (asked != NULL && asked->kind == LHAT_TYPE_HOSTVALUE_BOX &&
                !asked->v.table.sealed) {
                chk_report(c, key, LHAT_CHECK_ERR_MUTABLE_KEY);
            }
            if (chk_is_hostvalue(asked)) {
                chk_report(c, key, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            if (key != NULL && key->kind == LHAT_NODE_INT) {
                lhat_type_add_index_member(c->result->types, table,
                                           (size_t)key->v.integer.value, value);
            } else if (key != NULL && key->kind == LHAT_NODE_STRING) {
                lhat_type_add_member(c->result->types, table,
                                     c->lexer->strings + key->v.string.offset,
                                     key->v.string.length, value);
            }
            continue;
        }

        const char *name = NULL;
        size_t length = 0;
        if (chk_node_name(c, entry->v.entry.key, &name, &length)) {
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
            lhat_type_add_index_member(c->result->types, table, ++position,
                                       value);
        }
    }
    return table;
}

// 15.2: folds one more yield^/await^ site into the body's running Y or R.
// The first site fixes it; every later one has to agree, or the body is
// mixing yields the way 13.9 does not allow.
void chk_unify_yield(Checker *c, const LhatNode *at, LhatType **slot,
                     LhatType *candidate)
{
    if (candidate == NULL || candidate->kind == LHAT_TYPE_UNKNOWN ||
        candidate->kind == LHAT_TYPE_PENDING) {
        // 13.11: UNKNOWN carries no information, so there is nothing here to
        // agree or disagree with.
        return;
    }
    // 05 の 8.9改: a host value rides a yield^ whole -- one seat, its full
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

// 03 の 3.4: the result type is inferred from return^ unless it is written,
// and a subroutine that calls itself has to write it.
// 14.15改: the shape a subroutine literal declares, read off its annotations
// alone. What super^ stands for inside a pending override^ has to be known
// before the body is walked, and the body is what infer_func would walk.
//
// Answers NULL for anything that is not a subroutine literal, and leaves the
// result NULL when none was written -- 13.2 makes an f^ declare one, so what
// is missing here belongs to a p^, which answers with nothing anyway.
static LhatType *declared_signature(Checker *c, const LhatNode *node)
{
    if (node == NULL || node->kind != LHAT_NODE_FUNC) {
        return NULL;
    }
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    func->v.func.yields = node->v.func.yields;
    func->v.func.answers_fresh = node->v.func.answers_fresh;  // 15.1改3
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            // 15.1改2: the marker travels with the seat. On a p^ it says
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
            // 05 の 8.9: the tail is collected into a table (13.7), and a
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
        c->tuple_allowed = true;  // 13.8改, as above
        func->v.func.result = chk_resolve_type(c, node->v.func.return_type);
    }
    // 15.5: a yieldable's call answers a coroutine, and the seed says so
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

LhatType *chk_infer_func(Checker *c, const LhatNode *node)
{
    // 03 の 3.4改: what expects this literal, taken here and cleared at once
    // -- a body written inside this one is expected by nothing, and reading
    // an outer expectation there would put a type on a parameter no one was
    // talking about.
    LhatType *expected_func = c->expected_func;
    c->expected_func = NULL;
    if (expected_func != NULL && (expected_func->kind != LHAT_TYPE_FUNC ||
                                  expected_func->v.func.is_function !=
                                      node->v.func.is_function)) {
        expected_func = NULL;  // not a signature for this; nothing to take
    }
    const LhatTypeList *expected_param =
        expected_func != NULL ? expected_func->v.func.params : NULL;

    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    // 15.2: whether the body suspends is read off the body, not written.
    func->v.func.yields = node->v.func.yields;
    // 15.13: and whether it promises to capture nothing is written, not read
    // -- what a caller may rely on is what the writer said.
    func->v.func.closed = node->v.func.closed;
    // 15.1改3: as is whether the answer is promised new.
    func->v.func.answers_fresh = node->v.func.answers_fresh;

    // 03 の 3.4: where this body's own parameters begin. A body nested in
    // another leaves the enclosing one's in place behind this mark -- a demand
    // written in here still reaches them, since the value it names came from
    // out there.
    ParamVar *param_mark = c->param_vars;

    Scope body;
    body.bindings = NULL;
    body.tail = NULL;
    body.parent = c->scope;
    body.transparent = false;

    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        // 14.4: the receiver is not an ordinary parameter. Leaving it unbound
        // lets the self^ that infer_def put in scope show through, which is
        // what the body is actually talking about.
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            // 15.1改2: the marker travels with the seat. On a p^ it says
            // nothing a p^ could not already do, so it is not recorded.
            if (param->v.param.mutable_receiver && func->v.func.is_function) {
                func->v.func.mutable_self = true;
            }
            continue;
        }
        // 03 の 3.4改: what the position is expected to take, which stands
        // where a written annotation would. The variadic one is expected by
        // the signature's own tail; the rest walk in step, since neither
        // list counts the receiver (14.4).
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
        // 13.4: a default is what completion and the visual editor write into
        // a call site, so it has to fit the position it will be written into.
        // Read out here, before c->scope becomes the body below -- the
        // expression stands at the call, where none of these parameters is in
        // scope, so it may not name them.
        //
        // Only against a written type. With nothing written the slot is still
        // pending^, and 03 の 3.4 leaves what settles it to the body's demands;
        // a default is not one of them (it is a value the call carries, not a
        // use the body makes).
        // 3.4改: the written type stands beside the default the same way it
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
            // 13.7: '...' inside the body names what was collected. 14.10's
            // form for that is a table whose sequence is unbounded, one
            // element type throughout -- the same shape a written
            // 't^{ ...:T }' names, since both describe the same thing.
            LhatType *collected = lhat_type_table(c->result->types);
            collected->v.table.variadic = element;
            Binding *b = chk_scope_add(&body, "...", 3, collected, param->offset);
            if (b != NULL) {
                b->reached = true;
                b->is_parameter = true;
            }
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);
        // 03 の 3.4: with nothing written, the body is what decides. The
        // binding below and the signature above hold this same object, so
        // settling it once at the end writes through to both. A position an
        // expectation filled is decided already and collects no demands --
        // the body is checked against it, as against a written one.
        if (param->v.param.type == NULL && expected == NULL) {
            chk_push_param_var(c, type, param);
        }

        const char *name = NULL;
        size_t length = 0;
        if (chk_node_name(c, param->v.param.name, &name, &length)) {
            Binding *b = chk_scope_add(&body, name, length, type,
                                       param->v.param.name->offset);
            if (b != NULL) {
                b->reached = true;
                b->is_parameter = true;
            }
        }
    }

    LhatType *declared = NULL;
    if (node->v.func.return_type != NULL) {
        c->tuple_allowed = true;  // 13.8改, as in resolve_func_type
        declared = chk_resolve_type(c, node->v.func.return_type);
    }
    // 03 の 3.4改: a signature written on the binding says what the result is
    // as plainly as one written here does. The parameters above are filled
    // from it for that reason, and the result is not a different question --
    // leaving it out would have the body infer a result that the very
    // signature it was written under already gave.
    if (declared == NULL && expected_func != NULL) {
        declared = expected_func->v.func.result;
    }
    // 15.5 with 13.9: what a call to a yielding body answers is the coroutine,
    // and what the body itself returns is the third of its three types. So a
    // c^{ … } in the result position is unwrapped and the body handed that
    // third type -- the coroutine around it belongs to the caller. Written on
    // the literal or on the binding is the same statement about the same
    // value, so both spellings are read the same way here. Y and R stay
    // 15.2's to settle from the yield^ sites; what was written of them is
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
    Scope *outer_closed_scope = c->closed_scope;
    // 04 の 4.5: a try^ written in this body belongs to this body, whatever
    // block the literal itself was written inside.
    struct CatchFrame *outer_catch_frame = c->catch_frame;
    c->catch_frame = NULL;

    // 02 の 14.11: whether this literal is the written new being walked. Only
    // its own statements are a constructor's -- a literal nested inside it is
    // an ordinary body again, which the save/restore below arranges.
    bool constructor = node == c->new_func;
    bool outer_in_new_body = c->in_new_body;
    c->in_new_body = constructor;
    // 15.1改2: whether this body's own receiver seat is mutable.
    bool outer_receiver_mutable = c->receiver_mutable;
    c->receiver_mutable = func->v.func.mutable_self;
    // 15.1改3: whether every exit of this body owes something new.
    bool outer_must_answer_fresh = c->must_answer_fresh;
    c->must_answer_fresh = func->v.func.answers_fresh;

    c->scope = &body;
    c->declared_result = declared;
    c->inferred_result = NULL;
    c->saw_self_call = false;
    c->recursive_return = false;
    c->valueless_return = false;
    c->this_type = func;
    // 15.10: the chain this^^ walks. Lives here on the C stack, exactly
    // as long as the body is being checked.
    struct ThisLink this_link = { func, c->this_link };
    c->this_link = &this_link;
    c->in_function = node->v.func.is_function;
    c->body_scope = &body;
    // 15.13: the mark opens a boundary that reaches through every body
    // written inside this one -- a nested literal is inside it too, and what
    // it names from further out would be captured just the same.
    if (node->v.func.closed && c->closed_scope == NULL) {
        c->closed_scope = &body;
    }
    c->deferred++;
    // 15.2: a nested p^{...} starts collecting its own Y/R from scratch, so
    // its yield^ sites never unify with the ones out here.
    c->coroutine_produce = NULL;
    c->coroutine_receive = NULL;
    c->yield_context = YIELD_CTX_NONE;
    c->yield_bound_type = NULL;

    // 01 の 8 章: the body's '{' is the scope its parameters are already in
    // (made just above), so the statements go straight into it rather than
    // opening a second one a '$^' would have to count.
    if (node->v.func.body != NULL &&
        node->v.func.body->kind == LHAT_NODE_BLOCK) {
        chk_check_block_in_scope(c, node->v.func.body);
    } else {
        chk_check_statement(c, node->v.func.body);
    }

    // 03 の 3.4: every demand this body makes is in. Settled before the result
    // is put together, since a return^ of a parameter carries this very object
    // into it.
    chk_settle_param_vars(c, param_mark);

    if (node->v.func.yields) {
        func->v.func.yield_produce = c->coroutine_produce;
        func->v.func.yield_receive = c->coroutine_receive;
    }

    // Reaching the end of the body is an exit that produces no value, and a
    // bare return^ is the same exit written down. Both hand nil^ back at run
    // time, so 03 の 3.4 counts them together. What that means depends on
    // what the subroutine promised.
    bool falls_through = !chk_always_exits(node->v.func.body);
    bool leaves_without_value = falls_through || c->valueless_return;
    func->v.func.ends_without_value = leaves_without_value;

    // 02 の 13.2: a function always has a result. So an f^ with a path that
    // answers nothing has one
    // with nothing to answer with, and no result type would make it right.
    //
    // 15.3改 with 15.5: a yieldable one answers a coroutine, and it answers
    // one whatever the body goes on to do. Reaching the end of the body ends
    // the coroutine (13.9 puts that in the third type); it is not the
    // function failing to answer. So 13.2 is already satisfied here.
    if (leaves_without_value && node->v.func.is_function &&
        !node->v.func.yields && !constructor) {
        // 14.11: a new body is exempt -- it never answers for itself, so
        // reaching its end is the ordinary way out.
        chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    } else if (leaves_without_value && declared != NULL &&
               !lhat_type_conforms(chk_simple(c, LHAT_TYPE_NIL), declared)) {
        // A p^ may leave without a value, but then its result has to admit
        // one. 04 の 11.3 spells that nil^.
        chk_report(c, node, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    }

    if (declared == NULL) {
        // 03 の 3.4: the result is what the exits that do not go through the
        // subroutine itself agree on. Leaving without a value is one of
        // those exits, and 04 の 11.3 already spells "no value" nil^.
        //
        // 02 の 13.2 keeps that apart from a body that returns nothing at
        // all: there the writer never asked for a value, and the signature
        // has a form for it. nil^ joins in only when some other exit does
        // produce one.
        if (leaves_without_value &&
            (c->inferred_result != NULL || c->recursive_return)) {
            // 13.8改: there is no union of a tuple with nil^ to fall into, so
            // every exit of a body answering several values has to carry
            // them. 13.2 asks this of an f^ already; the width is what a
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
        // ever produces a value. 02 の 12.8 and 03 の 5.6 leave no other way
        // out -- no exceptions, no unwinding -- so this is decidable here.
        if (c->recursive_return && c->inferred_result == NULL) {
            chk_report(c, node, LHAT_CHECK_ERR_NEVER_RETURNS);
        }
        func->v.func.result = c->inferred_result;

        // 03 の 3.1: what the body did not decide is reported here rather
        // than left in the signature. A result type is a promise to every
        // caller, so a gap in one hands the gap out at every call -- and
        // where there is no call yet, nobody would ever have asked. The
        // parameters are not read this way (a gap there is a constraint
        // nobody wrote, which is what any^ already means), and relaxed
        // forgives all of it (3.5).
        //
        // A result 3.4改's expectation gave is written rather than inferred,
        // so this branch is not where it lands at all.
        if (c->strict && lhat_type_has_gap(func->v.func.result)) {
            chk_report(c, node, LHAT_CHECK_ERR_RESULT_UNDECIDED);
        }
    }

    // 15.5: what a call to this answers, settled here because this is where
    // the three types 13.9 needs are all known. Everything that asks what a
    // call is worth reads it from the signature rather than assembling its
    // own -- the writers and conformance among them, which is what lets the
    // written-out form ('p^number^ -> c^{ p^nil^ -> number^ -> nil^ };') be
    // read back as the annotation it looks like (05 の 8.7).
    if (node->v.func.yields) {
        func->v.func.answers = coroutine_made_by(c, func);
        // 13.9 with 15.2: R and Y are read off the yield^ sites. Where the
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

    // 14.11: whatever the body does, the member answers the copy the machine
    // made -- so the signature says so, whichever branch above wrote it.
    if (constructor) {
        func->v.func.result = c->self_link != NULL ? c->self_link->type : NULL;
        func->v.func.ends_without_value = false;
    }

    // 15.3改: an f^ coroutine may not leave the body that made it. Reaching
    // the outside is what would make advancing it observable from there, and
    // it reaches out through a table member or a nested signature as readily
    // as through the result itself -- so the whole result type is read rather
    // than its outermost shape. A p^ coroutine needs nothing here: advancing
    // one is a p^ call, which 15.1 already refuses inside an f^.
    if (node->v.func.is_function &&
        chk_mentions_function_coroutine(func->v.func.result, 0)) {
        chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
    }

    c->deferred--;
    c->in_new_body = outer_in_new_body;
    c->receiver_mutable = outer_receiver_mutable;
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
    c->closed_scope = outer_closed_scope;
    c->catch_frame = outer_catch_frame;

    chk_scope_dispose(&body);
    // The compiler reads this back instead of re-deriving the signature from
    // written annotations alone, so a result left to inference (no return^
    // type written) still reaches typeof^ and overload dispatch precisely.
    ((LhatNode *)node)->checked_type = func;
    return func;
}

// 14 章. A definition produces two structures, and 14.7 is what ties them:
// an instance can reach the definition's members, so its type contains them
// as well as the fields the template declares.
//
//   definition : the members, plus a new if none was written (14.11)
//   instance   : those members, plus the template's fields
//
// 14.9 keeps the name out of it. Both are ordinary structures, so 11.3's
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
    // 05 の 8.9: a host value type keeps its registered members in the same
    // list a table keeps its own, so the one search serves both.
    if (table == NULL || (table->kind != LHAT_TYPE_TABLE &&
                          table->kind != LHAT_TYPE_HOSTVALUE)) {
        return NULL;
    }
    return chk_members_search(table->v.table.members, name, length);
}

// 14.12's overload^ puts several signatures under one name, and 14.11's new
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

// 14.11 makes new return an instance, so a definition's own structure is
// where its instance type can be found again. Composition needs it, to carry
// the base's fields into the derived instance.
LhatType *chk_instance_of(const LhatType *definition)
{
    // 05 の 8.8: a registered host type is its own written type -- it has
    // no instance half, and a member the host happened to call "new"
    // (std.math.Vector3.new, say) is an ordinary constructor function, not
    // 14.11's mechanism. Without this, writing the type's name answered
    // with that member's result instead of the type.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.nominal) {
        return NULL;
    }
    // 14.7改: a def^ says outright what its instances carry. Read before new
    // is asked, since the two must not drift -- what new answers is built
    // from this very structure.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.instance != NULL) {
        return definition->v.table.instance;
    }
    const LhatTypeMember *constructor = chk_find_member(definition, "new", 3);
    const LhatType *arm =
        constructor != NULL ? constructor_arm(constructor->type) : NULL;
    return arm != NULL ? arm->v.func.result : NULL;
}

// 14.11 with 14.5: the new a definition gets before anything is written into
// it. The parameters come from the base -- a derived definition is built the
// way the base was -- but the result has to be the derived instance, since
// 14.5 composes to make something new and a constructor answering with the
// base would defeat that. With no base, or a base that carries no signature
// here, it is the one 14.11 gives every definition: no arguments, and the
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

// 02 の 14.11: the leaves a template initialiser may leave on the prototype
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
        // 05 の 8.9: a box is a copyable node -- bake and construction copy
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

// 02 の 14.11: whether this initialiser may sit on the prototype. A table
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
    if (node != NULL && node->kind == LHAT_NODE_TABLE) {
        size_t position = 0;
        for (const LhatNode *entry = node->v.list.items; entry != NULL;
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
                member = lhat_type_member_at(type, ++position);
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

// 02 の 14.12改 with 14.11: what super^ names inside a written new -- the
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
// replaced by 14.12's override^ instead of shadowed by position.
//
// 14.15: `abstract` travels with the type, so writing over a declaration with
// a real one is what fills the hole -- and writing a declaration over a
// declaration leaves it open.
// Answers the member it settled -- the one already there, or the one it
// added -- so that a caller holding the key it was written under can say
// where that key stands (07 の 4 章, chk_member_declared_at).
static LhatTypeMember *set_member_marked(Checker *c, LhatType *table,
                                         const char *name, size_t length,
                                         LhatType *type, bool abstract,
                                         bool pending)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            m->type = type;
            m->abstract = abstract;
            m->pending = pending;
            // 14.7改: whatever the first pass put here, this is the real one.
            m->provisional = false;
            return m;
        }
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->abstract = abstract;
        added->pending = pending;
    }
    return added;
}

// 02 の 14.7改: the first pass's seed. Puts the name there with the signature
// it was written with, so a body checked before it can reach it -- and leaves
// alone anything already there, which is the base's or an earlier entry's and
// is the real thing rather than a seed.
static LhatTypeMember *set_member_provisional(Checker *c, LhatType *table,
                                              const char *name, size_t length,
                                              LhatType *type, bool abstract)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m;
        }
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->provisional = true;
        added->abstract = abstract;
    }
    return added;
}

// 02 の 14.7改2: the next walk's seed. What the last walk inferred stays --
// that is what it learned -- and the mark says it is still not the answer, so
// a body reading it is known to have read ahead again, and 14.12's "a second
// member of this name wants a marker" does not fire on this def^'s own
// entries the way it would against a member that is really there.
static void mark_provisional(LhatType *table, const char *name, size_t length)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            m->provisional = true;
            return;
        }
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

// 14.5改: the name is carried by both sides of a composition and reaches no
// one answer through it. Marked rather than dropped, so an access can say
// what went wrong rather than that the name is not there.
static void mark_ambiguous(LhatType *table, const char *name, size_t length)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            m->ambiguous = true;
            return;
        }
    }
}

// 14.15: whether the definition is still waiting on a composition. 14.11's
// new is what this stands in the way of -- an abstract^ would leave a name
// with nothing under it, and 14.15改's pending override^ would leave a super^
// pointing at nothing.
//
// `field` answers which of the two lists the hole is in, since 14.15改3 gives
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
                *field = !m->pending;  // a wait is 14.15改's, not a field's
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

static void copy_members(Checker *c, LhatType *into, const LhatType *from)
{
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = from->v.table.members; m != NULL;
         m = m->next) {
        // 14.15: a hole in the base is a hole in what is composed onto it
        // until something fills it, and 14.15改's wait carries over too.
        set_member_marked(c, into, m->name, m->name_length, m->type,
                          m->abstract, m->pending);
    }
}

// 14.15改3: which fields a written new gives a value to.
//
// 14.11 builds an instance with `self^{ … }`, and 14.15's declaration on a
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

// 14.12's overlap test, applied to two signatures: is there an argument count
// admissible by both at which no position is separate? A separate position is
// enough to keep them apart, since a call has to satisfy every one.
//
// 13.4 keeps defaults out of a type, so a signature admits one count, or a
// range once it has a '...'. Checking the smallest count they share settles
// it: beyond that at least one side is variadic, and its element type does
// not change with position.
static bool signatures_overlap(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL || a->kind != LHAT_TYPE_FUNC ||
        b->kind != LHAT_TYPE_FUNC) {
        return true;  // nothing here can tell them apart
    }

    // 11.3改: which side the receiver stands on is a position too, and the
    // one a call fixes first. An operator written 'f^self^, rhs' answers a
    // use with its owner on the left and 'f^lhs, self^' one with it on the
    // right; 11.3改's order asks the left operand first and only then the
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

// 14.12. A member of a name the base already uses is an error unless it says
// which of the two things it means, and each says something checkable.
// 14.12: an override^ replaces the one candidate its signature overlaps.
// What a name carries is a single signature or an intersection of them
// (14.12改), so this walks the arms, checks substitutability against the one
// that overlaps, and answers the intersection with that arm swapped out.
//
// Refuses two overlaps -- 14.12 says which one was meant is then undecidable
// -- and none, since there was nothing at that name to replace.
static LhatType *override_one(Checker *c, const LhatNode *entry,
                              LhatType *inherited, LhatType *replacement)
{
    if (inherited == NULL || inherited->kind != LHAT_TYPE_INTERSECT) {
        // One candidate, so it is the one: ordinary conformance, arguments
        // wider and result narrower. The receiver is not in the parameter
        // list (14.4), so 14.12's exemption of self^ needs nothing of its own.
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
        chk_report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        return replacement;
    }
    if (overlaps > 1) {
        // 14.12: widening the arguments may reach two of them, and then
        // which is being replaced is not decidable. Reported as the overlap
        // it is, since that is what the writer has to take apart.
        chk_report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
        return replacement;
    }
    if (!lhat_type_conforms(replacement, overlapped)) {
        chk_report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        return replacement;
    }

    // 03 の 5.11c: which arm this is settles the shape of the group the run
    // builds, so the compiler is told rather than left to put the replacement
    // in front of a group that keeps the arm it replaced. Not a strict-only
    // matter: 03 の 4.2 asks that what runs be the same either way, and the
    // arms a name carries are read by typeof^ (14.16) whichever it is.
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

// `constructor` says this entry is 14.11's new, which 14.12改2 exempts from
// the substitutability an override^ otherwise owes. Nothing can be written
// that takes a definition where another definition is expected -- 05 の 2.2
// and 14.7 make a definition's name mean its instance -- so a new that
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
        // 14.15改: an override^ with nothing yet under the name is a mixin
        // written against a base it has not met. It is not an error -- it
        // says what the composition has to bring, and the member stays
        // pending until something does. 14.11's new is what it stands in
        // the way of; overload^ has no such reading, since adding a way to
        // call something that is not there says nothing.
        if (modifier == LHAT_DEF_OVERLOAD) {
            chk_report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        }
        return replacement;
    }

    // 14.15: a declaration is not a definition of the member, so filling it
    // in is not the collision 14.12 is about -- no marker is wanted. What the
    // declaration does ask is that the value fit the type it wrote.
    if (inherited->abstract) {
        if (modifier != LHAT_DEF_PLAIN) {
            chk_report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        } else if (!lhat_type_conforms(replacement, inherited->type)) {
            chk_report(c, entry, LHAT_CHECK_ERR_MISMATCH);
        }
        return replacement;
    }

    switch (modifier) {
        case LHAT_DEF_PLAIN:
            chk_report(c, entry, LHAT_CHECK_ERR_MEMBER_EXISTS);
            return replacement;

        case LHAT_DEF_OVERRIDE:
            // 14.12改2: the exemption above. The whole member goes, so there
            // is no arm to name and nothing to compare against.
            if (constructor) {
                return replacement;
            }
            // 14.12: what is replaced is the one existing candidate the new
            // signature overlaps. A name that was overloaded carries several,
            // and comparing against all of them at once would refuse every
            // replacement -- no one signature is usable where an intersection
            // of them was.
            return override_one(c, entry, inherited->type, replacement);

        case LHAT_DEF_OVERLOAD: {
            // 14.12: the ban is on one signature overlapping another, so a
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
            // 14.12: an overloaded member is callable both ways, which is
            // what '&' means (14.5).
            return lhat_type_intersect(c->result->types, inherited->type,
                                       replacement);
        }
    }
    return replacement;
}

// 14.5: composition where the right side is a name rather than a def^ literal.
// The literal form is infer_def, which reads the entries and so can see
// 14.12's markers. A name carries only its type, and the markers that made it
// went with its own base -- nothing in it was written against this left side.
//
// So a name shared between the two is the plain collision 14.12 refuses, and
// there is no marker to lift it. It is reported at the '..', which is where
// the writer brought them together.
//
// The compiler flattens the same chain through the def^ registry (14.2), so
// what this builds has to agree with that: every member of both sides.
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right)
{
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    definition->v.table.is_definition = true;
    definition->v.table.instance = instance;  // 14.7改
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    const LhatType *left_instance = chk_instance_of(left);
    const LhatType *right_instance = chk_instance_of(right);

    // new is on both sides whether or not either wrote one (14.11), so it
    // would collide every time. It is rebuilt at the end instead.
    for (const LhatTypeMember *m = left->v.table.members; m != NULL;
         m = m->next) {
        if (chk_name_is(m->name, m->name_length, "new")) {
            continue;
        }
        set_member_marked(c, definition, m->name, m->name_length, m->type,
                          m->abstract, m->pending);
    }
    if (left_instance != NULL) {
        for (const LhatTypeMember *m = left_instance->v.table.members;
             m != NULL; m = m->next) {
            set_member_marked(c, instance, m->name, m->name_length, m->type,
                              m->abstract, m->pending);
        }
    }

    for (const LhatTypeMember *m = right->v.table.members; m != NULL;
         m = m->next) {
        if (chk_name_is(m->name, m->name_length, "new")) {
            continue;
        }
        // 14.15: one side declaring what the other provides is the pairing
        // the declaration exists for, and neither order is a collision. What
        // is provided wins; the hole stays open only while both leave it so.
        const LhatTypeMember *held =
            chk_find_member(definition, m->name, m->name_length);
        if (held != NULL && (held->abstract || m->abstract)) {
            if (m->abstract) {
                continue;  // what is already there is the better answer
            }
        } else if (held != NULL && m->pending) {
            // 14.15改: this is what the pending override^ was waiting for.
            // 14.12's check is the one it would have had at the def^, run
            // here instead because here is where the two finally meet.
            if (!lhat_type_conforms(m->type, held->type)) {
                chk_report(c, node, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
            }
            // Unless what it landed on is waiting too -- stacking two mixins
            // settles neither, and the chain still wants something under
            // them both.
            set_member_marked(c, definition, m->name, m->name_length, m->type,
                              false, held->pending);
            continue;
        } else if (held != NULL) {
            // 14.5改: neither side was written against the other, so neither
            // is the answer. The name stops being reachable through the
            // composition; what each side wrote is still reachable through
            // that side, which 14.4 already spells 'let^ f = A.m'.
            mark_ambiguous(definition, m->name, m->name_length);
            continue;
        }
        set_member_marked(c, definition, m->name, m->name_length, m->type,
                          m->abstract, m->pending);
    }
    if (right_instance != NULL) {
        for (const LhatTypeMember *m = right_instance->v.table.members;
             m != NULL; m = m->next) {
            const LhatTypeMember *held =
                chk_find_member(instance, m->name, m->name_length);
            if (held != NULL && (held->abstract || m->abstract)) {
                if (m->abstract) {
                    continue;
                }
            } else if (held != NULL && m->pending) {
                // The definition side settled it, and reported anything
                // there was to chk_report (14.7 puts a method in both).
                set_member_marked(c, instance, m->name, m->name_length,
                                  m->type, false, held->pending);
                continue;
            } else if (held != NULL) {
                // A method sits in both tables (14.7), and the definition
                // side settled it -- mirror that here.
                const LhatTypeMember *shared =
                    chk_find_member(definition, m->name, m->name_length);
                if (shared != NULL && shared->ambiguous) {
                    mark_ambiguous(instance, m->name, m->name_length);
                    continue;
                }
                if (shared == NULL) {
                    // 14.5改: a field is the one that stays an error. A method
                    // is shared, so 14.4 can still reach either side's; a
                    // field is per-instance and the flattened table holds one,
                    // so dropping it would leave both sides' methods reading
                    // nothing. There is no qualified form to fall back to.
                    chk_report(c, node, LHAT_CHECK_ERR_COMPOSE_COLLIDES);
                }
            }
            set_member_marked(c, instance, m->name, m->name_length, m->type,
                              m->abstract, m->pending);
        }
    }

    // 14.5 composes to make something new, so the constructor has to build
    // the composed instance. The parameters come from the right, which is the
    // more derived of the two -- every way of constructing it (14.12's
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

// 02 の 14.15改2: whether this def^ also writes a value under the name, which
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

// 02 の 14.4: whether this member is a method -- one that wrote self^ among
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

// 02 の 14.7改 with 14.4: whether an instance carries this member. What may be
// reached through one is what is handed a receiver, and a signature says so --
// 11.3改's operator form writes self^ second, which takes_self covers either
// way. Everything else (new, a static member, a value) is the definition's.
//
// 14.12's overloaded name is carried when any arm takes a receiver: which arm
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

// 02 の 14.7改2: what one entry of a def^ left behind, kept from one walk of
// the entries to the next.
//
// `found` and `member` are what the first walk found under the entry's own
// name. A later walk asks 14.12 the same question, but by then the answer has
// been written over -- by the walk itself, and by the re-seeding that hands
// the next walk what the last one learned -- so it is kept here rather than
// looked up again. A copy: the member it was taken from is the one being
// written over.
//
// `inferred` is the type the last walk gave the entry, which is how 03 の
// 3.4改2 tells a walk that learned something from one that only said the same
// again.
typedef struct {
    bool found;
    LhatTypeMember member;
    LhatType *inferred;
} SeenMember;

LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base)
{
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    // 14.5: '..' between two definitions is composition, never a call of an
    // op^.. one of them carries -- this is what tells a definition from an
    // instance of it.
    definition->v.table.is_definition = true;
    // 14.7改: and this is what its instances carry. Held on the definition so
    // that 13.13's Self^ and the writer's self^{ … } section both read one
    // place, rather than going through what new answers.
    definition->v.table.instance = instance;
    // 8.8: and both sides are closed to a member being added afterwards.
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    // 14.5: composition is ordered, and the derived side is written against
    // what the base already provides.
    copy_members(c, definition, base);
    copy_members(c, instance, chk_instance_of(base));

    // 14.11改: the new every definition has is put here, before anything is
    // written -- so a written one is a second member of that name and 14.12
    // asks for its marker. It also replaces the base's, which answered with
    // the base's instance. What it is at run time has not changed: the
    // compiler has always written a real closure under this name.
    set_member(c, definition, "new", 3, constructor_for(c, base, instance));

    // 13.13 with 14.7: writing the name of a definition asks for an instance
    // of it, so Self^ inside one names the instance as well -- the same object
    // self^ is bound to below. A definition written with no binding to take a
    // name from (14.9) has this and nothing else to say its own type with.
    struct SelfLink self_here = { instance, c->self_link };
    c->self_link = &self_here;

    // 14.9: and the name this one is landing in, if check_define found one,
    // means the same instance from here on.
    if (c->def_link != NULL && c->def_link->node == node) {
        c->def_link->instance = instance;
    }

    const LhatNode *template = NULL;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key == NULL) {
            template = entry->v.entry.value;
        }
    }

    // 14.4: class^ names the definition, and every member reaches it -- a
    // static one included, since what it names is there before any instance
    // is. self^ is the other way round and is bound per member below: only
    // one that wrote it among its parameters is handed a receiver, so only
    // there does the name mean anything.
    Scope members;
    members.bindings = NULL;
    members.tail = NULL;
    members.parent = c->scope;
    members.transparent = false;
    Binding *owner = chk_scope_add(&members, "class^", 6, definition, node->offset);
    if (owner != NULL) {
        owner->reached = true;
    }

    Scope *outer = c->scope;
    c->scope = &members;

    // 14.7改: the names first, so the members can reach each other however
    // they are ordered -- 'expr calls term calls factor calls expr' is a ring
    // and no ordering unties it. What can be put down here is what can be
    // read without walking a body: a declaration's written type, and a
    // subroutine's written signature. A member whose type only its value
    // knows is not seeded, which is 03 の 3.4's line -- what is read before
    // it is inferred has to have been written.
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        // 02 の 18.4: a member of a def^ takes one.
        chk_check_annotations(c, entry->v.entry.annotations,
                              LHAT_ANNOTATION_MEMBER);
        if (entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        // A declaration's type is read here and nowhere else -- reading it
        // twice would report whatever is wrong with it twice.
        LhatType *seed = entry->v.entry.declared
                             ? chk_resolve_type(c, entry->v.entry.value)
                             : declared_signature(c, entry->v.entry.value);
        if (seed == NULL) {
            continue;
        }
        chk_member_declared_at(
            c,
            set_member_provisional(c, definition, name, length, seed,
                                   entry->v.entry.declared),
            entry->v.entry.key);
        // 14.7改: an instance carries what may be reached through one, which
        // the signature says. A declaration (14.15) is read the same way --
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
    // members scope: 14.11 evaluates an initialiser once, as the definition
    // is built, where class^ and every member are in place -- self^ is not,
    // since no instance exists yet, and the scope holds no such name.
    if (template != NULL) {
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            // 02 の 18.4: a field of a self^{ … } takes one, which is where
            // an @export-shaped annotation goes.
            chk_check_annotations(c, field->v.entry.annotations,
                                  LHAT_ANNOTATION_FIELD);
            if (!chk_node_name(c, field->v.entry.key, &name, &length)) {
                continue;
            }
            // 14.15: a field the composition has to provide carries its type
            // and no value -- and so no key on the prototype.
            if (field->v.entry.declared) {
                chk_member_declared_at(
                    c,
                    set_member_as(c, instance, name, length,
                                  chk_resolve_type(c, field->v.entry.value),
                                  true),
                    field->v.entry.key);
                continue;
            }
            // 14.6: a type written alongside the value is what the field
            // holds, and the value is measured against it -- the same reading
            // 8.6's 'let^ x : T = v' has, and for the same reason: what a
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
            // 14.11: the value goes on the prototype and every instance
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
            chk_member_declared_at(
                c,
                set_member(c, instance, name, length,
                           written != NULL ? written : actual),
                field->v.entry.key);
        }
    }

    // 03 の 3.4改2: the walk below is one iteration of a least fixpoint. A
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
            // 14.12 asks what was already under this name, and a seed from
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
                // 14.7改2: asking again would answer with what this very walk
                // wrote over the name, so the first walk's answer is kept
                // above and handed back here.
                hidden = &seen[index].member;
            }

            // 14.15: a declaration carries a type and no value, and says the
            // composition has to provide the member. It is not a definition
            // of it, so 14.12 has nothing to check here.
            if (entry->v.entry.declared) {
                const LhatTypeMember *seeded =
                    chk_find_member(definition, name, length);
                LhatType *declared =
                    seeded != NULL && seeded->provisional
                        ? seeded->type
                        : chk_resolve_type(c, entry->v.entry.value);
                if (!chk_is_operator_name(name, length)) {
                    chk_refuse_self_last(c, entry, declared);
                }
                if (hidden != NULL && !hidden->abstract) {
                    // Already provided, so the declaration asks for nothing.
                    chk_report(c, entry, LHAT_CHECK_ERR_ALREADY_PROVIDED);
                }
                // 14.15改2: and the composition is where it is provided.
                // Writing the value here as well leaves the declaration
                // saying nothing -- 14.7改 already lets the members reach
                // each other whatever the order, so there is no forward
                // reference left to declare for.
                if (defined_in_def(c, node, name, length)) {
                    chk_report(c, entry, LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE);
                }
                chk_member_declared_at(
                    c,
                    set_member_as(c, definition, name, length, declared, true),
                    entry->v.entry.key);
                if (chk_takes_receiver(declared)) {  // 14.7改, as above
                    chk_member_declared_at(
                        c,
                        set_member_as(c, instance, name, length, declared,
                                      true),
                        entry->v.entry.key);
                }
                continue;
            }

            // 14.11: a member spelled new and written as a body is the
            // constructor. The machine makes the instance and the body only
            // adjusts it -- so self^ is bound around it the way a method's
            // is, and chk_infer_func reads c->new_func to open the body as
            // one.
            bool constructor_entry =
                chk_name_is(name, length, "new") &&
                entry->v.entry.value != NULL &&
                entry->v.entry.value->kind == LHAT_NODE_FUNC;

            // 14.12改: super^ is a name only inside an override^, and what it
            // names is everything that was under this name -- the whole
            // intersection when 14.12's overload^ put several there, since
            // the write replaces the member as a whole.
            LhatType *outer_super = c->super_type;
            c->super_type = NULL;
            if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                // 14.15改: with nothing under the name yet, the shape super^
                // will have is the one written here. 14.12 has the
                // replacement usable where the original was -- arguments
                // wider, result narrower -- so what is written is admissible
                // wherever the original is, and taking it for super^ cannot
                // promise more than the base gives.
                c->super_type =
                    hidden != NULL
                        ? hidden->type
                        : declared_signature(c, entry->v.entry.value);
                // 14.11: inside a new the name means the hook of the new
                // before it -- the same arguments run against the same
                // receiver, with nothing to answer.
                if (constructor_entry) {
                    c->super_type = hook_signature(c, c->super_type);
                }
            }
            // 14.4: the receiver is this member's, so it is bound around this
            // body and nowhere else. A subroutine written inside the body
            // still reaches it, the way any name in scope is reached (5.4's
            // capture).
            Scope receiver;
            bool method = declares_self(c, entry->v.entry.value) ||
                          constructor_entry;
            if (method) {
                receiver.bindings = NULL;
                receiver.tail = NULL;
                receiver.parent = c->scope;
                receiver.transparent = false;
                Binding *bound = chk_scope_add(&receiver, "self^", 5, instance,
                                               node->offset);
                if (bound != NULL) {
                    bound->reached = true;
                }
                c->scope = &receiver;
            }
            const LhatNode *outer_new = c->new_func;
            if (constructor_entry) {
                c->new_func = entry->v.entry.value;
            }
            LhatType *type = chk_infer(c, entry->v.entry.value);
            c->new_func = outer_new;
            if (method) {
                c->scope = &members;
                chk_scope_dispose(&receiver);
            }
            c->super_type = outer_super;
            // 14.12: two members of one name in a single def^ need a marker
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
            // 14.15改: an override^ that found nothing waits for a
            // composition to bring what it replaces. Until then super^ inside
            // it points at nothing, so 14.11's new has to stay out of reach.
            // Landing on another one that is waiting settles neither.
            bool pending = entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                           (hidden == NULL || hidden->pending);
            chk_member_declared_at(
                c,
                set_member_marked(c, definition, name, length, type, false,
                                  pending),
                entry->v.entry.key);
            // 14.7改: only what is handed a receiver is reachable through an
            // instance. new and a static member stay the definition's.
            if (chk_takes_receiver(type)) {
                chk_member_declared_at(
                    c,
                    set_member_marked(c, instance, name, length, type, false,
                                      pending),
                    entry->v.entry.key);
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
    chk_scope_dispose(&members);

    // 14.15改3: a template field the written new gives a value to is provided,
    // the same as one a composition fills. 14.12改2 makes an override^ new
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
        break;  // 14.12 allows one entry of the name, so there is one to read
    }

    c->self_link = self_here.outer;
    return definition;
}

// 05 の 8.9: the one question every escape rule asks. 8.9改: through a
// union's arms as well -- every one of those rules is about the width a
// place has room for, and 'Vector3|nil^' is as wide as the Vector3 in it
// whichever arm is live. The narrower question (is this type itself a host
// value) is asked by kind where it is really about identity: what members
// hang off it, what box^ takes, what a field write reaches.
bool chk_is_hostvalue(const LhatType *type)
{
    return lhat_type_hostvalue_arm(type) != NULL;
}

static LhatType *infer_node(Checker *c, const LhatNode *node);

// The one door every expression's type leaves through. 05 の 8.9 with 03 の
// 5.11a: the compiler is otherwise type-blind, and a host value changes what
// it has to emit -- how many slots to reserve, how wide a move is -- so every
// expression whose type is one gets that type stamped on its node, the same
// checked_type channel infer_func already uses for a signature. Stamping only
// host values keeps the FUNC and TYPEOF stamps (whose kinds are never
// HOSTVALUE) untouched.
LhatType *chk_infer(Checker *c, const LhatNode *node)
{
    LhatType *type = infer_node(c, node);
    // 8.6.4: the place a '?op=' reads answers what is there. 04 の 11.3 puts
    // a nil^ arm on everything a key reaches, and the '?' spelling is how a
    // writer says the write is skipped rather than that the arm is gone --
    // so the operator above this is asked of the rest, and the compiler emits
    // the test that makes that true.
    if (node != NULL && node == c->nil_safe_place) {
        type = chk_without_nil_arm(c, type);
    }
    // 13.8改: a tuple is the other type the compiler has to size slots by --
    // a call answering one writes a run rather than a slot -- so it is
    // stamped for exactly the reason a host value is. A union carrying a
    // tuple arm ('(K, V)|nil^', a walk's resume) is stamped too: even a
    // discarded call has to reserve the arm's width for the run to land in.
    // 8.9改: and a union carrying a host value arm for the same reason --
    // 'Vector3|nil^' takes the Vector3's slots whichever arm turns up, since
    // the room has to be there before anyone knows which did. Its kind is no
    // more a FUNC or a TYPEOF than HOSTVALUE is, so those stamps stay
    // untouched.
    if (node != NULL &&
        (chk_is_hostvalue(type) || lhat_type_tuple_arm_width(type) > 0)) {
        ((LhatNode *)node)->checked_type = type;
    }
    return type;
}

static LhatType *infer_node(Checker *c, const LhatNode *node)
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
            // 01 の 5.4: a hole holds an ordinary expression, and lands in a
            // string whatever it turns out to be -- which is why the answer
            // here says nothing about it. What goes wrong inside one is still
            // wrong, so each is checked on its own.
            for (const LhatNode *part = node->v.list.items; part != NULL;
                 part = part->next) {
                if (part->kind == LHAT_NODE_INTERP_HOLE) {
                    // 05 の 8.9: a host value stands here like any other.
                    // The hole reads it and keeps nothing, which is what
                    // every other place 8.9 refuses does -- and 14.17's
                    // tostring reaches one the same as it reaches a number^:
                    // the library's registered one, or the built-in writing
                    // the type's name where none was registered.
                    chk_require_value(c, part->v.hole.value,
                                      chk_infer(c, part->v.hole.value));
                }
            }
            return chk_simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_SCOPE:
        case LHAT_NODE_FOCUS: {
            // 13.11: a branch may know more about this path than the binding.
            LhatType *narrowed = chk_narrowed_type(c, node);
            if (narrowed == NULL) {
                return chk_infer_name(c, node);
            }
#if LHAT_WITH_RESOLUTIONS
            // 07 の 4 章: chk_infer_name is what records a name, and a
            // narrowed one never reaches it -- so inside the branch that
            // knows most about it, a name was the one thing no tool could
            // say anything about. It is still the same binding: 8.9's word
            // and 13.1's declaration are the binding's to answer, and only
            // the type is the branch's.
            //
            // The binding is looked up rather than reported on: an
            // undefined name has no narrowing to reach this with, since
            // what put one here read the name first. A spelling with no
            // binding to find -- 8.2's $^, 16.3's focus -- keeps the type
            // and nothing else, which is what a member already does.
            const char *name = NULL;
            size_t length = 0;
            Binding *b = node->kind == LHAT_NODE_IDENT &&
                                 chk_node_name(c, node, &name, &length)
                             ? chk_scope_find(c->scope, name, length, NULL)
                             : NULL;
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
            // 14.11: reached as an expression this is the construction
            // notation, and construction is a written new's alone -- the
            // machine has just made the copy and nothing else holds it yet.
            // A method that wants to change its receiver writes the fields
            // one at a time, where 14.4's rules can see each write.
            if (!c->in_new_body) {
                chk_report(c, node, LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW);
            }
            // The fields are checked against the instance either way. new
            // is bound over self^ (chk_infer_def does this for the
            // constructor entry); the self_link arm keeps a refused stray
            // measured against the definition it sits in.
            Binding *receiver = chk_scope_find(c->scope, "self^", 5, NULL);
            LhatType *instance = receiver != NULL      ? receiver->type
                                 : c->self_link != NULL ? c->self_link->type
                                                        : NULL;
            for (const LhatNode *field = node->v.list.items; field != NULL;
                 field = field->next) {
                LhatType *value = chk_infer(c, field->v.entry.value);
                // 05 の 8.9: an instance is a table, so its fields refuse a
                // host value the way any table member does.
                if (chk_is_hostvalue(value)) {
                    chk_report(c, field->v.entry.value,
                               LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
                }
                // 14.11: the template says what a field holds, so what is
                // written here is held to it -- which is also where 03 の
                // 3.4 reads the type of an unannotated parameter handed
                // straight into a field. A name the template does not have
                // is not a field at all: 14.7 closes an instance to members
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
                    chk_report_named(c, field->v.entry.key,
                                     LHAT_CHECK_ERR_NO_MEMBER, name, length);
                    continue;
                }
                chk_expect(c, field->v.entry.value, value, held->type,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            return instance != NULL ? instance
                                    : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_UNARY: {
            LhatType *operand = chk_infer(c, node->v.unary.operand);
            // 11.7改2: 'x?' is '!(x isa^ nil^)' written short, so it
            // asks nothing of its operand -- every value either is nil^ or
            // is not. A type with no nil^ arm answers true^ always, which is
            // let through rather than reported: 11.7改 takes the same posture
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
            // 11.8改: 14.8 gives number^ the negation, and a type that wrote
            // its own is asked only where that does not reach -- the order the
            // machine takes, where NEG handles its own type and comes to the
            // member for everything else.
            LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);
            if (!lhat_type_conforms(operand, number)) {
                LhatType *own = infer_unary_operator(c, operand);
                if (own != NULL) {
                    return own;
                }
            }
            // 04 の 11.3: '-t[i]' is the same mistake the binary ones make,
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

        // 11.5 の (5): 'a < b < c' is '(a < b) and^ (b < c)', so every link
        // is asked what a comparison written on its own is asked. The operand
        // two links share is inferred once, the way it is evaluated once.
        //
        // 13.11: an isa^ link takes a type on the right, and what it tests is
        // the operand standing to its left -- a type is not a value the next
        // link could compare against, so it does not take that place. 'a < b
        // isa^ number^ < c' asks about b three times over.
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
                if (op == LHAT_OP_ISA) {
                    LhatType *asked = chk_resolve_type(c, operand);
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

        // 04 の 11.4: the second half of the '?' forms. Each of the
        // three reads its target with the nil^ arm set aside; what comes back
        // gains one here, since an absent target answers nil^ rather than a
        // member, a position or a call. A narrowed path never reaches
        // nil_propagated: 13.11's `narrowable` refuses a '?.' path outright.
        case LHAT_NODE_CALL:
            return nil_propagated(c, node, chk_infer_call(c, node));

        case LHAT_NODE_MEMBER: {
            LhatType *narrowed = chk_narrowed_type(c, node);
            LhatType *answer =
                narrowed != NULL
                    ? narrowed
                    : nil_propagated(c, node, chk_infer_member(c, node));
#if LHAT_WITH_RESOLUTIONS
            // 07 の 4 章: recorded here rather than inside chk_infer_member,
            // which answers from a dozen places -- the built-in members of a
            // coroutine (15.6改), an error's own two (04 の 2.3), a
            // definition's, a table's. What every one of them has in common
            // is that it came back through here.
            // A narrowed path answered without a lookup (13.11), so the
            // member left over is whatever ran before it -- not this one.
            chk_record_member_resolution(c, node->v.access.argument, answer,
                                         narrowed != NULL ? NULL
                                                          : c->resolved_member);
#endif
            return answer;
        }

        case LHAT_NODE_INDEX:
            return nil_propagated(c, node, infer_index(c, node));

        // 11.6: sound rather than a bare relabelling -- 14.12's
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
            if (lhat_type_disjoint(actual, wanted)) {
                chk_report(c, node, LHAT_CHECK_ERR_AS_IMPOSSIBLE);
            }
            return wanted;
        }

        case LHAT_NODE_FUNC:
            return chk_infer_func(c, node);

        // 15.2: what a yield^ answers (R) has to be fixed by whatever binds
        // it directly, since the value it carries out (Y) is only half of
        // what the expression is. check_define and the bare-statement case
        // in check_statement are the only two places that set yield_context
        // to anything other than NONE, and only for the yield^ they
        // themselves are looking at -- chk_infer() clears it immediately below
        // so it can never leak into node->v.jump.value.
        case LHAT_NODE_YIELD: {
            enum YieldContext ctx = c->yield_context;
            LhatType *bound = c->yield_bound_type;
            c->yield_context = YIELD_CTX_NONE;
            c->yield_bound_type = NULL;

            // 13.8改: 'yield^ a, b' answers a tuple, exactly as a return^ of
            // several values does. What comes back is R, sized on its own
            // (13.9 seats R and Y apart), so the binding below reads it the
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

        // 15.8: the value is the inner coroutine's return value, and the
        // right side has to be a coroutine -- there is nothing else to
        // delegate to. 15.2: whatever it yields passes through as this
        // body's own Y/R, same as a yield^ written directly here would.
        case LHAT_NODE_AWAIT: {
            LhatType *inner = chk_infer(c, node->v.jump.value);
            if (inner == NULL || inner->kind == LHAT_TYPE_UNKNOWN ||
                inner->kind == LHAT_TYPE_PENDING) {
                // 03 の 3.1・3.5、P6: delegating to a still-pending^ inner
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
            // 15.8 with 15.3改: delegating runs the inner body -- that is what
            // makes its yield^ reach out here -- so it advances the coroutine
            // as surely as resume() does, and the same two questions apply.
            // Asked here rather than left to 15.1, since no call of start()
            // or resume() is written for the rule to catch.
            if (c->in_function && !inner->v.coroutine.is_function) {
                chk_report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
            } else if (c->in_function &&
                       !chk_receiver_is_own_coroutine(c, node->v.jump.value)) {
                chk_report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
            }
            chk_unify_yield(c, node, &c->coroutine_produce, inner->v.coroutine.produce);
            chk_unify_yield(c, node, &c->coroutine_receive, inner->v.coroutine.receive);
            // 13.9: a coroutine that cannot end never reaches a last resume,
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
            // 04 の 5.3: the errors this lets through have to be ones the
            // enclosing subroutine may return, and the report belongs here
            // rather than at the caller.
            LhatType *value = chk_infer(c, node->v.jump.value);
            LhatType *error = chk_simple(c, LHAT_TYPE_ERROR);
            if (!chk_can_be(value, error)) {
                chk_report(c, node, LHAT_CHECK_ERR_CANNOT_FAIL);
            } else if (c->catch_frame != NULL) {
                // 04 の 4.5: a try^{ } stands between this and the caller.
                // What leaves here reaches its arms, and 5.3 is asked of
                // whatever they do not take, where the block closes.
                LhatType *escaping = chk_only(c, value, error);
                if (escaping != NULL) {
                    c->catch_frame->caught =
                        lhat_type_union(c->result->types,
                                        c->catch_frame->caught, escaping);
                }
            } else {
                chk_error_leaves(c, node, chk_only(c, value, error));
            }
            return chk_without(c, value, error);
        }

        // 02 の 14.16: the operand is still checked -- an error inside it is
        // still an error -- but what the operand's own type turns out to be
        // plays no part in typeof^'s own type, which is the uniform TypeInfo
        // carrier regardless. The descriptive payload is filled in at run
        // time by reflect_type reading the actual value, unless 03 の 5.11a's
        // narrow exception applies -- kept here on the node itself for
        // compile_expression to read back.
        case LHAT_NODE_TYPEOF: {
            LhatType *operand = chk_infer(c, node->v.jump.value);
            ((LhatNode *)node)->checked_type = operand;
            return chk_typeinfo_type(c);
        }

        case LHAT_NODE_IF_EXPR: {
            // The same shape as the statement form: each clause sees what the
            // earlier conditions ruled out, which is what makes a chain over
            // a union exhaustive (04 の 7 章).
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
            // 17.2: the expression form of a match. The subject is a binding
            // like any other focus, so it needs the scope 16.1 implies, and
            // the body is already the if-chain of 17.9.
            Scope scope;
            scope.bindings = NULL;
            scope.tail = NULL;
            scope.parent = c->scope;
            scope.transparent = false;

            Scope *outer = c->scope;
            c->scope = &scope;
            chk_check_statements(c, node->v.loop.focus);
            LhatType *result = chk_infer(c, node->v.loop.body);
            c->scope = outer;
            chk_scope_dispose(&scope);
            return result;
        }

        case LHAT_NODE_REQUIRE: {
            // 05 の 6.1: the checker follows this. 5.2 already had the parser
            // insist the path be written out, so there is text here to hand
            // the resolver rather than an expression to evaluate.
            const LhatNode *path = node->v.jump.value;
            if (path == NULL || c->require.resolve == NULL) {
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            const char *module_name = NULL;
            LhatType *exports = c->require.resolve(
                c->require.context, c->lexer->strings + path->v.string.offset,
                path->v.string.length, &module_name);
            if (exports == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 5.3: running the unit is what registers it, and this is where
            // it runs -- so L^.modules can be said to hold it from here on.
            chk_register_module_type(c, module_name, exports);
            return exports;
        }

        // 05 の 8.7: names a module the host registered, rather than a file.
        case LHAT_NODE_IMPORT: {
            LhatType *module = chk_hosted_module(c, node->v.jump.value);
            if (module == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_NOT_HOSTED);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return module;
        }

        case LHAT_NODE_ERROR_NEW: {
            // 04 の 2.5: the kind is written into the construction, and 2.3
            // makes it the type of the result.
            LhatType *kind = chk_resolve_type(c, node->v.named.name);
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
                // declared (2.3); the rest have to have been.
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
                    chk_report_named(c, entry, LHAT_CHECK_ERR_NO_MEMBER, name,
                                     length);
                }
            }

            // 04 の 2.5: a field without a default has to be written, since
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

        // 13.8改: '(a, b)' written as a value. Where the tuple came from
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

        // 13.8改: pack^ turns the several values a call answered with into a
        // table a name can hold -- 14.10's positional members, numbered from
        // 1, which is what 't[1]' and a destructuring both read.
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
                lhat_type_add_index_member(c->result->types, packed, i + 1,
                                           lhat_type_tuple_at(source, i));
            }
            return packed;
        }

        // 05 の 8.9: box^ puts a host value in the box the heap can hold.
        // What it takes is exactly a host value -- everything else already
        // lives on the heap and needs no box.
        case LHAT_NODE_BOX: {
            LhatType *held = chk_infer(c, node->v.jump.value);
            // 05 の 8.9: 'constbox^' off a box of either kind is a sealed
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

