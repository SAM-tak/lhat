// L^ (lhat) -- the type checking stage: subroutine literals ([syntax.sub]).

#include "check_internal.h"
#include "instantiation_internal.h"
#include "rttype.h"

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
LhatType *chk_declared_signature(Checker *c, const LhatNode *node)
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
    // What it yields only the body knows, so with no c^{ … } written the
    // answer is a coroutine of unknowns.
    LhatType *written = func->v.func.result;
    if (node->v.func.yields && (written == NULL || written->kind != LHAT_TYPE_CORO)) {
        func->v.func.result = lhat_type_coro(c->result->types, NULL,
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
        chk_retain_instance_failure(c, i, diagnostics);
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

// [syntax.sub.no-contagion] with [syntax.type.coroutine]: the coroutine a call to a yielding body
// makes, which is what its signature's result holds. R and Y are whatever the
// body's yield^/await^ sites agreed on ([syntax.sub.yieldable]); a body none of
// them reached leaves them NULL, and nil^ fills Y the way it fills an unwritten
// result.
static LhatType *coroutine_made(Checker *c, const LhatType *func, LhatType *receive,
                                LhatType *produce, LhatType *result)
{
    // [syntax.type.coroutine]: the third type is what the last resume receives. Where the body
    // ends without a value it is left empty and [syntax.sub.coroutine-members]'s nil^ joins Y|T at
    // the resume instead -- so the nil^ stands where it is really received
    // rather than in the coroutine's own type. A body that cannot end has no
    // last resume at all, which `endless` says: nothing joins Y there, since
    // putting anything in would make every consumer narrow away a value that
    // never arrives.
    bool endless = result == NULL && !func->v.func.ends_without_value;
    // [syntax.sub.yieldable-f]: the coroutine carries the kind of the body it came from, which
    // is what decides who may advance it ([syntax.sub.coroutine-members]).
    //
    // R is left empty when nothing in the body received a yield^: no var^ took
    // one, so there is no value being sent in and a resume takes no argument.
    // Nothing unions R with anything, so there is nothing for a nil^ to be an
    // arm of -- filling one in would be inventing a parameter. Y is not the
    // same: a yield^ with no value really does hand nil^ to the resumer.
    LhatType *coroutine = lhat_type_coro(
        c->result->types, receive, produce != NULL ? produce : chk_simple(c, LHAT_TYPE_NIL),
        result, endless, func->v.func.is_function);
    if (coroutine != NULL) coroutine->source_body = func->source_body;
    return coroutine;
}

LhatType *chk_infer_func(Checker *c, const LhatNode *node)
{
    if (node->v.func.is_template) return chk_infer_template(c, node);
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
    //
    // [syntax.type.coroutine-result]: the result position says what a call answers, and a yielding
    // body's call answers a coroutine -- so anything but a c^{ … } written on the
    // literal is wrong. One that came from the binding is left to the
    // assignment, which reads the call's answer and may well accept it (any^).
    LhatType *written_coroutine = NULL;
    if (node->v.func.yields && declared != NULL) {
        if (declared->kind == LHAT_TYPE_CORO) {
            written_coroutine = declared;
            declared = declared->v.coroutine.result;
        }
        else {
            if (node->v.func.return_type != NULL) {
                chk_report(c, node->v.func.return_type, LHAT_CHECK_ERR_YIELDABLE_RESULT);
            }
            declared = NULL;
        }
    }
    // Until the body settles Y and R, a call of it from inside (this^) answers
    // the coroutine as far as it is known.
    func->v.func.result = node->v.func.yields
                              ? coroutine_made(c, func, NULL, NULL, declared)
                              : declared;

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

    // What the body returns: [syntax.type.coroutine]'s third type where it yields.
    LhatType *result = declared;
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
        result = c->inferred_result;

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
        if (lhat_type_has_gap(result)) {
            chk_report(c, node, LHAT_CHECK_ERR_RESULT_UNDECIDED);
        }
    }

    // [syntax.sub.no-contagion]: what a call to this answers, settled here because this is where
    // the three types [syntax.type.coroutine] needs are all known. Everything that asks what a
    // call is worth reads it from the signature rather than assembling its
    // own -- the writers and conformance among them, which is what lets the
    // written-out form ('p^number^ -> c^{ p^nil^ -> number^ -> nil^ };') be
    // read back as the annotation it looks like ([modules.host.import]).
    func->v.func.result = node->v.func.yields
                              ? coroutine_made(c, func, c->coroutine_receive,
                                               c->coroutine_produce, result)
                              : result;
    // [syntax.type.coroutine] with [syntax.sub.yieldable]: R and Y are read off the yield^ sites. Where the
    // writer put them down as well, the two have to be the same
    // coroutine -- a signature saying something else is not a signature,
    // and one of the two has to move. A signature written on the binding
    // meets the same question at the assignment, which conforms_func
    // asks; one written here has no assignment to meet, so it is asked
    // here.
    if (written_coroutine != NULL &&
        !lhat_type_conforms(func->v.func.result, written_coroutine)) {
        chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_MISMATCH);
    }

    // [syntax.obj.new]: whatever the body does, the member answers the copy the machine
    // made -- so the signature says so, whichever branch above wrote it.
    if (constructor) {
        result = func->v.func.result = c->self_link != NULL ? c->self_link->type : NULL;
        func->v.func.ends_without_value = false;
    }

    // [syntax.sub.yieldable-f]: an f^ coroutine may not leave the body that made it. Reaching
    // the outside is what would make advancing it observable from there, and
    // it reaches out through a table member or a nested signature as readily
    // as through the result itself -- so the whole result type is read rather
    // than its outermost shape. A p^ coroutine needs nothing here: advancing
    // one is a p^ call, which [syntax.sub.kinds] already refuses inside an f^.
    // What a yielding f^ answers is its own coroutine, which is the caller's to
    // run; what must not leave is one its body returns.
    if (node->v.func.is_function && chk_mentions_function_coroutine(result, 0)) {
        chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
    }

    // [errors.kind.local-no-propagate], and the same shape of rule as the one above: a localerror^
    // may not be what a caller receives. Reading the whole settled signature
    // rather than its outermost shape catches it inside a table or a nested
    // one, and reaches a coroutine's Y as well as its T.
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
