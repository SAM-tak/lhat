# std.io

Standard I/O and files.

## Import

```lhat
import^std.io
```

## Example

<<< @/../sample/docs/stdlib/io.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/io.lh
```

## Usage and rules

`open(path, mode)` uses C file modes such as `"r"`, `"w"`, and `"rb"`. Close files with `with^` or `dispose()`.

## Types

### IOError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `IOError.NotFound`
- `IOError.Denied`
- `IOError.Eof`

### File

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### print

Print values to standard output.

```text
p^...;
```

### readLine

Read a line from standard input; end of input returns `IOError.Eof`.

```text
f^ -> string^|std.io.IOError.Eof|std.sys.Error.OutOfMemory;
```

### open

Open a file with a path and mode.

```text
f^string^, string^ -> std.io.File|std.io.IOError.NotFound|std.io.IOError.Denied|std.sys.Error.OutOfMemory;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### File.readLine

Read a file line; end of input returns `IOError.Eof`.

```text
f^self^ -> string^|std.io.IOError.Eof|std.sys.Error.OutOfMemory;
```

### File.readAll

Read the remainder of the file as a string.

```text
f^self^ -> string^|std.sys.Error.OutOfMemory;
```

### File.write

Write a string.

```text
p^self^, string^;
```

### File.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[io.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.h) / [io.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.c)
