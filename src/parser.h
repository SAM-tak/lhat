// L^ (lhat) -- parser.
//
// Recursive descent for statements and types, precedence climbing for
// expressions (11.6). The parser holds the keyword knowledge: the lexer
// returns every '^'-suffixed word as one token kind and leaves the meaning
// to this stage (01 の 2.1).
//
// Not covered yet: loop headers (for^, repeat^, while^) and their clauses
// (9 章), def^ (14 章), and the command form of a call (2 章).

#ifndef LHAT_PARSER_H
#define LHAT_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef enum {
    LHAT_PARSE_ERR_NONE,
    LHAT_PARSE_ERR_UNEXPECTED,
    LHAT_PARSE_ERR_EXPECTED_EXPRESSION,
    LHAT_PARSE_ERR_EXPECTED_TYPE,
    LHAT_PARSE_ERR_EXPECTED_NAME,
    LHAT_PARSE_ERR_EXPECTED_TOKEN,
    LHAT_PARSE_ERR_BARE_EXPRESSION,        // 8.2
    LHAT_PARSE_ERR_JUXTAPOSITION,          // 2.1: 'foo 1 2 3' outside command mode
    LHAT_PARSE_ERR_WITHDRAWN_ARROW,        // '->' as postfix reassignment (Q2)
    LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON,   // '::' as the return separator (Q9)
    LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_LET,  // 13.10
    LHAT_PARSE_ERR_BINDING_ARITY,          // targets and values disagree
    LHAT_PARSE_ERR_LEXICAL                 // the lexer already reported one
} LhatParseErrorCode;

typedef struct {
    LhatParseErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;
} LhatParseDiagnostic;

typedef struct {
    LhatNode *root;  // a BLOCK holding the statements of the unit

    LhatAstArena arena;

    LhatParseDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

    // 02 の 3.1: a REPL has to tell a syntax error apart from input that
    // merely stopped early, so this is reported separately rather than
    // inferred from the text of a message.
    bool incomplete;
} LhatParseResult;

// Parses a whole compilation unit. The lexer must outlive the result, since
// string and name nodes point into its decoded storage.
void lhat_parse(LhatLexer *lexer, LhatParseResult *result);

void lhat_parse_result_dispose(LhatParseResult *result);

const char *lhat_parse_error_message(LhatParseErrorCode code);

#endif  // LHAT_PARSER_H
