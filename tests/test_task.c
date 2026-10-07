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
#include "program_internal.h"

#include "../stdlib/async.h"
#include "../stdlib/channel.h"
#include "../stdlib/task.h"
#include "../stdlib/lton.h"
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
            "std.task.start(2) catch^ panic^ it^\n"
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

    // Arguments belong to coroutine construction, not to async itself.
    LHAT_TEST("an immediately ending coroutine carries its arguments");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(2) catch^ panic^ it^\n"
            "let^ t = std.task.async((p^ ... { _yield^ 0\n"
            "    let^ a = ...[0]\n"
            "    let^ b = ...[1]\n"
            "    if^ a fits^ number^ and^ b fits^ number^ { return^ a * b }\n"
            "    return^ 0\n"
            "})(6, 7)) catch^ panic^ it^\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
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
            "std.task.start(4) catch^ panic^ it^\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ spin = p^ { _yield^ 0 var^ i = 0 repeat^ { i += 1 } }\n"
            "let^ waits = p^ { yield^ std.async.timer(0.05) return^ 7 }\n"
            "std.task.async(spin()) catch^ panic^ it^\n"
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

    // The same for a closure job: the slice is the machine's, so a job that
    // is a plain call is taken off as surely as a coroutine is.
    LHAT_TEST("a spinning closure job does not hold the only worker");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "import^ std.async\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ spin = p^ { var^ i = 0 repeat^ { i += 1 } }\n"
            "let^ waits = p^ { yield^ std.async.timer(0.05) return^ 7 }\n"
            "std.task.async(spin) catch^ panic^ it^\n"
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
            "std.task.start(2) catch^ panic^ it^\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ t = std.task.async((p^ { _yield^ 0 panic^ \"boom\" })())\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
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
            "let^ t = std.task.async((p^ { _yield^ 0 return^ 1 })())\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
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
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ t = std.task.async((p^ { _yield^ 0 return^ 5 })())\n"
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
            "std.task.start(2) catch^ panic^ it^\n"
            "let^ c = std.channel.new()\n"
            "var^ n = 0\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ first = std.task.async((p^ { _yield^ 0 return^ 20 })())\n"
            "    if^ first fits^ std.task.Task { c.push(first) catch^ nil^ }\n"
            // The second job takes the first's handle through the channel
            // and awaits it there.
            "    let^ second = std.task.async((p^ ... { _yield^ 0\n"
            "        import^ std.task\n"
            "        let^ mine = ...[0]\n"
            "        if^ mine fits^ std.channel.Channel {\n"
            "            let^ handed = mine.demand(2)\n"
            "            if^ handed fits^ std.task.Task {\n"
            "                let^ got = std.task.await(handed)\n"
            "                if^ got fits^ number^ { return^ got + 1 }\n"
            "            }\n"
            "        }\n"
            "        return^ 0\n"
            "    })(c)) catch^ panic^ it^\n"
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

// The other half of "a waiting job holds nothing". A job waiting on a
// channel used to wait on the channel's own condition, which is the WORKER
// going to sleep -- so with one worker a consumer and a producer could not
// both make progress at all. Channel.awaitable() is the way down: the
// consumer yields a std.async wait, the scheduler puts it aside, and the
// push tells the wait.
//
// One worker on purpose. With demand() this cannot finish: the worker sleeps
// in the consumer and the producer never runs.
static void test_channel_without_holding(void)
{
    LHAT_TEST("a job waiting on a channel gives the worker up");
    {
        LhatTestRan ran = lhat_test_run(
            with_channel, 3,
            "import^ std.task\n"
            "import^ std.async\n"
            "import^ std.channel\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ c = std.channel.new()\n"
            "var^ n = -1\n"
            "if^ c fits^ std.channel.Channel {\n"
            // The consumer. awaitable() is what it yields instead of
            // demand()'s wait, and the loop is because another taker may
            // have had the value between the telling and the running.
            "    let^ eat = p^ mine:std.channel.Channel {\n"
            "        var^ got : any^ = mine.pop()\n"
            "        repeat^ while^ got is^ nil^ {\n"
            "            yield^ mine.awaitable()\n"
            "            got := mine.pop()\n"
            "        }\n"
            "        if^ got fits^ number^ { return^ got }\n"
            "        return^ 0\n"
            "    }\n"
            // Queued second, and it only runs if the first gave the worker
            // up.
            "    let^ feed = p^ mine:std.channel.Channel {\n"
            "        yield^ std.async.timer(0.03)\n"
            "        mine.push(41) catch^ nil^\n"
            "        return^ 1\n"
            "    }\n"
            "    let^ eater = std.task.async(eat(c))\n"
            "    std.task.async(feed(c)) catch^ panic^ it^\n"
            "    if^ eater fits^ std.task.Task {\n"
            "        let^ got = std.task.await(eater)\n"
            "        if^ got fits^ number^ { n := got }\n"
            "    }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 41);
        lhat_test_ran_dispose(&ran);
    }

    // The same thing said in one line. take() is the loop above as a
    // coroutine, so await^ hides both the parking and the retry -- and the
    // retry is what a writer forgets.
    LHAT_TEST("await^ take() is the whole of waiting on a channel");
    {
        LhatTestRan ran = lhat_test_run(
            with_channel, 3,
            "import^ std.task\n"
            "import^ std.async\n"
            "import^ std.channel\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ c = std.channel.new()\n"
            "var^ n = -1\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ eat = p^ mine:std.channel.Channel {\n"
            "        let^ got = await^ mine.take()\n"
            "        if^ got fits^ number^ { return^ got }\n"
            "        return^ 0\n"
            "    }\n"
            "    let^ feed = p^ mine:std.channel.Channel {\n"
            "        yield^ std.async.timer(0.03)\n"
            "        mine.push(41) catch^ nil^\n"
            "        return^ 1\n"
            "    }\n"
            "    let^ eater = std.task.async(eat(c))\n"
            "    std.task.async(feed(c)) catch^ panic^ it^\n"
            "    if^ eater fits^ std.task.Task {\n"
            "        let^ got = std.task.await(eater)\n"
            "        if^ got fits^ number^ { n := got }\n"
            "    }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 41);
        lhat_test_ran_dispose(&ran);
    }

    // A channel with something in it is answered by the first step, so the
    // walk ends without ever yielding -- and it takes them in order.
    LHAT_TEST("take() on a channel that has something never parks");
    {
        LhatTestRan ran = lhat_test_run(
            with_channel, 3,
            "import^ std.task\n"
            "import^ std.async\n"
            "import^ std.channel\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ c = std.channel.new()\n"
            "var^ n = -1\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    c.push(6) catch^ nil^\n"
            "    c.push(7) catch^ nil^\n"
            "    let^ eat = p^ mine:std.channel.Channel {\n"
            "        let^ a = await^ mine.take()\n"
            "        let^ b = await^ mine.take()\n"
            "        if^ a fits^ number^ and^ b fits^ number^ { return^ a * b }\n"
            "        return^ 0\n"
            "    }\n"
            "    let^ eater = std.task.async(eat(c))\n"
            "    if^ eater fits^ std.task.Task {\n"
            "        let^ got = std.task.await(eater)\n"
            "        if^ got fits^ number^ { n := got }\n"
            "    }\n"
            "}\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // Nothing to wait for is 0, and yield^ 0 is a hand-over rather than a
    // wait -- so a consumer that reads a channel with something in it never
    // parks at all.
    LHAT_TEST("awaitable answers 0 when the channel already has something");
    {
        LhatTestRan ran = lhat_test_run(
            with_channel, 3,
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "var^ n = -1\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    c.push(1) catch^ nil^\n"
            "    n := c.awaitable()\n"
            "}\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 0);
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
            "std.task.start(2) catch^ panic^ it^\n"
            // Two of them, so both workers are held.
            "let^ spin = p^ { _yield^ 0 var^ n = 0 repeat^ { n := n + 1 } }\n"
            "std.task.async(spin()) catch^ panic^ it^\n"
            "std.task.async(spin()) catch^ panic^ it^\n"
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

static char *typed_task_load(void *context, const char *path, size_t *length)
{
    const char *text = strcmp(path, "main.lh") == 0 ? (const char *)context :
        strcmp(path, "jobs.lh") == 0 ?
        "module^ jobs\n"
        "import^ std.task\n"
        "public^ let^ make = p^ {\n"
        " let^ job = p^ { yield^ 0 return^ 42 }\n"
        " return^ std.task.async(job()) catch^ panic^ it^\n"
        "}\n" : NULL;
    if (text == NULL) return NULL;
    *length = strlen(text);
    char *copy = malloc(*length + 1);
    if (copy != NULL) memcpy(copy, text, *length + 1);
    return copy;
}

static void test_task_unit_boundary(void)
{
    static const char *const roots[] = {
        "require^ 'jobs.lh'\nimport^ std.task\n"
        "let^ n:number^ = std.task.await(jobs.make()) catch^ panic^ it^\n",
        "require^ 'jobs.lh'\nimport^ std.task\n"
        "let^ n:string^ = std.task.await(jobs.make()) catch^ panic^ it^\n"
    };
    LHAT_TEST("task specialization survives a source-unit export boundary");
    for (size_t i = 0; i < 2; i++) {
        LhatProgram *program = lhat_program_new(true, typed_task_load, (void *)roots[i]);
        LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        LHAT_CHECK(root != NULL, "root loaded");
        LHAT_CHECK(lhat_program_has_errors(program) == (i == 1),
            "only the wrong result annotation fails across units");
        if (i == 0) {
            LHAT_CHECK(lhat_program_compile(program), "exported task compiles");
        }
        lhat_program_free(program);
    }
}

static void test_annotated_task_walk(void)
{
    LHAT_TEST("single-focus walks accept applied task type annotations on unconstrained tables");
    const char *source = "import^std.task\nlet^tasks = {}\n"
        "for^task:std.task.Task<number^> in^tasks {\n"
        " let^answer:number^ = std.task.await(task) catch^panic^it^\n}\n";
    LhatProgram *program = lhat_program_new(true, typed_task_load, (void *)source);
    LHAT_REQUIRE(program != NULL, "program created");
    LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
    LHAT_CHECK(lhat_program_check(program, "main.lh") != NULL, "source checked");
    LHAT_CHECK(!lhat_program_has_errors(program), "applied annotation resolves the focus");
    LHAT_CHECK(lhat_program_compile(program), "annotated walk compiles");
    lhat_program_free(program);
}

static void test_task_capture_initialization(void)
{
    LHAT_TEST("task transfer rejects uninitialized captures through nested and recursive calls");
    static const char *const sources[] = {
        "import^ std.task\n"
        "let^piece = p^n:number^ {_yield^0 let^solve = f^ {return^ppp()} return^solve()}\n"
        "let^split = p^n:number^ {if^n = 0 {let^t = std.task.async(piece(n)) catch^panic^it^ return^} this^(n-1)}\n"
        "split(1)\n"
        "let^ppp = f^{42}\n",
        // Defining piece before ppp is legal; the use must follow initialization.
        "import^ std.task\n"
        "let^piece = p^n:number^ {_yield^0 let^solve = f^ {return^ppp()} return^solve()}\n"
        "let^split = p^n:number^ {let^t = std.task.async(piece(n)) catch^panic^it^}\n"
        "let^ppp = f^{42}\n"
        "split(1)\n",
        // Coroutine construction holds cells; transfer snapshots their values.
        "import^ std.task\n"
        "let^piece = p^ {_yield^0 return^ppp()}\n"
        "let^co = piece()\n"
        "let^ppp = f^{42}\n"
        "let^t = std.task.async(co) catch^panic^it^\n",
        // An initialized captured closure can itself capture an uninitialized value.
        "import^ std.task\n"
        "let^piece = p^ {_yield^0 return^ppp()}\n"
        "let^ppp = f^{answer}\n"
        "let^t = std.task.async(piece()) catch^panic^it^\n"
        "let^answer = 42\n",
        // An uncalled body is not a transfer at its definition site.
        "import^ std.task\n"
        "let^piece = p^ {_yield^0 return^ppp()}\n"
        "let^unused = p^ {let^t = std.task.async(piece()) catch^panic^it^}\n"
        "let^ppp = f^{42}\n",
        // A wrapper's parameter retains the transferred coroutine's origin.
        "import^ std.task\n"
        "let^piece = p^ {_yield^0 return^ppp()}\n"
        "let^submit = p^co {let^t = std.task.async(co) catch^panic^it^}\n"
        "submit(piece())\n"
        "let^ppp = f^{42}\n",
        // A closure job transfers its captures the same way.
        "import^ std.task\n"
        "let^job = p^ {return^ppp()}\n"
        "let^unused = 0\n"
        "let^t = std.task.async(job) catch^panic^it^\n"
        "let^ppp = f^{42}\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof sources[0]; i++) {
        for (int strict = 0; strict < 2; strict++) {
            LhatProgram *program = lhat_program_new(strict != 0, typed_task_load, (void *)sources[i]);
            LHAT_REQUIRE(program != NULL, "program created");
            LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
            const LhatUnit *unit = lhat_program_check(program, "main.lh");
            LHAT_CHECK(unit != NULL, "checked");
            size_t reports = 0;
            if (unit != NULL) {
                for (size_t k = 0; k < unit->checked.diagnostic_count; k++) {
                    const LhatCheckDiagnostic *d = &unit->checked.diagnostics[k];
                    if (d->code != LHAT_CHECK_ERR_UNINITIALIZED_TASK_CAPTURE) continue;
                    reports++;
                    LHAT_CHECK_EQ_INT(d->line, 4);
                    LHAT_CHECK(!d->relaxed_ok && d->cause != NULL && d->cause->line == 5,
                               "hard error at use, with initialization location");
                    const char *name = i == 3 ? "answer" : "ppp";
                    LHAT_CHECK(d->name_length == strlen(name) &&
                               memcmp(d->name, name, d->name_length) == 0, "captured name");
                }
            }
            bool bad = i == 0 || i == 3 || i == 5 || i == 6;
            LHAT_CHECK_EQ_INT(reports, bad ? 1 : 0);
            LHAT_CHECK(lhat_program_has_errors(program) == bad, "only unsafe transfers fail");
            lhat_program_free(program);
        }
    }
}

static void test_await_union_projection(void)
{
    LHAT_TEST("await flattens a projected union together with all task errors");
    static const char source[] =
        "import^ std.task\n"
        "let^ gen = p^ flag:bool^ { _yield^ 0 if^ flag { return^ 'ok' } return^ 40 }\n"
        "let^ task:std.task.Task<string^|number^> = try^std.task.async(gen(true^))\n"
        "let^ raw = std.task.await(task)\n"
        "let^ handled = try^std.task.await(task)\n"
        "let^ copy = raw\nlet^ copyHandled = handled\n";
    LhatProgram *program = lhat_program_new(true, typed_task_load, (void *)source);
    LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
    const LhatUnit *unit = lhat_program_check(program, "main.lh");
    LHAT_CHECK(unit != NULL && !lhat_program_has_errors(program), "union task checks");
#if LHAT_WITH_RESOLUTIONS
    if (unit != NULL) {
        const char *names[] = {"= raw", "= handled"};
        const char *expected_text[] = {
            "string^|number^|std.task.TaskError.Taken|std.task.TaskError.NotStarted|std.task.TaskError.Refused"
            "|std.task.TaskError.Failed|std.error.OutOfMemory",
            "string^|number^"
        };
        for (size_t i = 0; i < 2; i++) {
            uint32_t offset = (uint32_t)(strstr(source, names[i]) - source + 2);
            const LhatResolution *use = lhat_check_resolution_at(&unit->checked, offset);
            const LhatType *expected = lhat_type_of_text(expected_text[i],
                strlen(expected_text[i]), &program->types, program->hosted, NULL);
            char actual[512] = "(missing)";
            if (use != NULL) lhat_type_write_full(use->type, actual, sizeof actual);
            LHAT_CHECK(use != NULL && expected != NULL && lhat_type_equal(use->type, expected),
                       "projected union has exactly the expected arms, got %s", actual);
            size_t arms = 0;
            if (use != NULL && use->type->kind == LHAT_TYPE_UNION) {
                for (const LhatTypeList *a = use->type->v.composite.arms; a; a = a->next) {
                    LHAT_CHECK(a->type->kind != LHAT_TYPE_UNION, "union is flattened");
                    arms++;
                }
            }
            LHAT_CHECK_EQ_INT(arms, i == 0 ? 7 : 2);
        }
    }
#endif
    lhat_program_free(program);
}

static void test_await_inferred_type(void)
{
    static const char source[] =
        "import^ std.task\n"
        "let^ gen1 = p^ { yield^ 0 return^ 40 }\n"
        "let^ t1 = try^std.task.async(gen1())\n"
        "let^ raw = std.task.await(t1)\n"
        "let^ handled = try^std.task.await(t1)\n"
        "let^ copy = raw\n"
        "let^ copyHandled = handled\n";
    LHAT_TEST("inspect await's inferred result before and after try");
    LhatProgram *program = lhat_program_new(true, typed_task_load, (void *)source);
    LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
    const LhatUnit *unit = lhat_program_check(program, "main.lh");
    LHAT_CHECK(unit != NULL && !lhat_program_has_errors(program), "source checks");
#if LHAT_WITH_RESOLUTIONS
    if (unit != NULL) {
        uint32_t raw_offset = (uint32_t)(strstr(source, "= raw") - source + 2);
        uint32_t handled_offset = (uint32_t)(strstr(source, "= handled") - source + 2);
        const LhatResolution *raw = lhat_check_resolution_at(&unit->checked, raw_offset);
        const LhatResolution *handled = lhat_check_resolution_at(&unit->checked, handled_offset);
        const char expected_text[] =
            "number^|std.task.TaskError.Taken|std.task.TaskError.NotStarted|std.task.TaskError.Refused"
            "|std.task.TaskError.Failed|std.error.OutOfMemory";
        const LhatType *expected = lhat_type_of_text(expected_text,
            strlen(expected_text), &program->types, program->hosted, NULL);
        char actual[512] = "(missing)";
        if (raw != NULL) lhat_type_write_full(raw->type, actual, sizeof actual);
        LHAT_CHECK(expected != NULL && raw != NULL && raw->type != NULL &&
                       raw->type->kind == LHAT_TYPE_UNION &&
                       lhat_type_equal(raw->type, expected),
                   "raw await must be exactly number plus task errors, got %s", actual);
        LHAT_CHECK(handled != NULL && handled->type != NULL &&
                       handled->type->kind == LHAT_TYPE_NUMBER,
                   "try await must be exactly number, not any or unknown");
    }
#endif
    lhat_program_free(program);

    LHAT_TEST("the reported two-job addition fails only without await error handling");
    static const char prefix[] =
        "import^ std.task\n"
        "let^ gen1 = p^ { yield^ 0 return^ 40 }\n"
        "let^ gen2 = p^ { yield^ 0 return^ 2 }\n"
        "std.task.start(2) catch^ panic^ it^\n"
        "let^ t1 = try^std.task.async(gen1())\n"
        "let^ t2 = try^std.task.async(gen2())\n"
        "var^ n = 0\n";
    char text[1024];
    snprintf(text, sizeof text, "%s%s", prefix,
        "n += std.task.await(t1)\nn += std.task.await(t2)\n"
        "std.task.stop()\nreturn^ n\n");
    program = lhat_program_new(true, typed_task_load, text);
    LHAT_CHECK(lhatstdlib_task_register(program), "task registered");
    unit = lhat_program_check(program, "main.lh");
    LHAT_CHECK(unit != NULL && lhat_program_has_errors(program),
               "unhandled await cannot be added to a number");
    if (unit != NULL) {
        LHAT_CHECK_EQ_INT(unit->checked.diagnostic_count, 2);
        for (size_t i = 0; i < unit->checked.diagnostic_count; i++) {
            LHAT_CHECK_EQ_INT(unit->checked.diagnostics[i].code, LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR);
        }
    }
    lhat_program_free(program);
    snprintf(text, sizeof text, "%s%s", prefix,
        "n += try^std.task.await(t1)\nn += try^std.task.await(t2)\n"
        "std.task.stop()\nreturn^ n\n");
    LhatTestRan ran = run_source(text);
    LHAT_CHECK_RAN_INTEGER(ran, 42);
    lhat_test_ran_dispose(&ran);
}

static void test_static_results(void)
{
    LHAT_TEST("task type arguments can be written explicitly");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ job = p^ { _yield^ 0 return^ 42 }\n"
            "let^ t:std.task.Task<number^>=try^std.task.async(job())\n"
            "let^ n:number^ = try^std.task.await(t)\n"
            "std.task.stop()\nreturn^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_TEST("async takes one coroutine or one closure taking nothing");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { let^ n:number^ = yield^ 0 return^ n }\n"
        "let^ t = std.task.async(job())\n"), "resume argument rejected statically");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { let^ a:number^, b:string^ = yield^ 0 return^ a }\n"
        "let^ t = std.task.async(job())\n"), "multiple resume arguments rejected statically");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { _yield^ 0 return^ 42 }\n"
        "let^ erased:c^ = job()\nlet^ t = std.task.async(erased)\n"),
        "erased receive shape cannot prove zero resume arguments");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = std.task.async(p^ ... { return^ 1 })\n"),
        "variadic closure rejected");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = std.task.async(p^ n:number^ -> number^ { return^ n })\n"),
        "closure with a parameter rejected");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ -> number^ { _yield^ 0 return^ 1 }\n"
        "let^ t = std.task.async(job)\n"), "yieldable closure rejected; its call is the job");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = std.task.async(p^ -> number^, number^ { return^ 1, 2 })\n"),
        "multi-value closure rejected");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = try^std.task.async(p^ -> number^ { return^ 42 })\n"
        "let^ n:number^ = try^std.task.await(t)\n"
        "let^ u = try^std.task.async(f^ -> string^ { \"s\" })\n"
        "let^ s:string^ = try^std.task.await(u)\n"
        "let^ v = try^std.task.async(p^ { })\n"
        "let^ z:nil^ = try^std.task.await(v)\n"),
        "a closure's return type becomes the task's");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = try^std.task.async(p^ -> number^ { return^ 42 })\n"
        "let^ s:string^ = try^std.task.await(t)\n"),
        "a closure task's result is not widened");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ t = std.task.async(42)\n"), "number rejected");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { _yield^ 0 return^ 42 }\n"
        "let^ t = std.task.async(job(), 1)\n"), "extra argument rejected");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { _yield^ 0 return^ 42 }\n"
        "let^ t:std.task.Task<string^> = try^std.task.async(job())\n"),
        "wrong explicit type argument rejected");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { _yield^ 0 return^ 42 }\n"
        "let^ t:std.task.Task = try^std.task.async(job())\n"
        "if^ t fits^ std.task.Task<string^> { let^ s = try^std.task.await(t) }\n"),
        "runtime fits checks preserved type arguments");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^ std.task\nlet^ job = p^ { _yield^ 0 return^ 42 }\n"
        "let^ t:std.task.Task = try^std.task.async(job())\n"
        "let^ other = t as^ std.task.Task<string^> catch^ panic^ it^\n"),
        "runtime cast checks preserved type arguments");
    LHAT_TEST("final results survive aliases, members and instantiated calls");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ job = p^ { yield^ 'not the final type' return^ { n = 42 } }\n"
            "let^ pass = f^ x { return^ x }\n"
            "let^ task = std.task.async(job()) catch^ panic^ it^\n"
            "let^ box = { task = pass(task) }\n"
            "let^ answer = std.task.await(box.task) catch^ panic^ it^\n"
            "let^ n:number^ = answer.n\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_TEST("independent call shapes do not overwrite each other's result");
    {
        LhatTestRan ran = run_source(
            "import^ std.task\n"
            "std.task.start(1) catch^ panic^ it^\n"
            "let^ num = p^ { yield^ 0 return^ 42 }\n"
            "let^ str = p^ { yield^ 0 return^ 'ok' }\n"
            "let^ a = std.task.async(num()) catch^ panic^ it^\n"
            "let^ b = std.task.async(str()) catch^ panic^ it^\n"
            "let^ read = p^ t { return^ std.task.await(t) catch^ panic^ it^ }\n"
            "let^ n:number^ = read(a)\n"
            "let^ s:string^ = read(b)\n"
            "std.task.stop()\n"
            "return^ n\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }
    LHAT_TEST("a wrong final-result annotation is refused");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ job = p^ { yield^ 0 return^ 42 }\n"
        "let^ t = std.task.async(job()) catch^ panic^ it^\n"
        "let^ bad:string^ = std.task.await(t) catch^ panic^ it^\n"),
        "number task cannot answer string");
    LHAT_TEST("a mutable binding cannot silently change its task argument");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ num = p^ { yield^ 0 return^ 42 }\n"
        "let^ str = p^ { yield^ 0 return^ 'ok' }\n"
        "var^ t = std.task.async(num()) catch^ panic^ it^\n"
        "t := std.task.async(str()) catch^ panic^ it^\n"
        "let^ n:number^ = std.task.await(t) catch^ panic^ it^\n"),
        "incompatible assignment cannot keep the old result type");
    LHAT_TEST("task errors remain in the refined result");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ job = p^ { yield^ 0 return^ 42 }\n"
        "let^ t = std.task.async(job()) catch^ panic^ it^\n"
        "let^ bad:number^ = std.task.await(t)\n"),
        "await still requires error handling");
    LHAT_TEST("erasing a task's arguments cannot restore a precise answer");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ job = p^ { yield^ 0 return^ 42 }\n"
        "let^ t:std.task.Task = std.task.async(job()) catch^ panic^ it^\n"
        "let^ bad:number^ = std.task.await(t) catch^ panic^ it^\n"),
        "plain Task answers any");
    LHAT_TEST("multi-value results cannot be promised by single-slot await");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ job = p^ { yield^ 0 return^ (1, 2) }\n"
        "let^ t = std.task.async(job()) catch^ panic^ it^\n"),
        "tuple result refused");
    LHAT_TEST("no-value coroutine result becomes nil");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ job = p^ { yield^ 0 }\n"
        "let^ t = std.task.async(job()) catch^ panic^ it^\n"
        "let^ n:nil^ = std.task.await(t) catch^ panic^ it^\n"),
        "no result transports nil");
    LHAT_TEST("a branch keeps both task result types");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ num = p^ { yield^ 0 return^ 42 }\n"
        "let^ str = p^ { yield^ 0 return^ 'ok' }\n"
        "let^ choose = p^ flag:bool^ {\n"
        " if^ flag { return^ std.task.async(num()) catch^ panic^ it^ }\n"
        " return^ std.task.async(str()) catch^ panic^ it^\n"
        "}\n"
        "let^ t = choose(true^)\n"
        "let^ n:number^|string^ = std.task.await(t) catch^ panic^ it^\n"),
        "the union remains precise");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^ std.task\n"
        "let^ num = p^ { yield^ 0 return^ 42 }\n"
        "let^ str = p^ { yield^ 0 return^ 'ok' }\n"
        "let^ choose = p^ flag:bool^ {\n"
        " if^ flag { return^ std.task.async(num()) catch^ panic^ it^ }\n"
        " return^ std.task.async(str()) catch^ panic^ it^\n"
        "}\n"
        "let^ t = choose(true^)\n"
        "let^ n:number^ = std.task.await(t) catch^ panic^ it^\n"),
        "neither task arm can disappear during merging");
}

static void test_await_transfer(void)
{
    LHAT_TEST("await preserves its transferred result type");
    LHAT_CHECK(lhat_test_check_text(regs, 2,
        "import^std.task\nlet^t = try^std.task.async(f^{42})\n"
        "let^n:number^ = try^std.task.await(t)\n"), "number result");
    LHAT_CHECK(!lhat_test_check_text(regs, 2,
        "import^std.task\nlet^t = try^std.task.async(f^{42})\n"
        "let^n:string^ = try^std.task.await(t)\n"), "wrong result type rejected");

    LHAT_TEST("await moves a table once, then reports Taken");
    LhatTestRan ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(f^{{answer = 42}})\n"
        "let^data = try^std.task.await(t)\n"
        "var^n = data.answer\n"
        "if^std.task.await(t) fits^std.task.TaskError.Taken {n += 1}\n"
        "if^t.done() {n += 10}\n"
        "t.dispose()\nstd.task.stop()\nL^.gc.collect()\nreturn^n + data.answer\n");
    LHAT_CHECK_RAN_INTEGER(ran, 95);
    lhat_test_ran_dispose(&ran);

    // 05 の 8.6: a task's machine collects the way the spawner's did when
    // it spawned -- read then, on the spawner's thread.
    LHAT_TEST("a task's machine takes the spawner's collector settings");
    ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "var^ s = L^.gc.settings()\ns.growth := 150\ns.stepsize := 7\n"
        "L^.gc.configure(s)\n"
        "let^t = try^std.task.async(f^{L^.gc.settings().growth * 100 + "
        "L^.gc.settings().stepsize})\n"
        "let^n = try^std.task.await(t)\nstd.task.stop()\nreturn^n\n");
    LHAT_CHECK_RAN_INTEGER(ran, 15007);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("nil is a result that can only be taken once");
    ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(p^{})\n"
        "let^v = try^std.task.await(t)\n"
        "let^again = std.task.await(t)\nstd.task.stop()\n"
        "return^if^v is^nil^ and^ again fits^std.task.TaskError.Taken: 1 el^: 0;\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("closed captures transfer with a program closure");
    ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(f^{let^x = {answer = 42} return^f^{x.answer}})\n"
        "let^f = try^std.task.await(t)\nt.dispose()\n"
        "std.task.stop()\nL^.gc.collect()\nreturn^f()\n");
    LHAT_CHECK_RAN_INTEGER(ran, 42);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("unmovable results are refused without falling back to copying");
    ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(p^{let^g = p^{yield^1 yield^2}\n"
        " let^c = g() c.start() return^{c = c}})\n"
        "let^r = std.task.await(t)\nlet^again = std.task.await(t)\n"
        "std.task.stop()\nreturn^if^r fits^std.task.TaskError.Refused and^\n"
        " again fits^std.task.TaskError.Taken: 1 el^: 0;\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("failure is also received exactly once");
    ran = run_source(
        "import^std.task\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(p^{panic^'failed'})\n"
        "let^r = std.task.await(t)\nlet^again = std.task.await(t)\n"
        "std.task.stop()\nreturn^if^r fits^std.task.TaskError.Failed and^\n"
        " again fits^std.task.TaskError.Taken: 1 el^: 0;\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("two receivers compete for exactly one terminal result");
    ran = run_source(
        "import^std.task\ntry^std.task.start(3)\n"
        "let^t = try^std.task.async(f^{{answer = 42}})\n"
        "let^claim = p^{let^r = std.task.await(t)\n"
        " return^if^r fits^t^{answer:number^}: r.answer el^: 0;}\n"
        "let^a = try^std.task.async(claim)\n"
        "let^b = try^std.task.async(claim)\n"
        "let^n = try^std.task.await(a) + try^std.task.await(b)\nstd.task.stop()\nreturn^n\n");
    LHAT_CHECK_RAN_INTEGER(ran, 42);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("loaded LTON strings survive worker destruction and receiver GC");
    const LhatTestRegister lton_regs[] = {lhatstdlib_task_register, lhatstdlib_lton_register};
    ran = lhat_test_run(lton_regs, 2,
        "import^std.task\nimport^std.lton\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(f^{return^std.lton.parse(\"answer = 42, child = {text = 'hello'}\")})\n"
        "let^data = try^std.task.await(t)\nt.dispose()\nstd.task.stop()\n"
        "L^.gc.collect()\nL^.gc.collect()\n"
        "return^if^data fits^t^{answer:number^, child:t^{text:string^}}:\n"
        " data.answer + (if^data.child.text = 'hello': 5 el^: 0;) el^: 0;\n");
    LHAT_CHECK_RAN_INTEGER(ran, 47);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("LTON errors can be taken as values");
    ran = lhat_test_run(lton_regs, 2,
        "import^std.task\nimport^std.lton\ntry^std.task.start(1)\n"
        "let^t = try^std.task.async(f^{return^std.lton.load('missing.lton')})\n"
        "let^r = std.task.await(t)\nstd.task.stop()\nL^.gc.collect()\n"
        "return^if^r fits^std.lton.LtonError.CannotRead: 1 el^: 0;\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);

    LHAT_TEST("completed data can be abandoned without a receiver");
    ran = run_source(
        "import^std.task\ntry^std.task.start(2)\n"
        "repeat^20 {let^t = try^std.task.async(p^{\n"
        " var^a = {}\nrepeat^500 {a.push^({1,2,3})}\nreturn^a})\n"
        " repeat^until^t.done() {}\nt.dispose()}\n"
        "std.task.stop()\nreturn^1\n");
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    lhat_test_ran_dispose(&ran);
}

static void test_member_race(void)
{
    LHAT_TEST("shared closure member reads and calls stay in their worker VM");
    LhatTestRan ran = run_source(
        "import^std.task\n"
        "let^read = p^id:number^ -> number^{\n"
        " let^own = {tag = id, get = f^ -> number^{id}}\n"
        " for^i from^0 to^100000 {\n"
        "  if^own.tag != id {return^0}\n"
        "  if^own.get() != id {return^0}\n"
        " }\nreturn^id\n}\n"
        "try^std.task.start(2)\nvar^total = 0\n"
        "repeat^8 {\n"
        " let^a = try^std.task.async(p^{return^read(111)})\n"
        " let^b = try^std.task.async(p^{return^read(222)})\n"
        " total += try^std.task.await(a) + try^std.task.await(b)\n"
        " a.dispose() b.dispose()\n"
        " L^.gc.collect()\n}\n"
        "std.task.stop()\nreturn^total\n");
    LHAT_CHECK_RAN_INTEGER(ran, 8 * 333);
    lhat_test_ran_dispose(&ran);
}

int main(void)
{
    test_member_race();
    test_await_transfer();
    test_await_inferred_type();
    test_await_union_projection();
    test_task_unit_boundary();
    test_annotated_task_walk();
    test_task_capture_initialization();
    test_static_results();
    test_the_sketch();
    test_side_by_side();
    test_answers();
    test_task_crosses();
    test_channel_without_holding();
    test_runaway();
    return lhat_test_report("test_task");
}
