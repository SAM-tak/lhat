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
    LHAT_PARSE_ERR_NO_BODY_CLAUSE,            // 9.3改: none of first^, pre^,
                                              // main^ or last^ was written
    LHAT_PARSE_ERR_PRE_IN_WALK,               // 9.10: pre^ runs before the
                                              // walk has bound anything
    LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP,        // 9 章 clauses outside a loop
    LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE,          // 16.3: for^ needs a driving clause
    LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT,      // 16.5
    LHAT_PARSE_ERR_NEXT_NOT_HERE,             // 16.3: next^ updates a while^
                                              // or until^; the other forms
                                              // advance their own focus
    LHAT_PARSE_ERR_FROM_NOT_HERE,             // 16.3改2: from^ opens the
                                              // counted range of a to^ or
                                              // downto^; the other forms take
                                              // an introducer
    LHAT_PARSE_ERR_FOCUS_NEEDS_FROM,          // 16.3改2: a to^ or downto^
                                              // advances a focus of its own,
                                              // so the header takes neither an
                                              // introducer nor a bare ':='
    LHAT_PARSE_ERR_EXPECTED_MEMBER,           // 14 章: def^ holds members
    LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE,    // 11.4: only '..' has an op^
                                              // definition so far
    LHAT_PARSE_ERR_FIELD_NEEDS_NAME,          // 14.6: every field is named
    LHAT_PARSE_ERR_DUPLICATE_TEMPLATE,        // 14.6: one self^{ ... } per def^
    LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE,      // 14.12 marks members, not fields
    LHAT_PARSE_ERR_FIELD_NEEDS_TYPE,          // 04 の 2.2: a type or a default
    LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME,       // 04 の 2.4: the name is the identity
    LHAT_PARSE_ERR_ERROR_NEEDS_KIND,          // 04 の 2.5
    LHAT_PARSE_ERR_WITHDRAWN_SHIFT,           // 8.6: '<<' replaced by ':='
    LHAT_PARSE_ERR_LET_NEEDS_VALUE,           // 8.7: no declaration without one
    LHAT_PARSE_ERR_LET_NEEDS_EQUALS,          // 8.9: let^ defines and never
                                              // reassigns, so ':=' after one
                                              // says two opposite things
    LHAT_PARSE_ERR_LET_NEEDS_NAME,            // 8.9: let^ binds a name; a
                                              // member of a table is var^'s
    LHAT_PARSE_ERR_EQUALS_IS_COMPARISON,      // 8.6: 'x = 1' as a statement
    LHAT_PARSE_ERR_MODULE_MISPLACED,          // 05 の 3 章: one, and first
    LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION,  // 05 の 4 章
    LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL,     // 05 の 5.2
    LHAT_PARSE_ERR_ELSE_NEEDS_COLON,          // 5.1: 'el^ v' where the value
                                              // was read as an else-if
                                              // condition and nothing follows
    LHAT_PARSE_ERR_SPREAD_NOT_LAST,           // 13.7: '...' forwards the
                                              // whole tail, so nothing can
                                              // follow it in a call
    LHAT_PARSE_ERR_COMPOUND_ASSIGN_ONE_TARGET, // 7.4改: 'a, b += 1' has no
                                              // one target to read once
    LHAT_PARSE_ERR_LEXICAL                    // the lexer already reported one
} LhatParseErrorCode;

typedef struct {
    LhatParseErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // What was wanted, for the codes that know. Every call of expect_op names
    // a token, and saying which turns "a different token" into something a
    // reader can act on. `has_expected` is false for the codes that name
    // nothing -- reading `expected` then is reading a zero.
    bool has_expected;
    LhatOpKind expected;

    // What was there instead. Recorded for every diagnostic, since the token
    // is at hand wherever one is made, and mentioned by the codes that are
    // about a token. `length` is its width, so a mark can cover the whole of
    // it rather than its first character.
    LhatTokenKind found;
    LhatOpKind found_op;  // meaningful when `found` is LHAT_TOKEN_OP
    uint32_t length;
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

// The same, for one input of an interactive session. 02 の 8.2 accepts a bare
// expression as a statement at the top level of interactive input and nowhere
// else: being unable to work out 2 + 3 at a prompt is not an option, and a
// file keeps the rule that only a call stands alone.
//
// Such an expression is read as `return^ expr`, so the value of the input is
// its value and nothing written after it runs -- which is what 8.3 already
// says a return^ does.
void lhat_parse_interactive(LhatLexer *lexer, LhatParseResult *result);

// 4 章: the entry points a host needs to build the loop of 3.2. The runtime
// holds no mode of its own -- which one to call is the host's decision, made
// per fragment.

// Whether this fragment satisfies 2.3 and would be read as a juxtaposed
// call. Reads the fragment without consuming it, so a host may ask first.
bool lhat_parse_is_command(const LhatLexer *lexer);

// Parses a fragment as the command form of 2 章, falling back to the normal
// form when 2.3 is not satisfied, as 3.2 has it.
void lhat_parse_command(LhatLexer *lexer, LhatParseResult *result);

// 13 章's type grammar on its own, for text that is a type and nothing else:
// 14.10's printed signatures and 05 の 8.7's host registrations. `root` is
// the type node, and anything left over is reported.
void lhat_parse_type_only(LhatLexer *lexer, LhatParseResult *result);

void lhat_parse_result_dispose(LhatParseResult *result);

const char *lhat_parse_error_message(LhatParseErrorCode code);

// The message for one diagnostic, which for some of them says more than the
// code alone can -- "a ';' was expected here" rather than "expected a
// different token here". Everything the code knows on its own is what
// lhat_parse_error_message answers, so this only differs where the diagnostic
// carries something besides its code.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it. So measuring
// is a call with (NULL, 0).
size_t lhat_parse_message_write(const LhatParseDiagnostic *diagnostic,
                                char *out, size_t capacity);

#endif  // LHAT_PARSER_H
