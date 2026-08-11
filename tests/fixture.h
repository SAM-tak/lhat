// L^ (lhat) -- the shared fixtures: source-to-tree, source-to-types and
// source-to-answer, each with its teardown. Before this header they were
// written out in every test file that needed them; the shapes and names are
// theirs, kept so a test reads the same wherever it lives.
//
// Parse  -- lex and parse, for asserting on the tree.
// Unit   -- the same, then the checker, for asserting on diagnostics.
// Run    -- the same, then the compiler and a machine, for asserting on the
//           answer a program gives.
//
// The functions live in fixture.c, inside the lhat_testkit library.

#ifndef LHAT_TEST_FIXTURE_H
#define LHAT_TEST_FIXTURE_H

#include <string.h>

#include "check.h"
#include "lexer.h"
#include "object.h"
#include "parser.h"
#include "source.h"
#include "testutil.h"
#include "vm.h"

// ---------------------------------------------------------------------------
// Parse: source to tree

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult result;
} Parse;

void parse_text(Parse *p, const char *text);
// 02 の 8.2: one input of an interactive session.
void parse_interactive_text(Parse *p, const char *text);
void parse_dispose(Parse *p);

// ---------------------------------------------------------------------------
// Unit: source to types

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
} Unit;

void check_text(Unit *u, const char *text);
// The same, relaxed (03 の 3.1). The few rules that differ by strictness are
// pinned through this -- most of the suite is strictness-independent and
// stays on check_text above.
void check_relaxed_text(Unit *u, const char *text);
// The same, as the next input of a session (03 の 4.3).
void check_next_text(Unit *u, LhatCheckSession *s, const char *text);
// The same again, read the way a prompt reads it (02 の 8.2).
void check_asked_text(Unit *u, LhatCheckSession *s, const char *text);
void unit_dispose(Unit *u);

size_t syntax_errors(const Unit *u);
bool has_error(const Unit *u, LhatCheckErrorCode code);

// Every case here should parse; a syntax error would make the check
// meaningless, so it is asserted separately.
#define CHECK_CLEAN(u)                                                        \
    do {                                                                      \
        LHAT_CHECK_EQ_INT(syntax_errors(u), 0);                               \
        LHAT_CHECK_EQ_INT((u)->checked.diagnostic_count, 0);                  \
    } while (0)

#define CHECK_REPORTS(u, code)                                                \
    do {                                                                      \
        LHAT_CHECK_EQ_INT(syntax_errors(u), 0);                               \
        LHAT_CHECK(has_error(u, code), "expected " #code);                    \
    } while (0)

// For a case that is allowed to report something else. Used where the point
// is one specific refusal not happening, and the surrounding program cannot
// yet be written without any diagnostic at all.
#define CHECK_NOT_REPORTED(u, code)                                           \
    do {                                                                      \
        LHAT_CHECK_EQ_INT(syntax_errors(u), 0);                               \
        LHAT_CHECK(!has_error(u, code), "unexpected " #code);                 \
    } while (0)

// ---------------------------------------------------------------------------
// Run: source to answer

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatProto *proto;
    LhatCompileStatus compiled;
    // 03 の 4.3: the machine owns what the program allocates, so the answer
    // is only good while it is. One test, one machine.
    LhatMachine *machine;
    LhatRunResult ran;
} Run;

// Everything up to the run, for a test driving a machine of its own.
void compile_text(Run *r, const char *text);
// The same, as the next input of a session (03 の 4.3).
void compile_next_text(Run *r, LhatCompileSession *s, const char *text);
// The same again, read the way a prompt reads it (02 の 8.2), so a bare
// expression is a statement and the last one is the answer (03 の 4.3).
void compile_asked_text(Run *r, LhatCompileSession *s, const char *text);
void compiled_dispose(Run *r);

void run_text(Run *r, const char *text);
void run_dispose(Run *r);
// The same, with the checker run first. 03 の 5.11a and 5.11b read what it
// settled off the tree, so the two paths they choose between are only both
// reachable here -- run_text above compiles without checking on purpose,
// which is what pins that compiling alone still works.
void run_checked_text(Run *r, const char *text);

// The value a unit's return^ produced, asserted as an exact integer.
#define CHECK_INTEGER(r, expected)                                            \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_integer((r)->ran.value), "expected an integer");   \
        LHAT_CHECK_EQ_INT(lhat_as_integer((r)->ran.value), (expected));       \
    } while (0)

#define CHECK_REAL(r, expected)                                               \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_real((r)->ran.value), "expected a real");          \
        LHAT_CHECK(lhat_as_real((r)->ran.value) == (expected), "value");      \
    } while (0)

#define CHECK_BOOL(r, expected)                                               \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_bool((r)->ran.value), "expected a bool");          \
        LHAT_CHECK_EQ_INT(lhat_as_bool((r)->ran.value), (expected));          \
    } while (0)

#define CHECK_STRING(r, expected)                                             \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING),   \
                   "expected a string");                                      \
        if (lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING)) {        \
            const LhatString *s_ =                                            \
                (const LhatString *)lhat_as_object((r)->ran.value);           \
            LHAT_CHECK(strcmp(s_->text, (expected)) == 0,                     \
                       "got \"%s\", want \"%s\"", s_->text, (expected));      \
        }                                                                     \
    } while (0)

#endif  // LHAT_TEST_FIXTURE_H
