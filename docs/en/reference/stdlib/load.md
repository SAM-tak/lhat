# std.load

Runtime source loading.

## Import

```lhat
import^std.load
```

## Usage and rules

`file(path)` and `text(source, name)` return a callable procedure; call it to run the loaded body.

Unlike `require^`, each load compiles anew. Modules return their exports table; scripts return their own `return^` result.

The second argument of `text` supplies the diagnostic name and base for relative `require^` paths.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.CannotRead`
- `Error.Rejected`

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### file

Load source from a runtime path.

```text
p^string^ -> p^... -> any^; | std.load.Error;
```

### text

Load source text with a supplied name.

```text
p^string^, string^ -> p^... -> any^; | std.load.Error;
```

## Implementation and registration

[load.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/load.h) / [load.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/load.c)
