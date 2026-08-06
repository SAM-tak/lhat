// L^ (lhat) -- tests for the lexer.
//
// Section numbers refer to DesignDocuments/01-lexical-structure.md. The cases
// under "lexical hazards" are the ones a naive maximal-munch scanner gets
// wrong; they are the reason the specification exists.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "source.h"
#include "testutil.h"
#include "token.h"

#define MAX_TOKENS 128

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatToken tokens[MAX_TOKENS];
    size_t count;
} Scan;

static void scan_text(Scan *s, const char *text)
{
    lhat_source_init_from_string(&s->source, "<test>", text, strlen(text));
    lhat_lexer_init(&s->lexer, &s->source);
    s->count = 0;
    for (;;) {
        LhatToken token = lhat_lexer_next(&s->lexer);
        if (s->count < MAX_TOKENS) {
            s->tokens[s->count++] = token;
        }
        if (token.kind == LHAT_TOKEN_EOF || s->count >= MAX_TOKENS) {
            break;
        }
    }
}

static void scan_dispose(Scan *s)
{
    lhat_lexer_dispose(&s->lexer);
    lhat_source_dispose(&s->source);
}

// Number of tokens excluding the trailing EOF.
static size_t token_count(const Scan *s)
{
    return s->count > 0 ? s->count - 1 : 0;
}

static bool is_op(const LhatToken *token, LhatOpKind op)
{
    return token->kind == LHAT_TOKEN_OP && token->v.op == op;
}

static const char *token_text(const Scan *s, size_t index)
{
    return s->source.text + s->tokens[index].offset;
}

static size_t token_length(const Scan *s, size_t index)
{
    return s->tokens[index].length;
}

// ---------------------------------------------------------------------------

static void test_identifiers(void)
{
    Scan s;

    LHAT_TEST("plain identifiers");
    scan_text(&s, "foo _bar baz9");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_STR(token_text(&s, 0), token_length(&s, 0), "foo");
    LHAT_CHECK_EQ_STR(token_text(&s, 1), token_length(&s, 1), "_bar");
    LHAT_CHECK_EQ_STR(token_text(&s, 2), token_length(&s, 2), "baz9");
    scan_dispose(&s);

    // Section 2.1: the lexer has no keyword table, so if^ and if are simply an
    // identifier with and without the suffix.
    LHAT_TEST("hat suffix marks a hat identifier");
    scan_text(&s, "if if^");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].v.hats, 1);
    scan_dispose(&s);

    // Section 2.3.
    LHAT_TEST("repeated hats are counted");
    scan_text(&s, "super^^^");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.hats, 3);
    scan_dispose(&s);

    // Section 2.4: no whitespace may separate the identifier from the hat.
    LHAT_TEST("a detached hat is an error");
    scan_text(&s, "if ^");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_BARE_HAT);
    scan_dispose(&s);

    LHAT_TEST("non-ascii identifiers are accepted");
    scan_text(&s, "\xE5\xA4\x89\xE6\x95\xB0");  // 変数
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    scan_dispose(&s);

    // Section 3.2: '?' and '!' are operators and terminate an identifier.
    LHAT_TEST("question mark no longer ends an identifier");
    scan_text(&s, "foo?.bar");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK_EQ_STR(token_text(&s, 0), token_length(&s, 0), "foo");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_NIL_DOT), "expected ?.");
    LHAT_CHECK_EQ_STR(token_text(&s, 2), token_length(&s, 2), "bar");
    scan_dispose(&s);
}

// Section 3.4.
static void test_name_literals(void)
{
    Scan s;
    size_t length = 0;
    const char *bytes = NULL;

    // The point of the form: a name may hold characters the bare identifier
    // rules exclude, such as the trailing '?' dropped in 3.2.
    LHAT_TEST("a name may contain a question mark");
    scan_text(&s, "`foo?`");
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_NAME_LITERAL);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "foo?");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    LHAT_TEST("a name may contain spaces and symbols");
    scan_text(&s, "`is empty? (yes/no)`");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "is empty? (yes/no)");
    scan_dispose(&s);

    // Nothing inside is interpreted, so a name may hold what would otherwise
    // be operators, comment markers or a hat suffix.
    LHAT_TEST("the contents are not interpreted");
    scan_text(&s, "`if^ # 1..2`");
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "if^ # 1..2");
    scan_dispose(&s);

    LHAT_TEST("a name may be written in japanese");
    scan_text(&s, "`\xE7\xA9\xBA\xE3\x81\x8B?`");  // 空か?
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "\xE7\xA9\xBA\xE3\x81\x8B?");
    scan_dispose(&s);

    // Distinct from LHAT_TOKEN_IDENT on purpose: `a` must not collapse into
    // the same token as a, or a name could not be used as a value.
    LHAT_TEST("a name literal is not an ordinary identifier");
    scan_text(&s, "a `a`");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_NAME_LITERAL);
    scan_dispose(&s);

    LHAT_TEST("the delimiter is not part of the name");
    scan_text(&s, "`ab`");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "ab");
    LHAT_CHECK_EQ_INT(token_length(&s, 0), 4);  // the token still spans the ticks
    scan_dispose(&s);

    LHAT_TEST("a doubled backtick stands for one backtick");
    scan_text(&s, "`a``b`");
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "a`b");
    scan_dispose(&s);

    LHAT_TEST("a name literal terminates an identifier");
    scan_text(&s, "x`y`");
    LHAT_CHECK_EQ_INT(token_count(&s), 2);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_NAME_LITERAL);
    scan_dispose(&s);

    LHAT_TEST("a name literal works as a table key");
    scan_text(&s, "foo[`a b`]");
    LHAT_CHECK_EQ_INT(token_count(&s), 4);
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_NAME_LITERAL);
    scan_dispose(&s);

    // Unlike a string, it stops at the end of the line so a missing delimiter
    // cannot swallow the rest of the file.
    LHAT_TEST("an unclosed name literal stops at the line end");
    scan_text(&s, "`oops\nnext");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code,
                      LHAT_ERR_UNTERMINATED_NAME_LITERAL);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_IDENT);
    scan_dispose(&s);

    LHAT_TEST("an empty name literal is rejected");
    scan_text(&s, "``");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_EMPTY_NAME_LITERAL);
    scan_dispose(&s);
}

static void test_numbers(void)
{
    Scan s;

    LHAT_TEST("decimal integer");
    scan_text(&s, "42");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_INT);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.value, 42);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.base, 10);
    scan_dispose(&s);

    LHAT_TEST("radix prefixes");
    scan_text(&s, "0xFF 0b1010 0o17");
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.value, 255);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.base, 16);
    LHAT_CHECK_EQ_INT(s.tokens[1].v.integer.value, 10);
    LHAT_CHECK_EQ_INT(s.tokens[2].v.integer.value, 15);
    scan_dispose(&s);

    LHAT_TEST("digit separators");
    scan_text(&s, "1_000_000");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_INT);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.value, 1000000);
    scan_dispose(&s);

    LHAT_TEST("a trailing separator is malformed");
    scan_text(&s, "1_");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_MALFORMED_NUMBER);
    scan_dispose(&s);

    LHAT_TEST("floats and exponents");
    scan_text(&s, "1.5 1.5e2 2.0E-1");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_FLOAT);
    LHAT_CHECK(fabs(s.tokens[0].v.real - 1.5) < 1e-12, "1.5");
    LHAT_CHECK(fabs(s.tokens[1].v.real - 150.0) < 1e-12, "1.5e2");
    LHAT_CHECK(fabs(s.tokens[2].v.real - 0.2) < 1e-12, "2.0E-1");
    scan_dispose(&s);

    // Section 4.2: C style octal is not supported, so 0777 is just decimal.
    LHAT_TEST("a leading zero does not mean octal");
    scan_text(&s, "0777");
    LHAT_CHECK_EQ_INT(s.tokens[0].v.integer.value, 777);
    scan_dispose(&s);

    LHAT_TEST("integer overflow is reported");
    scan_text(&s, "99999999999999999999999");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_INTEGER_OVERFLOW);
    scan_dispose(&s);
}

static void test_lexical_hazards(void)
{
    Scan s;

    // Section 10.1. A naive scanner reads ".1.1" as the float 1.1.
    LHAT_TEST("10.1 a.1.1 is a chain of integer keys");
    scan_text(&s, "a.1.1");
    LHAT_CHECK_EQ_INT(token_count(&s), 5);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_DOT), "expected .");
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_INT);
    LHAT_CHECK_EQ_INT(s.tokens[2].v.integer.value, 1);
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_DOT), "expected .");
    LHAT_CHECK_EQ_INT(s.tokens[4].kind, LHAT_TOKEN_INT);
    scan_dispose(&s);

    // Section 13.7: '...' must beat '..' at maximal munch, or a variadic
    // marker would scan as a concatenation followed by a dot.
    LHAT_TEST("13.7 ... beats ..");
    scan_text(&s, "...:number^ a..b");
    LHAT_CHECK(is_op(&s.tokens[0], LHAT_OP_ELLIPSIS), "expected ...");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_COLON), "expected :");
    LHAT_CHECK(is_op(&s.tokens[4], LHAT_OP_CONCAT), "expected ..");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    // Section 10.2. A naive scanner reads "1." as a float.
    LHAT_TEST("10.2 1..2 is integer concat integer");
    scan_text(&s, "1..2");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_INT);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_CONCAT), "expected ..");
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_INT);
    scan_dispose(&s);

    // Section 10.3 (Q7).
    LHAT_TEST("10.3 1to^3 is rejected");
    scan_text(&s, "1to^3");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK(s.lexer.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_IDENT_AFTER_NUMBER);
    scan_dispose(&s);

    LHAT_TEST("10.3 1 to^3 with a space is accepted");
    scan_text(&s, "1 to^3");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_INT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].v.hats, 1);
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_INT);
    scan_dispose(&s);

    // Section 4.5: Q7 removed the need to backtrack here, so this is an error
    // rather than "1" followed by "e^" followed by "3".
    LHAT_TEST("4.5 1e^3 is a malformed exponent");
    scan_text(&s, "1e^3");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_MALFORMED_EXPONENT);
    scan_dispose(&s);

    // Section 10.5.
    LHAT_TEST("10.5 \"\"\" wins over an empty string");
    scan_text(&s, "\"\"\"to the end");
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_STRING);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.string.kind, LHAT_STRING_LINE);
    {
        size_t length = 0;
        const char *bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
        LHAT_CHECK_EQ_STR(bytes, length, "to the end");
    }
    scan_dispose(&s);

    LHAT_TEST("10.5 a separated empty string still works");
    scan_text(&s, "\"\" \"abc\"");
    LHAT_CHECK_EQ_INT(token_count(&s), 2);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.string.length, 0);
    scan_dispose(&s);

    // Section 10.9: the sole newline-sensitive rule in the language.
    LHAT_TEST("10.9 a call paren on the same line is not flagged");
    scan_text(&s, "foo(a)");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_LPAREN), "expected (");
    LHAT_CHECK(!s.tokens[1].preceded_by_newline, "( should not be flagged");
    scan_dispose(&s);

    LHAT_TEST("10.9 a paren after a newline is flagged");
    scan_text(&s, "a := b\n(f or^ g)()");
    {
        size_t paren = 0;
        for (size_t i = 0; i < s.count; i++) {
            if (is_op(&s.tokens[i], LHAT_OP_LPAREN)) {
                paren = i;
                break;
            }
        }
        LHAT_CHECK(paren > 0, "expected to find a (");
        LHAT_CHECK(s.tokens[paren].preceded_by_newline,
                   "( at the start of a line should be flagged");
    }
    scan_dispose(&s);

    LHAT_TEST("10.9 the first token of a file is flagged");
    scan_text(&s, "foo");
    LHAT_CHECK(s.tokens[0].preceded_by_newline, "first token should be flagged");
    scan_dispose(&s);

    LHAT_TEST("10.9 a multi-line string does not flag the next token");
    scan_text(&s, "\"line one\nline two\" x");
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK(!s.tokens[1].preceded_by_newline,
               "the newline is inside the string, not between tokens");
    scan_dispose(&s);
}

static void test_strings(void)
{
    Scan s;
    size_t length = 0;
    const char *bytes = NULL;

    LHAT_TEST("escape sequences");
    scan_text(&s, "\"a\\nb\\t\\\\\\\"\"");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_STRING);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "a\nb\t\\\"");
    scan_dispose(&s);

    LHAT_TEST("hex and unicode escapes");
    scan_text(&s, "\"\\x41\\u{3042}\"");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "A\xE3\x81\x82");
    scan_dispose(&s);

    LHAT_TEST("an unknown escape is reported");
    scan_text(&s, "\"\\q\"");
    LHAT_CHECK(s.lexer.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_UNKNOWN_ESCAPE);
    scan_dispose(&s);

    LHAT_TEST("raw strings do not process escapes");
    scan_text(&s, "'a\\nb'");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "a\\nb");
    LHAT_CHECK_EQ_INT(s.tokens[0].v.string.kind, LHAT_STRING_RAW);
    scan_dispose(&s);

    // Section 5.2.
    LHAT_TEST("a doubled quote inside a raw string");
    scan_text(&s, "'it''s'");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "it's");
    scan_dispose(&s);

    LHAT_TEST("empty and single-quote raw strings");
    scan_text(&s, "'' ''''");
    LHAT_CHECK_EQ_INT(token_count(&s), 2);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.string.length, 0);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[1], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "'");
    scan_dispose(&s);

    LHAT_TEST("strings may span lines");
    scan_text(&s, "\"one\ntwo\"");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[0], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "one\ntwo");
    scan_dispose(&s);

    LHAT_TEST("an unterminated string is reported");
    scan_text(&s, "\"oops");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_UNTERMINATED_STRING);
    scan_dispose(&s);

}

// Section 5.4. The hole contents are scanned by the ordinary rules, so the
// checks below are mostly about the boundaries between the two modes.
static void test_interpolation(void)
{
    Scan s;
    size_t length = 0;
    const char *bytes = NULL;

    LHAT_TEST("text, hole, text");
    scan_text(&s, "$\"hi {name}!\"");
    LHAT_CHECK_EQ_INT(token_count(&s), 7);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_INTERP_BEGIN);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_INTERP_TEXT);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[1], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "hi ");
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_INTERP_EXPR_BEGIN);
    LHAT_CHECK_EQ_INT(s.tokens[3].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[4].kind, LHAT_TOKEN_INTERP_EXPR_END);
    LHAT_CHECK_EQ_INT(s.tokens[5].kind, LHAT_TOKEN_INTERP_TEXT);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[5], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "!");
    LHAT_CHECK_EQ_INT(s.tokens[6].kind, LHAT_TOKEN_INTERP_END);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    LHAT_TEST("an empty text segment is not emitted");
    scan_text(&s, "$\"{a}{b}\"");
    LHAT_CHECK_EQ_INT(token_count(&s), 8);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_INTERP_EXPR_BEGIN);
    scan_dispose(&s);

    // Q4: the format specifier follows a ':' and is raw text.
    LHAT_TEST("format specifier");
    scan_text(&s, "$\"{bar:2.4}\"");
    LHAT_CHECK_EQ_INT(token_count(&s), 6);
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[3].kind, LHAT_TOKEN_INTERP_FORMAT);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[3], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "2.4");
    LHAT_CHECK_EQ_INT(s.tokens[4].kind, LHAT_TOKEN_INTERP_EXPR_END);
    scan_dispose(&s);

    // The hole is real code, not text to be re-scanned later.
    LHAT_TEST("a hole contains ordinary tokens");
    scan_text(&s, "$\"{ a + f(1) }\"");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_ADD), "expected +");
    LHAT_CHECK(is_op(&s.tokens[5], LHAT_OP_LPAREN), "expected (");
    scan_dispose(&s);

    // The brace of a table literal must not close the hole.
    LHAT_TEST("braces inside a hole are counted");
    scan_text(&s, "$\"{ {a := 1} }\"");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    {
        size_t ends = 0;
        for (size_t i = 0; i < s.count; i++) {
            if (s.tokens[i].kind == LHAT_TOKEN_INTERP_EXPR_END) {
                ends++;
            }
        }
        LHAT_CHECK_EQ_INT(ends, 1);
        LHAT_CHECK_EQ_INT(s.tokens[s.count - 2].kind, LHAT_TOKEN_INTERP_END);
    }
    scan_dispose(&s);

    // A string inside a hole is scanned by the normal string rules.
    LHAT_TEST("a string literal inside a hole");
    scan_text(&s, "$\"{ f(\"x\") }\"");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(s.tokens[4].kind, LHAT_TOKEN_STRING);
    scan_dispose(&s);

    LHAT_TEST("nested interpolation");
    scan_text(&s, "$\"a{ $\"b{c}\" }d\"");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    {
        size_t begins = 0;
        size_t ends = 0;
        for (size_t i = 0; i < s.count; i++) {
            if (s.tokens[i].kind == LHAT_TOKEN_INTERP_BEGIN) {
                begins++;
            }
            if (s.tokens[i].kind == LHAT_TOKEN_INTERP_END) {
                ends++;
            }
        }
        LHAT_CHECK_EQ_INT(begins, 2);
        LHAT_CHECK_EQ_INT(ends, 2);
    }
    scan_dispose(&s);

    LHAT_TEST("doubled braces stand for a single brace");
    scan_text(&s, "$\"{{x}}\"");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[1], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "{x}");
    scan_dispose(&s);

    LHAT_TEST("escapes still work in the text segments");
    scan_text(&s, "$\"a\\nb\"");
    bytes = lhat_lexer_string(&s.lexer, &s.tokens[1], &length);
    LHAT_CHECK_EQ_STR(bytes, length, "a\nb");
    scan_dispose(&s);

    LHAT_TEST("an unterminated interpolated string is reported");
    scan_text(&s, "$\"oops");
    LHAT_CHECK(s.lexer.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_UNTERMINATED_STRING);
    scan_dispose(&s);

    // Memo.md L71: interpolation requires double quotes.
    LHAT_TEST("$'...' is rejected with a specific message");
    scan_text(&s, "$'no'");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code,
                      LHAT_ERR_INTERPOLATION_NEEDS_QUOTES);
    scan_dispose(&s);

    // Section 10.6: '$' followed by a name is still a scope specifier.
    LHAT_TEST("a scope specifier is unaffected");
    scan_text(&s, "$name");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_SCOPE);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);
}

static void test_comments(void)
{
    Scan s;

    // Section 6.1 (Q5).
    LHAT_TEST("hash starts a line comment");
    scan_text(&s, "a # this is ignored\nb");
    LHAT_CHECK_EQ_INT(token_count(&s), 2);
    LHAT_CHECK_EQ_STR(token_text(&s, 0), token_length(&s, 0), "a");
    LHAT_CHECK_EQ_STR(token_text(&s, 1), token_length(&s, 1), "b");
    LHAT_CHECK(s.tokens[1].preceded_by_newline, "b follows a newline");
    scan_dispose(&s);

    LHAT_TEST("a shebang line is just a comment");
    scan_text(&s, "#!/usr/bin/env lhat\nfoo");
    LHAT_CHECK_EQ_INT(token_count(&s), 1);
    LHAT_CHECK_EQ_STR(token_text(&s, 0), token_length(&s, 0), "foo");
    scan_dispose(&s);

    // Section 6.2.
    LHAT_TEST("block comments nest");
    scan_text(&s, "a #[ outer #[ inner ]# still outer ]# b");
    LHAT_CHECK_EQ_INT(token_count(&s), 2);
    LHAT_CHECK_EQ_STR(token_text(&s, 1), token_length(&s, 1), "b");
    scan_dispose(&s);

    LHAT_TEST("an unterminated block comment is reported");
    scan_text(&s, "a #[ oops");
    LHAT_CHECK(s.lexer.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code,
                      LHAT_ERR_UNTERMINATED_BLOCK_COMMENT);
    scan_dispose(&s);

    LHAT_TEST("// is floor division, not a comment");
    scan_text(&s, "7 // 2");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_FLOORDIV), "expected //");
    scan_dispose(&s);
}

static void test_operators(void)
{
    Scan s;

    LHAT_TEST("maximal munch on multi-character operators");
    scan_text(&s, ":= -> << ** // .. ?. ?( ?[ != =/ <= >= ??");
    LHAT_CHECK(is_op(&s.tokens[0], LHAT_OP_DEFINE), "expected :=");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_ARROW), "expected ->");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_REASSIGN), "expected <<");
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_POW), "expected **");
    LHAT_CHECK(is_op(&s.tokens[4], LHAT_OP_FLOORDIV), "expected //");
    LHAT_CHECK(is_op(&s.tokens[5], LHAT_OP_CONCAT), "expected ..");
    LHAT_CHECK(is_op(&s.tokens[6], LHAT_OP_NIL_DOT), "expected ?.");
    LHAT_CHECK(is_op(&s.tokens[7], LHAT_OP_NIL_CALL), "expected ?(");
    LHAT_CHECK(is_op(&s.tokens[8], LHAT_OP_NIL_INDEX), "expected ?[");
    LHAT_CHECK(is_op(&s.tokens[9], LHAT_OP_NE), "expected !=");
    LHAT_CHECK(is_op(&s.tokens[10], LHAT_OP_NE), "expected =/");
    LHAT_CHECK(is_op(&s.tokens[11], LHAT_OP_LE), "expected <=");
    LHAT_CHECK(is_op(&s.tokens[12], LHAT_OP_GE), "expected >=");
    LHAT_CHECK(is_op(&s.tokens[13], LHAT_OP_NIL_ELSE), "expected ??");
    scan_dispose(&s);

    // 7.4改: each compound spelling has to precede its plain operator, or
    // maximal munch would take the shorter one and leave a stray '='.
    LHAT_TEST("compound assignment operators");
    scan_text(&s, "+= -= *= /= %= //= **= ..=");
    LHAT_CHECK(is_op(&s.tokens[0], LHAT_OP_ADD_ASSIGN), "expected +=");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_SUB_ASSIGN), "expected -=");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_MUL_ASSIGN), "expected *=");
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_DIV_ASSIGN), "expected /=");
    LHAT_CHECK(is_op(&s.tokens[4], LHAT_OP_MOD_ASSIGN), "expected %%=");
    LHAT_CHECK(is_op(&s.tokens[5], LHAT_OP_FLOORDIV_ASSIGN), "expected //=");
    LHAT_CHECK(is_op(&s.tokens[6], LHAT_OP_POW_ASSIGN), "expected **=");
    LHAT_CHECK(is_op(&s.tokens[7], LHAT_OP_CONCAT_ASSIGN), "expected ..=");
    LHAT_CHECK_EQ_INT(token_count(&s), 8);
    scan_dispose(&s);

    // 02 の 11.7. The one member of the '?' family that is not a postfix
    // access, so it has to stay distinct from '?.' and from a bare '?'.
    LHAT_TEST("?? is distinct from the nil-propagating accesses");
    scan_text(&s, "t[k] ?? 0 a?.b ?? c");
    LHAT_CHECK(is_op(&s.tokens[4], LHAT_OP_NIL_ELSE), "expected ??");
    LHAT_CHECK(is_op(&s.tokens[7], LHAT_OP_NIL_DOT), "expected ?.");
    LHAT_CHECK(is_op(&s.tokens[9], LHAT_OP_NIL_ELSE), "expected ??");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    // '<<' must win over '<', and must not be confused with '<='.
    LHAT_TEST("<< is distinct from < and <=");
    scan_text(&s, "a << b a < b a <= b");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_REASSIGN), "expected <<");
    LHAT_CHECK(is_op(&s.tokens[4], LHAT_OP_LT), "expected <");
    LHAT_CHECK(is_op(&s.tokens[7], LHAT_OP_LE), "expected <=");
    scan_dispose(&s);

    // Unlike the '<-' the memo considered, '<<' does not collide with a
    // comparison against a negative number.
    LHAT_TEST("a<-2 stays a comparison alongside <<");
    scan_text(&s, "a<-2 b << -2");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_LT), "expected <");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_SUB), "expected -");
    LHAT_CHECK(is_op(&s.tokens[5], LHAT_OP_REASSIGN), "expected <<");
    scan_dispose(&s);

    LHAT_TEST("-> separates arguments from the return value");
    scan_text(&s, "f^number^ -> string^;");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_ARROW), "expected ->");
    scan_dispose(&s);

    // Withdrawn, but still scanned so the parser can explain the change.
    LHAT_TEST(":: is still recognised as one token");
    scan_text(&s, "f^number^ :: string^;");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_COLONCOLON), "expected ::");
    scan_dispose(&s);

    // Section 7.7: the '>>' in Memo.md is a prompt, not syntax, so it is not
    // an operator and scans as two separate '>' tokens.
    LHAT_TEST(">> is not an operator");
    scan_text(&s, "a >> b");
    LHAT_CHECK_EQ_INT(token_count(&s), 4);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_GT), "expected >");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_GT), "expected >");
    scan_dispose(&s);

    // Section 7.2: the preferred spellings are multi-byte UTF-8.
    LHAT_TEST("unicode comparison operators");
    scan_text(&s, "a \xE2\x89\xA0 b \xE2\x89\xA6 c \xE2\x89\xA7 d");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_NE), "expected the unicode form of !=");
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_LE), "expected the unicode form of <=");
    LHAT_CHECK(is_op(&s.tokens[5], LHAT_OP_GE), "expected the unicode form of >=");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    // Q10: U+2264 and U+2265 are accepted alongside U+2266 and U+2267.
    LHAT_TEST("both unicode spellings of <= and >=");
    scan_text(&s, "a \xE2\x89\xA4 b \xE2\x89\xA5 c");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_LE), "U+2264 must mean <=");
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_GE), "U+2265 must mean >=");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    // They must never be absorbed into an identifier, which would turn
    // "a \xE2\x89\xA4 b" into the two identifiers "a" and "\xE2\x89\xA4b".
    LHAT_TEST("comparison symbols never join an identifier");
    scan_text(&s, "a\xE2\x89\xA4""b");
    LHAT_CHECK_EQ_INT(token_count(&s), 3);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_LE), "expected <=");
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_IDENT);
    scan_dispose(&s);

    // Section 7.1: '=' compares, it does not assign, and '==' does not exist.
    LHAT_TEST("= is a single comparison operator");
    scan_text(&s, "a = b");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_EQ), "expected =");
    scan_dispose(&s);

    // Section 7.3 (Q2): '<-' is not a token, so this is a comparison against
    // a negative number.
    LHAT_TEST("a<-2 is a comparison, not a reassignment");
    scan_text(&s, "a<-2");
    LHAT_CHECK_EQ_INT(token_count(&s), 4);
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_LT), "expected <");
    LHAT_CHECK(is_op(&s.tokens[2], LHAT_OP_SUB), "expected -");
    LHAT_CHECK_EQ_INT(s.tokens[3].kind, LHAT_TOKEN_INT);
    scan_dispose(&s);

    LHAT_TEST("reassignment");
    scan_text(&s, "i << i + 1");
    LHAT_CHECK(is_op(&s.tokens[1], LHAT_OP_REASSIGN), "expected <<");
    scan_dispose(&s);

    // '|' is the type union operator. The lexer knows nothing of type
    // contexts and simply emits the token; the parser decides.
    LHAT_TEST("type union");
    scan_text(&s, "k as^number^|nil^");
    LHAT_CHECK_EQ_INT(token_count(&s), 5);
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK_EQ_INT(s.tokens[2].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK(is_op(&s.tokens[3], LHAT_OP_UNION), "expected |");
    LHAT_CHECK_EQ_INT(s.tokens[4].kind, LHAT_TOKEN_HAT_IDENT);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    scan_dispose(&s);

    LHAT_TEST("a lone question mark is an error");
    scan_text(&s, "a ? b");
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_LONE_QUESTION_MARK);
    scan_dispose(&s);
}

static void test_scope_specifiers(void)
{
    Scan s;

    // Section 8. '$' is the unit; a global table was given up, so nothing
    // shorter than it is left to name.
    LHAT_TEST("file and relative scopes");
    scan_text(&s, "$a $^c $^^^d");
    LHAT_CHECK_EQ_INT(token_count(&s), 6);
    LHAT_CHECK_EQ_INT(s.tokens[0].v.scope.kind, LHAT_SCOPE_FILE);
    LHAT_CHECK_EQ_INT(s.tokens[2].v.scope.kind, LHAT_SCOPE_RELATIVE);
    LHAT_CHECK_EQ_INT(s.tokens[2].v.scope.depth, 1);
    LHAT_CHECK_EQ_INT(s.tokens[4].v.scope.kind, LHAT_SCOPE_RELATIVE);
    LHAT_CHECK_EQ_INT(s.tokens[4].v.scope.depth, 3);
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_IDENT);
    scan_dispose(&s);

    // '$$' was the unit while '$' was a global. With the global gone the
    // doubled sigil is nothing at all -- the second '$' is not a name.
    LHAT_TEST("'$$' is no longer a specifier");
    scan_text(&s, "$$b");
    LHAT_CHECK(s.lexer.diagnostic_count > 0, "reported");
    if (s.lexer.diagnostic_count > 0) {
        // Named, so a reader is told which sigil moved rather than left to
        // read "a sigil with no name after it" about a '$' that has one.
        LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_SCOPE_DOUBLED);
    }
    scan_dispose(&s);

    LHAT_TEST("the sigil must be glued to the name");
    scan_text(&s, "$ a");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_SCOPE_WITHOUT_NAME);
    scan_dispose(&s);

    // What is glued has to be a name, so '$x' is the only spelling -- there
    // is no '$' standing alone with a member reached off it.
    LHAT_TEST("and '$.x' is not another way of writing it");
    scan_text(&s, "$.x");
    LHAT_CHECK_EQ_INT(s.tokens[0].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_SCOPE_WITHOUT_NAME);
    scan_dispose(&s);
}

static void test_positions(void)
{
    Scan s;

    LHAT_TEST("line and column tracking");
    scan_text(&s, "a\nbb ccc");
    LHAT_CHECK_EQ_INT(s.tokens[0].line, 1);
    LHAT_CHECK_EQ_INT(s.tokens[0].column, 1);
    LHAT_CHECK_EQ_INT(s.tokens[1].line, 2);
    LHAT_CHECK_EQ_INT(s.tokens[1].column, 1);
    LHAT_CHECK_EQ_INT(s.tokens[2].line, 2);
    LHAT_CHECK_EQ_INT(s.tokens[2].column, 4);
    scan_dispose(&s);

    // Section 11: columns count code points, not bytes, so a Japanese comment
    // does not shift the reported position of what follows it.
    LHAT_TEST("columns count code points, not bytes");
    scan_text(&s, "# \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nx");
    LHAT_CHECK_EQ_INT(s.tokens[0].line, 2);
    LHAT_CHECK_EQ_INT(s.tokens[0].column, 1);
    scan_dispose(&s);

    LHAT_TEST("crlf input keeps line numbers correct");
    scan_text(&s, "a\r\nb");
    LHAT_CHECK_EQ_INT(s.tokens[1].line, 2);
    scan_dispose(&s);

    LHAT_TEST("a full width space is rejected rather than absorbed");
    scan_text(&s, "a\xE3\x80\x80" "b");
    LHAT_CHECK_EQ_INT(s.tokens[1].kind, LHAT_TOKEN_ERROR);
    LHAT_CHECK_EQ_INT(s.lexer.diagnostics[0].code, LHAT_ERR_UNEXPECTED_CHARACTER);
    scan_dispose(&s);
}

static void test_realistic_snippet(void)
{
    Scan s;

    LHAT_TEST("a small program scans without diagnostics");
    scan_text(&s,
              "#!/usr/bin/env lhat\n"
              "# a small sample\n"
              "$Counter := {\n"
              "    value := 0,\n"
              "    bump := p^step -> number^ {\n"
              "        value << value + step\n"
              "        value\n"
              "    }\n"
              "}\n"
              "c := $Counter\n"
              "if^ c.value \xE2\x89\xA6 10 {\n"
              "    print('done')\n"
              "}\n");
    LHAT_CHECK_EQ_INT(s.lexer.diagnostic_count, 0);
    LHAT_CHECK(token_count(&s) > 30, "expected a decent number of tokens");
    scan_dispose(&s);
}

int main(void)
{
    test_identifiers();
    test_name_literals();
    test_numbers();
    test_lexical_hazards();
    test_strings();
    test_interpolation();
    test_comments();
    test_operators();
    test_scope_specifiers();
    test_positions();
    test_realistic_snippet();
    return lhat_test_report("test_lexer");
}
