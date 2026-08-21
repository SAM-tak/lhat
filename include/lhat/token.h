// L^ (lhat) -- token definitions.
//
// See DesignDocuments/01-lexical-structure.md for the specification these
// definitions follow. Section numbers in the comments refer to that document.
//
// Public, with lhat/lexer.h: a host that reads L^ rather than runs it -- one
// colouring a script editor, say -- needs to know what a token is. Not
// reached by lhat.h, which is the header for running; this one is named by
// whoever wants it (07 の 4 章).

#ifndef LHAT_TOKEN_H
#define LHAT_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LHAT_WITH_COMMENTS changes the shape of LhatLexer and LhatNode, so it
// comes from the generated header rather than a compile definition.
#include "lhat/version.h"

#ifdef __cplusplus
extern "C" {
#endif

// Section 10.9.
typedef enum {
    LHAT_TOKEN_EOF,
    // Section 3.1. A name written with backticks is one of these too -- the
    // form is a way of spelling an identifier, not a different kind of name,
    // so `a` and a are the same name and come back the same way. What tells
    // the two apart is `v.string`, where the lexer put the spelling: the
    // delimiters are not part of the name and a doubled backtick is one.
    LHAT_TOKEN_IDENT,
    LHAT_TOKEN_HAT_IDENT,    // identifier followed by one or more '^'
    // 02 の 18.2: '@' glued to an identifier. One token, so '@ export' is
    // not one -- the spelling carries the name without the '@', the way a
    // hat identifier's carries the hats separately.
    LHAT_TOKEN_ANNOTATION,

    LHAT_TOKEN_INT,
    LHAT_TOKEN_FLOAT,
    LHAT_TOKEN_STRING,
    LHAT_TOKEN_SCOPE,        // '$', '$^^^'
    LHAT_TOKEN_OP,

    // Section 5.4. An interpolated string is delivered as a sequence rather
    // than a single token, so the expressions inside the holes are scanned by
    // the ordinary rules instead of by a second, duplicated scanner:
    //
    //   BEGIN ( TEXT | EXPR_BEGIN <tokens> [FORMAT] EXPR_END )* END
    //
    // TEXT is emitted only when the segment is non-empty.
    LHAT_TOKEN_INTERP_BEGIN,       // $"
    LHAT_TOKEN_INTERP_TEXT,        // a literal run between holes
    LHAT_TOKEN_INTERP_EXPR_BEGIN,  // {
    LHAT_TOKEN_INTERP_FORMAT,      // the raw text after ':'
    LHAT_TOKEN_INTERP_EXPR_END,    // }
    LHAT_TOKEN_INTERP_END,         // closing "

    LHAT_TOKEN_ERROR
} LhatTokenKind;

// Section 7.1. The lexer never classifies a hat identifier as a keyword;
// that is the parser's job (section 2.1).
typedef enum {
    LHAT_OP_LPAREN,
    LHAT_OP_RPAREN,
    LHAT_OP_LBRACKET,
    LHAT_OP_RBRACKET,
    LHAT_OP_LBRACE,
    LHAT_OP_RBRACE,
    LHAT_OP_COMMA,
    LHAT_OP_SEMICOLON,
    LHAT_OP_COLON,

    LHAT_OP_REASSIGN,    // :=
    LHAT_OP_ARROW,       // ->   separates arguments from return value

    // 7.4: compound assignment. 'target op= value' means 'target :=
    // target op value', with target evaluated once. Each names the plain
    // operator it stands for; the parser reads the pair apart.
    LHAT_OP_ADD_ASSIGN,      // +=
    LHAT_OP_SUB_ASSIGN,      // -=
    LHAT_OP_MUL_ASSIGN,      // *=
    LHAT_OP_DIV_ASSIGN,      // /=
    LHAT_OP_FLOORDIV_ASSIGN, // //=
    LHAT_OP_MOD_ASSIGN,      // %=
    LHAT_OP_POW_ASSIGN,      // **=
    LHAT_OP_CONCAT_ASSIGN,   // ..=

    // 8.6.4: the assignments written to skip an absent place -- 'a ?op= b'
    // is 'if^ a? { a op= b }', and 'a ?:= b' is 'if^ a? { a := b }'. The
    // 'NIL_' reads as it does in NIL_DOT: the form steps around a nil^
    // rather than reaching into one.
    LHAT_OP_NIL_REASSIGN,        // ?:=
    LHAT_OP_NIL_ADD_ASSIGN,      // ?+=
    LHAT_OP_NIL_SUB_ASSIGN,      // ?-=
    LHAT_OP_NIL_MUL_ASSIGN,      // ?*=
    LHAT_OP_NIL_DIV_ASSIGN,      // ?/=
    LHAT_OP_NIL_FLOORDIV_ASSIGN, // ?//=
    LHAT_OP_NIL_MOD_ASSIGN,      // ?%=
    LHAT_OP_NIL_POW_ASSIGN,      // ?**=
    LHAT_OP_NIL_CONCAT_ASSIGN,   // ?..=

    // Not part of the language; recognised so the parser can point at '->'
    // (01 の 7.6) instead of reporting a stray character.

    LHAT_OP_LSHIFT,      // <<   reserved
    LHAT_OP_RSHIFT,      // >>   reserved

    LHAT_OP_EQ,          // =    comparison, not assignment
    LHAT_OP_NE,          // ≠ != =/
    LHAT_OP_LE,          // ≦ <=
    LHAT_OP_GE,          // ≧ >=
    LHAT_OP_LT,
    LHAT_OP_GT,
    // 11.9: three-way comparison. The one comparison a type defines,
    // and the four orderings are read off it -- 'a < b' is '(a <=> b) < 0'.
    LHAT_OP_SPACESHIP,   // <=>

    LHAT_OP_ADD,
    LHAT_OP_SUB,
    LHAT_OP_MUL,
    LHAT_OP_DIV,
    LHAT_OP_FLOORDIV,    // //   (01 の 6.1 freed this by putting comments on '#')
    LHAT_OP_MOD,         // %
    LHAT_OP_POW,         // **

    LHAT_OP_NOT,         // !    prefix logical negation

    // These are operators (01 の 7.1) but reach the parser as hat
    // identifiers, since the lexer keeps no keyword table (01 の 2.1). They
    // never appear in the operator table and are listed here so a syntax
    // tree can name them alongside the rest.
    LHAT_OP_AND,         // and^
    LHAT_OP_OR,          // or^
    LHAT_OP_IS,          // is^   identity: the same instance, not just equal
    LHAT_OP_ISA,         // isa^  fits: the left may stand where the type is
    LHAT_OP_CATCH,       // catch^  (04 の 4 章)


    LHAT_OP_UNION,       // |    type union, valid only in a type context
    LHAT_OP_INTERSECT,   // &    type intersection (14.5)

    LHAT_OP_DOT,
    LHAT_OP_CONCAT,      // ..
    LHAT_OP_ELLIPSIS,    // ...  variadic argument marker
    LHAT_OP_NIL_DOT,     // ?.
    LHAT_OP_NIL_CALL,    // ?(
    LHAT_OP_NIL_INDEX,   // ?[
    LHAT_OP_NIL_ELSE,    // ??   value to use when the left side is nil^ (11.7)
    LHAT_OP_PRESENT,     // ?    postfix: not absent (11.7改2). The rest of the
                         // '?' family reaches through an absent value; this
                         // one asks about it and answers bool^

    LHAT_OP_COUNT
} LhatOpKind;

// Section 5.
typedef enum {
    LHAT_STRING_ESCAPED,  // "..."   escapes processed, may span lines
    LHAT_STRING_RAW,      // '...'   no escapes, '' denotes a single quote
    LHAT_STRING_LINE      // """...  runs to the end of the line
} LhatStringKind;

// Section 8.
typedef enum {
    LHAT_SCOPE_FILE,      // $
    LHAT_SCOPE_RELATIVE   // $^, $^^, ...
} LhatScopeKind;

typedef struct {
    LhatTokenKind kind;

    uint32_t offset;  // byte offset into the normalised source text
    uint32_t length;  // length in bytes
    uint32_t line;    // 1-based
    uint32_t column;  // 1-based, counted in code points, not bytes

    // Section 10.9. The only newline-sensitive rule in the language reads
    // this flag: a call or index '(' must not be preceded by a newline.
    // The first token of a file has it set.
    bool preceded_by_newline;

    union {
        LhatOpKind op;

        uint32_t hats;  // LHAT_TOKEN_HAT_IDENT: number of trailing '^'

        struct {
            uint64_t value;
            uint8_t base;  // 2, 8, 10 or 16
        } integer;

        double real;

        // Also used by LHAT_TOKEN_INTERP_TEXT and LHAT_TOKEN_INTERP_FORMAT.
        struct {
            LhatStringKind kind;
            // Offsets into the lexer's decoded string storage, not into the
            // source. Use lhat_lexer_string() to read the bytes.
            uint32_t offset;
            uint32_t length;
        } string;

        struct {
            LhatScopeKind kind;
            uint32_t depth;  // number of '^' for LHAT_SCOPE_RELATIVE
        } scope;
    } v;
} LhatToken;

#if LHAT_WITH_COMMENTS
// 6.4. A comment, kept rather than discarded. The span covers the whole of it,
// '#' or '#[' and ']#' included, so the text of one is a slice of the source.
//
// The lexer keeps these in a table of its own, in source order, rather than
// mixing them into the token stream: the parser reads tokens and never has to
// know these exist. What ties one to a node is its position, which is why
// nothing here names a node -- parser.c's attach_comments does the tying,
// once the whole unit is parsed and the table has stopped growing.
//
// Here rather than in lexer.h so that ast.h can name it without the syntax
// tree having to know what a lexer is.
typedef struct LhatComment LhatComment;

struct LhatComment {
    uint32_t offset;
    uint32_t end;  // one past the last byte
    uint32_t line;
    uint32_t column;
    bool block;  // '#[ ... ]#' rather than '#' to the end of the line

    // The next comment of the same node, in source order. Threaded here
    // rather than kept as a range of the table, because a node may be given
    // comments in two goes -- the ones written above it and the one left at
    // the end of its last line -- with other nodes' comments in between.
    LhatComment *next_for_node;
};
#endif

const char *lhat_token_kind_name(LhatTokenKind kind);
const char *lhat_op_name(LhatOpKind op);
const char *lhat_string_kind_name(LhatStringKind kind);
const char *lhat_scope_kind_name(LhatScopeKind kind);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_TOKEN_H
