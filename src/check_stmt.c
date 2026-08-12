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

static const LhatNode *target_root(const LhatNode *target)
{
    return lhat_define_target_root(target);
}

static bool target_is_path(const LhatNode *target)
{
    return lhat_define_target_is_path(target);
}

static const LhatTypeMember *member_named(const LhatType *type,
                                          const char *name, size_t length);

// ast.c's L^ test (05 の 8.6), shared with the compiler.
static bool is_environment(const Checker *c, const LhatNode *node)
{
    return lhat_node_is_environment(node, c->lexer->source->text);
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
        chk_record_resolution(c, node, b);
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

    // 05 の 4.3: everything written inside a public^ declaration has to say
    // what its parameters take. 4.1's reason carries over -- what a unit
    // publishes is settled by reading the declaration, not by running or
    // checking a body -- and telling what leaves the unit from what stays
    // would need more than the syntax to decide (4.3).
    if (node->v.binding.exported) {
        c->exporting++;
        // 05 の 4 章 with 8.9: what a unit publishes is a name other units
        // read, and 01 の 8.3 refused a global variable outright. A public^
        // var^ would be one under another spelling -- a name a reader of
        // another unit sees change without being able to see what changed it.
        // A p^ that publishes an accessor is how a unit lets its state move.
        if (!node->v.binding.immutable) {
            chk_report(c, node, LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE);
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

        // Tell infer_func which name this subroutine is being given, so a
        // call to it inside its own body is recognised (03 の 3.4).
        const char *outer_name = c->defining_name;
        size_t outer_length = c->defining_length;
        chk_node_name(c, target_name_node(target), &c->defining_name,
                      &c->defining_length);

        // 15.2: a let^ that binds a yield^ directly is where R gets fixed --
        // it is the only place a yield^'s own annotation can be written. The
        // context is only good for the one chk_infer() call it is set around.
        enum YieldContext outer_yctx = c->yield_context;
        LhatType *outer_ybound = c->yield_bound_type;
        c->yield_context = (value != NULL && value->kind == LHAT_NODE_YIELD)
                                ? YIELD_CTX_BOUND : YIELD_CTX_NONE;
        c->yield_bound_type = annotated;

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

        c->yield_context = outer_yctx;
        c->yield_bound_type = outer_ybound;

        if (defining) {
            c->def_link = def_here.outer;
        }

        c->defining_name = outer_name;
        c->defining_length = outer_length;

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

    if (node->v.binding.exported) {
        c->exporting--;
    }
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
            // same as the registry itself.
            made->v.table.sealed = true;
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
        if (chk_scope_find_local(c->scope, name, length) != NULL) {
            chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name, length);
            return;
        }
        Binding *only = chk_scope_add(c->scope, name, length, module, node->offset);
        if (only != NULL) {
            only->reached = true;
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
                             lhat_type_table(c->result->types), node->offset);
        if (root == NULL) {
            return;
        }
        root->reached = true;
    } else if (root->type == NULL || root->type->kind == LHAT_TYPE_UNKNOWN ||
               root->type->kind == LHAT_TYPE_PENDING) {
        root->type = lhat_type_table(c->result->types);
    }

    LhatType *owner = path_table(c, path->v.access.target);
    if (owner == NULL) {
        return;
    }
    if (!chk_node_name(c, path->v.access.argument, &name, &length)) {
        return;
    }
    if (member_named(owner, name, length) != NULL) {
        chk_report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name, length);
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
                             lhat_type_table(c->result->types), node->offset);
        if (root == NULL) {
            return;
        }
        root->reached = true;
    } else if (root->type == NULL || root->type->kind == LHAT_TYPE_UNKNOWN ||
               root->type->kind == LHAT_TYPE_PENDING) {
        root->type = lhat_type_table(c->result->types);
    }

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

// Whether `found_in` is the body being checked or something inside it.
bool chk_scope_within_body(Checker *c, const Scope *found_in)
{
    for (Scope *s = c->scope; s != NULL; s = s->parent) {
        if (s == found_in) {
            return true;
        }
        if (s == c->body_scope) {
            return false;  // past the body's own outermost scope
        }
    }
    return false;
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

void chk_check_opaque_write(Checker *c, const LhatNode *target)
{
    const LhatNode *last = target_name_node(target);
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

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        check_immutable_write(c, target);
        chk_check_write_target(c, target);
        chk_check_opaque_write(c, target);
        LhatType *wanted = chk_infer(c, target_name_node(target));
        // 03 の 3.4: what the name holds from here on is not what was passed
        // in, so nothing after this says anything about the parameter.
        chk_close_param_var(c, wanted);
        if (tuple != NULL) {
            // 13.8改: the positions of what the one value answered with.
            chk_expect(c, node, lhat_type_tuple_at(tuple, position - 1), wanted,
                       LHAT_CHECK_ERR_MISMATCH);
        } else if (value != NULL) {
            LhatType *given = chk_infer(c, value);
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
        if (answer->kind != LHAT_TYPE_FUNC || answer->v.func.result == NULL ||
            answer->v.func.result->kind != LHAT_TYPE_CORO) {
            chk_report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        return answer->v.func.result->v.coroutine.produce;
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

// 16.3: the focus of an in^ loop is bound, not evaluated, so it is checked
// here rather than by check_statements -- which would read the names as uses
// and find nothing in scope.
static void check_focus(Checker *c, const LhatNode *node)
{
    size_t count = 0;
    for (const LhatNode *e = node->v.loop.focus; e != NULL; e = e->next) {
        count++;
    }

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
        }

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

    const LhatTypeMember *members =
        type->kind == LHAT_TYPE_TABLE ? type->v.table.members : NULL;
    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length != 7 || memcmp(m->name, "dispose", 7) != 0) {
            continue;
        }
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

    LhatType *set = lhat_type_error_set(c->result->types, name, length);
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
            LhatTypeMember *member = lhat_type_add_member(
                c->result->types, type, field_name, field_length,
                declared != NULL ? declared : fallback);
            if (member != NULL) {
                member->optional = field->v.param.fallback != NULL;
            }
        }
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
        } else if (s->kind == LHAT_NODE_ERRORDEF && s->v.named.exported) {
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
                }
            }
            lhat_type_add_member(c->result->types, table, name, length, b->type);
        }
    }
    return table;
}

void chk_check_statements(Checker *c, const LhatNode *statements)
{
    collect_bindings(c, statements);

    // A narrowing an if-statement leaves behind holds for the rest of this
    // list and no further, so the list is what bounds its life.
    Narrowing *mark = c->narrowings;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        chk_check_statement(c, s);
    }
    chk_pop_narrowings(c, mark);
}

// The block's statements in the scope that is already open. 01 の 8 章: a
// subroutine's body is written with one '{', so it is one scope for '$^' to
// count -- and its parameters are in it, not around it. infer_func has
// already made that scope and put them there, so this must not make another
// or a specifier would find the parameters one step too soon.
void chk_check_block_in_scope(Checker *c, const LhatNode *node)
{
    chk_check_statements(c, node->v.list.items);
    for (const LhatNode *clause = node->v.list.extra; clause != NULL;
         clause = clause->next) {
        chk_check_statements(c, clause->v.loop_clause.body);
    }
}

static void check_block(Checker *c, const LhatNode *node)
{
    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = c->scope;

    Scope *outer = c->scope;
    c->scope = &scope;

    chk_check_block_in_scope(c, node);

    c->scope = outer;
    chk_scope_dispose(&scope);
}

void chk_check_statement(Checker *c, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
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

        case LHAT_NODE_RETURN: {
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
                value = chk_infer(c, node->v.jump.value);
            }
            bool recursive = c->saw_self_call;
            c->saw_self_call = enclosing_self_call || recursive;

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

        case LHAT_NODE_YIELD_ALL:
            chk_infer(c, node);
            break;

        case LHAT_NODE_ERRORDEF:
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
                if (node->v.loop.kind == LHAT_FOR_IN) {
                    check_focus(c, node);
                } else {
                    chk_check_statements(c, node->v.loop.focus);
                    chk_infer(c, node->v.loop.bound);
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
                chk_check_statement(c, node->v.loop.body);
                if (!always) {
                    c->conditional--;
                }
            } else if (node->kind == LHAT_NODE_REPEAT) {
                chk_infer(c, node->v.repeat.bound);
                c->conditional++;
                chk_check_statement(c, node->v.repeat.body);
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

