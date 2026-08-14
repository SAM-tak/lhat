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
    LHAT_NODE_NAME,          // 01 の 3.3: id^name -- the spelling as a value
    LHAT_NODE_IDENT,
    LHAT_NODE_HAT_IDENT,     // used as a value: true^, nil^, self^ ...
    // 16.2: the focus for^ introduced when no name was written. Carries no
    // span, since 'it^' need not appear in the source for it to be there.
    LHAT_NODE_FOCUS,
    LHAT_NODE_SCOPE,         // $x, $$x, $^x  (01 の 8 章). Repeating '$'
                             // counts inwards from the unit and '^' outwards
                             // from here. The sigil is glued to the name and
                             // nothing may come between, so '$.x' is not
                             // another way of writing it
    LHAT_NODE_INTERP,        // $"..." -- list of INTERP_TEXT / INTERP_HOLE
    LHAT_NODE_INTERP_TEXT,
    LHAT_NODE_INTERP_HOLE,   // expression plus an optional format string
    LHAT_NODE_TABLE,         // { ... }
    LHAT_NODE_TABLE_ENTRY,   // key := value, or a positional value
    LHAT_NODE_TUPLE,         // (a, b)  (13.8改). The value side of the type
                             // grammar's own rule: a ',' inside the
                             // parentheses makes a tuple, none leaves the
                             // grouping '(x)' was always read as
    LHAT_NODE_DEF,           // def^{ ... }                  (14 章)
    LHAT_NODE_SELF_TABLE,    // self^{ ... }                 (14.6, 14.11)
    LHAT_NODE_ERROR_NEW,     // error^Kind{ ... }            (04 の 2.5)
    LHAT_NODE_TRY,           // try^ expr                    (04 の 5 章)
    LHAT_NODE_TYPEOF,        // typeof^(expr)                (14.16)
    LHAT_NODE_SPREAD,        // expr...  as the last call argument (13.7)
    LHAT_NODE_REQUIRE,       // require^ "path"              (05 の 5 章)
    LHAT_NODE_REQUIRE_STMT,  // require^ "path" on its own   (05 の 5.5)
    LHAT_NODE_IMPORT,        // import^ a.b.c                (05 の 8.7)
    LHAT_NODE_IMPORT_STMT,   // import^ a.b.c on its own     (05 の 8.7)
    LHAT_NODE_UNARY,
    LHAT_NODE_BINARY,
    LHAT_NODE_COMPARE_CHAIN, // a < b < c  (11.5 の (5))
    LHAT_NODE_MEMBER,        // a.b   a?.b
    LHAT_NODE_INDEX,         // a[i]  a?[i]
    LHAT_NODE_CALL,          // f(x)  f?(x)
    LHAT_NODE_AS,            // expr as^ Type
    LHAT_NODE_PACK,          // pack^ expr            (13.8改): the one bridge
                             // from a tuple to a table a name can hold
    LHAT_NODE_FUNC,          // f^... { ... } / p^... { ... }
    LHAT_NODE_IF_EXPR,       // if^c: e el^c: e el^: e ;

    // ---- statements ----
    LHAT_NODE_DEFINE,        // let^ a, b = x, y
    LHAT_NODE_REASSIGN,      // a, b := x, y
    LHAT_NODE_CALL_STMT,
    LHAT_NODE_BLOCK,         // do^{ ... } and any braced body
    LHAT_NODE_IF_STMT,
    LHAT_NODE_TRY_BLOCK,     // try^{ ... catch^T: ... catch^: ... }
                             // (04 の 4.5). The items are IF_CLAUSE nodes:
                             // the first is the body, the rest are arms
                             // whose `condition` is the written type -- NULL
                             // on the bare one, which takes what is left
    LHAT_NODE_RETURN,
    LHAT_NODE_BREAK,
    LHAT_NODE_PANIC,         // panic^ expr                 (04 の 11.6)
    LHAT_NODE_YIELD,
    LHAT_NODE_YIELD_ALL,     // yieldall^ expr              (15.8)
    LHAT_NODE_WITH,          // with^ x = e  ... { ... }    (12 章)
    LHAT_NODE_FOR,           // for^ ...                    (16 章)
    LHAT_NODE_REPEAT,        // repeat^ ...                 (16.5)
    LHAT_NODE_LOOP_CLAUSE,   // prolog^: first^: last^: ... (9 章)
    LHAT_NODE_ERRORDEF,      // errordef^ Name { ... }      (04 の 2.2)
    LHAT_NODE_ERROR_KIND,    // one kind inside an errordef^
    LHAT_NODE_MODULE,        // module^ a.b.c               (05 の 3 章)

    // ---- pieces ----
    LHAT_NODE_IF_CLAUSE,     // condition (may be absent) plus a body
    LHAT_NODE_PARAM,         // name:type = default, or ...:type
    LHAT_NODE_MEMBER_DECL,   // name : type   inside t^{ ... }

    // ---- types (13 章) ----
    LHAT_NODE_TYPE_NAME,     // number^, FooBar
    LHAT_NODE_TYPE_FUNC,     // f^A, B -> C;
    LHAT_NODE_TYPE_CORO,     // c^{ f^recv -> yield;, ret }  (13.9, 15.3改)
    LHAT_NODE_TYPE_TABLE,    // t^{ member : type }
    LHAT_NODE_TYPE_TUPLE,    // (A, B)  (13.8改). Two positions or more: '(T)'
                             // is the grouping the type grammar already had
    LHAT_NODE_TYPE_UNION,    // A | B
    LHAT_NODE_TYPE_INTERSECT,// A & B

    LHAT_NODE_ERROR,

    LHAT_NODE_KIND_COUNT
} LhatNodeKind;

// 16.1: what the clause after the focus does with it.
typedef enum {
    LHAT_FOR_TO,      // to^ limit [step^ n]
    LHAT_FOR_DOWNTO,  // downto^ limit [step^ n]
    LHAT_FOR_IN,      // in^ iterable
    LHAT_FOR_WHILE,   // while^ cond [next^ stmt]
    LHAT_FOR_UNTIL,   // until^ cond [next^ stmt]
    LHAT_FOR_IF,      // if^ cond -- not a loop at all (16.3)
    LHAT_FOR_WHEN,    // when^ patterns -- not a loop either (17 章)
    LHAT_FOR_ONCE     // do^: expr, and the outer one of a run of for^ (16.3):
                      // a focus with no clause to drive it, so the body is
                      // reached once
} LhatForKind;

// 16.5: repeat^ carries no focus.
typedef enum {
    LHAT_REPEAT_FOREVER,
    LHAT_REPEAT_COUNT,
    LHAT_REPEAT_WHILE,
    LHAT_REPEAT_UNTIL
} LhatRepeatKind;

// 14.12. Marks a member that replaces or extends an inherited one; without
// a marker, a member of the same name is an error.
typedef enum {
    LHAT_DEF_PLAIN,
    LHAT_DEF_OVERRIDE,
    LHAT_DEF_OVERLOAD,
    LHAT_DEF_ABSTRACT  // 14.15: declared, and left for a composition to give
} LhatDefModifier;

// 9.2, in the order they must be written -- which is also the order they run
// in. 9.10's pre^ is the one that runs before the condition is tested, so it
// stands between prolog^ and first^.
typedef enum {
    LHAT_CLAUSE_PROLOG = 0,
    LHAT_CLAUSE_PRE = 1,
    LHAT_CLAUSE_FIRST = 2,
    LHAT_CLAUSE_MAIN = 3,
    LHAT_CLAUSE_LAST = 4,
    LHAT_CLAUSE_EPILOG = 5,
    LHAT_CLAUSE_FINALLY = 6
} LhatClauseKind;

// 9.3: the clauses that are the loop's body. At least one has to be there,
// and an unheaded statement list is an implicit main^.
#define LHAT_CLAUSE_IS_BODY(k)                                                \
    ((k) == LHAT_CLAUSE_PRE || (k) == LHAT_CLAUSE_FIRST ||                    \
     (k) == LHAT_CLAUSE_MAIN || (k) == LHAT_CLAUSE_LAST)

typedef struct LhatNode LhatNode;

struct LhatNode {
    LhatNodeKind kind;

    // Where the construct starts, for diagnostics.
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // One past the last byte the construct consumed, so [offset, end) is the
    // whole of it. Diagnostics only ever needed the start, but a tool that
    // shows or replaces the source of one node needs the span (06 の 4.2).
    //
    // A node built from a single token gets this from that token. Anything
    // longer sets it on the way out of the construct, so a parse function
    // that forgets leaves a span that is too short rather than one that runs
    // past the construct -- and test_parser.c's span_encloses_children walk
    // finds the omission, since a parent that ends before a child does is
    // always a missed one.
    uint32_t end;

    LhatNode *next;  // next sibling when this node sits in a list

#if LHAT_WITH_COMMENTS
    // 01 の 6.4: the comments written against this node, in source order,
    // threaded through their own `next_for_node`. They live in the lexer's
    // table, so the lexer has to outlive the tree -- as it already must for
    // a string's bytes.
    //
    // Filled in by parser.c's attach_comments, once the whole unit is parsed
    // and the table has stopped growing.
    LhatComment *comments;
#endif

    // FUNC only: the whole signature infer_func (check.c) built for this
    // literal, an LhatType* from the checker's arena. void* here rather than
    // LhatType* so this parse-stage header need not depend on the check
    // stage's; compile_subroutine (vm.c) is the reader, and casts it back.
    // NULL until checking runs, and unset entirely when it never does.
    void *checked_type;

    // 03 の 5.11c: which arm of an overloaded member (02 の 14.12) the checker
    // settled on, as the index into that member's arms plus one -- zero means
    // it settled nothing, which is what an unchecked compile always sees.
    //
    // CALL: the candidate this call means, written only under strict, where
    // resolving is not optional (a call that fits no arm is a MISMATCH). The
    // compiler turns it into the PICKARM that spares the run the search.
    //
    // TABLE_ENTRY marked override^: the arm it replaces, written whatever the
    // strictness -- it decides the shape of the group rather than how a call
    // reads one, and 03 の 4.2 keeps that the same either way.
    uint16_t checked_arm;

    union {
        struct {
            uint64_t value;
            uint8_t base;
        } integer;

        double real;

        // STRING / INTERP_TEXT: bytes live in the lexer's decoded string
        // storage, so the lexer must outlive the tree.
        struct {
            uint32_t offset;
            uint32_t length;
            LhatStringKind kind;
        } string;

        // IDENT / HAT_IDENT / TYPE_NAME / NAME: a span of the source text.
        struct {
            uint32_t offset;
            uint32_t length;
            uint32_t hats;

            // 01 の 3.1: a name written with backticks is spelled by neither
            // the delimiters nor a doubled backtick, so its spelling is not
            // the span above -- the lexer decoded it into its string storage
            // and this is where. Zero length everywhere else, which is every
            // name that is its own source slice.
            uint32_t text_offset;
            uint32_t text_length;
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
            // 15.13: written closed^, so the body names nothing standing
            // outside it. Part of the type, and written rather than read off
            // the body -- what a caller may rely on is what was promised.
            bool closed;
        } func;

        // PARAM. `variadic` marks the '...' form (13.7).
        struct {
            LhatNode *name;     // NULL for a variadic or an unnamed type slot
            LhatNode *type;     // NULL when not annotated
            LhatNode *fallback; // default value, NULL when absent
            bool variadic;
        } param;

        // TABLE_ENTRY / MEMBER_DECL. Inside a def^ the key is NULL when the
        // entry is the self^{ ... } template, whose value carries it.
        struct {
            LhatNode *key;    // NULL for a positional entry or the template
            LhatNode *value;
            LhatDefModifier modifier;  // 14.12, def^ entries only
            // 14.14改: the key was written '[ ... ]', so it is an expression
            // rather than the name it would otherwise be read as.
            bool computed;
            // 14.15: written 'abstract^ name : type', so `value` is a type
            // and the definition provides nothing under the name. What is
            // composed onto it has to. Set on a template field too, where
            // 14.12's modifier has no place.
            bool declared;
            // 13.7, 14.10: MEMBER_DECL only. Written '...:type' in a
            // t^{ ... }, the way a parameter list writes a variadic one --
            // `value` is the element type, `key` is unused.
            bool variadic;
        } entry;

        // DEFINE / REASSIGN take a list of targets and a list of values;
        // LET takes a list of targets and a single value (13.10).
        struct {
            LhatNode *targets;
            LhatNode *values;
            bool exported;  // 05 の 4 章: written public^
            // 8.9. DEFINE only: written let^ (or with^) rather than var^, so
            // the names it introduces refuse a later ':='. What the machine
            // does to a slot is not a write in this sense -- 16.4's to^ and
            // downto^ advance their focus themselves, and 8.7's LOADNIL seeds
            // one before its own definition runs; both leave the name still
            // one nothing written in the source may reassign.
            bool immutable;
            // 8.9 with 12.1 and 16.3改2: the construct bound it, not a word
            // the writer chose -- a with^, or the focus of a counted for^.
            // There is no var^ to write instead, so the diagnostic for a ':='
            // on one of these must not offer it.
            bool bound_by_form;
            // 8.8改. DEFINE only, and only from let^: written ':=' rather
            // than '='. For a plain name this changes nothing (8.6 already
            // makes the two spellings the same there); for a path target
            // whose last segment already exists it makes this a reassignment
            // instead of the redefinition error 8.7 would otherwise report.
            bool via_reassign_op;
            // 7.4. REASSIGN only, and only from 'target op= value': the
            // plain operator the compound spelling stands for (e.g. '+=' ->
            // LHAT_OP_ADD). `values` holds just the right-hand side, not the
            // expanded 'target op value' -- target is evaluated once, and
            // where that matters (a path target) is a checker/compiler
            // concern, not a parse-time expansion.
            bool has_compound_op;
            LhatOpKind compound_op;
        } binding;

        // Statement or expression lists: BLOCK, TABLE, IF_STMT, IF_EXPR,
        // INTERP, CALL argument lists, WITH bindings.
        //
        // For BLOCK, `items` holds the unlabelled statements, which are the
        // main clause, and `extra` holds the other clauses of 9 章 as a list
        // of LHAT_NODE_LOOP_CLAUSE. A block with no clause markers keeps
        // `extra` empty, so the common case is unchanged.
        struct {
            LhatNode *items;
            LhatNode *extra;  // WITH: body. BLOCK: clause list.
        } list;

        // 16.3. `focus` is a list: bindings, destructuring targets, or one
        // expression when the focus is unnamed and reached through it^.
        struct {
            LhatForKind kind;
            LhatNode *focus;
            LhatNode *bound;    // limit, iterable or condition
            LhatNode *step;     // step^, absent for every kind but to^/downto^
            LhatNode *advance;  // next^ statements
            LhatNode *body;
            // 5.2 and 17.2: written in the form opened by ':' rather than
            // by '{', so it answers a value. Only if^ and the when^ clauses
            // have that form; the driving clauses iterate and answer nothing.
            bool is_expression;
        } loop;

        struct {
            LhatRepeatKind kind;
            LhatNode *bound;  // count or condition, absent when forever
            LhatNode *body;
        } repeat;

        struct {
            LhatClauseKind kind;
            LhatNode *body;
        } loop_clause;

        // ERRORDEF / ERROR_KIND / ERROR_NEW (04 の 2.2, 2.5). A name and the
        // list it introduces: kinds for a declaration, MEMBER_DECL fields for
        // a kind -- the same shape t^{ ... } already uses -- and TABLE_ENTRY
        // values for a construction. ERROR_NEW's name is a qualified path.
        struct {
            LhatNode *name;
            LhatNode *members;  // NULL when a kind declares no fields
            bool exported;      // 05 の 4 章: written public^
        } named;

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

        // 02 の 9.8: how many loops a break^ leaves, counting the innermost
        // as 1. Written either as the hats on the word (break^^^) or in
        // brackets after it (break^[3]); the parser settles the two into
        // this. Unused by the other jumps.
        //
        // BREAK only: `value` holds what was bracketed when it was not a
        // count -- 9.8's label form, which nothing labels a loop for yet.
        //
        // RETURN / YIELD carry an optional value; BREAK carries a level;
        // PANIC's value is required (`phantom` unused).
        struct {
            LhatNode *value;
            // YIELD only. 15.11: written '_yield^', which types exactly as a
            // yield^ does and makes the body yieldable, but never suspends.
            bool phantom;
            // BREAK: the number of loops to leave, and never zero -- a plain
            // break^ is one.
            //
            // RETURN / YIELD (13.8改): how many values are written, when
            // several are. They hang off `value` as a list. 0 and 1 both mean
            // one, which is every return^ and yield^ written before tuples.
            uint32_t level;
        } jump;

        // TYPE_CORO: the three types of 13.9.
        struct {
            LhatNode *receive;
            LhatNode *produce;
            LhatNode *result;
            // 15.3改: which kind of body it came from, written as the front
            // half of 13.9's form ('c^{ f^R -> Y;, T }'). What may advance a
            // coroutine, and where it may be held, follows from this.
            bool is_function;
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

// Calls `visit` once per child, in source order, for every kind of node.
//
// Nothing is visited twice and nothing is skipped, so a caller can walk the
// whole tree without a case of its own per kind -- which is what a tool that
// renders or serialises the tree wants, unlike semantic_tokens.c, whose whole
// job is to treat each kind differently.
//
// `field` names the place the child came from, spelled as the union member in
// this header ("left", "items", "targets"), so a serialised tree can be read
// back without knowing the order each kind writes its children in. `in_list`
// says that place holds a list rather than a single child -- true even when
// the list happens to hold one, so a reader can shape it the same way every
// time.
typedef void (*LhatNodeVisitor)(void *context, const char *field, bool in_list,
                                const LhatNode *child);
void lhat_node_visit_children(const LhatNode *node, LhatNodeVisitor visit,
                              void *context);

// Where the construct actually begins: the least offset in the subtree.
//
// Not the node's own offset. An infix or postfix node is written starting at
// its operator -- the '+' of 'a + b', the '(' of 'f(1)' -- with the left side
// hanging below it, so [lhat_node_span_start(n), n->end) is the whole of the
// construct while [n->offset, n->end) is only the part from the operator on.
uint32_t lhat_node_span_start(const LhatNode *node);

// 01 の 2.3: the hat is part of the name. A hat identifier's name is the
// word plus one hat -- 'self^' is a different name from 'self' -- and a
// spelling with more hats than one is that same name reached further out:
// this^^^ is the this^ two levels up, its name still 'this^' with the count
// kept on the node (v.name.hats) for the constructs that stack. What this
// answers is that canonical spelling, cut from `source_text` (the text the
// node's offsets index into). The checker looks a member up under it and
// the compiler uses it as a key, so one definition is what keeps the two
// agreeing byte for byte.
//
// 3.1: a name written with backticks is spelled in `strings` instead -- the
// lexer's decoded storage, since the delimiters are not part of the name.
// Both bases are handed over and the node says which one it is spelled in.
bool lhat_node_name(const LhatNode *node, const char *source_text,
                    const char *strings, const char **text, size_t *length);

bool lhat_name_is(const char *text, size_t length, const char *literal);

// 05 の 8.6: L^ names the machine's own table. Only the hatted spelling
// means it, so an ordinary name `L` is untouched.
bool lhat_node_is_environment(const LhatNode *node, const char *source_text,
                              const char *strings);

// The name a let^ target carries, whether or not it was annotated.
const LhatNode *lhat_define_target_name(const LhatNode *target);

// 8.8: 'let^ a.b.c = v' introduces c inside a table reached through a and
// b. Only the root is a name a scope holds; the rest are members.
const LhatNode *lhat_define_target_root(const LhatNode *target);
bool lhat_define_target_is_path(const LhatNode *target);

#endif  // LHAT_AST_H
