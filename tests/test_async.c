// L^ (lhat) -- tests for std.async, the two things a scheduler cannot write
// for itself.
//
// What is pinned here is the contract async.h states to a host: wait(0) does
// not sleep, next() says how long until the clock makes something ready, and
// an external wait is ended by a push that may come from any thread. The
// scheduler itself is L^ (sample/async.lh) and is not what these ask about.
//
// The pushing thread retries until the push lands rather than sleeping a
// guessed amount first: the L^ side arms the wait somewhere inside the run,
// and a test that raced it would fail once in a while and pass in a rerun.

#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/async.h"
#include "../stdlib/thread.h"
#include "port/thread.h"

// The program the harness registered into, so that the pushing thread can
// find the table. A test may capture what a host would hold onto.
static LhatProgram *registered = NULL;

static bool register_async(LhatProgram *program)
{
    registered = program;
    return lhatstdlib_async_register(program);
}

static const LhatTestRegister regs[] = {register_async};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// 05 の 8.7 with async.h: any thread may push. This one is the host that
// finished whatever the id stood for -- a loader, a signal, a worker.
static int push_first_id(void *unused)
{
    (void)unused;
    for (int tries = 0; tries < 600; tries++) {
        void *waits = registered != NULL ? lhatstdlib_async_waits(registered)
                                         : NULL;
        // The first wait a run arms is id 1: ids start there and this test's
        // source arms exactly one.
        if (waits != NULL && lhatstdlib_async_complete(waits, 1)) {
            return 0;
        }
        lhat_thread_sleep(5);
    }
    return 1;
}

static void test_external(void)
{
    // The whole of what an embedder needs: a wait nothing but a push ends,
    // and a push that crosses from another thread.
    LHAT_TEST("an external wait is ended by a push from another thread");
    {
        registered = NULL;
        LhatThread pusher;
        LHAT_CHECK(lhat_thread_start(&pusher, push_first_id, NULL),
                   "the pushing thread started");

        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ id = std.async.external()\n"
            "let^ got = std.async.wait(5)\n"
            "if^ got = id { return^ 1 }\n"
            "return^ 0\n");
        lhat_thread_join(&pusher);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
        registered = NULL;
    }

    // 15.14's shape: nothing about an external wait says when it will be
    // ready, so next() -- which reads the clock -- has nothing to answer.
    LHAT_TEST("and next() says nothing about one");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ id = std.async.external()\n"
            "if^ std.async.next() is^ nil^ { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // Whoever armed it may give up on it, and pending() drops -- without
    // which a scheduler waiting on a push that will never come cannot stop.
    LHAT_TEST("and dropping one takes it out of the table");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ id = std.async.external()\n"
            "std.async.drop(id)\n"
            "return^ std.async.pending()\n");
        LHAT_CHECK_RAN_INTEGER(ran, 0);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_timers(void)
{
    // The promise a host pumping its own loop is held to.
    LHAT_TEST("wait(0) takes what is ready and does not sleep");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ far = std.async.timer(30)\n"
            "if^ std.async.wait(0) is^ nil^ { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and a deadline already past is ready at once");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ now = std.async.timer(0)\n"
            "let^ got = std.async.wait(0)\n"
            "if^ got = now { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // What a host reads to decide its own sleep.
    LHAT_TEST("next() answers the seconds left on the earliest deadline");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "let^ far = std.async.timer(30)\n"
            "let^ near = std.async.timer(0.5)\n"
            "let^ left = std.async.next()\n"
            "if^ left fits^ number^ {\n"
            "    if^ left <= 0.5 and^ left > 0 { return^ 1 }\n"
            "}\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and nothing armed at all answers nil^");
    {
        LhatTestRan ran = run_source(
            "import^ std.async\n"
            "if^ std.async.next() is^ nil^ { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// ---------------------------------------------------------------------------
// 05 の 8.7: a wait belongs to the machine that armed it
// ---------------------------------------------------------------------------

static const LhatTestRegister with_thread[] = {register_async,
                                               lhatstdlib_thread_register};

static void test_waits_belong_to_a_machine(void)
{
    // The table is the program's and a program may have any number of
    // machines on it, so two schedulers meet in it. Each has to see its own
    // wait and nobody else's: a worker asking for "whatever is ready" used
    // to take the main machine's timer, leaving the main machine parked on
    // a wait that had already been handed away.
    LHAT_TEST("two machines do not take each other's waits");
    {
        LhatTestRan ran = lhat_test_run(
            with_thread, 2,
            "import^ std.async\n"
            "import^ std.thread\n"
            // The worker arms one of its own and waits for it. Its timer is
            // the longer of the two, so a machine taking whatever is ready
            // would take this one's.
            "let^ h = std.thread.spawn(p^ ... {\n"
            "    std.async.timer(0.15)\n"
            "    let^ got = std.async.wait(2)\n"
            "    if^ got fits^ number^ { return^ got }\n"
            "    return^ 0\n"
            "})\n"
            "if^ h fits^ std.thread.ThreadHandle {\n"
            "    let^ mine = std.async.timer(0.02)\n"
            "    let^ ours = std.async.wait(2)\n"
            "    let^ theirs = h.join()\n"
            "    var^ n = 0\n"
            "    if^ ours fits^ number^ and^ ours = mine { n += 1 }\n"
            // Two ids were handed out, and each machine took its own.
            "    if^ theirs fits^ number^ and^ theirs != mine and^ theirs > 0 {\n"
            "        n += 10\n"
            "    }\n"
            // And this machine's table is empty again: nothing of the
            // worker's is left standing here.
            "    if^ std.async.pending() = 0 { n += 100 }\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 111);
        lhat_test_ran_dispose(&ran);
    }

    // async.h's targeted take, which is what a worker driving one job uses.
    LHAT_TEST("a wait may be taken by its own id");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK(lhatstdlib_async_register(program), "registered");
        LhatMachine *a = lhat_machine_new();
        LhatMachine *b = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, a) &&
                       lhat_program_install(program, b),
                   "installed on both");
        void *waits = lhatstdlib_async_waits(program);
        LHAT_CHECK(waits != NULL, "the table is there");

        int64_t mine = lhatstdlib_async_external(waits, a);
        int64_t theirs = lhatstdlib_async_external(waits, b);
        LHAT_CHECK(mine > 0 && theirs > 0 && mine != theirs, "two waits");
        LHAT_CHECK_EQ_INT(lhatstdlib_async_take(waits, a, mine), 0);
        // Another machine's is not this one's to take, ready or not.
        LHAT_CHECK_EQ_INT(lhatstdlib_async_take(waits, a, theirs), -1);
        LHAT_CHECK(lhatstdlib_async_complete(waits, mine), "pushed");
        LHAT_CHECK_EQ_INT(lhatstdlib_async_take(waits, a, mine), 1);
        // Taken once and gone.
        LHAT_CHECK_EQ_INT(lhatstdlib_async_take(waits, a, mine), -1);

        lhat_machine_dispose(b);
        lhat_machine_dispose(a);
        lhat_program_free(program);
    }
}

int main(void)
{
    test_external();
    test_timers();
    test_waits_belong_to_a_machine();
    return lhat_test_report("test_async");
}
