// L^ (lhat) -- lexical analyser.
//
// Implements sections 1 through 10 of DesignDocuments/01-lexical-structure.md.
//
// The lexer deliberately has no keyword table. A '^'-suffixed identifier is
// always returned as LHAT_TOKEN_HAT_IDENT and the parser decides whether it
// is a keyword, a type name or a meta property (section 2.1).

#ifndef LHAT_LEXER_H
#define LHAT_LEXER_H

#include "lhatconfig.h"
#include "source.h"
#include "token.h"

typedef enum {
    LHAT_ERR_NONE,
    LHAT_ERR_UNEXPECTED_CHARACTER,
    LHAT_ERR_INVALID_UTF8,
    LHAT_ERR_BARE_HAT,                   // 2.5
    LHAT_ERR_LONE_QUESTION_MARK,         // 3.2: '?' only forms ?. ?( ?[
    LHAT_ERR_IDENT_AFTER_NUMBER,         // 10.3 (Q7): '1to^3'
    LHAT_ERR_MALFORMED_NUMBER,           // '0x' with no digits, stray '_'
    LHAT_ERR_MALFORMED_EXPONENT,         // 4.5: 'e' not followed by digits
    LHAT_ERR_INTEGER_OVERFLOW,
    LHAT_ERR_UNTERMINATED_STRING,        // 5.5
    LHAT_ERR_UNTERMINATED_NAME_LITERAL,  // 3.4: backtick not closed on the line
    LHAT_ERR_EMPTY_NAME_LITERAL,         // 3.4: `` names nothing
    LHAT_ERR_UNKNOWN_ESCAPE,             // 5.1
    LHAT_ERR_MALFORMED_ESCAPE,           // \xHH or \u{...} with bad digits
    LHAT_ERR_UNTERMINATED_BLOCK_COMMENT, // 6.2
    LHAT_ERR_SCOPE_WITHOUT_NAME,         // 8: '$' must be glued to a name
    LHAT_ERR_SCOPE_DOUBLED,              // 8: '$$' was the unit while '$' was
                                         // a global; the global is gone
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

#endif  // LHAT_LEXER_H
