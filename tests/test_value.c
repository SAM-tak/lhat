// L^ (lhat) -- tests for the runtime value representation.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed with "02". What is pinned here is what 2.2 decided and what it
// decided against: an integer keeps 64 bits, and the two representations of
// number^ stay one type as 02 の 14.8 requires.

#include <math.h>
#include <string.h>

#include "object.h"
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

// 03 の 4 章: a prompt answers with a value, so one has to be writable down.
static void wrote(LhatValue value, const char *expected)
{
    char buffer[256];
    size_t needed = lhat_value_write(value, buffer, sizeof buffer);
    LHAT_CHECK_EQ_INT(needed, strlen(expected));
    LHAT_CHECK(strcmp(buffer, expected) == 0, expected);
}

static void test_writing(void)
{
    LhatHeap heap = { NULL, 0 };

    LHAT_TEST("the values that are not objects");
    wrote(lhat_nil(), "nil^");
    wrote(lhat_bool(true), "true^");
    wrote(lhat_bool(false), "false^");
    wrote(lhat_integer(42), "42");
    wrote(lhat_integer(-7), "-7");
    wrote(lhat_real(1.5), "1.5");

    // 14.8 makes one number type of two representations, so a real that
    // happens to be whole still says which one it is.
    LHAT_TEST("a whole real still reads as one");
    wrote(lhat_real(4.0), "4.0");
    wrote(lhat_real(-4.0), "-4.0");

    // What settles it is the spelling, not the value. One written with an
    // exponent already cannot be read back as an integer, and one too large
    // to be an integer cannot be asked whether it is one -- the cast that
    // would ask is undefined for exactly those.
    LHAT_TEST("and one written with an exponent takes nothing further");
    wrote(lhat_real(9223372036854775808.0), "9.22337e+18");
    wrote(lhat_real(-9223372036854775808.0), "-9.22337e+18");

    LHAT_TEST("inf is a word, so it takes nothing either");
    {
        // Built rather than written, since a literal division by zero is one
        // the compiler refuses to fold.
        volatile double zero = 0.0;
        wrote(lhat_real(1.0 / zero), "inf");
        wrote(lhat_real(-1.0 / zero), "-inf");
    }

    // Quoted, so that 1 and "1" read differently at a prompt.
    LHAT_TEST("a string is quoted, and its escapes written back");
    {
        LhatString *plain = lhat_string_new(&heap, "plain", 5);
        wrote(lhat_object((LhatObject *)plain), "\"plain\"");
        LhatString *tricky = lhat_string_new(&heap, "a\"b\\c\nd", 7);
        wrote(lhat_object((LhatObject *)tricky), "\"a\\\"b\\\\c\\nd\"");
    }

    // 14 章: the sequence half in order, then the keyed half. 14.14 makes a
    // key that spells a name the same key the name form reaches, so it is
    // written back the shorter way.
    LHAT_TEST("a table writes its sequence half in order");
    {
        LhatTable *t = lhat_table_new(&heap);
        bool refused = false;
        lhat_table_set(t, lhat_integer(1), lhat_integer(10), &refused);
        lhat_table_set(t, lhat_integer(2), lhat_integer(20), &refused);
        wrote(lhat_object((LhatObject *)t), "{ 10, 20 }");
    }

    LHAT_TEST("and an empty one says so");
    {
        LhatTable *t = lhat_table_new(&heap);
        wrote(lhat_object((LhatObject *)t), "{}");
    }

    LHAT_TEST("a key that spells a name is written as one");
    {
        LhatTable *t = lhat_table_new(&heap);
        LhatString *key = lhat_string_new(&heap, "a", 1);
        bool refused = false;
        lhat_table_set(t, lhat_object((LhatObject *)key), lhat_integer(1),
                       &refused);
        wrote(lhat_object((LhatObject *)t), "{ a := 1 }");
    }

    LHAT_TEST("and one that does not is bracketed");
    {
        LhatTable *t = lhat_table_new(&heap);
        LhatString *key = lhat_string_new(&heap, "a b", 3);
        bool refused = false;
        lhat_table_set(t, lhat_object((LhatObject *)key), lhat_integer(1),
                       &refused);
        wrote(lhat_object((LhatObject *)t), "{ [\"a b\"] := 1 }");
    }

    // A table may hold itself, which nothing forbids. The depth is what
    // stops the walk.
    LHAT_TEST("a table holding itself is cut off rather than followed");
    {
        LhatTable *t = lhat_table_new(&heap);
        bool refused = false;
        lhat_table_set(t, lhat_integer(1),
                       lhat_object((LhatObject *)t), &refused);
        char buffer[256];
        size_t needed = lhat_value_write(lhat_object((LhatObject *)t), buffer,
                                         sizeof buffer);
        LHAT_CHECK(needed < sizeof buffer, "the walk ended");
        LHAT_CHECK(strstr(buffer, "…") != NULL, "and said where");
    }

    // snprintf's contract: ask with no room and be told how much is wanted.
    LHAT_TEST("asking with no room still answers how much there is");
    {
        LHAT_CHECK_EQ_INT(lhat_value_write(lhat_integer(12345), NULL, 0), 5);
        char small[4];
        size_t needed = lhat_value_write(lhat_integer(12345), small,
                                         sizeof small);
        LHAT_CHECK_EQ_INT(needed, 5);
        LHAT_CHECK_EQ_INT(strlen(small), 3);  // and what fits is terminated
    }

    lhat_object_free_all(&heap);
}

int main(void)
{
    test_tags();
    test_integer_width();
    test_equality();
    test_shape();
    test_writing();
    return lhat_test_report("test_value");
}
