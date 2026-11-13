# std.stream

In-memory byte streams.

## Import

```lhat
import^std.stream
```

## Usage and rules

`MemoryReader.read(buffer)` returns the bytes read, or 0 at end of input. Obtain written data with `MemoryWriter.toString()`.

Using a disposed stream returns `Error.Disposed`.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.Disposed`

### MemoryReader

A host-exposed type, created and used through the APIs below.

### MemoryWriter

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### reader

Create a reader from a string or `Bytes`.

```text
f^string^|std.binary.Bytes -> std.stream.MemoryReader|std.stream.Error|std.sys.Error.OutOfMemory;
```

### writer

Create an empty memory writer.

```text
f^ -> std.stream.MemoryWriter|std.sys.Error.OutOfMemory;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### MemoryReader.read

Read into the supplied buffer and return the bytes read.

```text
p^self^,std.binary.Bytes -> number^|std.stream.Error;
```

### MemoryWriter.write

Append a byte buffer.

```text
p^self^,std.binary.Bytes -> nil^|std.stream.Error|std.sys.Error.OutOfMemory;
```

### MemoryWriter.toString

Get written data as a string.

```text
f^self^ -> string^|std.stream.Error|std.sys.Error.OutOfMemory;
```

### MemoryReader.dispose

Release the reference to the resource.

```text
p^self^;
```

### MemoryWriter.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[stream.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/stream.h) / [stream.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/stream.c)
