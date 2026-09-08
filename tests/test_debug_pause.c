// L^ (lhat) -- D3's sample-library pause points.
//
// The normal module tests pin what each wait returns. These use the public
// debug hook to pin the other half: every cancellable wait reaches a host
// boundary rather than sleeping or waiting in one uninterruptible C call.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/async.h"
#include "../stdlib/channel.h"
#include "../stdlib/task.h"
#include "../stdlib/thread.h"
#include "port/thread.h"  // lhat_now_ms, for the timing a failure reports

#include <stdint.h>

typedef struct {
    size_t points;
    bool at_line[32];
    // How long the whole run took. A wait that reaches no pause point has
    // two possible reasons -- it never waited, or it waited in one
    // uninterruptible call -- and only the clock tells them apart.
    int64_t took_ms;
    // Every event, of whatever kind. Zero means the hook was never live at
    // all, which is a different fault from a wait that offered no boundary.
    size_t events;
} PauseTrace;

static void pause_hook(LhatMachine *machine, void *context,
                       LhatDebugEvent event, const LhatFrameInfo *where)
{
    (void)machine;
    (void)where;
    PauseTrace *trace = (PauseTrace *)context;
    trace->events++;
    if (event == LHAT_DEBUG_HOST_PAUSE_POINT) {
        trace->points++;
        if (where->line < sizeof trace->at_line / sizeof *trace->at_line) {
            trace->at_line[where->line] = true;
        }
    }
}

static const LhatTestRegister regs[] = {lhatstdlib_async_register,
                                        lhatstdlib_channel_register,
                                        lhatstdlib_thread_register,
                                        lhatstdlib_task_register};

static LhatTestRan run_waiting(PauseTrace *trace, const char *text)
{
    int64_t began = lhat_now_ms();
    LhatTestRan ran = lhat_test_run_hooked(regs, sizeof regs / sizeof *regs,
                                           text, pause_hook, trace);
    trace->took_ms = lhat_now_ms() - began;
    return ran;
}

static void check_wait(const char *name, PauseTrace *trace, LhatTestRan ran,
                       const uint32_t *lines, size_t line_count)
{
    LHAT_CHECK_RAN_INTEGER(ran, 1);
    LHAT_CHECK(trace->points > 0,
               "%s reached a host pause point (the run took %lldms and the "
               "hook saw %zu events of any kind; a wait that waits outlasts "
               "the 20ms pause interval several times over)",
               name, (long long)trace->took_ms, trace->events);
    for (size_t i = 0; i < line_count; i++) {
        LHAT_CHECK(lines[i] < sizeof trace->at_line / sizeof *trace->at_line &&
                       trace->at_line[lines[i]],
                   "%s reached its wait at line %u", name, lines[i]);
    }
    lhat_test_ran_dispose(&ran);
}

static void test_waits_pause(void)
{
    LHAT_TEST("std.async.wait reaches a pause point while sleeping");
    {
        PauseTrace trace = {0};
        LhatTestRan ran = run_waiting(
            &trace, "import^ std.async\n"
                    "std.async.timer(0.05)\n"
                    "std.async.wait(0.05)\n"
                    "return^ 1\n");
        const uint32_t lines[] = {3};
        check_wait("std.async.wait", &trace, ran, lines,
                   sizeof lines / sizeof *lines);
    }

    LHAT_TEST("std.thread sleep and join reach pause points");
    {
        PauseTrace trace = {0};
        LhatTestRan ran = run_waiting(
            &trace, "import^ std.thread\n"
                    "std.thread.sleep(0.03)\n"
                    "let^ h = std.thread.spawn(p^ ... {\n"
                    "    std.thread.sleep(0.05)\n"
                    "    return^ 1\n"
                    "})\n"
                    "if^ h fits^ std.thread.ThreadHandle {\n"
                    "    let^ got = h.join()\n"
                    "    if^ got = 1 { return^ 1 }\n"
                    "}\n"
                    "return^ 0\n");
        const uint32_t lines[] = {2, 8};
        check_wait("std.thread", &trace, ran, lines,
                   sizeof lines / sizeof *lines);
    }

    LHAT_TEST("std.channel demand and supply reach pause points");
    {
        PauseTrace trace = {0};
        LhatTestRan ran = run_waiting(
            &trace, "import^ std.channel\n"
                    "import^ std.thread\n"
                    "let^ c = std.channel.new()\n"
                    "if^ c fits^ std.channel.Channel {\n"
                    "    let^ empty = c.demand(0.03)\n"
                    "    let^ h = std.thread.spawn(p^ ... {\n"
                    "        let^ mine = ...[1]\n"
                    "        std.thread.sleep(0.04)\n"
                    "        if^ mine fits^ std.channel.Channel { mine.pop() }\n"
                    "    }, c)\n"
                    "    if^ h fits^ std.thread.ThreadHandle {\n"
                    "        let^ supplied = c.supply(7)\n"
                    "        h.join()\n"
                    "        if^ supplied fits^ bool^ and^ supplied {\n"
                    "            return^ 1\n"
                    "        }\n"
                    "    }\n"
                    "}\n"
                    "return^ 0\n");
        const uint32_t lines[] = {5, 12};
        check_wait("std.channel", &trace, ran, lines,
                   sizeof lines / sizeof *lines);
    }

    LHAT_TEST("std.task await reaches a pause point while its job waits");
    {
        PauseTrace trace = {0};
        LhatTestRan ran = run_waiting(
            &trace, "import^ std.async\n"
                    "import^ std.task\n"
                    "std.task.start(1) catch^ panic^ it^\n"
                    "let^ job = p^ { yield^ std.async.timer(0.05) return^ 1 }\n"
                    "let^ t = std.task.async(job())\n"
                    "if^ t fits^ std.task.Task {\n"
                    "    let^ got = std.task.await(t)\n"
                    "    std.task.stop()\n"
                    "    if^ got = 1 { return^ 1 }\n"
                    "}\n"
                    "std.task.stop()\n"
                    "return^ 0\n");
        const uint32_t lines[] = {7};
        check_wait("std.task.await", &trace, ran, lines,
                   sizeof lines / sizeof *lines);
    }
}

int main(void)
{
    test_waits_pause();
    return lhat_test_report("test_debug_pause");
}
