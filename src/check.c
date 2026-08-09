// L^ (lhat) -- the type checking stage.

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhatconfig.h"
#include "port.h"

// 05 の 8.7: a host writes a type out as text, so the checker reads the type
// grammar of 13 章 back through the parser it came from.
#include "parser.h"
#include "source.h"

// A name bound in a scope. `offset` is where its let^ stands, which 8.7 needs
// to tell a use before the definition from one after it.
typedef struct Binding {
    const char *name;
    size_t name_length;
    LhatType *type;
    uint32_t offset;
    bool reached;  // its let^ has been walked past
    // 15.1改: bound to a table this scope's own code made -- a literal or a
    // 14.11 new -- rather than to one that arrived from somewhere else. An
    // f^ may change the first and not the second, and reading it off the
    // initialiser's shape is what keeps that decidable without following
    // aliases: 'let^ v = t' binds a name, not a new table, so it is false.
    bool fresh;
    // 03 の 4.3: bound by an earlier input of a session. Writing the name
    // again is a redefinition rather than the clash 8.7 makes of two let^ in
    // one scope -- it is the same place, written again.
    bool from_session;
    // 8.9: introduced by a let^ rather than a var^, so nothing written after
    // it may reassign the name. 12 章's with^ and 16.3's in^ focus bind this
    // way too -- the first because the block disposes of whatever the name
    // still holds, the second because each turn binds it afresh.
    //
    // This says nothing about the value: a table a let^ holds is still the
    // table it was, and 8.8's members of it are still written through 15.1改
    // and 05 の 8.6改. What it refuses is the name coming to mean something
    // else.
    bool immutable;
    // 8.9 with 12.1 and 16.3改2: the construct bound it, not a word the writer
    // chose -- a with^, or the focus of a counted or walking for^. Writing
    // var^ instead is not open to them, so the diagnostic must not offer it.
    bool bound_by_form;
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

// 03 の 3.4: a parameter written without a type, while the body that decides
// it is being read. `slot` is the pending^ object infer_func made, and the
// binding and the signature both hold that very pointer -- settling one writes
// through to both, which is what keeps this to a single walk of the body.
//
// A member reached off such a parameter gets one of these too, so that
// 'x.foo(1)' can demand t^{ foo : p^number^; } rather than only the name.
// Its slot is the member's type inside the demand its parent is collecting.
typedef struct ParamVar {
    LhatType *slot;
    // What positions that always run have asked of it, and what the ones
    // under a branch have. Kept apart because 3.4 settles them differently:
    // the first is a requirement, the second is only added when it does not
    // contradict.
    LhatType *demanded;
    LhatType *conditional;
    // Two demands from under branches ruled each other out, so the branches
    // disagree and nothing here is decided (3.4 leaves that unknown^ rather
    // than reporting -- the uses report for themselves).
    bool conditional_dropped;
    // Reassigned, self-referential, or two unconditional demands ruled each
    // other out. Collection stops and the answer is unknown^.
    bool closed;
    struct ParamVar *next;
} ParamVar;

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

    // 03 の 3.4: the parameters whose types the bodies now open are deciding.
    // Innermost first, and a nested body's are pushed in front of the enclosing
    // one's -- a demand made inside one still reaches an outer parameter, since
    // the value it names came from out there.
    ParamVar *param_vars;

    // 05 の 4.3: inside a public^ declaration, where 3.4's inference of a
    // parameter type is not available. A counter for the same reason
    // `deferred` is one, though only a top-level declaration can carry the
    // marker.
    size_t exporting;

    // 8.7: inside a subroutine body nothing runs where it is written, so the
    // ordering rule does not apply. A counter rather than a flag, since
    // bodies nest.
    size_t deferred;

    // S23: inside an if^ clause, a for^ body or a repeat^ body, nothing
    // guarantees this specific piece of code runs at all before some later
    // point reads what it wrote -- unlike `deferred` above, this still runs
    // immediately if it runs, so 8.7's own rule does not read this one.
    // A counter, the same way and for the same reason as `deferred`.
    size_t conditional;

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

    // 15.10改 (S35): the bodies enclosing this one, outermost last -- what
    // this^^ walks. Links live on the C stack of the infer that pushed them,
    // the same way the saved this_type does.
    struct ThisLink {
        LhatType *type;
        struct ThisLink *outer;
    } *this_link;

    // 02 の 15.1: f^ may call only f^, never p^. True inside an f^ body
    // (and nested f^ literals within it), false everywhere else -- outside
    // any body (top level) and inside a p^ both allow either kind, so
    // false is the right default with no body open at all.
    bool in_function;

    // 15.1's other half: an f^ assigns to local variables only. This is the
    // scope its parameters were bound in, which is the outermost one its body
    // made -- a name found at or inside it is the body's own, and one found
    // past it belongs to whoever called. NULL outside any body. Saved and
    // restored around a nested one the same way `in_function` is, so an f^
    // written inside another measures against its own.
    Scope *body_scope;

    // 02 の 14.12改: what an override^ is writing over, which is what super^
    // names. NULL anywhere else, so 14.12's marker is what makes it a name.
    LhatType *super_type;

    // 03 の 4.3: the statement whose value the input answers with, when the
    // input is one of a session. NULL for a file, where nothing answers.
    const LhatNode *answering;

    // 05 の 8.6: the type L^ answers. One per check, since 8.8 lets a path
    // add to it and every mention has to see the same table. A session hands
    // its own in, so what one input registers is there for the next.
    LhatType *environment;

    // 02 の 14.16: what every typeof^(...) answers with. Nominal (8.8's
    // reason applies the same way: what it carries is fixed and comparing two
    // by shape would say nothing), so every mention has to be the very same
    // object -- kept here and handed forward by a session the way
    // `environment` is.
    LhatType *typeinfo_type;

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
        LhatCheckDiagnostic *bigger = (LhatCheckDiagnostic *)lhat_realloc(
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
    d->name = NULL;
    d->name_length = 0;
}

// 07 の 4 章: what a written name turned out to mean. Kept so that a tool
// reads the walk's own answer rather than resolving 8 章's scoping a second
// time. Dropped rather than failing the check when there is no room -- what
// it feeds is a convenience, and the program is no less checked without it.
static void record_resolution(Checker *c, const LhatNode *at, const Binding *b)
{
    LhatCheckResult *r = c->result;
    if (at->end <= at->offset) {
        return;  // no span: nothing to hover over
    }
    if (r->resolution_count == r->resolution_capacity) {
        size_t grown = r->resolution_capacity ? r->resolution_capacity * 2 : 64;
        LhatResolution *bigger = (LhatResolution *)lhat_realloc(
            r->resolutions, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        r->resolutions = bigger;
        r->resolution_capacity = grown;
    }

    LhatResolution *entry = &r->resolutions[r->resolution_count++];
    entry->use = at->offset;
    entry->use_end = at->end;
    entry->definition = b->offset;
    entry->type = b->type;
}

// The same, about a name. The text is borrowed from the source, which 6 章
// keeps alive as long as the result -- a copy per diagnostic would be paid
// for by every program, and almost none of them read one.
static void report_named(Checker *c, const LhatNode *at,
                         LhatCheckErrorCode code, const char *name,
                         size_t length)
{
    size_t before = c->result->diagnostic_count;
    report(c, at, code);
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
// 01 の 2.3改 (S34): the hat is part of the name -- 'self^' is a different
// name from 'self'. A spelling with more hats than one is the same name
// reached further out (this^^^ is the this^ two levels up), so the
// canonical name is the word plus one hat at most, and the count stays on
// the node for the constructs that stack. vm.c's node_name is the same rule
// on the other side, and the two have to agree exactly: what the checker
// looks a member up under is the string the machine will use as a key.
static bool node_name(const Checker *c, const LhatNode *node,
                      const char **text, size_t *length)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_TYPE_NAME: {
            *text = c->lexer->source->text + node->v.name.offset;
            // The hats sit right after the word in the source, so the
            // canonical name is the span cut after the first of them.
            size_t word = node->v.name.length >= node->v.name.hats
                              ? node->v.name.length - node->v.name.hats
                              : node->v.name.length;
            *length = node->v.name.hats > 0 ? word + 1 : word;
            return true;
        }
        case LHAT_NODE_SCOPE:
            return node_name(c, node->v.scope.name, text, length);
        case LHAT_NODE_FOCUS:
            // 16.2: the focus with no name written is called it^, and the
            // source need not contain the word for that to be its name.
            *text = "it^";
            *length = 3;
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

// 14.12改: whether this is super^ written out. Only the hatted spelling means
// it, so an ordinary name `super` is untouched.
static bool is_super_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           node_name(c, node, &name, &length) &&
           name_is(name, length, "super^");
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

// 15.1 asks which scope answered, not only what it holds: an f^ may write to
// a name its own body made and to no other. `found_in` may be NULL for the
// callers that only want the binding.
static Binding *scope_find_in(Scope *scope, const char *name, size_t length,
                              Scope **found_in)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = scope_find_local(s, name, length);
        if (b != NULL) {
            if (found_in != NULL) {
                *found_in = s;
            }
            return b;
        }
    }
    return NULL;
}

static Binding *scope_find(Scope *scope, const char *name, size_t length)
{
    return scope_find_in(scope, name, length, NULL);
}

// 01 の 2.3改 (S35): the stacked reach, passing over the innermost `skip`
// bindings of the name -- it^^ is the it^ one binding out, self^^/class^^
// the enclosing def^'s. The same search order as scope_find, so the two
// agree on which binding is "innermost".
static Binding *scope_find_skipping(Scope *scope, const char *name,
                                    size_t length, size_t skip)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = scope_find_local(s, name, length);
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
static Scope *scope_from(Scope *scope, const LhatNode *node)
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

static Binding *scope_add(Scope *scope, const char *name, size_t length,
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

static void scope_dispose(Scope *scope)
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

static LhatType *simple(Checker *c, LhatTypeKind kind)
{
    return lhat_type_simple(c->result->types, kind);
}

static LhatType *resolve_type(Checker *c, const LhatNode *node);
static LhatType *infer(Checker *c, const LhatNode *node);
static LhatType *environment_type(Checker *c);  // 05 の 8.6
static LhatType *typeinfo_type(Checker *c);     // 14.16
static void register_module_type(Checker *c, const char *module_name,
                                 LhatType *exports);  // 05 の 5.3
static LhatType *hosted_module(Checker *c, const LhatNode *path);  // 8.7
static void check_statement(Checker *c, const LhatNode *node);
static void check_block_in_scope(Checker *c, const LhatNode *node);
static LhatType *collect_exports(Checker *c, const LhatNode *statements);
static void check_statements(Checker *c, const LhatNode *statements);
static LhatType *infer_def(Checker *c, const LhatNode *node, LhatType *base);
static LhatType *compose_definitions(Checker *c, const LhatNode *node,
                                     LhatType *left, LhatType *right);
static LhatType *instance_of(const LhatType *definition);
static const LhatTypeMember *unimplemented_member(const LhatType *definition);
static const LhatTypeMember *find_member(const LhatType *table, const char *name,
                                         size_t length);
static LhatType *only(Checker *c, LhatType *type, LhatType *wanted);
static LhatType *without(Checker *c, LhatType *type, LhatType *unwanted);
static ParamVar *param_var_for(Checker *c, const LhatType *type);
static bool value_is_fresh(const Checker *c, const LhatNode *value,
                           const LhatType *type);
static void check_write_target(Checker *c, const LhatNode *target);
static void check_opaque_write(Checker *c, const LhatNode *target);
static bool receiver_is_own_coroutine(Checker *c, const LhatNode *receiver);

// 14.8: one number type over integers and reals. The rest are the plain
// builtin spellings.
static LhatType *builtin_type(Checker *c, const char *name, size_t length)
{
    if (name_is(name, length, "number^") || name_is(name, length, "int^") ||
        name_is(name, length, "float^")) {
        return simple(c, LHAT_TYPE_NUMBER);
    }
    if (name_is(name, length, "string^")) {
        return simple(c, LHAT_TYPE_STRING);
    }
    if (name_is(name, length, "bool^")) {
        return simple(c, LHAT_TYPE_BOOL);
    }
    if (name_is(name, length, "nil^")) {
        return simple(c, LHAT_TYPE_NIL);
    }
    if (name_is(name, length, "any^")) {
        return simple(c, LHAT_TYPE_ANY);
    }
    if (name_is(name, length, "error^")) {
        return simple(c, LHAT_TYPE_ERROR);
    }
    // 14.10: t^ and table^ are the same word. Bare, with no members listed,
    // it asks for nothing in particular -- the top of tables, which 13.7
    // notes is not the top of every value.
    if (name_is(name, length, "t^") || name_is(name, length, "table^")) {
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
        // 13.7, 14.10改: the unbounded tail. 'value' is NULL for an untyped
        // '...', which 13.7 makes any^.
        if (m->v.entry.variadic) {
            table->v.table.variadic = m->v.entry.value != NULL
                                          ? resolve_type(c, m->v.entry.value)
                                          : simple(c, LHAT_TYPE_ANY);
            continue;
        }
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
           name_is(name, length, "self^");
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
                                : simple(c, LHAT_TYPE_PENDING));
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
            // 13.9 with 15.3改: 'c^{ f^R -> Y;, T }'. An omitted R or Y is
            // nil^ -- 13.2's absent result and an empty parameter list both
            // mean "nothing here", and 04 の 11.3 already spells that nil^.
            return lhat_type_coro(
                c->result->types,
                node->v.coroutine.receive != NULL
                    ? resolve_type(c, node->v.coroutine.receive)
                    : simple(c, LHAT_TYPE_NIL),
                node->v.coroutine.produce != NULL
                    ? resolve_type(c, node->v.coroutine.produce)
                    : simple(c, LHAT_TYPE_NIL),
                resolve_type(c, node->v.coroutine.result),
                node->v.coroutine.is_function);

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
           type->kind == LHAT_TYPE_PENDING ||
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

// 11.8: an operator is a member whose name is the operator itself. NULL for
// the ones no op^ may define -- 11.8 keeps and^, or^ and '!' built in, and
// 11.5's comparisons decide by 14.12's disjointness rather than by asking.
static const char *operator_name(LhatOpKind op, size_t *length)
{
    switch (op) {
        case LHAT_OP_CONCAT:   *length = 2; return "..";
        case LHAT_OP_ADD:      *length = 1; return "+";
        case LHAT_OP_SUB:      *length = 1; return "-";
        case LHAT_OP_MUL:      *length = 1; return "*";
        case LHAT_OP_DIV:      *length = 1; return "/";
        case LHAT_OP_FLOORDIV: *length = 2; return "//";
        case LHAT_OP_MOD:      *length = 1; return "%";
        case LHAT_OP_POW:      *length = 2; return "**";
        default:               *length = 0; return NULL;
    }
}

// 11.8: whether this member name is an operator. The eight spellings above
// are the ones op^ writes, and 01 の 6 章 keeps a program from writing any of
// them as an ordinary name -- so a member of one of these names got there
// through op^ and nowhere else.
static bool is_operator_name(const char *name, size_t length)
{
    static const LhatOpKind written[] = {
        LHAT_OP_CONCAT, LHAT_OP_ADD, LHAT_OP_SUB,      LHAT_OP_MUL,
        LHAT_OP_DIV,    LHAT_OP_MOD, LHAT_OP_FLOORDIV, LHAT_OP_POW,
    };
    for (size_t i = 0; i < sizeof written / sizeof written[0]; i++) {
        size_t spelt = 0;
        const char *text = operator_name(written[i], &spelt);
        if (text != NULL && spelt == length &&
            memcmp(text, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// 11.8: an operator is an f^ that takes self^ and one argument. 11.1 makes it
// a function -- a p^ could carry side effects into an operator -- and 14.4
// puts the left operand in self^, which leaves the right one as the single
// parameter. Nothing later checks this: the call site reads the signature and
// believes it, and the machine hands over a receiver and one argument
// whatever the body declared.
static void check_operator_shape(Checker *c, const LhatNode *at,
                                 const LhatType *type)
{
    if (type == NULL || type->kind == LHAT_TYPE_UNKNOWN ||
        type->kind == LHAT_TYPE_PENDING) {
        return;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        // 14.12: an overloaded one is every arm at once, and each has to be
        // an operator on its own.
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            check_operator_shape(c, at, arm->type);
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
    if (type->kind != LHAT_TYPE_FUNC || !type->v.func.is_function ||
        !type->v.func.takes_self || params != 1 || type->v.func.yields) {
        report(c, at, LHAT_CHECK_ERR_BAD_OPERATOR);
    }
}

// 11.8: what a built-in type answers. The checker knows these rather than any
// L^ writing them out, the way 15.6 gives a coroutine start(). 14.4 puts the
// left operand in self^, so the right one is the only parameter.
static LhatType *builtin_operator(Checker *c, LhatTypeKind carrier,
                                  const char *name, size_t length)
{
    LhatTypeKind takes;
    if (carrier == LHAT_TYPE_STRING && name_is(name, length, "..")) {
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
    lhat_type_add_param(c->result->types, signature, simple(c, takes));
    signature->v.func.result = simple(c, takes);
    return signature;
}

// 11.1: an operator is a function, and 11.3 asks structurally whether a type
// carries it. 01 の 6 章 spells a member name as an identifier, so an
// operator is a name no program can write by hand and nothing of the
// writer's can collide with it.
static LhatType *operator_member(Checker *c, const LhatType *type,
                                 const char *name, size_t length)
{
    if (type == NULL || name == NULL) {
        return NULL;
    }
    if (type->kind == LHAT_TYPE_TABLE) {
        const LhatTypeMember *m = find_member(type, name, length);
        return m != NULL ? m->type : NULL;
    }
    return builtin_operator(c, type->kind, name, length);
}

// 03 の 3.5: a gap in inference, and 13.7's any^ which is every value at
// once, are both left to the machine rather than reported here.
static bool operator_undecided(const LhatType *type)
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

// 01 の 2.3改 (S35): the canonical name cuts after the first hat, so it^ and
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
    return node_name(c, a, &na, &la) && node_name(c, b, &nb, &lb) &&
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
    Narrowing *n = (Narrowing *)lhat_calloc(1, sizeof *n);
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
static void drop_narrowings_for(Checker *c, const LhatNode *target)
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
                // 16.3 and 17 章: the if^ and when^ forms do not iterate, so
                // a break^ inside one still leaves the loop out here. Every
                // other form is a loop, and standing inside it is one more
                // loop for a level to count past.
                if (node->v.loop.kind == LHAT_FOR_IF ||
                    node->v.loop.kind == LHAT_FOR_WHEN) {
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
static bool always_exits(const LhatNode *node)
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

    if (op != LHAT_OP_ISA) {
        return;
    }

    const LhatNode *path = condition->v.binary.left;
    if (!narrowable(path)) {
        return;
    }

    LhatType *current = infer(c, path);
    LhatType *tested = resolve_type(c, condition->v.binary.right);
    LhatType *inside =
        truth ? only(c, current, tested) : without(c, current, tested);

    // 03 の 3.4: a parameter still being decided says nothing to narrow, so
    // both sides hand its own object straight back. Inside the branch that
    // object would be read as the parameter itself and every use of it as a
    // demand -- which is exactly what a narrowing is written to prevent. The
    // true side knows what was tested for; the false side knows only what it
    // is not, which is not a type here.
    if (param_var_for(c, inside) != NULL) {
        inside = truth ? tested : simple(c, LHAT_TYPE_UNKNOWN);
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
static ParamVar *param_var_for(Checker *c, const LhatType *type)
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
static bool mentions_function_coroutine(const LhatType *type, unsigned depth)
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
                if (mentions_function_coroutine(m->type, depth + 1)) {
                    return true;
                }
            }
            return mentions_function_coroutine(type->v.table.variadic,
                                               depth + 1);

        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (mentions_function_coroutine(p->type, depth + 1)) {
                    return true;
                }
            }
            return mentions_function_coroutine(type->v.func.result, depth + 1) ||
                   mentions_function_coroutine(type->v.func.variadic, depth + 1);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (mentions_function_coroutine(arm->type, depth + 1)) {
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
            (LhatTypeMember *)find_member(merged, m->name, m->name_length);
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
static void constrain(Checker *c, LhatType *value, LhatType *wanted)
{
    ParamVar *pv = param_var_for(c, value);
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
static void close_param_var(Checker *c, const LhatType *value)
{
    ParamVar *pv = param_var_for(c, value);
    if (pv != NULL) {
        pv->closed = true;
    }
}

// 3.4: reading a member off a parameter demands a structure carrying it. The
// member's own type is left unknown^ -- what may be done with it is decided by
// what the member turns out to be, and a call through it says too little to
// pin a signature down (13.1 asks the f^/p^ distinction and 13.2 the presence
// of a result, and neither is readable off one call).
static void constrain_member(Checker *c, LhatType *target, const char *name,
                             size_t length)
{
    if (param_var_for(c, target) == NULL) {
        return;
    }
    LhatType *shape = lhat_type_table(c->result->types);
    if (shape == NULL ||
        lhat_type_add_member(c->result->types, shape, name, length,
                             simple(c, LHAT_TYPE_UNKNOWN)) == NULL) {
        return;
    }
    constrain(c, target, shape);
}

static ParamVar *push_param_var(Checker *c, LhatType *slot)
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
static void settle_param_vars(Checker *c, ParamVar *mark)
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
        // relaxed forgives (3.5改: no check is inserted; a real mismatch
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
// Expressions
// ---------------------------------------------------------------------------

static void expect(Checker *c, const LhatNode *at, LhatType *value,
                   LhatType *target, LhatCheckErrorCode code)
{
    // 03 の 3.4: a parameter whose type the body is deciding is not being
    // checked here -- this position is one of the things deciding it. Most of
    // what a body demands arrives through this one call.
    if (param_var_for(c, value) != NULL) {
        constrain(c, value, target);
        return;
    }

    // 03 の 3.1・3.5改: strict reports a lingering gap in inference here;
    // relaxed waves unknown^ through on purpose. 3.5改 withdrew the inserted
    // runtime check that was once meant to stand behind the waving -- what a
    // waved-through mismatch meets now is the machine's own instruction
    // check, which panics where it lands (04 の 11.6).
    bool ok = c->strict ? lhat_type_conforms_strict(value, target)
                        : lhat_type_conforms(value, target);
    if (!ok) {
        report(c, at, code);
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
static LhatType *infer_operator(Checker *c, const LhatNode *node, LhatOpKind op,
                                LhatType *left, LhatType *right)
{
    size_t length = 0;
    const char *name = operator_name(op, &length);
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
    if (param_var_for(c, left) != NULL) {
        if (name[0] == '.') {
            return NULL;
        }
        LhatType *number = simple(c, LHAT_TYPE_NUMBER);
        constrain(c, left, number);
        left = number;
    }

    if (operator_undecided(left)) {
        return NULL;
    }

    LhatType *carrier = operator_member(c, left, name, length);
    if (carrier == NULL) {
        report(c, node->v.binary.left, LHAT_CHECK_ERR_NO_OPERATOR);
        return NULL;
    }
    if (carrier->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    LhatType *wanted =
        carrier->v.func.params != NULL ? carrier->v.func.params->type : NULL;
    // 03 の 3.4: a parameter on this side is undecided in the sense above, but
    // what the operator takes is a demand on it rather than a gap to wave
    // through -- expect is what tells the two apart.
    if (wanted != NULL &&
        (param_var_for(c, right) != NULL || !operator_undecided(right))) {
        expect(c, node->v.binary.right, right, wanted,
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
        LhatType *env = environment_type(c);
        if (member == NULL || env == NULL) {
            return NULL;
        }
        const LhatTypeMember *m = find_member(env, member, strlen(member));
        return m != NULL ? m->type : NULL;
    }
    return NULL;
}

static LhatType *infer_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node, &name, &length)) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 01 の 2.3改 (S35): the stacked reach. this^^ walks the chain of
    // enclosing bodies; it^^/self^^/class^^ walk past inner bindings of the
    // same name -- the same search vm.c makes, so the two agree on which
    // binding a count lands on. The parser admits no other word here.
    if (node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats > 1) {
        size_t levels = node->v.name.hats - 1;
        if (name_is(name, length, "this^")) {
            struct ThisLink *link = c->this_link;
            for (size_t i = 0; i < levels && link != NULL; i++) {
                link = link->outer;
            }
            if (link == NULL) {
                report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            return link->type;
        }
        Binding *outer = scope_find_skipping(c->scope, name, length, levels);
        if (outer == NULL) {
            report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return simple(c, LHAT_TYPE_UNKNOWN);
        }
        outer->reached = true;
        return outer->type;
    }

    // A few hat identifiers are values rather than names (01 の 2.2).
    if (node->kind == LHAT_NODE_HAT_IDENT) {
        if (name_is(name, length, "true^") || name_is(name, length, "false^")) {
            return simple(c, LHAT_TYPE_BOOL);
        }
        if (name_is(name, length, "nil^")) {
            return simple(c, LHAT_TYPE_NIL);
        }
        // 15.10: this^ names the subroutine running, which is what lets a
        // body with no name recurse. Only the hatted spelling means it, so
        // an ordinary name `this` is untouched.
        if (name_is(name, length, "this^")) {
            if (c->this_type == NULL) {
                report(c, node, LHAT_CHECK_ERR_THIS_OUTSIDE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 03 の 3.4 counts it the same way a call by name is counted.
            c->saw_self_call = true;
            return c->this_type;
        }
        // 14.12改: super^ is the member an override^ is replacing. Named on
        // its own it is an ordinary value, so 14.4 applies and the receiver
        // is written out; called directly it is the bound form, which
        // infer_call takes.
        if (name_is(name, length, "super^")) {
            if (c->super_type == NULL) {
                report(c, node, LHAT_CHECK_ERR_SUPER_OUTSIDE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            return c->super_type;
        }
        // 05 の 8.6: the machine's own table, there without being imported.
        if (name_is(name, length, "L^")) {
            LhatType *env = environment_type(c);
            return env != NULL ? env : simple(c, LHAT_TYPE_UNKNOWN);
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
        from = scope_from(c->scope, node);
        if (from == NULL) {
            report(c, node, LHAT_CHECK_ERR_SCOPE_TOO_FAR);
            return simple(c, LHAT_TYPE_UNKNOWN);
        }
    }

    Binding *b = scope_find(from, name, length);
    if (b == NULL) {
        // 05 の 8.2: what the host bound before anything ran. Asked after
        // every scope, so a let^ of the same spelling shadows it -- and what
        // it reaches stays readable as L^.<member>, since 8.1 keeps the hat
        // identifier out of the spellings a let^ can make.
        LhatType *bound = initial_binding_type(c, name, length);
        if (bound != NULL) {
            return bound;
        }
        report_named(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    // 8.7: the name is visible throughout the scope, which is what makes
    // mutual recursion work, but its value only exists once its let^ has run.
    // A subroutine body does not run where it is written, so the rule only
    // applies outside one.
    if (!b->reached && c->deferred == 0) {
        report(c, node, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    }
    record_resolution(c, node, b);
    return b->type;
}

static LhatType *infer_binary(Checker *c, const LhatNode *node)
{
    LhatOpKind op = node->v.binary.op;

    // 04 の 4.1 and 11.7: both drop one arm and put a value in its place.
    if (op == LHAT_OP_CATCH || op == LHAT_OP_NIL_ELSE) {
        LhatType *left = infer(c, node->v.binary.left);
        LhatType *unwanted = simple(c, op == LHAT_OP_CATCH ? LHAT_TYPE_ERROR
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
            Binding *caught = scope_add(&scope, "it^", 3,
                                        only(c, left, unwanted), node->offset);
            if (caught != NULL) {
                caught->reached = true;
            }
            right = infer(c, node->v.binary.right);
            c->scope = outer;
            scope_dispose(&scope);
        } else {
            right = infer(c, node->v.binary.right);
        }

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

    // 13.11: isa^ takes a type on the right, so the right side is not a
    // value. 11.6改 moved the type-fit question here from is^, which now
    // asks identity and reads an ordinary value on both sides (below, with
    // the rest of the comparisons).
    if (op == LHAT_OP_ISA) {
        LhatType *asked = resolve_type(c, node->v.binary.right);
        // 13.7: any^ is the top of every value, so this holds of whatever is
        // on the left and the question is empty. 13.11 refuses to read the
        // left's inferred type against the right -- that would narrow the
        // escape hatch 13.7 exists to provide -- but this one is decided by
        // the right side alone, whatever the left turns out to be.
        if (asked != NULL && asked->kind == LHAT_TYPE_ANY) {
            report(c, node, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
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
            // 11.4改: arithmetic asks 11.3's question the way '..' does, so a
            // written op^ answers it. 14.8's number^ carries all seven built
            // in, which leaves ordinary arithmetic exactly as it was.
            LhatType *answer = infer_operator(c, node, op, left, right);
            return answer != NULL ? answer : simple(c, LHAT_TYPE_NUMBER);
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
        case LHAT_OP_IS:
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

        case LHAT_OP_CONCAT: {
            // 11.2: '..' is concatenation in general. 11.3 asks the left
            // operand for it -- 14.4 makes that the receiver -- and the right
            // one is the argument, which is what lets one type answer several
            // right-hand types through 14.12's overload^.
            left = require_value(c, node->v.binary.left, left);
            right = require_value(c, node->v.binary.right, right);

            // 14.5: between two definitions '..' composes, and never calls an
            // op^.. either of them carries. The literal form was taken above;
            // this is the one written with names.
            if (left != NULL && left->kind == LHAT_TYPE_TABLE &&
                left->v.table.is_definition && right != NULL &&
                right->kind == LHAT_TYPE_TABLE &&
                right->v.table.is_definition) {
                return compose_definitions(c, node, left, right);
            }

            LhatType *joined = infer_operator(c, node, op, left, right);
            return joined != NULL ? joined : simple(c, LHAT_TYPE_UNKNOWN);
        }

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

static LhatType *infer_call(Checker *c, const LhatNode *node)
{
    LhatType *callee = infer(c, node->v.access.target);

    size_t given = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        given++;
    }

    if (callee == NULL || callee->kind == LHAT_TYPE_UNKNOWN ||
        callee->kind == LHAT_TYPE_PENDING) {
        for (const LhatNode *arg = node->v.access.argument; arg != NULL;
             arg = arg->next) {
            infer(c, arg);
        }
        // 03 の 3.1・3.5、P6: a callee still pending^ (a mutually recursive
        // partner not yet checked) makes this call's result pending^ too,
        // not merely unknown^ -- strict needs to keep seeing the gap if it
        // survives to where a concrete type is wanted.
        return simple(c, callee != NULL && callee->kind == LHAT_TYPE_PENDING
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
                // 15.1, 15.5: f^ may call only f^, whichever arm 14.12
                // resolved to -- except a yieldable p^, whose call alone
                // stays referentially transparent (see the plain-call site
                // above for why).
                if (c->in_function && !arm->type->v.func.is_function &&
                    !arm->type->v.func.yields) {
                    report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
                }
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
        report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
    }

    // 14.15: an instance carries a value under every name its definition
    // holds, so one still only declared has nothing to make. Reported where
    // the construction is written, which is what the writer has to change.
    if (node->v.access.target != NULL &&
        node->v.access.target->kind == LHAT_NODE_MEMBER) {
        const char *called = NULL;
        size_t called_length = 0;
        if (node_name(c, node->v.access.target->v.access.argument, &called,
                      &called_length) &&
            name_is(called, called_length, "new")) {
            LhatType *owner = infer(c, node->v.access.target->v.access.target);
            const LhatTypeMember *hole = unimplemented_member(owner);
            if (hole != NULL) {
                report_named(c, node, LHAT_CHECK_ERR_STILL_ABSTRACT, hole->name,
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
        !is_super_name(c, node->v.access.target)) {
        declared++;
        skip = 1;
    }

    // 13.7: 'expr...' as the last argument spreads a collected table back
    // into the variadic tail. It stands for zero or more of the callee's
    // own, so what came before it has to be exactly the fixed arguments --
    // 'declared' does not count the variadic one (v.func.variadic is kept
    // apart from `params`, the same way self^ is).
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
            report(c, last_arg, LHAT_CHECK_ERR_NOT_VARIADIC);
        }
        if (given != declared) {
            report(c, node, LHAT_CHECK_ERR_ARITY);
        }
    } else if (given < declared ||
              (given > declared && callee->v.func.variadic == NULL)) {
        // 13.4: a written default does not make a parameter optional. What it
        // fills in is a call site being written by an editor, so a call that
        // reaches here still owes every declared argument.
        report(c, node, LHAT_CHECK_ERR_ARITY);
    }

    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        // 13.7: checked against the element type directly, rather than
        // through the ordinary per-argument loop below -- there is no
        // parameter position for it to line up with.
        if (arg->kind == LHAT_NODE_SPREAD) {
            LhatType *spread = infer(c, arg->v.jump.value);
            if (callee->v.func.variadic != NULL && spread != NULL &&
                spread->kind == LHAT_TYPE_TABLE &&
                spread->v.table.variadic != NULL) {
                expect(c, arg, spread->v.table.variadic,
                      callee->v.func.variadic, LHAT_CHECK_ERR_MISMATCH);
            } else if (callee->v.func.variadic != NULL) {
                report(c, arg, LHAT_CHECK_ERR_NOT_VARIADIC);
            }
            break;
        }
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
        // 15.3改: the coroutine carries the kind of the body it came from,
        // which is what decides who may advance it (15.6改).
        return lhat_type_coro(c->result->types,
                              callee->v.func.yield_receive != NULL
                                  ? callee->v.func.yield_receive
                                  : simple(c, LHAT_TYPE_NIL),
                              callee->v.func.yield_produce != NULL
                                  ? callee->v.func.yield_produce
                                  : simple(c, LHAT_TYPE_NIL),
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
    // 13.7: an unbounded tail is walked as more positions beyond any listed,
    // every one of the same element type.
    if (over->v.table.variadic != NULL) {
        keys = lhat_type_union(c->result->types, keys,
                               simple(c, LHAT_TYPE_NUMBER));
        values =
            lhat_type_union(c->result->types, values, over->v.table.variadic);
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

// 02 の 14.17: every value carries this, the way 05 の 8.5 gives a coroutine
// its operations -- a member of the value rather than a name a unit has to
// import. An f^: writing a value down changes nothing, which is 11.1's
// reason for keeping an operator pure applied to the same question.
static LhatType *builtin_tostring(Checker *c, const LhatType *target)
{
    LhatType *plain = lhat_type_func(c->result->types, true);
    plain->v.func.takes_self = true;
    plain->v.func.result = simple(c, LHAT_TYPE_STRING);
    if (target == NULL || target->kind != LHAT_TYPE_NUMBER) {
        return plain;
    }
    // 14.17: a number^ also takes a format. 14.12 makes the two an
    // intersection, and the counts they allow -- none and one -- keep them
    // from overlapping without the types being looked at.
    LhatType *formatted = lhat_type_func(c->result->types, true);
    formatted->v.func.takes_self = true;
    lhat_type_add_param(c->result->types, formatted,
                        simple(c, LHAT_TYPE_STRING));
    formatted->v.func.result = simple(c, LHAT_TYPE_STRING);
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

static LhatType *infer_member(Checker *c, const LhatNode *node)
{
    LhatType *target = infer(c, node->v.access.target);
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.access.argument, &name, &length)) {
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    if (target == NULL || target->kind == LHAT_TYPE_UNKNOWN ||
        target->kind == LHAT_TYPE_PENDING) {
        // 03 の 3.4: a parameter read for a member has to carry one, and
        // 14.10's "at least these" is exactly that demand written as a type.
        constrain_member(c, target, name, length);
        // 03 の 3.1・3.5、P6: a target still pending^ makes this member's
        // type pending^ too, not merely unknown^ -- the gap has to survive
        // for strict to see it if it reaches somewhere a concrete type was
        // wanted.
        return simple(c, target != NULL && target->kind == LHAT_TYPE_PENDING
                              ? LHAT_TYPE_PENDING
                              : LHAT_TYPE_UNKNOWN);
    }

    // 04 の 11.4改: under relaxed, a T|nil^ value may be referenced as T.
    // The checker steps aside; a nil^ actually arriving meets the machine's
    // own instruction check and panics where it lands, with 11.6's line.
    // strict keeps refusing -- narrowing (isa^, ??, ?.) is the spelling
    // there. Only nil^ is stepped past: a union of two real types still has
    // no one member type to answer with.
    if (!c->strict && target->kind == LHAT_TYPE_UNION) {
        LhatType *bare = without(c, target, simple(c, LHAT_TYPE_NIL));
        if (bare != NULL && bare->kind != LHAT_TYPE_UNION) {
            target = bare;
        }
    }

    // 04 の 2.3: every kind carries message and cause without declaring them
    // -- so they answer however much of the kind is known: a leaf, a
    // declaration's whole set, or error^ alone (4.2's it^ can be any of the
    // three). A declared field is the leaf's own and wants the narrowing.
    const LhatTypeMember *members = NULL;
    if (target->kind == LHAT_TYPE_TABLE) {
        members = target->v.table.members;
    } else if (target->kind == LHAT_TYPE_ERROR_KIND ||
               target->kind == LHAT_TYPE_ERROR_SET ||
               target->kind == LHAT_TYPE_ERROR) {
        if (name_is(name, length, "message")) {
            return simple(c, LHAT_TYPE_STRING);
        }
        if (name_is(name, length, "cause")) {
            return lhat_type_union(c->result->types, simple(c, LHAT_TYPE_ERROR),
                                   simple(c, LHAT_TYPE_NIL));
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
            (name_is(name, length, "start") ||
             name_is(name, length, "resume") ||
             name_is(name, length, "dispose")) &&
            !receiver_is_own_coroutine(c, node->v.access.target)) {
            report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
        }
        if (name_is(name, length, "start")) {
            // 15.2改: runs the body from the top, for a coroutine that has
            // never been resumed. Takes nothing, since nothing has been
            // yield^ed yet to send a value to. Answers the same union a
            // resume does -- which is the yield type alone when the third
            // type is absent, since a coroutine that cannot end never
            // answers with one (13.9改).
            LhatType *signature = lhat_type_func(c->result->types, advances);
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
            LhatType *signature = lhat_type_func(c->result->types, advances);
            lhat_type_add_param(c->result->types, signature,
                                target->v.coroutine.receive);
            signature->v.func.result = answer;
            return signature;
        }
        if (name_is(name, length, "dispose")) {
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
        if (name_is(name, length, "done")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        // done() alone leaves a fresh coroutine and a suspended one looking
        // alike, so a consumer handed one it did not make could not tell
        // which of start() and resume() this is.
        if (name_is(name, length, "started")) {
            LhatType *signature = lhat_type_func(c->result->types, true);
            signature->v.func.result = simple(c, LHAT_TYPE_BOOL);
            return signature;
        }
        if (builtin_named(name, length, "tostring", false)) {
            return builtin_tostring(c, target);  // 14.17
        }
        report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        return simple(c, LHAT_TYPE_UNKNOWN);
    } else {
        // 14.17: nil^, bool^, number^, string^ and the rest hold no members
        // of their own, and this is the one every value answers.
        if (builtin_named(name, length, "tostring", false)) {
            return builtin_tostring(c, target);
        }
        report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        return simple(c, LHAT_TYPE_UNKNOWN);
    }

    for (const LhatTypeMember *m = members; m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            // 14.5改: carried by both sides of a composition, so reading it
            // here would be picking one of two for the writer. 14.4's
            // 'let^ f = A.m' is how the side is named.
            if (m->ambiguous) {
                report_named(c, node, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER, name,
                             length);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            return m->type;
        }
    }

    // 16.3: `in^ e` asks e for the coroutine to walk, and a table answers
    // with one over its keys without anything being written. This comes
    // after the search, not before it, because 16.3 lets a written iterate
    // win -- the same order the machine reads it in.
    if (builtin_named(name, length, "iterate", plain_table_type(target))) {
        // 13.8 has no tuples, so a pair is a table. The walk sends nothing
        // in and ends without a value, which 04 の 11.3 spells nil^.
        //
        // 15.3改: the built-in walk changes nothing, so it is an f^ coroutine
        // -- which is what lets 'for^ k, v in^ t' stand inside an f^ body.
        LhatType *walk = lhat_type_coro(c->result->types,
                                        simple(c, LHAT_TYPE_NIL),
                                        table_walk_pair(c, target),
                                        simple(c, LHAT_TYPE_NIL), true);
        LhatType *signature = lhat_type_func(c->result->types, true);
        signature->v.func.result = walk;
        return signature;
    }

    // 14.17, the same order and for the same reason: a written tostring is
    // the one that answers, and this is what a table falls back to.
    if (builtin_named(name, length, "tostring", plain_table_type(target))) {
        return builtin_tostring(c, target);
    }

    report_named(c, node, LHAT_CHECK_ERR_NO_MEMBER, name, length);
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
                asked->kind != LHAT_TYPE_PENDING &&
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
    if (candidate == NULL || candidate->kind == LHAT_TYPE_UNKNOWN ||
        candidate->kind == LHAT_TYPE_PENDING) {
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
        if (param == node->v.func.params && is_self_marker(c, param)) {
            func->v.func.takes_self = true;
            continue;
        }
        LhatType *type = param->v.param.type != NULL
                             ? resolve_type(c, param->v.param.type)
                             : simple(c, LHAT_TYPE_PENDING);
        if (param->v.param.variadic) {
            func->v.func.variadic =
                param->v.param.type != NULL ? type : simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func, type);
    }
    if (node->v.func.return_type != NULL) {
        func->v.func.result = resolve_type(c, node->v.func.return_type);
    }
    return func;
}

static LhatType *infer_func(Checker *c, const LhatNode *node)
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
        if (param == node->v.func.params && is_self_marker(c, param)) {
            func->v.func.takes_self = true;
            continue;
        }
        // 05 の 4.3: what leaves the unit is not decided by reading a body.
        if (param->v.param.type == NULL && c->exporting > 0) {
            report(c, param, LHAT_CHECK_ERR_PUBLIC_NEEDS_TYPE);
        }
        LhatType *type = param->v.param.type != NULL
                             ? resolve_type(c, param->v.param.type)
                             : simple(c, LHAT_TYPE_PENDING);
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
        LhatType *fallback = infer(c, param->v.param.fallback);
        if (fallback != NULL && param->v.param.type != NULL) {
            expect(c, param->v.param.fallback, fallback, type,
                   LHAT_CHECK_ERR_MISMATCH);
        }
        if (param->v.param.variadic) {
            LhatType *element =
                param->v.param.type != NULL ? type : simple(c, LHAT_TYPE_ANY);
            func->v.func.variadic = element;
            // 13.7: '...' inside the body names what was collected. 14.10改's
            // form for that is a table whose sequence is unbounded, one
            // element type throughout -- the same shape a written
            // 't^{ ...:T }' names, since both describe the same thing.
            LhatType *collected = lhat_type_table(c->result->types);
            collected->v.table.variadic = element;
            Binding *b = scope_add(&body, "...", 3, collected, param->offset);
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
            push_param_var(c, type);
        }

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
    bool outer_in_function = c->in_function;
    Scope *outer_body_scope = c->body_scope;

    c->scope = &body;
    c->declared_result = declared;
    c->inferred_result = NULL;
    c->saw_self_call = false;
    c->recursive_return = false;
    c->valueless_return = false;
    c->this_type = func;
    // 15.10改: the chain this^^ walks. Lives here on the C stack, exactly
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
        check_block_in_scope(c, node->v.func.body);
    } else {
        check_statement(c, node->v.func.body);
    }

    // 03 の 3.4: every demand this body makes is in. Settled before the result
    // is put together, since a return^ of a parameter carries this very object
    // into it.
    settle_param_vars(c, param_mark);

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
    //
    // 15.3改 with 15.5: a yieldable one answers a coroutine, and it answers
    // one whatever the body goes on to do. Reaching the end of the body ends
    // the coroutine (13.9改 puts that in the third type); it is not the
    // function failing to answer. So 13.2 is already satisfied here.
    if (leaves_without_value && node->v.func.is_function &&
        !node->v.func.yields) {
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

    // 15.3改: an f^ coroutine may not leave the body that made it. Reaching
    // the outside is what would make advancing it observable from there, and
    // it reaches out through a table member or a nested signature as readily
    // as through the result itself -- so the whole result type is read rather
    // than its outermost shape. A p^ coroutine needs nothing here: advancing
    // one is a p^ call, which 15.1 already refuses inside an f^.
    if (node->v.func.is_function &&
        mentions_function_coroutine(func->v.func.result, 0)) {
        report(c, node, LHAT_CHECK_ERR_COROUTINE_ESCAPES);
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

    scope_dispose(&body);
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

// 14.11 makes new return an instance, so a definition's own structure is
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
static const LhatTypeMember *unimplemented_member(const LhatType *definition)
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
    const LhatType *instance = instance_of(definition);
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
            report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        }
        return replacement;
    }

    LhatType *overlapped = NULL;
    size_t overlaps = 0;
    for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
         arm = arm->next) {
        if (signatures_overlap(replacement, arm->type)) {
            overlapped = arm->type;
            overlaps++;
        }
    }

    if (overlaps == 0) {
        report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        return replacement;
    }
    if (overlaps > 1) {
        // 14.12: widening the arguments may reach two of them, and then
        // which is being replaced is not decidable. Reported as the overlap
        // it is, since that is what the writer has to take apart.
        report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
        return replacement;
    }
    if (!lhat_type_conforms(replacement, overlapped)) {
        report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        return replacement;
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

static LhatType *check_same_name(Checker *c, const LhatNode *entry,
                                 const LhatTypeMember *inherited,
                                 LhatType *replacement)
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
            report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        }
        return replacement;
    }

    // 14.15: a declaration is not a definition of the member, so filling it
    // in is not the collision 14.12 is about -- no marker is wanted. What the
    // declaration does ask is that the value fit the type it wrote.
    if (inherited->abstract) {
        if (modifier != LHAT_DEF_PLAIN) {
            report(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
        } else if (!lhat_type_conforms(replacement, inherited->type)) {
            report(c, entry, LHAT_CHECK_ERR_MISMATCH);
        }
        return replacement;
    }

    switch (modifier) {
        case LHAT_DEF_PLAIN:
            report(c, entry, LHAT_CHECK_ERR_MEMBER_EXISTS);
            return replacement;

        case LHAT_DEF_OVERRIDE:
            // 14.12: what is replaced is the one existing candidate the new
            // signature overlaps. A name that was overloaded carries several,
            // and comparing against all of them at once would refuse every
            // replacement -- no one signature is usable where an intersection
            // of them was.
            return override_one(c, entry, inherited->type, replacement);

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
static LhatType *compose_definitions(Checker *c, const LhatNode *node,
                                     LhatType *left, LhatType *right)
{
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    definition->v.table.is_definition = true;
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    const LhatType *left_instance = instance_of(left);
    const LhatType *right_instance = instance_of(right);

    // new is on both sides whether or not either wrote one (14.11), so it
    // would collide every time. It is rebuilt at the end instead.
    for (const LhatTypeMember *m = left->v.table.members; m != NULL;
         m = m->next) {
        if (name_is(m->name, m->name_length, "new")) {
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
        if (name_is(m->name, m->name_length, "new")) {
            continue;
        }
        // 14.15: one side declaring what the other provides is the pairing
        // the declaration exists for, and neither order is a collision. What
        // is provided wins; the hole stays open only while both leave it so.
        const LhatTypeMember *held =
            find_member(definition, m->name, m->name_length);
        if (held != NULL && (held->abstract || m->abstract)) {
            if (m->abstract) {
                continue;  // what is already there is the better answer
            }
        } else if (held != NULL && m->pending) {
            // 14.15改: this is what the pending override^ was waiting for.
            // 14.12's check is the one it would have had at the def^, run
            // here instead because here is where the two finally meet.
            if (!lhat_type_conforms(m->type, held->type)) {
                report(c, node, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
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
                find_member(instance, m->name, m->name_length);
            if (held != NULL && (held->abstract || m->abstract)) {
                if (m->abstract) {
                    continue;
                }
            } else if (held != NULL && m->pending) {
                // The definition side settled it, and reported anything
                // there was to report (14.7 puts a method in both).
                set_member_marked(c, instance, m->name, m->name_length,
                                  m->type, false, held->pending);
                continue;
            } else if (held != NULL) {
                // A method sits in both tables (14.7), and the definition
                // side settled it -- mirror that here.
                const LhatTypeMember *shared =
                    find_member(definition, m->name, m->name_length);
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
                    report(c, node, LHAT_CHECK_ERR_COMPOSE_COLLIDES);
                }
            }
            set_member_marked(c, instance, m->name, m->name_length, m->type,
                              m->abstract, m->pending);
        }
    }

    // 14.5 composes to make something new, so the constructor has to build
    // the composed instance. The parameters come from the right, which is the
    // more derived of the two.
    LhatType *constructor = lhat_type_func(c->result->types, true);
    const LhatTypeMember *inherited = find_member(right, "new", 3);
    if (inherited == NULL) {
        inherited = find_member(left, "new", 3);
    }
    if (inherited != NULL && inherited->type != NULL &&
        inherited->type->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = inherited->type->v.func.params; p != NULL;
             p = p->next) {
            lhat_type_add_param(c->result->types, constructor, p->type);
        }
        constructor->v.func.variadic = inherited->type->v.func.variadic;
    }
    constructor->v.func.result = instance;
    set_member(c, definition, "new", 3, constructor);
    return definition;
}

static LhatType *infer_def(Checker *c, const LhatNode *node, LhatType *base)
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
            // 14.15: a field the composition has to provide carries its type
            // and no value. 14.11 would otherwise want an initializer here.
            if (field->v.entry.declared) {
                set_member_as(c, instance, name, length,
                              resolve_type(c, field->v.entry.value), true);
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
    Binding *receiver = scope_add(&members, "self^", 5, instance, node->offset);
    Binding *owner = scope_add(&members, "class^", 6, definition, node->offset);
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
        const LhatTypeMember *hidden = find_member(definition, name, length);

        // 14.15: a declaration carries a type and no value, and says the
        // composition has to provide the member. It is not a definition of
        // it, so 14.12 has nothing to check here.
        if (entry->v.entry.declared) {
            LhatType *declared = resolve_type(c, entry->v.entry.value);
            if (hidden != NULL && !hidden->abstract) {
                // Already provided, so the declaration asks for nothing.
                report(c, entry, LHAT_CHECK_ERR_ALREADY_PROVIDED);
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
        LhatType *type = infer(c, entry->v.entry.value);
        c->super_type = outer_super;
        // 14.12: two members of one name in a single def^ need a marker too,
        // so what is already there has to include this def^'s earlier entries
        // and not only what the base brought. `definition` is both -- it was
        // copied from the base above and has been accumulating since.
        type = check_same_name(c, entry, hidden, type);
        if (is_operator_name(name, length)) {
            check_operator_shape(c, entry, type);
        }
        // 14.15改: an override^ that found nothing waits for a composition to
        // bring what it replaces. Until then super^ inside it points at
        // nothing, so 14.11's new has to stay out of reach. Landing on
        // another one that is waiting settles neither.
        bool pending = entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                       (hidden == NULL || hidden->pending);
        set_member_marked(c, definition, name, length, type, false, pending);
        set_member_marked(c, instance, name, length, type, false, pending);
        if (name_is(name, length, "new")) {
            has_new = true;
        }
    }

    c->scope = outer;
    scope_dispose(&members);

    // 14.11: without one written, a definition still offers a new taking no
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
            // inside new it builds one. Either way it names the instance,
            // which is what self^ is bound to.
            for (const LhatNode *field = node->v.list.items; field != NULL;
                 field = field->next) {
                infer(c, field->v.entry.value);
            }
            Binding *receiver = scope_find(c->scope, "self^", 5);
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
            // 13.7: every position of an unbounded tail is the one element
            // type, so a key that did not resolve to one specific position
            // above still has an answer -- unlike an ordinary table's
            // members, which need not share one type to union with nil^.
            if (over != NULL && over->kind == LHAT_TYPE_TABLE &&
                over->v.table.variadic != NULL) {
                return lhat_type_union(c->result->types,
                                       over->v.table.variadic,
                                       simple(c, LHAT_TYPE_NIL));
            }
            // 04 §11.3: a dynamic key may be absent, and absence is not a
            // failure, so nothing narrower than this is safe here.
            return simple(c, LHAT_TYPE_UNKNOWN);
        }

        // 11.6, S27: sound rather than a bare relabelling -- 14.12's
        // disjointness rules out what could never succeed at compile time
        // (a mistake, the same reasoning EQ/NE/... already use just above),
        // and compile_expression checks what disjointness cannot rule out
        // against the actual value at run time, panicking if it does not
        // hold. Either way the expression's own type becomes what was
        // written, since that is what a fitting value goes on to satisfy.
        case LHAT_NODE_AS: {
            LhatType *actual = require_value(
                c, node->v.ascription.value, infer(c, node->v.ascription.value));
            LhatType *wanted = resolve_type(c, node->v.ascription.type);
            if (lhat_type_disjoint(actual, wanted)) {
                report(c, node, LHAT_CHECK_ERR_AS_IMPOSSIBLE);
            }
            return wanted;
        }

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
            if (inner == NULL || inner->kind == LHAT_TYPE_UNKNOWN ||
                inner->kind == LHAT_TYPE_PENDING) {
                // 03 の 3.1・3.5、P6: delegating to a still-pending^ inner
                // expression makes this yieldall^ pending^ too.
                return simple(c, inner != NULL &&
                                          inner->kind == LHAT_TYPE_PENDING
                                      ? LHAT_TYPE_PENDING
                                      : LHAT_TYPE_UNKNOWN);
            }
            if (inner->kind != LHAT_TYPE_CORO) {
                report(c, node, LHAT_CHECK_ERR_NOT_COROUTINE);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 15.8 with 15.3改: delegating runs the inner body -- that is what
            // makes its yield^ reach out here -- so it advances the coroutine
            // as surely as resume() does, and the same two questions apply.
            // Asked here rather than left to 15.1, since no call of start()
            // or resume() is written for the rule to catch.
            if (c->in_function && !inner->v.coroutine.is_function) {
                report(c, node, LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE);
            } else if (c->in_function &&
                       !receiver_is_own_coroutine(c, node->v.jump.value)) {
                report(c, node, LHAT_CHECK_ERR_ADVANCES_OUTSIDE);
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

        // 02 の 14.16: the operand is still checked -- an error inside it is
        // still an error -- but what the operand's own type turns out to be
        // plays no part in typeof^'s own type, which is the uniform TypeInfo
        // carrier regardless. The descriptive payload is filled in at run
        // time by reflect_type reading the actual value, unless 03 の 5.11a's
        // narrow exception applies -- kept here on the node itself for
        // compile_expression to read back.
        case LHAT_NODE_TYPEOF: {
            LhatType *operand = infer(c, node->v.jump.value);
            ((LhatNode *)node)->checked_type = operand;
            return typeinfo_type(c);
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
            const char *module_name = NULL;
            LhatType *exports = c->require.resolve(
                c->require.context, c->lexer->strings + path->v.string.offset,
                path->v.string.length, &module_name);
            if (exports == NULL) {
                report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            // 5.3: running the unit is what registers it, and this is where
            // it runs -- so L^.modules can be said to hold it from here on.
            register_module_type(c, module_name, exports);
            return exports;
        }

        // 05 の 8.7: names a module the host registered, rather than a file.
        case LHAT_NODE_IMPORT: {
            LhatType *module = hosted_module(c, node->v.jump.value);
            if (module == NULL) {
                report(c, node, LHAT_CHECK_ERR_NOT_HOSTED);
                return simple(c, LHAT_TYPE_UNKNOWN);
            }
            return module;
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
                    report_named(c, entry, LHAT_CHECK_ERR_NO_MEMBER, name,
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

// 8.8: 'let^ a.b.c = v' introduces c inside a table reached through a and b.
// The root is the name a scope holds; the rest are members.
static const LhatNode *target_root(const LhatNode *target)
{
    const LhatNode *node = target_name_node(target);
    while (node->kind == LHAT_NODE_MEMBER) {
        node = node->v.access.target;
    }
    return node;
}

static bool target_is_path(const LhatNode *target)
{
    return target_name_node(target)->kind == LHAT_NODE_MEMBER;
}

static const LhatTypeMember *member_named(const LhatType *type,
                                          const char *name, size_t length);

// 05 の 8.6: L^ names the machine's own table. Only the hatted spelling means
// it, so an ordinary name `L` is untouched.
static bool is_environment(const Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node->kind == LHAT_NODE_HAT_IDENT &&
           node_name(c, node, &name, &length) && name_is(name, length, "L^");
}

// What L^ carries. vm.c's build_environment makes the values; the two lists
// have to say the same thing.
static LhatType *environment_type(Checker *c)
{
    if (c->environment != NULL) {
        return c->environment;
    }
    LhatType *env = lhat_type_table(c->result->types);
    LhatType *modules = lhat_type_table(c->result->types);
    // 12.7's shape: a p^ taking nothing and answering nothing. Running the
    // collector is an effect, so it is not an f^.
    LhatType *collect_now = lhat_type_func(c->result->types, false);
    if (env == NULL || modules == NULL || collect_now == NULL) {
        return NULL;
    }
    // 05 の 8.6改: L^ is the machine itself, and the registry inside it is
    // what require^ and import^ write. Both are the machine's to change, not
    // the program's -- M5 asked whether to say so and this is the answer.
    env->v.table.sealed = true;
    modules->v.table.sealed = true;
    // 05 の 5.3: the registry a unit is loaded into once, grown by 8.8.
    lhat_type_add_member(c->result->types, env, "modules", 7, modules);
    lhat_type_add_member(c->result->types, env, "collectgarbage", 14,
                         collect_now);
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
static LhatType *typeinfo_type(Checker *c)
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
                         simple(c, LHAT_TYPE_STRING));
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
        report(c, at, LHAT_CHECK_ERR_PATH_NOT_TABLE);
        return NULL;
    }
    if (type->v.table.from_definition) {
        report(c, at, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
        return NULL;
    }
    // 05 の 8.6改 (M5): the machine's own tables are not grown from here
    // either. Adding a member changes one as much as writing over one does,
    // and 8.8's own walk is what would make the segments on the way.
    if (type->v.table.sealed) {
        report(c, at, LHAT_CHECK_ERR_TABLE_IS_SEALED);
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
            return holds_members(c, node, environment_type(c));
        }
        // The root is a name a scope holds. 8.8 reaches an enclosing binding
        // rather than shadowing it: 'let^ a.b = 1' says where b goes and
        // nothing new about a, so a is only made when there is none.
        if (!node_name(c, node, &name, &length)) {
            return NULL;
        }
        Binding *b = scope_find(c->scope, name, length);
        if (b == NULL) {
            // collect_bindings puts an ordinary root there before the walk,
            // so what reaches here is a hat identifier that is not L^.
            report_named(c, node, LHAT_CHECK_ERR_UNDEFINED, name, length);
            return NULL;
        }
        if (b->type == NULL || b->type->kind == LHAT_TYPE_UNKNOWN ||
            b->type->kind == LHAT_TYPE_PENDING) {
            b->type = lhat_type_table(c->result->types);
        }
        b->reached = true;
        record_resolution(c, node, b);
        return holds_members(c, node, b->type);
    }

    LhatType *owner = path_table(c, node->v.access.target);
    if (owner == NULL || !node_name(c, node->v.access.argument, &name, &length)) {
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
// let^'s syntax instead. The type is not touched either way in that branch,
// so nothing here needs S23's `unconfirmed` marking -- that only matters
// where a member is being added.
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
    if (!node_name(c, last->v.access.argument, &name, &length)) {
        return;
    }
    const LhatTypeMember *found = member_named(owner, name, length);
    if (found != NULL) {
        if (!upsert) {
            report_named(c, last, LHAT_CHECK_ERR_REDEFINED, name, length);
            return;
        }
        expect(c, target, type, found->type, LHAT_CHECK_ERR_MISMATCH);
        return;
    }
    LhatTypeMember *added =
        lhat_type_add_member(c->result->types, owner, name, length, type);
    // S23: nothing here confirms this path actually runs before whatever
    // reads the member later -- inside a deferred body it may never run at
    // all, and inside a branch or loop it may not run this time.
    if (added != NULL && (c->deferred > 0 || c->conditional > 0)) {
        added->unconfirmed = true;
    }
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
            report(c, node, LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE);
        }
    }

    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        // 8.7 with 01 の 8 章: a let^ makes a name in the scope it is
        // written in, so there is no other scope for a specifier to name.
        // ':=' is what reaches an existing binding, here as anywhere.
        if (target_name_node(target)->kind == LHAT_NODE_SCOPE) {
            report(c, target, LHAT_CHECK_ERR_SCOPE_ON_DEFINE);
        }
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
            // 03 の 3.1・3.5: a target past the values a multiple assignment
            // actually gave is a gap in inference, not table subtyping's
            // silence -- there is genuinely nothing here yet, so it is
            // pending^ rather than unknown^.
            actual = value != NULL ? infer(c, value)
                                   : simple(c, LHAT_TYPE_PENDING);
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
        if (target_is_path(target)) {
            // 8.8: the place is a member of a table the path reaches, not a
            // name of this scope. 8.8改: let^'s ':=' spelling asks to
            // reassign rather than fail when the path already answers to
            // something (node->v.binding.via_reassign_op is only ever set
            // by parse_let -- for^/with^ still always define).
            //
            // 15.1改 and 05 の 8.6改: adding a member changes the table as
            // much as writing over one does, so both judgements apply here
            // the same way they do to a reassignment.
            check_write_target(c, target);
            check_opaque_write(c, target);
            define_path(c, target, annotated != NULL ? annotated : actual,
                       node->v.binding.via_reassign_op);
        } else if (node_name(c, target_name_node(target), &name, &length)) {
            Binding *b = scope_find_local(c->scope, name, length);
            if (b != NULL) {
                // Collected before the walk, so this is where its let^ runs.
                b->type = annotated != NULL ? annotated : actual;
                b->reached = true;
                // 15.1改. A destructuring bind takes pieces out of something
                // that was already there (13.10), so nothing it binds is new
                // whatever the source looks like.
                b->fresh = unpacked == NULL && value_is_fresh(c, value, actual);
            }
            // A new name of the same spelling makes any narrowing of the old
            // one stale, since the path now reaches something else.
            drop_narrowings_for(c, target_name_node(target));
        }
        if (unpacked == NULL && value != NULL) {
            value = value->next;
        }
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
static void register_module_type(Checker *c, const char *module_name,
                                 LhatType *exports)
{
    LhatType *env = environment_type(c);
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
            // 05 の 8.6改: a segment of the registry is the machine's, the
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
static LhatType *hosted_module(Checker *c, const LhatNode *path)
{
    LhatType *owner = c->require.hosted;
    if (owner == NULL || path == NULL) {
        return NULL;
    }
    // 'a.b.c' is the MEMBER chain of a path target, so the walk is the same
    // one, from the root outwards.
    if (path->kind == LHAT_NODE_MEMBER) {
        owner = hosted_module(c, path->v.access.target);
        if (owner == NULL) {
            return NULL;
        }
        path = path->v.access.argument;
    }

    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, path, &name, &length)) {
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
    LhatType *module = hosted_module(c, path);
    if (module == NULL) {
        // 5.4改 already binds what a unit declared; saying so here is what
        // keeps 'no such module' from being read as 'the host forgot it'.
        report(c, node, c->require.resolve != NULL
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
        if (!node_name(c, path, &name, &length)) {
            return;
        }
        if (scope_find_local(c->scope, name, length) != NULL) {
            report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name, length);
            return;
        }
        Binding *only = scope_add(c->scope, name, length, module, node->offset);
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
    if (!node_name(c, root_node, &name, &length)) {
        return;
    }
    Binding *root = scope_find(c->scope, name, length);
    if (root == NULL) {
        root = scope_add(c->scope, name, length,
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
    if (!node_name(c, path->v.access.argument, &name, &length)) {
        return;
    }
    if (member_named(owner, name, length) != NULL) {
        report_named(c, node, LHAT_CHECK_ERR_REDEFINED, name, length);
        return;
    }
    lhat_type_add_member(c->result->types, owner, name, length, module);
}

// 05 の 5.4改: a require^ standing alone binds the unit under the path 3 章
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
        report(c, node, LHAT_CHECK_ERR_REQUIRE_FAILED);
        return;
    }
    // 3.2 lets a unit declare no path. Then there is nothing to bind it
    // under, and the reader has to pick a name with let^ instead.
    if (module_name == NULL || *module_name == '\0') {
        report(c, node, LHAT_CHECK_ERR_MODULE_UNNAMED);
        return;
    }
    register_module_type(c, module_name, exports);

    const char *segment = module_name;
    size_t length = strcspn(segment, ".");

    // One segment binds the unit to that name directly; there is no table on
    // the way to make, and 8.7 refuses a name this scope already holds.
    if (segment[length] == '\0') {
        if (scope_find_local(c->scope, segment, length) != NULL) {
            report_named(c, node, LHAT_CHECK_ERR_REDEFINED, segment, length);
            return;
        }
        Binding *only =
            scope_add(c->scope, segment, length, exports, node->offset);
        if (only != NULL) {
            only->reached = true;
        }
        return;
    }

    // The root of a longer path. 8.8 reaches an enclosing binding rather than
    // shadowing it, which is what lets two units of one namespace meet. The
    // name points into the required unit's result, and 6 章 keeps that alive
    // as long as the program is.
    Binding *root = scope_find(c->scope, segment, length);
    if (root == NULL) {
        root = scope_add(c->scope, segment, length,
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
                report_named(c, node, LHAT_CHECK_ERR_REDEFINED, segment, length);
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
static bool value_is_fresh(const Checker *c, const LhatNode *value,
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
            return node_name(c, callee->v.access.argument, &name, &length) &&
                   name_is(name, length, "new");
        }

        default:
            return false;
    }
}

// The binding a path is rooted in, with the scope that answered, or NULL when
// the root is not a name a scope holds -- L^ (05 の 8.6) and self^ among them,
// neither of which any body made.
static Binding *path_root_binding(Checker *c, const LhatNode *target,
                                  Scope **found_in)
{
    const LhatNode *root = target_root(target);
    const char *name = NULL;
    size_t length = 0;
    if (root == NULL || !node_name(c, root, &name, &length)) {
        return NULL;
    }
    Scope *from = c->scope;
    if (root->kind == LHAT_NODE_SCOPE) {
        from = scope_from(c->scope, root);
        if (from == NULL) {
            return NULL;
        }
    }
    return scope_find_in(from, name, length, found_in);
}

// 15.3改: whether this expression names a coroutine the body being checked
// made. Advancing one is only allowed for those -- one that arrived is shared
// with whoever passed it, and the progress would be visible out there.
static bool scope_within_body(Checker *c, const Scope *found_in);

static bool receiver_is_own_coroutine(Checker *c, const LhatNode *receiver)
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
    return root != NULL && root->fresh && scope_within_body(c, found_in);
}

// Whether `found_in` is the body being checked or something inside it.
static bool scope_within_body(Checker *c, const Scope *found_in)
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
static void check_write_target(Checker *c, const LhatNode *target)
{
    if (!c->in_function || c->body_scope == NULL) {
        return;
    }

    if (target_is_path(target)) {
        Scope *found_in = NULL;
        Binding *root = path_root_binding(c, target, &found_in);
        // A root that is no binding at all is nothing this body made: L^ is
        // the machine's own table (05 の 8.6, and its M5), self^ is the
        // instance the caller handed over (14.4).
        if (root == NULL || !root->fresh || !scope_within_body(c, found_in)) {
            report(c, target, LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE);
        }
        return;
    }

    const LhatNode *name_node = target_name_node(target);
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, name_node, &name, &length)) {
        return;
    }

    // 01 の 8 章: a specifier says where to start looking. Reaching outward
    // with one is the same write as reaching outward without one, so it is
    // judged by where the name was found either way.
    Scope *from = c->scope;
    if (name_node->kind == LHAT_NODE_SCOPE) {
        from = scope_from(c->scope, name_node);
        if (from == NULL) {
            return;  // already reported where the name was read
        }
    }

    Scope *found_in = NULL;
    if (scope_find_in(from, name, length, &found_in) == NULL) {
        return;  // no such name; infer_name reports that
    }
    if (!scope_within_body(c, found_in)) {
        report(c, target, LHAT_CHECK_ERR_FUNCTION_WRITES_OUT);
    }
}

// 8.9: a name a let^ bound is not one anything may reassign. Kept apart from
// check_write_target because that one answers 15.1's question -- whose name is
// this to write -- and only inside an f^; this holds wherever the name does,
// in a p^, at the top level of a unit, and in a session.
//
// Only a plain name is asked about. A path is a member of a table (8.8), and
// what may be written there is settled by 15.1改 and 05 の 8.6改 -- a table a
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
    if (!node_name(c, name_node, &name, &length)) {
        return;
    }

    // 01 の 8 章: a specifier says where to start looking, exactly as it does
    // for 15.1's question above.
    Scope *from = c->scope;
    if (name_node->kind == LHAT_NODE_SCOPE) {
        from = scope_from(c->scope, name_node);
        if (from == NULL) {
            return;  // already reported where the name was read
        }
    }

    const Binding *b = scope_find(from, name, length);
    if (b != NULL && b->immutable) {
        // 12.1 and 16.3改2 bind without the writer choosing a word, so there
        // is no var^ for them to write instead. Saying otherwise would send a
        // reader to a spelling that is itself refused.
        report_named(c, name_node,
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
            return environment_type(c);
        }
        if (!node_name(c, node, &name, &length)) {
            return NULL;
        }
        Scope *from = c->scope;
        if (node->kind == LHAT_NODE_SCOPE) {
            from = scope_from(c->scope, node);
            if (from == NULL) {
                return NULL;
            }
        }
        const Binding *b = scope_find(from, name, length);
        return b != NULL ? b->type : NULL;
    }

    const LhatType *owner = path_type_of(c, node->v.access.target);
    if (owner == NULL || !node_name(c, node->v.access.argument, &name, &length)) {
        return NULL;
    }
    const LhatTypeMember *m = member_named(owner, name, length);
    return m != NULL ? m->type : NULL;
}

static void check_opaque_write(Checker *c, const LhatNode *target)
{
    const LhatNode *last = target_name_node(target);
    if (last->kind != LHAT_NODE_MEMBER) {
        return;
    }
    const LhatType *owner = path_type_of(c, last->v.access.target);
    if (owner == NULL || owner->kind != LHAT_TYPE_TABLE) {
        return;
    }
    if (owner->v.table.nominal) {
        report(c, last, LHAT_CHECK_ERR_PATH_IS_OPAQUE);
    }
    // 05 の 8.6改 (M5): the machine's own tables -- L^, its registry, and what
    // require^ or import^ answers with. The host writes these through its own
    // API, which never comes through here, so refusing what is written in L^
    // is the whole rule.
    if (owner->v.table.sealed) {
        report(c, last, LHAT_CHECK_ERR_TABLE_IS_SEALED);
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
        check_immutable_write(c, target);
        check_write_target(c, target);
        check_opaque_write(c, target);
        LhatType *wanted = infer(c, target_name_node(target));
        // 03 の 3.4: what the name holds from here on is not what was passed
        // in, so nothing after this says anything about the parameter.
        close_param_var(c, wanted);
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
    if (over != NULL && over->kind == LHAT_TYPE_PENDING) {
        // 03 の 3.1・3.5、P6: walking a still-pending^ expression makes the
        // element type pending^ too, not merely unknown^.
        return simple(c, LHAT_TYPE_PENDING);
    }
    if (over == NULL || over->kind == LHAT_TYPE_UNKNOWN ||
        over->kind == LHAT_TYPE_ANY) {
        return simple(c, LHAT_TYPE_UNKNOWN);
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
            return simple(c, answer != NULL &&
                                      answer->kind == LHAT_TYPE_PENDING
                                  ? LHAT_TYPE_PENDING
                                  : LHAT_TYPE_UNKNOWN);
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

            // 8.8: a path introduces a member, not a name of this scope. Its
            // root is only made when nothing anywhere holds it -- reaching an
            // enclosing table is the point, so this must not shadow one.
            if (target_is_path(target)) {
                const LhatNode *root = target_root(target);
                // A hat identifier is not a name a scope can hold. L^ is a
                // place of its own (05 の 8.6); anything else is refused when
                // the path is walked.
                if (root->kind != LHAT_NODE_HAT_IDENT &&
                    node_name(c, root, &name, &length) &&
                    scope_find(c->scope, name, length) == NULL) {
                    scope_add(c->scope, name, length,
                              simple(c, LHAT_TYPE_PENDING), root->offset);
                }
                continue;
            }

            if (!node_name(c, target_name_node(target), &name, &length)) {
                continue;
            }
            Binding *already = scope_find_local(c->scope, name, length);
            if (already != NULL) {
                // 03 の 4.3: a name an earlier input of a session bound is
                // written again here, which is the same place written again.
                // Clearing the mark leaves 8.7 in force for a second let^
                // within this input.
                if (!already->from_session) {
                    report_named(c, target_name_node(target),
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
                scope_add(c->scope, name, length, simple(c, LHAT_TYPE_PENDING),
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
    if (!node_name(c, node, &name, &length)) {
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
static char *read_module_name(const Checker *c, const LhatNode *statements)
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
                // 05 の 8.6改: what require^ answers is the machine's record
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

// The block's statements in the scope that is already open. 01 の 8 章: a
// subroutine's body is written with one '{', so it is one scope for '$^' to
// count -- and its parameters are in it, not around it. infer_func has
// already made that scope and put them there, so this must not make another
// or a specifier would find the parameters one step too soon.
static void check_block_in_scope(Checker *c, const LhatNode *node)
{
    check_statements(c, node->v.list.items);
    for (const LhatNode *clause = node->v.list.extra; clause != NULL;
         clause = clause->next) {
        check_statements(c, clause->v.loop_clause.body);
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

    check_block_in_scope(c, node);

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
            LhatType *value = infer(c, node->v.jump.value);
            // 15.8: 15.5 makes such a call run no part of the body, so the
            // statement provably has no effect. 04 の 8.1 の form: the type
            // is the detection, with no must-use machinery added.
            //
            // 03 の 4.3: unless it is the statement the input answers with.
            // There the value is shown, which is an effect -- the reasoning
            // above is what stops applying, not the rule.
            if (value != NULL && value->kind == LHAT_TYPE_CORO &&
                node != c->answering) {
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
                    // S23: still only reached when every earlier condition
                    // failed, so a let^ path written here is exactly as
                    // uncertain as one inside an ordinary clause.
                    c->conditional++;
                    check_statement(c, clause->v.clause.body);
                    c->conditional--;
                    continue;
                }
                expect(c, condition, infer(c, condition),
                       simple(c, LHAT_TYPE_BOOL), LHAT_CHECK_ERR_NOT_BOOL);

                Narrowing *before = c->narrowings;
                narrow_from(c, condition, true);
                c->conditional++;
                check_statement(c, clause->v.clause.body);
                c->conditional--;
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
                // 03 の 7 章、P6: unlike expect()'s other callers, this one
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
                    report(c, node, LHAT_CHECK_ERR_MISMATCH);
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
                report(c, node, LHAT_CHECK_ERR_MISMATCH);
                break;
            }
            // 03 の 3.4: several return^ make a union.
            c->inferred_result =
                lhat_type_union(c->result->types, c->inferred_result, value);
            break;
        }

        // 04 の 11.6改: panic^ answers no value of its own (always_exits
        // already treats it as a way out, same as return^/break^), so the
        // operand is only checked for its own sake -- any type at all is
        // fine, the same way typeof^'s operand asks nothing of its type.
        case LHAT_NODE_PANIC:
            infer(c, node->v.jump.value);
            break;

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
                    check_focus_in(c, node);
                } else {
                    check_statements(c, node->v.loop.focus);
                    infer(c, node->v.loop.bound);
                }
                infer(c, node->v.loop.step);
                check_statements(c, node->v.loop.advance);
                // S23: a loop body may run zero times.
                c->conditional++;
                check_statement(c, node->v.loop.body);
                c->conditional--;
            } else if (node->kind == LHAT_NODE_REPEAT) {
                infer(c, node->v.repeat.bound);
                c->conditional++;
                check_statement(c, node->v.repeat.body);
                c->conditional--;
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

    // 05 の 3 章: read before the statements, so a diagnostic about the path
    // does not wait behind the body.
    result->module_name = read_module_name(&checker, unit->v.list.items);

    check_statements(&checker, unit->v.list.items);

    // 05 の 4 章: gathered once the whole unit has been checked, so a public^
    // name written late is not missed. 8.7 already makes every name visible
    // throughout the scope, so this only reads what is there.
    result->exports = collect_exports(&checker, unit->v.list.items);

    scope_dispose(&scope);
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
    // and then a prompt's import^ finds nothing -- which is what it did
    // before this existed.
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
    if (session->count == session->capacity) {
        size_t grown = session->capacity ? session->capacity * 2 : 16;
        void *bigger = lhat_realloc(session->names, grown * sizeof *session->names);
        if (bigger == NULL) {
            return;
        }
        session->names = bigger;
        session->capacity = grown;
    }
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
        Binding *b = scope_add(&scope, session->names[i].name,
                               session->names[i].length,
                               session->names[i].type, 0);
        if (b != NULL) {
            b->reached = true;
            b->from_session = true;
            b->immutable = session->names[i].immutable;
        }
    }

    check_statements(&checker, unit->v.list.items);
    result->exports = collect_exports(&checker, unit->v.list.items);

    // What this input bound joins the session, replacing what an earlier one
    // bound under the same name. The types are in the session's arena
    // already, so only the names are copied.
    for (Binding *b = scope.bindings; b != NULL; b = b->next) {
        session_keep(session, b->name, b->name_length, b->type, b->immutable);
    }
    session->environment = checker.environment;
    session->typeinfo_type = checker.typeinfo_type;

    scope_dispose(&scope);
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
                Binding *b = scope_add(&scope, m->name, m->name_length,
                                       m->type, 0);
                if (b != NULL) {
                    b->reached = true;
                }
            }
        }

        type = resolve_type(&checker, parsed.root);
        // A name the scope does not hold resolves to UNKNOWN rather than to
        // nothing, and a signature that says nothing is not one.
        if (type != NULL &&
            (type->kind == LHAT_TYPE_UNKNOWN || result.diagnostic_count > 0)) {
            type = NULL;
        }
        lhat_check_result_dispose(&result);
        scope_dispose(&scope);
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
            return "an operator is answered by what stands to its left, and "
                   "this does not answer this one";
        case LHAT_CHECK_ERR_BAD_OPERATOR:
            return "an op^ is an f^ taking self^ and one argument, and it may "
                   "not yield^; the left operand is the self^ and the right "
                   "one the argument";
        case LHAT_CHECK_ERR_ISA_ALWAYS_TRUE:
            return "any^ holds of every value, so this asks nothing";
        case LHAT_CHECK_ERR_MEMBER_EXISTS:
            return "this name is already a member; write override^ or overload^";
        case LHAT_CHECK_ERR_ALREADY_PROVIDED:
            return "something in the chain already provides this member, so "
                   "there is nothing for an abstract^ to ask for";
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
