# std.binary

Binary data and fixed-bit formats.

## Import

```lhat
import^std.binary
```

## Example

<<< @/../sample/docs/stdlib/binary.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/binary.lh
```

## Usage and rules

Module-level `encode/decode` store data without a schema; `Format.encode/decode` use a typed, fixed-bit layout.

`format(type, layout)` preserves the represented type as `Format<T>`. Format decoding returns a tuple of value and next position.

See [Binary data and streams](../../guide/binary-data.md) for examples.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.Truncated`
- `Error.Malformed`

### DataError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `DataError.Truncated`
- `DataError.Malformed`
- `DataError.Unsupported`
- `DataError.Cycle`
- `DataError.Limit`
- `DataError.IO`

### Kind

A host-exposed type, created and used through the APIs below.

### Format

A host-exposed type, created and used through the APIs below.

`std.binary.Kind` — Members inherited from the base type are also available.

### Bytes

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### uint

Unsigned integer kind with the supplied bit width.

```text
f^number^ -> std.binary.Kind;
```

### int

Signed integer kind with the supplied bit width.

```text
f^number^ -> std.binary.Kind;
```

### bool

Boolean kind.

```text
f^ -> std.binary.Kind;
```

### range

Integer kind bounded by lower and upper values.

```text
f^number^, number^ -> std.binary.Kind;
```

### fixed

Quantized kind with lower bound, upper bound, and step.

```text
f^number^, number^, number^ -> std.binary.Kind;
```

### enum

Enum kind.

```text
f^any^ -> std.binary.Kind;
```

### array

Fixed array specified by element count and kind.

```text
f^number^, std.binary.Kind -> std.binary.Kind;
```

### format

Create a fixed format from a type and member layout.

```text
f^type^, t^{} -> std.binary.Format<ARG0.T0>;
```

### bytes

Create a reusable byte buffer.

```text
f^ -> std.binary.Bytes;
```

### encode

Encode a table as schema-free binary data.

```text
f^t^{} -> string^|std.binary.DataError|std.sys.Error.OutOfMemory;
```

### decode

Decode a table from schema-free binary data.

```text
f^string^|std.binary.Bytes -> t^{}|std.binary.DataError|std.sys.Error.OutOfMemory;
```

### serialize

Serialize a table to a supplied write stream.

```text
p^t^{write:p^self^,std.binary.Bytes -> nil^|error^;},t^{} -> nil^|error^;
```

### deserialize

Deserialize a table from a read stream.

```text
p^t^{read:p^self^,std.binary.Bytes -> number^|error^;} -> t^{}|error^;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Kind.bits

Required bit count.

```text
f^self^ -> number^;
```

### Kind.dispose

Release the reference to the resource.

```text
p^self^;
```

### Format.size

Fixed-format size.

```text
f^self^ -> number^;
```

### Format.encode

Encode a value as a fixed-format string.

```text
f^self^, self^.T0 -> string^;
```

### Format.encodeInto

Encode into an existing `Bytes` buffer.

```text
p^self^, self^.T0, std.binary.Bytes;
```

### Format.decode

Decode data from a starting position, returning value and next position.

```text
f^self^, string^|std.binary.Bytes, number^ -> (self^.T0, number^)|std.binary.Error;
```

### Format.decodeInto

Decode into an existing table and return the next position.

```text
p^self^, string^|std.binary.Bytes, number^, t^{} -> number^|std.binary.Error;
```

### Bytes.size

Buffer size in bytes.

```text
f^self^ -> number^;
```

### Bytes.toString

Convert the buffer to a string.

```text
f^self^ -> string^;
```

### Bytes.dispose

Release the reference to the resource.

```text
p^self^;
```

### Bytes.resize

Resize the buffer.

```text
p^self^, number^;
```

## Implementation and registration

[binary.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary.h) / [binary.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary.c)

[binary_data.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary_data.c)
