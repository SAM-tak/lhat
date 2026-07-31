// L^ (lhat) -- syntax tree: arena and node construction.

#include "ast.h"

#include <stdlib.h>
#include <string.h>

#define LHAT_ARENA_BLOCK_NODES 256

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
        free(block);
        block = next;
    }
    arena->blocks = NULL;
    arena->node_count = 0;
}

LhatNode *lhat_node_new(LhatAstArena *arena, LhatNodeKind kind,
                        const LhatToken *at)
{
    if (arena->blocks == NULL || arena->blocks->used == LHAT_ARENA_BLOCK_NODES) {
        LhatArenaBlock *block = (LhatArenaBlock *)malloc(sizeof *block);
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

const char *lhat_node_kind_name(LhatNodeKind kind)
{
    switch (kind) {
        case LHAT_NODE_INT:            return "int";
        case LHAT_NODE_FLOAT:          return "float";
        case LHAT_NODE_STRING:         return "string";
        case LHAT_NODE_NAME:           return "name";
        case LHAT_NODE_IDENT:          return "ident";
        case LHAT_NODE_HAT_IDENT:      return "hat-ident";
        case LHAT_NODE_SCOPE:          return "scope";
        case LHAT_NODE_INTERP:         return "interp";
        case LHAT_NODE_INTERP_TEXT:    return "interp-text";
        case LHAT_NODE_INTERP_HOLE:    return "interp-hole";
        case LHAT_NODE_TABLE:          return "table";
        case LHAT_NODE_TABLE_ENTRY:    return "table-entry";
        case LHAT_NODE_UNARY:          return "unary";
        case LHAT_NODE_BINARY:         return "binary";
        case LHAT_NODE_COMPARE_CHAIN:  return "compare-chain";
        case LHAT_NODE_MEMBER:         return "member";
        case LHAT_NODE_INDEX:          return "index";
        case LHAT_NODE_CALL:           return "call";
        case LHAT_NODE_AS:             return "as";
        case LHAT_NODE_UNPACK:         return "unpack";
        case LHAT_NODE_FUNC:           return "func";
        case LHAT_NODE_IF_EXPR:        return "if-expr";
        case LHAT_NODE_DEFINE:         return "define";
        case LHAT_NODE_REASSIGN:       return "reassign";
        case LHAT_NODE_CALL_STMT:      return "call-stmt";
        case LHAT_NODE_BLOCK:          return "block";
        case LHAT_NODE_IF_STMT:        return "if-stmt";
        case LHAT_NODE_RETURN:         return "return";
        case LHAT_NODE_BREAK:          return "break";
        case LHAT_NODE_YIELD:          return "yield";
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
        case LHAT_NODE_TYPE_UNION:     return "type-union";
        case LHAT_NODE_TYPE_INTERSECT: return "type-intersect";
        case LHAT_NODE_ERROR:          return "error";
        case LHAT_NODE_KIND_COUNT:     break;
    }
    return "?";
}
