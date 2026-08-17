// L^ (lhat) -- syntax tree: arena, node construction, and the readings of a
// node the checker and the compiler have to agree on.

#include "ast.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/config.h"
#include "lhat/port.h"

struct LhatArenaBlock {
    LhatArenaBlock *next;
    size_t used;
    LhatNode nodes[LHAT_ARENA_BLOCK_NODES];
};

void lhat_arena_init(LhatAstArena *arena)
{
    arena->blocks = NULL;
    arena->node_count = 0;
}

void lhat_arena_dispose(LhatAstArena *arena)
{
    LhatArenaBlock *block = arena->blocks;
    while (block != NULL) {
        LhatArenaBlock *next = block->next;
        lhat_free(block);
        block = next;
    }
    arena->blocks = NULL;
    arena->node_count = 0;
}

LhatNode *lhat_node_new(LhatAstArena *arena, LhatNodeKind kind,
                        const LhatToken *at)
{
    if (arena->blocks == NULL || arena->blocks->used == LHAT_ARENA_BLOCK_NODES) {
        LhatArenaBlock *block = (LhatArenaBlock *)lhat_alloc(sizeof *block);
        if (block == NULL) {
            return NULL;
        }
        block->next = arena->blocks;
        block->used = 0;
        arena->blocks = block;
    }

    LhatNode *node = &arena->blocks->nodes[arena->blocks->used++];
    arena->node_count++;

    memset(node, 0, sizeof *node);
    node->kind = kind;
    if (at != NULL) {
        node->offset = at->offset;
        node->line = at->line;
        node->column = at->column;
        // The whole of a one-token construct. A longer one widens this as the
        // parser leaves it (parser.c's finish).
        node->end = at->offset + at->length;
    }
    return node;
}

void lhat_node_append(LhatNode **head, LhatNode **tail, LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    if (*head == NULL) {
        *head = node;
    } else {
        (*tail)->next = node;
    }
    *tail = node;
}

size_t lhat_node_list_length(const LhatNode *head)
{
    size_t n = 0;
    for (const LhatNode *node = head; node != NULL; node = node->next) {
        n++;
    }
    return n;
}

// One child, which may be absent.
static void visit_one(const char *field, const LhatNode *child,
                      LhatNodeVisitor visit, void *context)
{
    if (child != NULL) {
        visit(context, field, false, child);
    }
}

// A list threaded through `next`. Each element is one child.
static void visit_list(const char *field, const LhatNode *head,
                       LhatNodeVisitor visit, void *context)
{
    for (const LhatNode *node = head; node != NULL; node = node->next) {
        visit(context, field, true, node);
    }
}

void lhat_node_visit_children(const LhatNode *node, LhatNodeVisitor visit,
                              void *context)
{
    if (node == NULL || visit == NULL) {
        return;
    }

    switch (node->kind) {
        // Nothing below them.
        case LHAT_NODE_INT:
        case LHAT_NODE_FLOAT:
        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_FOCUS:
        case LHAT_NODE_INTERP_TEXT:
        case LHAT_NODE_TYPE_NAME:
        case LHAT_NODE_ERROR:
            break;

        case LHAT_NODE_SCOPE:
            visit_one("name", node->v.scope.name, visit, context);
            break;

        case LHAT_NODE_INTERP_HOLE:
            visit_one("value", node->v.hole.value, visit, context);
            visit_one("format", node->v.hole.format, visit, context);
            break;

        // Braced lists. BLOCK and WITH carry a second one; for a BLOCK that
        // is 9 章's clauses, for a WITH the body it guards.
        case LHAT_NODE_INTERP:
        case LHAT_NODE_TABLE:
        case LHAT_NODE_DEF:
        case LHAT_NODE_SELF_TABLE:
        case LHAT_NODE_IF_EXPR:
        case LHAT_NODE_IF_STMT:
        case LHAT_NODE_TRY_BLOCK:  // 04 の 4.5: the body and its arms
        case LHAT_NODE_TYPE_TABLE:
        case LHAT_NODE_TYPE_TUPLE:
        case LHAT_NODE_TUPLE:  // 13.8改: the positions, in order
            visit_list("items", node->v.list.items, visit, context);
            break;

        case LHAT_NODE_BLOCK:
        case LHAT_NODE_WITH:
            visit_list("items", node->v.list.items, visit, context);
            visit_list("extra", node->v.list.extra, visit, context);
            break;

        case LHAT_NODE_TABLE_ENTRY:
        case LHAT_NODE_MEMBER_DECL:
            visit_one("key", node->v.entry.key, visit, context);
            visit_one("type", node->v.entry.type, visit, context);
            visit_one("value", node->v.entry.value, visit, context);
            break;

        case LHAT_NODE_ERROR_NEW:
        case LHAT_NODE_ERRORDEF:
        case LHAT_NODE_ERROR_KIND:
            visit_one("name", node->v.named.name, visit, context);
            visit_list("members", node->v.named.members, visit, context);
            break;

        case LHAT_NODE_MODULE:
            visit_one("name", node->v.named.name, visit, context);
            break;

        // Everything holding a single operand in `jump.value`, whether it is
        // an expression's operand or a statement's.
        case LHAT_NODE_TRY:
        case LHAT_NODE_TYPEOF:
        case LHAT_NODE_SPREAD:
        case LHAT_NODE_REQUIRE:
        case LHAT_NODE_REQUIRE_STMT:
        case LHAT_NODE_IMPORT:
        case LHAT_NODE_IMPORT_STMT:
        case LHAT_NODE_PACK:
        case LHAT_NODE_CALL_STMT:
        case LHAT_NODE_BREAK:
        case LHAT_NODE_NEXT:
        case LHAT_NODE_PANIC:
        case LHAT_NODE_YIELD_ALL:
            visit_one("value", node->v.jump.value, visit, context);
            break;

        // 13.8改: return^ may carry several values -- 'return^ a, b' answers
        // a tuple. They hang off `value` as a list, and one value is a list
        // of one, so this reads every return^ ever written.
        // 13.8改: yield^ answers several values as a statement, the same way.
        case LHAT_NODE_RETURN:
        case LHAT_NODE_YIELD:
            visit_list("value", node->v.jump.value, visit, context);
            break;

        case LHAT_NODE_UNARY:
            visit_one("operand", node->v.unary.operand, visit, context);
            break;

        case LHAT_NODE_BINARY:
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            visit_one("left", node->v.binary.left, visit, context);
            visit_one("right", node->v.binary.right, visit, context);
            break;

        // 11.5 の (5): the operators stand between the operands, but they are
        // held as two lists rather than interleaved. Operands first keeps the
        // walk in source order for everything that reads a whole subtree.
        case LHAT_NODE_COMPARE_CHAIN:
            visit_list("operands", node->v.chain.operands, visit, context);
            visit_list("operators", node->v.chain.operators, visit, context);
            break;

        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX:
        case LHAT_NODE_CALL:
            visit_one("target", node->v.access.target, visit, context);
            // A call's argument field is the list of them.
            visit_list("argument", node->v.access.argument, visit, context);
            break;

        case LHAT_NODE_AS:
            visit_one("value", node->v.ascription.value, visit, context);
            visit_one("type", node->v.ascription.type, visit, context);
            break;

        case LHAT_NODE_FUNC:
        case LHAT_NODE_TYPE_FUNC:
            visit_list("params", node->v.func.params, visit, context);
            visit_one("return_type", node->v.func.return_type, visit, context);
            visit_one("body", node->v.func.body, visit, context);
            break;

        case LHAT_NODE_TYPE_CORO:
            visit_one("receive", node->v.coroutine.receive, visit, context);
            visit_one("produce", node->v.coroutine.produce, visit, context);
            visit_one("result", node->v.coroutine.result, visit, context);
            break;

        case LHAT_NODE_DEFINE:
        case LHAT_NODE_REASSIGN:
            visit_list("targets", node->v.binding.targets, visit, context);
            visit_list("values", node->v.binding.values, visit, context);
            break;

        case LHAT_NODE_FOR:
            visit_list("focus", node->v.loop.focus, visit, context);
            visit_one("bound", node->v.loop.bound, visit, context);
            visit_one("step", node->v.loop.step, visit, context);
            visit_list("advance", node->v.loop.advance, visit, context);
            visit_one("body", node->v.loop.body, visit, context);
            break;

        case LHAT_NODE_REPEAT:
            visit_one("bound", node->v.repeat.bound, visit, context);
            visit_one("body", node->v.repeat.body, visit, context);
            break;

        case LHAT_NODE_LOOP_CLAUSE:
            visit_list("body", node->v.loop_clause.body, visit, context);
            break;

        case LHAT_NODE_IF_CLAUSE:
            visit_one("condition", node->v.clause.condition, visit, context);
            visit_one("body", node->v.clause.body, visit, context);
            break;

        case LHAT_NODE_PARAM:
            visit_one("name", node->v.param.name, visit, context);
            visit_one("type", node->v.param.type, visit, context);
            visit_one("fallback", node->v.param.fallback, visit, context);
            break;

        case LHAT_NODE_KIND_COUNT:
            break;
    }
}

static void least_offset(void *context, const char *field, bool in_list,
                         const LhatNode *child);

static void least_offset_of(const LhatNode *node, uint32_t *least)
{
    // 16.2: a FOCUS carries no span at all -- it^ need not appear in the
    // source -- so its zero would drag the whole subtree back to the top of
    // the file.
    if (node->kind != LHAT_NODE_FOCUS && node->offset < *least) {
        *least = node->offset;
    }
    lhat_node_visit_children(node, least_offset, least);
}

static void least_offset(void *context, const char *field, bool in_list,
                         const LhatNode *child)
{
    (void)field;
    (void)in_list;
    least_offset_of(child, (uint32_t *)context);
}

uint32_t lhat_node_span_start(const LhatNode *node)
{
    if (node == NULL) {
        return 0;
    }
    uint32_t least = node->offset;
    least_offset_of(node, &least);
    return least;
}

const char *lhat_node_kind_name(LhatNodeKind kind)
{
    switch (kind) {
        case LHAT_NODE_INT:            return "int";
        case LHAT_NODE_FLOAT:          return "float";
        case LHAT_NODE_STRING:         return "string";
        case LHAT_NODE_NAME:           return "name";
        case LHAT_NODE_IDENT:          return "ident";
        case LHAT_NODE_HAT_IDENT:      return "hat-ident";
        case LHAT_NODE_FOCUS:          return "focus (it^)";
        case LHAT_NODE_SCOPE:          return "scope";
        case LHAT_NODE_INTERP:         return "interp";
        case LHAT_NODE_INTERP_TEXT:    return "interp-text";
        case LHAT_NODE_INTERP_HOLE:    return "interp-hole";
        case LHAT_NODE_TABLE:          return "table";
        case LHAT_NODE_TUPLE:          return "tuple";
        case LHAT_NODE_TABLE_ENTRY:    return "table-entry";
        case LHAT_NODE_DEF:            return "def";
        case LHAT_NODE_SELF_TABLE:     return "self-table";
        case LHAT_NODE_ERROR_NEW:      return "error-new";
        case LHAT_NODE_TRY:            return "try";
        case LHAT_NODE_TYPEOF:         return "typeof";
        case LHAT_NODE_SPREAD:         return "spread";
        case LHAT_NODE_ERRORDEF:       return "errordef";
        case LHAT_NODE_ERROR_KIND:     return "error-kind";
        case LHAT_NODE_MODULE:         return "module";
        case LHAT_NODE_ANNOTATION:     return "annotation";
        case LHAT_NODE_REQUIRE:        return "require";
        case LHAT_NODE_REQUIRE_STMT:   return "require-stmt";
        case LHAT_NODE_IMPORT:         return "import";
        case LHAT_NODE_IMPORT_STMT:    return "import-stmt";
        case LHAT_NODE_UNARY:          return "unary";
        case LHAT_NODE_BINARY:         return "binary";
        case LHAT_NODE_COMPARE_CHAIN:  return "compare-chain";
        case LHAT_NODE_MEMBER:         return "member";
        case LHAT_NODE_INDEX:          return "index";
        case LHAT_NODE_CALL:           return "call";
        case LHAT_NODE_AS:             return "as";
        case LHAT_NODE_PACK:           return "pack";
        case LHAT_NODE_FUNC:           return "func";
        case LHAT_NODE_IF_EXPR:        return "if-expr";
        case LHAT_NODE_DEFINE:         return "define";
        case LHAT_NODE_REASSIGN:       return "reassign";
        case LHAT_NODE_CALL_STMT:      return "call-stmt";
        case LHAT_NODE_BLOCK:          return "block";
        case LHAT_NODE_IF_STMT:        return "if-stmt";
        case LHAT_NODE_TRY_BLOCK:      return "try-block";
        case LHAT_NODE_RETURN:         return "return";
        case LHAT_NODE_BREAK:          return "break";
        case LHAT_NODE_NEXT:           return "next";
        case LHAT_NODE_PANIC:          return "panic";
        case LHAT_NODE_YIELD:          return "yield";
        case LHAT_NODE_YIELD_ALL:      return "yieldall";
        case LHAT_NODE_WITH:           return "with";
        case LHAT_NODE_FOR:            return "for";
        case LHAT_NODE_REPEAT:         return "repeat";
        case LHAT_NODE_LOOP_CLAUSE:    return "loop-clause";
        case LHAT_NODE_IF_CLAUSE:      return "if-clause";
        case LHAT_NODE_PARAM:          return "param";
        case LHAT_NODE_MEMBER_DECL:    return "member-decl";
        case LHAT_NODE_TYPE_NAME:      return "type-name";
        case LHAT_NODE_TYPE_FUNC:      return "type-func";
        case LHAT_NODE_TYPE_CORO:      return "type-coro";
        case LHAT_NODE_TYPE_TABLE:     return "type-table";
        case LHAT_NODE_TYPE_TUPLE:     return "type-tuple";
        case LHAT_NODE_TYPE_UNION:     return "type-union";
        case LHAT_NODE_TYPE_INTERSECT: return "type-intersect";
        case LHAT_NODE_ERROR:          return "error";
        case LHAT_NODE_KIND_COUNT:     break;
    }
    return "?";
}

// 01 の 2.3: the canonical name -- the word plus one hat at most, with the
// extra hats counted on the node rather than kept in the spelling. One
// definition, because what the checker looks a member up under is the
// string the machine will use as a key.
bool lhat_node_name(const LhatNode *node, const char *source_text,
                    const char *strings, const char **text, size_t *length)
{
    if (node == NULL) {
        return false;
    }
    switch (node->kind) {
        // TYPE_NAME carries a name the same way: 13.11's is^ writes a type
        // on the right, and 04 の 14.4 lets that be a qualified error kind.
        // 3.3: id^name answers the name it was written with, which is the
        // whole of what that form is.
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_TYPE_NAME:
        case LHAT_NODE_NAME: {
            // 3.1: written with backticks, so the spelling is the decoded one
            // rather than the span -- delimiters left out and a doubled
            // backtick made one.
            if (node->v.name.text_length > 0) {
                *text = strings + node->v.name.text_offset;
                *length = node->v.name.text_length;
                return true;
            }
            *text = source_text + node->v.name.offset;
            // The hats sit right after the word in the source, so the
            // canonical name is the span cut after the first of them.
            size_t word = node->v.name.length >= node->v.name.hats
                              ? node->v.name.length - node->v.name.hats
                              : node->v.name.length;
            *length = node->v.name.hats > 0 ? word + 1 : word;
            return true;
        }
        case LHAT_NODE_FOCUS:
            // 16.2: the focus with no name written is called it^, and the
            // source need not contain the word for that to be its name.
            *text = "it^";
            *length = 3;
            return true;
        case LHAT_NODE_SCOPE:
            // 01 の 8 章: the sigil says where to look, and the name is what
            // to look for -- a specifier answers with the name it is glued
            // to.
            return lhat_node_name(node->v.scope.name, source_text, strings,
                                  text, length);
        default:
            return false;
    }
}

bool lhat_name_is(const char *text, size_t length, const char *literal)
{
    size_t n = strlen(literal);
    return length == n && memcmp(text, literal, n) == 0;
}

bool lhat_node_is_environment(const LhatNode *node, const char *source_text,
                              const char *strings)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           lhat_node_name(node, source_text, strings, &name, &length) &&
           lhat_name_is(name, length, "L^");
}

const LhatNode *lhat_define_target_name(const LhatNode *target)
{
    return target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
}

const LhatNode *lhat_define_target_root(const LhatNode *target)
{
    const LhatNode *node = lhat_define_target_name(target);
    while (node->kind == LHAT_NODE_MEMBER) {
        node = node->v.access.target;
    }
    return node;
}

bool lhat_define_target_is_path(const LhatNode *target)
{
    return lhat_define_target_name(target)->kind == LHAT_NODE_MEMBER;
}
