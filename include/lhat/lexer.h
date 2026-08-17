// L^ (lhat) -- lexical analyser.
//
// Implements sections 1 through 10 of DesignDocuments/01-lexical-structure.md.
//
// The lexer deliberately has no keyword table. A '^'-suffixed identifier is
// always returned as LHAT_TOKEN_HAT_IDENT and the parser decides whether it
// is a keyword, a type name or a meta property (section 2.1).
//
// Public, and the one part of the front end that is. Where a word begins and
// ends cannot be worked out from outside: 2.3 makes the hat part of the name,
// 6.2's block comments nest, 5.2's raw strings write a quote by doubling it,
// 5.4's interpolations hold code. A host that colours L^ in an editor has to
// read it the way the compiler does, and this is that reading (07 の 4 章).
//
// What it does not answer is which spellings the language knows -- 2.1
// reserves none, and a host drawing `def^` apart from a member somebody named
// `tostring^` is guessing from the spelling. The tree, the checker's types
// and the instruction set stay in src/: past the words, a host is better
// served by the checker's answers than by the parser's.
//
// Not reached by lhat.h, which is the header for running a program. This one
// is named by whoever wants it.

#ifndef LHAT_LEXER_H
#define LHAT_LEXER_H

#include "lhat/config.h"
#include "lhat/source.h"
#include "lhat/token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LHAT_ERR_NONE,
    LHAT_ERR_UNEXPECTED_CHARACTER,
    LHAT_ERR_INVALID_UTF8,
    LHAT_ERR_BARE_HAT,                   // 2.5
    LHAT_ERR_BARE_AT,                    // 02 の 18.2
    LHAT_ERR_IDENT_AFTER_NUMBER,         // 10.3: '1to^3'
    LHAT_ERR_MALFORMED_NUMBER,           // '0x' with no digits, stray '_'
    LHAT_ERR_MALFORMED_EXPONENT,         // 4.5: 'e' not followed by digits
    LHAT_ERR_INTEGER_OVERFLOW,
    LHAT_ERR_UNTERMINATED_STRING,        // 5.5
    LHAT_ERR_UNTERMINATED_NAME_LITERAL,  // 3.1: backtick not closed on the line
    LHAT_ERR_EMPTY_NAME_LITERAL,         // 3.1: `` names nothing
    LHAT_ERR_UNKNOWN_ESCAPE,             // 5.1
    LHAT_ERR_MALFORMED_ESCAPE,           // \xHH or \u{...} with bad digits
    LHAT_ERR_UNTERMINATED_BLOCK_COMMENT, // 6.2
    LHAT_ERR_SCOPE_WITHOUT_NAME,         // 8: '$' must be glued to a name
    LHAT_ERR_INTERPOLATION_NEEDS_QUOTES, // 5.4: $'...' does not interpolate
    LHAT_ERR_INTERPOLATION_TOO_DEEP      // 5.4: nesting limit reached
} LhatErrorCode;

typedef struct {
    LhatErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;
} LhatDiagnostic;


typedef struct {
    const LhatSource *source;

    size_t pos;
    uint32_t line;
    uint32_t column;

    bool pending_newline;  // a newline was crossed since the last token
    bool after_dot;        // 10.1: digits after '.' scan as an integer only

    // Interpolation mode stack (section 5.4). Frames alternate between a
    // string segment and a hole; brace_depth counts the '{' opened inside a
    // hole so that a table literal's '}' is not mistaken for the hole's end.
    struct {
        bool in_hole;
        uint32_t brace_depth;
    } interp[LHAT_INTERP_MAX_DEPTH];
    size_t interp_depth;

    char *strings;         // decoded string literal bytes
    size_t strings_length;
    size_t strings_capacity;

    LhatDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

#if LHAT_WITH_COMMENTS
    // 6.4, in source order. Grows as scanning goes; nothing removes from it.
    LhatComment *comments;
    size_t comment_count;
    size_t comment_capacity;
#endif
} LhatLexer;

void lhat_lexer_init(LhatLexer *lexer, const LhatSource *source);
void lhat_lexer_dispose(LhatLexer *lexer);

// Returns LHAT_TOKEN_EOF repeatedly once the input is exhausted. On a lexical
// error it records a diagnostic, returns LHAT_TOKEN_ERROR and skips the
// offending code point so that scanning can continue.
LhatToken lhat_lexer_next(LhatLexer *lexer);

// Decoded bytes of a LHAT_TOKEN_STRING, LHAT_TOKEN_INTERP_TEXT or
// LHAT_TOKEN_INTERP_FORMAT. The pointer stays valid until the lexer is
// disposed, but may be invalidated by further calls to lhat_lexer_next(), so
// copy it if it must outlive the scan.
const char *lhat_lexer_string(const LhatLexer *lexer, const LhatToken *token,
                              size_t *length);

const char *lhat_lexer_error_message(LhatErrorCode code);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_LEXER_H
