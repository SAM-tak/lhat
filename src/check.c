// L^ (lhat) -- the type checking stage.

#include "check.h"

#include <stdlib.h>
#include <string.h>

// A name bound in a scope. `offset` is where its let^ stands, which 8.7 needs
// to tell a use before the definition from one after it.
typedef struct Binding {
    const char *name;
    size_t name_length;
    LhatType *type;
    uint32_t offset;
    bool reached;  // its let^ has been walked past
    struct Binding *next;
} Binding;

typedef struct Scope {
    Binding *bindings;
    Binding *tail;
    struct Scope *parent;
} Scope;

// 13.11. What a branch knows about a value that the binding does not: inside
// 'if^ r is^ T', r is only the arms of its union that conform to T. Held as
// the path it was written with rather than as a binding, since 13.11 admits
// a dot path as well as a plain name.
typedef struct Narrowing {
    const LhatNode *path;
    LhatType *type;
    struct Narrowing *next;
} Narrowing;

typedef struct {
    // The tree points into both the source text and the lexer's decoded
    // string storage, so the lexer is what has to outlive the result.
    const LhatLexer *lexer;
    LhatCheckResult *result;
    bool strict;

    // 05 の 6.1: how an import is answered. Absent when a unit is checked on
    // its own, in which case a require^ cannot be followed.
    LhatRequire require;

    Scope *scope;
    // 05 の 2.2: one environment. A name means a value, a type, or both.

    // Innermost first. A branch pushes and pops around its body, so the list
    // is a stack rather than something scopes own.
    Narrowing *narrowings;

    // 8.7: inside a subroutine body nothing runs where it is written, so the
    // ordering rule does not apply. A counter rather than a flag, since
    // bodies nest.
    size_t deferred;

    // The result type of the subroutine being checked, for 04 の 5.3 and for
    // collecting return^ when 03 の 3.4 has to infer one.
    LhatType *declared_result;
    LhatType *inferred_result;
    bool saw_self_call;
    // 03 の 3.4: a return^ whose value goes through the subroutine itself.
    // Its type is the one being worked out, so it is left out of the union.
    bool recursive_return;
    // 03 の 3.4 counts every exit. A bare return^ is one that produces no
    // value, exactly like reaching the end of the body -- and it leaves the
    // same nil^ behind at run time.
    bool valueless_return;

    // 02 の 15.10: the type of the subroutine whose body is being checked,
    // which is what this^ names. NULL outside any body.
    LhatType *this_type;

    // The name the subroutine currently being checked is being bound to, so
    // that a call to it inside its own body can be spotted (03 の 3.4).
    const char *defining_name;
    size_t defining_length;

    // 15.2: what the yield^/yieldall^ sites seen so far in this body agree
    // on. infer_func saves and resets these around a nested body the same
    // way it does declared_result/inferred_result, so a nested p^{...} does
    // not pollute the enclosing one.
    LhatType *coroutine_produce;  // Y
    LhatType *coroutine_receive;  // R

    // How the yield^ infer() is about to see is being used. check_define
    // sets BOUND with the let^ target's annotation just around inferring
    // that one value; check_statement sets DISCARD around a bare yield^
    // statement. Left at NONE everywhere else, including inside whatever
    // infer() recurses into to produce a yield^'s own value -- that value is
    // never itself the direct target of a binding.
    enum YieldContext {
        YIELD_CTX_NONE,
        YIELD_CTX_DISCARD,
        YIELD_CTX_BOUND
    } yield_context;
    LhatType *yield_bound_type;  // YIELD_CTX_BOUND only; NULL means "no annotation"
} Checker;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static void report(Checker *c, const LhatNode *at, LhatCheckErrorCode code)
{
    if (at == NULL) {
        return;
    }

    LhatCheckResult *r = c->result;
    if (r->diagnostic_count == r->diagnostic_capacity) {
        size_t grown = r->diagnostic_capacity ? r->diagnostic_capacity * 2 : 8;
        LhatCheckDiagnostic *bigger = (LhatCheckDiagnostic *)realloc(
            r->diagnostics, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        r->diagnostics = bigger;
        r->diagnostic_capacity = grown;
    }

    LhatCheckDiagnostic *d = &r->diagnostics[r->diagnostic_count++];
    d->code = code;
    d->offset = at->offset;
    d->line = at->line;
    d->column = at->column;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// The text of an IDENT, HAT_IDENT or TYPE_NAME node. Names live as spans into
// the source, so nothing is copied.
//
// The trailing hats are not part of the name. 01 の 2.3 has the lexer count
// them and keep them on the token, so 'number^' arrives with a length that
// covers the '^' as well; comparing against the word means dropping them.
static bool node_name(const Checker *c, const LhatNode *node,
                      const char **text, size_t *length)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_TYPE_NAME:
            *text = c->lexer->source->text + node->v.name.offset;
            *length = node->v.name.length >= node->v.name.hats
                          ? node->v.name.length - node->v.name.hats
                          : node->v.name.length;
            return true;
        case LHAT_NODE_SCOPE:
            return node_name(c, node->v.scope.name, text, length);
        case LHAT_NODE_FOCUS:
            // 16.2: the focus with no name written is called it^, and the
            // source need not contain the word for that to be its name.
            *text = "it";
            *length = 2;
            return true;
        default:
            return false;
    }
}

static bool name_is(const char *text, size_t length, const char *literal)
{
    size_t n = strlen(literal);
    return length == n && memcmp(text, literal, n) == 0;
}

// ---------------------------------------------------------------------------
// Scopes (8.7)
// ---------------------------------------------------------------------------

static Binding *scope_find_local(Scope *scope, const char *name, size_t length)
{
    for (Binding *b = scope->bindings; b != NULL; b = b->next) {
        if (b->name_length == length && memcmp(b->name, name, length) == 0) {
            return b;
        }
    }
    return NULL;
}

static Binding *scope_find(Scope *scope, const char *name, size_t length)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = scope_find_local(s, name, length);
        if (b != NULL) {
            return b;
        }
    }
    return NULL;
}

static Binding *scope_add(Scope *scope, const char *name, size_t length,
                          LhatType *type, uint32_t offset)
{
    Binding *b = (Binding *)calloc(1, sizeof *b);
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

static void scope_dispose(Scope *scope)
{
    Binding *b = scope->bindings;
    while (b != NULL) {
        Binding *next = b->next;
        free(b);
        b = next;
    }
    scope->bindings = NULL;
    scope->tail = NULL;
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

static LhatType *simple(Checker *c, LhatTypeKind kind)
{
    return lhat_type_simple(c->result->types, kind);
}

static LhatType *resolve_type(Checker *c, const LhatNode *node);
static LhatType *infer(Checker *c, const LhatNode *node);
static void check_statement(Checker *c, const LhatNode *node);
static LhatType *collect_exports(Checker *c, const LhatNode *statements);
static void check_statements(Checker *c, const LhatNode *statements);
static LhatType *infer_def(Checker *c, const LhatNode *node, LhatType *base);
static LhatType *instance_of(const LhatType *definition);
static const LhatTypeMember *find_member(const LhatType *table, const char *name,
                                         size_t length);
static LhatType *only(Checker *c, LhatType *type, LhatType *wanted);
static LhatType *without(Checker *c, LhatType *type, LhatType *unwanted);

// 14.8: one number type over integers and reals. The rest are the plain
// builtin spellings.
static LhatType *builtin_type(Checker *c, const char *name, size_t length)
{
    if (name_is(name, length, "number") || name_is(name, length, "int") ||
        name_is(name, length, "float")) {
        return simple(c, LHAT_TYPE_NUMBER);
    }
    if (name_is(name, length, "string")) {
        return simple(c, LHAT_TYPE_STRING);
    }
    if (name_is(name, length, "bool")) {
        return simple(c, LHAT_TYPE_BOOL);
    }
    if (name_is(name, length, "nil")) {
        return simple(c, LHAT_TYPE_NIL);
    }
    if (name_is(name, length, "any")) {
        return simple(c, LHAT_TYPE_ANY);
    }
    if (name_is(name, length, "error")) {
        return simple(c, LHAT_TYPE_ERROR);
    }
    // 14.10: t^ and table^ are the same word. Bare, with no members listed,
    // it asks for nothing in particular -- the top of tables, which 13.7
    // notes is not the top of every value.
    if (name_is(name, length, "t") || name_is(name, length, "table")) {
        return lhat_type_table(c->result->types);
    }
    return NULL;
}

static LhatType *resolve_table_type(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(c->result->types);
    // 14.10改: an entry with no name is the type of the next position. They
    // are counted the way a literal counts its own -- from one, in written
    // order, with the named ones taking no place in the sequence.
    size_t position = 0;
    for (const LhatNode *m = node->v.list.items; m != NULL; m = m->next) {
        const char *name = NULL;
        size_t length = 0;
        if (m->v.entry.key == NULL) {
            lhat_type_add_index_member(c->result->types, table, ++position,
                                       resolve_type(c, m->v.entry.value));
            continue;
        }
        if (!node_name(c, m->v.entry.key, &name, &length)) {
            continue;
        }
        lhat_type_add_member(c->result->types, table, name, length,
                             resolve_type(c, m->v.entry.value));
    }
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
           node_name(c, written, &name, &length) &&
           name_is(name, length, "self");
}

static LhatType *resolve_func_type(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        if (param == node->v.func.params && is_self_marker(c, param)) {
            func->v.func.takes_self = true;
            continue;
        }
        if (param->v.param.variadic) {
            func->v.func.variadic = param->v.param.type != NULL
                                        ? resolve_type(c, param->v.param.type)
                                        : simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func,
                            param->v.param.type != NULL
                                ? resolve_type(c, param->v.param.type)
                                : simple(c, LHAT_TYPE_UNKNOWN));
    }
    if (node->v.func.return_type != NULL) {
        func->v.func.result = resolve_type(c, node->v.func.return_type);
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
    LhatType *instance = instance_of(bound);
    return instance != NULL ? instance : bound;
}

static LhatType *resolve_qualified_type(Checker *c, const LhatNode *node)
{
    LhatType *outer = resolve_type(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (outer == NULL || !node_name(c, node->v.access.argument, &name, &length)) {
        report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (outer->kind == LHAT_TYPE_ERROR_SET) {
        for (const LhatTypeList *k = outer->v.error.kinds; k != NULL;
             k = k->next) {
            if (k->type->v.error.name_length == length &&
                memcmp(k->type->v.error.name, name, length) == 0) {
                return k->type;
            }
        }
        report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 05 の 6.1: what require^ yields is a structure, so reaching a type out
    // of it is the same member access a value uses. 04 の 14.4 already made a
    // qualified name writable as a type; this is that form over a unit.
    if (outer->kind == LHAT_TYPE_TABLE) {
        const LhatTypeMember *member = find_member(outer, name, length);
        if (member != NULL) {
            return as_written_type(member->type);
        }
    }

    report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    return simple(c, LHAT_TYPE_UNKNOWN);
}

static LhatType *resolve_type(Checker *c, const LhatNode *node)
{
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
            if (!node_name(c, node, &name, &length)) {
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *builtin = builtin_type(c, name, length);
            if (builtin != NULL) {
                return builtin;
            }
            // 05 の 2.2: one environment. A name written as a type is looked
            // up in the same place a value is, which is what 14.9 needs --
            // it says a definition takes its name from its binding, and a
            // binding lives here.
            Binding *declared = scope_find(c->scope, name, length);
            if (declared == NULL) {
                report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }

            return as_written_type(declared->type);
        }

        case LHAT_NODE_MEMBER:
            return resolve_qualified_type(c, node);

        case LHAT_NODE_TYPE_TABLE:
            return resolve_table_type(c, node);

        case LHAT_NODE_TYPE_FUNC:
            return resolve_func_type(c, node);

        case LHAT_NODE_TYPE_UNION:
            return lhat_type_union(c->result->types,
                                   resolve_type(c, node->v.binary.left),
                                   resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_INTERSECT:
            return lhat_type_intersect(c->result->types,
                                       resolve_type(c, node->v.binary.left),
                                       resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_CORO:
            return lhat_type_coro(c->result->types,
                                  resolve_type(c, node->v.coroutine.receive),
                                  resolve_type(c, node->v.coroutine.produce),
                                  resolve_type(c, node->v.coroutine.result));

        default:
            report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
            return simple(c, LHAT_TYPE_UNKNOWN);
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
static LhatType *only(Checker *c, LhatType *type, LhatType *wanted)
{
    if (type == NULL) {
        return NULL;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        LhatType *kept = NULL;
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            kept = lhat_type_union(c->result->types, kept,
                                   only(c, arm->type, wanted));
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
static LhatType *without(Checker *c, LhatType *type, LhatType *unwanted)
{
    if (type == NULL) {
        return NULL;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        LhatType *kept = NULL;
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            kept = lhat_type_union(c->result->types, kept,
                                   without(c, arm->type, unwanted));
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
        return without(c, expand_set(c, type), unwanted);
    }
    return type;
}

static bool can_be(const LhatType *type, const LhatType *wanted)
{
    return type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
           !lhat_type_disjoint(type, wanted);
}

// 13.2: nothing inhabits "no value", so it cannot stand where one is wanted.
// The positions that want a value ask here, rather than infer_call reporting
// it -- 8.2 makes a call on its own a statement, and that is exactly where a
// subroutine with no result belongs. Answers unknown once it has reported, so
// the one mistake does not cascade.
static LhatType *require_value(Checker *c, const LhatNode *at, LhatType *type)
{
    if (type != NULL && type->kind == LHAT_TYPE_NONE) {
        report(c, at, LHAT_CHECK_ERR_MISMATCH);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }
    return type;
}

// ---------------------------------------------------------------------------
// Narrowing (13.11)
// ---------------------------------------------------------------------------

// 13.11 limits narrowing to a name and to a dot path from one. A call or an
// index is excluded because there is no guarantee the second evaluation
// yields the same value, and a narrowing that cannot be relied on afterwards
// is worse than none.
static bool narrowable(const LhatNode *node)
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
            return !node->v.access.nil_safe && narrowable(node->v.access.target);
        default:
            return false;
    }
}

static bool same_name(const Checker *c, const LhatNode *a, const LhatNode *b)
{
    const char *na = NULL;
    const char *nb = NULL;
    size_t la = 0;
    size_t lb = 0;
    return node_name(c, a, &na, &la) && node_name(c, b, &nb, &lb) &&
           la == lb && memcmp(na, nb, la) == 0;
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

static LhatType *narrowed_type(Checker *c, const LhatNode *path)
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
    Narrowing *n = (Narrowing *)calloc(1, sizeof *n);
    if (n == NULL) {
        return;
    }
    n->path = path;
    n->type = type;
    n->next = c->narrowings;
    c->narrowings = n;
}

static void pop_narrowings(Checker *c, Narrowing *mark)
{
    while (c->narrowings != mark) {
        Narrowing *next = c->narrowings->next;
        free(c->narrowings);
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
static void drop_narrowings_for(Checker *c, const LhatNode *target)
{
    const LhatNode *root = path_root(target);
    for (Narrowing *n = c->narrowings; n != NULL; n = n->next) {
        if (same_name(c, path_root(n->path), root)) {
            n->type = NULL;
        }
    }
}

// 02 の 9.8: break^ leaves the innermost loop. The multi-level form is still
// a proposal and the compiler refuses it, so a break^ found here belongs to
// the loop this is the body of -- unless another loop stands between them,
// which takes its own. Answers whether this loop has a way out written in it.
static bool breaks_out(const LhatNode *node)
{
    for (; node != NULL; node = node->next) {
        switch (node->kind) {
            case LHAT_NODE_BREAK:
                return true;

            case LHAT_NODE_BLOCK:
                if (breaks_out(node->v.list.items)) {
                    return true;
                }
                // 9 章: the clauses of a loop body are statements of it too.
                for (const LhatNode *clause = node->v.list.extra;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out(clause->v.loop_clause.body)) {
                        return true;
                    }
                }
                break;

            case LHAT_NODE_IF_STMT:
                for (const LhatNode *clause = node->v.list.items;
                     clause != NULL; clause = clause->next) {
                    if (breaks_out(clause->v.clause.body)) {
                        return true;
                    }
                }
                break;

            case LHAT_NODE_WITH:
                if (breaks_out(node->v.list.extra)) {
                    return true;
                }
                break;

            case LHAT_NODE_FOR:
                // 16.3 and 17 章: the if^ and when^ forms do not iterate, so
                // a break^ inside one still leaves the loop out here. Every
                // other form is a loop and takes its own.
                if ((node->v.loop.kind == LHAT_FOR_IF ||
                     node->v.loop.kind == LHAT_FOR_WHEN) &&
                    breaks_out(node->v.loop.body)) {
                    return true;
                }
                break;

            // A repeat^ is a loop, and a nested body has its own loops. What
            // a break^ in either one leaves is not this loop.
            default:
                break;
        }
    }
    return false;
}

// Whether control cannot reach the end of this statement. 04 の 6.1 is
// written in the early-return style -- handle the error, leave, and carry on
// below knowing it did not happen -- so the narrowing a branch established
// has to outlive a branch that never falls through.
static bool always_exits(const LhatNode *node)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        case LHAT_NODE_RETURN:
        case LHAT_NODE_BREAK:
            return true;

        case LHAT_NODE_BLOCK:
            for (const LhatNode *s = node->v.list.items; s != NULL;
                 s = s->next) {
                if (always_exits(s)) {
                    return true;
                }
            }
            return false;

        case LHAT_NODE_IF_STMT: {
            bool has_else = false;
            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                if (!always_exits(clause->v.clause.body)) {
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

        default:
            return false;
    }
}

// The narrowings a condition implies when it holds, or when it does not.
//
// 13.11: and^ tells us both sides held when it is true, or^ tells us both
// failed when it is false, and neither says anything in the other direction
// -- one of the two could have decided it alone.
static void narrow_from(Checker *c, const LhatNode *condition, bool truth)
{
    if (condition == NULL) {
        return;
    }

    if (condition->kind == LHAT_NODE_UNARY &&
        condition->v.unary.op == LHAT_OP_NOT) {
        narrow_from(c, condition->v.unary.operand, !truth);
        return;
    }

    if (condition->kind != LHAT_NODE_BINARY) {
        return;
    }

    LhatOpKind op = condition->v.binary.op;
    if ((op == LHAT_OP_AND && truth) || (op == LHAT_OP_OR && !truth)) {
        narrow_from(c, condition->v.binary.left, truth);
        narrow_from(c, condition->v.binary.right, truth);
        return;
    }

    if (op != LHAT_OP_IS) {
        return;
    }

    const LhatNode *path = condition->v.binary.left;
    if (!narrowable(path)) {
        return;
    }

    LhatType *current = infer(c, path);
    LhatType *tested = resolve_type(c, condition->v.binary.right);
    push_narrowing(c, path,
                   truth ? only(c, current, tested)
                         : without(c, current, tested));
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static void expect(Checker *c, const LhatNode *at, LhatType *value,
                   LhatType *target, LhatCheckErrorCode code)
{
    if (!lhat_type_conforms(value, target)) {
        report(c, at, code);
    }
}

static LhatType *infer_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node, &name, &length)) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // A few hat identifiers are values rather than names (01 の 2.2).
    if (node->kind == LHAT_NODE_HAT_IDENT) {
        if (name_is(name, length, "true") || name_is(name, length, "false")) {
            return simple(c, LHAT_TYPE_BOOL);
        }
        if (name_is(name, length, "nil")) {
            return simple(c, LHAT_TYPE_NIL);
        }
        // 15.10: this^ names the subroutine running, which is what lets a
        // body with no name recurse. Only the hatted spelling means it, so
        // an ordinary name `this` is untouched.
        if (name_is(name, length, "this")) {
            if (c->this_type == NULL) {
                report(c, node, LHAT_CHECK_ERR_THIS_OUTSIDE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 03 の 3.4 counts it the same way a call by name is counted.
            c->saw_self_call = true;
            return c->this_type;
        }
    }

    // 03 の 3.4: a subroutine calling itself cannot have its result inferred,
    // since the answer would depend on itself.
    if (c->defining_name != NULL && c->deferred > 0 &&
        length == c->defining_length &&
        memcmp(name, c->defining_name, length) == 0) {
        c->saw_self_call = true;
    }

    Binding *b = scope_find(c->scope, name, length);
    if (b == NULL) {
        report(c, node, LHAT_CHECK_ERR_UNDEFINED);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 8.7: the name is visible throughout the scope, which is what makes
    // mutual recursion work, but its value only exists once its let^ has run.
    // A subroutine body does not run where it is written, so the rule only
    // applies outside one.
    if (!b->reached && c->deferred == 0) {
        report(c, node, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    }
    return b->type;
}

static LhatType *infer_binary(Checker *c, const LhatNode *node)
{
    LhatOpKind op = node->v.binary.op;

    // 04 の 4.1 and 11.7: both drop one arm and put a value in its place.
    if (op == LHAT_OP_CATCH || op == LHAT_OP_NIL_ELSE) {
        LhatType *left = infer(c, node->v.binary.left);
        LhatType *right = infer(c, node->v.binary.right);
        LhatType *unwanted = simple(c, op == LHAT_OP_CATCH ? LHAT_TYPE_ERROR
                                                           : LHAT_TYPE_NIL);
        if (!can_be(left, unwanted)) {
            report(c, node, op == LHAT_OP_CATCH ? LHAT_CHECK_ERR_CANNOT_FAIL
                                                : LHAT_CHECK_ERR_CANNOT_BE_NIL);
        }
        return lhat_type_union(c->result->types, without(c, left, unwanted),
                               right);
    }

    LhatType *left = infer(c, node->v.binary.left);

    // 14.5: composition is written with '..' because the order matters. The
    // right side is not a value here -- it is a definition read against what
    // the left already provides, which is what 14.12 needs to see.
    if (op == LHAT_OP_CONCAT && node->v.binary.right != NULL &&
        node->v.binary.right->kind == LHAT_NODE_DEF) {
        return infer_def(c, node->v.binary.right, left);
    }

    // 13.11: is^ takes a type on the right, so the right side is not a value.
    if (op == LHAT_OP_IS) {
        LhatType *asked = resolve_type(c, node->v.binary.right);
        // 13.7: any^ is the top of every value, so this holds of whatever is
        // on the left and the question is empty. 13.11 refuses to read the
        // left's inferred type against the right -- that would narrow the
        // escape hatch 13.7 exists to provide -- but this one is decided by
        // the right side alone, whatever the left turns out to be.
        if (asked != NULL && asked->kind == LHAT_TYPE_ANY) {
            report(c, node, LHAT_CHECK_ERR_IS_ALWAYS_TRUE);
        }
        return simple(c, LHAT_TYPE_BOOL);
    }

    LhatType *right = infer(c, node->v.binary.right);

    switch (op) {
        case LHAT_OP_ADD:
        case LHAT_OP_SUB:
        case LHAT_OP_MUL:
        case LHAT_OP_DIV:
        case LHAT_OP_FLOORDIV:
        case LHAT_OP_MOD:
        case LHAT_OP_POW: {
            LhatType *number = simple(c, LHAT_TYPE_NUMBER);
            expect(c, node->v.binary.left, left, number,
                   LHAT_CHECK_ERR_NOT_NUMBER);
            expect(c, node->v.binary.right, right, number,
                   LHAT_CHECK_ERR_NOT_NUMBER);
            // 04 の 11.2: only // and % can fail, and / never does.
            if (op == LHAT_OP_FLOORDIV || op == LHAT_OP_MOD) {
                return number;
            }
            return number;
        }

        case LHAT_OP_AND:
        case LHAT_OP_OR: {
            LhatType *boolean = simple(c, LHAT_TYPE_BOOL);
            expect(c, node->v.binary.left, left, boolean, LHAT_CHECK_ERR_NOT_BOOL);
            expect(c, node->v.binary.right, right, boolean,
                   LHAT_CHECK_ERR_NOT_BOOL);
            return boolean;
        }

        case LHAT_OP_EQ:
        case LHAT_OP_NE:
        case LHAT_OP_LT:
        case LHAT_OP_GT:
        case LHAT_OP_LE:
        case LHAT_OP_GE:
            // 14.12's disjointness says whether any value inhabits both. If
            // none does the answer is fixed before the program runs, which is
            // a mistake rather than a comparison. Which types are ordered is
            // left alone -- this only refuses the pairs that can never meet.
            if (lhat_type_disjoint(left, right)) {
                report(c, node, LHAT_CHECK_ERR_INCOMPARABLE);
            }
            return simple(c, LHAT_TYPE_BOOL);

        case LHAT_OP_CONCAT:
            // 11.3 leaves this to the operator's own definition, which needs
            // op^ and is not implemented. Strings are the case that is
            // certain; anything else is left undecided rather than refused --
            // but "no value" is refused whatever the operator turns out to
            // want, since there is nothing there to hand it.
            require_value(c, node->v.binary.left, left);
            require_value(c, node->v.binary.right, right);
            if (lhat_type_conforms(left, simple(c, LHAT_TYPE_STRING)) &&
                lhat_type_conforms(right, simple(c, LHAT_TYPE_STRING))) {
                return simple(c, LHAT_TYPE_STRING);
            }
            return simple(c, LHAT_TYPE_UNKNOWN);

        default:
            return simple(c, LHAT_TYPE_UNKNOWN);
    }
}

// Whether a signature would take these arguments, asked without reporting.
// 14.12 forbids overlapping signatures precisely so that at most one arm of
// an overloaded member can answer yes, which makes resolution a search rather
// than a ranking.
static bool signature_accepts(const LhatType *func, LhatType *const *args,
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

#define LHAT_CHECK_MAX_TRACKED_ARGS 16

static LhatType *infer_call(Checker *c, const LhatNode *node)
{
    LhatType *callee = infer(c, node->v.access.target);

    size_t given = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        given++;
    }

    if (callee == NULL || callee->kind == LHAT_TYPE_UNKNOWN) {
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            infer(c, arg);
        }
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 14.12: an overloaded member is the intersection of its signatures, so
    // calling one means finding the arm that fits. Arguments are inferred
    // once here, since inferring them again would report twice.
    if (callee->kind == LHAT_TYPE_INTERSECT) {
        LhatType *args[LHAT_CHECK_MAX_TRACKED_ARGS];
        size_t tracked = 0;
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            LhatType *type = infer(c, arg);
            if (tracked < LHAT_CHECK_MAX_TRACKED_ARGS) {
                args[tracked++] = type;
            }
        }
        if (tracked < given) {
            return simple(c, LHAT_TYPE_UNKNOWN);  // more than worth tracking
        }

        bool through_member =
            node->v.access.target->kind == LHAT_NODE_MEMBER;
        for (const LhatTypeList *arm = callee->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (signature_accepts(arm->type, args, tracked, through_member)) {
                return arm->type->v.func.result;
            }
        }
        report(c, node, LHAT_CHECK_ERR_MISMATCH);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (callee->kind != LHAT_TYPE_FUNC) {
        report(c, node, LHAT_CHECK_ERR_NOT_CALLABLE);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    const LhatTypeList *param = callee->v.func.params;
    size_t declared = 0;
    for (const LhatTypeList *p = param; p != NULL; p = p->next) {
        declared++;
    }

    // 14.4: 'x.m()' hands x to the receiver without writing it, while a
    // method taken as a value is called with the receiver spelled out. The
    // receiver is kept out of `params`, so only the second form adjusts.
    size_t skip = 0;
    if (callee->v.func.takes_self &&
        node->v.access.target->kind != LHAT_NODE_MEMBER) {
        declared++;
        skip = 1;
    }

    // 13.7: a trailing '...' takes any number beyond the declared ones.
    if (given < declared || (given > declared && callee->v.func.variadic == NULL)) {
        report(c, node, LHAT_CHECK_ERR_ARITY);
    }

    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        LhatType *actual = infer(c, arg);
        if (skip > 0) {
            skip--;  // the receiver, whose type the call site already knows
            continue;
        }
        LhatType *wanted = param != NULL ? param->type : callee->v.func.variadic;
        if (wanted != NULL) {
            expect(c, arg, actual, wanted, LHAT_CHECK_ERR_MISMATCH);
        }
        if (param != NULL) {
            param = param->next;
        }
    }

    // 15.5: calling a yieldable procedure answers a coroutine rather than
    // running it. 13.9 gives that three types; the middle two come from
    // whatever infer_func found its yield^/yieldall^ sites agreeing on
    // (15.2改). A body with no yield^ at all -- only yieldall^ that never
    // ran, or none reached -- leaves them NULL, which nil^ fills the same
    // way an unwritten result does.
    if (callee->v.func.yields) {
        // 13.9改: the third type is what the last resume receives. A body
        // with no value-returning return^ hands nil^ back when it ends --
        // but a body that cannot end has no last resume at all, and putting
        // nil^ there would make every consumer narrow away something that
        // never arrives. NULL is how that is spelled, the same way it is for
        // a subroutine that answers nothing.
        LhatType *ends_with = callee->v.func.result;
        if (ends_with == NULL && callee->v.func.ends_without_value) {
            ends_with = simple(c, LHAT_TYPE_NIL);
        }
        return lhat_type_coro(c->result->types,
                              callee->v.func.yield_receive != NULL
                                  ? callee->v.func.yield_receive
                                  : simple(c, LHAT_TYPE_NIL),
                              callee->v.func.yield_produce != NULL
                                  ? callee->v.func.yield_produce
                                  : simple(c, LHAT_TYPE_NIL),
                              ends_with);
    }
    // 13.2: a signature with no result answers no value. That is not a gap in
    // inference, so it is not spelled with a NULL -- 03 の 3.4 kept it apart
    // from nil^, and this is where that stays observable.
    //
    // 15.10: except when the body being checked is calling itself. Its result
    // is still being worked out, and 03 の 3.4 reads the NULL to mean exactly
    // that -- so a self-call has to keep handing the NULL back.
    if (callee->v.func.result == NULL && callee != c->this_type) {
        return simple(c, LHAT_TYPE_NONE);
    }
    return callee->v.func.result;
}

// 02 § 16.3: what the built-in walk of a table yields. 13.8 has no tuples,
// so the pair is a table -- position 1 the key, position 2 the value. Both
// come from what the table holds: 14 章 makes it a sequence and a mapping at
// once, so a walk of one that is both hands over keys of either kind.
static LhatType *table_walk_pair(Checker *c, const LhatType *over)
{
    LhatType *pair = lhat_type_table(c->result->types);
    if (over == NULL || over->kind != LHAT_TYPE_TABLE) {
        return pair;
    }

    LhatType *keys = NULL;
    LhatType *values = NULL;
    for (const LhatTypeMember *m = over->v.table.members; m != NULL;
         m = m->next) {
        // The sequence half is described by members whose names are digits,
        // which 01 の 6 章 keeps a program from writing.
        bool positional = m->name_length > 0;
        for (size_t i = 0; positional && i < m->name_length; i++) {
            positional = m->name[i] >= '0' && m->name[i] <= '9';
        }
        keys = lhat_type_union(
            c->result->types, keys,
            simple(c, positional ? LHAT_TYPE_NUMBER : LHAT_TYPE_STRING));
        values = lhat_type_union(c->result->types, values, m->type);
    }

    // 14.10 lets a table carry more than its type lists, so what is written
    // down only ever adds to what a walk may hand over -- it never bounds it.
    // With nothing listed there is nothing to say.
    if (keys != NULL) {
        lhat_type_add_index_member(c->result->types, pair, 1, keys);
        lhat_type_add_index_member(c->result->types, pair, 2, values);
    }
    return pair;
}

static LhatType *infer_member(Checker *c, const LhatNode *node)
{
    LhatType *target = infer(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.access.argument, &name, &length)) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (target == NULL || target->kind == LHAT_TYPE_UNKNOWN) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 04 の 2.3: every kind carries message and cause without declaring them.
    const LhatTypeMember *members = NULL;
    if (target->kind == LHAT_TYPE_TABLE) {
        members = target->v.table.members;
    } else if (target->kind == LHAT_TYPE_ERROR_KIND) {
        if (name_is(name, length, "message")) {
            return simple(c, LHAT_TYPE_STRING);
        }
        if (name_is(name, length, "cause")) {
            return lhat_type_union(c->result->types, simple(c, LHAT_TYPE_ERROR),
                                   simple(c, LHAT_TYPE_NIL));
        }
        members = target->v.error.fields;
    } else if (target->kind == LHAT_TYPE_CORO) {
        // 05 の 8.5: a coroutine carries these without importing anything.
        // 02 の 12.6 spells dispose(), 15.6 puts resume beside it, and 16.3
        // makes iterate what `in^` asks for.
        if (name_is(name, length, "start")) {
            // 15.2改: runs the body from the top, for a coroutine that has
            // never been resumed. Takes nothing, since nothing has been
            // yield^ed yet to send a value to. Answers the same union a
            // resume does -- which is the yield type alone when the third
            // type is absent, since a coroutine that cannot end never
            // answers with one (13.9改).
            LhatType *signature = lhat_type_func(c->result->types, false);
            signature->v.func.result =
                lhat_type_union(c->result->types, target->v.coroutine.produce,
                                target->v.coroutine.result);
            return signature;
        }
        if (name_is(name, length, "resume")) {
            // 13.9: what a resume answers is the union of what the coroutine
            // yields and what it returns -- telling the two apart is what
            // done() does (15.6改). 15.2改: R is now one fixed type, so resume
            // takes exactly one argument of it -- start() is what a fresh
            // coroutine is resumed with instead of a sentinel "no argument"
            // call. An absent third type leaves the yield type alone.
            LhatType *answer =
                lhat_type_union(c->result->types, target->v.coroutine.produce,
                                target->v.coroutine.result);
            LhatType *signature = lhat_type_func(c->result->types, false);
            lhat_type_add_param(c->result->types, signature,
                                target->v.coroutine.receive);
            signature->v.func.result = answer;
            return signature;
        }
        if (name_is(name, length, "dispose")) {
            // 12.7: it answers nothing, which is what 12.5 checks for.
            return lhat_type_func(c->result->types, false);
        }
        if (name_is(name, length, "iterate")) {
            LhatType *signature = lhat_type_func(c->result->types, false);
            signature->v.func.result = target;  // 16.3: itself
            return signature;
        }
        // 15.6改: what a resume answers is a union of Y and the result, and
        // nothing keeps those two apart -- a body that yields nil^ and one
        // that has ended answer the same value. So the state is asked for
        // rather than read out of the value.
        if (name_is(name, length, "done")) {
            LhatType *signature = lhat_type_func(c->result->types, false);
            signature->v.func.result = simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        // done() alone leaves a fresh coroutine and a suspended one looking
        // alike, so a consumer handed one it did not make could not tell
        // which of start() and resume() this is.
        if (name_is(name, length, "started")) {
            LhatType *signature = lhat_type_func(c->result->types, false);
            signature->v.func.result = simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
        return simple(c, LHAT_TYPE_UNKNOWN);
    } else {
        report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m->type;
        }
    }

    // 16.3: `in^ e` asks e for the coroutine to walk, and a table answers
    // with one over its keys without anything being written. This comes
    // after the search, not before it, because 16.3 lets a written iterate
    // win -- the same order the machine reads it in.
    if (name_is(name, length, "iterate")) {
        // 13.8 has no tuples, so a pair is a table. The walk sends nothing
        // in and ends without a value, which 04 の 11.3 spells nil^.
        LhatType *walk = lhat_type_coro(c->result->types,
                                        simple(c, LHAT_TYPE_NIL),
                                        table_walk_pair(c, target),
                                        simple(c, LHAT_TYPE_NIL));
        LhatType *signature = lhat_type_func(c->result->types, false);
        signature->v.func.result = walk;
        return signature;
    }

    report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
    return simple(c, LHAT_TYPE_UNKNOWN);
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
        LhatType *value = require_value(c, entry->v.entry.value,
                                        infer(c, entry->v.entry.value));

        // 14.6改: a computed key is an expression, checked like any other.
        // What it names is only known when it is written out -- an integer
        // reaches the sequence half, a string the keyed one, and anything
        // else lands somewhere 14.10 lets the type stay quiet about.
        if (entry->v.entry.computed) {
            const LhatNode *key = entry->v.entry.key;
            LhatType *asked = require_value(c, key, infer(c, key));
            // 04 の 11.3: nil^ is how "not there" is spelled, so it cannot
            // also be a key. The machine reports one that turns out to be
            // nil^; a key that can only ever be one is decided here. A NaN
            // is the other refusal, and stays the machine's -- nothing in
            // 14.8's one number type tells them apart.
            if (asked != NULL && asked->kind != LHAT_TYPE_UNKNOWN &&
                lhat_type_conforms(asked, simple(c, LHAT_TYPE_NIL))) {
                report(c, key, LHAT_CHECK_ERR_BAD_KEY);
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
        if (node_name(c, entry->v.entry.key, &name, &length)) {
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
// mixing yields the way 13.9 no longer allows.
static void unify_yield(Checker *c, const LhatNode *at, LhatType **slot,
                        LhatType *candidate)
{
    if (candidate == NULL || candidate->kind == LHAT_TYPE_UNKNOWN) {
        // 13.11: UNKNOWN carries no information, so there is nothing here to
        // agree or disagree with.
        return;
    }
    if (*slot == NULL) {
        *slot = candidate;
        return;
    }
    if (!lhat_type_equal(*slot, candidate)) {
        report(c, at, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    }
}

// 03 の 3.4: the result type is inferred from return^ unless it is written,
// and a subroutine that calls itself has to write it.
static LhatType *infer_func(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    // 15.2: whether the body suspends is read off the body, not written.
    func->v.func.yields = node->v.func.yields;

    Scope body;
    body.bindings = NULL;
    body.tail = NULL;
    body.parent = c->scope;

    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        // 14.4: the receiver is not an ordinary parameter. Leaving it unbound
        // lets the self^ that infer_def put in scope show through, which is
        // what the body is actually talking about.
        if (param == node->v.func.params && is_self_marker(c, param)) {
            func->v.func.takes_self = true;
            continue;
        }
        LhatType *type = param->v.param.type != NULL
                             ? resolve_type(c, param->v.param.type)
                             : simple(c, LHAT_TYPE_UNKNOWN);
        if (param->v.param.variadic) {
            func->v.func.variadic =
                param->v.param.type != NULL ? type : simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);

        const char *name = NULL;
        size_t length = 0;
        if (node_name(c, param->v.param.name, &name, &length)) {
            Binding *b = scope_add(&body, name, length, type,
                                   param->v.param.name->offset);
            if (b != NULL) {
                b->reached = true;
            }
        }
    }

    LhatType *declared = node->v.func.return_type != NULL
                             ? resolve_type(c, node->v.func.return_type)
                             : NULL;
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

    c->scope = &body;
    c->declared_result = declared;
    c->inferred_result = NULL;
    c->saw_self_call = false;
    c->recursive_return = false;
    c->valueless_return = false;
    c->this_type = func;
    c->deferred++;
    // 15.2: a nested p^{...} starts collecting its own Y/R from scratch, so
    // its yield^ sites never unify with the ones out here.
    c->coroutine_produce = NULL;
    c->coroutine_receive = NULL;
    c->yield_context = YIELD_CTX_NONE;
    c->yield_bound_type = NULL;

    check_statement(c, node->v.func.body);

    if (node->v.func.yields) {
        func->v.func.yield_produce = c->coroutine_produce;
        func->v.func.yield_receive = c->coroutine_receive;
    }

    // Reaching the end of the body is an exit that produces no value, and a
    // bare return^ is the same exit written down. Both hand nil^ back at run
    // time, so 03 の 3.4 counts them together. What that means depends on
    // what the subroutine promised.
    bool falls_through = !always_exits(node->v.func.body);
    bool leaves_without_value = falls_through || c->valueless_return;
    func->v.func.ends_without_value = leaves_without_value;

    // 02 の 13.2: a function always has a result -- Memo.md L152 is where
    // that comes from. So an f^ with a path that answers nothing has one
    // with nothing to answer with, and no result type would make it right.
    if (leaves_without_value && node->v.func.is_function) {
        report(c, node, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    } else if (leaves_without_value && declared != NULL &&
               !lhat_type_conforms(simple(c, LHAT_TYPE_NIL), declared)) {
        // A p^ may leave without a value, but then its result has to admit
        // one. 04 の 11.3 spells that nil^.
        report(c, node, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
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
            c->inferred_result = lhat_type_union(c->result->types,
                                                 c->inferred_result,
                                                 simple(c, LHAT_TYPE_NIL));
        }

        // Every way out goes through the subroutine itself, so no call of it
        // ever produces a value. 02 の 12.8 and 03 の 5.6 leave no other way
        // out -- no exceptions, no unwinding -- so this is decidable here.
        if (c->recursive_return && c->inferred_result == NULL) {
            report(c, node, LHAT_CHECK_ERR_NEVER_RETURNS);
        }
        func->v.func.result = c->inferred_result;
    }

    c->deferred--;
    c->scope = outer_scope;
    c->declared_result = outer_declared;
    c->inferred_result = outer_inferred;
    c->saw_self_call = outer_self_call;
    c->recursive_return = outer_recursive;
    c->valueless_return = outer_valueless;
    c->this_type = outer_this;
    c->coroutine_produce = outer_coroutine_produce;
    c->coroutine_receive = outer_coroutine_receive;
    c->yield_context = outer_yield_context;
    c->yield_bound_type = outer_yield_bound_type;

    scope_dispose(&body);
    return func;
}

// 14 章. A definition produces two structures, and 14.7 is what ties them:
// an instance can reach the definition's members, so its type contains them
// as well as the fields the template declares.
//
//   definition : the members, plus a new^ if none was written (14.11)
//   instance   : those members, plus the template's fields
//
// 14.9 keeps the name out of it. Both are ordinary structures, so 11.3's
// structural identity applies unchanged and nothing here has to be interned.
static const LhatTypeMember *find_member(const LhatType *table,
                                         const char *name, size_t length)
{
    if (table == NULL || table->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    for (const LhatTypeMember *m = table->v.table.members; m != NULL;
         m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m;
        }
    }
    return NULL;
}

// 14.11 makes new^ return an instance, so a definition's own structure is
// where its instance type can be found again. Composition needs it, to carry
// the base's fields into the derived instance.
static LhatType *instance_of(const LhatType *definition)
{
    const LhatTypeMember *constructor = find_member(definition, "new", 3);
    if (constructor == NULL || constructor->type == NULL ||
        constructor->type->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    return constructor->type->v.func.result;
}

// Overwrites rather than appending, so a member the base already had ends up
// replaced by 14.12's override^ instead of shadowed by position.
static void set_member(Checker *c, LhatType *table, const char *name,
                       size_t length, LhatType *type)
{
    for (LhatTypeMember *m = table->v.table.members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            m->type = type;
            return;
        }
    }
    lhat_type_add_member(c->result->types, table, name, length, type);
}

static void copy_members(Checker *c, LhatType *into, const LhatType *from)
{
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = from->v.table.members; m != NULL;
         m = m->next) {
        lhat_type_add_member(c->result->types, into, m->name, m->name_length,
                             m->type);
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
static LhatType *check_same_name(Checker *c, const LhatNode *entry,
                                 const LhatTypeMember *inherited,
                                 LhatType *replacement)
{
    LhatDefModifier modifier = entry->v.entry.modifier;

    if (inherited == NULL) {
        if (modifier != LHAT_DEF_PLAIN) {
            report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        }
        return replacement;
    }

    switch (modifier) {
        case LHAT_DEF_PLAIN:
            report(c, entry, LHAT_CHECK_ERR_MEMBER_EXISTS);
            return replacement;

        case LHAT_DEF_OVERRIDE:
            // The replacement has to be usable where the original was, which
            // is ordinary conformance: arguments wider, result narrower. The
            // receiver is not in the parameter list (14.4), so 14.12's
            // exemption of self^ from variance needs nothing of its own.
            if (!lhat_type_conforms(replacement, inherited->type)) {
                report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
            }
            return replacement;

        case LHAT_DEF_OVERLOAD:
            if (signatures_overlap(replacement, inherited->type)) {
                report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
                return replacement;
            }
            // 14.12: an overloaded member is callable both ways, which is
            // what '&' means (14.5).
            return lhat_type_intersect(c->result->types, inherited->type,
                                       replacement);
    }
    return replacement;
}

static LhatType *infer_def(Checker *c, const LhatNode *node, LhatType *base)
{
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);

    // 14.5: composition is ordered, and the derived side is written against
    // what the base already provides.
    copy_members(c, definition, base);
    copy_members(c, instance, instance_of(base));

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
            if (!node_name(c, field->v.entry.key, &name, &length)) {
                continue;
            }
            // 14.11: an initializer, evaluated at each construction. Its type
            // is what the field holds.
            set_member(c, instance, name, length,
                       infer(c, field->v.entry.value));
        }
    }

    // 14.4: self^ reaches the instance, class^ the definition. Bound before
    // the members are walked so a body may use either.
    Scope members;
    members.bindings = NULL;
    members.tail = NULL;
    members.parent = c->scope;
    Binding *receiver = scope_add(&members, "self", 4, instance, node->offset);
    Binding *owner = scope_add(&members, "class", 5, definition, node->offset);
    if (receiver != NULL) {
        receiver->reached = true;
    }
    if (owner != NULL) {
        owner->reached = true;
    }

    Scope *outer = c->scope;
    c->scope = &members;

    bool has_new = false;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.key == NULL ||
            !node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        LhatType *type = infer(c, entry->v.entry.value);
        type = check_same_name(c, entry, find_member(base, name, length), type);
        set_member(c, definition, name, length, type);
        set_member(c, instance, name, length, type);
        if (name_is(name, length, "new")) {
            has_new = true;
        }
    }

    c->scope = outer;
    scope_dispose(&members);

    // 14.11: without one written, a definition still offers a new^ taking no
    // arguments, since the template already fixes every field's value.
    //
    // An inherited one keeps its parameters but has to build the derived
    // instance, so it is rebuilt rather than copied: 14.5 composes to make
    // something new, and a constructor returning the base would defeat that.
    if (!has_new) {
        const LhatTypeMember *inherited = find_member(base, "new", 3);
        LhatType *constructor = lhat_type_func(c->result->types, true);
        if (inherited != NULL && inherited->type != NULL &&
            inherited->type->kind == LHAT_TYPE_FUNC) {
            for (const LhatTypeList *p = inherited->type->v.func.params;
                 p != NULL; p = p->next) {
                lhat_type_add_param(c->result->types, constructor, p->type);
            }
            constructor->v.func.variadic = inherited->type->v.func.variadic;
        }
        constructor->v.func.result = instance;
        set_member(c, definition, "new", 3, constructor);
    }
    return definition;
}

static LhatType *infer(Checker *c, const LhatNode *node)
{
    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
        case LHAT_NODE_INT:
        case LHAT_NODE_FLOAT:
            return simple(c, LHAT_TYPE_NUMBER);

        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
            return simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_INTERP:
            // 01 の 5.4: a hole holds an ordinary expression, and lands in a
            // string whatever it turns out to be -- which is why the answer
            // here says nothing about it. What goes wrong inside one is still
            // wrong, so each is checked on its own.
            for (const LhatNode *part = node->v.list.items; part != NULL;
                 part = part->next) {
                if (part->kind == LHAT_NODE_INTERP_HOLE) {
                    require_value(c, part->v.hole.value,
                                  infer(c, part->v.hole.value));
                }
            }
            return simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_SCOPE:
        case LHAT_NODE_FOCUS: {
            // 13.11: a branch may know more about this path than the binding.
            LhatType *narrowed = narrowed_type(c, node);
            return narrowed != NULL ? narrowed : infer_name(c, node);
        }

        case LHAT_NODE_TABLE:
            return infer_table(c, node);

        case LHAT_NODE_DEF:
            return infer_def(c, node, NULL);

        case LHAT_NODE_SELF_TABLE: {
            // 14.11: in the body of a def^ this declares the fields, and
            // inside new^ it builds one. Either way it names the instance,
            // which is what self^ is bound to.
            for (const LhatNode *field = node->v.list.items; field != NULL;
                 field = field->next) {
                infer(c, field->v.entry.value);
            }
            Binding *receiver = scope_find(c->scope, "self", 4);
            return receiver != NULL ? receiver->type
                                    : simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_UNARY: {
            LhatType *operand = infer(c, node->v.unary.operand);
            if (node->v.unary.op == LHAT_OP_NOT) {
                expect(c, node, operand, simple(c, LHAT_TYPE_BOOL),
                       LHAT_CHECK_ERR_NOT_BOOL);
                return simple(c, LHAT_TYPE_BOOL);
            }
            expect(c, node, operand, simple(c, LHAT_TYPE_NUMBER),
                   LHAT_CHECK_ERR_NOT_NUMBER);
            return simple(c, LHAT_TYPE_NUMBER);
        }

        case LHAT_NODE_BINARY:
            return infer_binary(c, node);

        case LHAT_NODE_COMPARE_CHAIN:
            for (const LhatNode *operand = node->v.chain.operands;
                 operand != NULL; operand = operand->next) {
                infer(c, operand);
            }
            return simple(c, LHAT_TYPE_BOOL);

        case LHAT_NODE_CALL:
            return infer_call(c, node);

        case LHAT_NODE_MEMBER: {
            LhatType *narrowed = narrowed_type(c, node);
            return narrowed != NULL ? narrowed : infer_member(c, node);
        }

        case LHAT_NODE_INDEX: {
            LhatType *over = infer(c, node->v.access.target);
            require_value(c, node->v.access.argument,
                          infer(c, node->v.access.argument));
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
                    found = lhat_type_member_at(over,
                                                (size_t)key->v.integer.value);
                } else if (key->kind == LHAT_NODE_STRING) {
                    found = find_member(over,
                                        c->lexer->strings + key->v.string.offset,
                                        key->v.string.length);
                }
                if (found != NULL) {
                    return found->type;
                }
            }
            // 04 §11.3: a dynamic key may be absent, and absence is not a
            // failure, so nothing narrower than this is safe here.
            return simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_AS:
            require_value(c, node->v.ascription.value,
                          infer(c, node->v.ascription.value));
            return resolve_type(c, node->v.ascription.type);

        case LHAT_NODE_FUNC:
            return infer_func(c, node);

        // 15.2改: what a yield^ answers (R) has to be fixed by whatever binds
        // it directly, since the value it carries out (Y) is only half of
        // what the expression is. check_define and the bare-statement case
        // in check_statement are the only two places that set yield_context
        // to anything other than NONE, and only for the yield^ they
        // themselves are looking at -- infer() clears it immediately below
        // so it can never leak into node->v.jump.value.
        case LHAT_NODE_YIELD: {
            enum YieldContext ctx = c->yield_context;
            LhatType *bound = c->yield_bound_type;
            c->yield_context = YIELD_CTX_NONE;
            c->yield_bound_type = NULL;

            LhatType *produced = require_value(c, node,
                                               infer(c, node->v.jump.value));
            unify_yield(c, node, &c->coroutine_produce, produced);

            if (ctx == YIELD_CTX_DISCARD) {
                // Nothing receives this one, so it says nothing about R.
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (ctx == YIELD_CTX_BOUND && bound != NULL) {
                unify_yield(c, node, &c->coroutine_receive, bound);
                return bound;
            }
            // Either bound with no annotation to read R off of, or reached
            // some other way (buried inside a larger expression) where there
            // is nowhere to write one.
            report(c, node, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
            return simple(c, LHAT_TYPE_UNKNOWN);
        }

        // 15.8: the value is the inner coroutine's return value, and the
        // right side has to be a coroutine -- there is nothing else to
        // delegate to. 15.2改: whatever it yields passes through as this
        // body's own Y/R, same as a yield^ written directly here would.
        case LHAT_NODE_YIELD_ALL: {
            LhatType *inner = infer(c, node->v.jump.value);
            if (inner == NULL || inner->kind == LHAT_TYPE_UNKNOWN) {
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (inner->kind != LHAT_TYPE_CORO) {
                report(c, node, LHAT_CHECK_ERR_NOT_COROUTINE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            unify_yield(c, node, &c->coroutine_produce, inner->v.coroutine.produce);
            unify_yield(c, node, &c->coroutine_receive, inner->v.coroutine.receive);
            // 13.9改: a coroutine that cannot end has no return type, so a
            // delegation to one never produces a value either.
            return inner->v.coroutine.result != NULL
                       ? inner->v.coroutine.result
                       : simple(c, LHAT_TYPE_NONE);
        }

        case LHAT_NODE_TRY: {
            // 04 の 5.3: the errors this lets through have to be ones the
            // enclosing subroutine may return, and the report belongs here
            // rather than at the caller.
            LhatType *value = infer(c, node->v.jump.value);
            LhatType *error = simple(c, LHAT_TYPE_ERROR);
            if (!can_be(value, error)) {
                report(c, node, LHAT_CHECK_ERR_CANNOT_FAIL);
            } else if (c->declared_result != NULL) {
                LhatType *escaping = only(c, value, error);
                if (escaping != NULL &&
                    !lhat_type_conforms(escaping, c->declared_result)) {
                    report(c, node, LHAT_CHECK_ERR_TRY_OUTSIDE);
                }
            }
            return without(c, value, error);
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
                                             infer(c, clause->v.clause.body));
                    continue;
                }
                expect(c, condition, infer(c, condition),
                       simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);

                Narrowing *before = c->narrowings;
                narrow_from(c, condition, true);
                result = lhat_type_union(c->result->types, result,
                                         infer(c, clause->v.clause.body));
                pop_narrowings(c, before);

                narrow_from(c, condition, false);
            }
            pop_narrowings(c, outer);
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
            check_statements(c, node->v.loop.focus);
            LhatType *result = infer(c, node->v.loop.body);
            c->scope = outer;
            scope_dispose(&scope);
            return result;
        }

        case LHAT_NODE_REQUIRE: {
            // 05 の 6.1: the checker follows this. 5.2 already had the parser
            // insist the path be written out, so there is text here to hand
            // the resolver rather than an expression to evaluate.
            const LhatNode *path = node->v.jump.value;
            if (path == NULL || c->require.resolve == NULL) {
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *exports = c->require.resolve(
                c->require.context, c->lexer->strings + path->v.string.offset,
                path->v.string.length);
            if (exports == NULL) {
                report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            return exports;
        }

        case LHAT_NODE_ERROR_NEW: {
            // 04 の 2.5: the kind is written into the construction, and 2.3
            // makes it the type of the result.
            LhatType *kind = resolve_type(c, node->v.named.name);
            for (const LhatNode *entry = node->v.named.members; entry != NULL;
                 entry = entry->next) {
                LhatType *given = infer(c, entry->v.entry.value);
                const char *name = NULL;
                size_t length = 0;
                if (kind == NULL || kind->kind != LHAT_TYPE_ERROR_KIND ||
                    !node_name(c, entry->v.entry.key, &name, &length)) {
                    continue;
                }
                // message and cause exist on every kind without being
                // declared (2.3); the rest have to have been.
                if (name_is(name, length, "message")) {
                    expect(c, entry, given, simple(c, LHAT_TYPE_STRING),
                           LHAT_CHECK_ERR_MISMATCH);
                    continue;
                }
                if (name_is(name, length, "cause")) {
                    continue;
                }
                bool found = false;
                for (const LhatTypeMember *m = kind->v.error.fields; m != NULL;
                     m = m->next) {
                    if (m->name_length == length &&
                        memcmp(m->name, name, length) == 0) {
                        expect(c, entry, given, m->type, LHAT_CHECK_ERR_MISMATCH);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    report(c, entry, LHAT_CHECK_ERR_NO_MEMBER);
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
                        given = node_name(c, entry->v.entry.key, &name, &length) &&
                                length == m->name_length &&
                                memcmp(name, m->name, length) == 0;
                    }
                    if (!given) {
                        report(c, node, LHAT_CHECK_ERR_MISSING_FIELD);
                    }
                }
            }
            return kind;
        }

        case LHAT_NODE_UNPACK:
            require_value(c, node->v.jump.value, infer(c, node->v.jump.value));
            return simple(c, LHAT_TYPE_UNKNOWN);

        default:
            return simple(c, LHAT_TYPE_UNKNOWN);
    }
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static const LhatNode *target_name_node(const LhatNode *target)
{
    return target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
}

// 13.10: unpack^ marks one value being taken apart rather than one value per
// target -- which is what tells a destructuring bind from a multiple one, and
// why the mark is on the right. Answers what is being taken apart, or NULL
// when this is an ordinary bind.
static LhatType *unpacked_source(Checker *c, const LhatNode *values)
{
    if (values == NULL || values->next != NULL ||
        values->kind != LHAT_NODE_UNPACK) {
        return NULL;
    }
    LhatType *source = require_value(c, values->v.jump.value,
                                     infer(c, values->v.jump.value));
    return source != NULL ? source : simple(c, LHAT_TYPE_UNKNOWN);
}

// The type at a position of what unpack^ is taking apart. 14.10 lets a table
// carry more than its type lists, so a position it says nothing about is
// unknown rather than absent.
static LhatType *unpacked_at(Checker *c, LhatType *source, size_t position)
{
    const LhatTypeMember *at = lhat_type_member_at(source, position);
    return at != NULL ? at->type : simple(c, LHAT_TYPE_UNKNOWN);
}

static void check_define(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    LhatType *unpacked = unpacked_source(c, value);
    size_t position = 0;

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        LhatType *annotated = target->kind == LHAT_NODE_PARAM
                                  ? resolve_type(c, target->v.param.type)
                                  : NULL;

        // Tell infer_func which name this subroutine is being given, so a
        // call to it inside its own body is recognised (03 の 3.4).
        const char *outer_name = c->defining_name;
        size_t outer_length = c->defining_length;
        node_name(c, target_name_node(target), &c->defining_name,
                  &c->defining_length);

        // 15.2改: a let^ that binds a yield^ directly is where R gets fixed --
        // it is the only place a yield^'s own annotation can be written. The
        // context is only good for the one infer() call it is set around.
        enum YieldContext outer_yctx = c->yield_context;
        LhatType *outer_ybound = c->yield_bound_type;
        c->yield_context = (value != NULL && value->kind == LHAT_NODE_YIELD)
                                ? YIELD_CTX_BOUND : YIELD_CTX_NONE;
        c->yield_bound_type = annotated;

        LhatType *actual;
        if (unpacked != NULL) {
            actual = unpacked_at(c, unpacked, position);
        } else {
            actual = value != NULL ? infer(c, value)
                                   : simple(c, LHAT_TYPE_UNKNOWN);
        }

        c->yield_context = outer_yctx;
        c->yield_bound_type = outer_ybound;

        c->defining_name = outer_name;
        c->defining_length = outer_length;

        if (annotated != NULL && value != NULL) {
            expect(c, value, actual, annotated, LHAT_CHECK_ERR_MISMATCH);
        } else if (value != NULL && actual != NULL &&
                   actual->kind == LHAT_TYPE_NONE) {
            // 13.2: a name binds a value, and a call of a signature with no
            // result does not make one. With an annotation written the
            // expect above has already said so.
            report(c, value, LHAT_CHECK_ERR_MISMATCH);
        }

        const char *name = NULL;
        size_t length = 0;
        if (node_name(c, target_name_node(target), &name, &length)) {
            Binding *b = scope_find_local(c->scope, name, length);
            if (b != NULL) {
                // Collected before the walk, so this is where its let^ runs.
                b->type = annotated != NULL ? annotated : actual;
                b->reached = true;
            }
            // A new name of the same spelling makes any narrowing of the old
            // one stale, since the path now reaches something else.
            drop_narrowings_for(c, target_name_node(target));
        }
        if (unpacked == NULL && value != NULL) {
            value = value->next;
        }
    }
}

static void check_reassign(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    LhatType *unpacked = unpacked_source(c, value);
    size_t position = 0;

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        LhatType *wanted = infer(c, target_name_node(target));
        if (unpacked != NULL) {
            expect(c, node, unpacked_at(c, unpacked, position), wanted,
                   LHAT_CHECK_ERR_MISMATCH);
        } else if (value != NULL) {
            expect(c, value, infer(c, value), wanted, LHAT_CHECK_ERR_MISMATCH);
            value = value->next;
        }
        // 13.11: what a branch established about this path no longer holds.
        drop_narrowings_for(c, target_name_node(target));
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
static const LhatTypeMember *member_named(const LhatType *type,
                                          const char *name, size_t length)
{
    const LhatTypeMember *members = NULL;
    if (type->kind == LHAT_TYPE_TABLE) {
        members = type->v.table.members;
    } else if (type->kind == LHAT_TYPE_ERROR_KIND) {
        members = type->v.error.fields;
    }
    for (; members != NULL; members = members->next) {
        if (members->name_length == length &&
            memcmp(members->name, name, length) == 0) {
            return members;
        }
    }
    return NULL;
}

// 16.3: `in^ e` walks e.iterate(). A coroutine answers with itself, a table
// with a walk over its keys, and anything else by having a member of that
// name -- the same three infer_member answers for, read off the type here
// because the loop has no member access written in it to infer.
static LhatType *walk_produce(Checker *c, const LhatNode *at, LhatType *over)
{
    if (over == NULL || over->kind == LHAT_TYPE_UNKNOWN ||
        over->kind == LHAT_TYPE_ANY) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }
    if (over->kind == LHAT_TYPE_CORO) {
        return over->v.coroutine.produce;
    }

    const LhatTypeMember *written = member_named(over, "iterate", 7);
    if (written != NULL) {
        // 16.3 lets a written iterate win, so this comes before the built-in.
        LhatType *answer = written->type;
        if (answer == NULL || answer->kind == LHAT_TYPE_UNKNOWN) {
            return simple(c, LHAT_TYPE_UNKNOWN);
        }
        if (answer->kind != LHAT_TYPE_FUNC || answer->v.func.result == NULL ||
            answer->v.func.result->kind != LHAT_TYPE_CORO) {
            report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
            return simple(c, LHAT_TYPE_UNKNOWN);
        }
        return answer->v.func.result->v.coroutine.produce;
    }

    // The built-in walk of a table, whose pairs 13.8 makes tables.
    if (over->kind == LHAT_TYPE_TABLE || over->kind == LHAT_TYPE_ERROR_KIND) {
        return table_walk_pair(c, over);
    }

    report(c, at, LHAT_CHECK_ERR_NOT_COROUTINE);
    return simple(c, LHAT_TYPE_UNKNOWN);
}

// 16.3: the focus of an in^ loop is bound, not evaluated, so it is checked
// here rather than by check_statements -- which would read the names as uses
// and find nothing in scope.
static void check_focus_in(Checker *c, const LhatNode *node)
{
    LhatType *produced =
        walk_produce(c, node->v.loop.bound, infer(c, node->v.loop.bound));

    size_t count = 0;
    for (const LhatNode *e = node->v.loop.focus; e != NULL; e = e->next) {
        count++;
    }

    size_t position = 0;
    for (const LhatNode *e = node->v.loop.focus; e != NULL; e = e->next) {
        position++;
        const LhatNode *element = focus_element(e);
        if (element == NULL) {
            continue;
        }
        LhatType *annotated = element->kind == LHAT_NODE_PARAM
                                  ? resolve_type(c, element->v.param.type)
                                  : NULL;

        // 13.10 and 16.3: one name takes what was yielded whole, several take
        // it apart by position.
        LhatType *taken = produced;
        if (count > 1) {
            const LhatTypeMember *at = lhat_type_member_at(produced, position);
            taken = at != NULL ? at->type : simple(c, LHAT_TYPE_UNKNOWN);
        }

        LhatType *type = taken;
        if (annotated != NULL) {
            expect(c, element, taken, annotated, LHAT_CHECK_ERR_MISMATCH);
            type = annotated;
        }

        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, target_name_node(element), &name, &length)) {
            continue;
        }
        Binding *b = scope_add(c->scope, name, length, type,
                               target_name_node(element)->offset);
        if (b != NULL) {
            b->reached = true;  // 8.7: a turn of the loop has bound it
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
        type->kind == LHAT_TYPE_ANY) {
        return;
    }

    const LhatTypeMember *members =
        type->kind == LHAT_TYPE_TABLE ? type->v.table.members : NULL;
    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length != 7 || memcmp(m->name, "dispose", 7) != 0) {
            continue;
        }
        if (m->type == NULL || m->type->kind == LHAT_TYPE_UNKNOWN) {
            return;
        }
        if (m->type->kind != LHAT_TYPE_FUNC || m->type->v.func.result != NULL) {
            report(c, at, LHAT_CHECK_ERR_NOT_DISPOSABLE);
        }
        return;
    }
    report(c, at, LHAT_CHECK_ERR_NOT_DISPOSABLE);
}

// 04 の 2.2: the declaration creates the set and its kinds as types.
static void check_errordef(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.named.name, &name, &length)) {
        return;
    }

    LhatType *set = lhat_type_error_set(c->result->types, name, length);
    scope_add(c->scope, name, length, set, node->offset)->reached = true;

    for (const LhatNode *kind = node->v.named.members; kind != NULL;
         kind = kind->next) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (!node_name(c, kind->v.named.name, &kind_name, &kind_length)) {
            continue;
        }
        LhatType *type = lhat_type_error_kind(c->result->types, set, kind_name,
                                              kind_length);
        for (const LhatNode *field = kind->v.named.members; field != NULL;
             field = field->next) {
            const char *field_name = NULL;
            size_t field_length = 0;
            if (!node_name(c, field->v.param.name, &field_name, &field_length)) {
                continue;
            }

            // 04 の 2.2: a default may stand in for the type, and its own
            // type is then the field's, exactly as 14.11 reads a template.
            LhatType *declared = resolve_type(c, field->v.param.type);
            LhatType *fallback = infer(c, field->v.param.fallback);
            if (declared != NULL && fallback != NULL) {
                expect(c, field->v.param.fallback, fallback, declared,
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
            if (!node_name(c, target_name_node(target), &name, &length)) {
                continue;
            }
            if (scope_find_local(c->scope, name, length) != NULL) {
                report(c, target_name_node(target), LHAT_CHECK_ERR_REDEFINED);
                continue;
            }
            scope_add(c->scope, name, length, simple(c, LHAT_TYPE_UNKNOWN),
                      target_name_node(target)->offset);
        }
    }
}

// 05 の 4 章: the exports are the names marked public^, read from the
// declarations rather than from a value the unit returns. That is what lets
// 6 章 follow an import without running anything.
static LhatType *collect_exports(Checker *c, const LhatNode *statements)
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
            if (!node_name(c, target_name_node(named), &name, &length)) {
                continue;
            }
            Binding *b = scope_find_local(c->scope, name, length);
            if (b == NULL) {
                continue;
            }
            if (table == NULL) {
                table = lhat_type_table(c->result->types);
            }
            lhat_type_add_member(c->result->types, table, name, length, b->type);
        }
    }
    return table;
}

static void check_statements(Checker *c, const LhatNode *statements)
{
    collect_bindings(c, statements);

    // A narrowing an if-statement leaves behind holds for the rest of this
    // list and no further, so the list is what bounds its life.
    Narrowing *mark = c->narrowings;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        check_statement(c, s);
    }
    pop_narrowings(c, mark);
}

static void check_block(Checker *c, const LhatNode *node)
{
    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = c->scope;

    Scope *outer = c->scope;
    c->scope = &scope;

    check_statements(c, node->v.list.items);
    for (const LhatNode *clause = node->v.list.extra; clause != NULL;
         clause = clause->next) {
        check_statements(c, clause->v.loop_clause.body);
    }

    c->scope = outer;
    scope_dispose(&scope);
}

static void check_statement(Checker *c, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
            check_define(c, node);
            break;

        case LHAT_NODE_REASSIGN:
            check_reassign(c, node);
            break;

        case LHAT_NODE_CALL_STMT: {
            LhatType *value = infer(c, node->v.jump.value);
            // 15.8: 15.5 makes such a call run no part of the body, so the
            // statement provably has no effect. 04 の 8.1 の form: the type
            // is the detection, with no must-use machinery added.
            if (value != NULL && value->kind == LHAT_TYPE_CORO) {
                report(c, node, LHAT_CHECK_ERR_COROUTINE_DROPPED);
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
                    check_statement(c, clause->v.clause.body);
                    continue;
                }
                expect(c, condition, infer(c, condition),
                       simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);

                Narrowing *before = c->narrowings;
                narrow_from(c, condition, true);
                check_statement(c, clause->v.clause.body);
                pop_narrowings(c, before);

                if (!always_exits(clause->v.clause.body)) {
                    every_branch_exits = false;
                }
                narrow_from(c, condition, false);
            }

            // Reaching the statement after this one means no branch was taken
            // -- but only when every branch that was taken left, and there was
            // no else to fall out of. Otherwise what holds below is a join of
            // several paths, which is not attempted here.
            if (has_else || !every_branch_exits) {
                pop_narrowings(c, outer);
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
            LhatType *value = infer(c, node->v.jump.value);
            bool recursive = c->saw_self_call;
            c->saw_self_call = enclosing_self_call || recursive;

            if (c->declared_result != NULL) {
                expect(c, node, value, c->declared_result,
                       LHAT_CHECK_ERR_MISMATCH);
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
            if (recursive && (value == NULL || value->kind == LHAT_TYPE_UNKNOWN)) {
                break;
            }
            // 13.2: a return^ with an expression carries a value, and a call
            // of a signature with no result does not make one. Reported here
            // rather than let into the union, where it would reach every
            // caller as a type nothing inhabits. A written result has already
            // caught it above.
            if (value != NULL && value->kind == LHAT_TYPE_NONE) {
                report(c, node, LHAT_CHECK_ERR_MISMATCH);
                break;
            }
            // 03 の 3.4: several return^ make a union.
            c->inferred_result =
                lhat_type_union(c->result->types, c->inferred_result, value);
            break;
        }

        case LHAT_NODE_YIELD: {
            // 15.2改: nobody receives this one, so R is not being fixed here
            // -- only Y, from whatever infer() finds inside it.
            enum YieldContext outer_yctx = c->yield_context;
            c->yield_context = YIELD_CTX_DISCARD;
            infer(c, node);
            c->yield_context = outer_yctx;
            break;
        }

        case LHAT_NODE_YIELD_ALL:
            infer(c, node);
            break;

        case LHAT_NODE_ERRORDEF:
            break;  // handled while collecting, so kinds are visible early

        case LHAT_NODE_FOR:
        case LHAT_NODE_REPEAT:
        case LHAT_NODE_WITH: {
            // The focus and the bindings belong to a scope of their own, which
            // is why 8.6 treats these keywords as introducers.
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
                    check_focus_in(c, node);
                } else {
                    check_statements(c, node->v.loop.focus);
                    infer(c, node->v.loop.bound);
                }
                infer(c, node->v.loop.step);
                check_statements(c, node->v.loop.advance);
                check_statement(c, node->v.loop.body);
            } else if (node->kind == LHAT_NODE_REPEAT) {
                infer(c, node->v.repeat.bound);
                check_statement(c, node->v.repeat.body);
            } else {
                check_statements(c, node->v.list.items);
                // 12.5: the binding is what has to be disposable, so the
                // report belongs on it rather than inside the block.
                for (const LhatNode *b = node->v.list.items; b != NULL;
                     b = b->next) {
                    check_disposable(c, b->v.binding.values,
                                     infer(c, b->v.binding.targets));
                }
                check_statement(c, node->v.list.extra);
            }

            c->scope = outer;
            scope_dispose(&scope);
            break;
        }

        default:
            break;
    }
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

    check_statements(&checker, unit->v.list.items);

    // 05 の 4 章: gathered once the whole unit has been checked, so a public^
    // name written late is not missed. 8.7 already makes every name visible
    // throughout the scope, so this only reads what is there.
    result->exports = collect_exports(&checker, unit->v.list.items);

    scope_dispose(&scope);
}

void lhat_check(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                LhatCheckResult *result)
{
    lhat_check_unit(unit, lexer, strict, NULL, NULL, result);
}

void lhat_check_result_dispose(LhatCheckResult *result)
{
    free(result->diagnostics);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->diagnostic_capacity = 0;
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
        case LHAT_CHECK_ERR_ARITY:
            return "the wrong number of arguments";
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
        case LHAT_CHECK_ERR_COROUTINE_DROPPED:
            return "this call makes a coroutine and runs none of the body; "
                   "write yieldall^ to delegate, or let^ to keep it";
        case LHAT_CHECK_ERR_MISSING_FIELD:
            return "this field has no default, so it has to be written";
        case LHAT_CHECK_ERR_INCOMPARABLE:
            return "these can never be equal, so the comparison is fixed already";
        case LHAT_CHECK_ERR_BAD_KEY:
            return "nil^ is how a table spells 'not there', so it cannot be a key";
        case LHAT_CHECK_ERR_IS_ALWAYS_TRUE:
            return "any^ holds of every value, so this asks nothing";
        case LHAT_CHECK_ERR_MEMBER_EXISTS:
            return "this name is already a member; write override^ or overload^";
        case LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE:
            return "there is no member of this name to override^ or overload^";
        case LHAT_CHECK_ERR_NOT_SUBSTITUTABLE:
            return "override^ has to be usable where the original was";
        case LHAT_CHECK_ERR_OVERLOAD_OVERLAPS:
            return "overload^ overlaps an existing signature";
        case LHAT_CHECK_ERR_NOT_DISPOSABLE:
            return "with^ needs a value with a dispose() that returns nothing";
        case LHAT_CHECK_ERR_FUNCTION_FALLS_OUT:
            return "a function answers on every path; this one has a path "
                   "that leaves without a value";
        case LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT:
            return "this body has a path that leaves without a value, which "
                   "the result type it was given does not admit";
        case LHAT_CHECK_ERR_THIS_OUTSIDE:
            return "this^ names the subroutine running, and none is here";
        case LHAT_CHECK_ERR_NEVER_RETURNS:
            return "every way out of this body calls it again, so it never "
                   "produces a value";
    }
    return "unknown error";
}
