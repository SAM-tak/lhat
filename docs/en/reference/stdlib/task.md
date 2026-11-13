# std.task

Concurrent tasks on a worker pool.

## Import

```lhat
import^std.task
```

## Usage and rules

`start(n)` selects the worker count; omission uses the core count. `async` accepts an unstarted coroutine or a no-argument closure.

Captures and arguments cross as snapshots. A result can be collected by `await` exactly once; subsequent attempts return `TaskError.Taken`.

`await` blocks the calling machine. Use `done()` or `awaitable()` with your own event loop. `stop()` drops unfinished jobs and stops workers.

See [Concurrency](../../guide/tasks.md) for examples.

## Types

### TaskError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `TaskError.NotStarted`
- `TaskError.Refused`
- `TaskError.Failed`
- `TaskError.Taken`

### Task

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### start

Start the pool and return its worker count.

```text
p^ -> number^|std.sys.Error.OutOfMemory;
p^number^ -> number^|std.sys.Error.OutOfMemory;
```

### stop

Stop the pool.

```text
p^;
```

### workers

Get the worker count.

```text
f^ -> number^;
```

### async

Submit work and return `Task<T>` preserving its result type.

```text
p^c^{->*->*} -> std.task.Task<ARG0.ResultType>|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
p^(p^;)|(p^->any^;) -> std.task.Task<ARG0.ReturnType>|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
```

### await

Collect a task result exactly once.

```text
p^std.task.Task -> ARG0.T0|std.task.TaskError.Taken|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Task.done

Check completion without consuming the result.

```text
f^self^ -> bool^;
```

### Task.failed

Failure description, or `nil^`.

```text
f^self^ -> string^|nil^;
```

### Task.awaitable

Get a `std.async` ID for completion.

```text
f^self^ -> number^;
```

### Task.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[task.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/task.h) / [task.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/task.c)
