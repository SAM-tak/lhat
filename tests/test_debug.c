// L^ (lhat) -- tests for std.debug.
//
// One name and one property. std.debug.log is registered as an 'f^' that
// writes (02 の 10.6 の脱法関数): std.io.print is a 'p^' and so cannot be
// called from inside a pure function, which is exactly where a print is
// wanted while debugging. So what is pinned here is not that a string reaches
// stdout -- any host function could do that -- but that it reaches stdout
// from inside an 'f^', and that the call still answers nil^ so 13.2's "an f^
// answers along every path" holds.
//
// Asserting on what was written means putting a file where stdout is, which
// is stdlibutil.h's redirect; the fd is duplicated aside and put back, since
// LHAT_CHECK prints to stdout too and would otherwise report into the file.

#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/debug.h"

// Where a case's stdout goes while it runs. Written and removed by
// run_capturing, so nothing survives the case that wrote it.
#define SCRATCH "test_debug_out.txt"

static const LhatTestRegister regs[] = {lhatstdlib_debug_register};

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

// Every case here runs with stdout pointed at a file, so this is the one
// spelling of it -- the run is stdlibutil.h's, shared with test_io.c.
static LhatTestRan run_capturing(const char *text, char *buffer, size_t size)
{
    return lhat_test_run_capturing(regs, 1, SCRATCH, text, buffer, size);
}

static void test_log_writes(void)
{
    LHAT_TEST("log writes its argument and a newline");
    {
        char written[256];
        LhatTestRan ran =
            run_capturing("import^ std.debug\n"
                          "let^ noted : nil^ = std.debug.log(\"logged\")\n"
                          "return^ 1\n",
                          written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "logged\n");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("each call writes a line of its own, in order");
    {
        char written[256];
        LhatTestRan ran =
            run_capturing("import^ std.debug\n"
                          "let^ a : nil^ = std.debug.log(\"first\")\n"
                          "let^ b : nil^ = std.debug.log(\"second\")\n"
                          "return^ 1\n",
                          written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "first\nsecond\n");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("an empty string still writes its newline");
    {
        char written[256];
        LhatTestRan ran = run_capturing("import^ std.debug\n"
                                        "let^ a : nil^ = std.debug.log(\"\")\n"
                                        "return^ 1\n",
                                        written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "\n");
        lhat_test_ran_dispose(&ran);
    }
}

// The whole reason this module exists: 10.6 says nothing done inside an 'f^'
// can be observed from outside it, and std.io.print being a 'p^' is what
// keeps that true. std.debug.log is the one name registered to sit on the
// other side of that, so a pure function calling it has to check and has to
// write.
static void test_log_inside_a_pure_function(void)
{
    LHAT_TEST("an f^ may call log, and what it logged is written");
    {
        char written[256];
        LhatTestRan ran = run_capturing(
            "import^ std.debug\n"
            "let^ step = f^ n:number^ -> number^ {\n"
            "    let^ noted : nil^ = std.debug.log(\"inside\")\n"
            "    return^ n + 1\n"
            "}\n"
            "return^ step(41)\n",
            written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        LHAT_CHECK_EQ_STR(written, strlen(written), "inside\n");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a p^ may call it too -- nothing is refused the other way");
    {
        char written[256];
        LhatTestRan ran = run_capturing(
            "import^ std.debug\n"
            "let^ shout = p^ text:string^ {\n"
            "    let^ noted : nil^ = std.debug.log(text)\n"
            "}\n"
            "shout(\"from a procedure\")\n"
            "return^ 1\n",
            written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "from a procedure\n");
        lhat_test_ran_dispose(&ran);
    }
}

// 13.2 asks an f^ to answer along every path, and 04 の 11.3's nil^ is the
// value for "there is nothing here" -- so the registration says nil^ and the
// checker is what pins it, no run needed.
static void test_log_answers_nil(void)
{
    LHAT_TEST("what log answers is a nil^");
    {
        LHAT_CHECK(checks("import^ std.debug\n"
                          "let^ noted : nil^ = std.debug.log(\"x\")\n"),
                   "nil^ is what the registration declared");
        LHAT_CHECK(!checks("import^ std.debug\n"
                           "let^ noted : string^ = std.debug.log(\"x\")\n"),
                   "and it is not the string that went in");
    }

    LHAT_TEST("log takes a string and nothing else");
    {
        LHAT_CHECK(!checks("import^ std.debug\n"
                           "let^ noted : nil^ = std.debug.log(1)\n"),
                   "the checker refuses a number before anything runs");
    }

    // 03 の 3.4改2: a ring of names makes the checker walk these statements
    // again, and an import^ binds as well as answering. What its own first
    // walk put there is not a second import of the name.
    LHAT_TEST("an import^ survives the statements being walked again");
    {
        LHAT_CHECK(checks("import^ std.debug\n"
                          "let^ a = f^ n:number^ {\n"
                          "    if^ n <= 0 { return^ true^ }\n"
                          "    return^ b(n - 1)\n"
                          "}\n"
                          "let^ b = f^ n:number^ { return^ a(n - 1) }\n"
                          "let^ x : bool^ = a(4)\n"),
                   "the ring settles and the import is not a redefinition");
    }
}

int main(void)
{
    test_log_writes();
    test_log_inside_a_pure_function();
    test_log_answers_nil();
    return lhat_test_report("test_debug");
}
