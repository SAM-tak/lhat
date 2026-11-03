// L^ (lhat) -- parser.
//
// Recursive descent for statements and types, precedence climbing for
// expressions ([syntax.op.table]). The parser holds the keyword knowledge: the lexer
// returns every '^'-suffixed word as one token kind and leaves the meaning
// to this stage ([lex.hat.no-reserved]).
//
// Everything the design documents have settled parses, in both forms of
// [lton.why].

#ifndef LHAT_PARSER_H
#define LHAT_PARSER_H

#include "ast.h"
#include "lhat/error.h"  // [lsp.next]: the fix a diagnostic carries
#include "lhat/lexer.h"

typedef enum {
    LHAT_PARSE_ERR_NONE,
    LHAT_PARSE_ERR_UNEXPECTED,
    LHAT_PARSE_ERR_EXPECTED_EXPRESSION,
    LHAT_PARSE_ERR_EXPECTED_TYPE,
    LHAT_PARSE_ERR_EXPECTED_NAME,
    LHAT_PARSE_ERR_EXPECTED_TOKEN,
    LHAT_PARSE_ERR_BARE_EXPRESSION,        // [syntax.stmt.no-bare-expr]
    LHAT_PARSE_ERR_JUXTAPOSITION,          // [modules.env.why-one]: 'foo 1 2 3' outside command mode
    LHAT_PARSE_ERR_RESERVED_SHIFT,         // '<<' and '>>' are reserved
    LHAT_PARSE_ERR_ID_NEEDS_NAME,          // [lex.ident.id-hat]: id^ spells a name
    LHAT_PARSE_ERR_BINDING_ARITY,             // targets and values disagree
    LHAT_PARSE_ERR_CLAUSE_ORDER,              // [syntax.loop.order]: clauses have a fixed order
    LHAT_PARSE_ERR_MAIN_REQUIRED,             // [syntax.loop.body]
    LHAT_PARSE_ERR_NO_BODY_CLAUSE,            // [syntax.loop.body]: none of first^, pre^,
                                              // main^ or last^ was written
    LHAT_PARSE_ERR_PRE_IN_WALK,               // [syntax.loop.pre]: pre^ runs before the
                                              // walk has bound anything
    LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP,        // [syntax.loop.clauses] outside a loop
    LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE,          // [syntax.for.forms]: for^ needs a driving clause
    LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT,      // [syntax.for.repeat]
    LHAT_PARSE_ERR_NEXT_NOT_HERE,             // [syntax.for.forms]: next^ updates a while^
                                              // or until^; the other forms
                                              // advance their own focus
    LHAT_PARSE_ERR_FROM_NOT_HERE,             // [syntax.for.from]: from^ opens the
                                              // counted range of a to^ or
                                              // downto^; the other forms take
                                              // an introducer
    LHAT_PARSE_ERR_FOCUS_NEEDS_FROM,          // [syntax.for.from]: a to^ or downto^
                                              // advances a focus of its own,
                                              // so the header takes neither an
                                              // introducer nor a bare ':='
    LHAT_PARSE_ERR_EXPECTED_MEMBER,           // [syntax.obj]: def^ holds members
    LHAT_PARSE_ERR_SIGNATURE_NAMED,           // [syntax.type.semicolon]: a signature's
                                              // parameters carry no names
    LHAT_PARSE_ERR_PARAM_NEEDS_NAME,          // [syntax.type.as-expr]: a type stood where a
                                              // parameter name is wanted
    LHAT_PARSE_ERR_FRESH_TUPLE,               // [syntax.sub.fresh]: fresh^ covers one
                                              // result, and a tuple is several
    LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE,    // [syntax.op.framework]: only '..' has an op^
                                              // definition so far
    LHAT_PARSE_ERR_COMPOUND_NOT_DEFINABLE,    // [syntax.stmt.compound]: 'a += b' is
                                              // 'a := a + b', so the plain
                                              // operator is what answers it
    LHAT_PARSE_ERR_COMPARISON_NOT_DEFINABLE,  // [syntax.op.compare]: the orderings are read
                                              // off '<=>', so that is the one
                                              // comparison to write
    LHAT_PARSE_ERR_FIELD_NEEDS_NAME,          // [syntax.obj.template]: every field is named
    LHAT_PARSE_ERR_DUPLICATE_TEMPLATE,        // [syntax.obj.template]: one self^{ ... } per def^
    LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE,      // [syntax.obj.override] marks members, not fields
    LHAT_PARSE_ERR_OVERLOAD_OUTSIDE_DEF,
    LHAT_PARSE_ERR_OVERRIDE_OUTSIDE_DEF,
    LHAT_PARSE_ERR_DUPLICATE_DELEGATE,        // [syntax.obj.delegate]: one delegate^ per def^
    LHAT_PARSE_ERR_DELEGATE_TARGET,           // [syntax.obj.delegate]: self^.name, or a name
    LHAT_PARSE_ERR_TEMPLATE_NEEDS_BODY,
    LHAT_PARSE_ERR_CATCH_AFTER_BARE,          // [errors.catch.clause]: the bare arm takes
                                              // what is left, so it is last
    LHAT_PARSE_ERR_CATCH_ARM_NEEDS_TYPE,      // [errors.catch.clause]: an arm names a
                                              // kind and ends on ':'. The
                                              // other reading of the word is
                                              // [binary.net.api]'s, and takes parentheses
    LHAT_PARSE_ERR_FIELD_NEEDS_TYPE,          // [errors.kind.errordef]: a type or a default
    LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME,       // [errors.kind.nominal]: the name is the identity
    LHAT_PARSE_ERR_ENUMDEF_NEEDS_NAME,        // [syntax.enum]: no anonymous enum
    LHAT_PARSE_ERR_ERROR_NEEDS_KIND,          // [errors.kind.new]
    LHAT_PARSE_ERR_LET_NEEDS_VALUE,           // [syntax.stmt.visible]: no declaration without one
    LHAT_PARSE_ERR_FOCUS_TAKES_ONE,           // [syntax.for.forms]: one binding per for^ --
                                              // the next takes a for^ of its
                                              // own, never a comma
    LHAT_PARSE_ERR_LET_NEEDS_EQUALS,          // [syntax.stmt.let]: let^ defines and never
                                              // reassigns, so ':=' after one
                                              // says two opposite things
    LHAT_PARSE_ERR_LET_NEEDS_NAME,            // [syntax.stmt.let]: let^ binds a name; a
                                              // member of a table is var^'s
    LHAT_PARSE_ERR_EQUALS_IS_COMPARISON,      // [modules.host.machine]: 'x = 1' as a statement
    LHAT_PARSE_ERR_MODULE_MISPLACED,          // [modules.unit]: one, and first
    LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION,  // [modules.public]
    LHAT_PARSE_ERR_LTON_IMPORT,              // imports only in the LTON preamble
    LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL,     // [modules.require.literal]
    LHAT_PARSE_ERR_ELSE_NEEDS_COLON,          // [syntax.if.expr]: 'el^ v' where the value
                                              // was read as an else-if
                                              // condition and nothing follows
    LHAT_PARSE_ERR_IF_EXPR_NEEDS_ELSE,        // [syntax.if.expr]: an if^ written as an
                                              // expression answers in every
                                              // case, so the last clause takes
                                              // no condition
    LHAT_PARSE_ERR_MATCH_OPENS_AFTER_SUBJECT, // [syntax.match.semicolon]: a match is opened by the
                                              // ':' after its subject, so its
                                              // clauses do not follow a 'do^:'
    LHAT_PARSE_ERR_SPREAD_NOT_LAST,           // [syntax.type.variadic]: '...' forwards the
                                              // whole tail, so nothing can
                                              // follow it in a call
    LHAT_PARSE_ERR_HATS_DONT_STACK,           // [lex.hat.repeat]: extra hats count
                                              // levels, and this word has no
                                              // levels to count
    LHAT_PARSE_ERR_BAD_POSITION_COUNT,        // [syntax.obj.structure-repeat]: 'type[n]' makes n
                                              // positions, so n is a positive
                                              // integer written out -- a
                                              // count of none would leave the
                                              // type saying nothing
    // [syntax.annot.where]: an annotation is written above a declaration, and there is
    // nothing else for one to be about.
    LHAT_PARSE_ERR_ANNOTATION_NEEDS_DECLARATION,
    // [syntax.annot.literal-args]: an argument is a literal, since an annotation never runs.
    LHAT_PARSE_ERR_ANNOTATION_ARG_NOT_LITERAL,
    LHAT_PARSE_ERR_NAMED_TAKES_NO_COUNT,      // [syntax.obj.structure-repeat]: a name holds one
    LHAT_PARSE_ERR_DUPLICATE_INDEXER         // One key/value constraint per table.
                                              // value, and 'type[n]' is how
                                              // several positions are
                                              // written
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

    // [lsp.next]: the fixes this diagnostic knows how to make, read through
    // lhat_fix_slot_read. One fix of one edit is all the parser works out so
    // far; a diagnostic with more edits than one takes an array off the
    // result, which is where the public LhatFix's edit count is looking.
    LhatFixSlot fixes[LHAT_FIX_SLOTS];
} LhatParseDiagnostic;

typedef struct {
    LhatNode *root;  // a BLOCK holding the statements of the unit

    LhatAstArena arena;

    LhatParseDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

    // [syntax.repl.incomplete]: a REPL has to tell a syntax error apart from input that
    // merely stopped early, so this is reported separately rather than
    // inferred from the text of a message.
    bool incomplete;
} LhatParseResult;

// Parses a whole compilation unit in the normal form of [syntax.input.free]. The lexer must
// outlive the result, since string and name nodes point into its decoded
// storage.
void lhat_parse(LhatLexer *lexer, LhatParseResult *result);

// Parse leading imports followed by unbraced table entries as a pure function.
void lhat_parse_lton(LhatLexer *lexer, LhatParseResult *result);

// The same, for one input of an interactive session. [syntax.stmt.no-bare-expr] accepts a bare
// expression as a statement at the top level of interactive input and nowhere
// else: being unable to work out 2 + 3 at a prompt is not an option, and a
// file keeps the rule that only a call stands alone.
//
// Such an expression is read as `return^ expr`, so the value of the input is
// its value and nothing written after it runs -- which is what [syntax.stmt.forms] already
// says a return^ does.
void lhat_parse_interactive(LhatLexer *lexer, LhatParseResult *result);

// [lsp.hover]: the entry points a host needs to build the loop of [binary.codec.kinds]. The runtime
// holds no mode of its own -- which one to call is the host's decision, made
// per fragment.

// Whether this fragment satisfies [errors.kind.made] and would be read as a juxtaposed
// call. Reads the fragment without consuming it, so a host may ask first.
bool lhat_parse_is_command(const LhatLexer *lexer);

// Parses a fragment as the command form of [syntax.juxta], falling back to the normal
// form when [syntax.juxta.when] is not satisfied, as [binary.codec.kinds] has it.
void lhat_parse_command(LhatLexer *lexer, LhatParseResult *result);

// [syntax.type]'s type grammar on its own, for text that is a type and nothing else:
// [syntax.obj.structure]'s printed signatures and [modules.host.import]'s host registrations. `root` is
// the type node, and anything left over is reported.
void lhat_parse_type_only(LhatLexer *lexer, LhatParseResult *result);

void lhat_parse_result_dispose(LhatParseResult *result);

// The text of a code, holes unfilled: a code about the token it met holds
// the hole that names it ([l10n.compose.hole-kinds]).
const char *lhat_parse_error_message(LhatParseErrorCode code);
// [l10n.id]: the stable ID of the same message; NULL for a code the table
// does not hold.
const char *lhat_parse_error_id(LhatParseErrorCode code);

// [l10n.choose.program]: a message is written in the language of the program it is about.
// lhat/program.h is where that is declared; naming it here asks for nothing
// else.
struct LhatProgram;

// The message for one diagnostic, which for some of them says more than the
// code alone can -- "a ';' was expected here" rather than "expected a
// different token here", and what the token met was. The token wanted and
// the phrase naming the token met go into the text's holes.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it. So measuring
// is a call with (NULL, 0).
size_t lhat_parse_message_write(const struct LhatProgram *program,
                                const LhatParseDiagnostic *diagnostic,
                                char *out, size_t capacity);
// [l10n.id]: the ID of the text that message is drawn from; NULL for a code no
// table holds.
const char *lhat_parse_message_id(const LhatParseDiagnostic *diagnostic);
// [l10n.id]: the IDs of the texts a message is drawn from besides its code's, by
// index from 0; NULL past the last. For the tests that hold every ID to [syntax.runtime-api].
const char *lhat_parse_part_id(size_t index);

#endif  // LHAT_PARSER_H
