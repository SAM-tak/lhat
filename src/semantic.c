// L^ (lhat) -- what each name in a checked unit turned out to mean.
//
// See include/lhat/semantic.h for what this does and does not classify. The
// walk below mirrors cli/main.c's print_node in spirit -- one switch over
// every LhatNodeKind -- but instead of printing a tree it decides, per node,
// what an identifier appearing there means: a declaration, a plain reference,
// a call target, a type name, a parameter, a property key or a qualified path
// segment (module^/import^/error kind names).
//
// That is why this cannot use ast.c's lhat_node_visit_children the way
// hover.c and ast_json.c do: the visitor hands over every child alike, and
// what a name means here is exactly the thing its place in the parent says.
// The cost is that a kind added to ast.h has to be named below too, or
// everything under it goes uncoloured -- see the `default` at the end.

#include "lhat/semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"  // 07 の 4 章: what each name resolved to, for refine_by_type
#include "lhat/lexer.h"
#include "lhat/object.h"  // 05 の 8.8/8.9: the tags a host type is named by
#include "parser.h"
#include "program_internal.h"  // LhatUnit's parsed tree and checked result
#include "type.h"

// The walk names these rather than LhatSemanticKind's spellings, so that the
// switch below stays as wide as it was written.
enum {
    SEM_NAMESPACE = LHAT_SEMANTIC_NAMESPACE,
    SEM_TYPE = LHAT_SEMANTIC_TYPE,
    SEM_CLASS = LHAT_SEMANTIC_CLASS,  // 14.1: what a def^ made
    SEM_PARAMETER = LHAT_SEMANTIC_PARAMETER,
    SEM_VARIABLE = LHAT_SEMANTIC_VARIABLE,
    SEM_FUNCTION = LHAT_SEMANTIC_FUNCTION,
    SEM_PROPERTY = LHAT_SEMANTIC_PROPERTY,
};

enum {
    SEM_MOD_DECLARATION = 1u << 0,
    SEM_MOD_READONLY = 1u << 1,
};

typedef struct {
    uint32_t offset;
    uint32_t length;  // bytes
    uint8_t type;
    uint8_t modifiers;
} SemToken;

typedef struct {
    SemToken *tokens;
    size_t count;
    size_t capacity;
    // What the checker decided about each name, for refine_by_type below.
    // Borrowed; the walk does not outlive the unit.
    const LhatUnit *unit;
} SemCollector;

static void collector_init(SemCollector *c, const LhatUnit *unit)
{
    c->tokens = NULL;
    c->count = 0;
    c->capacity = 0;
    c->unit = unit;
}

static void collector_dispose(SemCollector *c)
{
    free(c->tokens);
}

#if LHAT_WITH_RESOLUTIONS
// 05 の 8.6: a table names are reached through rather than one that holds a
// value. The checker says so itself (type.h's is_module) -- the flag beside
// it, `sealed`, answers a different question and would be wrong here: the
// root of an import^ path is a namespace and is not sealed, because the
// import writes into it.
static bool names_a_namespace(const LhatType *type)
{
    return type->kind == LHAT_TYPE_TABLE && type->v.table.is_module;
}

// 05 の 8.8: a host type's name is a value like any other -- it is what
// carries the receiverless members, the way a def^'s name carries new -- and
// unlike a def^ there is one table for the type and for everything of it. So
// what the checker hands back for `godot.Object` and for a name holding one
// is the same type, and the type cannot say which was written.
//
// The spelling can. The member a registration made is the type's own name
// under the type's own module (lhat_register_hostdata_type), and a name
// holding a value of it is spelled something else -- std.io.stdout would hold
// a std.io.File. So a name spelled exactly as its type registered is that
// registration, and anything else is a value of it. What this misreads is a
// name deliberately spelled as its own type ('let^ Object = godot.Object
// .default()'), which reads as the type it is an instance of.
static bool names_its_own_type(const SemCollector *c, uint32_t offset,
                               uint32_t length, const LhatType *type)
{
    const char *registered = NULL;
    if (type->kind == LHAT_TYPE_TABLE && type->v.table.hostdata_tag != NULL) {
        registered = type->v.table.hostdata_tag->name;
    } else if (type->kind == LHAT_TYPE_HOSTVALUE &&
               type->v.table.hostvalue_tag != NULL) {
        registered = type->v.table.hostvalue_tag->name;
    }
    if (registered == NULL || c->unit->source.text == NULL) {
        return false;
    }
    size_t written = strlen(registered);
    return written == length && offset + length <= c->unit->source.length &&
           memcmp(c->unit->source.text + offset, registered, written) == 0;
}

// What the checker settled on, where the syntax could only say "a name".
// `variable` and `property` are the two the walk falls back on when the
// place a name stands does not say what it is; every other classification
// was read off the form it was written in, and stands.
//
// `modifiers` is added to rather than replaced: 8.9's readonly is a fact
// about the binding, not about which of the classifications above it got,
// so it stands beside whatever the name turned out to be.
static void refine_from_resolution(const SemCollector *c, uint32_t offset,
                                   uint32_t length, uint8_t *type,
                                   uint8_t *modifiers)
{
    if (c->unit == NULL) {
        return;
    }
    const LhatResolution *resolved =
        lhat_check_resolution_at(&c->unit->checked, offset);
    if (resolved == NULL) {
        return;
    }

    if (resolved->immutable) {
        *modifiers |= SEM_MOD_READONLY;
    }
    if (*type != SEM_VARIABLE && *type != SEM_PROPERTY) {
        return;  // the form already said what this is
    }

    // 13.1 before the type: a parameter holding a def^ is still a parameter
    // -- what declared the name is the more particular answer about it than
    // what the name happens to hold.
    if (resolved->is_parameter) {
        *type = SEM_PARAMETER;
        return;
    }
    const LhatType *settled = resolved->type;
    if (settled == NULL) {
        return;
    }

    // 05 の 8.8's own name before any of the shape questions: a host type is
    // a table, and asking the shape first would read it as one.
    if (names_its_own_type(c, offset, length, settled)) {
        *type = SEM_TYPE;
        return;
    }
    // A definition first: it is the more particular answer, and 14.1's
    // is_definition and 8.6's sealed never stand together.
    if (settled->kind == LHAT_TYPE_TABLE && settled->v.table.is_definition) {
        *type = SEM_CLASS;
    } else if (names_a_namespace(settled)) {
        *type = SEM_NAMESPACE;
    } else if (settled->kind == LHAT_TYPE_FUNC) {
        *type = SEM_FUNCTION;
    } else if (settled->kind == LHAT_TYPE_ERROR_SET ||
               settled->kind == LHAT_TYPE_ERROR_KIND) {
        // 04 の 2.2: what an errordef^ declared, and one kind within it.
        // Both are written where a type is.
        *type = SEM_TYPE;
    }
}
#endif  // LHAT_WITH_RESOLUTIONS

static void collector_add(SemCollector *c, uint32_t offset, uint32_t length,
                          uint8_t type, uint8_t modifiers)
{
    if (length == 0) {
        return;
    }
#if LHAT_WITH_RESOLUTIONS
    refine_from_resolution(c, offset, length, &type, &modifiers);
#endif
    if (c->count == c->capacity) {
        size_t grown = c->capacity ? c->capacity * 2 : 64;
        SemToken *bigger = (SemToken *)realloc(c->tokens, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        c->tokens = bigger;
        c->capacity = grown;
    }
    c->tokens[c->count].offset = offset;
    c->tokens[c->count].length = length;
    c->tokens[c->count].type = type;
    c->tokens[c->count].modifiers = modifiers;
    c->count++;
}

// 13 章: a name standing where a type is written names a type only if there
// is one. The place says nothing about that -- the parser reads `n : number`
// into exactly the node `n : number^` gives it, and the hat is the whole
// difference -- so a walk that reads the form alone paints a misspelling in
// the colour of the thing it was meant to be. That is how a forgotten hat
// hides: the diagnostic is under the name, and the name looks right.
//
// What resolved is the checker's answer, and check.c records it for a written
// type name (record_type_name) so that this can ask. A unit with nothing
// recorded at all was never checked -- one an editor opened from outside the
// workspace, or a build with LHAT_WITH_RESOLUTIONS off -- and there the form's
// word stands rather than every type name going grey.
static bool resolved_as_a_type(const SemCollector *c, uint32_t offset)
{
#if LHAT_WITH_RESOLUTIONS
    if (c->unit == NULL || c->unit->checked.resolution_count == 0) {
        return true;
    }
    return lhat_check_resolution_at(&c->unit->checked, offset) != NULL;
#else
    (void)c;
    (void)offset;
    return true;
#endif
}

// IDENT / HAT_IDENT / TYPE_NAME are the kinds that carry v.name (ast.h).
// Every other kind puts something else in that union, so the guard is what
// keeps this from reading the wrong field: an errordef^'s name comes from
// simple_node (parser.c), which answers LHAT_NODE_NAME -- v.string, not
// v.name -- when the name was written as a backtick literal.
static void emit_name(SemCollector *out, const LhatNode *node, uint8_t type,
                      uint8_t modifiers)
{
    if (node == NULL || (node->kind != LHAT_NODE_IDENT &&
                         node->kind != LHAT_NODE_HAT_IDENT &&
                         node->kind != LHAT_NODE_TYPE_NAME)) {
        return;
    }
    uint32_t length = node->v.name.length >= node->v.name.hats
                          ? node->v.name.length - node->v.name.hats
                          : node->v.name.length;
    collector_add(out, node->v.name.offset, length, type, modifiers);
}

static void walk_value(SemCollector *out, const LhatNode *node);
static void walk_type(SemCollector *out, const LhatNode *node);

static void walk_list(SemCollector *out, const LhatNode *list)
{
    for (const LhatNode *n = list; n != NULL; n = n->next) {
        walk_value(out, n);
    }
}

// 04 の 4.5: try^{ ... catch^T: ... catch^: ... }. The items are IF_CLAUSE
// nodes -- the first is the body and carries no condition, and each arm
// after it holds **a written type** where an if^'s clause would hold a
// condition (parse_try_block, parser.c). That is why these cannot go
// through walk_value's IF_CLAUSE case, which reads the condition as a
// value: it would walk a type as an expression and name nothing.
static void walk_try_clauses(SemCollector *out, const LhatNode *clauses)
{
    for (const LhatNode *c = clauses; c != NULL; c = c->next) {
        if (c->kind != LHAT_NODE_IF_CLAUSE) {
            continue;
        }
        walk_type(out, c->v.clause.condition);  // NULL on the body and the
                                                // bare arm, which take what
                                                // is left
        walk_value(out, c->v.clause.body);
    }
}

// module^/import^/require^'s qualified path (parse_qualified_name, parser.c):
// a chain of MEMBER nodes over a leading IDENT, e.g. "a.b.c". Every segment
// gets `kind` -- there is no receiver/property distinction here the way
// there is in an ordinary a.b, since the whole path names one thing.
//
// 04 の 14.4's qualified type name (E.Bad) has the same shape with a
// TYPE_NAME at its root instead (parse_type_primary, parser.c), so that is
// a leaf here too and walk_type reaches this for its MEMBER case.
static void walk_qualified_path(SemCollector *out, const LhatNode *node,
                                uint8_t kind)
{
    if (node == NULL) {
        return;
    }
    if (node->kind == LHAT_NODE_IDENT || node->kind == LHAT_NODE_TYPE_NAME) {
        if (kind != SEM_TYPE || resolved_as_a_type(out, node->v.name.offset)) {
            emit_name(out, node, kind, 0);
        }
        return;
    }
    if (node->kind == LHAT_NODE_MEMBER) {
        walk_qualified_path(out, node->v.access.target, kind);
        if (node->v.access.argument != NULL &&
            node->v.access.argument->kind == LHAT_NODE_IDENT &&
            (kind != SEM_TYPE ||
             resolved_as_a_type(out,
                                node->v.access.argument->v.name.offset))) {
            emit_name(out, node->v.access.argument, kind, 0);
        }
    }
}

// 14.14改: a TABLE_ENTRY/MEMBER_DECL key is a property name unless `computed`
// (written '[ expr ]', 14.14改), in which case it is an ordinary expression.
static void walk_table_entries(SemCollector *out, const LhatNode *entries)
{
    for (const LhatNode *e = entries; e != NULL; e = e->next) {
        if (e->kind != LHAT_NODE_TABLE_ENTRY && e->kind != LHAT_NODE_MEMBER_DECL) {
            continue;
        }
        if (e->v.entry.key != NULL) {
            if (e->v.entry.computed) {
                walk_value(out, e->v.entry.key);
            } else if (e->v.entry.key->kind == LHAT_NODE_IDENT) {
                emit_name(out, e->v.entry.key, SEM_PROPERTY, 0);
            }
        }
        // 14.15's 'abstract^ name : type' stands in a def^ or its self^{ … }
        // section, where every other entry is a value -- so the parser builds
        // it as a TABLE_ENTRY like the rest and says which it is with
        // `declared` (ast.h). Walking it as a value walks a written type as
        // an expression, and a qualified one is rooted at a TYPE_NAME, which
        // walk_value has no case for: the root of 'godot.Object' came back
        // with no token at all and the segment after it as a property.
        // 14.6: a template field may write both, in which case each half is
        // walked as what it is.
        walk_type(out, e->v.entry.type);
        if (e->kind == LHAT_NODE_MEMBER_DECL || e->v.entry.declared) {
            walk_type(out, e->v.entry.value);  // t^{ name : type }
        } else {
            walk_value(out, e->v.entry.value);
        }
    }
}

// 04 の 2.2: the fields a kind declares. parse_error_fields (parser.c)
// builds them as PARAM nodes rather than the MEMBER_DECL a t^{ ... } uses,
// since 2.2 lets a field carry a default -- so walk_table_entries would
// pass them over. What they name is read back off the error once 6.1 has
// narrowed to the kind, which makes each a property rather than a
// parameter.
static void walk_error_fields(SemCollector *out, const LhatNode *fields)
{
    for (const LhatNode *f = fields; f != NULL; f = f->next) {
        if (f->kind != LHAT_NODE_PARAM) {
            continue;
        }
        emit_name(out, f->v.param.name, SEM_PROPERTY, SEM_MOD_DECLARATION);
        walk_type(out, f->v.param.type);
        walk_value(out, f->v.param.fallback);
    }
}

// A parameter list (FUNC/TYPE_FUNC's params, PARAM.name declares it).
static void walk_params(SemCollector *out, const LhatNode *params)
{
    for (const LhatNode *p = params; p != NULL; p = p->next) {
        if (p->kind != LHAT_NODE_PARAM) {
            continue;
        }
        if (p->v.param.name != NULL && p->v.param.name->kind == LHAT_NODE_IDENT) {
            emit_name(out, p->v.param.name, SEM_PARAMETER, SEM_MOD_DECLARATION);
        }
        walk_type(out, p->v.param.type);
        walk_value(out, p->v.param.fallback);
    }
}

static void walk_type(SemCollector *out, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
        case LHAT_NODE_TYPE_NAME:
            if (resolved_as_a_type(out, node->v.name.offset)) {
                emit_name(out, node, SEM_TYPE, 0);
            }
            break;
        // 04 の 14.4: an error kind is named through the declaration that
        // introduced it, so a type may be a qualified name -- built as a
        // MEMBER chain over a TYPE_NAME rather than as a type node of its
        // own (parse_type_primary, parser.c).
        case LHAT_NODE_MEMBER:
            walk_qualified_path(out, node, SEM_TYPE);
            break;
        case LHAT_NODE_TYPE_FUNC:
            walk_params(out, node->v.func.params);
            walk_type(out, node->v.func.return_type);
            break;
        case LHAT_NODE_TYPE_TABLE:
            walk_table_entries(out, node->v.list.items);
            break;
        // 14.7改: the self^{ … } section of a written-out definition. Its
        // items are the same MEMBER_DECLs the t^{ … } around it holds -- what
        // an instance carries rather than what the definition does -- so they
        // read the same way. Without this case the section falls to `default`
        // below and everything named inside it goes uncoloured.
        case LHAT_NODE_SELF_TABLE:
            walk_table_entries(out, node->v.list.items);
            break;
        case LHAT_NODE_TYPE_TUPLE:
            // 13.8改: the positions are bare types, not member declarations,
            // so they walk as types rather than through walk_table_entries.
            for (const LhatNode *item = node->v.list.items; item != NULL;
                 item = item->next) {
                walk_type(out, item);
            }
            break;
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            walk_type(out, node->v.binary.left);
            walk_type(out, node->v.binary.right);
            break;
        case LHAT_NODE_TYPE_CORO:
            walk_type(out, node->v.coroutine.receive);
            walk_type(out, node->v.coroutine.produce);
            walk_type(out, node->v.coroutine.result);
            break;
        default:
            break;
    }
}

// DEFINE/REASSIGN's targets. `is_declaration` is true only for DEFINE
// (let^/with^) -- REASSIGN's ':=' writes to a binding that already exists,
// so its targets are references, not declarations (13.10's destructuring,
// and 8.8改's path targets, share this same target-list shape).
//
// 16.3's for^ focus reads through parse_let_target too, so it arrives here
// as well -- with the difference that 17.2's expression form puts an
// ordinary expression where a binding would be. That is what `default`
// answers: a position that binds nothing is still worth colouring.
// 14.1: 'let^ Reader = def^{ … }' declares a type, and the name it declares
// should read as one. The checker records nothing against a declaration --
// it binds a name rather than resolving one -- so refine_by_type has
// nothing to say here, and the answer comes off the tree instead: what the
// target is being given. 13.10 pairs targets with values by position, which
// is what walking the two lists together follows.
// 14.13: 'Base..def^{ … }' declares a class as surely as a bare def^ does,
// and the tree spells the inheritance as the '..' operator with the def^ on
// its right. So the value is read through however many stand there.
//
// Concatenation is written the same way ('a' .. 'b'), which costs nothing:
// what is on the right of one is a string and not a def^, so looking through
// finds nothing and the fallback stands.
static uint8_t declared_as(const LhatNode *value, uint8_t fallback)
{
    while (value != NULL && value->kind == LHAT_NODE_BINARY &&
           value->v.binary.op == LHAT_OP_CONCAT) {
        value = value->v.binary.right;
    }
    return value != NULL && value->kind == LHAT_NODE_DEF ? SEM_CLASS : fallback;
}

// `mod` is what the form already said about these names: whether they are
// being declared here, and -- 8.9 -- whether the word that declared them
// was a let^. A use of the same name gets the second from the checker
// instead (refine_from_resolution), and the two agree because both read
// what 8.9 decided.
static void walk_targets(SemCollector *out, const LhatNode *targets,
                         const LhatNode *values, uint8_t mod)
{
    const LhatNode *value = values;
    for (const LhatNode *t = targets; t != NULL; t = t->next) {
        uint8_t named = declared_as(value, SEM_VARIABLE);
        if (value != NULL) {
            value = value->next;
        }
        switch (t->kind) {
            case LHAT_NODE_IDENT:
                emit_name(out, t, named, mod);
                break;
            case LHAT_NODE_PARAM:
                // A type-annotated target: 'let^ x:number^ = 1'.
                if (t->v.param.name != NULL &&
                    t->v.param.name->kind == LHAT_NODE_IDENT) {
                    emit_name(out, t->v.param.name, named, mod);
                }
                walk_type(out, t->v.param.type);
                walk_value(out, t->v.param.fallback);
                break;
            case LHAT_NODE_MEMBER:
                // A path target ('let^ a.b = v', 8.8): not a fresh
                // declaration -- b is a property of the existing table a.
                walk_value(out, t->v.access.target);
                if (t->v.access.argument != NULL &&
                    t->v.access.argument->kind == LHAT_NODE_IDENT) {
                    emit_name(out, t->v.access.argument, SEM_PROPERTY, 0);
                }
                break;
            case LHAT_NODE_INDEX:
                walk_value(out, t->v.access.target);
                walk_value(out, t->v.access.argument);
                break;
            // 8.2: '$^x := 9' writes an existing outer binding, never a
            // fresh one -- always a reference, declaration or not. Which is
            // what `default` makes of anything else standing here: it binds
            // no name, so it is walked as the expression it is.
            case LHAT_NODE_SCOPE:
            default:
                walk_value(out, t);
                break;
        }
    }
}

// CALL/CALL_STMT's target: an IDENT or the last segment of a MEMBER chain
// names the thing being called, so it reads as 'function' rather than the
// 'variable'/'property' an ordinary reference or member access would get.
static void walk_call_target(SemCollector *out, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    if (node->kind == LHAT_NODE_IDENT) {
        emit_name(out, node, SEM_FUNCTION, 0);
        return;
    }
    if (node->kind == LHAT_NODE_MEMBER) {
        walk_value(out, node->v.access.target);
        if (node->v.access.argument != NULL &&
            node->v.access.argument->kind == LHAT_NODE_IDENT) {
            emit_name(out, node->v.access.argument, SEM_FUNCTION, 0);
        }
        return;
    }
    walk_value(out, node);  // anything else callable (e.g. a[i]()) is a value
}

static void walk_value(SemCollector *out, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
        case LHAT_NODE_IDENT:
            emit_name(out, node, SEM_VARIABLE, 0);
            break;
        case LHAT_NODE_SCOPE:
            walk_value(out, node->v.scope.name);
            break;
        case LHAT_NODE_INTERP:
            walk_list(out, node->v.list.items);
            break;
        case LHAT_NODE_INTERP_HOLE:
            walk_value(out, node->v.hole.value);
            break;
        case LHAT_NODE_TABLE:
        case LHAT_NODE_DEF:
        case LHAT_NODE_SELF_TABLE:
            walk_table_entries(out, node->v.list.items);
            break;
        // 13.8改: a tuple holds plain values, not table entries.
        case LHAT_NODE_TUPLE:
            walk_list(out, node->v.list.items);
            break;
        case LHAT_NODE_ERROR_NEW:
            // 04 の 2.5: 'error^Kind{ ... }' -- Kind is a qualified path.
            walk_qualified_path(out, node->v.named.name, SEM_TYPE);
            walk_table_entries(out, node->v.named.members);
            break;
        case LHAT_NODE_TRY:
        case LHAT_NODE_TYPEOF:
        case LHAT_NODE_SPREAD:
        case LHAT_NODE_REQUIRE_STMT:
        case LHAT_NODE_PACK:
        case LHAT_NODE_BOX:
        case LHAT_NODE_AWAIT:
        // 9.8 and 9.11: what these carry is the level, except where the
        // brackets held 9.8's label form instead -- an expression, and the
        // only thing here worth a token.
        case LHAT_NODE_BREAK:
        case LHAT_NODE_NEXT:
        case LHAT_NODE_PANIC:
        case LHAT_NODE_CALL_STMT:
            walk_value(out, node->v.jump.value);
            break;
        // 13.8改: return^ may carry several values, and one is a list of one.
        case LHAT_NODE_RETURN:
        case LHAT_NODE_YIELD:
            for (const LhatNode *value = node->v.jump.value; value != NULL;
                 value = value->next) {
                walk_value(out, value);
            }
            break;
        case LHAT_NODE_IMPORT:
        case LHAT_NODE_IMPORT_STMT:
            walk_qualified_path(out, node->v.jump.value, SEM_NAMESPACE);
            break;
        case LHAT_NODE_UNARY:
            walk_value(out, node->v.unary.operand);
            break;
        case LHAT_NODE_BINARY:
            walk_value(out, node->v.binary.left);
            // 13.11: isa^ asks whether the left may stand where the right is
            // written, so what stands there is a type -- parse_binary reads
            // it with parse_type (parser.c). 17.4's 'when^ isa^ T:' lowers to
            // this same node, so naming it once here covers both.
            if (node->v.binary.op == LHAT_OP_ISA) {
                walk_type(out, node->v.binary.right);
            } else {
                walk_value(out, node->v.binary.right);
            }
            break;
        // 11.5 の (5) with 13.11: a chain may hold an isa^ among the
        // comparisons ('a < b isa^ number^ < c'), and the type it asks about
        // does not stand where a value would. So the operands are paired
        // with the operators rather than walked alike -- the same pairing
        // chk_infer does (check_expr.c), and for the same reason: a type is
        // not what the next link compares against, so it does not take that
        // place.
        case LHAT_NODE_COMPARE_CHAIN: {
            const LhatNode *operand = node->v.chain.operands;
            walk_value(out, operand);
            for (const LhatNode *marker = node->v.chain.operators;
                 marker != NULL && operand != NULL; marker = marker->next) {
                operand = operand->next;
                if (operand == NULL) {
                    break;
                }
                if (marker->v.unary.op == LHAT_OP_ISA) {
                    walk_type(out, operand);
                } else {
                    walk_value(out, operand);
                }
            }
            break;
        }
        case LHAT_NODE_MEMBER:
            walk_value(out, node->v.access.target);
            if (node->v.access.argument != NULL &&
                node->v.access.argument->kind == LHAT_NODE_IDENT) {
                emit_name(out, node->v.access.argument, SEM_PROPERTY, 0);
            }
            break;
        case LHAT_NODE_INDEX:
            walk_value(out, node->v.access.target);
            walk_value(out, node->v.access.argument);
            break;
        case LHAT_NODE_CALL:
            walk_call_target(out, node->v.access.target);
            walk_list(out, node->v.access.argument);
            break;
        case LHAT_NODE_AS:
            walk_value(out, node->v.ascription.value);
            walk_type(out, node->v.ascription.type);
            break;
        case LHAT_NODE_FUNC:
            walk_params(out, node->v.func.params);
            walk_type(out, node->v.func.return_type);
            walk_value(out, node->v.func.body);
            break;
        case LHAT_NODE_IF_EXPR:
        case LHAT_NODE_IF_STMT:
            walk_list(out, node->v.list.items);
            break;
        case LHAT_NODE_TRY_BLOCK:
            walk_try_clauses(out, node->v.list.items);
            break;
        case LHAT_NODE_IF_CLAUSE:
            walk_value(out, node->v.clause.condition);
            walk_value(out, node->v.clause.body);
            break;
        case LHAT_NODE_DEFINE:
            walk_targets(out, node->v.binding.targets, node->v.binding.values,
                         SEM_MOD_DECLARATION |
                             (node->v.binding.immutable ? SEM_MOD_READONLY : 0));
            walk_list(out, node->v.binding.values);
            break;
        case LHAT_NODE_REASSIGN:
            // ':=' writes a name that already exists, so its targets declare
            // nothing -- and a name it may be written to is not a readonly
            // one, which 8.9 is what refuses.
            walk_targets(out, node->v.binding.targets, node->v.binding.values,
                         0);
            walk_list(out, node->v.binding.values);
            break;
        case LHAT_NODE_BLOCK:
            walk_list(out, node->v.list.items);
            walk_list(out, node->v.list.extra);  // 9 章's clauses
            break;
        case LHAT_NODE_LOOP_CLAUSE:
            walk_list(out, node->v.loop_clause.body);
            break;
        case LHAT_NODE_WITH:
            walk_list(out, node->v.list.items);  // DEFINE bindings
            walk_value(out, node->v.list.extra);  // body block
            break;
        case LHAT_NODE_FOR:
            // 16.3: parse_for_focus reads the focus through parse_let_target
            // (parser.c), so it takes every shape a let^ target does -- a
            // bare name, 'i:number^', a '.member' path, an index. The same
            // walk a define's targets get, then. A focus is given its value
            // by the clause after it rather than by a list beside it, so
            // there is nothing to pair here; and where the focus is itself a
            // DEFINE (the let^/var^ forms), that node says whether 8.9 made
            // it readonly when the walk reaches it.
            walk_targets(out, node->v.loop.focus, NULL, SEM_MOD_DECLARATION);
            walk_value(out, node->v.loop.bound);
            walk_value(out, node->v.loop.step);
            walk_list(out, node->v.loop.advance);
            walk_value(out, node->v.loop.body);
            break;
        case LHAT_NODE_REPEAT:
            walk_value(out, node->v.repeat.bound);
            walk_value(out, node->v.repeat.body);
            break;
        case LHAT_NODE_ERRORDEF:
            emit_name(out, node->v.named.name, SEM_TYPE, SEM_MOD_DECLARATION);
            walk_list(out, node->v.named.members);  // ERROR_KIND list
            break;
        case LHAT_NODE_ERROR_KIND:
            emit_name(out, node->v.named.name, SEM_TYPE, SEM_MOD_DECLARATION);
            walk_error_fields(out, node->v.named.members);
            break;
        case LHAT_NODE_MODULE:
            walk_qualified_path(out, node->v.named.name, SEM_NAMESPACE);
            break;
        default:
            // INT/FLOAT/STRING/NAME/HAT_IDENT/FOCUS/INTERP_TEXT, every
            // type-only node (reached through walk_type, not here), a bare
            // PARAM/MEMBER_DECL (reached through walk_params/
            // walk_table_entries, not here), REQUIRE (its jump.value is a
            // STRING, nothing to name), ERROR: nothing to emit.
            //
            // 02 の 18's annotation hangs off the declaration it was written
            // over rather than standing among the statements, so it is not
            // reached from here at all -- and is left that way on purpose.
            // 18.2's name is the host's registration and 18.3 carries a name
            // argument by its spelling without ever resolving it, so the
            // checker settled nothing about either. The grammar file colours
            // them, which is where a spelling with no meaning behind it
            // belongs; test_semantic_tokens' first test says so out loud.
            //
            // A node kind this switch has never heard of lands here too, and
            // then everything under it goes uncoloured -- which is what a
            // new kind added to ast.h costs until it is named above.
            // test_semantic_tokens' first test is what says so out loud:
            // it walks the tree through ast.c's own visitor, which does not
            // have to be taught each kind separately, and asks that every
            // name it finds came back with a token.
            break;
    }
}

static int compare_tokens(const void *a, const void *b)
{
    const SemToken *ta = (const SemToken *)a;
    const SemToken *tb = (const SemToken *)b;
    if (ta->offset != tb->offset) {
        return ta->offset < tb->offset ? -1 : 1;
    }
    return 0;
}

size_t lhat_unit_semantic_names(const LhatUnit *unit, LhatSemanticName *into,
                                size_t capacity)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return 0;
    }

    SemCollector collector;
    collector_init(&collector, unit);
    walk_value(&collector, unit->parsed.root);

    if (collector.count > 1) {
        qsort(collector.tokens, collector.count, sizeof *collector.tokens,
             compare_tokens);
    }

    size_t answered = 0;
    bool have_previous = false;
    uint32_t previous_offset = 0;

    for (size_t i = 0; i < collector.count; i++) {
        const SemToken *token = &collector.tokens[i];

        // One name written once answers once, even where the tree holds it
        // twice: 7.4改 expands 'a[i] += 1' into the reassignment and the
        // 'a[i] + 1' it stands for, both carrying the same spans, while
        // saying the target is read exactly once. Two names can never start
        // at one offset, so a repeat here is always that -- the tree
        // spelling something out, not the source saying it again.
        if (have_previous && token->offset == previous_offset) {
            continue;
        }
        have_previous = true;
        previous_offset = token->offset;

        if (into != NULL && answered < capacity) {
            into[answered].kind = (LhatSemanticKind)token->type;
            into[answered].offset = token->offset;
            into[answered].length = token->length;
            into[answered].declaration =
                (token->modifiers & SEM_MOD_DECLARATION) != 0;
            into[answered].readonly =
                (token->modifiers & SEM_MOD_READONLY) != 0;
        }
        answered++;
    }

    collector_dispose(&collector);
    return answered;
}
