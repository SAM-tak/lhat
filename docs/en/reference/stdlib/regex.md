# std.regex

Regular expressions.

## Import

```lhat
import^std.regex
```

## Example

<<< @/../sample/docs/stdlib/regex.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/regex.lh
```

## Usage and rules

Compile reused patterns with `new(pattern)`. Module-level `match`, `gmatch`, and `gsub` take `(pattern, subject, …)`.

Capture 0 is the whole match; higher numbers are groups. Replacement strings support `$1` and similar references. Exhausting the search budget returns `Error.Exhausted`, or ends a `gmatch` walk.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.BadPattern`
- `Error.Exhausted`

### Regex

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Compile a pattern.

```text
f^string^ -> std.regex.Regex|std.regex.Error.BadPattern;
```

### match

Return the first match, or `nil^` if none.

```text
f^string^, string^ -> string^|nil^|std.regex.Error.BadPattern|std.regex.Error.Exhausted;
```

### gmatch

Create an iterator yielding match ordinals and strings.

```text
f^string^, string^ -> c^{f^ -> number^, string^ -> nil^}|std.regex.Error.BadPattern;
```

### gsub

Replace matches using a string or function.

```text
f^string^, string^, string^|f^string^, number^, t^{string^[]} -> string^|nil^; -> string^|std.regex.Error.BadPattern|std.regex.Error.Exhausted;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Regex.match

Find the first match of a compiled pattern.

```text
f^self^, string^ -> string^|nil^|std.regex.Error.Exhausted;
```

### Regex.captures

Return the whole match and captured groups, or `nil^` if none.

```text
f^self^, string^ -> t^{string^[]}|nil^|std.regex.Error.Exhausted;
```

### Regex.gmatch

Iterate matches of a compiled pattern.

```text
f^self^, string^ -> c^{f^ -> number^, string^ -> nil^};
```

### Regex.gsub

Replace compiled-pattern matches; callbacks receive the match, ordinal, and captures.

```text
f^self^, string^, string^|f^string^, number^, t^{string^[]} -> string^|nil^; -> string^|std.regex.Error.Exhausted;
```

### Regex.split

Collect text between matches into an array, retaining empty pieces.

```text
f^self^, string^ -> t^{string^[]}|std.regex.Error.Exhausted;
```

### Regex.dispose

Release the reference to the resource.

```text
p^self^;
```

## Implementation and registration

[regex.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/regex.h) / [regex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/regex.c)
