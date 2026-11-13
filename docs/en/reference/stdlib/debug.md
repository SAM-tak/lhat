# std.debug

Debug output and stack information.

## Import

```lhat
import^std.debug
```

## Usage and rules

`log` is exposed as `f^` for debugging and can be called inside functions. It writes to standard output.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### log

Write a debug string followed by a newline.

```text
f^string^ -> nil^;
```

### traceback

Get the current call stack as a string.

```text
f^ -> string^;
```

## Implementation and registration

[debug.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/debug.h) / [debug.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/debug.c)
