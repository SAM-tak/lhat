# std.math.quaternion

Quaternions.

## Import

```lhat
import^std.math.quaternion
```

## Example

`std.math.quaternion.Quaternion` is the value type. To construct a value from components, use `std.math.quaternion.new(...)` directly on the module. Aliasing the module also allows `alias.new(...)`.

You can also alias the type for annotations. The constructor `new` belongs to the module; `std.math.quaternion.Quaternion.new(...)` and `new^std.math.quaternion.Quaternion{…}` are not supported construction forms.

<<< @/../sample/docs/stdlib/math-quaternion.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/math-quaternion.lh
```

## Usage and rules

In `new(x, y, z, w)`, `w` is the scalar part. Quaternion `*` is the Hamilton product. `fromAxisAngle(axis, radians)` takes radians and returns `nil^` for a zero axis. Registration of `std.math.vector3` is required.

## Types

### Quaternion

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
f^number^, number^, number^, number^ -> std.math.quaternion.Quaternion;
```

### identity

Identity rotation.

```text
f^ -> std.math.quaternion.Quaternion;
```

### fromTo

Rotation between two direction vectors; zero directions return `nil^`.

```text
f^std.math.vector3.Vector3, std.math.vector3.Vector3 -> std.math.quaternion.Quaternion|nil^;
```

### fromAxisAngle

Construct a rotation from an axis and angle.

```text
f^std.math.vector3.Vector3, number^ -> std.math.quaternion.Quaternion|nil^;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Quaternion.+

Addition.

```text
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
```

### Quaternion.-

Subtraction or unary negation.

```text
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.*

Multiplication; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.quaternion.Quaternion;
f^number^, self^ -> std.math.quaternion.Quaternion;
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
```

### Quaternion.slerp

Spherical interpolation with `(other, factor)`.

```text
f^self^, std.math.quaternion.Quaternion, number^ -> std.math.quaternion.Quaternion;
```

### Quaternion.conjugate

Conjugate.

```text
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.dot

Dot product.

```text
f^self^, std.math.quaternion.Quaternion -> number^;
```

### Quaternion.length

Length.

```text
f^self^ -> number^;
```

### Quaternion.normalized

Normalized value.

```text
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.tostring

String representation.

```text
f^self^ -> string^;
```

### Quaternion.rotate

Rotate a vector.

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

## Implementation and registration

[mathquaternion.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathquaternion.h) / [mathquaternion.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathquaternion.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
