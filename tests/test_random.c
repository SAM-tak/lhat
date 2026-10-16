// L^ (lhat) -- tests for std.random.
//
// A generator has no answer that can be written down in advance, so what is
// pinned here is what holds whatever the stream produces: the same seed gives
// the same sequence, a different one does not, next() stays inside [0, 1) and
// rangeint() inside [lo, hi]. Most cases are algorithm-independent; the targeted
// rejection and full-span cases pin xorshift64* draws to exercise those paths.
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

    LHAT_TEST("an omitted seed answers a Random");
    {
        LhatTestRan ran = run_source(WITH_RANDOM("",
                                                 "    r.dispose()\n"
                                                 "    return^ 1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Independent 64-bit OS seeds have a negligible chance of collision.
    LHAT_TEST("automatic seeds differ even for successive constructions");
    {
        LhatTestRan ran = run_source(WITH_TWO("", "",
            "        let^ first = a.save()\n"
            "        let^ second = b.save()\n"
            "        a.dispose()\n"
            "        b.dispose()\n"
            "        if^ first != 0 and^ second != 0 and^ first != second { return^ 1 }\n"
            "        return^ 0\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("explicit zero seeds and reseed zero are reproducible");
    {
        LhatTestRan ran = run_source(WITH_TWO("0", "0",
            "        let^ initial = a.save()\n"
            "        var^ same = 1\n"
            "        for^ i from^ 1 to^ 16 {\n"
            "            if^ a.next() != b.next() { same := 0 }\n"
            "        }\n"
            "        b.reseed(0)\n"
            "        if^ initial != b.save() { same := 0 }\n"
            "        a.dispose()\n"
            "        b.dispose()\n"
            "        return^ same\n"));
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

    // reseed puts a generator back where new of that seed starts.
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

static void test_state(void)
{
    LHAT_TEST("saved state restores a mixed stream on another generator");
    LhatTestRan ran = run_source(WITH_TWO("31", "4",
        "        var^ burn = a.next()\n"
        "        burn := try^a.rangeint(-20, 20)\n"
        "        let^ state = a.save()\n"
        "        var^ same = 1\n"
        "        if^ a.save() != state { same := 0 }\n"
        "        if^ b.load(state) != nil^ { same := 0 }\n"
        "        for^ i from^ 1 to^ 16 {\n"
        "            if^ a.next() != b.next() { same := 0 }\n"
        "            if^ try^a.rangeint(-100, 100) != try^b.rangeint(-100, 100) { same := 0 }\n"
        "        }\n"
        "        a.dispose()\n"
        "        b.dispose()\n"
        "        return^ same\n"));
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    const char *const valid[] = {"1", "-1", "(-9223372036854775807 - 1)",
                                "9223372036854775807", "8.0"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
        LHAT_TEST(valid[i]);
        char source[1024];
        snprintf(source, sizeof source, WITH_RANDOM("1",
            "    let^ state = %s\n"
            "    let^ result = r.load(state)\n"
            "    let^ saved = r.save()\n"
            "    r.dispose()\n"
            "    if^ result = nil^ and^ saved = state { return^ 1 }\n"
            "    return^ 0\n"), valid[i]);
        ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    const char *const invalid[] = {"0", "0.0", "0.5", "-1.5", "1e300",
                                  "9223372036854775808.0"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        LHAT_TEST("invalid state returns InvalidState without changing the stream");
        char source[1024];
        snprintf(source, sizeof source, WITH_RANDOM("1",
            "    let^ state = r.save()\n"
            "    let^ result = r.load(%s)\n"
            "    let^ saved = r.save()\n"
            "    r.dispose()\n"
            "    if^ result fits^ std.random.Error.InvalidState {\n"
            "        if^ saved = state { return^ 1 }\n"
            "    }\n"
            "    return^ 0\n"), invalid[i]);
        ran = run_source(source);
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

static void test_normal(void)
{
    LHAT_TEST("normal applies mean and deviation and replays saved state");
    LhatTestRan ran = run_source(WITH_TWO("314", "314",
        "        var^ same = 1\n"
        "        for^ i from^ 1 to^ 32 {\n"
        "            let^ z = a.normal(0, 1)\n"
        "            let^ x = b.normal(170, 6)\n"
        "            if^ z fits^ std.random.Error.InvalidArgument { return^ 0 }\n"
        "            if^ x != 170 + 6 * z { same := 0 }\n"
        "        }\n"
        "        let^ state = a.save()\n"
        "        let^ first = a.normal(-2, 0.5)\n"
        "        let^ following = a.next()\n"
        "        let^ loaded = a.load(state)\n"
        "        if^ first != a.normal(-2, 0.5) { same := 0 }\n"
        "        if^ following != a.next() { same := 0 }\n"
        "        a.dispose()\n"
        "        b.dispose()\n"
        "        return^ same\n"));
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("zero deviation returns the exact mean without advancing");
    ran = run_source(WITH_RANDOM("7",
        "    let^ state = r.save()\n"
        "    let^ x = r.normal(9223372036854775807, 0)\n"
        "    let^ y = r.normal(-3, -0.0)\n"
        "    let^ saved = r.save()\n"
        "    r.dispose()\n"
        "    if^ x = 9223372036854775807 and^ y = -3 and^ saved = state { return^ 1 }\n"
        "    return^ 0\n"));
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    const char *const invalid[] = {
        "0, -1", "number^.nan, 1", "number^.inf, 1",
        "-number^.inf, 1", "0, number^.nan", "0, number^.inf",
        "0, -number^.inf", "number^.nan, 0", "number^.inf, 0"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        LHAT_TEST("invalid normal arguments return InvalidArgument without advancing");
        char source[1024];
        snprintf(source, sizeof source, WITH_RANDOM("7",
            "    let^ state = r.save()\n"
            "    let^ result = r.normal(%s)\n"
            "    let^ saved = r.save()\n"
            "    r.dispose()\n"
            "    if^ result fits^ std.random.Error.InvalidArgument {\n"
            "        if^ state = saved { return^ 1 }\n"
            "    }\n"
            "    return^ 0\n"), invalid[i]);
        ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Fixed seed and broad tolerances catch broken transforms, not sampling noise.
    LHAT_TEST("standard normals have approximately unit variance and normal tails");
    ran = run_source(WITH_RANDOM("2026",
        "    var^ sum = 0\n"
        "    var^ squares = 0\n"
        "    var^ center = 0\n"
        "    var^ low = 0\n"
        "    var^ high = 0\n"
        "    for^ i from^ 1 to^ 12000 {\n"
        "        let^ x = r.normal(0, 1)\n"
        "        if^ x fits^ std.random.Error.InvalidArgument { return^ 0 }\n"
        "        sum := sum + x\n"
        "        squares := squares + x * x\n"
        "        if^ x > -1 and^ x < 1 { center := center + 1 }\n"
        "        if^ x < -2 { low := low + 1 }\n"
        "        if^ x > 2 { high := high + 1 }\n"
        "    }\n"
        "    r.dispose()\n"
        "    if^ sum < -600 or^ sum > 600 { return^ 0 }\n"
        "    if^ squares < 10800 or^ squares > 13200 { return^ 0 }\n"
        "    if^ center < 7700 or^ center > 8700 { return^ 0 }\n"
        "    if^ low < 150 or^ low > 400 or^ high < 150 or^ high > 400 { return^ 0 }\n"
        "    return^ 1\n"));
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);
}

static void test_uniform(void)
{
    LHAT_TEST("uniform agrees with next on the unit interval and replays");
    LhatTestRan ran = run_source(WITH_TWO("321", "321",
        "        var^ same = 1\n"
        "        for^ i from^ 1 to^ 32 {\n"
        "            if^ a.uniform(0, 1) != b.next() { same := 0 }\n"
        "        }\n"
        "        let^ state = a.save()\n"
        "        let^ x = a.uniform(-3, 7)\n"
        "        let^ loaded = a.load(state)\n"
        "        if^ x != a.uniform(-3, 7) { same := 0 }\n"
        "        let^ saved = a.save()\n"
        "        if^ a.uniform(2, 2) != 2 { same := 0 }\n"
        "        if^ saved != a.save() { same := 0 }\n"
        "        a.dispose()\n"
        "        b.dispose()\n"
        "        return^ same\n"));
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    const char *const bounds[] = {"1, 6", "-7, -2", "-1e308, 1e308",
                                  "1, 1.0000000000000002"};
    const char *const low[] = {"1", "-7", "-1e308", "1"};
    const char *const high[] = {"6", "-2", "1e308", "1.0000000000000002"};
    for (size_t i = 0; i < sizeof bounds / sizeof bounds[0]; ++i) {
        LHAT_TEST("uniform enforces half-open bounds even for adjacent or extreme reals");
        char source[1024];
        snprintf(source, sizeof source, WITH_RANDOM("1",
            "    var^ inside = 1\n"
            "    for^ i from^ 1 to^ 100 {\n"
            "        let^ x = try^r.uniform(%s)\n"
            "        if^ (x - %s).sign() < 0 or^ (x - %s).sign() >= 0 { inside := 0 }\n"
            "    }\n"
            "    r.dispose()\n"
            "    return^ inside\n"), bounds[i], low[i], high[i]);
        ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    const char *const invalid[] = {
        "uniform(2, 1)", "uniform(number^.nan, 1)", "uniform(0, number^.inf)",
        "uniform(-number^.inf, 0)", "uniform(0, number^.nan)",
        "rangeint(2, 1)", "rangeint(0.5, 2)", "rangeint(0, 2.5)",
        "rangeint(number^.nan, 2)", "rangeint(0, number^.inf)",
        "rangeint(0, 9223372036854775808.0)"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        LHAT_TEST("invalid uniform and rangeint bounds preserve state");
        char source[1024];
        snprintf(source, sizeof source, WITH_RANDOM("7",
            "    let^ state = r.save()\n"
            "    let^ result = r.%s\n"
            "    let^ saved = r.save()\n"
            "    r.dispose()\n"
            "    if^ result fits^ std.random.Error.InvalidArgument {\n"
            "        if^ saved = state { return^ 1 }\n"
            "    }\n"
            "    return^ 0\n"), invalid[i]);
        ran = run_source(source);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_rangeint(void)
{
    // State 1 produces a rejected draw followed by an accepted one for this
    // span. Pin this path so a plain modulo cannot accidentally pass.
    LHAT_TEST("range rejects a draw in the incomplete residue block");
    {
        LhatTestRan ran = run_source(WITH_RANDOM("1",
            "    let^ loaded = r.load(1)\n"
            "    let^ n = try^r.rangeint((-9223372036854775807 - 1), 0)\n"
            "    let^ state = r.save()\n"
            "    r.dispose()\n"
            "    if^ n = -6066446928794000100 and^ state = 1126174793148417 { return^ 1 }\n"
            "    return^ 0\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("range supports exact large bounds and the full signed span");
    {
        LhatTestRan ran = run_source(WITH_RANDOM("1",
            "    var^ inside = 1\n"
            "    for^ i from^ 1 to^ 32 {\n"
            "        let^ n = try^r.rangeint(9223372036854775805, 9223372036854775807)\n"
            "        if^ n < 9223372036854775805 or^ n > 9223372036854775807 { inside := 0 }\n"
            "    }\n"
            "    let^ loaded = r.load(1)\n"
            "    let^ n = try^r.rangeint((-9223372036854775807 - 1), 9223372036854775807)\n"
            "    if^ n != -4042879741648380643 { inside := 0 }\n"
            "    r.dispose()\n"
            "    return^ inside\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

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
                        "        let^ n = try^r.rangeint(1, 3)\n"
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

    // Equal bounds return lo; backwards bounds return InvalidArgument.
    // Neither consumes a draw, so the two generators must remain in step.
    LHAT_TEST("equal and backwards bounds spend no draw");
    {
        LhatTestRan ran = run_source(
            WITH_TWO("77", "77",
                     "        var^ answered = 0\n"
                     "        if^ try^a.rangeint(5, 5) = 5 { answered := answered + 1 }\n"
                     "        if^ a.rangeint(9, 2) fits^ std.random.Error.InvalidArgument { answered := answered + 1 }\n"
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
    test_state();
    test_next();
    test_normal();
    test_uniform();
    test_rangeint();
    return lhat_test_report("test_random");
}
