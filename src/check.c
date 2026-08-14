// L^ (lhat) -- the type checking stage.


#include "check_internal.h"

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void chk_report(Checker *c, const LhatNode *at, LhatCheckErrorCode code)
{
    if (at == NULL) {
        return;
    }

    LhatCheckResult *r = c->result;
    LHAT_GROW(r->diagnostics, r->diagnostic_count, r->diagnostic_capacity, 8,
              return);

    LhatCheckDiagnostic *d = &r->diagnostics[r->diagnostic_count++];
    d->code = code;
    d->offset = at->offset;
    d->line = at->line;
    d->column = at->column;
    d->name = NULL;
    d->name_length = 0;
}

// 07 の 4 章: what a written name turned out to mean. Kept so that a tool
// reads the walk's own answer rather than resolving 8 章's scoping a second
// time. Dropped rather than failing the check when there is no room -- what
// it feeds is a convenience, and the program is no less checked without it.
void chk_record_resolution(Checker *c, const LhatNode *at, const Binding *b)
{
    LhatCheckResult *r = c->result;
    if (at->end <= at->offset) {
        return;  // no span: nothing to hover over
    }
    LHAT_GROW(r->resolutions, r->resolution_count, r->resolution_capacity, 64,
              return);

    LhatResolution *entry = &r->resolutions[r->resolution_count++];
    entry->use = at->offset;
    entry->use_end = at->end;
    entry->definition = b->offset;
    entry->type = b->type;
}

// The same, about a name. The text is borrowed from the source, which 6 章
// keeps alive as long as the result -- a copy per diagnostic would be paid
// for by every program, and almost none of them read one.
void chk_report_named(Checker *c, const LhatNode *at,
                      LhatCheckErrorCode code, const char *name,
                      size_t length)
{
    size_t before = c->result->diagnostic_count;
    chk_report(c, at, code);
    if (c->result->diagnostic_count == before || name == NULL) {
        return;  // report kept quiet, so there is nothing to say it on
    }
    LhatCheckDiagnostic *d = &c->result->diagnostics[before];
    d->name = name;
    d->name_length = (uint32_t)length;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// The text of an IDENT, HAT_IDENT or TYPE_NAME node. Names live as spans into
// the source, so nothing is copied.
//
// 01 の 2.3: the hat is part of the name -- 'self^' is a different
// name from 'self'. A spelling with more hats than one is the same name
// reached further out (this^^^ is the this^ two levels up), so the
// ast.c's canonical-name reading (01 の 2.3), against this unit's source.
// One definition is what keeps the checker and the machine agreeing on the
// key a member lives under.
bool chk_node_name(const Checker *c, const LhatNode *node,
                   const char **text, size_t *length)
{
    return lhat_node_name(node, c->lexer->source->text, c->lexer->strings,
                          text, length);
}

bool chk_name_is(const char *text, size_t length, const char *literal)
{
    return lhat_name_is(text, length, literal);
}

// 13.12: whether this is '_^', the receptacle that binds nothing. Only the
// hatted spelling means it -- 13.12 refuses `_x` on purpose, so an ordinary
// name beginning with an underscore is untouched.
bool chk_is_discard(const Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           chk_node_name(c, node, &name, &length) &&
           chk_name_is(name, length, "_^");
}

// 14.12改: whether this is super^ written out. Only the hatted spelling means
// it, so an ordinary name `super` is untouched.
bool chk_is_super_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           chk_node_name(c, node, &name, &length) &&
           chk_name_is(name, length, "super^");
}

// ---------------------------------------------------------------------------
// Scopes (8.7)
// ---------------------------------------------------------------------------

Binding *chk_scope_find_local(Scope *scope, const char *name, size_t length)
{
    for (Binding *b = scope->bindings; b != NULL; b = b->next) {
        if (b->name_length == length && memcmp(b->name, name, length) == 0) {
            return b;
        }
    }
    return NULL;
}

// 15.1 asks which scope answered, not only what it holds: an f^ may write to
// a name its own body made and to no other. `found_in` may be NULL for the
// callers that only want the binding.
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = chk_scope_find_local(s, name, length);
        if (b != NULL) {
            if (found != NULL) {
                *found = s;
            }
            return b;
        }
    }
    return NULL;
}

// 01 の 2.3: the stacked reach, passing over the innermost `skip`
// bindings of the name -- it^^ is the it^ one binding out, self^^/class^^
// the enclosing def^'s. The same search order as scope_find, so the two
// agree on which binding is "innermost".
Binding *chk_scope_find_skipping(Scope *scope, const char *name,
                                 size_t length, size_t skip)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = chk_scope_find_local(s, name, length);
        if (b != NULL) {
            if (skip == 0) {
                return b;
            }
            skip--;
        }
    }
    return NULL;
}

// 01 の 8 章: where a scope specifier starts looking, counted either way.
//
// `$^` skips the scope the name was written in and searches from its
// parent, `$^^` from the one above that -- outwards from here. `$` names
// the outermost, which 05 の 3 章 makes the unit and 03 の 4.3 makes the
// session's top level; `$$` the one scope inside it and `$$$` the one
// inside that -- inwards from there. The lexer eats the first sigil before
// counting, so `depth` is the absolute index outright: 0 is the unit.
//
// Answers NULL when there are not that many scopes, which is the writer
// counting wrong rather than a name that is missing.
//
// The search from there is the ordinary one -- a specifier says where to
// begin, not where to stop, so an ancestor further out still answers.
Scope *chk_scope_from(Scope *scope, const LhatNode *node)
{
    if (node->v.scope.kind == LHAT_SCOPE_FILE) {
        // How many scopes stand between here and the outermost, so that an
        // absolute depth can be turned into that many steps outwards.
        uint32_t open = 0;
        for (Scope *at = scope; at != NULL && at->parent != NULL;
             at = at->parent) {
            open++;
        }
        if (node->v.scope.depth > open) {
            return NULL;  // naming a scope further in than the one here
        }
        uint32_t out = open - node->v.scope.depth;
        for (uint32_t i = 0; i < out; i++) {
            if (scope == NULL) {
                return NULL;
            }
            scope = scope->parent;
        }
        return scope;
    }
    for (uint32_t out = 0; out < node->v.scope.depth; out++) {
        if (scope == NULL) {
            return NULL;
        }
        scope = scope->parent;
    }
    return scope;
}

Binding *chk_scope_add(Scope *scope, const char *name, size_t length,
                       LhatType *type, uint32_t offset)
{
    Binding *b = (Binding *)lhat_calloc(1, sizeof *b);
    if (b == NULL) {
        return NULL;
    }
    b->name = name;
    b->name_length = length;
    b->type = type;
    b->offset = offset;

    if (scope->bindings == NULL) {
        scope->bindings = b;
    } else {
        scope->tail->next = b;
    }
    scope->tail = b;
    return b;
}

void chk_scope_dispose(Scope *scope)
{
    Binding *b = scope->bindings;
    while (b != NULL) {
        Binding *next = b->next;
        lhat_free(b);
        b = next;
    }
    scope->bindings = NULL;
    scope->tail = NULL;
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

LhatType *chk_simple(Checker *c, LhatTypeKind kind)
{
    return lhat_type_simple(c->result->types, kind);
}

LhatType *chk_resolve_type(Checker *c, const LhatNode *node);
LhatType *chk_infer(Checker *c, const LhatNode *node);
LhatType *chk_environment_type(Checker *c);  // 05 の 8.6
// 05 の 8.9: the escape rules, written where the escapes would happen.
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found);
bool chk_scope_within_body(Checker *c, const Scope *found);
bool chk_is_hostvalue(const LhatType *type);
LhatType *chk_without_nil_arm(Checker *c, LhatType *target);
LhatType *chk_typeinfo_type(Checker *c);     // 14.16
void chk_register_module_type(Checker *c, const char *module_name,
                              LhatType *exports);  // 05 の 5.3
LhatType *chk_hosted_module(Checker *c, const LhatNode *path);  // 8.7
void chk_check_statement(Checker *c, const LhatNode *node);
void chk_check_block_in_scope(Checker *c, const LhatNode *node);
LhatType *chk_collect_exports(Checker *c, const LhatNode *statements);
void chk_check_statements(Checker *c, const LhatNode *statements);
LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base);
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right);
LhatType *chk_instance_of(const LhatType *definition);
const LhatTypeMember *chk_unimplemented_member(const LhatType *definition);
const LhatTypeMember *chk_find_member(const LhatType *table, const char *name,
                                      size_t length);
LhatType *chk_only(Checker *c, LhatType *type, LhatType *wanted);
LhatType *chk_without(Checker *c, LhatType *type, LhatType *unwanted);
ParamVar *chk_param_var_for(Checker *c, const LhatType *type);
bool chk_signature_accepts(const LhatType *func, LhatType *const *args,
                           size_t count, bool through_member);
bool chk_value_is_fresh(const Checker *c, const LhatNode *value,
                        const LhatType *type);
void chk_check_write_target(Checker *c, const LhatNode *target);
void chk_check_opaque_write(Checker *c, const LhatNode *target);
bool chk_receiver_is_own_coroutine(Checker *c, const LhatNode *receiver);

// 14.8: one number type over integers and reals. The rest are the plain
// builtin spellings.
static LhatType *builtin_type(Checker *c, const char *name, size_t length)
{
    if (chk_name_is(name, length, "number^") || chk_name_is(name, length, "int^") ||
        chk_name_is(name, length, "float^")) {
        return chk_simple(c, LHAT_TYPE_NUMBER);
    }
    if (chk_name_is(name, length, "string^")) {
        return chk_simple(c, LHAT_TYPE_STRING);
    }
    if (chk_name_is(name, length, "bool^")) {
        return chk_simple(c, LHAT_TYPE_BOOL);
    }
    if (chk_name_is(name, length, "nil^")) {
        return chk_simple(c, LHAT_TYPE_NIL);
    }
    if (chk_name_is(name, length, "any^")) {
        return chk_simple(c, LHAT_TYPE_ANY);
    }
    if (chk_name_is(name, length, "error^")) {
        return chk_simple(c, LHAT_TYPE_ERROR);
    }
    // 14.10: t^ and table^ are the same word. Bare, with no members listed,
    // it asks for nothing in particular -- the top of tables, which 13.7
    // notes is not the top of every value.
    if (chk_name_is(name, length, "t^") || chk_name_is(name, length, "table^")) {
        return lhat_type_table(c->result->types);
    }
    return NULL;
}

// The written members of one t^{ ... } or of its self^{ ... } section, read
// into 'table'. The section itself is not a member and is passed over here --
// resolve_table_type reads it before any of this.
static void resolve_members_into(Checker *c, LhatType *table,
                                 const LhatNode *items, const LhatNode *node)
{
    // 14.10: an entry with no name is the type of the next position. They
    // are counted the way a literal counts its own -- from one, in written
    // order, with the named ones taking no place in the sequence.
    size_t position = 0;
    for (const LhatNode *m = items; m != NULL; m = m->next) {
        const char *name = NULL;
        size_t length = 0;
        if (m->v.entry.value != NULL &&
            m->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
            continue;
        }
        LhatType *member = chk_resolve_type(c, m->v.entry.value);
        // 05 の 8.9: a table member is never a host value, written no more
        // than inferred.
        if (chk_is_hostvalue(member)) {
            chk_report(c, m->v.entry.value != NULL ? m->v.entry.value : node,
                       LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        // 13.7, 14.10: the unbounded tail. 'value' is NULL for an untyped
        // '...', which 13.7 makes any^.
        if (m->v.entry.variadic) {
            table->v.table.variadic =
                m->v.entry.value != NULL ? member : chk_simple(c, LHAT_TYPE_ANY);
            continue;
        }
        if (m->v.entry.key == NULL) {
            lhat_type_add_index_member(c->result->types, table, ++position,
                                       member);
            continue;
        }
        if (!chk_node_name(c, m->v.entry.key, &name, &length)) {
            continue;
        }
        lhat_type_add_member(c->result->types, table, name, length, member);
    }
}

static LhatType *resolve_table_type(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(c->result->types);
    // 13.13: the structure is what Self^ inside it names, so it is bound
    // before the members are read -- a member's type is written after the
    // literal it belongs to has been opened, which is the whole point.
    struct SelfLink here = { table, c->self_link };
    c->self_link = &here;

    // 14.7改: a self^{ ... } section makes this a definition, and says what
    // its instances carry. It is read first, and from then on 13.13's Self^
    // names the instance -- inside a def^ that is what the word means, and
    // this is the same structure written down (14.16). The definition itself
    // is one structure further out, Self^^.
    const LhatNode *section = NULL;
    for (const LhatNode *m = node->v.list.items; m != NULL; m = m->next) {
        if (m->v.entry.value != NULL &&
            m->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
            section = m->v.entry.value;
            break;
        }
    }
    struct SelfLink within = { NULL, c->self_link };
    if (section != NULL) {
        LhatType *instance = lhat_type_table(c->result->types);
        table->v.table.instance = instance;
        within.type = instance;
        c->self_link = &within;
        resolve_members_into(c, instance, section->v.list.items, node);
        // 14.4: a member is reached through the definition as well
        // ('let^ f = A.m'), so it stands on both. A field does not: the
        // template says what an instance carries, and nothing of it is on the
        // definition. Which is which is the same question 14.7改 asks of a
        // def^'s entries, so the written form says what a def^ builds.
        for (const LhatTypeMember *m = instance->v.table.members; m != NULL;
             m = m->next) {
            if (chk_takes_receiver(m->type)) {
                lhat_type_add_member(c->result->types, table, m->name,
                                     m->name_length, m->type);
            }
        }
    }

    resolve_members_into(c, table, node->v.list.items, node);
    c->self_link = here.outer;
    return table;
}

// 14.4: in a type the receiver is written as the first parameter, 'p^self^;'.
// 13.4 keeps names out of a type, so it arrives as a parameter whose type is
// the word self^ rather than as a named one.
static bool is_self_marker(const Checker *c, const LhatNode *param)
{
    const LhatNode *written = param->v.param.name != NULL ? param->v.param.name
                                                          : param->v.param.type;
    const char *name = NULL;
    size_t length = 0;
    return written != NULL && !param->v.param.variadic &&
           (written->kind == LHAT_NODE_HAT_IDENT ||
            written->kind == LHAT_NODE_TYPE_NAME) &&
           chk_node_name(c, written, &name, &length) &&
           chk_name_is(name, length, "self^");
}

// Where the receiver stands in a written parameter list: 0 when this parameter
// is not the marker at all, 1 when it leads, 2 when it trails.
//
// 11.3改: a trailing self^ says the receiver is the RIGHT operand, which
// only an op^ may mean -- 'op^+ = f^lhs:number^, self^' is what answers
// '1 + v', where the left operand can carry no answer of its own. infer_def
// refuses it on any other member (14.4: everywhere else the receiver is what
// stands before the dot).
//
// Written at both ends it counts as both, leaving a signature with no ordinary
// parameter at all -- which 11.8's shape rule then reports for an op^, and the
// rule above for anything else. Neither needs a case here.
int chk_self_marker_at(const Checker *c, const LhatNode *params,
                       const LhatNode *param)
{
    if (!is_self_marker(c, param)) {
        return 0;
    }
    if (param == params) {
        return 1;
    }
    // A self^ written in the middle is not a marker: it stays a parameter
    // whose type is the word self^, which 13.1 has no such type for.
    return param->next == NULL ? 2 : 0;
}

LhatType *chk_resolve_func_type(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    func->v.func.closed = node->v.func.closed;  // 15.13
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            continue;
        }
        if (param->v.param.variadic) {
            func->v.func.variadic = param->v.param.type != NULL
                                        ? chk_resolve_type(c, param->v.param.type)
                                        : chk_simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func,
                            param->v.param.type != NULL
                                ? chk_resolve_type(c, param->v.param.type)
                                : chk_simple(c, LHAT_TYPE_PENDING));
    }
    if (node->v.func.return_type != NULL) {
        // 13.8改: a result is one of the few positions a tuple may be written
        // in. The parameters above were resolved without the permission, so
        // '(A, B)' as an argument is already refused -- which is what leaves
        // 13.7's expansion rule with nothing to expand.
        c->tuple_allowed = true;
        func->v.func.result = chk_resolve_type(c, node->v.func.return_type);
    }
    return func;
}

// 04 の 14.4: a kind is reached through the declaration that introduced it,
// so a type may be a qualified name.
// 05 の 2.2 with 14.7: a name that holds a definition means, as a type, an
// instance of it -- writing the name asks for the whole structure, and that
// is what an instance carries.
static LhatType *as_written_type(LhatType *bound)
{
    LhatType *instance = chk_instance_of(bound);
    return instance != NULL ? instance : bound;
}

// 05 の 2.2: one environment, and a name in it stands for a value, a type, or
// both. Only three things carry a type -- a def^ (its instances, 14.7), an
// errordef^ and its kinds (04 の 2.4), and a host type (05 の 8.8) -- so a
// name bound to anything else is not one that may be written where a type is.
//
// Without this the lookup answered with whatever the name was worth, which
// made every binding a type meaning "what this value is". 'let^ x = 1' then
// admitted 'y : x' for number^, which 2.2 does not say and nothing relies on.
static bool names_a_type(const LhatType *bound)
{
    if (bound == NULL) {
        return true;  // 3.4's gap; nothing here says it is wrong
    }
    switch (bound->kind) {
        // A value and nothing else. 11.3 makes identity structural, so a
        // table -- a def^, a host type (8.8), what require^ answers (6.1) --
        // has a structure that can be asked for, and is left alone here. What
        // these have is a value's worth and no structure at all.
        case LHAT_TYPE_NUMBER:
        case LHAT_TYPE_STRING:
        case LHAT_TYPE_BOOL:
        case LHAT_TYPE_NIL:
        case LHAT_TYPE_FUNC:
        case LHAT_TYPE_CORO:
            return false;
        default:
            return true;
    }
}

static LhatType *resolve_qualified_type(Checker *c, const LhatNode *node)
{
    LhatType *outer = chk_resolve_type(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (outer == NULL || !chk_node_name(c, node->v.access.argument, &name, &length)) {
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (outer->kind == LHAT_TYPE_ERROR_SET) {
        for (const LhatTypeList *k = outer->v.error.kinds; k != NULL;
             k = k->next) {
            if (k->type->v.error.name_length == length &&
                memcmp(k->type->v.error.name, name, length) == 0) {
                return k->type;
            }
        }
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 05 の 6.1: what require^ yields is a structure, so reaching a type out
    // of it is the same member access a value uses. 04 の 14.4 already made a
    // qualified name writable as a type; this is that form over a unit.
    if (outer->kind == LHAT_TYPE_TABLE) {
        const LhatTypeMember *member = chk_find_member(outer, name, length);
        // 2.2 again: a unit publishes values as well as types, and only the
        // types among them may be written here.
        if (member != NULL && names_a_type(member->type)) {
            return as_written_type(member->type);
        }
    }

    chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// 13.8改 asks this of whatever stands beside a tuple in a union. The
// principle is not "errors only": it is that the other arm must be told
// apart by the tag in the head slot, with a construct that does the telling.
// An error qualifies (ISERROR, try^/catch^), and so does nil^ (ISNIL and the
// loop's own isDone; 04 の 11.3's absence) -- which is what types a table's
// walk, whose resume answers (K, V)|nil^. Anything else has no construct
// that discriminates it, so '(A, B)|number^' stays unwritable.
bool chk_may_stand_beside_tuple(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
        case LHAT_TYPE_NIL:
            return true;
        case LHAT_TYPE_UNION:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (!chk_may_stand_beside_tuple(arm->type)) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

// 13.8改 with 04 の 8.2: does an error hide anywhere in this type? A tuple
// position may not carry one -- not directly and not buried in a union --
// because a value beside the good ones is a value a program may drop, which
// is the whole of what 8.2 kept unwritable.
void chk_check_tuple_position(Checker *c, const LhatNode *at,
                              const LhatType *position)
{
    // 04 の 8.2: the error goes around the values, never among them -- the
    // same rule resolve_type applies to a written '(A, SomeError)', asked
    // here of an inferred one.
    if (chk_contains_error(position)) {
        chk_report(c, at, LHAT_CHECK_ERR_TUPLE_ERROR_POSITION);
    }
    // A position is a slot, not a run of slots.
    if (position != NULL && position->kind == LHAT_TYPE_TUPLE) {
        chk_report(c, at, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    }
    // 05 の 8.9: a host value is as wide as its tag says, and a position is
    // one slot. A frame's answer room carries one of the two, never a
    // mixture.
    if (chk_is_hostvalue(position)) {
        chk_report(c, at, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
    }
}

bool chk_contains_error(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
            return true;
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (chk_contains_error(arm->type)) {
                    return true;
                }
            }
            return false;
        default:
            // Deliberately shallow otherwise. An error reachable through a
            // table member or a signature's result is a value that has to be
            // asked for, not one sitting beside the answer.
            return false;
    }
}

// 13.8改: does a tuple hide in this type -- as itself or as a union arm? A
// name may bind neither: the union case is a walk's '(K, V)|nil^', which
// would make the name a maybe-run.
bool chk_contains_tuple(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_TUPLE) {
        return true;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (chk_contains_tuple(arm->type)) {
                return true;
            }
        }
    }
    return false;
}

LhatType *chk_resolve_type(Checker *c, const LhatNode *node)
{
    // 13.8改: the permission is for this one type, not for what it contains.
    // Taking it here means every nested resolve below starts out refusing a
    // tuple, and the few places that may hold one -- a result, a coroutine's
    // Y and T, a union's arms -- hand it back deliberately. Cleared before
    // the NULL test so an absent type consumes it too: an unwritten result
    // must not leave the permission standing for whatever resolves next.
    bool tuple_allowed = c->tuple_allowed;
    c->tuple_allowed = false;

    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
        // An IDENT reaches here from the qualified name of a construction
        // (04 の 2.5), which is written in expression position but names a
        // type.
        case LHAT_NODE_IDENT:
        case LHAT_NODE_TYPE_NAME: {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(c, node, &name, &length)) {
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *builtin = builtin_type(c, name, length);
            if (builtin != NULL) {
                return builtin;
            }

            // 13.13: the type literal this is written inside. Read
            // before the environment, since 05 の 2.2 has no binding for it --
            // a name for a type and nothing else has no value to stand for,
            // and putting one in scope would make it answerable to 8.7's
            // ordering and to every other rule about names.
            //
            // 14.9 already lets a definition say its own type by the name its
            // binding gave it. A literal written where no name is taken has
            // only this: a t^{ … } holding one of
            // itself, or a def^ with no binding, could not be written at all.
            if (chk_name_is(name, length, "Self^")) {
                // 01 の 2.3: one hat is the word, and the rest count
                // outwards -- Self^^ is the literal one further out.
                uint32_t levels = node->v.name.hats > 0 ? node->v.name.hats : 1;
                const struct SelfLink *link = c->self_link;
                for (uint32_t i = 1; link != NULL && i < levels; i++) {
                    link = link->outer;
                }
                if (link == NULL) {
                    chk_report(c, node, LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE);
                    return chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
                return link->type;
            }

            // 13 章 has no such type as self^ or class^. They are the
            // receiver and the definition being built (14.4, 03 の 5.10),
            // and both are values -- 'p^self^;' writes a receiver into a
            // signature, which is resolve_func_type's business, not a name
            // standing for a type.
            //
            // class^ needs saying separately: it is bound to the def^ under
            // construction, so names_a_type below would let it through. What
            // that answers with is a type holding itself, and the relations
            // walk one of those until the stack is gone -- a crash with no
            // diagnostic at all.
            if (chk_name_is(name, length, "self^") ||
                chk_name_is(name, length, "class^")) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            // 05 の 2.2: one environment. A name written as a type is looked
            // up in the same place a value is, which is what 14.9 needs --
            // it says a definition takes its name from its binding, and a
            // binding lives here.
            Binding *declared = chk_scope_find(c->scope, name, length, NULL);
            if (declared == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            // 14.9: written inside the very def^ this name is being bound to,
            // where the binding still holds the collecting pass's pending^
            // seed. What it will mean is already built, so the name says the
            // same thing here as it does anywhere else -- 14.7's instance,
            // which is what Self^ answers with too.
            for (const struct DefLink *d = c->def_link; d != NULL;
                 d = d->outer) {
                if (d->binding == declared && d->instance != NULL) {
                    return d->instance;
                }
            }

            // 2.2 gives a type to a def^ and an errordef^, and to nothing
            // else. Being in scope is not enough to be written here.
            if (!names_a_type(declared->type)) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            return as_written_type(declared->type);
        }

        case LHAT_NODE_MEMBER:
            return resolve_qualified_type(c, node);

        case LHAT_NODE_TYPE_TABLE:
            return resolve_table_type(c, node);

        case LHAT_NODE_TYPE_FUNC:
            return chk_resolve_func_type(c, node);

        case LHAT_NODE_TYPE_TUPLE: {
            // 13.8改: two positions or more, in order. The parser only builds
            // one from a parenthesised list with a ',' in it, so there is no
            // one-position form to reject here.
            if (!tuple_allowed) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_MISPLACED);
            }
            LhatType *tuple = lhat_type_tuple(c->result->types);
            for (const LhatNode *item = node->v.list.items; item != NULL;
                 item = item->next) {
                // c->tuple_allowed is already false, so a tuple written as a
                // position lands on the refusal above -- 13.8改 does not nest
                // them, since a position is a slot and not a run of slots.
                LhatType *position = chk_resolve_type(c, item);
                // The same three a position has to satisfy wherever it comes
                // from. Written positions used to ask only about errors,
                // which let a registered '-> (number^, SomeHostValue)'
                // through -- 05 の 8.9 has no slot to spare for one.
                chk_check_tuple_position(c, item, position);
                lhat_type_add_position(c->result->types, tuple, position);
            }
            return tuple;
        }

        case LHAT_NODE_TYPE_UNION: {
            // 13.8改: a union is transparent to the permission. '(A, B)|E' is
            // written where a result is, and both arms are that result.
            c->tuple_allowed = tuple_allowed;
            LhatType *left = chk_resolve_type(c, node->v.binary.left);
            c->tuple_allowed = tuple_allowed;
            LhatType *right = chk_resolve_type(c, node->v.binary.right);
            // 04 の 3.1: what stands beside a tuple has to be discriminable
            // -- an error or nil^. This keeps 04 の 8.2 all the same: an
            // error among the values rather than around them is the shape
            // 'v, err := f()' needs, and 8.2 leaves it unwritable.
            if ((lhat_type_tuple_width(left) > 0 &&
                 !chk_may_stand_beside_tuple(right)) ||
                (lhat_type_tuple_width(right) > 0 &&
                 !chk_may_stand_beside_tuple(left))) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
            }
            return lhat_type_union(c->result->types, left, right);
        }

        case LHAT_NODE_TYPE_INTERSECT:
            return lhat_type_intersect(c->result->types,
                                       chk_resolve_type(c, node->v.binary.left),
                                       chk_resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_CORO: {
            // 13.9 with 15.3改: 'c^{ f^R -> Y;, T }'. An omitted R or Y is
            // nil^ -- 13.2's absent result and an empty parameter list both
            // mean "nothing here", and 04 の 11.3 already spells that nil^.
            LhatType *receive = node->v.coroutine.receive != NULL
                                    ? chk_resolve_type(c, node->v.coroutine.receive)
                                    : chk_simple(c, LHAT_TYPE_NIL);
            // 13.8改: Y and T are results -- what the body yields and what it
            // finally answers -- so a tuple may be written in either. R is an
            // input and takes none: a resume sends one value.
            LhatType *produce = chk_simple(c, LHAT_TYPE_NIL);
            if (node->v.coroutine.produce != NULL) {
                c->tuple_allowed = true;
                produce = chk_resolve_type(c, node->v.coroutine.produce);
            }
            c->tuple_allowed = true;
            LhatType *result = chk_resolve_type(c, node->v.coroutine.result);
            // 05 の 8.9: what crosses a suspension crosses frames, so none
            // of the three positions carries a host value -- the same rule
            // unify_yield applies to the inferred side.
            if (chk_is_hostvalue(receive) || chk_is_hostvalue(produce) ||
                chk_is_hostvalue(result)) {
                chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            return lhat_type_coro(c->result->types, receive, produce, result,
                                  node->v.coroutine.is_function);
        }

        default:
            chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
}

// ---------------------------------------------------------------------------
// Narrowing helpers (04 の 4.1, 11.7)
// ---------------------------------------------------------------------------

// 04 の 2.3: a set stands for the union of its kinds, so a test against one
// kind has something to take apart. Without this, narrowing a value typed as
// IOError by IOError.NotFound would find nothing to remove.
static LhatType *expand_set(Checker *c, LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_ERROR_SET) {
        return type;
    }
    LhatType *expanded = NULL;
    for (const LhatTypeList *k = type->v.error.kinds; k != NULL; k = k->next) {
        expanded = lhat_type_union(c->result->types, expanded, k->type);
    }
    return expanded != NULL ? expanded : type;
}

// The part of `type` that could still be `wanted`. This is the true side of
// 13.11's narrowing, and 04 の 5.3 uses it to name the errors a try^ lets
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
// gone. The false side of 13.11, and what catch^ and ?? do to their left.
LhatType *chk_without(Checker *c, LhatType *type, LhatType *unwanted)
{
    if (type == NULL) {
        return NULL;
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
    return type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
           type->kind == LHAT_TYPE_PENDING ||
           !lhat_type_disjoint(type, wanted);
}

// 13.2: nothing inhabits "no value", so it cannot stand where one is wanted.
// The positions that want a value ask here, rather than infer_call reporting
// it -- 8.2 makes a call on its own a statement, and that is exactly where a
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

// 11.8: an operator is a member whose name is the operator itself. NULL for
// the ones no op^ may define -- 11.8 keeps and^, or^ and '!' built in, and
// 11.5's comparisons decide by 14.12's disjointness rather than by asking.
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

// 11.8: whether this member name is an operator. These spellings are the
// ones op^ writes, and 01 の 6 章 keeps a program from writing any of them
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

// 11.8: an operator is an f^ that takes self^ and one argument. 11.1 makes it
// a function -- a p^ could carry side effects into an operator -- and 14.4
// puts the left operand in self^, which leaves the right one as the single
// parameter. Nothing later checks this: the call site reads the signature and
// believes it, and the machine hands over a receiver and one argument
// whatever the body declared.
// 11.9: and a '<=>' answers a number^, since the four orderings are
// read off it by asking which side of zero the answer falls.
//
// 11.8改: '-' is the one operator written both ways, so it also takes the
// shape with no argument at all -- that is the unary one. Told apart by the
// count, which is what lets 14.12 hold both under the one name. The other
// spellings keep refusing it: there is no unary '..' for it to mean.
void chk_check_operator_shape(Checker *c, const LhatNode *at,
                              const LhatType *type, const char *name,
                              size_t length)
{
    bool compares = length == 3 && memcmp(name, "<=>", 3) == 0;
    bool equals = length == 1 && name[0] == '=';  // 11.9改
    bool may_be_unary = length == 1 && name[0] == '-';
    if (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING) {
        return;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        // 14.12: an overloaded one is every arm at once, and each has to be
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
    // 15.7改: and it may not be yieldable. Not because it is an f^ -- 15.3改
    // lets one of those suspend -- but because 15.5 has a yieldable call
    // answer a coroutine, where the signature above says it answers T.
    //
    // 11.3改: which side the self^ was written on is not judged here.
    // Either way the receiver is out of `params`, so the count is the same,
    // and both spellings are operators -- the position only says which
    // operand the receiver is.
    // 11.8改: a unary one takes self^ and nothing else. 11.3改's trailing
    // self^ cannot arise there -- with no other parameter, the self^ is the
    // first, and chk_self_marker_at reads that as the leading one.
    bool shaped = params == 1 || (may_be_unary && params == 0);
    if (type->kind != LHAT_TYPE_FUNC || !type->v.func.is_function ||
        !type->v.func.takes_self || !shaped || type->v.func.yields) {
        chk_report(c, at, LHAT_CHECK_ERR_BAD_OPERATOR);
        return;
    }
    // 11.9: '(a <=> b) < 0' is what an ordering reads, so the answer has
    // to be a thing zero can be on one side of.
    if (compares && (type->v.func.result == NULL ||
                     !lhat_type_conforms(type->v.func.result,
                                         chk_simple(c, LHAT_TYPE_NUMBER)))) {
        chk_report(c, at, LHAT_CHECK_ERR_COMPARE_NOT_NUMBER);
    }
    // 11.9改: an '=' is read as it stands rather than against zero, and
    // 5.4 makes bool^ the one thing a condition may be -- so that is what
    // the answer has to be.
    if (equals && (type->v.func.result == NULL ||
                   !lhat_type_conforms(type->v.func.result,
                                       chk_simple(c, LHAT_TYPE_BOOL)))) {
        chk_report(c, at, LHAT_CHECK_ERR_EQUAL_NOT_BOOL);
    }
}

// 11.3改: and nothing but an op^ has any use for a trailing self^. A
// call written 'x.m(y)' takes its receiver from what stands before the dot,
// so a member saying the receiver is its last argument says nothing 14.4 can
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

// 11.8: what a built-in type answers. The checker knows these rather than any
// L^ writing them out, the way 15.6 gives a coroutine start(). 14.4 puts the
// left operand in self^, so the right one is the only parameter.
LhatType *chk_builtin_operator(Checker *c, LhatTypeKind carrier,
                               const char *name, size_t length)
{
    // 11.9: number^ and string^ each order their own, so both carry the
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
        takes = LHAT_TYPE_STRING;  // 11.2: joining two strings answers one
    } else if (carrier == LHAT_TYPE_NUMBER && length > 0 && name[0] != '.') {
        // 14.8 makes number^ one type, and every arithmetic operator on it
        // takes and answers one. '..' is the only name reaching here that
        // starts with a dot, and joining numbers is not arithmetic (11.2).
        takes = LHAT_TYPE_NUMBER;
    } else {
        // 11.8: bool^ carries none. and^, or^ and '!' are the built-in
        // logic and nothing writes over them.
        return NULL;
    }
    LhatType *signature = lhat_type_func(c->result->types, true);
    signature->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, signature, chk_simple(c, takes));
    signature->v.func.result = chk_simple(c, takes);
    return signature;
}

// 11.1: an operator is a function, and 11.3 asks structurally whether a type
// carries it. 01 の 6 章 spells a member name as an identifier, so an
// operator is a name no program can write by hand and nothing of the
// writer's can collide with it.
LhatType *chk_operator_member(Checker *c, const LhatType *type,
                              const char *name, size_t length)
{
    if (type == NULL || name == NULL) {
        return NULL;
    }
    // 05 の 8.9: a host value carries its registered operators in the same
    // member list a table carries 11.8's.
    if (type->kind == LHAT_TYPE_TABLE || type->kind == LHAT_TYPE_HOSTVALUE) {
        const LhatTypeMember *m = chk_find_member(type, name, length);
        return m != NULL ? m->type : NULL;
    }
    return chk_builtin_operator(c, type->kind, name, length);
}

// 03 の 3.5: a gap in inference, and 13.7's any^ which is every value at
// once, are both left to the machine rather than reported here.
bool chk_operator_undecided(const LhatType *type)
{
    return type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
           type->kind == LHAT_TYPE_PENDING || type->kind == LHAT_TYPE_ANY;
}

// ---------------------------------------------------------------------------
// Narrowing (13.11)
// ---------------------------------------------------------------------------

// 13.11 limits narrowing to a name and to a dot path from one. A call or an
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
            return true;
        case LHAT_NODE_MEMBER:
            return !node->v.access.nil_safe && chk_narrowable(node->v.access.target);
        default:
            return false;
    }
}

// 13.11: the nil^ written down, which is what makes a comparison against it a
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

// 01 の 2.3: the canonical name cuts after the first hat, so it^ and
// it^^ spell the same name reaching two different bindings -- a narrowing
// recorded for one must not apply to the other, which is what comparing the
// counts guards.
static size_t name_hats(const LhatNode *node)
{
    if (node == NULL) {
        return 0;
    }
    // 16.2: the focus with no name written is it^ -- one hat, so a written
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
    if (a == NULL || b == NULL || a->kind != b->kind) {
        return false;
    }
    if (a->kind == LHAT_NODE_MEMBER) {
        return same_path(c, a->v.access.target, b->v.access.target) &&
               same_name(c, a->v.access.argument, b->v.access.argument);
    }
    return same_name(c, a, b);
}

// The name a path starts from, which is what a reassignment invalidates.
static const LhatNode *path_root(const LhatNode *node)
{
    while (node != NULL && node->kind == LHAT_NODE_MEMBER) {
        node = node->v.access.target;
    }
    return node;
}

LhatType *chk_narrowed_type(Checker *c, const LhatNode *path)
{
    for (const Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        if (same_path(c, n->path, path)) {
            return n->type;
        }
    }
    return NULL;
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

void chk_pop_narrowings(Checker *c, Narrowing *mark)
{
    while (c->narrowings != mark) {
        Narrowing *next = c->narrowings->next;
        lhat_free(c->narrowings);
        c->narrowings = next;
    }
}

// 13.11: reassigning ends it, since the claim was about the value that was
// examined. A let^ in the branch does the same by introducing a new name.
//
// Emptied rather than unlinked: a branch remembers where its narrowings begin
// so it can pop them, and removing a node from under that mark would leave it
// pointing at freed memory. An emptied entry reports nothing, which sends the
// caller back to the binding -- the answer wanted here anyway.
void chk_drop_narrowings_for(Checker *c, const LhatNode *target)
{
    const LhatNode *root = path_root(target);
    for (Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        if (same_name(c, path_root(n->path), root)) {
            n->type = NULL;
        }
    }
}

// 02 の 9.8: a break^ leaves as many loops as its level names, counting the
// innermost as 1. Answers whether this loop has a way out written in it --
// `depth` is how many loops stand between the statements being read and the
// one being asked about, so a break^ leaves it when its level reaches past
// them. Whether it is that loop's normal end or one it is only passed
// through does not matter here: either way control leaves.
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
                // 9 章: the clauses of a loop body are statements of it too.
                for (const LhatNode *clause = node->v.list.extra;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out_from(clause->v.loop_clause.body, depth)) {
                        return true;
                    }
                }
                break;

            case LHAT_NODE_IF_STMT:
                for (const LhatNode *clause = node->v.list.items;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out_from(clause->v.clause.body, depth)) {
                        return true;
                    }
                }
                break;

            case LHAT_NODE_WITH:
                if (breaks_out_from(node->v.list.extra, depth)) {
                    return true;
                }
                break;

            case LHAT_NODE_FOR:
                // 16.3 and 17 章: the if^, when^ and do^ forms do not iterate,
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

// Whether control cannot reach the end of this statement. 04 の 6.1 is
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
        case LHAT_NODE_PANIC:
            return true;

        case LHAT_NODE_BLOCK:
            for (const LhatNode *s = node->v.list.items; s != NULL;
                 s = s->next) {
                if (chk_always_exits(s)) {
                    return true;
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
            return has_else;
        }

        // 16.5: a repeat^ with no bound runs until something leaves it. With
        // no break^ of its own, nothing after it is ever reached, so it ends
        // the statements around it the way a return^ does. 03 の 3.4 counts
        // exits to infer a result, and this is one that produces no value
        // and never happens -- which keeps it out of the result entirely.
        case LHAT_NODE_REPEAT:
            return node->v.repeat.kind == LHAT_REPEAT_FOREVER &&
                   !breaks_out(node->v.repeat.body);

        // 02 の 12.1: a with^ runs its block once and always. Nothing stands
        // between the bindings and the body, which is why the checker walks
        // it outside c->conditional where a loop's or an if^'s body is
        // inside. So what the block does is what the with^ does -- and 04 の
        // 5.1's 'with^ h = try^ open(p) { return^ … }' is a body that answers
        // on every path, which is what it was written to be.
        case LHAT_NODE_WITH:
            return chk_always_exits(node->v.list.extra);

        default:
            return false;
    }
}

// The narrowings a condition implies when it holds, or when it does not.
//
// 13.11: and^ tells us both sides held when it is true, or^ tells us both
// failed when it is false, and neither says anything in the other direction
// -- one of the two could have decided it alone.
void chk_narrow_from(Checker *c, const LhatNode *condition, bool truth)
{
    if (condition == NULL) {
        return;
    }

    // 13.11 with 04 の 11.3: three shapes narrow. 'x isa^ T' asks about a
    // type; '=' / '!=' / 'is^' against the nil^ literal asks about absence,
    // which 11.3 spells with nil^ and is the first thing anyone writes; and
    // 11.7改2's 'x?' is the second of those written short. Without the nil^
    // ones, 'if^ t != nil^ { t[1] }' passed the condition and then reported
    // against t[1] -- a diagnostic nowhere near its cause.
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
        // 11.7改2: 'x?' is '!(x isa^ nil^)', so it narrows to exactly
        // what that would -- the true side without the nil^ arm.
        if (condition->v.unary.op != LHAT_OP_PRESENT) {
            return;
        }
        path = condition->v.unary.operand;
        tested = chk_simple(c, LHAT_TYPE_NIL);
        holds = !truth;
    } else if (condition->kind == LHAT_NODE_BINARY) {
        LhatOpKind op = condition->v.binary.op;
        if ((op == LHAT_OP_AND && truth) || (op == LHAT_OP_OR && !truth)) {
            chk_narrow_from(c, condition->v.binary.left, truth);
            chk_narrow_from(c, condition->v.binary.right, truth);
            return;
        }
        if (op == LHAT_OP_ISA) {
            path = condition->v.binary.left;
            tested = chk_resolve_type(c, condition->v.binary.right);
        } else if (op == LHAT_OP_EQ || op == LHAT_OP_NE || op == LHAT_OP_IS) {
            // Either side may carry the literal; the other is the path.
            // 'is^' rides with '=' because nil^ has one value, so 13.11's
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

    LhatType *current = chk_infer(c, path);
    LhatType *inside =
        holds ? chk_only(c, current, tested) : chk_without(c, current, tested);

    // 03 の 3.4: a parameter still being decided says nothing to narrow, so
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

// ---------------------------------------------------------------------------
// Parameter types (03 の 3.4)
// ---------------------------------------------------------------------------

// A parameter still being decided, found by the pending^ object standing for
// it. 13.11's narrowing is what keeps a narrowed use from arriving here: infer
// answers a narrowed path with the narrowed type, which is a different object,
// so the demand a branch makes of the narrowed value never reaches the
// parameter itself.
ParamVar *chk_param_var_for(Checker *c, const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_PENDING) {
        return NULL;
    }
    for (ParamVar *pv = c->param_vars; pv != NULL; pv = pv->next) {
        if (pv->slot == type) {
            return pv;
        }
    }
    return NULL;
}

// 15.3改: whether an f^ coroutine is reachable anywhere in this type. The
// result of an f^ is read with this, since one reaches the outside through a
// member or a nested signature as readily as by being the result itself.
bool chk_mentions_function_coroutine(const LhatType *type, unsigned depth)
{
    if (type == NULL || depth > 8) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_CORO:
            return type->v.coroutine.is_function;

        case LHAT_TYPE_TABLE:
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (chk_mentions_function_coroutine(m->type, depth + 1)) {
                    return true;
                }
            }
            return chk_mentions_function_coroutine(type->v.table.variadic,
                                                   depth + 1);

        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (chk_mentions_function_coroutine(p->type, depth + 1)) {
                    return true;
                }
            }
            return chk_mentions_function_coroutine(type->v.func.result, depth + 1) ||
                   chk_mentions_function_coroutine(type->v.func.variadic, depth + 1);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (chk_mentions_function_coroutine(arm->type, depth + 1)) {
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
}

// Whether a demand would contain the very variable it is about, which would
// leave the settled type pointing at itself.
static bool demand_mentions(const LhatType *type, const LhatType *slot,
                            unsigned depth)
{
    if (type == NULL || depth > 8) {
        return false;
    }
    if (type == slot) {
        return true;
    }
    switch (type->kind) {
        case LHAT_TYPE_TABLE:
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (demand_mentions(m->type, slot, depth + 1)) {
                    return true;
                }
            }
            return demand_mentions(type->v.table.variadic, slot, depth + 1);

        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (demand_mentions(p->type, slot, depth + 1)) {
                    return true;
                }
            }
            return demand_mentions(type->v.func.result, slot, depth + 1) ||
                   demand_mentions(type->v.func.variadic, slot, depth + 1);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (demand_mentions(arm->type, slot, depth + 1)) {
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
}

// Two demands held at once. 14.5's intersection is the general answer, but two
// structural demands describe one table rather than two: 14.10 reads 't^{ … }'
// as "at least these", so asking for 'a' and asking for 'b' is asking for a
// table carrying both, and writing that out is what the writer would have
// written by hand.
static LhatType *merge_demand(Checker *c, LhatType *a, LhatType *b)
{
    if (a == NULL) {
        return b;
    }
    if (b == NULL || lhat_type_equal(a, b)) {
        return a;
    }
    if (a->kind != LHAT_TYPE_TABLE || b->kind != LHAT_TYPE_TABLE ||
        a->v.table.is_definition || b->v.table.is_definition ||
        a->v.table.nominal || b->v.table.nominal) {
        return lhat_type_intersect(c->result->types, a, b);
    }

    LhatType *merged = lhat_type_table(c->result->types);
    if (merged == NULL) {
        return a;
    }
    for (const LhatTypeMember *m = a->v.table.members; m != NULL; m = m->next) {
        lhat_type_add_member(c->result->types, merged, m->name, m->name_length,
                             m->type);
    }
    for (const LhatTypeMember *m = b->v.table.members; m != NULL; m = m->next) {
        LhatTypeMember *into =
            (LhatTypeMember *)chk_find_member(merged, m->name, m->name_length);
        if (into != NULL) {
            into->type = merge_demand(c, into->type, m->type);
        } else {
            lhat_type_add_member(c->result->types, merged, m->name,
                                 m->name_length, m->type);
        }
    }
    merged->v.table.variadic =
        merge_demand(c, a->v.table.variadic, b->v.table.variadic);
    return merged;
}

// 03 の 3.4: `value` was used somewhere that wants `wanted`. When the value is
// a parameter whose type is still being decided, that is a demand on it rather
// than something to report.
void chk_constrain(Checker *c, LhatType *value, LhatType *wanted)
{
    ParamVar *pv = chk_param_var_for(c, value);
    if (pv == NULL || pv->closed || wanted == NULL) {
        return;
    }
    // A position that would take anything says nothing about what arrives.
    switch (wanted->kind) {
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
        case LHAT_TYPE_ANY:
        case LHAT_TYPE_NONE:
            return;
        default:
            break;
    }
    if (demand_mentions(wanted, pv->slot, 0)) {
        pv->closed = true;
        return;
    }

    // 3.4: only what always runs decides. What a branch asks is added when it
    // agrees with that, and dropped when it does not -- the branch reports for
    // itself, and intersecting the two would leave a type no value inhabits.
    if (c->conditional > 0) {
        if (pv->conditional_dropped) {
            return;
        }
        if (pv->conditional == NULL) {
            pv->conditional = wanted;
        } else if (lhat_type_disjoint(pv->conditional, wanted)) {
            pv->conditional = NULL;
            pv->conditional_dropped = true;
        } else {
            pv->conditional = merge_demand(c, pv->conditional, wanted);
        }
        return;
    }

    if (pv->demanded == NULL) {
        pv->demanded = wanted;
    } else if (lhat_type_disjoint(pv->demanded, wanted)) {
        pv->closed = true;  // 3.4: undecided rather than reported
    } else {
        pv->demanded = merge_demand(c, pv->demanded, wanted);
    }
}

// 3.4: what arrives is no longer what the name holds, so nothing after this
// says anything about the parameter.
void chk_close_param_var(Checker *c, const LhatType *value)
{
    ParamVar *pv = chk_param_var_for(c, value);
    if (pv != NULL) {
        pv->closed = true;
    }
}

// 3.4: reading a member off a parameter demands a structure carrying it. The
// member's own type is left unknown^ -- what may be done with it is decided by
// what the member turns out to be, and a call through it says too little to
// pin a signature down (13.1 asks the f^/p^ distinction and 13.2 the presence
// of a result, and neither is readable off one call).
void chk_constrain_member(Checker *c, LhatType *target, const char *name,
                          size_t length)
{
    if (chk_param_var_for(c, target) == NULL) {
        return;
    }
    LhatType *shape = lhat_type_table(c->result->types);
    if (shape == NULL ||
        lhat_type_add_member(c->result->types, shape, name, length,
                             chk_simple(c, LHAT_TYPE_UNKNOWN)) == NULL) {
        return;
    }
    chk_constrain(c, target, shape);
}

ParamVar *chk_push_param_var(Checker *c, LhatType *slot)
{
    ParamVar *pv = (ParamVar *)lhat_calloc(1, sizeof *pv);
    if (pv == NULL) {
        return NULL;
    }
    pv->slot = slot;
    pv->next = c->param_vars;
    c->param_vars = pv;
    return pv;
}

// The body has been read, so the demands are all in. Written through the slot
// rather than returned: the binding and the signature hold that same pointer,
// which is what let a single walk decide both.
void chk_settle_param_vars(Checker *c, ParamVar *mark)
{
    while (c->param_vars != mark) {
        ParamVar *pv = c->param_vars;
        c->param_vars = pv->next;

        LhatType *settled = NULL;
        if (!pv->closed) {
            settled = pv->demanded;
            if (settled == NULL) {
                settled = pv->conditional_dropped ? NULL : pv->conditional;
            } else if (pv->conditional != NULL &&
                       !lhat_type_disjoint(settled, pv->conditional)) {
                settled = merge_demand(c, settled, pv->conditional);
            }
        }
        // 3.4: nothing was demanded, or the demands did not agree. Inference
        // did not decide, so the slot is left as what it already is -- a
        // constraint nobody wrote, which 3.1 reports under strict and
        // relaxed forgives (3.5: no check is inserted; a real mismatch
        // meets the machine's own instruction check where it lands). Not
        // any^: 3.5 makes that the reading under which relaxed would be the
        // stricter setting.
        if (settled != NULL && settled != pv->slot) {
            *pv->slot = *settled;
        }
        lhat_free(pv);
    }
}

// ---------------------------------------------------------------------------
// 03 の 3.4改2: walking the same ground again
// ---------------------------------------------------------------------------

// Opens the loop. `count` is how many elements are being walked, which is what
// bounds the number of walks.
void chk_rounds_begin(Checker *c, Rounds *r, size_t count)
{
    r->diagnostics = c->result->diagnostic_count;
    r->resolutions = c->result->resolution_count;
    r->round = 0;
    r->cap = count + 1;
    r->changed = false;
    r->read_provisional_outside = c->read_provisional;
    c->read_provisional = false;
}

// Answers whether to walk again, and rolls the diagnostics back when it does.
// The caller has just finished a walk and has said, through `r->changed`,
// whether any answer moved.
bool chk_rounds_next(Checker *c, Rounds *r)
{
    // Nothing was read ahead, so another walk reads the same things and says
    // the same things -- or this walk answered exactly what the last one did,
    // which is the fixpoint. Either way what it has just said stands.
    if (!c->read_provisional || !r->changed || r->round + 1 >= r->cap) {
        return false;
    }
    r->round++;
    r->changed = false;
    c->read_provisional = false;
    c->result->diagnostic_count = r->diagnostics;
    c->result->resolution_count = r->resolutions;
    return true;
}

// Closes the loop. A def^ or a statement list inside this one ran its own
// walks, and a seed it read may as easily have been this one's as its own --
// the two are not told apart, so the reading is carried outward. One walk too
// many costs a walk; one too few costs the writer an annotation.
void chk_rounds_end(Checker *c, Rounds *r)
{
    c->read_provisional = r->read_provisional_outside || c->read_provisional;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void lhat_check_unit(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                     LhatTypeArena *arena, const LhatRequire *require,
                     LhatCheckResult *result)
{
    memset(result, 0, sizeof *result);
    if (arena != NULL) {
        result->types = arena;
    } else {
        lhat_type_arena_init(&result->owned);
        result->types = &result->owned;
    }

    if (unit == NULL || lexer == NULL) {
        return;
    }

    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = NULL;

    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.lexer = lexer;
    checker.result = result;
    checker.strict = strict;
    checker.scope = &scope;
    if (require != NULL) {
        checker.require = *require;
    }

    // 05 の 3 章: read before the statements, so a diagnostic about the path
    // does not wait behind the body.
    result->module_name = chk_read_module_name(&checker, unit->v.list.items);

    chk_check_statements(&checker, unit->v.list.items);

    // 05 の 4 章: gathered once the whole unit has been checked, so a public^
    // name written late is not missed. 8.7 already makes every name visible
    // throughout the scope, so this only reads what is there.
    result->exports = chk_collect_exports(&checker, unit->v.list.items);

    chk_scope_dispose(&scope);
}

// 03 の 4.3: the names the inputs so far have bound, with their types. The
// names are copies -- a Binding points into the source it was read from, and
// that source goes when its input does. The arena is the session's so the
// types outlive any one input, the same way 05 の 6 章 shares one across a
// program's units.
struct LhatCheckSession {
    LhatTypeArena types;
    struct {
        char *name;
        size_t length;
        LhatType *type;
        // 8.9: which word bound it, so that a let^ in one input is still a
        // let^ when a later one writes ':=' -- and a var^ over it makes the
        // name writable again, the way 03 の 4.3 makes a redefinition the
        // same place written afresh.
        bool immutable;
    } *names;
    size_t count;
    size_t capacity;

    // 05 の 8.6: made on the first input that mentions L^ and kept, so a
    // module one input registers is still there in the next.
    LhatType *environment;

    // 02 の 14.16: made on the first input that mentions typeof^ and kept,
    // the same way -- so a typeof^ result bound in one input is still the
    // same nominal type when a later input names it in a comparison.
    LhatType *typeinfo_type;

    // 05 の 8.6 and 8.2: what the host put in L^, and the names it bound to
    // members of it. A prompt has no LhatProgram to carry these.
    LhatType *globals;
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // 05 の 8.7: the host registry import^ resolves against, when a host made
    // an LhatProgram to hold one and handed it over. NULL when it did not,
    // and then a prompt's import^ finds nothing.
    LhatType *hosted;
};

bool lhat_check_session_global(LhatCheckSession *session, const char *name,
                               const char *signature)
{
    if (session == NULL || name == NULL || signature == NULL) {
        return false;
    }
    if (session->globals == NULL) {
        session->globals = lhat_type_table(&session->types);
        if (session->globals == NULL) {
            return false;
        }
    }
    LhatType *written =
        lhat_type_of_text(signature, strlen(signature), &session->types, NULL);
    return written != NULL &&
           lhat_type_add_member(&session->types, session->globals, name,
                                strlen(name), written) != NULL;
}

void lhat_check_session_bind(LhatCheckSession *session,
                             const char *const *names,
                             const char *const *members, size_t count)
{
    if (session == NULL) {
        return;
    }
    session->initial_names = names;
    session->initial_members = members;
    session->initial_count = count;
}

void lhat_check_session_hosted(LhatCheckSession *session, LhatType *hosted,
                               LhatType *globals)
{
    if (session == NULL) {
        return;
    }
    session->hosted = hosted;
    // 8.6: the two arrive together, since both are what the same registry
    // registered. Replacing rather than merging is what makes handing over a
    // program the whole answer -- what lhat_check_session_global built in the
    // session's own arena was the other way of answering the same question.
    if (globals != NULL) {
        session->globals = globals;
    }
}

LhatCheckSession *lhat_check_session_new(void)
{
    LhatCheckSession *session =
        (LhatCheckSession *)lhat_calloc(1, sizeof *session);
    if (session != NULL) {
        lhat_type_arena_init(&session->types);
    }
    return session;
}

void lhat_check_session_dispose(LhatCheckSession *session)
{
    if (session == NULL) {
        return;
    }
    for (size_t i = 0; i < session->count; i++) {
        lhat_free(session->names[i].name);
    }
    lhat_free(session->names);
    lhat_type_arena_dispose(&session->types);
    lhat_free(session);
}

// Keeps what an input bound, replacing what an earlier one bound under the
// same name -- 8.7 makes a second let^ a new name, and at the top level of a
// REPL that is what a writer means by it.
static void session_keep(LhatCheckSession *session, const char *name,
                         size_t length, LhatType *type, bool immutable)
{
    for (size_t i = 0; i < session->count; i++) {
        if (session->names[i].length == length &&
            memcmp(session->names[i].name, name, length) == 0) {
            session->names[i].type = type;
            session->names[i].immutable = immutable;
            return;
        }
    }
    LHAT_GROW(session->names, session->count, session->capacity, 16, return);
    char *kept = (char *)lhat_alloc(length + 1);
    if (kept == NULL) {
        return;
    }
    memcpy(kept, name, length);
    kept[length] = '\0';
    session->names[session->count].name = kept;
    session->names[session->count].length = length;
    session->names[session->count].type = type;
    session->names[session->count].immutable = immutable;
    session->count++;
}

void lhat_check_next(LhatCheckSession *session, const LhatNode *unit,
                     const LhatLexer *lexer, bool strict,
                     LhatCheckResult *result)
{
    memset(result, 0, sizeof *result);
    result->types = &session->types;

    if (unit == NULL || lexer == NULL) {
        return;
    }

    // 03 の 4.3: one scope across the whole session. A name written again is
    // the same place written again, so the top level of a prompt keeps the
    // slot it had rather than taking another.
    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = NULL;

    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.lexer = lexer;
    checker.result = result;
    checker.strict = strict;
    checker.scope = &scope;
    checker.session = true;  // 05 の 8.9: a prompt's top level, see Checker
    // 05 の 8.6: one L^ for the whole session, so what an input registers in
    // it is still there in the next.
    checker.environment = session->environment;
    checker.typeinfo_type = session->typeinfo_type;
    // 05 の 8.6 and 8.2: what the host put in L^ and bound names to. A file
    // gets these through LhatRequire; a prompt has no program to carry them.
    checker.require.globals = session->globals;
    checker.require.initial_names = session->initial_names;
    checker.require.initial_members = session->initial_members;
    checker.require.initial_count = session->initial_count;
    // 05 の 8.7: and what import^ resolves against, when a host handed a
    // registry over. NULL leaves import^ finding nothing, as before.
    checker.require.hosted = session->hosted;

    // 03 の 4.3: the last statement is what the input answers with, when it
    // is an expression. What that changes here is 15.8's reasoning about a
    // call whose value goes nowhere.
    if (unit->v.list.items != NULL) {
        const LhatNode *last = unit->v.list.items;
        while (last->next != NULL) {
            last = last->next;
        }
        if (last->kind == LHAT_NODE_CALL_STMT) {
            checker.answering = last;
        }
    }

    // What earlier inputs bound is settled already -- 8.7's "used before
    // defined" is about the statements of this input.
    for (size_t i = 0; i < session->count; i++) {
        Binding *b = chk_scope_add(&scope, session->names[i].name,
                                   session->names[i].length,
                                   session->names[i].type, 0);
        if (b != NULL) {
            b->reached = true;
            b->from_session = true;
            b->immutable = session->names[i].immutable;
        }
    }

    chk_check_statements(&checker, unit->v.list.items);
    result->exports = chk_collect_exports(&checker, unit->v.list.items);

    // What this input bound joins the session, replacing what an earlier one
    // bound under the same name. The types are in the session's arena
    // already, so only the names are copied.
    for (Binding *b = scope.bindings; b != NULL; b = b->next) {
        session_keep(session, b->name, b->name_length, b->type, b->immutable);
    }
    session->environment = checker.environment;
    session->typeinfo_type = checker.typeinfo_type;

    chk_scope_dispose(&scope);
}

void lhat_check(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                LhatCheckResult *result)
{
    lhat_check_unit(unit, lexer, strict, NULL, NULL, result);
}

// 05 の 8.7: from written text to a type, with no unit around it. The
// checker needs a scope for the names a type may mention, so `named`'s
// members are seeded into one -- which is how a registration names a type an
// earlier one made, and why nothing else is reachable from here.
LhatType *lhat_type_of_text(const char *text, size_t length,
                            LhatTypeArena *arena, LhatType *named)
{
    LhatSource source;
    if (!lhat_source_init_from_string(&source, "<signature>", text, length)) {
        return NULL;
    }

    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);

    LhatParseResult parsed;
    lhat_parse_type_only(&lexer, &parsed);

    LhatType *type = NULL;
    if (parsed.diagnostic_count == 0 && lexer.diagnostic_count == 0 &&
        parsed.root != NULL) {
        LhatCheckResult result;
        memset(&result, 0, sizeof result);
        result.types = arena;

        Scope scope;
        scope.bindings = NULL;
        scope.tail = NULL;
        scope.parent = NULL;

        Checker checker;
        memset(&checker, 0, sizeof checker);
        checker.lexer = &lexer;
        checker.result = &result;
        checker.strict = true;
        checker.scope = &scope;

        if (named != NULL && named->kind == LHAT_TYPE_TABLE) {
            for (const LhatTypeMember *m = named->v.table.members; m != NULL;
                 m = m->next) {
                Binding *b = chk_scope_add(&scope, m->name, m->name_length,
                                           m->type, 0);
                if (b != NULL) {
                    b->reached = true;
                }
            }
        }

        type = chk_resolve_type(&checker, parsed.root);
        // A name the scope does not hold resolves to UNKNOWN rather than to
        // nothing, and a signature that says nothing is not one.
        if (type != NULL &&
            (type->kind == LHAT_TYPE_UNKNOWN || result.diagnostic_count > 0)) {
            type = NULL;
        }
        lhat_check_result_dispose(&result);
        chk_scope_dispose(&scope);
    }

    lhat_parse_result_dispose(&parsed);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return type;
}

const LhatResolution *lhat_check_resolution_at(const LhatCheckResult *result,
                                               uint32_t offset)
{
    if (result == NULL) {
        return NULL;
    }
    // Recorded in walk order, which is source order for the names within one
    // unit, so this can halve its way in rather than scanning.
    size_t low = 0;
    size_t high = result->resolution_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const LhatResolution *entry = &result->resolutions[mid];
        if (offset < entry->use) {
            high = mid;
        } else if (offset >= entry->use_end) {
            low = mid + 1;
        } else {
            return entry;
        }
    }
    return NULL;
}

void lhat_check_result_dispose(LhatCheckResult *result)
{
    lhat_free(result->diagnostics);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->diagnostic_capacity = 0;
    lhat_free(result->resolutions);
    result->resolutions = NULL;
    result->resolution_count = 0;
    result->resolution_capacity = 0;
    lhat_free(result->module_name);
    result->module_name = NULL;
    // Only the arena this result made for itself; a shared one belongs to
    // whoever passed it in.
    if (result->types == &result->owned) {
        lhat_type_arena_dispose(&result->owned);
    }
    result->types = NULL;
}

const char *lhat_check_error_message(LhatCheckErrorCode code)
{
    switch (code) {
        case LHAT_CHECK_ERR_NONE:
            return "no error";
        case LHAT_CHECK_ERR_UNDEFINED:
            return "no such name in scope";
        case LHAT_CHECK_ERR_USED_BEFORE_DEFINED:
            return "this name is read before its let^ has run";
        case LHAT_CHECK_ERR_REDEFINED:
            return "this name is already defined in this scope";
        case LHAT_CHECK_ERR_UNKNOWN_TYPE:
            return "no such type";
        case LHAT_CHECK_ERR_MISMATCH:
            return "this value does not fit where it is written";
        case LHAT_CHECK_ERR_NOT_NUMBER:
            return "arithmetic needs number^";
        case LHAT_CHECK_ERR_NOT_BOOL:
            return "this has to be bool^";
        case LHAT_CHECK_ERR_NOT_CALLABLE:
            return "this is not a function or a procedure";
        case LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE:
            return "f^ may call only f^, and this callee is a p^";
        case LHAT_CHECK_ERR_ARITY:
            return "the wrong number of arguments";
        case LHAT_CHECK_ERR_NOT_VARIADIC:
            return "'...' spreads into a variadic tail, and this callee "
                   "takes none";
        case LHAT_CHECK_ERR_NO_MEMBER:
            return "this value has no such member";
        case LHAT_CHECK_ERR_CANNOT_FAIL:
            return "the left of catch^ or try^ cannot return an error";
        case LHAT_CHECK_ERR_CANNOT_BE_NIL:
            return "the left of ?? cannot be nil^";
        case LHAT_CHECK_ERR_TRY_OUTSIDE:
            return "try^ would return an error this subroutine cannot return";
        case LHAT_CHECK_ERR_REQUIRE_FAILED:
            return "this unit could not be required";
        case LHAT_CHECK_ERR_NOT_COROUTINE:
            return "this has no coroutine to walk or delegate to";
        case LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION:
            return "a yield^ that is bound needs a written type there";
        case LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH:
            return "every yield^ in one body has to agree on what it sends "
                   "and what it answers";
        case LHAT_CHECK_ERR_PATH_NOT_TABLE:
            return "this holds the name written after it, so it has to be a "
                   "table";
        case LHAT_CHECK_ERR_PATH_IS_DEFINITION:
            return "a def^ says what its instances carry, so a member cannot "
                   "be added to one here";
        case LHAT_CHECK_ERR_MODULE_UNNAMED:
            return "this unit declares no module^, so there is no path to "
                   "bind it under; write 'let^ name = require^ ...' instead";
        case LHAT_CHECK_ERR_NOT_HOSTED:
            return "import^ reaches what the host registered, and no module "
                   "of this name is there; a unit read from a file comes in "
                   "with require^ \"path\"";
        case LHAT_CHECK_ERR_COROUTINE_DROPPED:
            return "this call makes a coroutine and runs none of the body; "
                   "write yieldall^ to delegate, or let^ to keep it";
        case LHAT_CHECK_ERR_MISSING_FIELD:
            return "this field has no default, so it has to be written";
        case LHAT_CHECK_ERR_INCOMPARABLE:
            return "these can never be equal, so the comparison is fixed already";
        case LHAT_CHECK_ERR_AS_IMPOSSIBLE:
            return "nothing is both of these, so this as^ could never succeed";
        case LHAT_CHECK_ERR_BAD_KEY:
            return "nil^ is how a table spells 'not there', so it cannot be a key";
        case LHAT_CHECK_ERR_NO_OPERATOR:
            return "an operator is answered by what stands to its left, or by "
                   "what stands to its right when that side writes the self^ "
                   "last; neither answers this one";
        case LHAT_CHECK_ERR_BAD_OPERATOR:
            return "an op^ is an f^ taking self^ and one argument, and it may "
                   "not yield^; the self^ is whichever operand it is written "
                   "as -- first for the left one, last for the right. Only "
                   "op^- is also written with self^ alone, which is the unary "
                   "one";
        case LHAT_CHECK_ERR_COMPARE_NOT_NUMBER:
            return "op^<=> answers a number^: '<' and the rest read which "
                   "side of zero the answer falls on";
        case LHAT_CHECK_ERR_EQUAL_NOT_BOOL:
            return "op^= answers a bool^: '=' takes it as it stands, and "
                   "'\xE2\x89\xA0' negates it";
        case LHAT_CHECK_ERR_NOT_ORDERED:
            return "nothing here says how these compare: an ordering is read "
                   "off '<=>', and neither side carries one that takes the "
                   "other";
        case LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR:
            return "only an op^ writes its self^ last, to say the right "
                   "operand is the receiver; everywhere else the receiver is "
                   "what stands before the dot";
        case LHAT_CHECK_ERR_ISA_ALWAYS_TRUE:
            return "any^ holds of every value, so this asks nothing";
        case LHAT_CHECK_ERR_MEMBER_EXISTS:
            return "this name is already a member; write override^ or overload^";
        case LHAT_CHECK_ERR_ALREADY_PROVIDED:
            return "something in the chain already provides this member, so "
                   "there is nothing for an abstract^ to ask for";
        case LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE:
            return "an abstract^ asks a composition for what this def^ does "
                   "not have, and this one is written here as well; the "
                   "members reach each other whatever the order, so drop the "
                   "declaration";
        case LHAT_CHECK_ERR_STILL_ABSTRACT:
            return "this definition is still waiting on a composition -- a "
                   "member is declared with nothing providing it, or an "
                   "override^ has met nothing to replace";
        case LHAT_CHECK_ERR_AMBIGUOUS_MEMBER:
            return "both sides of the composition carry this name, so it "
                   "reaches no one answer; name the side you mean";
        case LHAT_CHECK_ERR_COMPOSE_COLLIDES:
            return "both definitions carry a member of this name, and a "
                   "marker can only be written inside a def^";
        case LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE:
            return "there is no member of this name to override^ or overload^";
        case LHAT_CHECK_ERR_NOT_SUBSTITUTABLE:
            return "override^ has to be usable where the original was";
        case LHAT_CHECK_ERR_OVERLOAD_OVERLAPS:
            return "overload^ overlaps an existing signature";
        case LHAT_CHECK_ERR_DISCARD_READ:
            return "'_^' throws the value away, so it is not a name and there "
                   "is nothing here to read; write a name where the value is "
                   "wanted";
        case LHAT_CHECK_ERR_NOT_DISPOSABLE:
            return "with^ needs a value with a dispose() that returns nothing";
        case LHAT_CHECK_ERR_FUNCTION_FALLS_OUT:
            return "a function answers on every path; this one has a path "
                   "that leaves without a value";
        case LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT:
            return "this body has a path that leaves without a value, which "
                   "the result type it was given does not admit";
        case LHAT_CHECK_ERR_SUPER_OUTSIDE:
            return "super^ is the member an override^ replaces, so it is a "
                   "name only inside one";
        case LHAT_CHECK_ERR_THIS_OUTSIDE:
            return "this^ names the subroutine running, and none is here";
        case LHAT_CHECK_ERR_NEVER_RETURNS:
            return "every way out of this body calls it again, so it never "
                   "produces a value";
        case LHAT_CHECK_ERR_RESULT_UNDECIDED:
            return "the result type did not come out of this body, so it has "
                   "to be written";
        case LHAT_CHECK_ERR_SCOPE_TOO_FAR:
            return "this reaches out past more scopes than are open here";
        case LHAT_CHECK_ERR_SCOPE_ON_DEFINE:
            return "let^ makes a name here, so it takes no scope specifier; "
                   "':=' is what writes one that is already there";
        case LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE:
            return "a parameter written inside a public^ declaration needs a "
                   "type; what leaves the unit is not read off a body";
        case LHAT_CHECK_ERR_FUNCTION_WRITES_OUT:
            return "an f^ assigns to local variables only, and this name was "
                   "bound outside its body";
        case LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE:
            return "an f^ may change only a table its own body made; this one "
                   "came from somewhere else";
        case LHAT_CHECK_ERR_PATH_IS_OPAQUE:
            return "this type carries what registered it, so what it holds "
                   "cannot be written over";
        case LHAT_CHECK_ERR_ADVANCES_OUTSIDE:
            return "an f^ may advance only a coroutine its own body made; "
                   "this one came from somewhere else";
        case LHAT_CHECK_ERR_COROUTINE_ESCAPES:
            return "an f^ coroutine may not leave the body that made it";
        case LHAT_CHECK_ERR_TABLE_IS_SEALED:
            return "this table belongs to the machine; what it holds is "
                   "written by the host, not from here";
        case LHAT_CHECK_ERR_ASSIGN_TO_LET:
            return "this name was bound by a let^ and is not reassigned; "
                   "write var^ where the name has to change";
        case LHAT_CHECK_ERR_ASSIGN_TO_FORM:
            return "the construct that introduces this name is what gives it a "
                   "value -- a with^ holds it for the block, a for^ advances "
                   "or rebinds its focus -- so nothing else writes it";
        case LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE:
            return "a public^ declaration binds with let^; another unit would "
                   "otherwise see a name change under it";
        case LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE:
            return "Self^ names the t^ or def^ written around it, and there is "
                   "no such literal here -- or a second hat counted past the "
                   "outermost one";
        case LHAT_CHECK_ERR_CLOSED_CAPTURES:
            return "a closed^ body names nothing standing outside it: pass "
                   "this as an argument instead. An import^ed module, a name "
                   "the host bound, and L^ are reached without capturing and "
                   "may be written here";
        case LHAT_CHECK_ERR_HOSTVALUE_ESCAPES:
            return "a host value lives on the stack and nowhere else; box it "
                   "into the container type its library provides to keep it";
        case LHAT_CHECK_ERR_TUPLE_MISPLACED:
            return "(A, B) is what a subroutine answers with, and it is "
                   "written nowhere else -- not as an argument, a name, a "
                   "table member, or a position of another tuple; pack^ makes "
                   "a t^{ A, B } of one";
        case LHAT_CHECK_ERR_TUPLE_UNION:
            return "the only thing (A, B) may be written in a union with is "
                   "an error";
        case LHAT_CHECK_ERR_TUPLE_ARITY:
            return "this answers a different number of values than there are "
                   "names to take them";
        case LHAT_CHECK_ERR_TUPLE_ERROR_POSITION:
            return "an error goes around the values, not among them; write "
                   "(A, B)|SomeError rather than (A, SomeError)";
    }
    return "unknown error";
}

size_t lhat_check_message_write(const LhatCheckDiagnostic *diagnostic,
                                char *out, size_t capacity)
{
    const char *plain = diagnostic != NULL
                            ? lhat_check_error_message(diagnostic->code)
                            : "unknown error";

    // Only where the diagnostic knows something its code does not.
    int written;
    if (diagnostic != NULL && diagnostic->name != NULL) {
        written = snprintf(out, out != NULL ? capacity : 0, "%s: %.*s", plain,
                           (int)diagnostic->name_length, diagnostic->name);
    } else {
        written = snprintf(out, out != NULL ? capacity : 0, "%s", plain);
    }

    if (written < 0) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return (size_t)written;
}
