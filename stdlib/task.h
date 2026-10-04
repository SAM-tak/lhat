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
// A job is an unstarted coroutine or a closure called without arguments.
// Captures and coroutine arguments cross through carry.h as snapshots.
// A coroutine that cannot cross answers TaskError.Refused.
//
// Each task owns a machine. Workers run one slice and requeue unfinished
// tasks; a later slice can run on another worker. Async waits park the task
// and leave the worker available. Stopping the pool drops unfinished jobs.
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
// Completion is claimed exactly once, including failure or refusal. A later
// await, even on another machine, answers TaskError.Taken. There is no take
// member. Successful reference results transfer ownership, never copy.
// Plain tables, strings, errors and program closures with closed captures
// can move; machine-bound resources and other unsupported objects answer
// TaskError.Refused. Cycles and shared children retain their identity.
// An unclaimed result is freed with the Task. Claiming it removes the Task's
// ownership, so disposing the Task cannot destroy the received result.
//
// A Task may cross machines (05 の 8.8改2), so a job may be handed the Task
// of another job, and a Task may be pushed into a std.channel.
//
// The signature preserves the final result T as Task<T>, also writable in
// source annotations. await answers T plus task errors;
// yield types do not enter T. No result becomes nil. Multi-value and direct
// host-value results are refused because await transports one ordinary value.
// Explicitly erased Task annotations retain the any fallback.
// Static preservation does not extend what the result transfer can move.
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
