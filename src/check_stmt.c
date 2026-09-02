// L^ (lhat) -- the type checking stage: statements.

#include "check_internal.h"

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

// ast.c's target readings (8.8), shared with the compiler.
static const LhatNode *target_name_node(const LhatNode *target)
{
    return lhat_define_target_name(target);
}

// Wider than ast.c's lhat_define_target_root on purpose: a define target's
// grammar never holds an index, but a reassignment's does -- 't[k] := v'
// changes the table exactly as 't.x := v' does, so every question asked of
// a path here has to see both spellings. (This is where 15.1改's origin rule
// once let an indexed write through unasked.)
static const LhatNode *target_root(const LhatNode *target)
{
    const LhatNode *node = lhat_define_target_name(target);
    while (node->kind == LHAT_NODE_MEMBER || node->kind == LHAT_NODE_INDEX) {
        node = node->v.access.target;
    }
    return node;
}

static bool target_is_path(const LhatNode *target)
{
    const LhatNode *name = lhat_define_target_name(target);
    return name->kind == LHAT_NODE_MEMBER || name->kind == LHAT_NODE_INDEX;
}

static const LhatTypeMember *member_named(const LhatType *type,
                                          const char *name, size_t length);

// ast.c's L^ test (05 の 8.6), shared with the compiler.
static bool is_environment(const Checker *c, const LhatNode *node)
{
    return lhat_node_is_environment(node, c->lexer->source->text,
                                    c->lexer->strings);
}

// What L^ carries. vm.c's build_environment makes the values; the two lists
// have to say the same thing.
LhatType *chk_environment_type(Checker *c)
{
    if (c->environment != NULL) {
        return c->environment;
    }
    LhatType *env = lhat_type_table(c->result->types);
    LhatType *modules_type = lhat_type_table(c->result->types);
    // 12.7's shape: a p^ taking nothing and answering nothing. Running the
    // collector is an effect, so it is not an f^.
    LhatType *collectgarbage_type = lhat_type_func(c->result->types, false);
    if (env == NULL || modules_type == NULL || collectgarbage_type == NULL) {
        return NULL;
    }
    // 05 の 8.6: L^ is the machine itself, and the registry inside it is
    // what require^ and import^ write. Both are the machine's to change, not
    // the program's.
    env->v.table.sealed = true;
    modules_type->v.table.sealed = true;
    // And both are reached through rather than held: L^.print, L^.modules.a.
    env->v.table.is_module = true;
    modules_type->v.table.is_module = true;
    // environment.h's one list -- vm.c's build_environment expands the same
    // one for the values.
#define LHAT_ENVIRONMENT_ADD(name, value, type)                     \
    lhat_type_add_member(c->result->types, env, #name,              \
                         sizeof #name - 1, (type));
    LHAT_ENVIRONMENT(LHAT_ENVIRONMENT_ADD)
#undef LHAT_ENVIRONMENT_ADD
    // 05 の 8.6: and whatever the host put here on top of that list.
    if (c->require.globals != NULL) {
        for (const LhatTypeMember *m = c->require.globals->v.table.members;
             m != NULL; m = m->next) {
            lhat_type_add_member(c->result->types, env, m->name,
                                 m->name_length, m->type);
        }
    }
    c->environment = env;
    return env;
}

// 02 の 14.16: the one nominal type every typeof^(...) has, whatever the
// operand's own type is -- the descriptive payload is a runtime concern
// (reflect_type, in vm.c), resolved at the call and not something a second
// checker pass could see (03 の 4.2).
LhatType *chk_typeinfo_type(Checker *c)
{
    if (c->typeinfo_type != NULL) {
        return c->typeinfo_type;
    }
    LhatType *info = lhat_type_table(c->result->types);
    if (info == NULL) {
        return NULL;
    }
    info->v.table.nominal = true;
    lhat_type_add_member(c->result->types, info, "signature", 9,
                         chk_simple(c, LHAT_TYPE_STRING));
    c->typeinfo_type = info;
    return info;
}

// 8.8: everything before the last segment holds the one written after it, so
// it has to be a table. 14 章 fixes what an instance of a def^ carries, which
// is the one kind of table a member cannot be added to.
static LhatType *holds_members(Checker *c, const LhatNode *at, LhatType *type)
{
    if (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING) {
        return NULL;  // already reported, or nothing known to report against
    }
    if (type->kind != LHAT_TYPE_TABLE) {
        chk_report(c, at, LHAT_CHECK_ERR_PATH_NOT_TABLE);
        return NULL;
    }
    if (type->v.table.from_definition) {
        chk_report(c, at, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
        return NULL;
    }
    // 05 の 8.6: the machine's own tables are not grown from here
    // either. Adding a member changes one as much as writing over one does,
    // and 8.8's own walk is what would make the segments on the way.
    if (type->v.table.sealed) {
        chk_report(c, at, LHAT_CHECK_ERR_TABLE_IS_SEALED);
        return NULL;
    }
    return type;
}

// The table a segment of a path names, made where it is not there yet.
// Answers NULL when the path cannot be followed, having reported why.
static LhatType *path_table(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;

    if (node->kind != LHAT_NODE_MEMBER) {
        // 05 の 8.6: L^ is a place too, and the one no scope holds.
        if (is_environment(c, node)) {
            return holds_members(c, node, chk_environment_type(c));
        }
        // The root is a name a scope holds. 8.8 reaches an enclosing binding
        // rather than shadowing it: 'let^ a.b = 1' says where b goes and
        // nothing new about a, so a is only made when there is none.
        if (!chk_node_name(c, node, &name, &length)) {
            return NULL;
        }
        Binding *b = chk_scope_find(c->scope, name, length, NULL);
        if (b == NULL) {
            // collect_bindings puts an ordinary root there before the walk,
            // so what reaches here is a hat identifier that is not L^.
            chk_report_named(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length);
            return NULL;
        }
        if (b->type == NULL || b->type->kind == LHAT_TYPE_UNKNOWN ||
            b->type->kind == LHAT_TYPE_PENDING) {
            b->type = lhat_type_table(c->result->types);
        }
        b->reached = true;
#if LHAT_WITH_RESOLUTIONS
        chk_record_resolution(c, node, b);
#endif
        return holds_members(c, node, b->type);
    }

    LhatType *owner = path_table(c, node->v.access.target);
    if (owner == NULL || !chk_node_name(c, node->v.access.argument, &name, &length)) {
        return NULL;
    }

    const LhatTypeMember *found = member_named(owner, name, length);
    if (found != NULL) {
        return holds_members(c, node, found->type);
    }

    // Nothing there yet, so the path says there is a table here now.
    LhatType *made = lhat_type_table(c->result->types);
    if (made == NULL || lhat_type_add_member(c->result->types, owner, name,
                                             length, made) == NULL) {
        return NULL;
    }
    return made;
}

// The last segment is the name being introduced, so 8.7 refuses one that is
// already there -- writing let^ twice for one place is a redefinition here
// exactly as it is for a name of its own.
//
// 8.8改: unless `upsert` says otherwise (only let^'s ':=' spelling asks for
// this -- 8.6's table still makes '=' mean plain definition). Then a path
// that already answers to something is reassigned instead: the same check
// check_reassign makes for a bare 'path := value', just reached through a
// let^'s syntax instead. The type is not touched either way in that branch.
static void define_path(Checker *c, const LhatNode *target, LhatType *type,
                        bool upsert)
{
    const LhatNode *last = target_name_node(target);
    LhatType *owner = path_table(c, last->v.access.target);
    if (owner == NULL) {
        return;
    }

    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, last->v.access.argument, &name, &length)) {
        return;
    }
    const LhatTypeMember *found = member_named(owner, name, length);
    if (found != NULL) {
        // 03 の 3.4改2: on a later walk over these statements the member is
        // the one this very statement made, so this walk writes over it --
        // the walk before it is not a second definition, and what it put
        // there is not something to hold this one to either.
        if (c->rewalking && !upsert) {
            ((LhatTypeMember *)found)->type = type;
            return;
        }
        if (!upsert) {
            chk_report_named(c, last, LHAT_CHECK_ERR_REDEFINED, name, length);
            return;
        }
        chk_expect(c, target, type, found->type, LHAT_CHECK_ERR_MISMATCH);
        return;
    }
    lhat_type_add_member(c->result->types, owner, name, length, type);
}

void chk_check_define(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    size_t position = 0;
    // 13.8改: what a call answered with, when several names are taking it
    // apart. Settled on the first target from the type of the one value, so
    // nothing is inferred twice -- and so the inference still happens inside
    // the defining-name context the loop sets up around it.
    //
    // 13.10 asks for no mark of its own, because counting the right side
    // settles which reading applies: one value there with several names on
    // the left is a tuple being taken apart, two values is 8.6's multiple
    // definition, and neither reading fits the other's shape.
    LhatType *tuple = NULL;
    size_t target_count = lhat_node_list_length(node->v.binding.targets);

    // 15.2 with 13.8改: several names binding one yield^ directly is where a
    // tuple R gets fixed -- resume(a, b) sends what these annotations spell,
    // and the binding takes it apart. Built before the walk so the one
    // chk_infer of the value sees the whole tuple; a name with no annotation
    // leaves it NULL, which the yield^ reports as needing one.
    // Only where the yield^ is the sole value: 'a, b = yield^ x, y' parses
    // as two values (the comma is the binding's -- 13.9), and there the
    // yield^ binds one name, as before.
    LhatType *yield_bound = NULL;
    if (value != NULL && value->kind == LHAT_NODE_YIELD &&
        value->next == NULL && target_count > 1) {
        LhatType *sent = lhat_type_tuple(c->result->types);
        bool spelt = sent != NULL;
        for (const LhatNode *target = node->v.binding.targets;
             spelt && target != NULL; target = target->next) {
            LhatType *piece = target->kind == LHAT_NODE_PARAM
                                  ? chk_resolve_type(c, target->v.param.type)
                                  : NULL;
            if (piece == NULL) {
                spelt = false;
                break;
            }
            // The same three a position has to satisfy anywhere (13.8改,
            // 05 の 8.9).
            chk_check_tuple_position(c, target, piece);
            lhat_type_add_position(c->result->types, sent, piece);
        }
        yield_bound = spelt ? sent : NULL;
    }

    // 05 の 4 章 with 8.9: what a unit publishes is a name other units read,
    // and 01 の 8.3 refused a global variable outright. A public^ var^ would
    // be one under another spelling -- a name a reader of another unit sees
    // change without being able to see what changed it. A p^ that publishes
    // an accessor is how a unit lets its state move.
    if (node->v.binding.exported && !node->v.binding.immutable) {
        chk_report(c, node, LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE);
    }

    // 15.11: a _yield^ answers nothing at run time -- the whole statement
    // is the type's alone and compiles to nothing, so only _^ may stand on
    // the left. A name would hold a value that never arrives.
    if (value != NULL && value->next == NULL &&
        value->kind == LHAT_NODE_YIELD && value->v.jump.phantom) {
        for (const LhatNode *target = node->v.binding.targets;
             target != NULL; target = target->next) {
            if (!chk_is_discard(c, target_name_node(target))) {
                chk_report(c, target, LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS);
            }
        }
    }

    // 8.7改: every name this statement binds is unreadable from its own
    // value -- the whole right side reads the old world, a deferred body
    // included (recursion by the bound name went with this; this^ is the
    // spelling). Marked before the walk, cleared at the end of this
    // function. A session's names stay readable (03 の 4.3), and a path
    // target binds no scope name to mark.
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const char *marked = NULL;
        size_t marked_length = 0;
        if (target_is_path(target) ||
            !chk_node_name(c, target_name_node(target), &marked,
                           &marked_length)) {
            continue;
        }
        Binding *being = chk_scope_find_local(c->scope, marked, marked_length);
        if (being != NULL && !being->from_session) {
            being->being_defined = true;
        }
    }

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        // 8.7 with 01 の 8 章: a let^ makes a name in the scope it is
        // written in, so there is no other scope for a specifier to name.
        // ':=' is what reaches an existing binding, here as anywhere.
        if (target_name_node(target)->kind == LHAT_NODE_SCOPE) {
            chk_report(c, target, LHAT_CHECK_ERR_SCOPE_ON_DEFINE);
        }
        LhatType *annotated = target->kind == LHAT_NODE_PARAM
                                  ? chk_resolve_type(c, target->v.param.type)
                                  : NULL;

        // 15.2: a let^ that binds a yield^ directly is where R gets fixed --
        // it is the only place a yield^'s own annotation can be written. The
        // context is only good for the one chk_infer() call it is set around.
        enum YieldContext outer_yctx = c->yield_context;
        LhatType *outer_ybound = c->yield_bound_type;
        c->yield_context = (value != NULL && value->kind == LHAT_NODE_YIELD)
                                ? YIELD_CTX_BOUND : YIELD_CTX_NONE;
        // 13.8改: several names read R as the tuple built above; one name
        // reads its own annotation, as before.
        c->yield_bound_type = target_count > 1 ? yield_bound : annotated;

        // 14.9: a def^ landing in a name may be written against that name
        // from inside itself. 14.5 composes with '..' and the definition is
        // the right side of it, which is where infer_binary reads it too.
        // Anything deeper is some other def^ -- one made inside a body, say --
        // and this name is not what it will be bound to.
        const LhatNode *definition = value;
        if (definition != NULL && definition->kind == LHAT_NODE_BINARY &&
            definition->v.binary.op == LHAT_OP_CONCAT) {
            definition = definition->v.binary.right;
        }
        struct DefLink def_here = {NULL, definition, NULL, c->def_link};
        bool defining = false;
        if (definition != NULL && definition->kind == LHAT_NODE_DEF &&
            !target_is_path(target)) {
            const char *bound = NULL;
            size_t bound_length = 0;
            if (chk_node_name(c, target_name_node(target), &bound, &bound_length)) {
                def_here.binding =
                    chk_scope_find_local(c->scope, bound, bound_length);
                defining = def_here.binding != NULL;
                if (defining) {
                    c->def_link = &def_here;
                }
            }
        }

        LhatType *actual;
        if (tuple != NULL) {
            // 13.8改: the positions of what the one value answered with.
            actual = lhat_type_tuple_at(tuple, position - 1);
            if (actual == NULL) {
                actual = chk_simple(c, LHAT_TYPE_PENDING);
            }
        } else {
            // 03 の 3.1・3.5: a target past the values a multiple assignment
            // actually gave is a gap in inference, not table subtyping's
            // silence -- there is genuinely nothing here yet, so it is
            // pending^ rather than unknown^.
            // 03 の 3.4改: a signature written on the binding is what the
            // value is expected to have, so a parameter nothing was written
            // on takes its type from there. Set around this one call, as the
            // yield context above is.
            LhatType *outer_expected = c->expected_func;
            c->expected_func = annotated;
            actual = value != NULL ? chk_infer(c, value)
                                   : chk_simple(c, LHAT_TYPE_PENDING);
            c->expected_func = outer_expected;

            if (lhat_type_tuple_width(actual) > 0) {
                if (position == 1 && target_count > 1 && value->next == NULL) {
                    // 13.8改: several values, several names, no word needed.
                    tuple = actual;
                    actual = lhat_type_tuple_at(tuple, 0);
                    if (actual == NULL) {
                        actual = chk_simple(c, LHAT_TYPE_PENDING);
                    }
                } else {
                    // A tuple is not a value one name can hold, and it is not
                    // an item of a longer right-hand side either -- that
                    // would bind one name to the whole run. This is what
                    // makes 'var^ a, b = f(), 0' an error.
                    chk_report(c, value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
                }
            } else if (chk_contains_tuple(actual)) {
                // 13.8改: a union with a tuple arm ('(K, V)|nil^', a walk's
                // resume) is no more bindable than the tuple itself -- the
                // name would hold a maybe-run. The loop is what discriminates
                // and takes one apart; drive the walk with it.
                chk_report(c, value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
            }
        }
        // 05 の 8.9: a tuple crosses as one value per slot, so a host value
        // among its positions arrives as a pointer into scratch the next
        // crossing overwrites -- the refusal check_focus makes for a walk's
        // focus, made here for a registered signature's answer.
        if (tuple != NULL && chk_is_hostvalue(actual)) {
            chk_report(c, target, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }

        c->yield_context = outer_yctx;
        c->yield_bound_type = outer_ybound;

        if (defining) {
            c->def_link = def_here.outer;
        }

        if (annotated != NULL && value != NULL) {
            chk_expect(c, value, actual, annotated, LHAT_CHECK_ERR_MISMATCH);
        } else if (value != NULL && actual != NULL &&
                   actual->kind == LHAT_TYPE_NONE) {
            // 13.2: a name binds a value, and a call of a signature with no
            // result does not make one. With an annotation written the
            // expect above has already said so.
            chk_report(c, value, LHAT_CHECK_ERR_MISMATCH);
        }

        const char *name = NULL;
        size_t length = 0;
        LhatType *held = annotated != NULL ? annotated : actual;
        // 03 の 3.1③: strict leaves nothing undecided in a unit. A gap that
        // reached here came through whatever the value was inferred from, so
        // this name would hold something nobody decided and hand that on to
        // every reader of it. 3.4改2's rounds are safe to report from --
        // chk_rounds_next rolls back everything but the last walk. The arity
        // cases below say their own thing about a target with no value, so
        // they are left to say it.
        if (c->strict && annotated == NULL && tuple == NULL && value != NULL &&
            lhat_type_has_gap(held)) {
            chk_report(c, target, LHAT_CHECK_ERR_TYPE_UNDECIDED);
        }
        if (target_is_path(target)) {
            // 8.8: the place is a member of a table the path reaches, not a
            // name of this scope. 8.8改: let^'s ':=' spelling asks to
            // reassign rather than fail when the path already answers to
            // something (node->v.binding.via_reassign_op is only ever set
            // by parse_let -- for^/with^ still always define).
            //
            // 15.1改 and 05 の 8.6: adding a member changes the table as
            // much as writing over one does, so both judgements apply here
            // the same way they do to a reassignment.
            chk_check_write_target(c, target);
            chk_check_opaque_write(c, target);
            // 05 の 8.9: a path lands in a table, and a table member is
            // never a host value.
            if (chk_is_hostvalue(held)) {
                chk_report(c, target, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            define_path(c, target, held, node->v.binding.via_reassign_op);
        } else if (chk_node_name(c, target_name_node(target), &name, &length)) {
            // 05 の 8.9: a session's top-level names keep one-slot places
            // across inputs (03 の 4.3), which a host value's width does
            // not fit. A unit's top level is one ordinary frame and holds
            // one fine.
            if (c->session && c->body_scope == NULL && chk_is_hostvalue(held)) {
                chk_report(c, target, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            Binding *b = chk_scope_find_local(c->scope, name, length);
            if (b != NULL) {
                // Collected before the walk, so this is where its let^ runs.
                b->type = annotated != NULL ? annotated : actual;
                b->reached = true;
                // 03 の 3.4改4: which literal the name means, for the call
                // sites that hand it their shapes. Only a let^'s -- a var^
                // may come to mean another subroutine -- and never a
                // public^ one's: a published signature is a promise to
                // callers in other units, and the calls of this one must
                // not be what narrows it.
                b->value_node = node->v.binding.immutable &&
                                        !node->v.binding.exported &&
                                        tuple == NULL && value != NULL &&
                                        value->kind == LHAT_NODE_FUNC
                                    ? value
                                    : NULL;
                // 15.1改. A destructuring bind takes pieces out of something
                // that was already there (13.10), so nothing it binds is new
                // whatever the source looks like.
                b->fresh = chk_value_is_fresh(c, value, actual);
            }
            // A new name of the same spelling makes any narrowing of the old
            // one stale, since the path now reaches something else.
            chk_drop_narrowings_for(c, target_name_node(target));
        }
        if (tuple == NULL && value != NULL) {
            value = value->next;
        }
    }

    // 13.8改: exactly the positions, both ways. 14.10's width subtyping has
    // nothing to say here -- a tuple carries its positions and no others,
    // since each one is a slot the caller reserved.
    if (tuple != NULL && position != lhat_type_tuple_width(tuple)) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);
    }
    // 13.8改: the parser stopped asking for a mark, so this is where one
    // value meeting several names is answered for. It is a tuple being taken
    // apart, or an error.
    if (tuple == NULL && target_count > 1 &&
        lhat_node_list_length(node->v.binding.values) == 1) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);
    }

    // 8.7改: the statement is over; its names stand readable from here on.
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const char *marked = NULL;
        size_t marked_length = 0;
        if (target_is_path(target) ||
            !chk_node_name(c, target_name_node(target), &marked,
                           &marked_length)) {
            continue;
        }
        Binding *being = chk_scope_find_local(c->scope, marked, marked_length);
        if (being != NULL) {
            being->being_defined = false;
        }
    }
}

// 05 の 8.6: the table a dotted import^ or require^ path stands on -- the
// 'a' of 'import^ a.b.c'. Names are reached through it rather than held in
// it, which is what `is_module` says.
//
// Not sealed, unlike the segments the walk above makes: the import^ that
// built this root goes on to write the segment it brought in *into* it, and
// a sealed table refuses exactly that (path_table below). The two flags ask
// different questions, which is why they are two.
LhatType *chk_module_root_table(Checker *c)
{
    LhatType *table = lhat_type_table(c->result->types);
    if (table != NULL) {
        table->v.table.is_module = true;
    }
    return table;
}

// 05 の 5.3: a require^ runs the unit, and a unit that named itself registers
// under that name. So the type of L^.modules can say so -- what makes this
// honest rather than a guess is that the registration is in the unit's own
// prologue, not something a caller may skip.
//
// Idempotent: 5.3 loads once, so meeting the same unit again adds nothing
// and is not the clash 8.7 makes of two let^ writing one place.
void chk_register_module_type(Checker *c, const char *module_name,
                              LhatType *exports)
{
    LhatType *env = chk_environment_type(c);
    if (env == NULL || module_name == NULL || *module_name == '\0') {
        return;
    }
    const LhatTypeMember *modules = member_named(env, "modules", 7);
    if (modules == NULL || modules->type->kind != LHAT_TYPE_TABLE) {
        return;
    }

    LhatType *owner = modules->type;
    for (const char *segment = module_name;;) {
        size_t length = strcspn(segment, ".");
        const LhatTypeMember *found = member_named(owner, segment, length);
        if (segment[length] == '\0') {
            if (found == NULL) {
                lhat_type_add_member(c->result->types, owner, segment, length,
                                     exports);
            }
            return;
        }
        if (found != NULL) {
            if (found->type->kind != LHAT_TYPE_TABLE) {
                return;
            }
            owner = found->type;
        } else {
            LhatType *made = lhat_type_table(c->result->types);
            if (made == NULL ||
                lhat_type_add_member(c->result->types, owner, segment, length,
                                     made) == NULL) {
                return;
            }
            // 05 の 8.6: a segment of the registry is the machine's, the
            // same as the registry itself -- and is named through, the same
            // way.
            made->v.table.sealed = true;
            made->v.table.is_module = true;
            owner = made;
        }
        segment += length + 1;
    }
}

// 05 の 8.7: what import^ names, or NULL when the host registered nothing
// under that path. Only the host registry is searched -- what a require^
// brought in is reachable through require^ and through nothing else, which
// is what stops the answer depending on the order units are checked in.
LhatType *chk_hosted_module(Checker *c, const LhatNode *path)
{
    LhatType *owner = c->require.hosted;
    if (owner == NULL || path == NULL) {
        return NULL;
    }
    // 'a.b.c' is the MEMBER chain of a path target, so the walk is the same
    // one, from the root outwards.
    if (path->kind == LHAT_NODE_MEMBER) {
        owner = chk_hosted_module(c, path->v.access.target);
        if (owner == NULL) {
            return NULL;
        }
        path = path->v.access.argument;
    }

    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, path, &name, &length)) {
        return NULL;
    }
    const LhatTypeMember *found = member_named(owner, name, length);
    return found != NULL ? found->type : NULL;
}

// 05 の 8.7: the table the segments before the last name reach, made where
// the path does not reach one. The same walk 8.8 does over a written path,
// with what import^ has of its own: a table the machine holds is entered
// rather than refused -- these are the machine's tables (8.6) and this is
// the machine filling them, which is exactly what path_table, the writer's
// walk, has to say no to.
static LhatType *import_owner(Checker *c, const LhatNode *node, LhatType *root)
{
    if (node->kind != LHAT_NODE_MEMBER) {
        return root;  // the root binding, which the caller resolved
    }
    LhatType *owner = import_owner(c, node->v.access.target, root);
    const char *name = NULL;
    size_t length = 0;
    if (owner == NULL || owner->kind != LHAT_TYPE_TABLE ||
        !chk_node_name(c, node->v.access.argument, &name, &length)) {
        return NULL;
    }
    const LhatTypeMember *found = member_named(owner, name, length);
    if (found != NULL) {
        return found->type != NULL && found->type->kind == LHAT_TYPE_TABLE
                   ? found->type
                   : NULL;
    }
    LhatType *made = chk_module_root_table(c);
    if (made == NULL || lhat_type_add_member(c->result->types, owner, name,
                                             length, made) == NULL) {
        return NULL;
    }
    return made;
}

// 05 の 8.7: whether `module` offers everything `standing` already does,
// under the same names and as the same types. What an import^ of a child
// path leaves behind is a table holding that child (and no more), so the
// import of its parent -- the host's own table, which holds the child among
// the rest -- may take its place. Anything else there is a real collision.
static bool import_subsumes(const LhatType *module, const LhatType *standing)
{
    if (module == NULL || standing == NULL ||
        module->kind != LHAT_TYPE_TABLE || standing->kind != LHAT_TYPE_TABLE) {
        return false;
    }
    for (const LhatTypeMember *m = standing->v.table.members; m != NULL;
         m = m->next) {
        const LhatTypeMember *offered =
            member_named(module, m->name, m->name_length);
        if (offered == NULL || offered->type != m->type) {
            return false;
        }
    }
    return true;
}

// The last segment of a written path, which is the name an import^ standing
// alone binds -- the rest are the tables it sits in.
static void check_import(Checker *c, const LhatNode *node, bool binds)
{
    const LhatNode *path = node->v.jump.value;
    LhatType *module = chk_hosted_module(c, path);
    if (module == NULL) {
        // 5.5 already binds what a unit declared; saying so here is what
        // keeps 'no such module' from being read as 'the host forgot it'.
        chk_report(c, node, c->require.resolve != NULL
                            ? LHAT_CHECK_ERR_NOT_HOSTED
                            : LHAT_CHECK_ERR_NOT_HOSTED);
        return;
    }
    if (!binds) {
        return;
    }

    // The path is the same MEMBER chain 8.8 walks, so binding it is that walk
    // with the module put at the end.
    const char *name = NULL;
    size_t length = 0;
    if (path->kind != LHAT_NODE_MEMBER) {
        if (!chk_node_name(c, path, &name, &length)) {
            return;
        }
        // 03 の 3.4改2: on a later walk over these statements the name is
        // bound because this very statement bound it, which is not the
        // collision 8.7 is about.
        Binding *standing = chk_scope_find_local(c->scope, name, length);
        if (standing != NULL) {
            // 8.7: and an import^ of a child path put a table here holding
            // that child. The module named now is the host's own, which
            // holds the child too -- so it takes the place of the stand-in
            // and both are reachable. This is what lets 'import^ love' and
            // 'import^ love.graphics' stand in one scope, in either order.
            if (standing->import_root && standing->type != module &&
                import_subsumes(module, standing->type)) {
                standing->type = module;
                return;
            }
            if (!c->rewalking && standing->type != module) {
                chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name,
                                 length);
            }
            return;
        }
        Binding *only = chk_scope_add(c->scope, name, length, module, node->offset);
        if (only != NULL) {
            only->reached = true;
            only->import_root = true;  // 05 の 8.7, read rather than captured
        }
        return;
    }

    // The root is a name of this scope. 8.8 reaches an enclosing one rather
    // than shadowing it, so two imports of one namespace meet; only when
    // nothing holds it is a new one made.
    const LhatNode *root_node = path;
    while (root_node->kind == LHAT_NODE_MEMBER) {
        root_node = root_node->v.access.target;
    }
    if (!chk_node_name(c, root_node, &name, &length)) {
        return;
    }
    Binding *root = chk_scope_find(c->scope, name, length, NULL);
    if (root == NULL) {
        root = chk_scope_add(c->scope, name, length,
                             chk_module_root_table(c), node->offset);
        if (root == NULL) {
            return;
        }
        root->reached = true;
        root->import_root = true;  // 05 の 8.7, read rather than captured
    } else if (root->type == NULL || root->type->kind == LHAT_TYPE_UNKNOWN ||
               root->type->kind == LHAT_TYPE_PENDING) {
        root->type = chk_module_root_table(c);
    }
    // 8.7: the name is a place from here on, whichever import made it --
    // path_table said so while it was the walk below, and the walk below is
    // import^'s own now.
    root->reached = true;

    // 8.7: the tables an import^ builds are the machine's own (8.6), and
    // this walk is what fills them -- path_table is the writer's, and
    // refuses a table the machine holds, which the host's own registry
    // tables are.
    LhatType *owner = import_owner(c, path->v.access.target, root->type);
    if (owner == NULL) {
        return;
    }
    if (!chk_node_name(c, path->v.access.argument, &name, &length)) {
        return;
    }
    const LhatTypeMember *found = member_named(owner, name, length);
    if (found != NULL) {
        // Already reaching what this import names -- the parent's own table
        // holds it (8.7's registry is one nested table), or an earlier
        // import of this very path put it there. Nothing to bind.
        if (found->type == module) {
            return;
        }
        // A stand-in an earlier import of a child left behind, the same as
        // in the root branch above.
        if (import_subsumes(module, found->type)) {
            ((LhatTypeMember *)found)->type = module;
            return;
        }
        // 03 の 3.4改2: on a later walk the member is there because this very
        // import put it there, which is not a second import of the name.
        if (!c->rewalking) {
            chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name, length);
        }
        return;
    }
    lhat_type_add_member(c->result->types, owner, name, length, module);
}

// 05 の 5.5: a require^ standing alone binds the unit under the path 3 章
// had it declare, rather than under a name the reader picks. The segments
// come from that text, and 8.8's rules then hold one for one: a table is
// made where the path does not reach one, and the last segment is a name
// being introduced.
static void check_require_stmt(Checker *c, const LhatNode *node)
{
    const LhatNode *path = node->v.jump.value;
    if (path == NULL || c->require.resolve == NULL) {
        return;
    }

    const char *module_name = NULL;
    LhatType *exports = c->require.resolve(
        c->require.context, c->lexer->strings + path->v.string.offset,
        path->v.string.length, &module_name);
    if (exports == NULL) {
        chk_report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
        return;
    }
    // 3.2 lets a unit declare no path. Then there is nothing to bind it
    // under, and the reader has to pick a name with let^ instead.
    if (module_name == NULL || *module_name == '\0') {
        chk_report(c, node, LHAT_CHECK_ERR_MODULE_UNNAMED);
        return;
    }
    chk_register_module_type(c, module_name, exports);

    const char *segment = module_name;
    size_t length = strcspn(segment, ".");

    // One segment binds the unit to that name directly; there is no table on
    // the way to make, and 8.7 refuses a name this scope already holds.
    if (segment[length] == '\0') {
        if (chk_scope_find_local(c->scope, segment, length) != NULL) {
            chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, segment, length);
            return;
        }
        Binding *only =
            chk_scope_add(c->scope, segment, length, exports, node->offset);
        if (only != NULL) {
            only->reached = true;
        }
        return;
    }

    // The root of a longer path. 8.8 reaches an enclosing binding rather than
    // shadowing it, which is what lets two units of one namespace meet. The
    // name points into the required unit's result, and 6 章 keeps that alive
    // as long as the program is.
    Binding *root = chk_scope_find(c->scope, segment, length, NULL);
    if (root == NULL) {
        root = chk_scope_add(c->scope, segment, length,
                             chk_module_root_table(c), node->offset);
        if (root == NULL) {
            return;
        }
        root->reached = true;
    } else if (root->type == NULL || root->type->kind == LHAT_TYPE_UNKNOWN ||
               root->type->kind == LHAT_TYPE_PENDING) {
        root->type = chk_module_root_table(c);
    }
    // 05 の 8.7: what this puts under the root was made by running a unit and
    // is in no registry to read back, so the root is captured like any other
    // name from here on -- even if an import^ also landed on it.
    root->import_root = false;

    LhatType *owner = holds_members(c, node, root->type);
    while (owner != NULL && segment[length] == '.') {
        segment += length + 1;
        length = strcspn(segment, ".");

        const LhatTypeMember *found = member_named(owner, segment, length);
        if (segment[length] == '\0') {
            // 8.7 on the last segment: two units may not claim one path.
            if (found != NULL) {
                chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, segment, length);
                return;
            }
            lhat_type_add_member(c->result->types, owner, segment, length,
                                 exports);
            return;
        }
        if (found != NULL) {
            owner = holds_members(c, node, found->type);
            continue;
        }
        LhatType *made = lhat_type_table(c->result->types);
        if (made == NULL || lhat_type_add_member(c->result->types, owner,
                                                 segment, length,
                                                 made) == NULL) {
            return;
        }
        owner = made;
    }
}

// 15.1改: whether this initialiser makes a table rather than naming one that
// was already there. Read off the shape alone, so that no aliases have to be
// followed: 'let^ v = t' names a table, and a call that is not a new may well
// answer one of its arguments ('f^ t { return^ t }' is a function like any
// other), so neither counts.
bool chk_value_is_fresh(const Checker *c, const LhatNode *value,
                        const LhatType *type)
{
    if (value == NULL) {
        return false;
    }
    switch (value->kind) {
        // 14.1, 14.11: a literal and a self^{ … } both build one here.
        case LHAT_NODE_TABLE:
        case LHAT_NODE_SELF_TABLE:
            return true;

        case LHAT_NODE_CALL: {
            // 15.1改3: the resolved callee promised '-> fresh^T', so the
            // answer is new by the signature's own word.
            if (value->call_answers_fresh) {
                return true;
            }
            // 15.5: calling a yieldable subroutine builds a coroutine, and
            // builds a new one every time -- the call is what makes it, so
            // 15.3改 counts it the way 14.11's new is counted below.
            if (type != NULL && type->kind == LHAT_TYPE_CORO) {
                return true;
            }
            // 14.11: 'X.new()' answers an instance nobody else holds yet.
            const LhatNode *callee = value->v.access.target;
            if (callee == NULL || callee->kind != LHAT_NODE_MEMBER) {
                return false;
            }
            const char *name = NULL;
            size_t length = 0;
            return chk_node_name(c, callee->v.access.argument, &name, &length) &&
                   chk_name_is(name, length, "new");
        }

        default:
            return false;
    }
}

// The binding a path is rooted in, with the scope that answered, or NULL when
// the root is not a name a scope holds -- L^ (05 の 8.6) and self^ among them,
// neither of which any body made.
static Binding *path_root_binding(Checker *c, const LhatNode *target, Scope **found)
{
    const LhatNode *root = target_root(target);
    const char *name = NULL;
    size_t length = 0;
    if (root == NULL || !chk_node_name(c, root, &name, &length)) {
        return NULL;
    }
    Scope *from = c->scope;
    if (root->kind == LHAT_NODE_SCOPE) {
        from = chk_scope_from(c->scope, root);
        if (from == NULL) {
            return NULL;
        }
    }
    return chk_scope_find(from, name, length, found);
}

// 15.3改: whether this expression names a coroutine the body being checked
// made. Advancing one is only allowed for those -- one that arrived is shared
// with whoever passed it, and the progress would be visible out there.
bool chk_scope_within_body(Checker *c, const Scope *found_in);

bool chk_receiver_is_own_coroutine(Checker *c, const LhatNode *receiver)
{
    if (receiver == NULL) {
        return false;
    }
    // 15.5: a call builds one on the spot, so 'gen().start()' can never be
    // reaching anyone else's.
    if (receiver->kind == LHAT_NODE_CALL) {
        return true;
    }
    Scope *found_in = NULL;
    Binding *root = path_root_binding(c, receiver, &found_in);
    return root != NULL && root->fresh && chk_scope_within_body(c, found_in);
}

// 15.1改2: the same question of a table -- whether the receiver of a
// mutable^self^ call is something this body made. A literal or a new() is
// made on the spot; a name answers by the binding's origin, exactly as the
// write rule below reads it.
bool chk_receiver_is_own_table(Checker *c, const LhatNode *receiver)
{
    if (receiver == NULL) {
        return false;
    }
    if (chk_value_is_fresh(c, receiver, NULL)) {
        return true;
    }
    Scope *found_in = NULL;
    Binding *root = path_root_binding(c, receiver, &found_in);
    return root != NULL && root->fresh && chk_scope_within_body(c, found_in);
}

// Whether `found_in` is at or inside `boundary` -- the outermost scope of
// some body -- counting from where the walk stands now.
bool chk_scope_within(Checker *c, const Scope *found_in, const Scope *boundary)
{
    for (Scope *s = c->scope; s != NULL; s = s->parent) {
        if (s == found_in) {
            return true;
        }
        if (s == boundary) {
            return false;  // past that body's own outermost scope
        }
    }
    return false;
}

// Whether `found_in` is the body being checked or something inside it.
bool chk_scope_within_body(Checker *c, const Scope *found_in)
{
    return chk_scope_within(c, found_in, c->body_scope);
}

// 15.1: an f^ assigns to local variables only. 10.6 reads that twice over --
// a body with nothing writable outside it has nothing a finally^ could
// restore, which is why one may not be written there at all.
//
// 15.1改 carries it to 't.x := v', which changes a table rather than a name.
// A table the body made is its own and changing it is not observable from
// outside; one that arrived belongs to whoever passed it.
void chk_check_write_target(Checker *c, const LhatNode *target)
{
    if (!c->in_function || c->body_scope == NULL) {
        return;
    }

    if (target_is_path(target)) {
        // 02 の 14.11: a new body adjusts the copy the machine just made --
        // the one table an f^ may change, since nothing outside holds it
        // yet. Only the immediate body: a literal nested inside it is an
        // ordinary one again.
        // 15.1改2: and a mutable^self^ body writes through its receiver by
        // its own signature's word -- the same one root, the same one body.
        if (c->in_new_body || c->receiver_mutable) {
            const LhatNode *root = target_root(target);
            const char *root_name = NULL;
            size_t root_length = 0;
            if (root != NULL &&
                chk_node_name(c, root, &root_name, &root_length) &&
                chk_name_is(root_name, root_length, "self^")) {
                return;
            }
        }
        Scope *found_in = NULL;
        Binding *root = path_root_binding(c, target, &found_in);
        // A root that is no binding at all is nothing this body made: L^ is
        // the machine's own table (05 の 8.6), self^ is the
        // instance the caller handed over (14.4).
        if (root == NULL || !root->fresh || !chk_scope_within_body(c, found_in)) {
            chk_report(c, target, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
        }
        return;
    }

    const LhatNode *name_node = target_name_node(target);
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, name_node, &name, &length)) {
        return;
    }

    // 01 の 8 章: a specifier says where to start looking. Reaching outward
    // with one is the same write as reaching outward without one, so it is
    // judged by where the name was found either way.
    Scope *from = c->scope;
    if (name_node->kind == LHAT_NODE_SCOPE) {
        from = chk_scope_from(c->scope, name_node);
        if (from == NULL) {
            return;  // already reported where the name was read
        }
    }

    Scope *found = NULL;
    if (chk_scope_find(from, name, length, &found) == NULL) {
        return;  // no such name; infer_name reports that
    }
    if (!chk_scope_within_body(c, found)) {
        chk_report(c, target, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    }
}

// 8.9: a name a let^ bound is not one anything may reassign. Kept apart from
// check_write_target because that one answers 15.1's question -- whose name is
// this to write -- and only inside an f^; this holds wherever the name does,
// in a p^, at the top level of a unit, and in a session.
//
// Only a plain name is asked about. A path is a member of a table (8.8), and
// what may be written there is settled by 15.1改 and 05 の 8.6 -- a table a
// let^ holds is still the table it was. What this refuses is the name coming
// to mean something else.
static void check_immutable_write(Checker *c, const LhatNode *target)
{
    if (target_is_path(target)) {
        return;
    }

    const LhatNode *name_node = target_name_node(target);
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, name_node, &name, &length)) {
        return;
    }

    // 01 の 8 章: a specifier says where to start looking, exactly as it does
    // for 15.1's question above.
    Scope *from = c->scope;
    if (name_node->kind == LHAT_NODE_SCOPE) {
        from = chk_scope_from(c->scope, name_node);
        if (from == NULL) {
            return;  // already reported where the name was read
        }
    }

    const Binding *b = chk_scope_find(from, name, length, NULL);
    if (b != NULL && b->immutable) {
        // 12.1 and 16.3改2 bind without the writer choosing a word, so there
        // is no var^ for them to write instead. Saying otherwise would send a
        // reader to a spelling that is itself refused.
        chk_report_named(c, name_node,
                         b->bound_by_form ? LHAT_CHECK_ERR_ASSIGN_TO_FORM
                                          : LHAT_CHECK_ERR_ASSIGN_TO_LET,
                         name, length);
    }
}

// 05 の 8.8: a host type is opaque -- 11.3 gives way to nominal identity
// precisely because there is no structure to compare -- and what it carries
// is what the host registered: C functions reading a raw pointer as the type
// their tag names. Writing over one of those leaves the tag saying one thing
// and the member doing another, which is the mistake 8.8 spends its two
// checks avoiding. 8.8's own mark already refuses a member being *added*
// (holds_members reads from_definition for that); this is the other half.
//
// 02 の 14.16's type info is nominal for the same kind of reason: what it
// carries is fixed by what made it.
//
// A def^ instance is not nominal, so 14 章's fields stay writable -- that is
// what a field is for.
// The type a path segment names, followed without reporting anything. The
// ordinary walk below does the reporting; inferring the same nodes twice
// would say everything twice.
static const LhatType *path_type_of(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;

    if (node->kind != LHAT_NODE_MEMBER) {
        // 05 の 8.6: L^ is a place too, and the one no scope holds.
        if (is_environment(c, node)) {
            return chk_environment_type(c);
        }
        if (!chk_node_name(c, node, &name, &length)) {
            return NULL;
        }
        Scope *from = c->scope;
        if (node->kind == LHAT_NODE_SCOPE) {
            from = chk_scope_from(c->scope, node);
            if (from == NULL) {
                return NULL;
            }
        }
        const Binding *b = chk_scope_find(from, name, length, NULL);
        return b != NULL ? b->type : NULL;
    }

    const LhatType *owner = path_type_of(c, node->v.access.target);
    if (owner == NULL || !chk_node_name(c, node->v.access.argument, &name, &length)) {
        return NULL;
    }
    const LhatTypeMember *m = member_named(owner, name, length);
    return m != NULL ? m->type : NULL;
}

// 02 の 14.11: whether this path reads self^ off a definition anywhere along
// the way. That member is the prototype every instance starts as -- read by
// anyone, written by no one, and nothing inside it is written through it
// either, however deep the path goes and whether the steps are members or
// indices.
static bool path_through_prototype(Checker *c, const LhatNode *node)
{
    while (node != NULL &&
           (node->kind == LHAT_NODE_MEMBER || node->kind == LHAT_NODE_INDEX)) {
        const char *name = NULL;
        size_t length = 0;
        if (node->kind == LHAT_NODE_MEMBER &&
            chk_node_name(c, node->v.access.argument, &name, &length) &&
            chk_name_is(name, length, "self^")) {
            const LhatType *owner = path_type_of(c, node->v.access.target);
            if (owner != NULL && owner->kind == LHAT_TYPE_TABLE &&
                owner->v.table.is_definition) {
                return true;
            }
        }
        node = node->v.access.target;
    }
    return false;
}

void chk_check_opaque_write(Checker *c, const LhatNode *target)
{
    const LhatNode *last = target_name_node(target);
    if (last->kind != LHAT_NODE_MEMBER && last->kind != LHAT_NODE_INDEX) {
        return;
    }
    if (path_through_prototype(c, last)) {
        chk_report(c, last, LHAT_CHECK_ERR_PROTOTYPE_SEALED);
        return;
    }
    // What follows reads the owner of a member; an index writes an element,
    // which 8.8 leaves to the table itself.
    if (last->kind != LHAT_NODE_MEMBER) {
        return;
    }
    const LhatType *owner = path_type_of(c, last->v.access.target);
    if (owner == NULL) {
        return;
    }
    // 05 の 8.9: a host value's registered fields take writes -- they are
    // number^ members, and writing the bytes of one's own copy is the
    // field's whole purpose. Its registered members (operators, methods)
    // stay the host's, 8.8's rule unchanged.
    if (owner->kind == LHAT_TYPE_HOSTVALUE) {
        const char *name = NULL;
        size_t length = 0;
        const LhatTypeMember *member =
            chk_node_name(c, last->v.access.argument, &name, &length)
                ? chk_find_member(owner, name, length)
                : NULL;
        if (member != NULL && member->type != NULL &&
            member->type->kind != LHAT_TYPE_NUMBER) {
            chk_report(c, last, LHAT_CHECK_ERR_PATH_IS_OPAQUE);
        }
        return;
    }
    if (owner->kind != LHAT_TYPE_TABLE) {
        return;
    }
    if (owner->v.table.nominal) {
        chk_report(c, last, LHAT_CHECK_ERR_PATH_IS_OPAQUE);
    }
    // 05 の 8.6: the machine's own tables -- L^, its registry, and what
    // require^ or import^ answers with. The host writes these through its own
    // API, which never comes through here, so refusing what is written in L^
    // is the whole rule.
    if (owner->v.table.sealed) {
        chk_report(c, last, LHAT_CHECK_ERR_TABLE_IS_SEALED);
    }
}

static void check_reassign(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    size_t position = 0;
    LhatType *tuple = NULL;  // 13.8改, as in check_define
    size_t target_count = lhat_node_list_length(node->v.binding.targets);

    // 15.11: a _yield^ answers nothing at run time, and a reassignment's
    // left side is always a name -- there is nothing to write.
    if (value != NULL && value->next == NULL &&
        value->kind == LHAT_NODE_YIELD && value->v.jump.phantom) {
        chk_report(c, node, LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS);
    }

    // 15.2 with 13.8改: a reassignment binding a yield^ directly fixes R
    // too -- off what the names already hold, so no annotation is written.
    // Several names make the tuple, as at a define; one name reads its own
    // held type in the loop below.
    LhatType *yield_bound = NULL;
    if (value != NULL && value->kind == LHAT_NODE_YIELD &&
        value->next == NULL && target_count > 1) {
        LhatType *sent = lhat_type_tuple(c->result->types);
        bool spelt = sent != NULL;
        for (const LhatNode *target = node->v.binding.targets;
             spelt && target != NULL; target = target->next) {
            const LhatNode *outer_target = c->writing_to;
            c->writing_to = target_name_node(target);
            LhatType *held = chk_infer(c, target_name_node(target));
            c->writing_to = outer_target;
            if (held == NULL || held->kind == LHAT_TYPE_UNKNOWN ||
                held->kind == LHAT_TYPE_PENDING) {
                spelt = false;
                break;
            }
            chk_check_tuple_position(c, target, held);
            lhat_type_add_position(c->result->types, sent, held);
        }
        yield_bound = spelt ? sent : NULL;
    }

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        check_immutable_write(c, target);
        chk_check_write_target(c, target);
        chk_check_opaque_write(c, target);
        // 13.11: what may be written is what the name holds, not what a
        // branch narrowed it to -- the write is exactly what ends that claim,
        // and 13.11's own example writes a wider value inside a narrowed
        // branch. The value on the right is read below, where the claim still
        // holds, so 'x := x + 1' keeps it.
        const LhatNode *outer_target = c->writing_to;
        c->writing_to = target_name_node(target);
        LhatType *wanted = chk_infer(c, target_name_node(target));
        c->writing_to = outer_target;
        // 03 の 3.4: what the name holds from here on is not what was passed
        // in, so nothing after this says anything about the parameter.
        chk_close_param_var(c, wanted);
        if (tuple != NULL) {
            // 13.8改: the positions of what the one value answered with.
            LhatType *piece = lhat_type_tuple_at(tuple, position - 1);
            // 05 の 8.9: a host value among the positions arrives as a
            // pointer into scratch -- refused as at a define's destructure.
            if (chk_is_hostvalue(piece)) {
                chk_report(c, target, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            chk_expect(c, node, piece, wanted, LHAT_CHECK_ERR_MISMATCH);
        } else if (value != NULL) {
            // 8.6.4: the value of a '?op=' is 'target op rhs' built around
            // this very target node, so naming it here is what lets the one
            // read inside it answer without its nil^ arm. Set for the one
            // inference and taken back off: the same shape written in the
            // right-hand side is a read like any other.
            const LhatNode *outer_place = c->nil_safe_place;
            if (node->v.binding.compound_nil_safe) {
                c->nil_safe_place = target;
            }
            // 15.2: the yield^'s R is what this place already holds -- the
            // tuple built above for several names, this target's own type
            // for one. Set around the one chk_infer, as at a define.
            enum YieldContext outer_yctx = c->yield_context;
            LhatType *outer_ybound = c->yield_bound_type;
            if (value->kind == LHAT_NODE_YIELD && value->next == NULL) {
                c->yield_context = YIELD_CTX_BOUND;
                c->yield_bound_type =
                    target_count > 1 ? yield_bound : wanted;
            }
            // 03 の 3.4改: what the place holds says what is expected here,
            // as a signature written on a define does -- so a literal
            // written on the right takes its parameters from it.
            LhatType *outer_expected = c->expected_func;
            c->expected_func = target_count > 1 ? NULL : wanted;
            LhatType *given = chk_infer(c, value);
            c->expected_func = outer_expected;
            c->yield_context = outer_yctx;
            c->yield_bound_type = outer_ybound;
            c->nil_safe_place = outer_place;
            // 05 の 8.9: a dotted or indexed target lands in a table, and a
            // table member is never a host value. Conformance alone would
            // let this through where the table's own typing went unknown^.
            if (chk_is_hostvalue(given) && target_is_path(target)) {
                chk_report(c, target, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            if (lhat_type_tuple_width(given) > 0) {
                if (position == 1 && target_count > 1 && value->next == NULL) {
                    tuple = given;  // 13.8改, as in check_define
                    chk_expect(c, node, lhat_type_tuple_at(tuple, 0), wanted,
                               LHAT_CHECK_ERR_MISMATCH);
                } else {
                    chk_report(c, value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
                }
            } else {
                chk_expect(c, value, given, wanted, LHAT_CHECK_ERR_MISMATCH);
            }
            if (tuple == NULL) {
                value = value->next;
            }
        }
        // 13.11: what a branch established about this path no longer holds.
        chk_drop_narrowings_for(c, target_name_node(target));
    }

    if (tuple != NULL && position != lhat_type_tuple_width(tuple)) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);  // 13.8改
    }
    if (tuple == NULL && target_count > 1 &&
        lhat_node_list_length(node->v.binding.values) == 1) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);  // 13.8改, as above
    }
}

// 16.3: with in^ the focus is what each turn binds, not a value to evaluate.
// A lone one is wrapped as `it^ := name` by the parser (16.2), which puts the
// name on the value side -- the same unwrapping the compiler's target_of
// does. What comes back may still carry the annotation of 16.3.
static const LhatNode *focus_element(const LhatNode *element)
{
    if (element->kind != LHAT_NODE_DEFINE) {
        return element;
    }
    if (element->v.binding.targets != NULL &&
        element->v.binding.targets->kind == LHAT_NODE_FOCUS) {
        return element->v.binding.values;
    }
    return element->v.binding.targets;
}

// The member of that name a structure declares, or NULL.
// find_member's search, reaching an error kind's fields too (04 の 2.2).
static const LhatTypeMember *member_named(const LhatType *type,
                                          const char *name, size_t length)
{
    if (type->kind == LHAT_TYPE_ERROR_KIND) {
        return chk_members_search(type->v.error.fields, name, length);
    }
    return chk_find_member(type, name, length);
}

// 16.3: `in^ e` walks e.iterate(). A coroutine answers with itself, a table
// with a walk over its keys, and anything else by having a member of that
// name -- the same three infer_member answers for, read off the type here
// because the loop has no member access written in it to infer.
// `count` is how many names the focus binds. 16.3 with 13.8改 gives the two
// forms different meanings over a table -- one name takes the values of the
// sequence half in order, several take the (K, V) pairs apart -- so what the
// walk produces depends on which was written.
static LhatType *walk_produce(Checker *c, const LhatNode *at, LhatType *over,
                              size_t count)
{
    if (over != NULL && over->kind == LHAT_TYPE_PENDING) {
        // 03 の 3.1・3.5、P6: walking a still-pending^ expression makes the
        // element type pending^ too, not merely unknown^.
        return chk_simple(c, LHAT_TYPE_PENDING);
    }
    if (over == NULL || over->kind == LHAT_TYPE_UNKNOWN ||
        over->kind == LHAT_TYPE_ANY) {
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    if (over->kind == LHAT_TYPE_CORO) {
        return over->v.coroutine.produce;
    }

    const LhatTypeMember *written = member_named(over, "iterate^", 8);
    if (written == NULL &&
        !(over->kind == LHAT_TYPE_TABLE && !over->v.table.is_definition &&
          !over->v.table.from_definition && !over->v.table.nominal)) {
        // 14.17改: everywhere but a plain table the two spellings name one
        // member, so a bare `iterate` written on a def^ or registered on a
        // host type is the same declaration -- the runtime's member_written
        // makes the same crossover.
        written = member_named(over, "iterate", 7);
    }
    if (written != NULL) {
        // 16.3 lets a written iterate win, so this comes before the built-in.
        LhatType *answer = written->type;
        if (answer == NULL || answer->kind == LHAT_TYPE_UNKNOWN ||
            answer->kind == LHAT_TYPE_PENDING) {
            return chk_simple(c, answer != NULL &&
                                      answer->kind == LHAT_TYPE_PENDING
                                  ? LHAT_TYPE_PENDING
                                  : LHAT_TYPE_UNKNOWN);
        }
        // 15.5: what calling it answers -- either a body that yields (13.9),
        // or one that hands back a coroutine it made itself. 16.3 asks only
        // that a coroutine arrive; which of the two wrote it does not matter.
        LhatType *walks = lhat_type_call_answer(answer);
        if (answer->kind != LHAT_TYPE_FUNC || walks == NULL ||
            walks->kind != LHAT_TYPE_CORO) {
            chk_report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        return walks->v.coroutine.produce;
    }

    // 05 の 8.8: a host type is a table only in how its members are reached
    // -- there is nothing of a table's own behind it to walk, so one that
    // registered no iterate is refused here rather than silently walked as
    // an empty table at run time.
    if (over->kind == LHAT_TYPE_TABLE && over->v.table.nominal) {
        chk_report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // The built-in walk of a table. 13.8改: several names take the (K, V)
    // pairs; one name takes the sequence half's values, and never visits the
    // keyed half at all -- 'for^ i from^ 1 to^ the length { t[i] }' written
    // as a walk.
    if (over->kind == LHAT_TYPE_TABLE || over->kind == LHAT_TYPE_ERROR_KIND) {
        return count > 1 ? chk_table_walk_tuple(c, over)
                         : chk_table_element_type(c, over);
    }

    chk_report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// 13.11改 with 16.4: 'for^ i from^ 1 to^ 9' says which whole numbers reach
// the body, so the focus carries that the way a branch's condition makes one
// carry what it tested. Both ends have to be written out -- a limit read off
// a length names no number here, and 14.10's width subtyping puts no ceiling
// on one anyway.
//
// The step is passed over where it is written: with 'to^' the focus stays
// between the two ends whatever a positive step skips, and a step that is not
// positive leaves no such promise to make.
static void bound_the_focus(Checker *c, const LhatNode *node)
{
    if (node->v.loop.kind != LHAT_FOR_TO &&
        node->v.loop.kind != LHAT_FOR_DOWNTO) {
        return;
    }
    const LhatNode *focus = node->v.loop.focus;
    if (focus == NULL || focus->next != NULL ||
        focus->kind != LHAT_NODE_DEFINE) {
        return;
    }
    const LhatNode *name = focus->v.binding.targets;
    if (name == NULL || name->next != NULL || !chk_narrowable(name)) {
        return;
    }
    int64_t from = 0;
    int64_t limit = 0;
    if (!chk_whole_literal(focus->v.binding.values, &from) ||
        !chk_whole_literal(node->v.loop.bound, &limit)) {
        return;
    }
    int64_t step = 1;
    if (node->v.loop.step != NULL &&
        (!chk_whole_literal(node->v.loop.step, &step) || step <= 0)) {
        return;
    }
    if (node->v.loop.kind == LHAT_FOR_TO) {
        chk_push_bounds(c, name, from, limit);
    } else {
        chk_push_bounds(c, name, limit, from);
    }
}

// 16.3: the focus of an in^ loop is bound, not evaluated, so it is checked
// here rather than by check_statements -- which would read the names as uses
// and find nothing in scope.
static void check_focus(Checker *c, const LhatNode *node)
{
    size_t count = 0;
    for (const LhatNode *e = node->v.loop.focus; e != NULL; e = e->next) {
        count++;
    }

    // Counted around walk_produce so the annotation demand below stays
    // quiet when the walk itself was already refused -- one report per
    // mistake.
    size_t already = c->result->diagnostic_count;
    LhatType *produced = walk_produce(c, node->v.loop.bound,
                                      chk_infer(c, node->v.loop.bound), count);

    // 13.8改: a tuple has exactly its positions, and each one is a slot the
    // loop reserved -- said once here rather than once per name below.
    size_t width = lhat_type_tuple_width(produced);
    if (width > 0 && count > 1 && count != width) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);
    }
    // A single name cannot hold a tuple, over an iterator or anywhere else.
    // A table never reaches this: walk_produce answered its element type.
    if (width > 0 && count == 1) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        produced = chk_simple(c, LHAT_TYPE_UNKNOWN);
        width = 0;
    }
    // Several names take what was yielded apart, so there has to be
    // something to take apart: a tuple above, or a table by position below
    // (13.10). Anything else is a loop written with more names than the walk
    // hands over -- 16.3改2's projections are the everyday way to arrive
    // here, since a keys^ walk yields one value and never a pair.
    if (width == 0 && count > 1 && produced != NULL &&
        produced->kind != LHAT_TYPE_TABLE &&
        produced->kind != LHAT_TYPE_UNKNOWN &&
        produced->kind != LHAT_TYPE_PENDING &&
        produced->kind != LHAT_TYPE_ANY) {
        chk_report(c, node, LHAT_CHECK_ERR_TUPLE_ARITY);
    }

    size_t position = 0;
    for (const LhatNode *e = node->v.loop.focus; e != NULL; e = e->next) {
        position++;
        const LhatNode *element = focus_element(e);
        if (element == NULL) {
            continue;
        }
        LhatType *annotated = element->kind == LHAT_NODE_PARAM
                                  ? chk_resolve_type(c, element->v.param.type)
                                  : NULL;

        // 13.10 and 16.3: one name takes what was yielded whole, several
        // take it apart by position. A tuple's positions come off the tuple;
        // a table's (a user iterator yielding one) off its indexed members.
        LhatType *taken = produced;
        if (count > 1 && width > 0) {
            taken = lhat_type_tuple_at(produced, position - 1);
            if (taken == NULL) {
                taken = chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
        } else if (count > 1) {
            const LhatTypeMember *at = lhat_type_member_at(produced, position);
            taken = at != NULL ? at->type : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        LhatType *type = taken;
        if (annotated != NULL) {
            chk_expect(c, element, taken, annotated, LHAT_CHECK_ERR_MISMATCH);
            type = annotated;
        } else if (c->strict &&
                   (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
                    lhat_type_has_gap(type)) &&
                   c->result->diagnostic_count == already) {
            // 03 の 3.1③: strict leaves nothing undecided, and a walk the
            // table's type cannot describe (computed keys -- the dictionary
            // type is still unwritten) leaves the focus exactly that. An
            // annotation is the way to say it: 'for^ k:K, v:V in^ t'.
            // The same guard a define's name has (check_define); unknown^
            // is included here because the table walk answers it for the
            // halves it cannot name, with nothing else reported.
            chk_report(c, element, LHAT_CHECK_ERR_TYPE_UNDECIDED);
        }

        // 05 の 8.9: a tuple crosses the host boundary as copied values, and
        // a host value among them would arrive as a pointer into scratch the
        // next step overwrites -- the same escape every other tuple position
        // refuses.
        if (count > 1 && chk_is_hostvalue(type)) {
            chk_report(c, element, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        // Stamped so the compiler can size the focus: width_of reads this,
        // and it is the one channel a host value's width travels by. On the
        // name node, which is the one the compiler's target_of answers for
        // a bare name and an annotated one alike.
        ((LhatNode *)target_name_node(element))->checked_type = type;

        const char *name = NULL;
        size_t length = 0;
        if (!chk_node_name(c, target_name_node(element), &name, &length)) {
            continue;
        }
        Binding *b = chk_scope_add(c->scope, name, length, type,
                                   target_name_node(element)->offset);
        if (b != NULL) {
            b->reached = true;  // 8.7: a turn of the loop has bound it
            // 8.9: what each turn binds is the walk's to say, so the focus of
            // an in^ is a let^ -- 16.3 already refuses the ':=' and the
            // introducer that would have said otherwise, since there is no
            // initial value for either to give.
            b->immutable = true;
            b->bound_by_form = true;
        }
    }
}

// 12.5 with 12.7: what with^ asks of a value is that it has a dispose() and
// that the dispose() returns nothing. The second half is not decoration --
// 12.7 made it so that cleanup cannot fail, and a dispose() with a result
// would have somewhere to report a failure from.
//
// Checked by hand rather than against a written structure, because the
// implicit self^ of 14.4 means the parameter list of the member and of a call
// to it do not have the same length.
static void check_disposable(Checker *c, const LhatNode *at, LhatType *type)
{
    if (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING || type->kind == LHAT_TYPE_ANY) {
        return;
    }

    // 05 の 8.8改: through the base link too -- a derived host type is
    // disposable when what it is declared under registered a dispose,
    // which is exactly what the machine does with the tag chain.
    const LhatTypeMember *m =
        type->kind == LHAT_TYPE_TABLE
            ? chk_find_member(type, "dispose", 7)
            : NULL;
    if (m != NULL) {
        if (m->type == NULL || m->type->kind == LHAT_TYPE_UNKNOWN ||
            m->type->kind == LHAT_TYPE_PENDING) {
            return;
        }
        // 12.5 spells the condition 'dispose()' with the parentheses: with^
        // hands over nothing, so one asking for an argument could not be
        // called. 13.4 keeps self^ out of the list, which is why the ordinary
        // 'p^self^ { }' of 14.4 has an empty one.
        if (m->type->kind != LHAT_TYPE_FUNC || m->type->v.func.result != NULL ||
            m->type->v.func.params != NULL || m->type->v.func.variadic != NULL) {
            chk_report(c, at, LHAT_CHECK_ERR_NOT_DISPOSABLE);
        }
        return;
    }
    chk_report(c, at, LHAT_CHECK_ERR_NOT_DISPOSABLE);
}

// 04 の 2.2: the declaration creates the set and its kinds as types.
static void check_errordef(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node->v.named.name, &name, &length)) {
        return;
    }

    // 04 の 2.7: which top, from which word declared it. The kinds read it
    // off the set, so nothing below has to be told twice.
    LhatType *set = lhat_type_error_set(c->result->types, name, length,
                                        node->v.named.local);
    chk_scope_add(c->scope, name, length, set, node->offset)->reached = true;

    for (const LhatNode *kind = node->v.named.members; kind != NULL;
         kind = kind->next) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (!chk_node_name(c, kind->v.named.name, &kind_name, &kind_length)) {
            continue;
        }
        LhatType *type = lhat_type_error_kind(c->result->types, set, kind_name,
                                              kind_length);
        for (const LhatNode *field = kind->v.named.members; field != NULL;
             field = field->next) {
            const char *field_name = NULL;
            size_t field_length = 0;
            if (!chk_node_name(c, field->v.param.name, &field_name, &field_length)) {
                continue;
            }

            // 04 の 2.2: a default may stand in for the type, and its own
            // type is then the field's, exactly as 14.11 reads a template.
            LhatType *declared = chk_resolve_type(c, field->v.param.type);
            LhatType *fallback = chk_infer(c, field->v.param.fallback);
            if (declared != NULL && fallback != NULL) {
                chk_expect(c, field->v.param.fallback, fallback, declared,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            // 04 の 2.7改: a kind is nominal, so chk_type_touches_local stops
            // at it and never sees this field -- which is the whole reason
            // that walk can stop there (2.3's `cause` takes either family).
            // The hole is closed on the written side instead, here.
            if (chk_type_touches_local(declared != NULL ? declared : fallback, 0)) {
                chk_report(c, field, LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN);
            }
            LhatTypeMember *member = lhat_type_add_member(
                c->result->types, type, field_name, field_length,
                declared != NULL ? declared : fallback);
            if (member != NULL) {
                member->optional = field->v.param.fallback != NULL;
            }
            chk_member_declared_at(c, member, field->v.param.name);
        }
    }
}

// 02 の 19 章: the declaration makes the enum and its members -- the
// members as singleton types below it, and the name as a binding whose
// type is the enum itself, so E.AAA reads as a member access and x : E
// takes any member. The value expressions are inferred here for the
// members' `.value` types; they run where the declaration stands.
static void check_enumdef(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node->v.named.name, &name, &length)) {
        return;
    }
    LhatType *decl = lhat_type_enum_decl(c->result->types, name, length);
    chk_scope_add(c->scope, name, length, decl, node->offset)->reached = true;
    // The compiler stamps this declaration into the RT_ENUM descriptor --
    // what fits^ compares.
    ((LhatNode *)node)->checked_type = decl;

    for (const LhatNode *member = node->v.named.members; member != NULL;
         member = member->next) {
        const char *member_name = NULL;
        size_t member_length = 0;
        if (!chk_node_name(c, member->v.named.name, &member_name,
                           &member_length)) {
            continue;
        }
        LhatType *value_type =
            member->v.named.members != NULL
                ? chk_infer(c, member->v.named.members)
                : chk_simple(c, LHAT_TYPE_NUMBER);
        lhat_type_enum_member(c->result->types, decl, member_name,
                              member_length, value_type);
    }
}

// 8.7: every name a scope defines is visible throughout it, so they are
// collected before the statements are walked. That is what lets two
// subroutines call each other without a forward declaration.
static void collect_bindings(Checker *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_ERRORDEF) {
            check_errordef(c, s);
            continue;
        }
        if (s->kind == LHAT_NODE_ENUMDEF) {
            check_enumdef(c, s);
            continue;
        }
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        for (const LhatNode *target = s->v.binding.targets; target != NULL;
             target = target->next) {
            const char *name = NULL;
            size_t length = 0;

            // 8.8: a path introduces a member, not a name of this scope. Its
            // root is only made when nothing anywhere holds it -- reaching an
            // enclosing table is the point, so this must not shadow one.
            if (target_is_path(target)) {
                const LhatNode *root = target_root(target);
                // A hat identifier is not a name a scope can hold. L^ is a
                // place of its own (05 の 8.6); anything else is refused when
                // the path is walked.
                if (root->kind != LHAT_NODE_HAT_IDENT &&
                    chk_node_name(c, root, &name, &length) &&
                    chk_scope_find(c->scope, name, length, NULL) == NULL) {
                    chk_scope_add(c->scope, name, length,
                                  chk_simple(c, LHAT_TYPE_PENDING), root->offset);
                }
                continue;
            }

            if (!chk_node_name(c, target_name_node(target), &name, &length)) {
                continue;
            }
            // 13.12: '_^' may be written as often as it likes, so a second one
            // is not a redefinition -- it is the same place being thrown away
            // again. Nothing reads it either way: chk_infer refuses the
            // spelling wherever a value is wanted.
            bool discard = chk_is_discard(c, target_name_node(target));
            Binding *already = chk_scope_find_local(c->scope, name, length);
            if (discard && already != NULL) {
                continue;
            }
            if (already != NULL) {
                // 03 の 4.3: a name an earlier input of a session bound is
                // written again here, which is the same place written again.
                // Clearing the mark leaves 8.7 in force for a second let^
                // within this input.
                if (!already->from_session) {
                    chk_report_named(c, target_name_node(target),
                                     LHAT_CHECK_ERR_REDEFINED, name, length);
                }
                already->from_session = false;
                // 8.9 and 03 の 4.3: a session redefinition is the name being
                // written again, so what the new input says about it is what
                // holds -- a let^ over an earlier var^ makes it a let^ from
                // here on, and the other way round.
                already->immutable = s->v.binding.immutable;
                continue;
            }
            Binding *b =
                chk_scope_add(c->scope, name, length, chk_simple(c, LHAT_TYPE_PENDING),
                              target_name_node(target)->offset);
            // 8.9: set here rather than in check_define, so that a write
            // standing textually before the definition it names is judged by
            // the same rule as one standing after it -- 8.7 makes the name
            // visible throughout the scope either way.
            if (b != NULL) {
                b->immutable = s->v.binding.immutable;
                b->bound_by_form = s->v.binding.bound_by_form;
            }
        }
    }
}

// 05 の 3 章: 'module^ a.b.c' is the same MEMBER chain a path target is, so
// writing it out is one walk. Appends to `out` when there is room, and
// answers how much room the whole path wants either way.
static size_t write_module_path(const Checker *c, const LhatNode *node,
                                char *out, size_t room, size_t used)
{
    if (node->kind == LHAT_NODE_MEMBER) {
        used = write_module_path(c, node->v.access.target, out, room, used);
        if (used < room) {
            out[used] = '.';
        }
        used++;
        node = node->v.access.argument;
    }

    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, node, &name, &length)) {
        return used;
    }
    for (size_t i = 0; i < length; i++) {
        if (used + i < room) {
            out[used + i] = name[i];
        }
    }
    return used + length;
}

// The path the unit declared, or NULL when it declared none (3.2). The
// parser has already put module^ first and refused a second one.
char *chk_read_module_name(const Checker *c, const LhatNode *statements)
{
    const LhatNode *declaration = NULL;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_MODULE) {
            declaration = s;
            break;
        }
    }
    if (declaration == NULL || declaration->v.named.name == NULL) {
        return NULL;
    }

    size_t needed = write_module_path(c, declaration->v.named.name, NULL, 0, 0);
    char *text = (char *)lhat_alloc(needed + 1);
    if (text == NULL) {
        return NULL;
    }
    write_module_path(c, declaration->v.named.name, text, needed, 0);
    text[needed] = '\0';
    return text;
}

// 05 の 4 章: the exports are the names marked public^, read from the
// declarations rather than from a value the unit returns. That is what lets
// 6 章 follow an import without running anything.
LhatType *chk_collect_exports(Checker *c, const LhatNode *statements)
{
    LhatType *table = NULL;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        const LhatNode *named = NULL;
        if (s->kind == LHAT_NODE_DEFINE && s->v.binding.exported) {
            named = s->v.binding.targets;
        } else if ((s->kind == LHAT_NODE_ERRORDEF ||
                    s->kind == LHAT_NODE_ENUMDEF) &&
                   s->v.named.exported) {
            named = s->v.named.name;
        } else {
            continue;
        }

        for (; named != NULL; named = named->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(c, target_name_node(named), &name, &length)) {
                continue;
            }
            Binding *b = chk_scope_find_local(c->scope, name, length);
            if (b == NULL) {
                continue;
            }
            if (table == NULL) {
                table = lhat_type_table(c->result->types);
                // 05 の 8.6: what require^ answers is the machine's record
                // of what the unit published, not a table the requiring unit
                // may add to or write over. What the published names hold is
                // untouched -- 4.2 publishes names, and a table one of them
                // holds stays as mutable as it was.
                if (table != NULL) {
                    table->v.table.sealed = true;
                    table->v.table.is_module = true;  // named through
                }
            }
            // 05 の 6.1: a reader of the published type is in another unit,
            // and the place to point at is the declaration here.
            chk_member_declared_at(
                c, lhat_type_add_member(c->result->types, table, name, length,
                                        b->type),
                target_name_node(named));
        }
    }
    return table;
}

void chk_check_statements(Checker *c, const LhatNode *statements)
{
    // 8.7 collects the names once. What it reports -- a name written twice in
    // one scope -- is about the statements as written, so a second walk over
    // them has nothing to add to it.
    Binding *before = c->scope != NULL ? c->scope->tail : NULL;
    collect_bindings(c, statements);

    // 03 の 3.4改2: the walk below is one iteration of a least fixpoint, the
    // same as a def^'s entries. 8.7 makes every name of this list visible
    // throughout it, so a body may call one whose let^ this walk has not
    // reached -- and what answers there is the mark collect_bindings put down
    // rather than a type. Walking again from what the last walk inferred is
    // what closes a ring of them.
    size_t count = 0;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        count++;
    }
    Rounds rounds;
    chk_rounds_begin(c, &rounds, count);
    bool outer_rewalking = c->rewalking;

    do {
        // A narrowing an if-statement leaves behind holds for the rest of this
        // list and no further, so the list is what bounds its life.
        Narrowing *mark = c->narrowings;
        for (const LhatNode *s = statements; s != NULL; s = s->next) {
            chk_check_statement(c, s);
        }
        chk_pop_narrowings(c, mark);

        // What this list bound is what the next walk is seeded with, so the
        // types are compared before they are put back -- an answer that moved
        // is what says another walk is worth running.
        for (Binding *b = before != NULL ? before->next
                                         : (c->scope != NULL ? c->scope->bindings
                                                             : NULL);
             b != NULL; b = b->next) {
            if (rounds.round == 0 || !lhat_type_equal(b->seed, b->type)) {
                rounds.changed = true;
            }
        }
        if (!chk_rounds_next(c, &rounds)) {
            break;
        }
        c->rewalking = true;
        for (Binding *b = before != NULL ? before->next
                                         : (c->scope != NULL ? c->scope->bindings
                                                             : NULL);
             b != NULL; b = b->next) {
            // 8.7 again from the top: which names this walk has reached is
            // what it is about to work out a second time.
            b->reached = false;
            // 3.4改2: the type the last walk inferred is the seed, with the
            // gap arms taken out. A 'bool^|pending^' read back whole would
            // union its own gap in again and never lose it; seeded 'bool^',
            // the walk answers what it can, and an arm that really belongs
            // comes back from the body it is read off.
            b->type = lhat_type_without_gaps(c->result->types, b->type);
            b->seed = b->type;
        }
    } while (true);

    c->rewalking = outer_rewalking;
    chk_rounds_end(c, &rounds);
}

// The block's statements in the scope that is already open. 01 の 8 章: a
// subroutine's body is written with one '{', so it is one scope for '$^' to
// count -- and its parameters are in it, not around it. infer_func has
// already made that scope and put them there, so this must not make another
// or a specifier would find the parameters one step too soon.
// 9 章: the clauses other than main^, which the parser leaves in `extra`
// whatever order they were written in. Read by kind so the walk can follow
// 9.2's order rather than the source's -- compile.c's clause_of is the same
// lookup on the same list.
static const LhatNode *clause_named(const LhatNode *node, LhatClauseKind kind)
{
    for (const LhatNode *clause = node->v.list.extra; clause != NULL;
         clause = clause->next) {
        if (clause->v.loop_clause.kind == kind) {
            return clause->v.loop_clause.body;
        }
    }
    return NULL;
}

// One clause, walked where its names belong and with what it may narrow.
// `into` is the layer to bind into (NULL for the one already open), and
// `with` the narrowings that hold while it runs.
static void check_clause(Checker *c, const LhatNode *node, LhatClauseKind kind,
                         Scope *into, Narrowing *with)
{
    const LhatNode *body = clause_named(node, kind);
    if (body == NULL) {
        return;
    }
    Scope *held = c->scope;
    Narrowing *saved = c->narrowings;
    if (into != NULL) {
        c->scope = into;
    }
    c->narrowings = with;
    chk_check_statements(c, body);
    c->narrowings = saved;
    c->scope = held;
}

void chk_check_block_in_scope(Checker *c, const LhatNode *node)
{
    // 13.11: the enclosing loop's condition, if this block is its body. Taken
    // here, so a block written inside the body is walked without one.
    struct LoopTest *test = c->loop_test;
    c->loop_test = NULL;

    // 9.3: the block's own statements are main^, written or implied. With no
    // clauses beside them there is one layer and one order, which is every
    // block that is not a loop body -- a subroutine's among them.
    if (node->v.list.extra == NULL) {
        chk_check_statements(c, node->v.list.items);
        return;
    }

    // 9.4: the body has two layers. prolog^ and first^ last the whole loop
    // and bind into the layer around this one; pre^ and main^ last one
    // iteration and bind here, so last^ and epilog^ -- walked back out there
    // -- do not see them. 9.2 fixes the order the six run in and the walk
    // follows it, which is what lets main^ read what prolog^ declared.
    Scope *carried = c->scope->parent;
    Narrowing *narrowed = c->narrowings;
    // 9.2: main^ and first^ are the two clauses on the far side of the test,
    // so they are the two that keep what the condition narrowed. The list is
    // a stack, so setting the head back is the whole of putting it aside.
    Narrowing *plain = test != NULL ? test->before : narrowed;

    check_clause(c, node, LHAT_CLAUSE_PROLOG, carried, plain);
    check_clause(c, node, LHAT_CLAUSE_PRE, NULL, plain);
    check_clause(c, node, LHAT_CLAUSE_FIRST, carried, narrowed);

    chk_check_statements(c, node->v.list.items);  // main^

    check_clause(c, node, LHAT_CLAUSE_LAST, carried, plain);
    check_clause(c, node, LHAT_CLAUSE_EPILOG, carried, plain);
    // 10 章: a finally^ is the block's own cleanup rather than one of 9.2's
    // six, and it runs where the block is left -- with what the body bound
    // still in reach.
    check_clause(c, node, LHAT_CLAUSE_FINALLY, NULL, plain);
}

static void check_block(Checker *c, const LhatNode *node)
{
    Scope *outer = c->scope;

    // 9.4: a loop body written with clauses binds into two layers. The outer
    // one carries what lasts the whole loop; 01 の 8 章 does not count it,
    // since the writer put down one '{' and compile.c counts one scope too.
    Scope carried;
    bool layered = node->v.list.extra != NULL;
    if (layered) {
        carried.bindings = NULL;
        carried.tail = NULL;
        carried.parent = c->scope;
        carried.transparent = true;
        c->scope = &carried;
    }

    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = c->scope;
    scope.transparent = false;

    c->scope = &scope;

    chk_check_block_in_scope(c, node);

    c->scope = outer;
    chk_scope_dispose(&scope);
    if (layered) {
        chk_scope_dispose(&carried);
    }
}

// 13.11 with 16.3: a conditional loop tests before every turn -- 9.10 puts
// the form that runs its body first under pre^ rather than in the condition
// -- so a body that runs at all runs where the condition held. That is the
// same ground an if^ body stands on, and it narrows the same way: while^
// where the condition is true, until^ where it is not.
//
// Nothing is narrowed after the loop. 9.8's break^ leaves from anywhere, so
// what ended it is not known out there.
static void check_loop_body(Checker *c, const LhatNode *body,
                            const LhatNode *condition, bool tested_true)
{
    if (condition == NULL) {
        chk_check_statement(c, body);  // to^, in^, do^, repeat^ n: no condition
        return;
    }

    Narrowing *before = c->narrowings;
    chk_narrow_from(c, condition, tested_true);

    // Which clauses stand on the far side of the test is the block's own
    // question (9.2), and this is how it is told.
    struct LoopTest test;
    test.before = before;
    struct LoopTest *outer = c->loop_test;
    c->loop_test = &test;

    chk_check_statement(c, body);

    c->loop_test = outer;
    chk_pop_narrowings(c, before);
}

// 04 の 4.5: the body is checked with a frame open, so every try^ written in
// it hands its errors here instead of to the subroutine's result. Each arm
// then takes what it is written for, with it^ narrowed to that (4.2), and
// what no arm took goes on out exactly as a bare try^ would have sent it.
static void check_try_block(Checker *c, const LhatNode *node)
{
    const LhatNode *body = node->v.list.items;
    if (body == NULL) {
        return;
    }

    struct CatchFrame frame = { NULL, c->catch_frame };
    c->catch_frame = &frame;
    chk_check_statement(c, body->v.clause.body);
    c->catch_frame = frame.outer;

    // 4.1's line, one construct over: catching what cannot fail says nothing.
    if (frame.caught == NULL) {
        chk_report(c, node, LHAT_CHECK_ERR_CATCHES_NOTHING);
    }

    LhatType *left = frame.caught;
    for (const LhatNode *arm = body->next; arm != NULL; arm = arm->next) {
        LhatType *want = arm->v.clause.condition != NULL
                             ? chk_resolve_type(c, arm->v.clause.condition)
                             : NULL;
        // The bare arm takes everything still standing; a written one takes
        // the arms of that which fit it, the same reading 13.11's fits^ makes.
        LhatType *here = want != NULL ? chk_only(c, left, want) : left;

        Scope scope;
        scope.bindings = NULL;
        scope.tail = NULL;
        scope.parent = c->scope;
        Scope *outer = c->scope;
        c->scope = &scope;
        Binding *caught =
            chk_scope_add(&scope, "it^", 3,
                          here != NULL ? here : chk_any_error(c), arm->offset);
        if (caught != NULL) {
            caught->reached = true;
        }
        // An arm is one path among several, so a let^ written in it is as
        // uncertain as one inside an if^ clause.
        c->conditional++;
        chk_check_statement(c, arm->v.clause.body);
        c->conditional--;
        c->scope = outer;
        chk_scope_dispose(&scope);

        if (want != NULL) {
            left = chk_without(c, left, want);
        } else {
            left = NULL;
        }
    }

    chk_error_leaves(c, node, left);
}

void chk_check_statement(Checker *c, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
            // 02 の 18.4: a binding takes one. The unit's own were read where
            // the unit was, and a field's and a member's where the def^ is.
            //
            // 18.4改: whether it is published is part of the place. A host
            // reaches a value through the table the unit answers with (05 の
            // 5.5), so an annotation about the value has no hold on a name
            // kept private -- and saying so here is what keeps it from being
            // written and quietly doing nothing.
            chk_check_annotations(
                c, node->v.binding.annotations,
                node->v.binding.exported
                    ? (LHAT_ANNOTATION_BINDING | LHAT_ANNOTATION_PUBLIC)
                    : LHAT_ANNOTATION_BINDING);
            chk_check_define(c, node);
            break;

        case LHAT_NODE_REQUIRE_STMT:
            check_require_stmt(c, node);
            break;

        case LHAT_NODE_IMPORT_STMT:
            check_import(c, node, true);  // 05 の 8.7
            break;

        case LHAT_NODE_REASSIGN:
            check_reassign(c, node);
            break;

        case LHAT_NODE_CALL_STMT: {
            LhatType *value = chk_infer(c, node->v.jump.value);
            // 15.8: 15.5 makes such a call run no part of the body, so the
            // statement provably has no effect. 04 の 8.1 の form: the type
            // is the detection, with no must-use machinery added.
            //
            // 03 の 4.3: unless it is the statement the input answers with.
            // There the value is shown, which is an effect -- the reasoning
            // above is what stops applying, not the rule.
            if (value != NULL && value->kind == LHAT_TYPE_CORO &&
                node != c->answering) {
                chk_report(c, node, LHAT_CHECK_ERR_COROUTINE_DROPPED);
            }
            break;
        }

        case LHAT_NODE_BLOCK:
            check_block(c, node);
            break;

        case LHAT_NODE_IF_STMT: {
            // 13.11. A clause's body sees what its own condition established;
            // every later clause, and the final else, see what the earlier
            // conditions ruled out. Handling every arm of a union that way is
            // all 04 の 7 章 needs for exhaustiveness.
            Narrowing *outer = c->narrowings;
            bool has_else = false;
            bool every_branch_exits = true;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    has_else = true;
                    // still only reached when every earlier condition
                    // failed, so a let^ path written here is exactly as
                    // uncertain as one inside an ordinary clause.
                    c->conditional++;
                    chk_check_statement(c, clause->v.clause.body);
                    c->conditional--;
                    continue;
                }
                chk_expect(c, condition, chk_infer(c, condition),
                           chk_simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);

                Narrowing *before = c->narrowings;
                chk_narrow_from(c, condition, true);
                c->conditional++;
                chk_check_statement(c, clause->v.clause.body);
                c->conditional--;
                chk_pop_narrowings(c, before);

                if (!chk_always_exits(clause->v.clause.body)) {
                    every_branch_exits = false;
                }
                chk_narrow_from(c, condition, false);
            }

            // Reaching the statement after this one means no branch was taken
            // -- but only when every branch that was taken left, and there was
            // no else to fall out of. Otherwise what holds below is a join of
            // several paths, which is not attempted here.
            if (has_else || !every_branch_exits) {
                chk_pop_narrowings(c, outer);
            }
            break;
        }

        case LHAT_NODE_TRY_BLOCK:
            check_try_block(c, node);
            break;

        case LHAT_NODE_RETURN: {
            // 02 の 14.11: construction answers the instance itself, so a
            // new body has nothing to return^ -- what it writes through
            // self^ is already on what the caller gets. A literal nested
            // inside the body is an ordinary one again (in_new_body is the
            // immediate body's alone), and a body that is one expression
            // (15.12) wrote no return^ to refuse. Either way the values
            // still run, so they are read here -- and nothing about the
            // result is, since the signature never asks the body.
            if (c->in_new_body) {
                if (!node->v.jump.implicit) {
                    chk_report(c, node, LHAT_CHECK_ERR_NEW_RETURNS);
                }
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    chk_infer(c, item);
                }
                break;
            }
            // 03 の 3.4: a return^ that reaches the subroutine itself says
            // nothing about the result -- its type is the one being worked
            // out. The others determine it between them.
            // 03 の 3.4: a bare return^ leaves without a value. That is the
            // same kind of exit as reaching the end of the body, and infer_func
            // treats the two together -- so nothing more is decided here.
            if (node->v.jump.value == NULL) {
                c->valueless_return = true;
                break;
            }

            bool enclosing_self_call = c->saw_self_call;
            c->saw_self_call = false;
            LhatType *value = NULL;
            if (node->v.jump.level > 1) {
                // 13.8改: 'return^ a, b' answers a tuple. The values hang off
                // `value` as a list and `level` counted them.
                LhatType *tuple = lhat_type_tuple(c->result->types);
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    LhatType *position = chk_infer(c, item);
                    chk_check_tuple_position(c, item, position);
                    lhat_type_add_position(c->result->types, tuple, position);
                }
                value = tuple;
            } else {
                // 03 の 3.4改: a written result type is what this value is
                // expected to have, so a literal returned here takes the
                // parameters nothing was written on from it.
                LhatType *outer_expected = c->expected_func;
                c->expected_func = c->declared_result;
                value = chk_infer(c, node->v.jump.value);
                c->expected_func = outer_expected;
            }
            bool recursive = c->saw_self_call;
            c->saw_self_call = enclosing_self_call || recursive;

            // 15.1改3: a '-> fresh^T' body owes every exit something this
            // body made -- a literal, a new(), the answer of another fresh^
            // call ('return^ this^(…)' among them, since the signature
            // already carries the promise), or a name this body bound to
            // one of those (the origin question 15.1改2 asks of a
            // receiver). In an f^ the reading is tight -- nothing this body
            // made can have leaked out; in a p^ it is closer to a
            // declaration, since a p^ could also have stored it somewhere.
            if (c->must_answer_fresh &&
                (node->v.jump.value == NULL || node->v.jump.level > 1 ||
                 !chk_receiver_is_own_table(c, node->v.jump.value))) {
                chk_report(c, node, LHAT_CHECK_ERR_ANSWER_NOT_FRESH);
            }

            // 05 の 8.9: at the top level of a unit this is the program's
            // answer, which leaves through a single value slot the host
            // reads -- there is no frame above for the slots to land in.
            if (c->body_scope == NULL && chk_is_hostvalue(value)) {
                chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            // 13.8改: a tuple does cross -- 05 の 8.7's LhatRunResult carries
            // the positions beside the value, so a unit may answer several.
            // A host value still may not: its width is its tag's, and the
            // result has one slot for the value itself.

            // 04 の 2.7改: what a return^ carries is what a caller receives,
            // so this is the other door chk_error_leaves guards. Asked before
            // either branch below: a written result that admits one has
            // already been refused where it was written, and an inferred one
            // must not quietly take it.
            //
            // A 'return^ try^ …' never lands here carrying one -- the try^
            // reported for itself and handed back the value without the
            // error arm -- so what this finds is a value that failed and was
            // returned as it stands, which is what 'return^ x as^ T' is.
            //
            // The unit's own top level (05 の 3.2) reaches here too, and only
            // here: it is no func literal, so the signature check in
            // chk_infer_func never sees it.
            if (chk_type_touches_local(value, 0)) {
                chk_report(c, node, LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES);
                break;
            }

            if (c->declared_result != NULL) {
                // 03 の 7 章、P6: unlike chk_expect()'s other callers, this one
                // runs while the subroutine being defined is still open --
                // a call to a not-yet-checked partner in a mutually
                // recursive pair is pending^ here for a reason that has
                // nothing to do with strict, and forgiving it is what lets
                // 03 の 3.4's "no forward declaration needed" hold under
                // strict too. lhat_type_conforms_strict belongs where a
                // value's own checking has already finished (this one's
                // declared result has not) -- this call stays on the plain,
                // always-lenient lhat_type_conforms instead.
                if (!lhat_type_conforms(value, c->declared_result)) {
                    chk_report(c, node, LHAT_CHECK_ERR_MISMATCH);
                }
                break;
            }

            if (recursive) {
                c->recursive_return = true;
            }
            // 03 の 3.4: a recursive exit says nothing only when its type is
            // the one being worked out. Once the call sits inside a larger
            // expression the answer may not depend on it at all --
            // 'return^ "a" .. f(n).to_s()' is string^ whatever f answers --
            // and dropping it would lose that.
            if (recursive && (value == NULL || value->kind == LHAT_TYPE_UNKNOWN ||
                              value->kind == LHAT_TYPE_PENDING)) {
                break;
            }
            // 13.2: a return^ with an expression carries a value, and a call
            // of a signature with no result does not make one. Reported here
            // rather than let into the union, where it would reach every
            // caller as a type nothing inhabits. A written result has already
            // caught it above.
            if (value != NULL && value->kind == LHAT_TYPE_NONE) {
                chk_report(c, node, LHAT_CHECK_ERR_MISMATCH);
                break;
            }
            // 13.8改: every exit of one subroutine answers the same width.
            // A union of a tuple with anything but an error cannot be written
            // (resolve_type refuses it), and is not inferred either -- one
            // that could be would leave a caller with no width to reserve.
            if (c->inferred_result != NULL &&
                lhat_type_tuple_width(value) !=
                    lhat_type_tuple_width(c->inferred_result)) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
                break;
            }
            // 03 の 3.4: several return^ make a union.
            c->inferred_result =
                lhat_type_union(c->result->types, c->inferred_result, value);
            break;
        }

        // 04 の 11.6: panic^ answers no value of its own (always_exits
        // already treats it as a way out, same as return^/break^), so the
        // operand is only checked for its own sake -- any type at all is
        // fine, the same way typeof^'s operand asks nothing of its type.
        case LHAT_NODE_PANIC:
            chk_infer(c, node->v.jump.value);
            break;

        case LHAT_NODE_YIELD: {
            // 15.2: nobody receives this one, so R is not being fixed here
            // -- only Y, from whatever chk_infer() finds inside it.
            enum YieldContext outer_yctx = c->yield_context;
            c->yield_context = YIELD_CTX_DISCARD;
            chk_infer(c, node);
            c->yield_context = outer_yctx;
            break;
        }

        case LHAT_NODE_AWAIT:
            chk_infer(c, node);
            break;

        case LHAT_NODE_ERRORDEF:
        case LHAT_NODE_ENUMDEF:
            break;  // handled while collecting, so kinds are visible early

        case LHAT_NODE_FOR:
        case LHAT_NODE_REPEAT:
        case LHAT_NODE_WITH: {
            // The focus and the bindings belong to a scope of their own.
            // with^ still treats its own keyword as the introducer (12 章);
            // for^'s focus is introduced by let^ written inside it, if any --
            // otherwise a bare ':=' reaches out through this scope to an
            // existing name instead of defining one here (8.6改, 16.3改).
            Scope scope;
            scope.bindings = NULL;
            scope.tail = NULL;
            scope.parent = c->scope;

            Scope *outer = c->scope;
            c->scope = &scope;

            if (node->kind == LHAT_NODE_FOR) {
                // 16.3: in^ binds its focus each turn from what the walk
                // yields, so the names are defined here rather than read.
                LhatType *bound_type = NULL;
                if (node->v.loop.kind == LHAT_FOR_IN) {
                    check_focus(c, node);
                } else {
                    chk_check_statements(c, node->v.loop.focus);
                    bound_type = chk_infer(c, node->v.loop.bound);
                    bound_the_focus(c, node);
                }
                chk_infer(c, node->v.loop.step);
                chk_check_statements(c, node->v.loop.advance);
                // a loop body may run zero times. 16.3's do^ form has no
                // clause to drive it, so its body is reached whatever the
                // focus turned out to be.
                bool always = node->v.loop.kind == LHAT_FOR_ONCE;
                if (!always) {
                    c->conditional++;
                }
                // 13.11: the two conditional forms narrow their body. 16.4's
                // to^ and downto^ are driven by a bound rather than tested,
                // and in^ by what the walk answers, so neither has a
                // condition to read -- and if^ and when^ are an if^ already.
                bool conditional_loop = node->v.loop.kind == LHAT_FOR_WHILE ||
                                        node->v.loop.kind == LHAT_FOR_UNTIL;
                // The condition is a condition -- bool^, as an if^'s is.
                if (conditional_loop && bound_type != NULL) {
                    chk_expect(c, node->v.loop.bound, bound_type,
                               chk_simple(c, LHAT_TYPE_BOOL),
                               LHAT_CHECK_ERR_NOT_BOOL);
                }
                check_loop_body(c, node->v.loop.body,
                                conditional_loop ? node->v.loop.bound : NULL,
                                node->v.loop.kind == LHAT_FOR_WHILE);
                if (!always) {
                    c->conditional--;
                }
            } else if (node->kind == LHAT_NODE_REPEAT) {
                // 16.5: the same two forms, and repeat^ n counts rather than
                // testing, so its bound is not a condition either.
                bool conditional_loop =
                    node->v.repeat.kind == LHAT_REPEAT_WHILE ||
                    node->v.repeat.kind == LHAT_REPEAT_UNTIL;
                // The condition is a condition -- bool^, as an if^'s is.
                if (conditional_loop && node->v.repeat.bound != NULL) {
                    chk_expect(c, node->v.repeat.bound,
                               chk_infer(c, node->v.repeat.bound),
                               chk_simple(c, LHAT_TYPE_BOOL),
                               LHAT_CHECK_ERR_NOT_BOOL);
                } else {
                    chk_infer(c, node->v.repeat.bound);
                }
                c->conditional++;
                check_loop_body(c, node->v.repeat.body,
                                conditional_loop ? node->v.repeat.bound : NULL,
                                node->v.repeat.kind == LHAT_REPEAT_WHILE);
                c->conditional--;
            } else {
                chk_check_statements(c, node->v.list.items);
                // 12.5: the binding is what has to be disposable, so the
                // report belongs on it rather than inside the block.
                for (const LhatNode *b = node->v.list.items; b != NULL;
                     b = b->next) {
                    // 13.12: a '_^' is refused wherever a value is read, and
                    // the target here is not read -- what is wanted is the
                    // type the binding took, which is looked up rather than
                    // inferred. 12.6's disposal is the whole point of writing
                    // the form with no name.
                    const LhatNode *bound = target_name_node(b->v.binding.targets);
                    LhatType *held = NULL;
                    if (chk_is_discard(c, bound)) {
                        const char *name = NULL;
                        size_t length = 0;
                        Binding *place =
                            chk_node_name(c, bound, &name, &length)
                                ? chk_scope_find(c->scope, name, length, NULL)
                                : NULL;
                        held = place != NULL ? place->type : NULL;
                    } else {
                        held = chk_infer(c, b->v.binding.targets);
                    }
                    check_disposable(c, b->v.binding.values, held);
                }
                chk_check_statement(c, node->v.list.extra);
            }

            c->scope = outer;
            chk_scope_dispose(&scope);
            break;
        }

        default:
            break;
    }
}

