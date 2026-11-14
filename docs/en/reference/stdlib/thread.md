# std.thread

Individual OS threads.

## Import

```lhat
import^std.thread
```

## Example

<<< @/../sample/docs/stdlib/thread.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/thread.lh
```

## Usage and rules

`spawn(procedure, arguments…)` runs a procedure on a new thread, transporting arguments and captured values.

`sleep` uses seconds. `join()` waits for completion; disposing an unjoined handle also waits for the thread.

## Types

### ThreadError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `ThreadError.NotSpawnable`
- `ThreadError.BadArgument`
- `ThreadError.SpawnFailed`
- `ThreadError.BadResult`
- `ThreadError.AlreadyJoined`

### ThreadHandle

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### spawn

Start a new thread with a procedure and arguments.

```text
f^p^... -> any^;, ...:any^ -> std.thread.ThreadHandle|std.thread.ThreadError.NotSpawnable|std.thread.ThreadError.BadArgument|std.thread.ThreadError.SpawnFailed|std.sys.Error.OutOfMemory;
```

### sleep

Sleep the calling thread for the supplied seconds.

```text
p^number^;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### ThreadHandle.join

Wait and collect the result; another collection returns `AlreadyJoined`.

```text
f^self^ -> any^|std.thread.ThreadError.AlreadyJoined|std.thread.ThreadError.BadResult|std.sys.Error.OutOfMemory;
```

### ThreadHandle.done

Test completion.

```text
f^self^ -> bool^;
```

### ThreadHandle.awaitable

A `std.async` completion ID.

```text
f^self^ -> number^;
```

### ThreadHandle.failed

Failure description, or `nil^`.

```text
f^self^ -> string^|nil^;
```

### ThreadHandle.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[thread.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/thread.h) / [thread.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/thread.c)
