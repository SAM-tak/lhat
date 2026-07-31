// L^ (lhat) -- tests for the runtime value representation.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed with "02". What is pinned here is what 2.2 decided and what it
// decided against: an integer keeps 64 bits, and the two representations of
// number^ stay one type as 02 の 14.8 requires.

#include <math.h>
#include <string.h>

#include "testutil.h"
#include "value.h"

static void test_tags(void)
{
    LHAT_TEST("a value carries its own type");
    {
        // 2.1. Each answers only to its own question.
        LHAT_CHECK(lhat_is_nil(lhat_nil()), "nil^");
        LHAT_CHECK(lhat_is_bool(lhat_bool(true)), "bool^");
        LHAT_CHECK(lhat_is_integer(lhat_integer(1)), "an integer");
        LHAT_CHECK(lhat_is_real(lhat_real(1.0)), "a real");
        LHAT_CHECK(!lhat_is_integer(lhat_real(1.0)), "a real is not an integer");
        LHAT_CHECK(!lhat_is_nil(lhat_bool(false)), "false is not nil^");
    }

    LHAT_TEST("round trip");
    {
        LHAT_CHECK(lhat_as_bool(lhat_bool(true)), "true");
        LHAT_CHECK(!lhat_as_bool(lhat_bool(false)), "false");
        LHAT_CHECK(lhat_as_integer(lhat_integer(-42)) == -42, "an integer");
        LHAT_CHECK(lhat_as_real(lhat_real(0.5)) == 0.5, "a real");
    }

    // 02 の 14.8: one type, two representations. Both are number^.
    LHAT_TEST("both representations are number^");
    {
        LHAT_CHECK(lhat_is_number(lhat_integer(1)), "an integer is a number");
        LHAT_CHECK(lhat_is_number(lhat_real(1.0)), "a real is a number");
        LHAT_CHECK(!lhat_is_number(lhat_nil()), "nil^ is not");
        LHAT_CHECK(!lhat_is_number(lhat_bool(true)), "bool^ is not");
    }

    LHAT_TEST("an object carries its kind");
    {
        LhatObject header;
        header.kind = LHAT_OBJECT_TABLE;
        header.next = NULL;
        LhatValue v = lhat_object(&header);
        LHAT_CHECK(lhat_is_object(v), "an object");
        LHAT_CHECK(lhat_is_object_kind(v, LHAT_OBJECT_TABLE), "a table");
        LHAT_CHECK(!lhat_is_object_kind(v, LHAT_OBJECT_STRING), "not a string");
        LHAT_CHECK(lhat_as_object(v) == &header, "the same object");
        LHAT_CHECK(!lhat_is_object_kind(lhat_nil(), LHAT_OBJECT_TABLE),
                   "nil^ is no object");
    }
}

// 2.2: this is the reason NaN boxing was not taken. Its payload holds about
// 48 bits, which is not enough for these.
static void test_integer_width(void)
{
    LHAT_TEST("an integer keeps all 64 bits");
    {
        int64_t big = 9007199254740993;  // 2^53 + 1, the first a double loses
        LHAT_CHECK(lhat_as_integer(lhat_integer(big)) == big,
                   "2^53 + 1 survives");
        LHAT_CHECK((double)big == (double)(big - 1),
                   "and a double really cannot tell it from 2^53");

        int64_t largest = INT64_MAX;
        LHAT_CHECK(lhat_as_integer(lhat_integer(largest)) == largest,
                   "INT64_MAX survives");
        LHAT_CHECK(lhat_as_integer(lhat_integer(INT64_MIN)) == INT64_MIN,
                   "INT64_MIN survives");

        int64_t wider_than_a_pointer = (int64_t)1 << 60;
        LHAT_CHECK(lhat_as_integer(lhat_integer(wider_than_a_pointer)) ==
                       wider_than_a_pointer,
                   "wider than any pointer payload");
    }
}

// 02 の 11.6 で '=' が問うこと。
static void test_equality(void)
{
    LHAT_TEST("like for like");
    {
        LHAT_CHECK(lhat_value_equal(lhat_nil(), lhat_nil()), "nil^ = nil^");
        LHAT_CHECK(lhat_value_equal(lhat_bool(true), lhat_bool(true)), "true");
        LHAT_CHECK(!lhat_value_equal(lhat_bool(true), lhat_bool(false)), "differ");
        LHAT_CHECK(!lhat_value_equal(lhat_nil(), lhat_bool(false)),
                   "nil^ is not false");
        LHAT_CHECK(lhat_value_equal(lhat_integer(7), lhat_integer(7)), "7 = 7");
        LHAT_CHECK(!lhat_value_equal(lhat_integer(7), lhat_integer(8)), "7 ≠ 8");
    }

    // 14.8 makes number^ one type, so which representation a value happens to
    // have must not show through here.
    LHAT_TEST("the two representations compare as one type");
    {
        LHAT_CHECK(lhat_value_equal(lhat_integer(1), lhat_real(1.0)), "1 = 1.0");
        LHAT_CHECK(lhat_value_equal(lhat_real(1.0), lhat_integer(1)), "1.0 = 1");
        LHAT_CHECK(!lhat_value_equal(lhat_integer(1), lhat_real(1.5)), "1 ≠ 1.5");
    }

    // Past 2^53 a double names fewer integers than an int64 does, so the
    // comparison has to go back through the integer rather than widening it.
    LHAT_TEST("a large integer is not equal to a nearby double");
    {
        int64_t big = 9007199254740993;  // 2^53 + 1
        LHAT_CHECK(!lhat_value_equal(lhat_integer(big), lhat_real(9007199254740992.0)),
                   "2^53 + 1 is not 2^53");
        LHAT_CHECK(lhat_value_equal(lhat_integer(9007199254740992),
                                    lhat_real(9007199254740992.0)),
                   "2^53 itself still is");
    }

    LHAT_TEST("an object compares by identity");
    {
        LhatObject a;
        LhatObject b;
        a.kind = b.kind = LHAT_OBJECT_TABLE;
        a.next = b.next = NULL;
        LHAT_CHECK(lhat_value_equal(lhat_object(&a), lhat_object(&a)), "itself");
        LHAT_CHECK(!lhat_value_equal(lhat_object(&a), lhat_object(&b)),
                   "another object of the same kind");
    }
}

// 2.2: the whole point of going through accessors is that this can change.
static void test_shape(void)
{
    LHAT_TEST("a value is one word plus a tag");
    {
        LHAT_CHECK(sizeof(LhatValue) <= 16,
                   "a tagged union should not grow past two words");
        LHAT_CHECK(sizeof(((LhatValue *)0)->as) == 8,
                   "the payload is one word");
    }
}

int main(void)
{
    test_tags();
    test_integer_width();
    test_equality();
    test_shape();
    return lhat_test_report("test_value");
}
