# Concurrency

`std.task` is a standard library for running multiple jobs concurrently on worker threads.
Submit work with `async` and collect results with `await`. Multiple CPU cores allow computations to run in parallel.

This page uses the Tower of Hanoi to move from sequential computation to task splitting.
These examples count moves by recursively solving subproblems; they do not perform or display the disk moves.
The left and right subproblems are independent computations, so their results can be calculated concurrently and added together.

## Start with sequential computation

`sample/hanoi.lh` is a short program that counts the moves required for 24 disks.
`this^` refers to the function currently executing.

<<< @/../sample/hanoi.lh{lhat}

It recursively solves the left and right subproblems in sequence, adding the single move between them.
The result is `16777215` (2 to the power of 24, minus 1).

## Split the work into two tasks

`sample/hanoi2.lh` assigns the left and right computations to two tasks.

<<< @/../sample/hanoi2.lh{lhat}

| Call | Purpose |
| --- | --- |
| `std.task.start(2)` | Start two worker threads |
| `std.task.async(...)` | Submit work and return a `Task` for collecting its result |
| `std.task.await(task)` | Wait for completion and collect the job's return value |
| `std.task.stop()` | Stop the workers and clean up |

The `_yield^0` in `half` marks its call as producing an unstarted coroutine.
Calling `half(...)` does not complete the computation immediately; the job passed to `async` runs on a worker.
`_yield^` itself does not suspend execution at runtime, and `0` is not the computation's result.
`async` also accepts a function or procedure that can be called without arguments directly.

The key is to **submit both jobs with `async` before calling `await`**.
While waiting for `left`, `right` can also make progress. The result is `16777215`, just as in the sequential version.
Each `try^` propagates errors from starting workers, submitting work, or collecting results to the caller.
A given `Task`'s result can only be collected once.

## Separate task count from worker count

`sample/hanoi3.lh` splits the top five levels of recursion into 32 tasks and distributes them across six worker threads.

<<< @/../sample/hanoi3.lh{lhat}

`split` submits subproblems and adds the single move between each split to `moves`.
Each task computes the subproblem supplied through its arguments; the final loop collects their results.
`Task<number^>` is the type of a task whose result is a number.

**You do not need a thread for every task.** Workers process queued jobs in turn.
`workers` sets the worker count, while `depth` controls how finely the work is split.
This example creates 2 to the power of `depth` tasks.
Splitting too finely increases the overhead of creating and handing off tasks, so adjust it to suit the workload.

## Run the examples

Run these commands from the repository root. All three print `16777215`.

```sh
lhat sample/hanoi.lh
lhat sample/hanoi2.lh
lhat sample/hanoi3.lh
```

Use the same Release build when comparing speeds.
Actual performance depends on the number of cores, the amount of computation, and how the work is split.
