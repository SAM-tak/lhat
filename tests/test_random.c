// L^ (lhat) -- tests for std.random.
//
// A generator has no answer that can be written down in advance, so what is
// pinned here is what holds whatever the stream produces: the same seed gives
// the same sequence, a different one does not, next() stays inside [0, 1) and
// range() inside [lo, hi]. The bits themselves are deliberately not asserted
// on -- stdlib/random.c is free to stop being xorshift64* without these
// having to be rewritten.
//
// Everything is asked from inside L^ rather than by reading the numbers out:
// the comparisons are what the cases are about, and an L^ program can make
// all of them and answer 1 or 0. That also keeps every Random disposed
// exactly once, on the same line that finished with it -- disposing one twice
// by hand is a double free, since vm.c only stops the collector from
// disposing what a program already did.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/random.h"

static const LhatTestRegister regs[] = {lhatstdlib_random_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// Every case narrows what make answered before using it, so the preamble is
// the same each time: one generator, the body, and a dispose on the way out.
// `body` names it `r` and answers 1 for a pass.
#define WITH_RANDOM(seed, body)                     \
    "import^ std.random\n"                          \
    "let^ r = std.random.new(" seed ")\n"           \
    "if^ r fits^ std.random.Random {\n" body         \
    "}\n"                                           \
    "return^ 0 - 1\n"

// Two generators of the same seed, for the cases that ask whether two streams
// agree. `body` names them `a` and `b`.
#define WITH_TWO(first, second, body)           \
    "import^ std.random\n"                      \
    "let^ a = std.random.new(" first ")\n"      \
    "let^ b = std.random.new(" second ")\n"     \
    "if^ a fits^ std.random.Random {\n"          \
    "    if^ b fits^ std.random.Random {\n" body \
    "    }\n"                                   \
    "}\n"                                       \
    "return^ 0 - 1\n"

static void test_make(void)
{
    LHAT_TEST("make answers a Random");
    {
        LhatTestRan ran = run_source(WITH_RANDOM("12345",
                                                 "    r.dispose()\n"
                                                 "    return^ 1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // A seed of 0 asks for one seeded from the clock, so this is the one case
    // whose stream cannot be asserted on at all -- only that the branch
    // answers a Random like any other.
    LHAT_TEST("a seed of 0 answers a Random too");
    {
        LhatTestRan ran = run_source(WITH_RANDOM("0",
                                                 "    r.dispose()\n"
                                                 "    return^ 1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_sequences(void)
{
    LHAT_TEST("two generators of one seed walk the same sequence");
    {
        LhatTestRan ran = run_source(
            WITH_TWO("2024", "2024",
                     "        var^ same = 1\n"
                     "        for^ i from^ 1 to^ 16 {\n"
                     "            if^ a.next() != b.next() { same := 0 }\n"
                     "        }\n"
                     "        a.dispose()\n"
                     "        b.dispose()\n"
                     "        return^ same\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("two seeds that differ do not");
    {
        LhatTestRan ran = run_source(
            WITH_TWO("1", "2",
                     "        var^ differed = 0\n"
                     "        for^ i from^ 1 to^ 16 {\n"
                     "            if^ a.next() != b.next() { differed := 1 }\n"
                     "        }\n"
                     "        a.dispose()\n"
                     "        b.dispose()\n"
                     "        return^ differed\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // reseed puts a generator back where make of that seed starts -- the two
    // are the same call underneath for any seed but 0, which only make treats
    // as "ask the clock".
    LHAT_TEST("reseed puts a stream where make of that seed would start");
    {
        LhatTestRan ran = run_source(
            WITH_TWO("31", "4",
                     "        var^ burn = 0\n"
                     "        for^ i from^ 1 to^ 5 { burn := burn + b.next() }\n"
                     "        b.reseed(31)\n"
                     "        var^ same = 1\n"
                     "        for^ i from^ 1 to^ 8 {\n"
                     "            if^ a.next() != b.next() { same := 0 }\n"
                     "        }\n"
                     "        a.dispose()\n"
                     "        b.dispose()\n"
                     "        return^ same\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_next(void)
{
    LHAT_TEST("next stays inside [0, 1)");
    {
        LhatTestRan ran = run_source(
            WITH_RANDOM("7",
                        "    var^ inside = 1\n"
                        "    for^ i from^ 1 to^ 400 {\n"
                        "        let^ x = r.next()\n"
                        "        if^ x < 0 { inside := 0 }\n"
                        "        if^ x >= 1 { inside := 0 }\n"
                        "    }\n"
                        "    r.dispose()\n"
                        "    return^ inside\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_range(void)
{
    // Both ends inclusive, so a span of three has to produce its lowest and
    // its highest as well as staying between them -- 400 draws over three
    // values is what makes missing either one a failure rather than luck.
    LHAT_TEST("range stays inside [lo, hi] and reaches both ends");
    {
        LhatTestRan ran = run_source(
            WITH_RANDOM("99",
                        "    var^ inside = 1\n"
                        "    var^ sawLow = 0\n"
                        "    var^ sawHigh = 0\n"
                        "    for^ i from^ 1 to^ 400 {\n"
                        "        let^ n = r.range(1, 3)\n"
                        "        if^ n < 1 { inside := 0 }\n"
                        "        if^ n > 3 { inside := 0 }\n"
                        "        if^ n = 1 { sawLow := 1 }\n"
                        "        if^ n = 3 { sawHigh := 1 }\n"
                        "    }\n"
                        "    r.dispose()\n"
                        "    if^ inside = 1 and^ sawLow = 1 and^ sawHigh = 1 {\n"
                        "        return^ 1\n"
                        "    }\n"
                        "    return^ 0\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // An empty or backwards span answers lo rather than an error, and takes
    // nothing from the stream to do it -- which is what the second generator
    // is for: a draw spent here would put the two out of step forever after.
    LHAT_TEST("a span with nothing in it answers lo and spends no draw");
    {
        LhatTestRan ran = run_source(
            WITH_TWO("77", "77",
                     "        var^ answered = 0\n"
                     "        if^ a.range(5, 5) = 5 { answered := answered + 1 }\n"
                     "        if^ a.range(9, 2) = 9 { answered := answered + 1 }\n"
                     "        var^ same = 1\n"
                     "        for^ i from^ 1 to^ 8 {\n"
                     "            if^ a.next() != b.next() { same := 0 }\n"
                     "        }\n"
                     "        a.dispose()\n"
                     "        b.dispose()\n"
                     "        if^ answered = 2 and^ same = 1 { return^ 1 }\n"
                     "        return^ 0\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_make();
    test_sequences();
    test_next();
    test_range();
    return lhat_test_report("test_random");
}
