# std.math.vector2

2D vectors.

## Import

```lhat
import^std.math.vector2
```

## Usage and rules

Vector `*` and `/` are component-wise; numeric operands scale values. `lerp` does not clamp its factor to 0–1. Normalizing a zero vector leaves it zero.

Coordinates are +x right and +y down. `angle()` is `atan2(y, x)`; `rotate(radians)` takes radians.

## Types

### Vector2

Host-value components are writable 32-bit floats, read and written from L^ as `number^`. Use `box^` when retaining these values in ordinary tables or similar storage.

| Fields | Type |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Construct a value from components.

```text
f^number^, number^ -> std.math.vector2.Vector2;
```

### zero

Zero value.

```text
f^ -> std.math.vector2.Vector2;
```

### one

Value with every component equal to one.

```text
f^ -> std.math.vector2.Vector2;
```

### right

Right unit vector.

```text
f^ -> std.math.vector2.Vector2;
```

### left

Left unit vector.

```text
f^ -> std.math.vector2.Vector2;
```

### up

Up unit vector.

```text
f^ -> std.math.vector2.Vector2;
```

### down

Down unit vector.

```text
f^ -> std.math.vector2.Vector2;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Vector2.+

Addition.

```text
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.-

Subtraction or unary negation.

```text
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.*

Multiplication; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector2.Vector2;
f^number^, self^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2./

Division; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.lerp

Linear interpolation with `(other, factor)`.

```text
f^self^, std.math.vector2.Vector2, number^ -> std.math.vector2.Vector2;
```

### Vector2.clamp

Clamp components between lower and upper bounds.

```text
f^self^, number^, number^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.angle

Angle.

```text
f^self^ -> number^;
```

### Vector2.rotate

Rotate a vector.

```text
f^self^, number^ -> std.math.vector2.Vector2;
```

### Vector2.dot

Dot product.

```text
f^self^, std.math.vector2.Vector2 -> number^;
```

### Vector2.cross

Cross product.

```text
f^self^, std.math.vector2.Vector2 -> number^;
```

### Vector2.abs

Absolute value.

```text
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.length

Length.

```text
f^self^ -> number^;
```

### Vector2.normalized

Normalized value.

```text
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.tostring

String representation.

```text
f^self^ -> string^;
```

## Implementation and registration

[mathvector2.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector2.h) / [mathvector2.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector2.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
