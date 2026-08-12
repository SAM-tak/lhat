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

// 11.9: whether one of the two carries a '<=>' taking the other, which
// is what an ordering is read off. 3.5: wherever inference has not decided,
// this says nothing rather than reporting -- the machine asks the same
// question of the actual values.
static bool ordered_pair(Checker *c, LhatType *left, LhatType *right)
{
    if (chk_operator_undecided(left) || chk_operator_undecided(right)) {
        return true;
    }
    LhatType *args[1];
    args[0] = right;
    if (operator_arm(chk_operator_member(c, left, "<=>", 3), args, 1, false) !=
        NULL) {
        return true;
    }
    // 11.3改: or the right operand carries one written as the receiver, which
    // is how a value joins a comparison whose left side is a built-in.
    args[0] = left;
    return operator_arm(chk_operator_member(c, right, "<=>", 3), args, 1,
                        true) != NULL;
}

// 11.5 の (5) with 11.9: one link of a comparison, asked the same way whether
// it stands alone or in a chain. Always answers bool^ -- what may be wrong is
// the pair, never the shape of the answer.
static LhatType *check_comparison(Checker *c, const LhatNode *at, LhatOpKind op,
                                  LhatType *left, LhatType *right)
{
    // 11.9: a '<=>' taking the two is what says how they compare, and
    // it is asked first. One written across two types -- 11.3改 lets the
    // right operand carry it -- relates a pair that 14.12 would otherwise
    // call separate, so the judgement below has to come second.
    bool related = ordered_pair(c, left, right);

    // 14.12's disjointness says whether any value inhabits both. If none
    // does, and nothing says how they compare either, the answer is fixed
    // before the program runs -- a mistake rather than a comparison.
    if (!related && lhat_type_disjoint(left, right)) {
        chk_report(c, at, LHAT_CHECK_ERR_INCOMPARABLE);
        return chk_simple(c, LHAT_TYPE_BOOL);
    }
    // An ordering has nothing but a '<=>' to read. Equality is a different
    // matter and is left alone -- every value is the same as itself or not,
    // whatever it is (14.2), and a '<=>' only refines that for a type that
    // writes one.
    if (!related && op != LHAT_OP_EQ && op != LHAT_OP_NE && op != LHAT_OP_IS) {
        chk_report(c, at, LHAT_CHECK_ERR_NOT_ORDERED);
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
    return arm->v.func.result;
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
    return arm != NULL ? arm->v.func.result : NULL;
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
    // on a parameter is a demand on it. 14.8 makes number^ the one type
    // carrying the arithmetic, which is what makes this readable -- '..' is
    // the one name here that more than one type answers (11.2), so it demands
    // nothing. builtin_operator reads the same distinction the same way.
    //
    // Once demanded, the rest reads as though number^ had been written: the
    // right operand is asked for what number^'s operator takes, which is a
    // demand of its own when that side is a parameter too ('x + y').
    if (chk_param_var_for(c, left) != NULL) {
        if (name[0] == '.') {
            return NULL;
        }
        LhatType *number = chk_simple(c, LHAT_TYPE_NUMBER);
        chk_constrain(c, left, number);
        left = number;
    }

    if (chk_operator_undecided(left)) {
        return NULL;
    }

    LhatType *carrier = chk_operator_member(c, left, name, length);
    // 11.3改: a self^-last one on this side describes the other order --
    // it answers when its owner stands on the RIGHT. Carrying it is not
    // answering here, so the question moves on exactly as if it were absent.
    if (carrier != NULL && carrier->kind == LHAT_TYPE_FUNC &&
        carrier->v.func.self_last) {
        carrier = NULL;
    }
    if (carrier == NULL) {
        // 11.3改: nothing on the left, so the right operand gets the
        // question -- it may have been written as the receiver.
        bool answered = false;
        LhatType *result =
            right_operator(c, name, length, left, right, &answered);
        if (answered) {
            return result;
        }
        chk_report(c, node->v.binary.left, LHAT_CHECK_ERR_NO_OPERATOR);
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
    if (carrier->kind == LHAT_TYPE_INTERSECT) {
        // 3.5: with nothing decided about the right operand there is nothing
        // to choose by. The demand a single arm would make cannot be made
        // either, since the arms disagree about what they want.
        if (chk_operator_undecided(right) || chk_param_var_for(c, right) != NULL) {
            return NULL;
        }
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
                return arm->type->v.func.result;
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
        // The same report the single arm makes below: what is wrong is that
        // nothing here takes this right operand.
        chk_report(c, node->v.binary.right, LHAT_CHECK_ERR_NO_OPERATOR);
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
        chk_expect(c, node->v.binary.right, right, wanted,
                   LHAT_CHECK_ERR_NO_OPERATOR);
    }
    return carrier->v.func.result;
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
            return c->super_type;
        }
        // 05 の 8.6: the machine's own table, there without being imported.
        if (chk_name_is(name, length, "L^")) {
            LhatType *env = chk_environment_type(c);
            return env != NULL ? env : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
    }

    // 03 の 3.4: a subroutine calling itself cannot have its result inferred,
    // since the answer would depend on itself.
    if (c->defining_name != NULL && c->deferred > 0 &&
        length == c->defining_length &&
        memcmp(name, c->defining_name, length) == 0) {
        c->saw_self_call = true;
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

    Binding *b = chk_scope_find(from, name, length, NULL);
    if (b == NULL) {
        // 05 の 8.2: what the host bound before anything ran. Asked after
        // every scope, so a let^ of the same spelling shadows it -- and what
        // it reaches stays readable as L^.<member>, since 8.1 keeps the hat
        // identifier out of the spellings a let^ can make.
        LhatType *bound = initial_binding_type(c, name, length);
        if (bound != NULL) {
            return bound;
        }
        chk_report_named(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 8.7: the name is visible throughout the scope, which is what makes
    // mutual recursion work, but its value only exists once its let^ has run.
    // A subroutine body does not run where it is written, so the rule only
    // applies outside one.
    if (!b->reached && c->deferred == 0) {
        chk_report(c, node, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    }
    // 05 の 8.9: a name bound outside this body reaches the value through a
    // capture, and a capture outlives the frame the slots belong to. The
    // same boundary test 15.1 uses for writes, asked of a read here because
    // for a host value the read is what would build the upvalue.
    if (chk_is_hostvalue(b->type) && c->body_scope != NULL) {
        Scope *found = NULL;
        chk_scope_find(from, name, length, &found);
        if (!chk_scope_within_body(c, found)) {
            chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
    }
    chk_record_resolution(c, node, b);
    return b->type;
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

        case LHAT_OP_AND:
        case LHAT_OP_OR: {
            LhatType *boolean = chk_simple(c, LHAT_TYPE_BOOL);
            chk_expect(c, node->v.binary.left, left, boolean, LHAT_CHECK_ERR_NOT_BOOL);
            chk_expect(c, node->v.binary.right, right, boolean,
                       LHAT_CHECK_ERR_NOT_BOOL);
            return boolean;
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

LhatType *chk_infer_call(Checker *c, const LhatNode *node)
{
    LhatType *callee = chk_infer(c, node->v.access.target);
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
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
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

    // 14.12: an overloaded member is the intersection of its signatures, so
    // calling one means finding the arm that fits. Arguments are inferred
    // once here, since inferring them again would report twice.
    if (callee->kind == LHAT_TYPE_INTERSECT) {
        LhatType *args[LHAT_CHECK_MAX_TRACKED_ARGS];
        size_t tracked = 0;
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            LhatType *type = chk_infer(c, arg);
            if (tracked < LHAT_CHECK_MAX_TRACKED_ARGS) {
                args[tracked++] = type;
            }
        }
        if (tracked < given) {
            return chk_simple(c, LHAT_TYPE_UNKNOWN);  // more than worth tracking
        }

        bool through_member =
            node->v.access.target->kind == LHAT_NODE_MEMBER;
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
                return arm->type->v.func.result;
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

    // 14.15: an instance carries a value under every name its definition
    // holds, so one still only declared has nothing to make. Reported where
    // the construction is written, which is what the writer has to change.
    if (node->v.access.target != NULL &&
        node->v.access.target->kind == LHAT_NODE_MEMBER) {
        const char *called = NULL;
        size_t called_length = 0;
        if (chk_node_name(c, node->v.access.target->v.access.argument, &called,
                          &called_length) &&
                chk_name_is(called, called_length, "new")) {
            LhatType *owner = chk_infer(c, node->v.access.target->v.access.target);
            const LhatTypeMember *hole = chk_unimplemented_member(owner);
            if (hole != NULL) {
                chk_report_named(c, node, LHAT_CHECK_ERR_STILL_ABSTRACT, hole->name,
                                 hole->name_length);
            }
        }
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
        LhatType *actual = chk_infer(c, arg);
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
        LhatType *wanted = param != NULL ? param->type : callee->v.func.variadic;
        if (wanted != NULL) {
            chk_expect(c, arg, actual, wanted, LHAT_CHECK_ERR_MISMATCH);
        }
        if (param != NULL) {
            param = param->next;
        }
    }

    // 15.5: calling a yieldable procedure answers a coroutine rather than
    // running it. 13.9 gives that three types; the middle two come from
    // whatever infer_func found its yield^/yieldall^ sites agreeing on
    // (15.2). A body with no yield^ at all -- only yieldall^ that never
    // ran, or none reached -- leaves them NULL, which nil^ fills the same
    // way an unwritten result does.
    if (callee->v.func.yields) {
        // 13.9: the third type is what the last resume receives. A body
        // with no value-returning return^ hands nil^ back when it ends --
        // but a body that cannot end has no last resume at all, and putting
        // nil^ there would make every consumer narrow away something that
        // never arrives. NULL is how that is spelled, the same way it is for
        // a subroutine that answers nothing.
        LhatType *ends_with = callee->v.func.result;
        if (ends_with == NULL && callee->v.func.ends_without_value) {
            ends_with = chk_simple(c, LHAT_TYPE_NIL);
        }
        // 15.3改: the coroutine carries the kind of the body it came from,
        // which is what decides who may advance it (15.6改).
        return lhat_type_coro(c->result->types,
                              callee->v.func.yield_receive != NULL
                                  ? callee->v.func.yield_receive
                                  : chk_simple(c, LHAT_TYPE_NIL),
                              callee->v.func.yield_produce != NULL
                                  ? callee->v.func.yield_produce
                                  : chk_simple(c, LHAT_TYPE_NIL),
                              ends_with, callee->v.func.is_function);
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
LhatType *chk_table_walk_tuple(Checker *c, const LhatType *over)
{
    LhatType *keys = NULL;
    LhatType *values = NULL;
    if (over != NULL && over->kind == LHAT_TYPE_TABLE) {
        for (const LhatTypeMember *m = over->v.table.members; m != NULL;
             m = m->next) {
            // The sequence half is described by members whose names are
            // digits, which 01 の 6 章 keeps a program from writing.
            bool positional = m->name_length > 0;
            for (size_t i = 0; positional && i < m->name_length; i++) {
                positional = m->name[i] >= '0' && m->name[i] <= '9';
            }
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
    // down only ever adds to what a walk may hand over -- it never bounds
    // it. The width is always two, whatever is known about the halves.
    LhatType *pair = lhat_type_tuple(c->result->types);
    lhat_type_add_position(c->result->types, pair,
                           keys != NULL ? keys : chk_simple(c, LHAT_TYPE_UNKNOWN));
    lhat_type_add_position(c->result->types, pair,
                           values != NULL ? values
                                          : chk_simple(c, LHAT_TYPE_UNKNOWN));
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
        bool positional = m->name_length > 0;
        for (size_t i = 0; positional && i < m->name_length; i++) {
            positional = m->name[i] >= '0' && m->name[i] <= '9';
        }
        if (positional) {
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

// 14.9 with 14.17改: a table nobody made with a def^. Every name on one is
// the writer's -- vm.c's plain_table asks the same of the value, and the two
// have to answer alike or the checker would allow what the machine refuses.
static bool plain_table_type(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_TABLE &&
           !type->v.table.is_definition && !type->v.table.from_definition;
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
// Per link, not per chain: 'a?.b.c' leaves 'a?.b' a T|nil^, which 11.4
// refuses to reach through under strict -- so the writer marks every link,
// 'a?.b?.c'. Kotlin reads the same way. Nothing here has to know where a
// chain begins or ends.
//
// A p^ call answers nothing (13.2), and nothing unions with nil^ into
// something -- a nil-safe call of one produces no value either way.
static LhatType *nil_propagated(Checker *c, const LhatNode *node,
                                LhatType *answer)
{
    if (!node->v.access.nil_safe || answer == NULL ||
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

LhatType *chk_infer_member(Checker *c, const LhatNode *node)
{
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
    // declaration's whole set, or error^ alone (4.2's it^ can be any of the
    // three). A declared field is the leaf's own and wants the narrowing.
    const LhatTypeMember *members = NULL;
    if (target->kind == LHAT_TYPE_TABLE ||
        target->kind == LHAT_TYPE_HOSTVALUE) {
        // 05 の 8.9: fields and registered members alike, off the shared
        // member list.
        members = target->v.table.members;
    } else if (target->kind == LHAT_TYPE_ERROR_KIND ||
               target->kind == LHAT_TYPE_ERROR_SET ||
               target->kind == LHAT_TYPE_ERROR) {
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
        // ERROR and ERROR_SET carry no fields of their own; the search below
        // finds nothing and the shared tail still answers tostring.
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
            // yield^ed yet to send a value to. Answers the same union a
            // resume does -- which is the yield type alone when the third
            // type is absent, since a coroutine that cannot end never
            // answers with one (13.9).
            LhatType *signature = lhat_type_func(c->result->types, advances);
            signature->v.func.result =
                lhat_type_union(c->result->types, target->v.coroutine.produce,
                                target->v.coroutine.result);
            return signature;
        }
        if (chk_name_is(name, length, "resume")) {
            // 13.9: what a resume answers is the union of what the coroutine
            // yields and what it returns -- telling the two apart is what
            // done() does (15.6改). 15.2: R is now one fixed type, so resume
            // takes exactly one argument of it -- start() is what a fresh
            // coroutine is resumed with instead of a sentinel "no argument"
            // call. An absent third type leaves the yield type alone.
            LhatType *answer =
                lhat_type_union(c->result->types, target->v.coroutine.produce,
                                target->v.coroutine.result);
            LhatType *signature = lhat_type_func(c->result->types, advances);
            lhat_type_add_param(c->result->types, signature,
                                target->v.coroutine.receive);
            signature->v.func.result = answer;
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
        // 14.17改2: and this is the one only a string^ answers.
        if (target->kind == LHAT_TYPE_STRING &&
            builtin_named(name, length, "tonumber", false)) {
            return builtin_tonumber(c);
        }
        // 14.18: how long it is, and how much of it there is. Always the hat
        // spelling -- a string^ has no names of its own to take, but the
        // spelling is one across every value the way 14.17's is.
        if (target->kind == LHAT_TYPE_STRING &&
            (builtin_named(name, length, "length", true) ||
             builtin_named(name, length, "len", true) ||
             builtin_named(name, length, "size", true))) {
            return chk_simple(c, LHAT_TYPE_NUMBER);
        }
        chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            // 14.5改: carried by both sides of a composition, so reading it
            // here would be picking one of two for the writer. 14.4's
            // 'let^ f = A.m' is how the side is named.
            if (m->ambiguous) {
                chk_report_named(c, node, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER, name,
                                 length);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return m->type;
        }
    }

    // 16.3: `in^ e` asks e for the coroutine to walk, and a table answers
    // with one over its keys without anything being written. This comes
    // after the search, not before it, because 16.3 lets a written iterate
    // win -- the same order the machine reads it in.
    if (builtin_named(name, length, "iterate", plain_table_type(target))) {
        // 13.8改: the pair is the tuple (K, V). The walk sends nothing in
        // and ends without a value, which 04 の 11.3 spells nil^ -- so what
        // resume() answers is (K, V)|nil^, the union 13.8改 admits beside
        // an error's, since nil^ is discriminable off the head slot too.
        //
        // 15.3改: the built-in walk changes nothing, so it is an f^ coroutine
        // -- which is what lets 'for^ k, v in^ t' stand inside an f^ body.
        LhatType *walk = lhat_type_coro(c->result->types,
                                        chk_simple(c, LHAT_TYPE_NIL),
                                        chk_table_walk_tuple(c, target),
                                        chk_simple(c, LHAT_TYPE_NIL), true);
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
    // through: these are the words a writer reaches for first, so on a table
    // the bare ones stay the writer's whatever kind of table it is.
    if (target->kind == LHAT_TYPE_TABLE &&
        (builtin_named(name, length, "length", true) ||
         builtin_named(name, length, "len", true) ||
         builtin_named(name, length, "count", true))) {
        return chk_simple(c, LHAT_TYPE_NUMBER);
    }

    chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// The '[' half of the same. Lifted out of infer's switch so that it reads
// beside infer_member, which it now shares its nil^ handling with.
static LhatType *infer_index(Checker *c, const LhatNode *node)
{
    LhatType *over = chk_infer(c, node->v.access.target);
    chk_require_value(c, node->v.access.argument,
                      chk_infer(c, node->v.access.argument));
    // 04 の 11.4: as in infer_member -- relaxed steps past a nil^
    // arm, and '?[' steps past it under strict as well.
    if (!c->strict || node->v.access.nil_safe) {
        over = chk_without_nil_arm(c, over);
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
    // 13.7: every position of an unbounded tail is the one element
    // type, so a key that did not resolve to one specific position
    // above still has an answer -- unlike an ordinary table's
    // members, which need not share one type to union with nil^.
    if (over != NULL && over->kind == LHAT_TYPE_TABLE &&
        over->v.table.variadic != NULL) {
        return lhat_type_union(c->result->types, over->v.table.variadic,
                               chk_simple(c, LHAT_TYPE_NIL));
    }
    // 04 §11.3: a dynamic key may be absent, and absence is not a
    // failure, so nothing narrower than this is safe here.
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

static LhatType *infer_table(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(c->result->types);
    // 02 §14 makes a table a sequence as well as a mapping. The keyed
    // half is described by name; the sequence half by position, counted the
    // way the machine lays it out -- one-based, in the order written.
    size_t position = 0;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        LhatType *value = chk_require_value(c, entry->v.entry.value,
                                            chk_infer(c, entry->v.entry.value));

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
            lhat_type_add_member(c->result->types, table, name, length, value);
            continue;
        }
        if (entry->v.entry.key == NULL) {
            lhat_type_add_index_member(c->result->types, table, ++position,
                                       value);
        }
    }
    return table;
}

// 15.2: folds one more yield^/yieldall^ site into the body's running Y or R.
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
    // 05 の 8.9: what a yield^ carries crosses the frame boundary -- it lands
    // in the resumer's slot, and the suspended registers may be walked over
    // -- so a host value does not ride one. Its locals inside the body are
    // fine: suspension copies every register blindly.
    if (chk_is_hostvalue(candidate)) {
        chk_report(c, at, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        return;
    }
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
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
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
    return func;
}

LhatType *chk_infer_func(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    // 15.2: whether the body suspends is read off the body, not written.
    func->v.func.yields = node->v.func.yields;

    // 03 の 3.4: where this body's own parameters begin. A body nested in
    // another leaves the enclosing one's in place behind this mark -- a demand
    // written in here still reaches them, since the value it names came from
    // out there.
    ParamVar *param_mark = c->param_vars;

    Scope body;
    body.bindings = NULL;
    body.tail = NULL;
    body.parent = c->scope;

    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        // 14.4: the receiver is not an ordinary parameter. Leaving it unbound
        // lets the self^ that infer_def put in scope show through, which is
        // what the body is actually talking about.
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            continue;
        }
        // 05 の 4.3: what leaves the unit is not decided by reading a body.
        if (param->v.param.type == NULL && c->exporting > 0) {
            chk_report(c, param, LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE);
        }
        LhatType *type = param->v.param.type != NULL
                             ? chk_resolve_type(c, param->v.param.type)
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
        LhatType *fallback = chk_infer(c, param->v.param.fallback);
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
            }
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);
        // 03 の 3.4: with nothing written, the body is what decides. The
        // binding below and the signature above hold this same object, so
        // settling it once at the end writes through to both.
        if (param->v.param.type == NULL) {
            chk_push_param_var(c, type);
        }

        const char *name = NULL;
        size_t length = 0;
        if (chk_node_name(c, param->v.param.name, &name, &length)) {
            Binding *b = chk_scope_add(&body, name, length, type,
                                       param->v.param.name->offset);
            if (b != NULL) {
                b->reached = true;
            }
        }
    }

    LhatType *declared = NULL;
    if (node->v.func.return_type != NULL) {
        c->tuple_allowed = true;  // 13.8改, as in resolve_func_type
        declared = chk_resolve_type(c, node->v.func.return_type);
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
        !node->v.func.yields) {
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

// Overwrites rather than appending, so a member the base already had ends up
// replaced by 14.12's override^ instead of shadowed by position.
//
// 14.15: `abstract` travels with the type, so writing over a declaration with
// a real one is what fills the hole -- and writing a declaration over a
// declaration leaves it open.
static void set_member_marked(Checker *c, LhatType *table, const char *name,
                              size_t length, LhatType *type, bool abstract,
                              bool pending)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            m->type = type;
            m->abstract = abstract;
            m->pending = pending;
            return;
        }
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->abstract = abstract;
        added->pending = pending;
    }
}

static void set_member_as(Checker *c, LhatType *table, const char *name,
                          size_t length, LhatType *type, bool abstract)
{
    set_member_marked(c, table, name, length, type, abstract, false);
}

static void set_member(Checker *c, LhatType *table, const char *name,
                       size_t length, LhatType *type)
{
    set_member_marked(c, table, name, length, type, false, false);
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
const LhatTypeMember *chk_unimplemented_member(const LhatType *definition)
{
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
            return m;
        }
    }
    return NULL;
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

LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base)
{
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    // 14.5: '..' between two definitions is composition, never a call of an
    // op^.. one of them carries. 14.7 gives both structures the same members,
    // so this is what tells the definition from an instance of it.
    definition->v.table.is_definition = true;
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

    // The fields first, so a method body sees them through self^.
    if (template != NULL) {
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(c, field->v.entry.key, &name, &length)) {
                continue;
            }
            // 14.15: a field the composition has to provide carries its type
            // and no value. 14.11 would otherwise want an initializer here.
            if (field->v.entry.declared) {
                set_member_as(c, instance, name, length,
                              chk_resolve_type(c, field->v.entry.value), true);
                continue;
            }
            // 14.11: an initializer, evaluated at each construction. Its type
            // is what the field holds.
            set_member(c, instance, name, length,
                       chk_infer(c, field->v.entry.value));
        }
    }

    // 14.4: self^ reaches the instance, class^ the definition. Bound before
    // the members are walked so a body may use either.
    Scope members;
    members.bindings = NULL;
    members.tail = NULL;
    members.parent = c->scope;
    Binding *receiver = chk_scope_add(&members, "self^", 5, instance, node->offset);
    Binding *owner = chk_scope_add(&members, "class^", 6, definition, node->offset);
    if (receiver != NULL) {
        receiver->reached = true;
    }
    if (owner != NULL) {
        owner->reached = true;
    }

    Scope *outer = c->scope;
    c->scope = &members;

    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        const LhatTypeMember *hidden = chk_find_member(definition, name, length);

        // 14.15: a declaration carries a type and no value, and says the
        // composition has to provide the member. It is not a definition of
        // it, so 14.12 has nothing to check here.
        if (entry->v.entry.declared) {
            LhatType *declared = chk_resolve_type(c, entry->v.entry.value);
            if (!chk_is_operator_name(name, length)) {
                chk_refuse_self_last(c, entry, declared);
            }
            if (hidden != NULL && !hidden->abstract) {
                // Already provided, so the declaration asks for nothing.
                chk_report(c, entry, LHAT_CHECK_ERR_ALREADY_PROVIDED);
            }
            set_member_as(c, definition, name, length, declared, true);
            set_member_as(c, instance, name, length, declared, true);
            continue;
        }

        // 14.12改: super^ is a name only inside an override^, and what it
        // names is everything that was under this name -- the whole
        // intersection when 14.12's overload^ put several there, since the
        // write replaces the member as a whole.
        LhatType *outer_super = c->super_type;
        c->super_type = NULL;
        if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
            // 14.15改: with nothing under the name yet, the shape super^ will
            // have is the one written here. 14.12 has the replacement usable
            // where the original was -- arguments wider, result narrower --
            // so what is written is admissible wherever the original is, and
            // taking it for super^ cannot promise more than the base gives.
            c->super_type = hidden != NULL
                                ? hidden->type
                                : declared_signature(c, entry->v.entry.value);
        }
        LhatType *type = chk_infer(c, entry->v.entry.value);
        c->super_type = outer_super;
        // 14.12: two members of one name in a single def^ need a marker too,
        // so what is already there has to include this def^'s earlier entries
        // and not only what the base brought. `definition` is both -- it was
        // copied from the base above and has been accumulating since.
        type = check_same_name(c, entry, hidden, type,
                               chk_name_is(name, length, "new"));
        if (chk_is_operator_name(name, length)) {
            chk_check_operator_shape(c, entry, type, name, length);
        } else {
            chk_refuse_self_last(c, entry, type);
        }
        // 14.15改: an override^ that found nothing waits for a composition to
        // bring what it replaces. Until then super^ inside it points at
        // nothing, so 14.11's new has to stay out of reach. Landing on
        // another one that is waiting settles neither.
        bool pending = entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                       (hidden == NULL || hidden->pending);
        set_member_marked(c, definition, name, length, type, false, pending);
        set_member_marked(c, instance, name, length, type, false, pending);
    }

    c->scope = outer;
    chk_scope_dispose(&members);

    c->self_link = self_here.outer;
    return definition;
}

// 05 の 8.9: the one question every escape rule asks.
bool chk_is_hostvalue(const LhatType *type)
{
    return type != NULL && type->kind == LHAT_TYPE_HOSTVALUE;
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
    // 13.8改: a tuple is the other type the compiler has to size slots by --
    // a call answering one writes a run rather than a slot -- so it is
    // stamped for exactly the reason a host value is. A union carrying a
    // tuple arm ('(K, V)|nil^', a walk's resume) is stamped too: even a
    // discarded call has to reserve the arm's width for the run to land in.
    // Its kind is no more a FUNC or a TYPEOF than HOSTVALUE is, so those
    // stamps stay untouched.
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
            return narrowed != NULL ? narrowed : chk_infer_name(c, node);
        }

        case LHAT_NODE_TABLE:
            return infer_table(c, node);

        case LHAT_NODE_DEF:
            return chk_infer_def(c, node, NULL);

        case LHAT_NODE_SELF_TABLE: {
            // 14.11: in the body of a def^ this declares the fields, and
            // inside new it builds one. Either way it names the instance,
            // which is what self^ is bound to.
            for (const LhatNode *field = node->v.list.items; field != NULL;
                 field = field->next) {
                LhatType *value = chk_infer(c, field->v.entry.value);
                // 05 の 8.9: an instance is a table, so its fields refuse a
                // host value the way any table member does.
                if (chk_is_hostvalue(value)) {
                    chk_report(c, field->v.entry.value,
                               LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
                }
            }
            Binding *receiver = chk_scope_find(c->scope, "self^", 5, NULL);
            return receiver != NULL ? receiver->type
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
            chk_expect(c, node, operand, number, LHAT_CHECK_ERR_NOT_NUMBER);
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
            return narrowed != NULL
                       ? narrowed
                       : nil_propagated(c, node, chk_infer_member(c, node));
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
            // several values does. Only written as a statement, so nothing
            // receives what comes back here.
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
        case LHAT_NODE_YIELD_ALL: {
            LhatType *inner = chk_infer(c, node->v.jump.value);
            if (inner == NULL || inner->kind == LHAT_TYPE_UNKNOWN ||
                inner->kind == LHAT_TYPE_PENDING) {
                // 03 の 3.1・3.5、P6: delegating to a still-pending^ inner
                // expression makes this yieldall^ pending^ too.
                return chk_simple(c, inner != NULL &&
                                          inner->kind == LHAT_TYPE_PENDING
                                      ? LHAT_TYPE_PENDING
                                      : LHAT_TYPE_UNKNOWN);
            }
            if (inner->kind != LHAT_TYPE_CORO) {
                chk_report(c, node, LHAT_CHECK_ERR_NOT_COROUTINE);
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
            // 13.9: a coroutine that cannot end has no return type, so a
            // delegation to one never produces a value either.
            return inner->v.coroutine.result != NULL
                       ? inner->v.coroutine.result
                       : chk_simple(c, LHAT_TYPE_NONE);
        }

        case LHAT_NODE_TRY: {
            // 04 の 5.3: the errors this lets through have to be ones the
            // enclosing subroutine may return, and the report belongs here
            // rather than at the caller.
            LhatType *value = chk_infer(c, node->v.jump.value);
            LhatType *error = chk_simple(c, LHAT_TYPE_ERROR);
            if (!chk_can_be(value, error)) {
                chk_report(c, node, LHAT_CHECK_ERR_CANNOT_FAIL);
            } else if (c->declared_result != NULL) {
                LhatType *escaping = chk_only(c, value, error);
                if (escaping != NULL &&
                    !lhat_type_conforms(escaping, c->declared_result)) {
                    chk_report(c, node, LHAT_CHECK_ERR_TRY_OUTSIDE);
                }
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

        default:
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
}

