# std.async

Timers and external completion waits.

## Import

```lhat
import^std.async
```

## Example

<<< @/../sample/docs/stdlib/async.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/async.lh
```

## Usage and rules

IDs are managed per machine. Delays and wait limits use seconds. `wait(0)` polls ready completions without sleeping.

The host signals completion for `external()`. This module does not itself schedule tasks.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### timer

Create a wait ID ready after a delay in seconds.

```text
f^number^ -> number^;
```

### external

Create an externally completed wait ID.

```text
f^ -> number^;
```

### drop

Drop a wait ID.

```text
p^number^;
```

### wait

Wait up to the supplied seconds, returning a ready ID or `nil^`.

```text
f^number^ -> number^|nil^;
```

### next

Seconds until the next deadline, or `nil^`.

```text
f^ -> number^|nil^;
```

### pending

Number of outstanding waits.

```text
f^ -> number^;
```

## Implementation and registration

[async.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/async.h) / [async.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/async.c)
