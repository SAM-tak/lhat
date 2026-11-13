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
Additional hats in `this^^` or `break^^` count outward through the corresponding context; arbitrary words cannot take them.

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

## Comments

`#` starts a line comment. `#[` through `]#` forms a block comment, which can nest.
Quotes inside a block comment do not disable its delimiters.

## Input forms and newlines

Ordinary source uses parentheses for calls. Command input supports juxtaposed calls on a per-line basis.
Ordinary `f(x)` and command-mode `f x` belong to different input forms.
The opening call parenthesis must be on the same line as its target; a parenthesis after a newline is not joined to the previous line as a call.

Sources: [Lexical structure](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/01-lexical-structure.md)
([lex.source], [lex.hat], [lex.ident], [lex.number], [lex.string], [lex.comment], [lex.ambig.call-line]);
[Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md) ([syntax.input], [syntax.juxta]).
