# std.lton

Configuration data using L^ notation.

## Import

```lhat
import^std.lton
```

## Example

<<< @/../sample/docs/stdlib/lton.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/lton.lh
```

## Usage and rules

LTON contains the inside of a table literal, optionally preceded by imports. Reading it does not permit procedure calls.

Writing supports plain data tables, finite numbers, strings, booleans, and public enums. Cycles are errors; shared children are expanded.

## Types

### LtonError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `LtonError.CannotRead`
- `LtonError.Rejected`
- `LtonError.Unsupported`
- `LtonError.Cycle`
- `LtonError.TooDeep`
- `LtonError.CannotWrite`

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### parse

Read a table from text.

```text
f^string^ -> t^{}|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### load

Read a table from a path.

```text
f^string^ -> t^{}|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### stringify

Write a table as LTON text.

```text
f^t^{} -> string^|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### save

Save a table to the supplied path.

```text
p^string^,t^{} -> nil^|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

## Implementation and registration

[lton.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/lton.h) / [lton.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/lton.c)
