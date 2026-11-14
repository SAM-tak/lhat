# std.math.vector3

3D vectors.

## Import

```lhat
import^std.math.vector3
```

## Example

`std.math.vector3.Vector3` is the value type. To construct a value from components, use `std.math.vector3.new(...)` directly on the module. Aliasing the module also allows `alias.new(...)`.

You can also alias the type for annotations. The constructor `new` belongs to the module; `std.math.vector3.Vector3.new(...)` and `new^std.math.vector3.Vector3{…}` are not supported construction forms.

<<< @/../sample/docs/stdlib/math-vector3.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/math-vector3.lh
```

## Usage and rules

Vector `*` and `/` are component-wise; numeric operands scale values. `lerp` does not clamp its factor to 0–1. Normalizing a zero vector leaves it zero.

+Z is forward and −Z is back.

## Types

### Vector3

Host-value components are writable 32-bit floats, read and written from L^ as `number^`. Use `box^` when retaining these values in ordinary tables or similar storage.

| Fields | Type |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |
| `z` | `number^` (f32) |

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Construct a value from components.

```text
f^number^, number^, number^ -> std.math.vector3.Vector3;
```

### zero

Zero value.

```text
f^ -> std.math.vector3.Vector3;
```

### one

Value with every component equal to one.

```text
f^ -> std.math.vector3.Vector3;
```

### right

Right unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

### left

Left unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

### up

Up unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

### down

Down unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

### forward

Forward unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

### back

Back unit vector.

```text
f^ -> std.math.vector3.Vector3;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Vector3.+

Addition.

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.-

Subtraction or unary negation.

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.*

Multiplication; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector3.Vector3;
f^number^, self^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3./

Division; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.lerp

Linear interpolation with `(other, factor)`.

```text
f^self^, std.math.vector3.Vector3, number^ -> std.math.vector3.Vector3;
```

### Vector3.clamp

Clamp components between lower and upper bounds.

```text
f^self^, number^, number^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.dot

Dot product.

```text
f^self^, std.math.vector3.Vector3 -> number^;
```

### Vector3.cross

Cross product.

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.abs

Absolute value.

```text
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.length

Length.

```text
f^self^ -> number^;
```

### Vector3.normalized

Normalized value.

```text
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.tostring

String representation.

```text
f^self^ -> string^;
```

## Implementation and registration

[mathvector3.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.h) / [mathvector3.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
