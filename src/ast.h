// L^ (lhat) -- syntax tree.
//
// Section numbers in the comments refer to DesignDocuments/02-syntax.md
// unless prefixed with "01", which means 01-lexical-structure.md.
//
// Nodes come from an arena and are never freed individually; disposing the
// arena frees the whole tree. Lists are singly linked through `next` rather
// than held in arrays, so a node can be appended without ever reallocating
// what an earlier node already points at.

#ifndef LHAT_AST_H
#define LHAT_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "token.h"

typedef enum {
    // ---- expressions ----
    LHAT_NODE_INT,
    LHAT_NODE_FLOAT,
    LHAT_NODE_STRING,
    LHAT_NODE_NAME,          // `a name`
    LHAT_NODE_IDENT,
    LHAT_NODE_HAT_IDENT,     // used as a value: true^, nil^, self^ ...
    LHAT_NODE_SCOPE,         // $x, $$x, $^x  (01 の 8 章)
    LHAT_NODE_INTERP,        // $"..." -- list of INTERP_TEXT / INTERP_HOLE
    LHAT_NODE_INTERP_TEXT,
    LHAT_NODE_INTERP_HOLE,   // expression plus an optional format string
    LHAT_NODE_TABLE,         // { ... }
    LHAT_NODE_TABLE_ENTRY,   // key := value, or a positional value
    LHAT_NODE_UNARY,
    LHAT_NODE_BINARY,
    LHAT_NODE_COMPARE_CHAIN, // a < b < c  (11.5 の (5))
    LHAT_NODE_MEMBER,        // a.b   a?.b
    LHAT_NODE_INDEX,         // a[i]  a?[i]
    LHAT_NODE_CALL,          // f(x)  f?(x)
    LHAT_NODE_AS,            // expr as^ Type
    LHAT_NODE_UNPACK,        // unpack^ expr          (13.10)
    LHAT_NODE_FUNC,          // f^... { ... } / p^... { ... }
    LHAT_NODE_IF_EXPR,       // if^c: e el^c: e el^: e ;

    // ---- statements ----
    LHAT_NODE_DEFINE,        // a, b := x, y
    LHAT_NODE_REASSIGN,      // a, b << x, y
    LHAT_NODE_CALL_STMT,
    LHAT_NODE_BLOCK,         // do^{ ... } and any braced body
    LHAT_NODE_IF_STMT,
    LHAT_NODE_RETURN,
    LHAT_NODE_BREAK,
    LHAT_NODE_YIELD,
    LHAT_NODE_WITH,          // with^ x := e  ... { ... }   (12 章)

    // ---- pieces ----
    LHAT_NODE_IF_CLAUSE,     // condition (may be absent) plus a body
    LHAT_NODE_PARAM,         // name:type = default, or ...:type
    LHAT_NODE_MEMBER_DECL,   // name : type   inside t^{ ... }

    // ---- types (13 章) ----
    LHAT_NODE_TYPE_NAME,     // number^, FooBar
    LHAT_NODE_TYPE_FUNC,     // f^A, B -> C;
    LHAT_NODE_TYPE_CORO,     // c^{ recv, yield, ret }
    LHAT_NODE_TYPE_TABLE,    // t^{ member : type }
    LHAT_NODE_TYPE_UNION,    // A | B
    LHAT_NODE_TYPE_INTERSECT,// A & B

    LHAT_NODE_ERROR,

    LHAT_NODE_KIND_COUNT
} LhatNodeKind;

typedef struct LhatNode LhatNode;

struct LhatNode {
    LhatNodeKind kind;

    // Where the construct starts, for diagnostics.
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    LhatNode *next;  // next sibling when this node sits in a list

    union {
        struct {
            uint64_t value;
            uint8_t base;
        } integer;

        double real;

        // STRING / NAME / INTERP_TEXT: bytes live in the lexer's decoded
        // string storage, so the lexer must outlive the tree.
        struct {
            uint32_t offset;
            uint32_t length;
            LhatStringKind kind;
        } string;

        // IDENT / HAT_IDENT / TYPE_NAME: a span of the source text.
        struct {
            uint32_t offset;
            uint32_t length;
            uint32_t hats;
        } name;

        // 01 の 8 章: the sigil is glued to a name, so the two are one node.
        struct {
            LhatScopeKind kind;
            uint32_t depth;
            LhatNode *name;
        } scope;

        struct {
            LhatOpKind op;
            LhatNode *operand;
        } unary;

        struct {
            LhatOpKind op;
            LhatNode *left;
            LhatNode *right;
        } binary;

        // COMPARE_CHAIN: operands are a list of length n, operators a list of
        // length n-1 held as UNARY nodes carrying just the operator.
        struct {
            LhatNode *operands;
            LhatNode *operators;
        } chain;

        // MEMBER / INDEX / CALL. `nil_safe` marks the ?. ?[ ?( forms.
        struct {
            LhatNode *target;
            LhatNode *argument;  // member name, index expression, or arg list
            bool nil_safe;
        } access;

        struct {
            LhatNode *value;
            LhatNode *type;
        } ascription;

        // FUNC and TYPE_FUNC. `is_function` separates f^ from p^ (15 章).
        struct {
            LhatNode *params;
            LhatNode *return_type;  // NULL when nothing is returned
            LhatNode *body;         // NULL for a type
            bool is_function;
            bool yields;            // 15.2: inferred from the body
        } func;

        // PARAM. `variadic` marks the '...' form (13.7).
        struct {
            LhatNode *name;     // NULL for a variadic or an unnamed type slot
            LhatNode *type;     // NULL when not annotated
            LhatNode *fallback; // default value, NULL when absent
            bool variadic;
        } param;

        // TABLE_ENTRY / MEMBER_DECL.
        struct {
            LhatNode *key;    // NULL for a positional table entry
            LhatNode *value;
        } entry;

        // DEFINE / REASSIGN take a list of targets and a list of values;
        // LET takes a list of targets and a single value (13.10).
        struct {
            LhatNode *targets;
            LhatNode *values;
        } binding;

        // Statement or expression lists: BLOCK, TABLE, IF_STMT, IF_EXPR,
        // INTERP, CALL argument lists, WITH bindings.
        struct {
            LhatNode *items;
            LhatNode *extra;  // WITH: body. IF_*: nothing.
        } list;

        // IF_CLAUSE. `condition` is NULL for the final else.
        struct {
            LhatNode *condition;
            LhatNode *body;
        } clause;

        // INTERP_HOLE. `format` is the raw text after ':' (5.4).
        struct {
            LhatNode *value;
            LhatNode *format;
        } hole;

        // RETURN / YIELD carry an optional value; BREAK carries a level.
        struct {
            LhatNode *value;
        } jump;

        // TYPE_CORO: the three types of 13.9.
        struct {
            LhatNode *receive;
            LhatNode *produce;
            LhatNode *result;
        } coroutine;
    } v;
};

// Arena holding every node of one tree.
typedef struct LhatArenaBlock LhatArenaBlock;

typedef struct {
    LhatArenaBlock *blocks;
    size_t node_count;
} LhatAstArena;

void lhat_arena_init(LhatAstArena *arena);
void lhat_arena_dispose(LhatAstArena *arena);

// Returns NULL only when out of memory.
LhatNode *lhat_node_new(LhatAstArena *arena, LhatNodeKind kind,
                        const LhatToken *at);

const char *lhat_node_kind_name(LhatNodeKind kind);

// Appends to a list built from `next` pointers. `head` and `tail` are the
// caller's cursors; passing the tail avoids walking the list every time.
void lhat_node_append(LhatNode **head, LhatNode **tail, LhatNode *node);

size_t lhat_node_list_length(const LhatNode *head);

#endif  // LHAT_AST_H
