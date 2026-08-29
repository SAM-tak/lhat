// L^ (lhat) -- LSP server: textDocument/documentSymbol.

#include "document_symbol.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"

#include "position.h"
#include "util.h"

// LSP's SymbolKind, by the numbers the spec assigns. Only the ones used.
enum {
    SYMBOL_MODULE = 2,
    SYMBOL_CLASS = 5,
    SYMBOL_METHOD = 6,
    SYMBOL_PROPERTY = 7,
    SYMBOL_FIELD = 8,
    SYMBOL_ENUM = 10,
    SYMBOL_FUNCTION = 12,
    SYMBOL_VARIABLE = 13,
    SYMBOL_CONSTANT = 14,
    SYMBOL_OBJECT = 19,
    SYMBOL_ENUM_MEMBER = 22
};

typedef struct {
    const LhatUnit *unit;
    bool failed;  // an allocation failed; the walk stops adding
} Outline;

static bool name_of(const Outline *o, const LhatNode *node, const char **text,
                    size_t *length)
{
    return node != NULL && lhat_node_name(node, o->unit->source.text,
                                          o->unit->lexer.strings, text, length);
}

// The source the node covers, as written -- a qualified path, a signature.
static void text_of(const Outline *o, const LhatNode *node, const char **text,
                    size_t *length)
{
    uint32_t start = lhat_node_span_start(node);
    uint32_t end = node->end > start ? node->end : start;
    if (end > o->unit->source.length) {
        end = (uint32_t)o->unit->source.length;
    }
    *text = o->unit->source.text + start;
    *length = end > start ? end - start : 0;
}

static void add_position(cJSON *into, const char *field, const Outline *o,
                         uint32_t offset)
{
    LspPosition at = lsp_unit_position_at(o->unit, offset);
    cJSON *position = cJSON_CreateObject();
    cJSON_AddItemToObject(into, field, position);
    cJSON_AddNumberToObject(position, "line", at.line);
    cJSON_AddNumberToObject(position, "character", at.character);
}

static void add_range(cJSON *into, const char *field, const Outline *o,
                      const LhatNode *node)
{
    cJSON *range = cJSON_CreateObject();
    cJSON_AddItemToObject(into, field, range);
    add_position(range, "start", o, lhat_node_span_start(node));
    add_position(range, "end", o, node->end);
}

// One DocumentSymbol appended to `into`. `whole` is what the symbol spans,
// `name_node` where its name stands -- the two ranges LSP asks for. Answers
// the symbol so children can hang off it; NULL when nothing could be made.
static cJSON *symbol(Outline *o, cJSON *into, const char *name,
                     size_t name_length, int kind, const LhatNode *whole,
                     const LhatNode *name_node, const char *detail,
                     size_t detail_length)
{
    if (o->failed) {
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        o->failed = true;
        return NULL;
    }
    cJSON_AddItemToArray(into, out);
    // A name and a detail are slices of the source; cJSON copies from a
    // NUL-terminated string only.
    char *name_copy = lsp_strndup(name, name_length);
    char *detail_copy = detail_length > 0 ? lsp_strndup(detail, detail_length)
                                          : NULL;
    if (name_copy == NULL || (detail_length > 0 && detail_copy == NULL)) {
        o->failed = true;
    } else {
        cJSON_AddStringToObject(out, "name", name_copy);
        if (detail_copy != NULL) {
            cJSON_AddStringToObject(out, "detail", detail_copy);
        }
    }
    free(name_copy);
    free(detail_copy);
    if (o->failed) {
        return NULL;
    }
    cJSON_AddNumberToObject(out, "kind", kind);
    add_range(out, "range", o, whole);
    // selectionRange has to sit inside range; a name always does, since the
    // form that declares it encloses it.
    add_range(out, "selectionRange", o, name_node);
    return out;
}

// The "children" array of `parent`, made on first use so a leaf carries none.
static cJSON *children_of(cJSON *parent)
{
    cJSON *children = cJSON_GetObjectItemCaseSensitive(parent, "children");
    if (children == NULL) {
        children = cJSON_CreateArray();
        cJSON_AddItemToObject(parent, "children", children);
    }
    return children;
}

// 14.13: 'Base..def^{ … }' is the '..' operator with the def^ on its right,
// so a value is read through however many stand there (semantic.c's
// declared_as makes the same walk). `base` is what stood on the left of the
// last one -- what the class was made from -- or NULL.
static const LhatNode *through_concat(const LhatNode *value,
                                      const LhatNode **base)
{
    *base = NULL;
    while (value != NULL && value->kind == LHAT_NODE_BINARY &&
           value->v.binary.op == LHAT_OP_CONCAT) {
        *base = value->v.binary.left;
        value = value->v.binary.right;
    }
    return value;
}

// What a function was written with, up to its body: 'f^p:Vector3 -> Vector3'.
// Cut from the source rather than rebuilt, so it reads as the writer spelt it.
static void signature_of(const Outline *o, const LhatNode *func,
                         const char **text, size_t *length)
{
    uint32_t start = func->offset;
    uint32_t end = func->v.func.body != NULL
                       ? lhat_node_span_start(func->v.func.body)
                       : func->end;
    if (end < start) {
        end = start;
    }
    if (end > o->unit->source.length) {
        end = (uint32_t)o->unit->source.length;
    }
    // A body that starts at its first statement rather than at its brace
    // leaves the brace on this side; neither it nor the space before it is
    // part of the signature.
    while (end > start && (o->unit->source.text[end - 1] == ' ' ||
                           o->unit->source.text[end - 1] == '\t' ||
                           o->unit->source.text[end - 1] == '{')) {
        end--;
    }
    *text = o->unit->source.text + start;
    *length = end - start;
}

static void walk_statements(Outline *o, const LhatNode *list, cJSON *into,
                            bool at_root);
static void walk_entries(Outline *o, const LhatNode *entries, cJSON *into,
                         int plain_kind, bool in_class);

// A name bound to a value -- a let^ target, a table entry, a template field.
// What the value is decides what the name is listed as, and what hangs
// under it.
static void add_binding(Outline *o, cJSON *into, const LhatNode *whole,
                        const LhatNode *name_node, const char *name,
                        size_t name_length, const LhatNode *value,
                        int plain_kind, bool in_class)
{
    const LhatNode *base;
    const LhatNode *held = through_concat(value, &base);
    const char *detail = NULL;
    size_t detail_length = 0;
    int kind = plain_kind;

    if (held == NULL) {
        // A target past the values: a name, and nothing to read it off.
    } else if (held->kind == LHAT_NODE_FUNC) {
        kind = in_class ? SYMBOL_METHOD : SYMBOL_FUNCTION;
        signature_of(o, held, &detail, &detail_length);
    } else if (held->kind == LHAT_NODE_DEF) {
        kind = SYMBOL_CLASS;
        if (base != NULL) {
            text_of(o, base, &detail, &detail_length);
        }
    } else if (held->kind == LHAT_NODE_TABLE) {
        kind = SYMBOL_OBJECT;
    } else if (held->kind == LHAT_NODE_IMPORT || held->kind == LHAT_NODE_REQUIRE) {
        kind = SYMBOL_MODULE;
        text_of(o, held, &detail, &detail_length);
    }

    cJSON *out = symbol(o, into, name, name_length, kind, whole, name_node,
                        detail, detail_length);
    if (out == NULL || held == NULL) {
        return;
    }
    switch (held->kind) {
        case LHAT_NODE_FUNC:
            // Its locals: the let^s of the body, not of the callbacks the
            // body passes along.
            if (held->v.func.body != NULL &&
                held->v.func.body->kind == LHAT_NODE_BLOCK) {
                walk_statements(o, held->v.func.body->v.list.items,
                                children_of(out), false);
            }
            break;
        case LHAT_NODE_DEF:
            walk_entries(o, held->v.list.items, children_of(out),
                         SYMBOL_PROPERTY, true);
            break;
        case LHAT_NODE_TABLE:
            walk_entries(o, held->v.list.items, children_of(out),
                         SYMBOL_PROPERTY, false);
            break;
        default:
            break;
    }
}

static void walk_entries(Outline *o, const LhatNode *entries, cJSON *into,
                         int plain_kind, bool in_class)
{
    for (const LhatNode *e = entries; e != NULL && !o->failed; e = e->next) {
        if (e->kind != LHAT_NODE_TABLE_ENTRY) {
            continue;
        }
        // 14.7改2: a delegate^ entry is not a member, and names nothing new.
        if (e->v.entry.modifier == LHAT_DEF_DELEGATE) {
            continue;
        }
        if (e->v.entry.key == NULL) {
            // 14.6: the self^{ … } template of a def^ carries no key, and
            // what it holds are the instance's fields. A positional entry
            // has no key either, and no name to list.
            if (in_class && e->v.entry.value != NULL &&
                e->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
                walk_entries(o, e->v.entry.value->v.list.items, into,
                             SYMBOL_FIELD, true);
            }
            continue;
        }
        // 14.14改: '[expr] = v' is keyed by a value nobody can read off the
        // tree.
        if (e->v.entry.computed) {
            continue;
        }
        const char *name;
        size_t name_length;
        if (!name_of(o, e->v.entry.key, &name, &name_length)) {
            continue;
        }
        // 14.15: 'abstract^ name : type' -- declared, and given nothing.
        // What stands in `value` is the type, so it says what kind of member
        // this will be without being one to walk into.
        if (e->v.entry.declared) {
            const LhatNode *type = e->v.entry.value;
            int kind = type != NULL && type->kind == LHAT_NODE_TYPE_FUNC
                           ? (in_class ? SYMBOL_METHOD : SYMBOL_FUNCTION)
                           : plain_kind;
            const char *detail = NULL;
            size_t detail_length = 0;
            if (type != NULL) {
                text_of(o, type, &detail, &detail_length);
            }
            symbol(o, into, name, name_length, kind, e, e->v.entry.key, detail,
                   detail_length);
            continue;
        }
        add_binding(o, into, e, e->v.entry.key, name, name_length,
                    e->v.entry.value, plain_kind, in_class);
    }
}

// 04 の 2.2: the fields a kind declares, built as PARAM nodes (parser.c).
static void walk_error_fields(Outline *o, const LhatNode *fields, cJSON *into)
{
    for (const LhatNode *f = fields; f != NULL && !o->failed; f = f->next) {
        const char *name;
        size_t name_length;
        if (f->kind != LHAT_NODE_PARAM ||
            !name_of(o, f->v.param.name, &name, &name_length)) {
            continue;
        }
        const char *detail = NULL;
        size_t detail_length = 0;
        if (f->v.param.type != NULL) {
            text_of(o, f->v.param.type, &detail, &detail_length);
        }
        symbol(o, into, name, name_length, SYMBOL_FIELD, f, f->v.param.name,
               detail, detail_length);
    }
}

// The table a unit answers with, when it does: 'return^ { … }' -- or the
// same inside the wrapper an LTON file is read through (lsp/lton.h, 08 章):
// 'return^ (f^ -> t^{} { return^ { … } })()'. One level of that and no more.
static const LhatNode *returned_table(const LhatNode *value, bool unwrap)
{
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == LHAT_NODE_TABLE) {
        return value;
    }
    if (!unwrap || value->kind != LHAT_NODE_CALL ||
        value->v.access.argument != NULL) {
        return NULL;
    }
    const LhatNode *func = value->v.access.target;
    if (func == NULL || func->kind != LHAT_NODE_FUNC ||
        func->v.func.params != NULL || func->v.func.body == NULL ||
        func->v.func.body->kind != LHAT_NODE_BLOCK) {
        return NULL;
    }
    for (const LhatNode *s = func->v.func.body->v.list.items; s != NULL;
         s = s->next) {
        if (s->kind == LHAT_NODE_RETURN) {
            return returned_table(s->v.jump.value, false);
        }
    }
    return NULL;
}

static void walk_clauses(Outline *o, const LhatNode *clauses, cJSON *into)
{
    for (const LhatNode *c = clauses; c != NULL && !o->failed; c = c->next) {
        if (c->kind == LHAT_NODE_IF_CLAUSE) {
            walk_statements(o, c->v.clause.body, into, false);
        } else if (c->kind == LHAT_NODE_LOOP_CLAUSE) {
            walk_statements(o, c->v.loop_clause.body, into, false);
        }
    }
}

static void walk_statement(Outline *o, const LhatNode *node, cJSON *into,
                           bool at_root)
{
    switch (node->kind) {
        case LHAT_NODE_DEFINE: {
            // 13.10 pairs targets with values by position, the way
            // semantic.c's walk_targets does; a target past the values is a
            // name with nothing to read a kind off.
            const LhatNode *value = node->v.binding.values;
            int plain_kind = node->v.binding.immutable ? SYMBOL_CONSTANT
                                                       : SYMBOL_VARIABLE;
            for (const LhatNode *t = node->v.binding.targets; t != NULL;
                 t = t->next) {
                const LhatNode *name_node = lhat_define_target_name(t);
                const char *name = NULL;
                size_t name_length = 0;
                int kind = plain_kind;
                if (name_node != NULL && name_node->kind == LHAT_NODE_MEMBER) {
                    // 8.8: 'let^ a.b = v' adds b to the table a holds. Not
                    // a name of this scope, so it is listed as the member
                    // it is, under the path that reaches it.
                    text_of(o, name_node, &name, &name_length);
                    kind = SYMBOL_PROPERTY;
                } else if (!name_of(o, name_node, &name, &name_length)) {
                    name_node = NULL;
                }
                if (name_node != NULL) {
                    add_binding(o, into, node, name_node, name, name_length,
                                value, kind, false);
                }
                if (value != NULL) {
                    value = value->next;
                }
            }
            break;
        }
        case LHAT_NODE_ERRORDEF: {
            const char *name;
            size_t name_length;
            if (!name_of(o, node->v.named.name, &name, &name_length)) {
                break;
            }
            cJSON *out = symbol(o, into, name, name_length, SYMBOL_ENUM, node,
                                node->v.named.name, NULL, 0);
            if (out == NULL) {
                break;
            }
            for (const LhatNode *k = node->v.named.members;
                 k != NULL && !o->failed; k = k->next) {
                const char *kind_name;
                size_t kind_length;
                if (k->kind != LHAT_NODE_ERROR_KIND ||
                    !name_of(o, k->v.named.name, &kind_name, &kind_length)) {
                    continue;
                }
                cJSON *member =
                    symbol(o, children_of(out), kind_name, kind_length,
                           SYMBOL_ENUM_MEMBER, k, k->v.named.name, NULL, 0);
                if (member != NULL && k->v.named.members != NULL) {
                    walk_error_fields(o, k->v.named.members,
                                      children_of(member));
                }
            }
            break;
        }
        case LHAT_NODE_MODULE: {
            const char *name;
            size_t name_length;
            if (node->v.named.name == NULL) {
                break;
            }
            text_of(o, node->v.named.name, &name, &name_length);
            symbol(o, into, name, name_length, SYMBOL_MODULE, node,
                   node->v.named.name, NULL, 0);
            break;
        }
        case LHAT_NODE_RETURN: {
            const LhatNode *table = at_root ? returned_table(node->v.jump.value,
                                                             true)
                                            : NULL;
            if (table != NULL) {
                walk_entries(o, table->v.list.items, into, SYMBOL_PROPERTY,
                             false);
            }
            break;
        }
        // The forms that hold statements without naming anything themselves
        // are looked through, so a let^ inside an if^ is still listed.
        case LHAT_NODE_BLOCK:
            walk_statements(o, node->v.list.items, into, false);
            walk_clauses(o, node->v.list.extra, into);
            break;
        case LHAT_NODE_IF_STMT:
        case LHAT_NODE_TRY_BLOCK:
            walk_clauses(o, node->v.list.items, into);
            break;
        case LHAT_NODE_WITH:
            walk_statements(o, node->v.list.items, into, false);
            if (node->v.list.extra != NULL) {
                walk_statement(o, node->v.list.extra, into, false);
            }
            break;
        case LHAT_NODE_FOR:
            // The focus is the loop's, not the file's.
            walk_statements(o, node->v.loop.advance, into, false);
            if (node->v.loop.body != NULL) {
                walk_statement(o, node->v.loop.body, into, false);
            }
            break;
        case LHAT_NODE_REPEAT:
            if (node->v.repeat.body != NULL) {
                walk_statement(o, node->v.repeat.body, into, false);
            }
            break;
        default:
            break;
    }
}

static void walk_statements(Outline *o, const LhatNode *list, cJSON *into,
                            bool at_root)
{
    for (const LhatNode *s = list; s != NULL && !o->failed; s = s->next) {
        walk_statement(o, s, into, at_root);
    }
}

cJSON *lsp_document_symbols_for_unit(const LhatUnit *unit)
{
    cJSON *symbols = cJSON_CreateArray();
    if (symbols == NULL || unit == NULL || unit->parsed.root == NULL) {
        return symbols;
    }
    Outline o;
    o.unit = unit;
    o.failed = false;
    const LhatNode *root = unit->parsed.root;
    if (root->kind == LHAT_NODE_BLOCK) {
        walk_statements(&o, root->v.list.items, symbols, true);
        walk_clauses(&o, root->v.list.extra, symbols);
    } else {
        walk_statement(&o, root, symbols, true);
    }
    if (o.failed) {
        cJSON_Delete(symbols);
        return NULL;
    }
    return symbols;
}
