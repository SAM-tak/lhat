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

#include "../stdlib/async.h"
#include "../stdlib/io.h"
#include "../stdlib/thread.h"
#include "port/thread.h"

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
// narrowed with fits^ rather than read outright.
#define WITH_SPAWN(call)                                                   \
    "import^ std.thread\n"                                                 \
    "let^ h = " call "\n"                                                  \
    "if^ h fits^ std.thread.ThreadHandle {\n"                               \
    "    let^ answer = h.join()\n"                                         \
    "    if^ answer fits^ std.thread.ThreadError { return^ \"error\" }\n"   \
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

    // carry.h: a table crosses now, as a deep copy -- the checker has
    // nothing to refuse here, and join hands the copy back.
    LHAT_TEST("a closure answering a table checks clean");
    {
        LHAT_CHECK(checks("import^ std.thread\n"
                          "let^ h = std.thread.spawn(closed^p^ ... "
                          "{ return^ {1, 2} })\n"),
                   "a table crosses as a copy");
    }
}

// 15.13: a closed^ closure still holds its body to capturing nothing, and
// still fits spawn (promising more than is asked). What spawn asks for is
// now a plain 'p^...': carry.h takes what a closure closes over across as a
// snapshot, so the mark is no longer the price of a thread.
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

    LHAT_TEST("and an unmarked closure fits spawn");
    {
        LHAT_CHECK(checks("import^ std.thread\n"
                          "let^ h = std.thread.spawn(p^ ... { return^ 1 })\n"),
                   "p^...; is what spawn asks for now");
    }

    // carry.h: the capture crosses as a snapshot. The thread sees 7, and
    // what it does to its own copy never reaches back.
    LHAT_TEST("a closure that closes over a variable carries a snapshot");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "var^ n = 7\n"
            "let^ h = std.thread.spawn(p^ ... { n := n + 1 return^ n })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answered = h.join()\n"
            "    h.dispose()\n"
            "    if^ answered fits^ number^ {\n"
            "        return^ answered * 100 + n\n"
            "    }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 800 + 7);
        lhat_test_ran_dispose(&ran);
    }

    // A cycle survives the trip both ways, and the copy over there is its
    // own: a push^ on it leaves the original at three.
    LHAT_TEST("a table crosses with its cycles, and comes back the same way");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "var^ ring = { name = \"a\" }\n"
            "var^ ring.me := ring\n"
            "var^ items = {1, 2, 3}\n"
            "let^ h = std.thread.spawn(p^ ... {\n"
            "    let^ got = ...[1]\n"
            "    let^ list = ...[2]\n"
            "    var^ cyclic = 0\n"
            "    if^ got fits^ t^{} {\n"
            "        if^ got[\"me\"] is^ got { cyclic := 1000 }\n"
            "    }\n"
            "    if^ list fits^ t^{number^[]} { list.push^(4) }\n"
            "    return^ { cyclic, list }\n"
            "}, ring, items)\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ back = h.join()\n"
            "    h.dispose()\n"
            "    if^ back fits^ t^{} {\n"
            "        let^ c = back[1]\n"
            "        let^ l = back[2]\n"
            "        if^ c fits^ number^ {\n"
            "            if^ l fits^ t^{} {\n"
            "                return^ c + l.count^ * 10 + items.count^\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1000 + 40 + 3);
        lhat_test_ran_dispose(&ran);
    }

    // Two closures that captured one place keep sharing it over there.
    LHAT_TEST("closures sharing a place share it again on the far side");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "var^ shared = 0\n"
            "let^ bump = p^ { shared := shared + 1 }\n"
            "let^ read = f^ -> number^ { return^ shared }\n"
            "let^ h = std.thread.spawn(p^ ... {\n"
            "    bump()\n"
            "    bump()\n"
            "    return^ read()\n"
            "})\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answered = h.join()\n"
            "    h.dispose()\n"
            "    if^ answered fits^ number^ {\n"
            "        return^ answered * 10 + shared\n"
            "    }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 20);
        lhat_test_ran_dispose(&ran);
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
    // 13.11's fits^ reaches a builtin name now, so the far side reads the
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
                       "        if^ x fits^ string^ { joined := joined .. x }\n"
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
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer fits^ std.thread.ThreadError { return^ 0 - 1 }\n"
            "    return^ answer\n"
            "}\n"
            "return^ 0 - 2\n");
        LHAT_CHECK_RAN_INTEGER(ran, 21);
        lhat_test_ran_dispose(&ran);
    }

    // 05 の 8.8改3: a coroutine that has not started crosses, so a worker
    // may be handed the very thing the caller wrote as a call.
    LHAT_TEST("a spawn takes a coroutine that has not started");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... {\n"
            "    let^ job = ...[1]\n"
            "    if^ job fits^ c^{p^ -> number^ -> nil^} {\n"
            "        let^ first = job.start()\n"
            "        if^ first fits^ number^ { return^ first }\n"
            "    }\n"
            "    return^ 0\n"
            "}, (p^ n:number^ { yield^ n + 1 })(41))\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer fits^ number^ { return^ answer }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // Only what carry carries crosses, and spawn's own '...' is any^ so that
    // a forwarded collector fits (see the registration) -- which puts this
    // refusal at run time, by name, rather than in the checker.
    LHAT_TEST("a coroutine that has started is not one spawn can carry");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
            "let^ started = gen()\n"
            "started.start()\n"
            "let^ h = std.thread.spawn(closed^p^ ... { return^ 1 }, started)\n"
            "if^ h fits^ std.thread.ThreadError.BadArgument {\n"
            "    return^ \"refused\"\n"
            "}\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
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
                       "    if^ h fits^ std.thread.ThreadHandle {\n"
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
                       "if^ h fits^ std.thread.ThreadHandle {\n"
                       "    let^ first = h.join()\n"
                       "    let^ again = h.join()\n"
                       "    if^ again fits^ std.thread.ThreadError.AlreadyJoined {\n"
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
            "if^ h fits^ std.thread.ThreadHandle {\n"
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
            "if^ h fits^ std.thread.ThreadHandle {\n"
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
            "if^ h fits^ std.thread.ThreadHandle {\n"
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

// ---------------------------------------------------------------------------
// 15.14改: what a body's end pushes, rather than what a caller has to ask for
// ---------------------------------------------------------------------------

static const LhatTestRegister with_async[] = {lhatstdlib_thread_register,
                                              lhatstdlib_async_register};

// What the host is told, from the worker's own thread.
typedef struct {
    LhatMutex lock;
    int calls;
    bool ok;
    char message[256];
} Heard;

static Heard heard;

static void note_finished(void *context, void *handle, bool ok,
                          const char *message)
{
    Heard *said = (Heard *)context;
    (void)handle;
    lhat_mutex_lock(&said->lock);
    said->calls++;
    said->ok = ok;
    said->message[0] = '\0';
    if (message != NULL) {
        size_t length = strlen(message);
        if (length >= sizeof said->message) {
            length = sizeof said->message - 1;
        }
        memcpy(said->message, message, length);
        said->message[length] = '\0';
    }
    lhat_mutex_unlock(&said->lock);
}

static bool register_and_watch(LhatProgram *program)
{
    return lhatstdlib_thread_register(program) &&
           lhatstdlib_thread_on_finish(program, note_finished, &heard);
}

static void test_pushed_completion(void)
{
    // The wait awaitable() arms is pushed when the body finishes, so a
    // scheduler parks on it instead of asking done() over and over. Here
    // the L^ side does what a scheduler would: take the id, then ask
    // std.async which wait is ready.
    LHAT_TEST("15.14改: the end of a body completes the wait it armed");
    {
        LhatTestRan ran = lhat_test_run(
            with_async, 2,
            "import^ std.async\n"
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { std.thread.sleep(0.02) })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ id = h.awaitable()\n"
            "    if^ id > 0 {\n"
            // A wait of its own is armed, so what comes back has to be the
            // thread's rather than whatever was in the table.
            "        let^ ready = std.async.wait(2)\n"
            "        h.join()\n"
            "        if^ ready = id { return^ 1 }\n"
            "        return^ 0\n"
            "    }\n"
            "    h.join()\n"
            "    return^ -2\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Asking after it has already finished is the race this has to answer
    // for: the push happened before there was anything to push to.
    LHAT_TEST("and one asked for after the body finished is ready at once");
    {
        LhatTestRan ran = lhat_test_run(
            with_async, 2,
            "import^ std.async\n"
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    repeat^ { if^ h.done() { break^ } std.thread.sleep(0.005) }\n"
            "    let^ id = h.awaitable()\n"
            "    let^ ready = std.async.wait(2)\n"
            "    h.join()\n"
            "    if^ id > 0 and^ ready = id { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // No std.async on the program: nothing to arm, and the answer says so
    // rather than failing.
    LHAT_TEST("without a scheduler it answers nothing to wait on");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ id = h.awaitable()\n"
            "    h.join()\n"
            "    if^ id = 0 { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("failed() reads the fault without joining");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { panic^ \"boom\" })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    repeat^ { if^ h.done() { break^ } std.thread.sleep(0.005) }\n"
            "    let^ said = h.failed()\n"
            "    h.join()\n"
            "    if^ said fits^ string^ { return^ said }\n"
            "    return^ \"nothing said\"\n"
            "}\n"
            "return^ \"refused\"\n");
        LHAT_CHECK(ran.ok && ran.text != NULL &&
                       strstr(ran.text, "panic") != NULL,
                   "the fault is readable: %s",
                   ran.text != NULL ? ran.text : "(none)");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and answers nothing for a body that ran clean");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { return^ 1 })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    h.join()\n"
            "    if^ h.failed() fits^ nil^ { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // The host's hook, which is what lets a loop notice a worker dying
    // without ever joining one.
    LHAT_TEST("the host hears about a fault without joining");
    {
        memset(&heard, 0, sizeof heard);
        lhat_mutex_init(&heard.lock);
        static const LhatTestRegister watched[] = {register_and_watch};
        LhatTestRan ran = lhat_test_run(
            watched, 1,
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { panic^ \"boom\" })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    repeat^ { if^ h.done() { break^ } std.thread.sleep(0.005) }\n"
            "    h.dispose()\n"
            "    return^ 1\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_mutex_lock(&heard.lock);
        LHAT_CHECK_EQ_INT(heard.calls, 1);
        LHAT_CHECK(!heard.ok, "it was told the body failed");
        LHAT_CHECK(strstr(heard.message, "panic") != NULL,
                   "with what a join would have said: %s", heard.message);
        lhat_mutex_unlock(&heard.lock);
        lhat_test_ran_dispose(&ran);
        lhat_mutex_destroy(&heard.lock);
    }

    // 8.8改2 made carry answer with more than the scalars the old signature
    // listed, so join says any^ and the caller narrows.
    LHAT_TEST("a body answering a table is joined as one");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { return^ { x = 6, y = 7 } })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer fits^ t^{ x : number^, y : number^ } {\n"
            "        return^ answer.x * answer.y\n"
            "    }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }
}

// 4.5: a host disposing of a program has to know nothing of it is still
// running. Every handle is dropped here without a join, which before this
// left three threads reading a proto the program was about to free.
// The one file these build a program out of. stdlibutil's own loader is
// not reachable from here, and what this needs is the program itself rather
// than the run its helpers wrap.
static char *only_file(void *context, const char *path, size_t *length)
{
    const char *text = (const char *)context;
    if (strcmp(path, "main.lh") != 0) {
        return NULL;
    }
    *length = strlen(text);
    char *copy = (char *)lhat_alloc(*length + 1);
    if (copy != NULL) {
        memcpy(copy, text, *length + 1);
    }
    return copy;
}

static void test_join_all(void)
{
    LHAT_TEST("a host waits for every thread the program still has");
    {
        static const char text[] =
            "import^ std.thread\n"
            "var^ made = 0\n"
            "for^ i from^ 1 to^ 3 {\n"
            "    let^ h = std.thread.spawn(p^ ... {\n"
            "        std.thread.sleep(0.05)\n"
            "    })\n"
            "    if^ h fits^ std.thread.ThreadHandle { made := made + 1 }\n"
            "}\n"
            "return^ made\n";
        LhatProgram *program =
            lhat_program_new(true, only_file, (void *)text);
        LHAT_CHECK(lhatstdlib_thread_register(program), "registered");
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(program) &&
                       lhat_program_compile(program),
                   "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, machine), "installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3);
        // The handles are still the machine's, unjoined; this is what says
        // the bodies are done before anything they read goes.
        lhatstdlib_thread_join_all(program);
        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }

    LHAT_TEST("and a program without the module is left alone");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        lhatstdlib_thread_join_all(program);  // nothing to wait for
        LHAT_CHECK(!lhatstdlib_thread_on_finish(program, note_finished,
                                                &heard),
                   "and has no hook to set");
        lhat_program_free(program);
    }
}

// ---------------------------------------------------------------------------
// 05 の 8.11: the host's lock over the program's writes
// ---------------------------------------------------------------------------

// A lock that counts, so the test can say the entries took it -- and a
// plain one underneath, so an entry taking it twice would hang rather than
// pass quietly (port/thread.h's mutex does not nest).
typedef struct {
    LhatMutex mutex;
    int taken;
    int held;
    int deepest;
} Counted;

static Counted counted;

static void counted_lock(void *context)
{
    Counted *lock = (Counted *)context;
    lhat_mutex_lock(&lock->mutex);
    lock->taken++;
    lock->held++;
    if (lock->held > lock->deepest) {
        lock->deepest = lock->held;
    }
}

static void counted_unlock(void *context)
{
    Counted *lock = (Counted *)context;
    lock->held--;
    lhat_mutex_unlock(&lock->mutex);
}

static void test_program_lock(void)
{
    LHAT_TEST("every write and every install goes through the host's lock");
    {
        memset(&counted, 0, sizeof counted);
        lhat_mutex_init(&counted.mutex);
        static const char text[] =
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { return^ 5 })\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer fits^ number^ { return^ answer }\n"
            "}\n"
            "return^ -1\n";
        LhatProgram *program =
            lhat_program_new(true, only_file, (void *)text);
        lhat_program_set_lock(program, counted_lock, counted_unlock, &counted);
        LHAT_CHECK(lhatstdlib_thread_register(program), "registered");
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(program) &&
                       lhat_program_compile(program),
                   "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, machine), "installed");
        int before = counted.taken;
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
        // The check, the compile and the install above, and the worker's
        // own install during the run.
        LHAT_CHECK(before >= 3, "the entries took it: %d", before);
        LHAT_CHECK(counted.taken > before,
                   "and so did the worker's install: %d", counted.taken);
        LHAT_CHECK_EQ_INT(counted.deepest, 1);
        LHAT_CHECK_EQ_INT(counted.held, 0);
        lhat_machine_dispose(machine);
        lhat_program_free(program);
        lhat_mutex_destroy(&counted.mutex);
    }

    LHAT_TEST("and a program given none runs as it did");
    {
        static const char text[] = "return^ 7\n";
        LhatProgram *program =
            lhat_program_new(true, only_file, (void *)text);
        lhat_program_set_lock(program, NULL, NULL, NULL);
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(program), "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, machine), "installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }
}

// ---------------------------------------------------------------------------
// 03 の 5.1改5: the member cache under several machines at once
// ---------------------------------------------------------------------------

// A proto belongs to the program, so every worker running this body reads
// the same chunk -- and the member cache is the one thing on a chunk that
// is written while it runs. The site below is polymorphic on purpose: two
// definitions with the same member name, alternating, so the workers keep
// filling one site with different places. A read that took a mix of two
// fills would answer another member (or walk off the end); every worker
// answering the same total is what says it cannot.
static void test_shared_member_cache(void)
{
    LHAT_TEST("many machines fill one member site without mixing it");
    {
        LhatTestRan ran = run_source(
            "import^ std.thread\n"
            // The definitions are made inside the body: a def^ does not
            // cross (carry.h), and this is the sharper test anyway -- every
            // worker fills the one site with tables of its own.
            "let^ work = p^ ... {\n"
            "    let^ A = def^{ self^{ },\n"
            "        what = f^self^ -> number^ { return^ 1 },\n"
            "        other = f^self^ -> number^ { return^ 100 } }\n"
            "    let^ B = def^{ self^{ },\n"
            "        what = f^self^ -> number^ { return^ 2 },\n"
            "        other = f^self^ -> number^ { return^ 200 } }\n"
            "    let^ a = A.new()\n"
            "    let^ b = B.new()\n"
            "    var^ sum = 0\n"
            "    for^ i from^ 1 to^ 1000 {\n"
            "        var^ it = a\n"
            "        if^ i % 2 = 0 { it := b }\n"
            "        sum += it.what() + it.other()\n"
            "    }\n"
            "    return^ sum\n"
            "}\n"
            // Four of them at once on the one chunk.
            "var^ hands : t^{std.thread.ThreadHandle[]} = {}\n"
            "for^ i from^ 1 to^ 4 {\n"
            "    let^ h = std.thread.spawn(work)\n"
            "    if^ h fits^ std.thread.ThreadHandle { hands.push^(h) }\n"
            "}\n"
            "var^ total = 0\n"
            "for^ h in^ hands {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer fits^ number^ { total += answer }\n"
            "}\n"
            "return^ total\n");
        // 500 turns of (1 + 100) and 500 of (2 + 200), four times over.
        LHAT_CHECK_RAN_INTEGER(ran, 4 * (500 * 101 + 500 * 202));
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
    test_pushed_completion();
    test_join_all();
    test_program_lock();
    test_shared_member_cache();
    return lhat_test_report("test_thread");
}
