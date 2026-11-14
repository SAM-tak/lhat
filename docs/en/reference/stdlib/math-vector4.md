# std.math.vector4

4D vectors.

## Import

```lhat
import^std.math.vector4
```

## Example

`std.math.vector4.Vector4` is the value type. To construct a value from components, use `std.math.vector4.new(...)` directly on the module. Aliasing the module also allows `alias.new(...)`.

You can also alias the type for annotations. The constructor `new` belongs to the module; `std.math.vector4.Vector4.new(...)` and `new^std.math.vector4.Vector4{…}` are not supported construction forms.

<<< @/../sample/docs/stdlib/math-vector4.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/math-vector4.lh
```

## Usage and rules

Vector `*` and `/` are component-wise; numeric operands scale values. `lerp` does not clamp its factor to 0–1. Normalizing a zero vector leaves it zero.

## Types

### Vector4

Host-value components are writable 32-bit floats, read and written from L^ as `number^`. Use `box^` when retaining these values in ordinary tables or similar storage.

| Fields | Type |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |
| `z` | `number^` (f32) |
| `w` | `number^` (f32) |

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Construct a value from components.

```text
f^number^, number^, number^, number^ -> std.math.vector4.Vector4;
```

### zero

Zero value.

```text
f^ -> std.math.vector4.Vector4;
```

### one

Value with every component equal to one.

```text
f^ -> std.math.vector4.Vector4;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Vector4.+

Addition.

```text
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.-

Subtraction or unary negation.

```text
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.*

Multiplication; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector4.Vector4;
f^number^, self^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4./

Division; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.lerp

Linear interpolation with `(other, factor)`.

```text
f^self^, std.math.vector4.Vector4, number^ -> std.math.vector4.Vector4;
```

### Vector4.clamp

Clamp components between lower and upper bounds.

```text
f^self^, number^, number^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.dot

Dot product.

```text
f^self^, std.math.vector4.Vector4 -> number^;
```

### Vector4.abs

Absolute value.

```text
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.length

Length.

```text
f^self^ -> number^;
```

### Vector4.normalized

Normalized value.

```text
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.tostring

String representation.

```text
f^self^ -> string^;
```

## Implementation and registration

[mathvector4.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector4.h) / [mathvector4.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector4.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
