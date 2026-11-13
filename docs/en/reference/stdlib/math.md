# std.math

Mathematical functions and constants.

## Import

```lhat
import^std.math
```

## Usage and rules

Trigonometric angles are radians. `atan2` takes `(y, x)`; two-argument `log` takes `(value, base)`.

`lerp(a, b, t)` computes `a + (b - a) * t` without clamping `t` to 0–1.

## Constants

| Name | Value |
| --- | --- |
| `pi` | `3.141592653589793` |
| `tau` | `6.283185307179586` |
| `e` | `2.718281828459045` |

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### sin

Sine.

```text
f^number^ -> number^;
```

### cos

Cosine.

```text
f^number^ -> number^;
```

### tan

Tangent.

```text
f^number^ -> number^;
```

### asin

Inverse sine.

```text
f^number^ -> number^;
```

### acos

Inverse cosine.

```text
f^number^ -> number^;
```

### atan

Inverse tangent.

```text
f^number^ -> number^;
```

### atan2

Angle of coordinates `(y, x)`.

```text
f^number^, number^ -> number^;
```

### deg

Convert radians to degrees.

```text
f^number^ -> number^;
```

### rad

Convert degrees to radians.

```text
f^number^ -> number^;
```

### sqrt

Square root.

```text
f^number^ -> number^;
```

### cbrt

Cube root.

```text
f^number^ -> number^;
```

### exp

Natural exponential.

```text
f^number^ -> number^;
```

### log

Natural logarithm, or logarithm in the base supplied as the second argument.

```text
f^number^ -> number^;
f^number^, number^ -> number^;
```

### log2

Base-2 logarithm.

```text
f^number^ -> number^;
```

### log10

Base-10 logarithm.

```text
f^number^ -> number^;
```

### hypot

Euclidean length of two components.

```text
f^number^, number^ -> number^;
```

### fmod

Floating-point remainder.

```text
f^number^, number^ -> number^;
```

### min

Minimum of one or more numbers.

```text
f^number^, ...:number^ -> number^;
```

### max

Maximum of one or more numbers.

```text
f^number^, ...:number^ -> number^;
```

### lerp

Linear interpolation of two values.

```text
f^number^, number^, number^ -> number^;
```

## Implementation and registration

[math.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.h) / [math.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.c)
