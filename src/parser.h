// L^ (lhat) -- parser.
//
// Recursive descent for statements and types, precedence climbing for
// expressions (11.6). The parser holds the keyword knowledge: the lexer
// returns every '^'-suffixed word as one token kind and leaves the meaning
// to this stage (01 の 2.1).
//
// Everything the design documents have settled parses, in both forms of
// 1 章.

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
    LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_UNPACK,  // 13.10
    LHAT_PARSE_ERR_BINDING_ARITY,             // targets and values disagree
    LHAT_PARSE_ERR_UNPACK_NOT_ALONE,          // 13.10: unpack^ must be the only value
    LHAT_PARSE_ERR_UNPACK_MISPLACED,          // unpack^ outside a binding
    LHAT_PARSE_ERR_CLAUSE_ORDER,              // 9.2: clauses have a fixed order
    LHAT_PARSE_ERR_MAIN_REQUIRED,             // 9.3
    LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP,        // 9 章 clauses outside a loop
    LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE,          // 16.3: for^ needs a driving clause
    LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT,      // 16.5
    LHAT_PARSE_ERR_WITHDRAWN_FROM,            // 16.3: from^ replaced by ':='
    LHAT_PARSE_ERR_EXPECTED_MEMBER,           // 14 章: def^ holds members
    LHAT_PARSE_ERR_FIELD_NEEDS_NAME,          // 14.6: every field is named
    LHAT_PARSE_ERR_DUPLICATE_TEMPLATE,        // 14.6: one self^{ ... } per def^
    LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE,      // 14.12 marks members, not fields
    LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME,       // 04 の 2.4: the name is the identity
    LHAT_PARSE_ERR_ERROR_NEEDS_KIND,          // 04 の 2.5
    LHAT_PARSE_ERR_WITHDRAWN_SHIFT,           // 8.6: '<<' replaced by ':='
    LHAT_PARSE_ERR_LET_NEEDS_VALUE,           // 8.7: no declaration without one
    LHAT_PARSE_ERR_EQUALS_IS_COMPARISON,      // 8.6: 'x = 1' as a statement
    LHAT_PARSE_ERR_LEXICAL                    // the lexer already reported one
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

// Parses a whole compilation unit in the normal form of 1.1. The lexer must
// outlive the result, since string and name nodes point into its decoded
// storage.
void lhat_parse(LhatLexer *lexer, LhatParseResult *result);

// 4 章: the entry points a host needs to build the loop of 3.2. The runtime
// holds no mode of its own -- which one to call is the host's decision, made
// per fragment.

// Whether this fragment satisfies 2.3 and would be read as a juxtaposed
// call. Reads the fragment without consuming it, so a host may ask first.
bool lhat_parse_is_command(const LhatLexer *lexer);

// Parses a fragment as the command form of 2 章, falling back to the normal
// form when 2.3 is not satisfied, as 3.2 has it.
void lhat_parse_command(LhatLexer *lexer, LhatParseResult *result);

void lhat_parse_result_dispose(LhatParseResult *result);

const char *lhat_parse_error_message(LhatParseErrorCode code);

#endif  // LHAT_PARSER_H
