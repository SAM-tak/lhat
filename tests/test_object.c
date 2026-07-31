// L^ (lhat) -- tests for the heap values: strings and tables.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. What is pinned here is the behaviour 02 の 14 章 and 04 の 11.3
// ask for, not the split between the dense part and the hash part, which is
// an implementation choice free to change.

#include <string.h>

#include "object.h"
#include "testutil.h"

static LhatValue string_value(LhatObject **owner, const char *text)
{
    LhatString *string = lhat_string_new(owner, text, strlen(text));
    return lhat_object((LhatObject *)string);
}

static void test_strings(void)
{
    LhatObject *owner = NULL;

    LHAT_TEST("a string keeps its bytes and a terminator");
    {
        LhatString *s = lhat_string_new(&owner, "hello", 5);
        LHAT_CHECK_EQ_INT(s->length, 5);
        LHAT_CHECK(strcmp(s->text, "hello") == 0, "the bytes came through");
    }

    LHAT_TEST("an empty string is a string");
    {
        LhatString *s = lhat_string_new(&owner, "", 0);
        LHAT_CHECK_EQ_INT(s->length, 0);
        LHAT_CHECK(s->text[0] == '\0', "terminated");
    }

    LHAT_TEST("a string may hold a zero byte");
    {
        LhatString *s = lhat_string_new(&owner, "a\0b", 3);
        LHAT_CHECK_EQ_INT(s->length, 3);
        LHAT_CHECK(memcmp(s->text, "a\0b", 3) == 0, "all three bytes");
    }

    // 02 の 11.6: a string is what it says, so two copies are equal.
    LHAT_TEST("two strings spelling the same thing are equal");
    {
        LhatValue a = string_value(&owner, "foo");
        LhatValue b = string_value(&owner, "foo");
        LHAT_CHECK(lhat_as_object(a) != lhat_as_object(b), "two objects");
        LHAT_CHECK(lhat_value_equal(a, b), "one value");
    }

    LHAT_TEST("different strings are not equal");
    {
        LhatValue a = string_value(&owner, "foo");
        LhatValue b = string_value(&owner, "bar");
        LHAT_CHECK(!lhat_value_equal(a, b), "not equal");
    }

    LHAT_TEST("a string is not equal to a number");
    {
        LhatValue a = string_value(&owner, "1");
        LHAT_CHECK(!lhat_value_equal(a, lhat_integer(1)), "not equal");
    }

    lhat_object_free_all(&owner);
    LHAT_CHECK(owner == NULL, "the owner is emptied");
}

// 04 の 11.3: a table is a mapping. There is no such thing as out of range,
// and a key that is not there answers nil^.
static void test_table_basics(void)
{
    LhatObject *owner = NULL;
    LhatTable *t = lhat_table_new(&owner);
    bool refused = false;

    LHAT_TEST("a missing key answers nil^, whatever it is");
    {
        LHAT_CHECK(lhat_is_nil(lhat_table_get(t, lhat_integer(1))), "integer");
        LHAT_CHECK(lhat_is_nil(lhat_table_get(t, lhat_integer(-5))), "negative");
        LHAT_CHECK(lhat_is_nil(lhat_table_get(t, string_value(&owner, "x"))),
                   "string");
        LHAT_CHECK(lhat_is_nil(lhat_table_get(t, lhat_bool(true))), "bool");
    }

    LHAT_TEST("what was put in comes back out");
    {
        LHAT_CHECK(lhat_table_set(t, lhat_integer(1), lhat_integer(10), &refused),
                   "stored");
        LHAT_CHECK(!refused, "accepted");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_integer(1))), 10);
    }

    LHAT_TEST("a key is found by what it spells, not which copy it is");
    {
        lhat_table_set(t, string_value(&owner, "foo"), lhat_integer(7), &refused);
        LhatValue other = string_value(&owner, "foo");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, other)), 7);
    }

    // 02 の 14.8: number^ is one type, so the representation of a key cannot
    // decide which key it is.
    LHAT_TEST("2 and 2.0 are one key");
    {
        lhat_table_set(t, lhat_real(2.0), lhat_integer(20), &refused);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_integer(2))), 20);
        lhat_table_set(t, lhat_integer(2), lhat_integer(21), &refused);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_real(2.0))), 21);
    }

    LHAT_TEST("a key that is not a number keeps its own place");
    {
        lhat_table_set(t, lhat_bool(true), lhat_integer(1), &refused);
        lhat_table_set(t, lhat_bool(false), lhat_integer(2), &refused);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_bool(true))), 1);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_bool(false))), 2);
    }

    // 11.3 again: storing nil^ is how a key stops being there, since "absent"
    // and "nil^" are the one answer.
    LHAT_TEST("storing nil^ removes the key");
    {
        lhat_table_set(t, string_value(&owner, "gone"), lhat_integer(1), &refused);
        lhat_table_set(t, string_value(&owner, "gone"), lhat_nil(), &refused);
        LHAT_CHECK(lhat_is_nil(lhat_table_get(t, string_value(&owner, "gone"))),
                   "not there");
    }

    LHAT_TEST("nil^ cannot be a key");
    {
        LHAT_CHECK(lhat_table_set(t, lhat_nil(), lhat_integer(1), &refused),
                   "did not fail");
        LHAT_CHECK(refused, "refused");
    }

    lhat_object_free_all(&owner);
}

static void test_table_growth(void)
{
    LhatObject *owner = NULL;
    bool refused = false;

    // Enough to force the hash part to grow several times.
    LHAT_TEST("many keys all stay reachable");
    {
        LhatTable *t = lhat_table_new(&owner);
        for (int64_t i = 1; i <= 500; i++) {
            lhat_table_set(t, lhat_integer(i * 7919), lhat_integer(i), &refused);
        }
        bool all = true;
        for (int64_t i = 1; i <= 500; i++) {
            LhatValue v = lhat_table_get(t, lhat_integer(i * 7919));
            if (!lhat_is_integer(v) || lhat_as_integer(v) != i) {
                all = false;
            }
        }
        LHAT_CHECK(all, "every key came back");
    }

    LHAT_TEST("removing then adding again keeps the probe honest");
    {
        LhatTable *t = lhat_table_new(&owner);
        for (int64_t i = 0; i < 200; i++) {
            lhat_table_set(t, lhat_integer(i * 64), lhat_integer(i), &refused);
        }
        for (int64_t i = 0; i < 200; i += 2) {
            lhat_table_set(t, lhat_integer(i * 64), lhat_nil(), &refused);
        }
        bool all = true;
        for (int64_t i = 1; i < 200; i += 2) {
            LhatValue v = lhat_table_get(t, lhat_integer(i * 64));
            if (!lhat_is_integer(v) || lhat_as_integer(v) != i) {
                all = false;
            }
        }
        LHAT_CHECK(all, "the odd ones survived");

        bool none = true;
        for (int64_t i = 0; i < 200; i += 2) {
            if (!lhat_is_nil(lhat_table_get(t, lhat_integer(i * 64)))) {
                none = false;
            }
        }
        LHAT_CHECK(none, "the even ones went");
    }

    // The keys 1, 2, 3 ... are what a sequence is made of, and they are worth
    // holding densely however they arrive.
    LHAT_TEST("a run of keys is counted whichever order it arrives in");
    {
        LhatTable *forwards = lhat_table_new(&owner);
        for (int64_t i = 1; i <= 20; i++) {
            lhat_table_set(forwards, lhat_integer(i), lhat_integer(i), &refused);
        }
        LHAT_CHECK_EQ_INT(lhat_table_length(forwards), 20);

        LhatTable *backwards = lhat_table_new(&owner);
        for (int64_t i = 20; i >= 1; i--) {
            lhat_table_set(backwards, lhat_integer(i), lhat_integer(i), &refused);
        }
        LHAT_CHECK_EQ_INT(lhat_table_length(backwards), 20);

        bool all = true;
        for (int64_t i = 1; i <= 20; i++) {
            LhatValue v = lhat_table_get(backwards, lhat_integer(i));
            if (!lhat_is_integer(v) || lhat_as_integer(v) != i) {
                all = false;
            }
        }
        LHAT_CHECK(all, "and every one of them reads back");
    }

    LHAT_TEST("a gap stops the run");
    {
        LhatTable *t = lhat_table_new(&owner);
        lhat_table_set(t, lhat_integer(1), lhat_integer(1), &refused);
        lhat_table_set(t, lhat_integer(2), lhat_integer(2), &refused);
        lhat_table_set(t, lhat_integer(4), lhat_integer(4), &refused);
        LHAT_CHECK_EQ_INT(lhat_table_length(t), 2);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_table_get(t, lhat_integer(4))), 4);
    }

    lhat_object_free_all(&owner);
}

int main(void)
{
    test_strings();
    test_table_basics();
    test_table_growth();
    return lhat_test_report("test_object");
}
