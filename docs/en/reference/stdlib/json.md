# std.json

JSON encoding and decoding.

## Import

```lhat
import^std.json
```

## Usage and rules

A nonempty table containing only an array part becomes a JSON array; other tables become objects. An empty table becomes `{}`.

Cycles, unsupported values, and nonfinite numbers produce errors. `pretty` accepts an indentation width from 0 to 16.

## Types

### JsonError

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `JsonError.BadText`
- `JsonError.Unsupported`
- `JsonError.TooDeep`

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### encode

Encode a table as JSON.

```text
f^t^{} -> string^|std.json.JsonError|std.sys.Error.OutOfMemory;
```

### pretty

Encode a table as formatted JSON with the given indentation width.

```text
f^t^{}, number^ -> string^|std.json.JsonError|std.sys.Error.OutOfMemory;
```

### decode

Decode JSON into a table.

```text
f^string^ -> t^{}|std.json.JsonError|std.sys.Error.OutOfMemory;
```

## Implementation and registration

[json.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/json.h) / [json.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/json.c)
