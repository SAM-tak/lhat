// L^ (lhat) -- tests for std.thread and for the variadic host functions it
// is the first user of.
//
// Two things are pinned here. The first is 13.7 reaching a host registration:
// a signature ending in '...' makes the arity a floor rather than an exact
// count, and the tail arrives at the LhatHostFn uncollected. The second is
// what std.thread does with it -- that fn is a 15.13 'closed^p^...' closure
// and nothing else, and that the four kinds a value crosses machines as make
// the round trip unchanged while everything else is refused by name.
//
// The threads themselves are real OS threads (port/thread.h), so every one
// started here is joined or disposed before the case ends: an unjoined one
// outliving its program would be reading a chunk that has been freed, which
// is the crash stdlib/thread.c's join_and_free comment describes.

#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/io.h"
#include "../stdlib/thread.h"

// std.thread is the module every case here registers; the run itself is
// stdlibutil.h's, shared with the other stdlib tests.
static const LhatTestRegister regs[] = {lhatstdlib_thread_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// Whether the unit checks at all. The refusals below are type errors rather
// than run-time ones wherever the checker can see them, so this is what those
// cases ask.
static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

// The preamble every case shares: spawn, join, and hand the answer back. Both
// results are unions (a ThreadHandle or an error either way), so both are
// narrowed with isa^ rather than read outright.
#define WITH_SPAWN(call)                                                   \
    "import^ std.thread\n"                                                 \
    "let^ h = " call "\n"                                                  \
    "if^ h isa^ std.thread.ThreadHandle {\n"                               \
    "    let^ answer = h.join()\n"                                         \
    "    if^ answer isa^ std.thread.ThreadError { return^ \"error\" }\n"   \
    "    return^ answer\n"                                                 \
    "}\n"                                                                  \
    "return^ \"refused\"\n"

static void test_spawn_shape(void)
{
    LHAT_TEST("a 'closed^p^...' closure is what spawn takes");
    {
        LhatTestRan ran = run_source(WITH_SPAWN("std.thread.spawn(closed^p^ ... "
                                                "{ return^ 42 })"));
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // 13.7: the collector's element type is any^, so a plain 'p^ ...' is what
    // the declared parameter 'p^...' asks for. A written parameter is not:
    // conformance compares the lists, and one of those has a variadic tail
    // where the other has a slot of its own.
    LHAT_TEST("a closure with a parameter of its own is not a 'p^...'");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(closed^p^ n:number^ { })\n"),
                   "the checker refuses it before anything runs");
    }

    LHAT_TEST("a closure taking nothing at all is not a 'p^...' either");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(closed^p^ { })\n"),
                   "13.7's floor is one slot, and it has none");
    }

    // The result is the one half of the boundary the checker settles outright
    // -- it is copied back rather than forwarded, so nothing has to fit
    // through a variadic slot and the four kinds can be named.
    LHAT_TEST("a closure answering something that cannot cross is refused");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(closed^p^ ... "
                           "{ return^ {1, 2} })\n"),
                   "a table is not one of the four that cross");
    }
}

// 15.13: what a body captures is decided where the body is written, so both
// halves of this are type errors. spawn asks for a closed^ closure, and the
// mark is what the checker holds the body to -- the run-time refusal
// (ThreadError.NotSpawnable) is left as the backstop for a run that was
// never checked.
static void test_spawn_upvalue(void)
{
    LHAT_TEST("a closed^ closure that closes over a variable is refused");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ n = 7\n"
                           "let^ h = std.thread.spawn(closed^p^ ... "
                           "{ return^ n })\n"),
                   "the capture is reported where the body is written");
    }

    LHAT_TEST("and an unmarked closure does not fit spawn at all");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(p^ ... { return^ 1 })\n"),
                   "p^...; does not conform to closed^p^...;");
    }

    // The mark reaches through a body written inside the marked one: what
    // that names from further out would be captured just the same.
    LHAT_TEST("and a body nested in a closed^ one is inside it too");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ n = 7\n"
                           "let^ h = std.thread.spawn(closed^p^ ... {\n"
                           "    let^ inner = f^ -> number^ { return^ n }\n"
                           "    return^ inner()\n"
                           "})\n"),
                   "the boundary is the closed^ body, not the innermost one");
    }

    // What it may still name: an import^ root is read off L^.modules wherever
    // it is written (05 の 8.7), so naming a module captures nothing.
    LHAT_TEST("but the module it was written beside is not a capture");
    {
        LHAT_CHECK(checks("import^ std.thread\n"
                          "let^ h = std.thread.spawn(closed^p^ ... {\n"
                          "    std.thread.sleep(0)\n"
                          "    return^ 1\n"
                          "})\n"),
                   "std.thread is reached without capturing");
    }
}

static void test_arguments(void)
{
    // 13.7: what spawn was written past fn reaches thread_spawn as the tail of
    // its own arguments, is copied into the plain form, and is collected into
    // the callee's '...' by lhat_machine_call on the other machine.
    LHAT_TEST("what is written past fn arrives as fn's '...'");
    {
        LhatTestRan ran = run_source(
            WITH_SPAWN("std.thread.spawn(closed^p^ ... {\n"
                       "    var^ total = 0\n"
                       "    for^ i, x in^ ... { total := total + x }\n"
                       "    return^ total\n"
                       "}, 3, 4, 5)"));
        LHAT_CHECK_RAN_INTEGER(ran, 12);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("no arguments at all leaves '...' empty");
    {
        LhatTestRan ran = run_source(
            WITH_SPAWN("std.thread.spawn(closed^p^ ... {\n"
                       "    var^ total = 0\n"
                       "    for^ i, x in^ ... { total := total + 1 }\n"
                       "    return^ total\n"
                       "})"));
        LHAT_CHECK_RAN_INTEGER(ran, 0);
        lhat_test_ran_dispose(&ran);
    }

    // A string is the one carried kind that allocates on both sides, so it is
    // what pins the copy rather than the value. A position of '...' is any^
    //, and the only thing to do with an any^ is narrow it -- 02 の
    // 13.11's isa^ reaches a builtin name now, so the far side reads the
    // bytes it was given rather than counting that something arrived. Joining
    // them is what pins their order as well: the two go out separately and
    // come back as one string, which they could not do if either had been
    // rebuilt wrongly on the way.
    LHAT_TEST("the bytes of a string cross to the thread and come back");
    {
        LhatTestRan ran = run_source(
            WITH_SPAWN("std.thread.spawn(closed^p^ ... {\n"
                       "    var^ joined = \"\"\n"
                       "    for^ i, x in^ ... {\n"
                       "        if^ x isa^ string^ { joined := joined .. x }\n"
                       "    }\n"
                       "    return^ joined\n"
                       "}, \"carried \", \"across\")"));
        LHAT_CHECK_RAN_TEXT(ran, "carried across");
        lhat_test_ran_dispose(&ran);
    }

    // A collector is a table, and a table holds no nil^ -- so a nil^ written
    // among the arguments leaves no position behind. That is 13.7's own
    // behaviour and not spawn's: an ordinary L^ call written the same way
    // counts the same one position, which is what this pins.
    LHAT_TEST("a nil^ argument collapses the same way an ordinary call's does");
    {
        LhatTestRan spawned = run_source(
            WITH_SPAWN("std.thread.spawn(closed^p^ ... {\n"
                       "    var^ n = 0\n"
                       "    for^ i, x in^ ... { n := n + 1 }\n"
                       "    return^ n\n"
                       "}, \"here\", nil^)"));
        LhatTestRan called = run_source("let^ count = p^ ... {\n"
                                "    var^ n = 0\n"
                                "    for^ i, x in^ ... { n := n + 1 }\n"
                                "    return^ n\n"
                                "}\n"
                                "return^ count(\"here\", nil^)\n");
        LHAT_CHECK(spawned.ok && called.ok, "both programs ran");
        LHAT_CHECK_EQ_INT(spawned.integer, called.integer);
        LHAT_CHECK_EQ_INT(spawned.integer, 1);
        lhat_test_ran_dispose(&spawned);
        lhat_test_ran_dispose(&called);
    }

    // 13.7's 'expr...' at a host call: the closure path unpacks a spread into
    // the frame it pushes, and the host path has no frame, so it packs one
    // array instead. Without that the table would arrive as a single argument.
    LHAT_TEST("a spread into spawn forwards the caller's own '...'");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ forward = p^ ... {\n"
            "    return^ std.thread.spawn(closed^p^ ... {\n"
            "        var^ total = 0\n"
            "        for^ i, x in^ ... { total := total + x }\n"
            "        return^ total\n"
            "    }, ...)\n"
            "}\n"
            "let^ h = forward(6, 7, 8)\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer isa^ std.thread.ThreadError { return^ 0 - 1 }\n"
            "    return^ answer\n"
            "}\n"
            "return^ 0 - 2\n");
        LHAT_CHECK_RAN_INTEGER(ran, 21);
        lhat_test_ran_dispose(&ran);
    }

    // Only the four kinds cross, and spawn's own '...' is any^ so that a
    // forwarded collector fits (see the registration) -- which puts this
    // refusal at run time, by name, rather than in the checker.
    LHAT_TEST("a table is not an argument spawn can carry");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(closed^p^ ... { return^ 1 }, {1, 2})\n"
            "if^ h isa^ std.thread.ThreadError.BadArgument {\n"
            "    return^ \"refused\"\n"
            "}\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    h.dispose()\n"
            "    return^ \"taken\"\n"
            "}\n"
            "return^ \"other\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "refused");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_dispose(void)
{
    // stdlib/thread.c's thread_dispose: a handle let go without a join is
    // waited for there instead. Twenty of them at once is what turned the
    // detach-and-forget version into a use-after-free.
    LHAT_TEST("a handle disposed without a join waits for its thread");
    {
        LhatTestRan ran =
            run_source("import^ std.thread\n"
                       "var^ started = 0\n"
                       "for^ i from^ 1 to^ 20 {\n"
                       "    let^ h = std.thread.spawn(closed^p^ ... { return^ 1 })\n"
                       "    if^ h isa^ std.thread.ThreadHandle {\n"
                       "        started := started + 1\n"
                       "        h.dispose()\n"
                       "    }\n"
                       "}\n"
                       "return^ started\n");
        LHAT_CHECK_RAN_INTEGER(ran, 20);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("joining twice answers AlreadyJoined rather than waiting again");
    {
        LhatTestRan ran =
            run_source("import^ std.thread\n"
                       "let^ h = std.thread.spawn(closed^p^ ... { return^ 1 })\n"
                       "if^ h isa^ std.thread.ThreadHandle {\n"
                       "    let^ first = h.join()\n"
                       "    let^ again = h.join()\n"
                       "    if^ again isa^ std.thread.ThreadError.AlreadyJoined {\n"
                       "        return^ 1\n"
                       "    }\n"
                       "    return^ 0\n"
                       "}\n"
                       "return^ 0 - 1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// sleep is the one registration here that starts nothing. What is pinned is
// the shape -- that it is written in seconds, takes a number^ and nothing
// else, and comes back whatever it was handed. How long it actually waited is
// deliberately not asserted: a wall-clock lower bound is flaky on a loaded
// machine, and one long enough to be worth believing would make this the
// slowest case in the suite.
static void test_sleep(void)
{
    LHAT_TEST("sleep waits and the unit carries on");
    {
        LhatTestRan ran = run_source("import^ std.thread\n"
                                     "std.thread.sleep(0.01)\n"
                                     "return^ 7\n");
        LHAT_CHECK_RAN_INTEGER(ran, 7);
        lhat_test_ran_dispose(&ran);
    }

    // Nothing to wait for is not an error -- there is no result to read one
    // out of, and a caller computing a delay that came out at or below zero
    // meant "do not wait". A whole number is the same number^ either way
    // (14.8), which is what these two are written as.
    LHAT_TEST("and nothing to wait for returns at once");
    {
        LhatTestRan ran = run_source("import^ std.thread\n"
                                     "std.thread.sleep(0)\n"
                                     "std.thread.sleep(0 - 1)\n"
                                     "return^ 7\n");
        LHAT_CHECK_RAN_INTEGER(ran, 7);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and what is not a number^ is refused by the checker");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "std.thread.sleep(\"x\")\n"),
                   "a string is not a duration");
    }
}

// 05 の 8.7: what a spawned body can reach. A registration is an object on
// the heap of the machine it was installed on, so the new machine is given
// its own copies before anything runs -- without that the body reaches a
// nil^ where print was and faults on the call. And an import^ root is read
// off L^.modules rather than captured, which is what lets a body name a
// module at all: spawn refuses a closure with captures, and naming one used
// to be a capture.
static void test_modules_reach_the_thread(void)
{
    LHAT_TEST("a spawned body may name the module it was written beside");
    {
        LhatTestRan ran = run_source(WITH_SPAWN("std.thread.spawn(closed^p^ ... {\n"
                                                "    std.thread.sleep(0.01)\n"
                                                "    return^ 42\n"
                                                "})"));
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // A second module, named inside the body: what it reaches there is the
    // new machine's own registration, installed before the body ran.
    LHAT_TEST("and a module it was not spawned from writes on that machine");
    {
        static const LhatTestRegister with_io[] = {lhatstdlib_thread_register,
                                                   lhatstdlib_io_register};
        char written[64] = {0};
        LhatTestRan ran = lhat_test_run_capturing(
            with_io, 2, "thread_print.txt",
            "import^ std.thread\n"
            "import^ std.io\n"
            "let^ h = std.thread.spawn(closed^p^ ... {\n"
            "    std.io.print(\"in the thread\")\n"
            "    return^ 1\n"
            "})\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    return^ 1\n"
            "}\n"
            "return^ 0\n",
            written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK(strcmp(written, "in the thread\n") == 0,
                   "got \"%s\"", written);
        lhat_test_ran_dispose(&ran);
    }
}

// 02 の 15.14: what a scheduler asks in place of waiting. join() blocks, and
// a loop with other tasks to run may not -- so done() answers whether the
// join would return at once.
static void test_done(void)
{
    LHAT_TEST("done() answers true once the body has finished");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(closed^p^ ... { return^ 1 })\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ h.done() { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ 0 - 1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // A body that is still sleeping has not finished, so the answer is false
    // -- which is what makes the ask worth making rather than always true.
    LHAT_TEST("and false while it is still running");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(closed^p^ ... {\n"
            "    std.thread.sleep(0.25)\n"
            "    return^ 1\n"
            "})\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    let^ early = h.done()\n"
            "    let^ answer = h.join()\n"
            "    if^ !early and^ h.done() { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ 0 - 1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_spawn_shape();
    test_spawn_upvalue();
    test_arguments();
    test_dispose();
    test_sleep();
    test_modules_reach_the_thread();
    test_done();
    return lhat_test_report("test_thread");
}
