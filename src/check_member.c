// L^ (lhat) -- the type checking stage: members, built-in words and indexing.

#include "check_internal.h"
#include "instantiation_internal.h"
#include "rttype.h"

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
        chk_join_table_entry_types(c, over, &keys, &values);
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
LhatType *chk_nil_propagated(Checker *c, const LhatNode *node,
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
bool chk_run_of_names(const LhatNode *node, bool allow_scope)
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
        chk_hostvalue_temporary_write(c, node, node->v.access.target, target);
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
    // Optional access restores its nil^ arm through chk_nil_propagated.
    if (!builtin_named(name, length, "tostring", false)) {
        target = chk_access_target(c, node, target);
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
        if (node != NULL && !chk_run_of_names(node->v.access.target, false)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        LhatType *answer = chk_call_answer(c, target);
        if (answer == NULL || answer->kind == LHAT_TYPE_NONE) {
            chk_report(c, node, LHAT_CHECK_ERR_NO_RESULT_TYPE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        if (chk_runtime_has_unresolved_arguments(answer)) {
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
            signature->v.func.result = chk_coroutine_answer(c, target);
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
            signature->v.func.result = chk_coroutine_answer(c, target);
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
        chk_report_no_member(c, node, target, name, length);
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
        chk_report_no_member(c, node, target, name, length);
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

    chk_report_no_member(c, node, target, name, length);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

bool chk_string_member_key(Checker *c, const LhatNode *key)
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
            const LhatType *arm = chk_operator_arm(getter->type, args, 1, false);
            if (arm == NULL) {
                chk_report(c, node->v.access.argument, LHAT_CHECK_ERR_NO_OPERATOR);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            return lhat_type_func_result(arm);
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
LhatType *chk_infer_index(Checker *c, const LhatNode *node)
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
    over = chk_access_target(c, node, over);
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
        } else if (chk_string_member_key(c, key)) {
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
