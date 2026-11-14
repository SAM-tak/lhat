# Lexical structure and source

## Source format

- Source encoding is UTF-8. An initial BOM is skipped.
- `LF`, `CRLF`, and `CR` are accepted and normalized to `LF`, including inside strings.
- Unicode normalization is not performed. Names are distinguished by their code-point sequences.
- Whitespace generally separates tokens. Calls and hats have additional adjacency rules.

## Names and hats

`^` is called a hat. `if` is an ordinary name; `if^` is a syntax word.
Ordinary names are not reserved words. Whitespace cannot separate a word from its hat.
A bare `^` is not an operator; exponentiation uses `**`.

Identifiers follow Unicode `XID_Start` and `XID_Continue`, with `_` also allowed.
The current implementation lacks these Unicode tables and accepts non-ASCII characters more broadly than the specification.
Ordinary identifiers cannot contain `?` or `!`.

Backticks form name literals, allowing names such as `` `empty?` ``.
The delimiters are not part of the name: `` `name` `` and `name` are the same name.
Double a backtick to include one. Empty names and embedded newlines are not allowed.
`id^name` produces the spelling as a string without evaluating the name.

Hat names belong to language-defined positions. Users cannot declare arbitrary `name^` bindings or members.
Specified positions such as `self^`, `_^`, `tostring^`, and `iterate^` are exceptions.
Additional hats in `this^^` or `break^^` count outward through the corresponding context. This notation is supported by specific identifiers such as `this^` and `break^`.

## Numeric literals

| Form | Meaning |
| --- | --- |
| `42`, `1_000` | Decimal integers, digit separators |
| `0x2a`, `0b101010`, `0o52` | Hexadecimal, binary, octal integers |
| `0.5`, `1.0e-3` | Fractional and exponential notation |

`.5` and `1.` are invalid. `_` cannot occur at either end or next to a decimal point.
Separate a number from a following identifier, as in `1 to^ 3`.
Integers and floating-point numbers have distinct runtime representations, but share the language type `number^`.

## String literals

| Form | Rules |
| --- | --- |
| `"…"` | Escapes enabled; may span lines |
| `'…'` | No escapes; `''` represents one quote; may span lines |
| `"""` through end of line | No closing delimiter or escapes |
| `$"…{expression}…"` | String interpolation |

Double-quoted escapes are `\n`, `\r`, `\t`, `\0`, `\\`, `\"`, `\xHH`, `\u{…}`,
and a backslash followed by a newline for continuation. Unknown escapes are errors.
In interpolation, doubling either `{` or `}` represents a literal brace. `{expression:format}` supports numeric formats.
An outermost `:` starts the format, so parenthesize expressions containing one, such as `if^` expressions.
Character counts and byte counts differ; see [Tables and objects](./objects.md).

### Interpolation evaluation and formats

Holes are evaluated when the string is built, and their results are converted with `tostring^()`.
`{expression:format}` is equivalent to numeric `tostring(format)`. The format is literal text from `:` to the closing `}`, not an evaluated expression.
It cannot contain a closing brace. There is no `$'…'` interpolation syntax.

Formats use C `printf` notation and contain **exactly one numeric conversion**:

```text
%[flags][width][.precision]conversion
```

| Conversion | Meaning |
| --- | --- |
| `d`, `i` | Signed decimal integer |
| `u` | Unsigned decimal integer |
| `o` | Octal integer |
| `x`, `X` | Hexadecimal integer, lowercase or uppercase |
| `f`, `F` | Fixed-point notation |
| `e`, `E` | Exponential notation |
| `g`, `G` | Fixed-point or exponential notation according to significant digits |
| `a`, `A` | Hexadecimal floating-point notation |

| Option | Meaning |
| --- | --- |
| `-` | Left-align within the width |
| `+` | Include the sign of positive numbers |
| Space | Use a space for a positive sign; `+` takes precedence |
| `0` | Zero-pad to width; disabled by left alignment, and superseded by integer precision |
| `#` | Alternate form, such as octal/hexadecimal prefixes or retaining the decimal point |
| Width (decimal integer) | Minimum output width; longer values are not truncated |
| `.precision` | Minimum integer digits; fractional digits for `f`/`e`; significant digits for `g`; hexadecimal fractional digits for `a` |

Uppercase conversions also uppercase exponent markers or hexadecimal letters where applicable.
Default precision is 6 fractional digits for `f`/`e`, or 6 significant digits for `g`.
Precision 0 for `g` means 1; a bare `.` means precision 0.
Without `#`, `g` removes unnecessary trailing zeros and the decimal point.
Literal text can surround the conversion; `%%` outputs `%` without counting as a conversion.

Dynamic width/precision with `*`, length modifiers such as `l`/`ll`, positional arguments, and `s`, `c`, `p`, `n` are not allowed.
Zero conversions, multiple conversions, and embedded NUL also make a format invalid.
Invalid formats panic at runtime rather than returning an error value.
Integer formats accept floating-point values by truncating toward zero, within the signed 64-bit range.
Floating-point formats also accept integers.

Examples are `{n:%04d}`, `{n:%8.2f}`, and `{n:n=%d%%}`.
A format without `%`, such as `{n:2.4}`, is invalid.
See [Numbers and strings](./numbers-and-strings.md) for conversion, slicing, and the full member list.

## Comments

`#` starts a line comment. `#[` through `]#` forms a block comment, which can nest.
Quotes inside a block comment do not disable its delimiters.

### Disabled-code comments

`#[~` through `]#` is a block comment that the visual editor recognizes as disabled code.
The runtime skips it just like an ordinary block comment; its contents are neither executed nor type-checked.
Language-server and visual-editor tools can preserve its statements as a disabled region.
These comments can nest. `#~` is only an ordinary line comment.

Disable whole statements, including annotations attached to declarations.
Even a `]#` inside a quoted string in the comment closes it, so delimiters must remain balanced.

<<< @/../sample/docs/reference/lexical.lh{lhat}

### Documentation comments

There is no special documentation-comment syntax. Tools associate preceding comments and trailing same-line comments with declarations.
A disabled-code comment breaks a documentation-comment group and does not document the following declaration.

## Input forms and newlines

Ordinary source uses parentheses for calls. Command input supports juxtaposed calls on a per-line basis.
Ordinary `f(x)` and command-mode `f x` belong to different input forms.
The opening call parenthesis must be on the same line as its target; a parenthesis after a newline is not joined to the previous line as a call.

Command notation applies to lines readable as calls; statements beginning with syntax keywords follow ordinary rules.
Parenthesized calls bind more tightly than juxtaposition. A REPL waits for more input while an expression or statement is incomplete.
Ordinary source does not treat `f x` as a parenthesis-free call.

Integer-key dot access `a.0.0` means `a[0][0]`.
`1..2` is a number, concatenation, and another number, not a decimal point.
`++` and `--` are not increment/decrement operators. A BOM anywhere except the start of a file is an error.

Sources: [Lexical structure](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/01-lexical-structure.md)
([lex.source], [lex.hat], [lex.ident], [lex.number], [lex.string], [lex.comment], [lex.comment.disabled], [lex.ambig]);
[Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md) ([syntax.input], [syntax.juxta], [syntax.repl], [syntax.obj.tostring]).
