# Numbers and strings

## Numeric representation and comparison

The single type `number^` has integer and floating-point representations.
Arithmetic stays integer when possible; overflow in `+`, `-`, `*`, and unary `-` widens to floating point.
`**` always produces a floating-point result and accepts non-integer exponents. A negative base with a non-integer exponent can produce NaN.
`number^.inf` is positive infinity and `number^.nan` is NaN. Mathematical constants are `std.math.pi`, `tau`, and `e`.

Two integers compare exactly. Numeric equality and ordering involving floating-point values use this tolerance:

```text
abs(a - b) <= (2 ** -52 * 8) * max(abs(a), abs(b), 1)
```

Values within this band compare as neither less nor greater. Approximate equality is not transitive.
Table keys use exact comparison without this tolerance. NaN is unequal to itself.

## Numeric built-ins

These require no import. There are no hat spellings such as `floor^`.

| Call | Result |
| --- | --- |
| `n.tostring()` | Text distinguishing integer and floating-point spellings |
| `n.tostring(format)` | Text using the [format rules](./lexical.md#interpolation-evaluation-and-formats) |
| `n.eq(other, tolerance)` | Compare within tolerance multiplied by `max(abs(a), abs(b), 1)`; zero means exact |
| `n.floor()` | Greatest integer at most n |
| `n.ceil()` | Least integer at least n |
| `n.round()` | Nearest integer, ties to even |
| `n.abs()` | Absolute value |
| `n.sign()` | −1, 0, or 1 |
| `n.clamp(lower, upper)` | Constrain to the interval |
| `n.isinteger()` | Whether the value is integral and fits a signed 64-bit integer |
| `n.bitany(mask)` | Whether any masked bit is set |
| `n.bitall(mask)` | Whether all masked bits are set |
| `n.bitat(position)` | Bit position, with 0 least significant; false outside 0–63 |

`(2.5).round() = 2` and `(3.5).round() = 4`. This differs from string-index rounding.
Very large floating-point values, infinities, and NaN remain unchanged by rounding.

## Bit operations

`bitand^`, `bitor^`, `bitxor^`, `bitshift^`, and prefix `bitnot^` operate on 64-bit integers.
An integral floating-point value such as `8.0` is accepted; fractions, out-of-range values, infinities, and NaN stop execution.
Results do not widen to floating point; overflowing bits are discarded.
`a bitshift^ n` shifts left for positive n and logically right for negative n, filling upper bits with zero.
A shift whose absolute count is at least 64 produces zero. `&`, `|`, `<<`, and `>>` are not bit operators.

## Conversion to strings

Every value has `tostring^()`. Ordinary values, definitions, and instances also expose the same member as `tostring()`.
On a plain table, bare `tostring` is a user key; the built-in is `tostring^()`.
`nil^` and booleans produce `nil`, `true`, and `false`; a string returns itself without quotes.
Values inside tables use table notation.

Interpolation calls `tostring^`. A replacement must conform to the pure signature `f^self^ -> string^;`.
`..` does not convert values to strings implicitly.

## Conversion from strings to numbers

`s.tonumber()` trims surrounding whitespace and reads one optional sign followed by a complete L^ numeric literal.
It supports `0x`, `0b`, `0o`, digit separators, fractions, and exponents; a mismatch returns `nil^`.
Comments and extra text after the number are not accepted.

`s.tonumber(format)` uses C `scanf`-style input.
The format contains exactly one numeric conversion from `d i o u x X f F e E g G a A`, optionally with literal text and `%%`.
Input width is allowed; output flags, precision, `*`, and length modifiers are not.
The format must match the entire input. An invalid format panics; a valid format with mismatched input returns `nil^`.
`"ff".tonumber("%x")` returns 255.

## Characters and bytes

`length` and `len` count extended grapheme clusters; `size` counts bytes.
Character ordinals start at 0, ranges include both ends, and negative ordinals count from the end.
Fractional positions round with `floor(x + 0.5)`. An invalid range produces an empty string.
There is no character type: `at()` also returns a string. `s[0]` does not access a character.

| Call | Meaning |
| --- | --- |
| `s.substring(begin[, end])`, `substr`, `sub` | Equivalent inclusive slices; omitted end means the end of the string |
| `s.at(position)` | Same as `substr(position, position)` |
| `s.chars()` | Iterate extended grapheme-cluster strings |
| `s.codepoints()` | Iterate numeric Unicode scalar values |
| `s.sliceBytes(begin[, end])` | Half-open byte interval `[begin, end)`; character boundaries are not guaranteed |

A slice covering the whole string returns the original string.
`sliceBytes` positions must be non-negative integers: negative-from-end indexing and fractional rounding do not apply; invalid positions are runtime faults.
A reversed interval or one extending past the end produces an empty string.
Strings retain arbitrary bytes. For malformed UTF-8, `chars()` returns each invalid byte separately and `codepoints()` returns U+FFFD per invalid byte.
NUL is also traversed as an ordinary character or code point 0.
For sequential traversal, prefer `chars()` or `codepoints()` over repeated `at()` calls.

## Search, replacement, and splitting

| Call | Meaning |
| --- | --- |
| `s.find(needle[, from])` | First starting character ordinal, or `nil^` |
| `s.findall(needle)` | Iterate non-overlapping starting character ordinals |
| `s.replace(needle, replacement)` | Replace all matches |
| `s.split()` | Split on runs of whitespace, discarding empty pieces |
| `s.split(separator)` | Preserve empty pieces; an empty separator splits into characters |
| `s.toupper()`, `s.tolower()` | ASCII case conversion only |

Needles and separators are literal strings, not regular expressions.
Matching requires identical bytes and grapheme boundaries at both ends, without Unicode normalization.
For an empty needle, `find` returns the starting position, `findall` is empty, and `replace` leaves the input unchanged.
Explicit-separator splitting preserves `s.split(sep).join^(sep) = s`.
Use [std.regex](./stdlib/regex.md) for regular expressions.

## Example

<<< @/../sample/docs/reference/numbers-and-strings.lh{lhat}

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.obj.number], [syntax.obj.number-consts], [syntax.obj.tostring], [syntax.obj.tonumber], [syntax.obj.count],
[syntax.obj.substring], [syntax.obj.chars], [syntax.obj.find], [syntax.obj.eq], [syntax.obj.round], [syntax.obj.abs], [syntax.obj.bits]).
