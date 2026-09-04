// L^ (lhat) -- sample standard library: std.task.
//
// A pool of worker machines. One function a host calls once, before
// lhat_program_check, the same as any of program.h's own registrations
// (05 の 8.7). Needs an OS thread implementation: port/thread.h. A host
// linking this must also link lhatthread.
//
// std.thread starts a machine per call, which costs a machine and an OS
// thread every time; std.task keeps N of them standing and hands them work:
//
//     std.task.start(6)                 # six workers. Nothing said: one per core
//
//     let^ t1 = std.task.async(slow(1))  # slow is yieldable, so slow(1) is a
//     let^ t2 = std.task.async(slow(2))  # coroutine that has not started
//     let^ a = std.task.await(t1)        # the two ran side by side
//     let^ b = std.task.await(t2)
//
// A JOB IS EITHER of two things. A coroutine whose body has not started
// (05 の 8.8改3) -- which is what `slow(1)` is, since 02 の 15.5 runs
// nothing at the call -- or a `p^...` closure, with what follows it handed
// over as its arguments (std.thread.spawn's shape). What crosses is what
// carry.h carries; anything else answers TaskError.Refused with carry's own
// reason for it.
//
// A JOB RUNS TO COMPLETION on the worker that took it. It never moves to
// another, and no other job of the pool runs on that worker while it does.
// It is run in slices all the same (02 の 15.15), which is what lets a stop
// through: a job that neither yields nor ends is put down between two of
// them and its worker goes, machine and all.
//
// Inside a job, `await^` works: the worker drives
// the coroutine and waits on std.async for whatever it yielded, so a job
// may await a timer, a thread, or anything else a host completes. That
// takes std.async registered on the program; without it a job that yields
// answers TaskError.Failed rather than waiting for something nobody drives.
//
// await() stops the machine that calls it. A caller with a loop of its own
// asks `t.done()`, or parks a scheduler on `t.awaitable()` -- the same push
// std.thread's handle answers with (15.14改).
//
// A Task may cross machines (05 の 8.8改2), so a job may be handed the Task
// of another job, and a Task may be pushed into a std.channel.
//
// THE POOL HOLDS THE PROGRAM'S BODIES. A worker runs protos the program
// owns, so the pool has to stop before the program is disposed of.
// std.task.stop() is that, and this module's own disposal does it as a
// backstop -- but a host doing anything else with the program first should
// call it itself.

#ifndef LHATSTDLIB_TASK_H
#define LHATSTDLIB_TASK_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_task_register(LhatProgram *program);

// Stops the pool: no more work is taken, what is queued is dropped, and
// every worker is joined. What std.task.stop() does, for a host that has to
// do it from C -- before a reload, before disposing of the program.
// Answers when the last worker has gone.
void lhatstdlib_task_stop(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_TASK_H
