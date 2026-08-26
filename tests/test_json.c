// L^ (lhat) -- tests for std.json.
//
// Two things are worth pinning. One is the rule json.h writes out -- which
// half of a table decides between an array and an object, and what each kind
// of value becomes -- since that rule is the whole of what a caller has to
// hold in their head. The other is the round trip: a table encoded and read
// again is the table it was, which is the promise the pair of calls exists
// to make.
//
// The one place the trip does not close is null, and that is 04 の 11.3
// rather than a gap here: JSON's null and a key that was never set are the
// same thing in L^, so an array with a null in it comes back holed and
// encodes as an object. Asserted rather than left to be found.
//
// A run answers an integer or a string (stdlibutil.h), so the cases that ask
// a yes-or-no question answer 1 for yes, which is the shape test_thread.c
// already uses.

#include <stdio.h>
#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/json.h"

static const LhatTestRegister regs[] = {lhatstdlib_json_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

// Most cases are "encode this and look at the text", so the preamble is
// written once.
#define ENCODING(expression) \
    "import^ std.json\n"     \
    "return^ try^ std.json.encode(" expression ")\n"

static void test_writing(void)
{
    LHAT_TEST("the rule that decides between an array and an object");
    {
        // json.h: only the dense part, and not empty, is an array.
        LhatTestRan ran = run_source(ENCODING("{1, 2, 3}"));
        LHAT_CHECK_RAN_TEXT(ran, "[1,2,3]");
        lhat_test_ran_dispose(&ran);

        // An empty table has no half to read. It is written as an object,
        // since an empty table is more often a record about to be filled.
        ran = run_source(ENCODING("{}"));
        LHAT_CHECK_RAN_TEXT(ran, "{}");
        lhat_test_ran_dispose(&ran);

        // Both halves: an object, and the dense half took the keys "1"…"n".
        ran = run_source(ENCODING("{1, 2, a := 3}"));
        LHAT_CHECK_RAN_TEXT(ran, "{\"1\":1,\"2\":2,\"a\":3}");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what each kind of value becomes");
    {
        LhatTestRan ran = run_source(ENCODING("{true^, false^, 7}"));
        LHAT_CHECK_RAN_TEXT(ran, "[true,false,7]");
        lhat_test_ran_dispose(&ran);

        // 04 の 11.3: storing nil^ is how a key goes, so a table never holds
        // one -- an encode has no null to write. The third slot here is not
        // a null but nothing at all, which breaks the dense half and makes
        // the whole an object.
        ran = run_source(ENCODING("{true^, false^, nil^, 7}"));
        LHAT_CHECK_RAN_TEXT(ran, "{\"1\":true,\"2\":false,\"4\":7}");
        lhat_test_ran_dispose(&ran);

        // 14.8's two representations: an integer keeps its shape, and a real
        // is written so that reading it back answers the same real. %g, which
        // 14.17 writes a number for a reader with, would not come back.
        ran = run_source(ENCODING("{0.1, 1.5, -2}"));
        LHAT_CHECK_RAN_TEXT(ran, "[0.1,1.5,-2]");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a string is quoted, escaped, and left as the UTF-8 it is");
    {
        LhatTestRan ran = run_source(ENCODING("{\"a\\\"b\\\\c\"}"));
        LHAT_CHECK_RAN_TEXT(ran, "[\"a\\\"b\\\\c\"]");
        lhat_test_ran_dispose(&ran);

        ran = run_source(ENCODING("{\"one\\ntwo\\ttab\"}"));
        LHAT_CHECK_RAN_TEXT(ran, "[\"one\\ntwo\\ttab\"]");
        lhat_test_ran_dispose(&ran);

        // Text above a space goes down as itself rather than as \u escapes:
        // the result is still JSON, and a great deal easier to read.
        ran = run_source(ENCODING("{\"日本\"}"));
        LHAT_CHECK_RAN_TEXT(ran, "[\"日本\"]");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what JSON cannot carry is refused rather than guessed at");
    {
        // A closure has no spelling in JSON, and neither has a key that is
        // not a string or an integer.
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ answered = std.json.encode({ f := p^ { } })\n"
            "if^ answered isa^ std.json.JsonError.Unsupported { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);

        ran = run_source(
            "import^ std.json\n"
            "let^ answered = std.json.encode({ [true^] = 1 })\n"
            "if^ answered isa^ std.json.JsonError.Unsupported { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // json.h: the depth is the whole of what is counted, so a table holding
    // itself reaches the same answer a genuinely deep one does. Nothing
    // hangs, which is the point.
    LHAT_TEST("a table holding itself is too deep rather than for ever");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "var^ t = { }\n"
            "t[\"self\"] := t\n"
            "let^ answered = std.json.encode(t)\n"
            "if^ answered isa^ std.json.JsonError.TooDeep { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_reading(void)
{
    LHAT_TEST("an object and an array come back as one table each");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ t = try^ std.json.decode("
            "\"{\\\"a\\\": 1, \\\"b\\\": [10, 20]}\")\n"
            "return^ t[\"a\"] + t[\"b\"][2]\n");
        LHAT_CHECK_RAN_INTEGER(ran, 21);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a number is an integer where JSON wrote one");
    {
        // 14.8: no fraction and no exponent, and small enough to hold.
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ t = try^ std.json.decode(\"[3, 3.5, 3e2, -4]\")\n"
            "if^ (t[1] = 3) and^ (t[2] = 3.5) and^ (t[3] = 300.0)\n"
            "   and^ (t[4] = -4) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("an escape, and a surrogate pair joined into one code point");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ t = try^ std.json.decode(\"[\\\"a\\\\tb\\\"]\")\n"
            "return^ t[1]\n");
        LHAT_CHECK_RAN_TEXT(ran, "a\tb");
        lhat_test_ran_dispose(&ran);

        // 日 is 日; the pair spells U+1F600, which one escape cannot.
        ran = run_source(
            "import^ std.json\n"
            "let^ t = try^ std.json.decode("
            "\"[\\\"\\\\u65e5\\\\ud83d\\\\ude00\\\"]\")\n"
            "return^ t[1]\n");
        LHAT_CHECK_RAN_TEXT(ran, "日\xF0\x9F\x98\x80");
        lhat_test_ran_dispose(&ran);
    }

    // 04 の 11.3: null is nothing, and nothing is what an unset key holds.
    LHAT_TEST("null puts no key, so an array with one comes back holed");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ t = try^ std.json.decode("
            "\"{\\\"a\\\": null, \\\"b\\\": 1}\")\n"
            "if^ (t[\"a\"] is^ nil^) and^ (t[\"b\"] = 1) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);

        // And the hole is why this one does not close: the table is no
        // longer only a dense part, so it writes as an object.
        ran = run_source(
            "import^ std.json\n"
            "return^ try^ std.json.encode("
            "try^ std.json.decode(\"[1,null,3]\"))\n");
        LHAT_CHECK_RAN_TEXT(ran, "{\"1\":1,\"3\":3}");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what is not JSON, and what is JSON but not a table");
    {
        // Written as they appear inside the L^ string literal below.
        static const char *const refused[] = {
            "{",                  // an object with no end
            "[1,]",               // nothing where a value was promised
            "{\\\"a\\\" 1}",      // a key with no value
            "[1] [2]",            // more text after the value
            "5",                  // JSON, but the top of it is not a table
            "\\\"text\\\"",       // the same
            "null",               // and the same again
        };
        for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
            char source[256];
            snprintf(source, sizeof source,
                     "import^ std.json\n"
                     "let^ answered = std.json.decode(\"%s\")\n"
                     "if^ answered isa^ std.json.JsonError.BadText "
                     "{ return^ 1 }\n"
                     "return^ 0\n",
                     refused[i]);
            LhatTestRan ran = run_source(source);
            LHAT_CHECK_RAN_INTEGER(ran, 1);
            lhat_test_ran_dispose(&ran);
        }
    }
}

static void test_round_trip(void)
{
    // The promise the pair exists to make, over one value of every kind
    // that has a spelling.
    LHAT_TEST("a table encoded and read again is the table it was");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ was = {\n"
            "  name := \"日 \\\"quoted\\\" and \\t tabbed\",\n"
            "  n := -42,\n"
            "  r := 0.1,\n"
            "  yes := true^,\n"
            "  no := false^,\n"
            "  list := {1, 2, {3, 4}},\n"
            "  nested := { deep := { deeper := \"end\" } },\n"
            "}\n"
            "let^ back = try^ std.json.decode(try^ std.json.encode(was))\n"
            "if^ (back[\"name\"] = was.name) and^ (back[\"n\"] = -42)\n"
            "   and^ (back[\"r\"] = 0.1) and^ (back[\"yes\"] = true^)\n"
            "   and^ (back[\"no\"] = false^)\n"
            "   and^ (back[\"list\"][3][2] = 4)\n"
            "   and^ (back[\"nested\"][\"deep\"][\"deeper\"] = \"end\")\n"
            "   { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Encoding what came back has to answer the very same text. The walk
    // over a table is in a fixed order, so this is a promise rather than a
    // coincidence of how the table happened to be built.
    LHAT_TEST("and encoding what came back writes the same text");
    {
        LhatTestRan ran = run_source(
            "import^ std.json\n"
            "let^ once = try^ std.json.encode({ a := 1, b := {2, 3} })\n"
            "let^ twice = try^ std.json.encode(try^ std.json.decode(once))\n"
            "if^ once = twice { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_signatures(void)
{
    // 03 の 3.1: encode takes a table, so what is not one is refused before
    // anything runs rather than answered Unsupported while it does.
    LHAT_TEST("encode takes a table and nothing else");
    {
        LHAT_CHECK(checks("import^ std.json\n"
                          "let^ t = try^ std.json.encode({ a := 1 })\n"),
                   "a table is what it asks for");
        LHAT_CHECK(!checks("import^ std.json\n"
                           "let^ t = try^ std.json.encode(5)\n"),
                   "and a number is refused by the checker");
    }

    // What decode answers promises nothing about its shape, so a name read
    // off it is refused and an index is not.
    LHAT_TEST("decode answers a table that promises nothing");
    {
        LHAT_CHECK(checks("import^ std.json\n"
                          "let^ t = try^ std.json.decode(\"{}\")\n"
                          "let^ n = t[\"a\"]\n"),
                   "read by index");
        LHAT_CHECK(!checks("import^ std.json\n"
                           "let^ t = try^ std.json.decode(\"{}\")\n"
                           "let^ n = t.a\n"),
                   "and not by name");
    }
}

int main(void)
{
    test_writing();
    test_reading();
    test_round_trip();
    test_signatures();
    return lhat_test_report("test_json");
}
