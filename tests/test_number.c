// L^ (lhat) -- tests for the numeric literal grammar.
//
// Section numbers refer to DesignDocuments/01-lexical-structure.md. The lexer
// reads this grammar and so does 02 の 14.17改2's tonumber, which is why it is
// a module of its own -- test_lexer.c pins what a lexer adds around it
// (spans, diagnostics, 10.3's rule about what may follow), and this pins the
// grammar itself and the whole-text reading tonumber asks for.

#include <stdint.h>

#include "number.h"
#include "testutil.h"

// Scanning one literal off the front of a longer text, which is what the
// lexer does with it.
static void test_literal(void)
{
    LhatNumberLiteral n;

    LHAT_TEST("a literal answers its value, its kind and its length");
    LHAT_CHECK(lhat_number_literal("123", 3, false, &n), "read");
    LHAT_CHECK_EQ_INT(n.kind, LHAT_NUMBER_INTEGER);
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 123);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 3);
    LHAT_CHECK_EQ_INT(n.base, 10);

    LHAT_CHECK(lhat_number_literal("3.5", 3, false, &n), "read");
    LHAT_CHECK_EQ_INT(n.kind, LHAT_NUMBER_REAL);
    LHAT_CHECK(n.real == 3.5, "value");

    LHAT_TEST("it stops where the literal stops");
    LHAT_CHECK(lhat_number_literal("12+3", 4, false, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 12);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 2);

    // 10.2: the fraction is taken only when a digit follows the '.', so
    // "1..2" scans as INT CONCAT INT.
    LHAT_CHECK(lhat_number_literal("1..2", 4, false, &n), "read");
    LHAT_CHECK_EQ_INT(n.kind, LHAT_NUMBER_INTEGER);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 1);

    LHAT_TEST("the bases of 10.1");
    LHAT_CHECK(lhat_number_literal("0xff", 4, false, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 255);
    LHAT_CHECK_EQ_INT(n.base, 16);

    LHAT_CHECK(lhat_number_literal("0b1010", 6, false, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 10);
    LHAT_CHECK_EQ_INT(n.base, 2);

    LHAT_CHECK(lhat_number_literal("0o777", 5, false, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 511);
    LHAT_CHECK_EQ_INT(n.base, 8);

    LHAT_TEST("'_' between two digits, and nowhere else");
    LHAT_CHECK(lhat_number_literal("1_000", 5, false, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 1000);

    LHAT_CHECK(!lhat_number_literal("1_", 2, false, &n), "trailing");
    LHAT_CHECK_EQ_INT(n.status, LHAT_NUMBER_MALFORMED);
    LHAT_CHECK(!lhat_number_literal("_1", 2, false, &n), "leading");

    LHAT_TEST("an exponent makes a real of it");
    LHAT_CHECK(lhat_number_literal("1e3", 3, false, &n), "read");
    LHAT_CHECK_EQ_INT(n.kind, LHAT_NUMBER_REAL);
    LHAT_CHECK(n.real == 1000.0, "value");

    LHAT_CHECK(lhat_number_literal("1e-3", 4, false, &n), "read");
    LHAT_CHECK(n.real == 0.001, "value");

    // 4.5: no backtracking, so a malformed exponent is an error rather than
    // an integer with a word after it -- and the 'e' stays consumed.
    LHAT_TEST("and one with no digits after it is its own error");
    LHAT_CHECK(!lhat_number_literal("1e+", 3, false, &n), "refused");
    LHAT_CHECK_EQ_INT(n.status, LHAT_NUMBER_BAD_EXPONENT);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 2);

    LHAT_TEST("'0x' with no digits is malformed");
    LHAT_CHECK(!lhat_number_literal("0x", 2, false, &n), "refused");
    LHAT_CHECK_EQ_INT(n.status, LHAT_NUMBER_MALFORMED);

    LHAT_CHECK(!lhat_number_literal("abc", 3, false, &n), "no digits at all");
    LHAT_CHECK(!lhat_number_literal("", 0, false, &n), "nothing at all");

    LHAT_TEST("more than an integer literal can hold");
    LHAT_CHECK(!lhat_number_literal("99999999999999999999", 20, false, &n),
               "refused");
    LHAT_CHECK_EQ_INT(n.status, LHAT_NUMBER_OVERFLOW);

    // 10.1: immediately after a '.' the digits form an integer key and must
    // not swallow a further '.'.
    LHAT_TEST("after a '.' only decimal digits are taken");
    LHAT_CHECK(lhat_number_literal("1.5", 3, true, &n), "read");
    LHAT_CHECK_EQ_INT(n.kind, LHAT_NUMBER_INTEGER);
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 1);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 1);

    LHAT_CHECK(lhat_number_literal("0x1", 3, true, &n), "read");
    LHAT_CHECK_EQ_INT((int64_t)n.integer, 0);
    LHAT_CHECK_EQ_INT((int64_t)n.length, 1);
}

// 02 の 14.17改2: the whole text as one number^, which is what tonumber asks.
static void test_read(void)
{
    bool is_real = false;
    int64_t whole = 0;
    double real = 0.0;

    LHAT_TEST("a literal on its own is read");
    LHAT_CHECK(lhat_number_read("123", 3, &is_real, &whole, &real), "read");
    LHAT_CHECK(!is_real, "an integer");
    LHAT_CHECK_EQ_INT(whole, 123);

    LHAT_CHECK(lhat_number_read("0xff", 4, &is_real, &whole, &real), "read");
    LHAT_CHECK_EQ_INT(whole, 255);

    LHAT_CHECK(lhat_number_read("1_000", 5, &is_real, &whole, &real), "read");
    LHAT_CHECK_EQ_INT(whole, 1000);

    LHAT_CHECK(lhat_number_read("3.5", 3, &is_real, &whole, &real), "read");
    LHAT_CHECK(is_real, "a real");
    LHAT_CHECK(real == 3.5, "value");

    LHAT_TEST("whitespace around it is trimmed");
    LHAT_CHECK(lhat_number_read("  12\n", 5, &is_real, &whole, &real), "read");
    LHAT_CHECK_EQ_INT(whole, 12);

    // Section 10 has no sign in it: '-1' is 02 の 11 章's unary minus applied
    // to a literal, so the sign is read here rather than by the grammar.
    LHAT_TEST("a single sign in front is read");
    LHAT_CHECK(lhat_number_read("-5", 2, &is_real, &whole, &real), "read");
    LHAT_CHECK_EQ_INT(whole, -5);

    LHAT_CHECK(lhat_number_read("+5", 2, &is_real, &whole, &real), "read");
    LHAT_CHECK_EQ_INT(whole, 5);

    LHAT_CHECK(lhat_number_read("-2.5", 4, &is_real, &whole, &real), "read");
    LHAT_CHECK(real == -2.5, "value");

    LHAT_CHECK(!lhat_number_read("- 5", 3, &is_real, &whole, &real),
               "a gap after the sign");
    LHAT_CHECK(!lhat_number_read("--5", 3, &is_real, &whole, &real),
               "two signs");
    LHAT_CHECK(!lhat_number_read("-", 1, &is_real, &whole, &real),
               "a sign and nothing else");

    // The lexer skips trivia, so reading this through one would make a
    // comment part of a number. Nothing here skips anything.
    LHAT_TEST("nothing but the number may be there");
    LHAT_CHECK(!lhat_number_read("12 # hi", 7, &is_real, &whole, &real),
               "a comment beside it");
    LHAT_CHECK(!lhat_number_read("# hi\n12", 7, &is_real, &whole, &real),
               "or in front of it");
    LHAT_CHECK(!lhat_number_read("1 2", 3, &is_real, &whole, &real),
               "a second number");
    LHAT_CHECK(!lhat_number_read("12abc", 5, &is_real, &whole, &real),
               "a word stuck to it");
    LHAT_CHECK(!lhat_number_read("abc", 3, &is_real, &whole, &real),
               "no number at all");
    LHAT_CHECK(!lhat_number_read("", 0, &is_real, &whole, &real),
               "nothing at all");
    LHAT_CHECK(!lhat_number_read("   ", 3, &is_real, &whole, &real),
               "whitespace alone");
    LHAT_CHECK(!lhat_number_read("0x", 2, &is_real, &whole, &real),
               "a malformed literal");
    LHAT_CHECK(!lhat_number_read("1_", 2, &is_real, &whole, &real),
               "a misplaced separator");
    LHAT_CHECK(!lhat_number_read("12\0" "3", 4, &is_real, &whole, &real),
               "a NUL among the bytes");

    // The grammar carries the magnitude alone, so this is the one place that
    // holds both it and the sign -- and so the only one that can tell a
    // number^ from a run of digits naming none.
    LHAT_TEST("a magnitude a number^ cannot hold is not one");
    LHAT_CHECK(!lhat_number_read("99999999999999999999", 20, &is_real, &whole,
                                 &real), "past u64");
    LHAT_CHECK(!lhat_number_read("9223372036854775808", 19, &is_real, &whole,
                                 &real), "past i64");
    LHAT_CHECK(lhat_number_read("9223372036854775807", 19, &is_real, &whole,
                                &real), "the top");
    LHAT_CHECK_EQ_INT(whole, INT64_MAX);
    LHAT_CHECK(lhat_number_read("-9223372036854775808", 20, &is_real, &whole,
                                &real), "the floor");
    LHAT_CHECK_EQ_INT(whole, INT64_MIN);
    LHAT_CHECK(!lhat_number_read("-9223372036854775809", 20, &is_real, &whole,
                                 &real), "below it");
}

int main(void)
{
    test_literal();
    test_read();
    return lhat_test_report("test_number");
}
