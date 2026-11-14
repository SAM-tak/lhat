# Expressions and operators

## Precedence

From weakest to strongest. Operators at the same level associate left unless noted otherwise.

| Level | Operators | Association |
| --- | --- | --- |
| 1 | `or^` | Left |
| 2 | `and^` | Left |
| 3 | `=`, `!=`, `is^`, `<`, `<=`, `>`, `>=`, `fits^` | Chained comparison |
| 4 | `<=>` | Left |
| 5 | `..`, `::` | Right |
| 6 | `+`, `-` | Left |
| 7 | `*`, `/`, `//`, `%`, `cross^`, `dot^`, `bitand^`, `bitor^`, `bitxor^`, `bitshift^` | Left |
| 8 | `catch^`, `??` | Left |
| 9 | `as^` | Left |
| 10 | Prefix `!`, `-`, `bitnot^`, `try^`, `await^` | Prefix |
| 11 | `**` | Right |
| 12 | Members, calls, indexing, nil-safe postfix operations, postfix `?` | Left |

`typeof^expression` is a prefix form including calls and member access.
Use parentheses for compound expressions, as in `typeof^(a + b)`.

## Arithmetic, logic, and concatenation

`**` is exponentiation, `//` is floor division, and `%` is remainder.
Numeric operations widen to floating point when an integer result does not fit; division by zero also widens to inf/nan.
Bitwise operations use word operators; `|` and `&` are reserved for type unions and intersections.

`!`, `and^`, and `or^` operate on booleans. `and^` and `or^` short-circuit.
Numbers, strings, and tables do not implicitly convert to booleans.
`..` and `::` are equivalent concatenation operators for strings, tables, and definition composition.

## Comparison

`=` tests equality, `is^` tests object identity, and `fits^` tests type conformance.
Do not use `is^` to compare numbers.
Inequality is `!=` (also `=/` or `≠`); `<=` and `>=` also accept `≤`, `≦`, `≥`, and `≧`.
Chained comparisons evaluate the middle expressions once.
`<=>` performs three-way comparison and enables ordering for user-defined types.

## nil and safe access

| Form | Meaning |
| --- | --- |
| `value?` | Boolean test for a value other than `nil^` |
| `value ?? fallback` | Evaluate the right side only if the left is `nil^` |
| `value?.member` | Return `nil^` if the receiver is `nil^` |
| `value?[index]` | Return `nil^` if the receiver is `nil^` |
| `value?(arguments)` | Return `nil^` if the call target is `nil^` |

Safe access handles absent receivers, rather than suppressing type mismatches or arbitrary runtime failures.
`catch^` handles errors; `??` handles `nil^`.

The first nil-safe operation short-circuits the rest of the same postfix chain.
If a is absent in `a?.b.c()`, subsequent operations and argument evaluation are skipped.
Parentheses break the chain: `(a?.b).c()` does not protect the outer `.c()`.
A trailing presence test `?` is false when the chain ends in absence.

Concatenating plain tables creates a new table with the array parts in sequence and the keyed parts combined.
Duplicate named keys are errors. Inputs remain unchanged and elements are shared shallowly.
See [Numbers and strings](./numbers-and-strings.md) for numeric and string built-ins.

## Defining operators

Inside `def^`, `op^operator = function` defines arithmetic, concatenation, `cross^`, `dot^`, `<=>`, or `=`.
Binary resolution first queries the left operand; if no alternative accepts the argument, it queries a right-hand implementation with trailing `self^`.
`op^- = f^self^ -> ResultType {…}` defines unary minus; an additional argument makes it binary minus.
Logical and bitwise operators cannot be replaced.
Extend ordering with `op^<=>` and equality with boolean-returning `op^=`.
`=` and `!=` prefer `op^=`, falling back to `<=>`. Individual ordering and inequality operators are not defined separately.
Operator implementations cannot be yieldable.

Sources: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.op.nil-safe-chain], [syntax.op.concat-table], [syntax.op], [syntax.op.table], [syntax.op.default], [syntax.op.nil-safe], [syntax.op.present], [syntax.op.right], [syntax.op.equal], [syntax.sub.op-not-yieldable]);
[Errors](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/04-errors.md) ([errors.runtime.div-zero]).
