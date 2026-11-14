# std.sys

Shared system errors.

## Import

```lhat
import^std.sys
```

## Example

This example constructs an error value explicitly to demonstrate handling; it does not exhaust memory.

<<< @/../sample/docs/stdlib/sys.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/sys.lh
```

## Usage and rules

`std.sys.Error.OutOfMemory` is the out-of-memory type shared by multiple libraries. This module has no standalone functions.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.OutOfMemory`

## Implementation and registration

[sys.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/sys.h) / [sys.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/sys.c)
