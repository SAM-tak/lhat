// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.
//
// See semantic_tokens.h for what this does and does not classify. The walk
// below mirrors cli/main.c's print_node in spirit -- one switch over every
// LhatNodeKind -- but instead of printing a tree it decides, per node, what
// an identifier appearing there means: a declaration, a plain reference, a
// call target, a type name, a parameter, a property key or a qualified
// path segment (module^/import^/error kind names).

#include "semantic_tokens.h"

#include <stdint.h>
#include <stdlib.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include "position.h"

const char *const LSP_SEMANTIC_TOKEN_TYPES[] = {
    "namespace", "type", "parameter", "variable", "function", "property",
};
const size_t LSP_SEMANTIC_TOKEN_TYPES_COUNT =
    sizeof LSP_SEMANTIC_TOKEN_TYPES / sizeof LSP_SEMANTIC_TOKEN_TYPES[0];

const char *const LSP_SEMANTIC_TOKEN_MODIFIERS[] = {
    "declaration",
};
const size_t LSP_SEMANTIC_TOKEN_MODIFIERS_COUNT =
    sizeof LSP_SEMANTIC_TOKEN_MODIFIERS / sizeof LSP_SEMANTIC_TOKEN_MODIFIERS[0];

enum {
    SEM_NAMESPACE = 0,
    SEM_TYPE = 1,
    SEM_PARAMETER = 2,
    SEM_VARIABLE = 3,
    SEM_FUNCTION = 4,
    SEM_PROPERTY = 5,
};

enum {
    SEM_MOD_DECLARATION = 1u << 0,
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
} SemCollector;

static void collector_init(SemCollector *c)
{
    c->tokens = NULL;
    c->count = 0;
    c->capacity = 0;
}

static void collector_dispose(SemCollector *c)
{
    free(c->tokens);
}

static void collector_add(SemCollector *c, uint32_t offset, uint32_t length,
                          uint8_t type, uint8_t modifiers)
{
    if (length == 0) {
        return;
    }
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

// IDENT / HAT_IDENT / TYPE_NAME share v.name (ast.h); this only ever runs on
// IDENT or TYPE_NAME (callers check the kind first -- HAT_IDENT is left to
// TextMate, and other kinds use a different union member entirely, so
// reading v.name off one would be reading the wrong field).
static void emit_name(SemCollector *out, const LhatNode *node, uint8_t type,
                      uint8_t modifiers)
{
    if (node == NULL) {
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

// module^/import^/require^'s qualified path (parse_qualified_name, parser.c):
// a chain of MEMBER nodes over a leading IDENT, e.g. "a.b.c". Every segment
// gets `kind` -- there is no receiver/property distinction here the way
// there is in an ordinary a.b, since the whole path names one thing.
static void walk_qualified_path(SemCollector *out, const LhatNode *node,
                                uint8_t kind)
{
    if (node == NULL) {
        return;
    }
    if (node->kind == LHAT_NODE_IDENT) {
        emit_name(out, node, kind, 0);
        return;
    }
    if (node->kind == LHAT_NODE_MEMBER) {
        walk_qualified_path(out, node->v.access.target, kind);
        if (node->v.access.argument != NULL &&
            node->v.access.argument->kind == LHAT_NODE_IDENT) {
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
        if (e->kind == LHAT_NODE_MEMBER_DECL) {
            walk_type(out, e->v.entry.value);  // t^{ name : type }
        } else {
            walk_value(out, e->v.entry.value);
        }
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
            emit_name(out, node, SEM_TYPE, 0);
            break;
        case LHAT_NODE_TYPE_FUNC:
            walk_params(out, node->v.func.params);
            walk_type(out, node->v.func.return_type);
            break;
        case LHAT_NODE_TYPE_TABLE:
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
static void walk_targets(SemCollector *out, const LhatNode *targets,
                         bool is_declaration)
{
    uint8_t mod = is_declaration ? SEM_MOD_DECLARATION : 0;
    for (const LhatNode *t = targets; t != NULL; t = t->next) {
        switch (t->kind) {
            case LHAT_NODE_IDENT:
                emit_name(out, t, SEM_VARIABLE, mod);
                break;
            case LHAT_NODE_PARAM:
                // A type-annotated target: 'let^ x:number^ = 1'.
                if (t->v.param.name != NULL &&
                    t->v.param.name->kind == LHAT_NODE_IDENT) {
                    emit_name(out, t->v.param.name, SEM_VARIABLE, mod);
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
            case LHAT_NODE_SCOPE:
                // 8.2: '$^x := 9' writes an existing outer binding, never a
                // fresh one -- always a reference, declaration or not.
                walk_value(out, t);
                break;
            default:
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
        case LHAT_NODE_YIELD_ALL:
        case LHAT_NODE_BREAK:
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
            walk_value(out, node->v.binary.right);
            break;
        case LHAT_NODE_COMPARE_CHAIN:
            walk_list(out, node->v.chain.operands);
            break;
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
        case LHAT_NODE_IF_CLAUSE:
            walk_value(out, node->v.clause.condition);
            walk_value(out, node->v.clause.body);
            break;
        case LHAT_NODE_DEFINE:
            walk_targets(out, node->v.binding.targets, true);
            walk_list(out, node->v.binding.values);
            break;
        case LHAT_NODE_REASSIGN:
            walk_targets(out, node->v.binding.targets, false);
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
            for (const LhatNode *f = node->v.loop.focus; f != NULL; f = f->next) {
                if (f->kind == LHAT_NODE_IDENT) {
                    emit_name(out, f, SEM_VARIABLE, SEM_MOD_DECLARATION);
                } else {
                    walk_value(out, f);
                }
            }
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
            walk_table_entries(out, node->v.named.members);
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

cJSON *lsp_semantic_tokens_for_unit(const LhatUnit *unit)
{
    SemCollector collector;
    collector_init(&collector);
    walk_value(&collector, unit->parsed.root);

    if (collector.count > 1) {
        qsort(collector.tokens, collector.count, sizeof *collector.tokens,
             compare_tokens);
    }

    cJSON *data = cJSON_CreateArray();
    if (data == NULL) {
        collector_dispose(&collector);
        return NULL;
    }

    const char *text = unit->source.text;
    size_t text_length = unit->source.length;
    int prev_line = 0;
    int prev_char = 0;

    for (size_t i = 0; i < collector.count; i++) {
        const SemToken *token = &collector.tokens[i];
        LspPosition start = lsp_position_at(text, text_length, token->offset);
        LspPosition end =
            lsp_position_at(text, text_length, token->offset + token->length);
        if (end.line != start.line) {
            continue;  // an identifier never spans a line; be safe anyway
        }

        int delta_line = start.line - prev_line;
        int delta_char =
            delta_line == 0 ? start.character - prev_char : start.character;

        cJSON_AddItemToArray(data, cJSON_CreateNumber(delta_line));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(delta_char));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(end.character - start.character));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(token->type));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(token->modifiers));

        prev_line = start.line;
        prev_char = start.character;
    }

    collector_dispose(&collector);
    return data;
}
