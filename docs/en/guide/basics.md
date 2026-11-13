# Language basics

This page walks through a small program that binds values, iterates over a table, and calculates results with a function.

## Keywords and comments

L^ syntax keywords end in `^`, as in `let^`, `var^`, and `for^`.
This distinguishes them from ordinary names, so you can use names such as `if` for your own variables.
A comment starts with `#` and continues to the end of the line.

Use `..` or `::` to concatenate strings or tables.
`"a" :: "b"` and `"a" .. "b"` mean the same thing, with the same precedence and right associativity.
Both spellings also work for definition composition and `op^` operator declarations.

## Variables, tables, and iteration

<<< @/../sample/readme/basics.lh{lhat}

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

## Array ranges and reverse iteration

`range^(n, m)` walks array indices from lower bound n to upper bound m in ascending order,
`reverse_range^(n, m)` walks the same interval in reverse, and `reverse_iterate^()` walks the
whole array in descending order. Each returns an iterator (coroutine) yielding
the original index and value, without copying the array. Dictionary entries are excluded.

```lhat
let^ values = {10, 20, 30, 40}
for^ i, v in^ values.range^(1, 2) { print(i, v) }
# 1 20, then 2 30

for^ i, v in^ values.reverse_iterate^() {
    if^ v >= 30 { values.remove^(i) }
}
# values is now {10, 20}
```

Bounds are zero-based and inclusive. Negative indices count from the end, with
`-1` naming the last position. `range^` and `reverse_range^` take lower and upper
bounds; omitting the second argument sets the upper bound to the last index.
Alternatively, `range_downto^(start, finish)` takes the start and finish in traversal
order, with index 0 as the default finish. `reverse_range^(1, 3)` and
`range_downto^(3, 1)` both visit indices 3, 2, 1.
A lower bound greater than the upper bound, or out-of-bounds endpoints, produces an empty iterator.
Bounds are resolved at the call; values are read as iteration advances.
These iterators do not introduce `index^`.

Calling `remove^` during forward iteration can skip elements because later
elements shift left. To delete the current element, iterate in reverse as above.
Reverse iteration does not make deleting other positions, inserting, or sorting safe.
Deleting existing dictionary keys with `values[k] := nil^` during traversal
does not skip the remaining keys. Assigning `nil^` to an array position keeps
the position and does not reduce the array length.

`slice^` returns a shallow copy, while `reverse^()` reverses the original array in place.

## Constructing values with an explicit type

Use `new^Type{ field=value, ... }` to construct a value with an explicit type.

```lhat
let^ t = new^t^{ aaa:string^ } { aaa="aaa", bbb=42 }
let^ Point = def^{ self^{ x=0, y=0 } }
let^ p = new^Point { x=10 }

errordef^ InputError { Invalid { line:number^ } }
let^ e = new^InputError.Invalid { line=3, message="invalid input" }
```

A structural table type constrains the constructed value. Extra fields are
allowed and retained in the inferred type: `t` has both `aaa:string^` and
`bbb:number^`. Missing required fields and incompatible values such as `aaa=42`
are errors.

For a `def^` type, construction uses its defaults and initializes the specified
fields. Here, `p.y` is `0`. This also works when `overload^new` retains the
default `new()`. If `override^new` replaces the default, use `.new(...)` instead.

The legacy error construction syntax `error^InputError.Invalid{ ... }` remains supported.
It is retained for compatibility and is planned for removal in the future; no
removal date has been set. Use `new^InputError.Invalid{ ... }` in new code.
The error supertype `error^` will remain available.

## Functions and procedures

<<< @/../sample/readme/functions.lh{lhat}

`discounted` is a function (`f^`) that calculates a discounted price.
`checkout` is a procedure (`p^`) that displays the result using `print`.
`checkout(100, 0.2)` prints `80.0`.

Functions may update local state, but cannot modify external state or call procedures.
Procedures can call both functions and procedures.
Separating calculations from I/O and external updates lets the type checker enforce the boundary of side effects.

## Handling errors

<<< @/../sample/readme/errors.lh{lhat}

`errordef^` defines error kinds.
This `divide` returns an error when the divisor is 0, and the division result otherwise.
`catch^0` substitutes `0` when an error is returned, so this program prints `0`.

## More examples

The [sample directory](https://github.com/SAM-tak/lhat/tree/main/sample) includes examples of
object composition, coroutines, concurrent tasks, and more.
For detailed rules, currently refer to the [design documents](https://github.com/SAM-tak/lhat/tree/main/DesignDocuments) (Japanese).
