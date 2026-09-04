// L^ (lhat) -- tests for std.task, a pool of worker machines.
//
// What is pinned here is the shape Memo's sketch asked for: a job written
// as the call of a yieldable procedure (which 02 の 15.5 makes a coroutine
// that has not started, and 05 の 8.8改3 lets cross), handed to a pool, and
// waited for -- with the jobs actually running side by side rather than one
// after the other.
//
// The pool holds OS threads and machines, so every case that starts one
// stops it before it ends: a worker outliving its program would be reading
// a chunk that has been freed, which is the crash stdlib/thread.c's
// join_and_free comment describes.

#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/async.h"
#include "../stdlib/channel.h"
#include "../stdlib/task.h"
#include "port/thread.h"

static const LhatTestRegister regs[] = {lhatstdlib_task_register,
                                        lhatstdlib_async_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 2, text);
}

static void test_the_sketch(void)
{
    // Memo's own spelling, and the whole point of the module: two yieldable
    // procedures called (not started), handed over, and awaited.
    LHAT_TEST("two jobs written as calls of a yieldable run and answer");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "let^ gen1 = p^ { yield^ 0 return^ 40 }\n"
            "let^ gen2 = p^ { yield^ 0 return^ 2 }\n"
            "std.task.start(2)\n"
            "let^ t1 = std.task.async(gen1())\n"
            "let^ t2 = std.task.async(gen2())\n"
            "var^ n = 0\n"
            "if^ t1 fits^ std.task.Task and^ t2 fits^ std.task.Task {\n"
            "    let^ a = std.task.await(t1)\n"
            "    let^ b = std.task.await(t2)\n"
            "    if^ a fits^ number^ { n += a }\n"
            "    if^ b fits^ number^ { n += b }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // A closure job, with what follows handed over as its arguments --
    // std.thread.spawn's shape, for a body that does not yield.
    LHAT_TEST("a closure job takes the arguments written after it");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(2)\n"
            "let^ t = std.task.async(p^ ... {\n"
            "    let^ a = ...[1]\n"
            "    let^ b = ...[2]\n"
            "    if^ a fits^ number^ and^ b fits^ number^ { return^ a * b }\n"
            "    return^ 0\n"
            "}, 6, 7)\n"
            "var^ n = 0\n"
            "if^ t fits^ std.task.Task {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n := got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // A job that captures crosses with its captures, as a snapshot
    // (carry.h) -- and answers a table, which carry moves too.
    LHAT_TEST("a job carries what it closed over, and answers a table");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1)\n"
            "let^ scale = 5\n"
            "let^ job = p^ n:number^ { yield^ 0 return^ { got = n * scale } }\n"
            "let^ t = std.task.async(job(8))\n"
            "var^ n = 0\n"
            "if^ t fits^ std.task.Task {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ t^{ got : number^ } { n := got.got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 40);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_side_by_side(void)
{
    // The claim the module exists for. Eight jobs of 60ms on four workers
    // is two rounds -- about 120ms. One worker would be 480ms, so the
    // bound below tells the two apart with room to spare on a slow machine.
    LHAT_TEST("the jobs run side by side rather than one after another");
    {
        int64_t before = lhat_now_ms();
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(4)\n"
            "let^ slow = p^ { yield^ std.async.timer(0.06) return^ 1 }\n"
            "var^ tasks : t^{std.task.Task[]} = {}\n"
            "for^ i from^ 1 to^ 8 {\n"
            "    let^ t = std.task.async(slow())\n"
            "    if^ t fits^ std.task.Task { tasks.push^(t) }\n"
            "}\n"
            "var^ n = 0\n"
            "for^ t in^ tasks {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n += got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        int64_t spent = lhat_now_ms() - before;
        LHAT_CHECK_RAN_INTEGER(ran, 8);
        LHAT_CHECK(spent < 400, "four at a time, not one: %lld ms",
                   (long long)spent);
        lhat_test_ran_dispose(&ran);
    }

    // The point of a machine per task. ONE worker and eight jobs that all
    // wait: a worker that held a job while the job waited would run them one
    // after another (eight waits end to end), and one that puts a waiting
    // job down runs all eight waits at once. The bound tells those apart
    // with room to spare -- 8 x 60ms is 480, and the whole thing should
    // take about one wait.
    LHAT_TEST("one worker keeps working while its jobs wait");
    {
        int64_t before = lhat_now_ms();
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(1)\n"
            "let^ slow = p^ { yield^ std.async.timer(0.06) return^ 1 }\n"
            "var^ tasks : t^{std.task.Task[]} = {}\n"
            "for^ i from^ 1 to^ 8 {\n"
            "    let^ t = std.task.async(slow())\n"
            "    if^ t fits^ std.task.Task { tasks.push^(t) }\n"
            "}\n"
            "var^ n = 0\n"
            "for^ t in^ tasks {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n += got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        int64_t spent = lhat_now_ms() - before;
        LHAT_CHECK_RAN_INTEGER(ran, 8);
        LHAT_CHECK(spent < 300,
                   "the waits overlapped on one worker: %lld ms",
                   (long long)spent);
        lhat_test_ran_dispose(&ran);
    }

    // And a job that neither waits nor ends does not starve one that does.
    // 02 の 15.15's slice is the quantum now: the spinner is taken off after
    // TASK_SLICE turns and the waiter gets the worker. Under a pool that ran
    // one job to its end this could not return at all.
    LHAT_TEST("a spinning job does not hold the only worker");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(1)\n"
            // 15.5: spin has no yield^, so the closure itself is the job --
            // spin() would run here and never come back.
            "let^ spin = p^ ... { var^ i = 0 repeat^ { i += 1 } }\n"
            "let^ waits = p^ { yield^ std.async.timer(0.05) return^ 7 }\n"
            "std.task.async(spin)\n"
            "let^ t = std.task.async(waits())\n"
            "var^ n = 0\n"
            "if^ t fits^ std.task.Task {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n := got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 7);
        lhat_test_ran_dispose(&ran);
    }

    // await^ inside a job: the worker drives the coroutine and waits for
    // the very wait it yielded (05 の 8.7改's take by id), so a delay is a
    // delay rather than a resume that came back early.
    LHAT_TEST("a job may wait inside itself, and the wait is kept");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(2)\n"
            "let^ job = p^ {\n"
            "    let^ began = std.async.pending()\n"
            "    yield^ std.async.timer(0.05)\n"
            "    yield^ std.async.timer(0.05)\n"
            "    return^ began\n"
            "}\n"
            "let^ t = std.task.async(job())\n"
            "var^ n = -1\n"
            "if^ t fits^ std.task.Task {\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n := got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        // Its own machine's table was empty when it began: the waits it
        // armed are its own, and nobody else's were left lying in it.
        LHAT_CHECK_RAN_INTEGER(ran, 0);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_answers(void)
{
    LHAT_TEST("a job that faults is read without awaiting it");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1)\n"
            "let^ t = std.task.async(p^ ... { panic^ \"boom\" })\n"
            "var^ said = \"nothing\"\n"
            "if^ t fits^ std.task.Task {\n"
            "    repeat^ { if^ t.done() { break^ } }\n"
            "    let^ failed = t.failed()\n"
            "    if^ failed fits^ string^ { said := failed }\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ std.task.TaskError.Failed { said := got.message }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ said\n");
        // 04 の 11.6改: what it panicked with and where, not just the
        // word `panic^`.
        LHAT_CHECK(ran.ok && ran.text != NULL &&
                       strstr(ran.text, "boom") != NULL &&
                       strstr(ran.text, "line ") != NULL,
                   "the fault is readable: %s",
                   ran.text != NULL ? ran.text : "(none)");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what cannot cross is refused with carry's own reason");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1)\n"
            "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
            "let^ started = gen()\n"
            "started.start()\n"
            "let^ t = std.task.async(started)\n"
            "started.dispose()\n"
            "var^ said = \"took it\"\n"
            "if^ t fits^ std.task.TaskError.Refused { said := t.message }\n"
            "std.task.stop()\n"
            "return^ said\n");
        LHAT_CHECK_RAN_TEXT(ran,
                            "a coroutine that has started stays on its machine");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("without a pool there is nothing to give work to");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "let^ t = std.task.async(p^ ... { return^ 1 })\n"
            "if^ t fits^ std.task.TaskError.NotStarted { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("workers() says how many are standing, and stop ends them");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "var^ n = 0\n"
            "let^ said = std.task.start(3)\n"
            "if^ said fits^ number^ and^ said = 3 { n += 1 }\n"
            "if^ std.task.workers() = 3 { n += 10 }\n"
            "std.task.stop()\n"
            "if^ std.task.workers() = 0 { n += 100 }\n"
            // Starting again is a fresh pool.
            "std.task.start(1)\n"
            "if^ std.task.workers() = 1 { n += 1000 }\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1111);
        lhat_test_ran_dispose(&ran);
    }

    // 15.14改: the push a scheduler parks on rather than asking done().
    LHAT_TEST("a task hands out a wait that its end completes");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(1)\n"
            "let^ t = std.task.async(p^ ... { return^ 5 })\n"
            "var^ n = 0\n"
            "if^ t fits^ std.task.Task {\n"
            "    let^ id = t.awaitable()\n"
            "    if^ id > 0 {\n"
            "        let^ ready = std.async.wait(2)\n"
            "        if^ ready = id { n += 1 }\n"
            "    }\n"
            "    let^ got = std.task.await(t)\n"
            "    if^ got fits^ number^ { n += got }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 6);
        lhat_test_ran_dispose(&ran);
    }
}

static const LhatTestRegister with_channel[] = {lhatstdlib_task_register,
                                                lhatstdlib_async_register,
                                                lhatstdlib_channel_register};

static void test_task_crosses(void)
{
    // 05 の 8.8改2: a Task is a shared type, so one job may be handed
    // another's -- which is what a pipeline is written with.
    LHAT_TEST("a Task may be pushed into a channel and read on a worker");
    {
        LhatTestRan ran = lhat_test_run(
            with_channel, 3,
            "import^ std.task\n"
            "import^ std.channel\n"
            "std.task.start(2)\n"
            "let^ c = std.channel.new()\n"
            "var^ n = 0\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ first = std.task.async(p^ ... { return^ 20 })\n"
            "    if^ first fits^ std.task.Task { c.push(first) }\n"
            // The second job takes the first's handle through the channel
            // and awaits it there.
            "    let^ second = std.task.async(p^ ... {\n"
            "        import^ std.task\n"
            "        let^ mine = ...[1]\n"
            "        if^ mine fits^ std.channel.Channel {\n"
            "            let^ handed = mine.demand(2)\n"
            "            if^ handed fits^ std.task.Task {\n"
            "                let^ got = std.task.await(handed)\n"
            "                if^ got fits^ number^ { return^ got + 1 }\n"
            "            }\n"
            "        }\n"
            "        return^ 0\n"
            "    }, c)\n"
            "    if^ second fits^ std.task.Task {\n"
            "        let^ got = std.task.await(second)\n"
            "        if^ got fits^ number^ { n := got }\n"
            "    }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 21);
        lhat_test_ran_dispose(&ran);
    }
}

// 02 の 15.15: the slice. A job that neither yields nor ends used to hold
// its worker for ever, and a stop would wait for it -- which is to say the
// program hung. The budget is what gets the worker's attention back.
static void test_runaway(void)
{
    LHAT_TEST("a job that never ends does not hold the pool for ever");
    {
        int64_t before = lhat_now_ms();
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(2)\n"
            // Two of them, so both workers are held.
            "let^ spin = p^ ... { var^ n = 0 repeat^ { n := n + 1 } }\n"
            "std.task.async(spin)\n"
            "std.task.async(spin)\n"
            // What is asked here is only that the stop comes back at all.
            "std.task.stop()\n"
            "return^ std.task.workers()\n");
        int64_t spent = lhat_now_ms() - before;
        LHAT_CHECK_RAN_INTEGER(ran, 0);
        LHAT_CHECK(spent < 5000, "the stop came back: %lld ms",
                   (long long)spent);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_the_sketch();
    test_side_by_side();
    test_answers();
    test_task_crosses();
    test_runaway();
    return lhat_test_report("test_task");
}
