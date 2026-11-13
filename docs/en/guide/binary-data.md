# Binary data and streams

`std.binary` saves and restores tables without an explicit schema. These module-level
`encode/decode` functions are distinct from the existing fixed-bit `Format.encode/decode`.
They also work in VMOnly builds without the front end.

## Saving in memory

<<< @/../sample/docs/binary-data.lh{lhat}

The output is `player 12`, `player 12`, then `next 13`.
`encode` returns a binary `string^`; `decode` accepts a `string^` or `std.binary.Bytes`.
The restored type is `t^{}`. Check its expected shape with `fits^` before using it.

## Readers and Writers

`serialize(writer, table)` writes incrementally; `deserialize(reader)` reads exactly one record.
Neither closes the stream, so records can be concatenated.
No common base class is required: pass a table or host type with these methods.

| Method | Contract |
| --- | --- |
| `read(buffer)` | Read up to the buffer's current length, set its length to the actual count, and return that count. 0 means EOF |
| `write(buffer)` | Consume all bytes before returning `nil^`, or return an error value |

Buffers are `std.binary.Bytes`. `resize(n)` sets the request size and zero-fills extensions.
Short reads are allowed. Methods must not retain the buffer after returning.
Both operations advance state and are `p^` procedures.

`std.stream.writer()` accumulates in memory; `toString()` copies the complete output.
`std.stream.reader(bytes)` owns a copy of its input.
These memory streams are the currently implemented stream types. Connect files or
custom storage through host-provided Readers/Writers or C callbacks.
Compression and encryption wrappers are planned extensions of the same contract.

## Supported values and failures

Plain tables, integers, finite reals, booleans, `nil^`, and arbitrary byte strings are supported.
Array and dictionary parts are distinct, preserving nil array positions.
Dictionary keys may be numbers, booleans, or strings. Definitions, instances, enums,
functions, errors, and host values cannot be saved.
Cycles are rejected; shared child tables are restored as independent copies.
Game-specific object relationships can instead be represented by IDs.

Failures are `std.binary.DataError` (`Truncated`, `Malformed`, `Unsupported`, `Cycle`,
`Limit`, `IO`) or `std.sys.Error.OutOfMemory`. Reader/Writer error values propagate unchanged.
Default limits are 256 MiB per record, one million array positions plus dictionary entries,
and 96 table levels.

Do not mutate the source during serialization. A failed Writer may contain a partial record.
Streaming avoids a complete intermediate byte string, but the source and restored tables
still require memory.

## Calling from C

`stdlib/binary.h` exposes `lhatstdlib_binary_serialize` and `lhatstdlib_binary_deserialize`.
Pass a Machine and read/write callbacks; no Program or module registration is required.
`LhatBinaryDataLimits` customizes byte and entry limits.
To expose `std.stream` to L^, call `lhatstdlib_stream_register(program)`.
