# std.random

Random numbers and reproducible state.

## Import

```lhat
import^std.random
```

## Example

<<< @/../sample/docs/stdlib/random.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/random.lh
```

## Usage and rules

`new(seed)` produces the same sequence for the same seed, including zero. With no argument, seeding uses the OS. This is not a cryptographic generator.

Preserve saved states as integers; floating-point conversion can lose bits. State compatibility depends on the generator algorithm version.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.InvalidState`
- `Error.InvalidArgument`
- `Error.SeedUnavailable`

### Random

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Create a generator from the OS or a supplied seed.

```text
f^ -> std.random.Random|std.random.Error.SeedUnavailable|std.sys.Error.OutOfMemory;
f^number^ -> std.random.Random|std.sys.Error.OutOfMemory;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Random.reseed

Restart the sequence from a seed.

```text
p^self^, number^;
```

### Random.save

Read the integer state without advancing.

```text
f^self^ -> number^;
```

### Random.load

Restore a saved integer state.

```text
p^self^, number^ -> nil^|std.random.Error.InvalidState;
```

### Random.next

Draw a uniform real in `[0, 1)` and advance the state.

```text
f^self^ -> number^;
```

### Random.rangeint

Integer draw including both lower and upper bounds.

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.uniform

Uniform real draw in `[lower, upper)`.

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.normal

Normal draw with `(mean, standard deviation)`.

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[random.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/random.h) / [random.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/random.c)
