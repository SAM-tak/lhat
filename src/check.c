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

typedef struct {
    const LhatSource *source;
    LhatCheckResult *result;
    bool strict;

    Scope *scope;
    Scope *type_scope;  // errordef^ sets and their kinds

    // 8.7: inside a subroutine body nothing runs where it is written, so the
    // ordering rule does not apply. A counter rather than a flag, since
    // bodies nest.
    size_t deferred;

    // The result type of the subroutine being checked, for 04 の 5.3 and for
    // collecting return^ when 03 の 3.4 has to infer one.
    LhatType *declared_result;
    LhatType *inferred_result;
    bool saw_self_call;

    // The name the subroutine currently being checked is being bound to, so
    // that a call to it inside its own body can be spotted (03 の 3.4).
    const char *defining_name;
    size_t defining_length;
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
            *text = c->source->text + node->v.name.offset;
            *length = node->v.name.length >= node->v.name.hats
                          ? node->v.name.length - node->v.name.hats
                          : node->v.name.length;
            return true;
        case LHAT_NODE_SCOPE:
            return node_name(c, node->v.scope.name, text, length);
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
    return lhat_type_simple(&c->result->types, kind);
}

static LhatType *resolve_type(Checker *c, const LhatNode *node);
static LhatType *infer(Checker *c, const LhatNode *node);
static void check_statement(Checker *c, const LhatNode *node);

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
        return lhat_type_table(&c->result->types);
    }
    return NULL;
}

static LhatType *resolve_table_type(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(&c->result->types);
    for (const LhatNode *m = node->v.list.items; m != NULL; m = m->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, m->v.entry.key, &name, &length)) {
            continue;
        }
        lhat_type_add_member(&c->result->types, table, name, length,
                             resolve_type(c, m->v.entry.value));
    }
    return table;
}

static LhatType *resolve_func_type(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(&c->result->types, node->v.func.is_function);
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        if (param->v.param.variadic) {
            func->v.func.variadic = param->v.param.type != NULL
                                        ? resolve_type(c, param->v.param.type)
                                        : simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(&c->result->types, func,
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
static LhatType *resolve_qualified_type(Checker *c, const LhatNode *node)
{
    LhatType *outer = resolve_type(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (outer == NULL || outer->kind != LHAT_TYPE_ERROR_SET ||
        !node_name(c, node->v.access.argument, &name, &length)) {
        report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    for (const LhatTypeList *k = outer->v.error.kinds; k != NULL; k = k->next) {
        if (k->type->v.error.name_length == length &&
            memcmp(k->type->v.error.name, name, length) == 0) {
            return k->type;
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
            Binding *declared = scope_find(c->type_scope, name, length);
            if (declared != NULL) {
                return declared->type;
            }
            report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
            return simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_MEMBER:
            return resolve_qualified_type(c, node);

        case LHAT_NODE_TYPE_TABLE:
            return resolve_table_type(c, node);

        case LHAT_NODE_TYPE_FUNC:
            return resolve_func_type(c, node);

        case LHAT_NODE_TYPE_UNION:
            return lhat_type_union(&c->result->types,
                                   resolve_type(c, node->v.binary.left),
                                   resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_INTERSECT:
            return lhat_type_intersect(&c->result->types,
                                       resolve_type(c, node->v.binary.left),
                                       resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_CORO:
            return lhat_type_coro(&c->result->types,
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

// Drops the arms of a union that conform to `unwanted`. This is what catch^
// and '??' do to their left side, and 13.11's narrowing is the same
// operation with a different filter.
static LhatType *without(Checker *c, LhatType *type, const LhatType *unwanted)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) {
        return (type != NULL && lhat_type_conforms(type, unwanted)) ? NULL : type;
    }

    LhatType *kept = NULL;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (!lhat_type_conforms(arm->type, unwanted)) {
            kept = lhat_type_union(&c->result->types, kept, arm->type);
        }
    }
    return kept;
}

// The other half: the arms that do conform. 04 の 5.3 needs it to name the
// errors a try^ would let past.
static LhatType *only(Checker *c, LhatType *type, const LhatType *wanted)
{
    if (type == NULL || type->kind != LHAT_TYPE_UNION) {
        return (type != NULL && lhat_type_conforms(type, wanted)) ? type : NULL;
    }

    LhatType *kept = NULL;
    for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (lhat_type_conforms(arm->type, wanted)) {
            kept = lhat_type_union(&c->result->types, kept, arm->type);
        }
    }
    return kept;
}

static bool can_be(const LhatType *type, const LhatType *wanted)
{
    return type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
           !lhat_type_disjoint(type, wanted);
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
        return lhat_type_union(&c->result->types, without(c, left, unwanted),
                               right);
    }

    LhatType *left = infer(c, node->v.binary.left);

    // 13.11: is^ takes a type on the right, so the right side is not a value.
    if (op == LHAT_OP_IS) {
        resolve_type(c, node->v.binary.right);
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
            return simple(c, LHAT_TYPE_BOOL);

        case LHAT_OP_CONCAT:
            // 11.3 leaves this to the operator's own definition, which needs
            // op^ and is not implemented. Strings are the case that is
            // certain; anything else is left undecided rather than refused.
            if (lhat_type_conforms(left, simple(c, LHAT_TYPE_STRING)) &&
                lhat_type_conforms(right, simple(c, LHAT_TYPE_STRING))) {
                return simple(c, LHAT_TYPE_STRING);
            }
            return simple(c, LHAT_TYPE_UNKNOWN);

        default:
            return simple(c, LHAT_TYPE_UNKNOWN);
    }
}

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
    if (callee->kind != LHAT_TYPE_FUNC) {
        report(c, node, LHAT_CHECK_ERR_NOT_CALLABLE);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    const LhatTypeList *param = callee->v.func.params;
    size_t declared = 0;
    for (const LhatTypeList *p = param; p != NULL; p = p->next) {
        declared++;
    }

    // 13.7: a trailing '...' takes any number beyond the declared ones.
    if (given < declared || (given > declared && callee->v.func.variadic == NULL)) {
        report(c, node, LHAT_CHECK_ERR_ARITY);
    }

    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        LhatType *actual = infer(c, arg);
        LhatType *wanted = param != NULL ? param->type : callee->v.func.variadic;
        if (wanted != NULL) {
            expect(c, arg, actual, wanted, LHAT_CHECK_ERR_MISMATCH);
        }
        if (param != NULL) {
            param = param->next;
        }
    }
    return callee->v.func.result;
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
            return lhat_type_union(&c->result->types, simple(c, LHAT_TYPE_ERROR),
                                   simple(c, LHAT_TYPE_NIL));
        }
        members = target->v.error.fields;
    } else {
        report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m->type;
        }
    }
    report(c, node, LHAT_CHECK_ERR_NO_MEMBER);
    return simple(c, LHAT_TYPE_UNKNOWN);
}

static LhatType *infer_table(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(&c->result->types);
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        LhatType *value = infer(c, entry->v.entry.value);
        const char *name = NULL;
        size_t length = 0;
        if (node_name(c, entry->v.entry.key, &name, &length)) {
            lhat_type_add_member(&c->result->types, table, name, length, value);
        }
    }
    return table;
}

// 03 の 3.4: the result type is inferred from return^ unless it is written,
// and a subroutine that calls itself has to write it.
static LhatType *infer_func(Checker *c, const LhatNode *node)
{
    LhatType *func = lhat_type_func(&c->result->types, node->v.func.is_function);

    Scope body;
    body.bindings = NULL;
    body.tail = NULL;
    body.parent = c->scope;

    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        LhatType *type = param->v.param.type != NULL
                             ? resolve_type(c, param->v.param.type)
                             : simple(c, LHAT_TYPE_UNKNOWN);
        if (param->v.param.variadic) {
            func->v.func.variadic =
                param->v.param.type != NULL ? type : simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(&c->result->types, func, type);

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

    c->scope = &body;
    c->declared_result = declared;
    c->inferred_result = NULL;
    c->saw_self_call = false;
    c->deferred++;

    check_statement(c, node->v.func.body);

    if (declared == NULL) {
        if (c->saw_self_call) {
            report(c, node, LHAT_CHECK_ERR_RECURSION_NEEDS_TYPE);
        } else {
            func->v.func.result = c->inferred_result;
        }
    }

    c->deferred--;
    c->scope = outer_scope;
    c->declared_result = outer_declared;
    c->inferred_result = outer_inferred;
    c->saw_self_call = outer_self_call;

    scope_dispose(&body);
    return func;
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
        case LHAT_NODE_INTERP:
            return simple(c, LHAT_TYPE_STRING);

        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_SCOPE:
            return infer_name(c, node);

        case LHAT_NODE_TABLE:
            return infer_table(c, node);

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

        case LHAT_NODE_MEMBER:
            return infer_member(c, node);

        case LHAT_NODE_INDEX:
            // 04 の 11.3: a dynamic key may be absent, and absence is not a
            // failure. The element type needs an array type, which 13 章 does
            // not have yet.
            infer(c, node->v.access.target);
            infer(c, node->v.access.argument);
            return simple(c, LHAT_TYPE_UNKNOWN);

        case LHAT_NODE_AS:
            infer(c, node->v.ascription.value);
            return resolve_type(c, node->v.ascription.type);

        case LHAT_NODE_FUNC:
            return infer_func(c, node);

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
            LhatType *result = NULL;
            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                if (clause->v.clause.condition != NULL) {
                    expect(c, clause->v.clause.condition,
                           infer(c, clause->v.clause.condition),
                           simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);
                }
                result = lhat_type_union(&c->result->types, result,
                                         infer(c, clause->v.clause.body));
            }
            return result;
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
            return kind;
        }

        case LHAT_NODE_UNPACK:
            infer(c, node->v.jump.value);
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

static void check_define(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        LhatType *annotated = target->kind == LHAT_NODE_PARAM
                                  ? resolve_type(c, target->v.param.type)
                                  : NULL;

        // Tell infer_func which name this subroutine is being given, so a
        // call to it inside its own body is recognised (03 の 3.4).
        const char *outer_name = c->defining_name;
        size_t outer_length = c->defining_length;
        node_name(c, target_name_node(target), &c->defining_name,
                  &c->defining_length);

        LhatType *actual = value != NULL ? infer(c, value)
                                         : simple(c, LHAT_TYPE_UNKNOWN);

        c->defining_name = outer_name;
        c->defining_length = outer_length;

        if (annotated != NULL && value != NULL) {
            expect(c, value, actual, annotated, LHAT_CHECK_ERR_MISMATCH);
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
        }
        if (value != NULL) {
            value = value->next;
        }
    }
}

static void check_reassign(Checker *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        LhatType *wanted = infer(c, target_name_node(target));
        if (value != NULL) {
            expect(c, value, infer(c, value), wanted, LHAT_CHECK_ERR_MISMATCH);
            value = value->next;
        }
    }
}

// 04 の 2.2: the declaration creates the set and its kinds as types.
static void check_errordef(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.named.name, &name, &length)) {
        return;
    }

    LhatType *set = lhat_type_error_set(&c->result->types, name, length);
    scope_add(c->type_scope, name, length, set, node->offset);

    for (const LhatNode *kind = node->v.named.members; kind != NULL;
         kind = kind->next) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (!node_name(c, kind->v.named.name, &kind_name, &kind_length)) {
            continue;
        }
        LhatType *type = lhat_type_error_kind(&c->result->types, set, kind_name,
                                              kind_length);
        for (const LhatNode *field = kind->v.named.members; field != NULL;
             field = field->next) {
            const char *field_name = NULL;
            size_t field_length = 0;
            if (node_name(c, field->v.entry.key, &field_name, &field_length)) {
                lhat_type_add_member(&c->result->types, type, field_name,
                                     field_length,
                                     resolve_type(c, field->v.entry.value));
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

static void check_statements(Checker *c, const LhatNode *statements)
{
    collect_bindings(c, statements);
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        check_statement(c, s);
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

        case LHAT_NODE_CALL_STMT:
            infer(c, node->v.jump.value);
            break;

        case LHAT_NODE_BLOCK:
            check_block(c, node);
            break;

        case LHAT_NODE_IF_STMT:
            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                if (clause->v.clause.condition != NULL) {
                    expect(c, clause->v.clause.condition,
                           infer(c, clause->v.clause.condition),
                           simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);
                }
                check_statement(c, clause->v.clause.body);
            }
            break;

        case LHAT_NODE_RETURN: {
            LhatType *value = infer(c, node->v.jump.value);
            if (c->declared_result != NULL) {
                expect(c, node, value, c->declared_result,
                       LHAT_CHECK_ERR_MISMATCH);
            } else {
                // 03 の 3.4: several return^ make a union.
                c->inferred_result =
                    lhat_type_union(&c->result->types, c->inferred_result, value);
            }
            break;
        }

        case LHAT_NODE_YIELD:
            infer(c, node->v.jump.value);
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
                check_statements(c, node->v.loop.focus);
                infer(c, node->v.loop.bound);
                infer(c, node->v.loop.step);
                check_statements(c, node->v.loop.advance);
                check_statement(c, node->v.loop.body);
            } else if (node->kind == LHAT_NODE_REPEAT) {
                infer(c, node->v.repeat.bound);
                check_statement(c, node->v.repeat.body);
            } else {
                check_statements(c, node->v.list.items);
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

void lhat_check(const LhatNode *unit, const LhatSource *source, bool strict,
                LhatCheckResult *result)
{
    memset(result, 0, sizeof *result);
    lhat_type_arena_init(&result->types);

    if (unit == NULL || source == NULL) {
        return;
    }

    Scope scope;
    scope.bindings = NULL;
    scope.tail = NULL;
    scope.parent = NULL;

    Scope types;
    types.bindings = NULL;
    types.tail = NULL;
    types.parent = NULL;

    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.source = source;
    checker.result = result;
    checker.strict = strict;
    checker.scope = &scope;
    checker.type_scope = &types;

    check_statements(&checker, unit->v.list.items);

    scope_dispose(&scope);
    scope_dispose(&types);
}

void lhat_check_result_dispose(LhatCheckResult *result)
{
    free(result->diagnostics);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->diagnostic_capacity = 0;
    lhat_type_arena_dispose(&result->types);
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
        case LHAT_CHECK_ERR_RECURSION_NEEDS_TYPE:
            return "a subroutine that calls itself needs its result type written";
    }
    return "unknown error";
}
