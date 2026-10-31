# Language basics

This page walks through a small program that binds values, iterates over a table, and calculates results with a function.

## Keywords and comments

L^ syntax keywords end in `^`, as in `let^`, `var^`, and `for^`.
This distinguishes them from ordinary names, so you can use names such as `if` for your own variables.
A comment starts with `#` and continues to the end of the line.

## Variables, tables, and iteration

<<< @/../sample/readme/basics.lh{text}

Running this file prints `10 120`.
From the repository root, you can run it with:

```sh
lhat sample/readme/basics.lh
```

- `let^` creates a binding that cannot be reassigned.
- `var^` creates a variable that can be reassigned. `total += ...` adds to its current value.
- `{ 10, 20, 30 }` is a table, used here as an array of numbers.
- Array indices start at 0, so `values[0]` is `10`.
- `for^value in^values` iterates over the table's values.
- `f^x { x * 2 }` is a function that doubles its argument `x`. It returns the value of its final expression.

The values returned by `double` are `20`, `40`, and `60`, giving a total of `120`.
There are no type annotations here; the type checker infers types from how the values are used.

::: tip Bindings and table mutation are separate
`let^` prevents reassignment of a name. Whether a table's contents can be changed depends on its type and the context in which it is used.
:::

## Functions and procedures

<<< @/../sample/readme/functions.lh{text}

`discounted` is a function (`f^`) that calculates a discounted price.
`checkout` is a procedure (`p^`) that displays the result using `print`.
`checkout(100, 0.2)` prints `80.0`.

Functions may update local state, but cannot modify external state or call procedures.
Procedures can call both functions and procedures.
Separating calculations from I/O and external updates lets the type checker enforce the boundary of side effects.

## Handling errors

<<< @/../sample/readme/errors.lh{text}

`errordef^` defines error kinds.
This `divide` returns an error when the divisor is 0, and the division result otherwise.
`catch^0` substitutes `0` when an error is returned, so this program prints `0`.

## More examples

The [sample directory](https://github.com/SAM-tak/lhat/tree/main/sample) includes examples of
object composition, coroutines, concurrent tasks, and more.
For detailed rules, currently refer to the [design documents](https://github.com/SAM-tak/lhat/tree/main/DesignDocuments) (Japanese).
