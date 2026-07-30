// L^ (lhat) -- token definitions.
//
// See DesignDocuments/01-lexical-structure.md for the specification these
// definitions follow. Section numbers in the comments refer to that document.

#ifndef LHAT_TOKEN_H
#define LHAT_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Section 11.
typedef enum {
    LHAT_TOKEN_EOF,
    LHAT_TOKEN_IDENT,
    LHAT_TOKEN_HAT_IDENT,    // identifier followed by one or more '^'

    // Section 3.4. A backtick delimited name denotes one that either cannot be
    // written as a bare identifier, or is meant as a value rather than as a
    // reference. Deliberately a separate kind from LHAT_TOKEN_IDENT: were it
    // the same, `a` and a would be indistinguishable and the second use would
    // be impossible to express.
    LHAT_TOKEN_NAME_LITERAL,

    LHAT_TOKEN_INT,
    LHAT_TOKEN_FLOAT,
    LHAT_TOKEN_STRING,
    LHAT_TOKEN_SCOPE,        // '$', '$$', '$^^^'
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

    LHAT_OP_DEFINE,      // :=
    LHAT_OP_REASSIGN,    // <<   target << value (Q2)
    LHAT_OP_ARROW,       // ->   separates arguments from return value (Q9)

    // Neither of these is part of the language; both are recognised so the
    // parser can say what replaced them instead of reporting a stray
    // character. Q2 first made '->' postfix reassignment before settling on
    // '<<', and Q9 first used '::' as the return separator before moving to
    // '->', which reads better among the many uses of ':'.
    LHAT_OP_COLONCOLON,  // ::


    LHAT_OP_EQ,          // =    comparison, not assignment
    LHAT_OP_NE,          // ≠ != =/
    LHAT_OP_LE,          // ≦ <=
    LHAT_OP_GE,          // ≧ >=
    LHAT_OP_LT,
    LHAT_OP_GT,

    LHAT_OP_ADD,
    LHAT_OP_SUB,
    LHAT_OP_MUL,
    LHAT_OP_DIV,
    LHAT_OP_FLOORDIV,    // //   (Q5 freed this by moving comments to '#')
    LHAT_OP_MOD,         // %
    LHAT_OP_POW,         // **

    LHAT_OP_NOT,         // !    prefix logical negation (Q3)

    // These are operators (01 の 7.1) but reach the parser as hat
    // identifiers, since the lexer keeps no keyword table (01 の 2.1). They
    // never appear in the operator table and are listed here so a syntax
    // tree can name them alongside the rest.
    LHAT_OP_AND,         // and^
    LHAT_OP_OR,          // or^
    LHAT_OP_IS,          // is^


    LHAT_OP_UNION,       // |    type union, valid only in a type context
    LHAT_OP_INTERSECT,   // &    type intersection (14.5)

    LHAT_OP_DOT,
    LHAT_OP_CONCAT,      // ..
    LHAT_OP_ELLIPSIS,    // ...  variadic argument marker
    LHAT_OP_NIL_DOT,     // ?.
    LHAT_OP_NIL_CALL,    // ?(
    LHAT_OP_NIL_INDEX,   // ?[

    LHAT_OP_AT,          // @

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
    LHAT_SCOPE_GLOBAL,    // $
    LHAT_SCOPE_FILE,      // $$
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

const char *lhat_token_kind_name(LhatTokenKind kind);
const char *lhat_op_name(LhatOpKind op);
const char *lhat_string_kind_name(LhatStringKind kind);
const char *lhat_scope_kind_name(LhatScopeKind kind);

#endif  // LHAT_TOKEN_H
