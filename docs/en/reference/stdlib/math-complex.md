# std.math.complex

Complex numbers.

## Import

```lhat
import^std.math.complex
```

## Usage and rules

`new(re, im)` takes real and imaginary parts. Complex `*` performs complex multiplication. Normalizing zero leaves it zero.

## Types

### Complex

Host-value components are writable 32-bit floats, read and written from L^ as `number^`. Use `box^` when retaining these values in ordinary tables or similar storage.

| Fields | Type |
| --- | --- |
| `re` | `number^` (f32) |
| `im` | `number^` (f32) |

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### new

Construct a value from components.

```text
f^number^, number^ -> std.math.complex.Complex;
```

### zero

Zero value.

```text
f^ -> std.math.complex.Complex;
```

### one

The value with real part 1 and imaginary part 0.

```text
f^ -> std.math.complex.Complex;
```

### i

Imaginary unit with real part 0 and imaginary part 1.

```text
f^ -> std.math.complex.Complex;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Complex.+

Addition.

```text
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
```

### Complex.-

Subtraction or unary negation.

```text
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
f^self^ -> std.math.complex.Complex;
```

### Complex.*

Multiplication; supported operand types are listed in the signatures.

```text
f^self^, number^ -> std.math.complex.Complex;
f^number^, self^ -> std.math.complex.Complex;
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
```

### Complex.conjugate

Conjugate.

```text
f^self^ -> std.math.complex.Complex;
```

### Complex.length

Length.

```text
f^self^ -> number^;
```

### Complex.normalized

Normalized value.

```text
f^self^ -> std.math.complex.Complex;
```

### Complex.tostring

String representation.

```text
f^self^ -> string^;
```

## Implementation and registration

[mathcomplex.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.h) / [mathcomplex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
