# std.channel

Value queues between machines.

## Import

```lhat
import^std.channel
```

## Usage and rules

`new(T)` creates `Channel<T>`; `named(name, T)` accesses a shared named channel. Untyped forms are also available.

`demand` and `supply` timeouts use seconds. Inside `atomic`, they do not wait and act like `pop` and `push`.

Untransportable values or mismatched types for an existing named channel return `ChannelError.Refused`.

## Types

### ChannelError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `ChannelError.Refused`

### Channel

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Create an independent channel.

```text
p^ -> std.channel.Channel|std.sys.Error.OutOfMemory;
p^type^ -> std.channel.Channel<ARG0.T0>|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### named

Access a channel shared by name.

```text
p^string^ -> std.channel.Channel|std.sys.Error.OutOfMemory;
p^string^, type^ -> std.channel.Channel<ARG1.T0>|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Channel.push

Enqueue a value and return its sequence number.

```text
p^self^, self^.T0 -> number^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### Channel.supply

Enqueue and wait for consumption; an optional second argument is the timeout in seconds.

```text
p^self^, self^.T0 -> bool^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
p^self^, self^.T0, number^ -> bool^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### Channel.pop

Take the first value, or `nil^` if empty.

```text
p^self^ -> self^.T0|nil^;
```

### Channel.demand

Wait for a value; timeout and unavailable results produce `nil^`.

```text
p^self^ -> self^.T0|nil^;
p^self^, number^ -> self^.T0|nil^;
```

### Channel.awaitable

A `std.async` ID for readability.

```text
f^self^ -> number^;
```

### Channel.take

Create a coroutine awaiting a value.

```text
f^self^ -> c^{f^ -> number^ -> self^.T0|nil^};
```

### Channel.peek

Read the first value without removing it.

```text
p^self^ -> self^.T0|nil^;
```

### Channel.count

Number of queued values.

```text
f^self^ -> number^;
```

### Channel.hasRead

Test whether a submitted sequence number has been consumed.

```text
f^self^, number^ -> bool^;
```

### Channel.clear

Empty the queue.

```text
p^self^;
```

### Channel.atomic

Hold the channel lock while calling a procedure with it.

```text
p^self^, p^std.channel.Channel<self^.T0>;;
```

### Channel.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[channel.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/channel.h) / [channel.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/channel.c)
