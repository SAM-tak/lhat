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
let^values = {10, 20, 30, 40}
for^i, v in^values.range^(1, 2) { print(i, v) }
# 1 20, then 2 30
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

### Removing matching elements during iteration

To remove matching array elements, the idiom is to iterate in reverse and call
`remove^` with the current index.

```lhat
let^values = {10, 20, 30, 40}
for^i, v in^values.reverse_iterate^() {
    if^v >= 30 { values.remove^(i) }
}
# values is now {10, 20}
```

Calling `remove^` during forward iteration can skip the next element because later
elements shift left. In reverse, removing the current element does not change the
indices of elements still to be visited.
Reverse iteration does not make deleting other positions, inserting, or sorting safe.
Deleting existing dictionary keys with `values[k] := nil^` during traversal
does not skip the remaining keys. Assigning `nil^` to an array position keeps
the position and does not reduce the array length.

`slice^` returns a shallow copy, while `reverse^()` reverses the original array in place.

## if^ statements and expressions

An `if^` statement executes code according to a condition. The statement itself does not return a value.

```lhat
let^n = 2
if^n < 0 {
    print("negative")
elif^n = 0:
    print("zero")
else^:
    print("positive")
}
```

It starts with `if^ condition {` and encloses all branches in **one pair of `{` and `}`**.
Unlike C, which encloses each branch in its own `{ ... }`, L^ places `elif^ condition:`
and `else^:` inside the same braces. `elif^` adds a condition; `else^` handles the case
where none of the conditions holds. A statement can omit `else^`, in which case it
does nothing if no condition holds.

Use an `if^` expression to select a value for a binding or calculation.

```lhat
let^n = 2
let^sign = if^n < 0: "negative" el^n = 0: "zero" el^: "positive";
print(sign)
```

An expression starts with `if^ condition:` and **ends with `;`, which closes the entire expression**.
`el^ condition:` adds a conditional branch, while `el^:` introduces the final, unconditional branch.
The selected branch supplies the expression's value; here, `sign` is `"positive"`.
An `if^` expression must produce a value in every case, so its final `el^:` is required.
A list of conditions is never treated as proof of exhaustiveness; always include an unconditional branch.

## Pattern matching statements and expressions

Pattern matching uses `for^` and `when^`. It evaluates the subject once and selects
the first matching branch from top to bottom. `other^:` handles the case where no pattern matches.

```lhat
let^n = 2
for^n {
when^0:
    print("zero")
when^1 to^3:
    print("few")
other^:
    print("many")
}
```

The statement form starts with `for^ subject {` and, like an `if^` statement,
encloses all branches in **one pair of `{` and `}`**. The statement itself does not return a value.
It can omit `other^:` and does nothing if no branch matches.
`when^ value:` tests equality, `when^ lower to^ upper:` tests an inclusive range,
and `when^ fits^ Type:` tests whether the subject fits a type.

Use the expression form to select a value:

```lhat
let^n = 2
let^label = for^n:
    when^0: "zero"
    when^1 to^3: "few"
    other^: "many";
print(label)
```

The expression form starts with `for^ subject:` and **ends with `;`, which closes the entire expression**.
The selected branch supplies its value; here, `label` is `"few"`.

Expression forms have an **exhaustiveness check** to ensure that every case produces a value.
Normally, include `other^:`. It can be omitted when the type checker can prove coverage,
such as when type patterns cover all possible types, both `true^` and `false^` cover a `bool^`,
or all members of an enum are listed. Numeric value and range patterns alone do not establish exhaustiveness.
A non-exhaustive expression is a type error in strict mode.

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

### Returning multiple values

Functions and procedures can return several values with `return^expr1, expr2`.

<<< @/../sample/docs/multiple-results.lh{lhat}

Write the result type as a comma-separated list, as in `-> number^, number^`.
Receive the values with the same number of names, as in `let^q, r = divmod(7, 2)`.
You cannot bind them to a single name or pass them directly as an argument.

To pass them as arguments to another call, write `...` after the call to expand them.
`add(divmod(7, 2)...)` is the same as `add(3, 1)`.
To collect the values in a table, use `pack^`. `pack^divmod(7, 2)` is an array table equal to `{ 3, 1 }`.
For what can be expanded and where, see [Multiple results](../reference/subroutines.md#multiple-results) and [Expanding into arguments](../reference/subroutines.md#expanding-into-arguments).

## Handling errors

<<< @/../sample/readme/errors.lh{lhat}

`errordef^` defines error kinds.
This `divide` returns an error when the divisor is 0, and the division result otherwise.
`catch^0` substitutes `0` when an error is returned, so this program prints `0`.

### Writing result types with multiple values and errors

When a subroutine returns either several values or an error, parenthesize the values before adding the error type to the union.

<<< @/../sample/docs/multiple-results-errors.lh{lhat}

`,` binds more loosely than `|`. Without parentheses, `-> number^, number^|MathError` means
"two values: `number^` and `number^|MathError`", which is a type error.
An error is returned instead of the whole list of values, not as one of them.
`-> (number^, number^)|MathError` means "two numbers, or an error".
With `nil^`, both forms are valid, but `-> A, B|nil^` and `-> (A, B)|nil^` mean different things; see the [reference](../reference/subroutines.md#parentheses-change-what-a-union-with-nil-means).

The caller uses `try^` and `catch^` as with a single value.
A `catch^` fallback also supplies the same number of values, as in `catch^(0, 0)`.
For details, see [Multiple values and errors](../reference/errors.md#multiple-values-and-errors) in the reference.

## Cleaning up resources

Bind values that must be closed after use, such as files and sockets, with `with^`.
When the block ends, the bound value's `dispose()` is called automatically.

<<< @/../sample/docs/with.lh{lhat}

A `def^` type can provide `dispose`.
Consecutive `with^` lines share the following block, and values are disposed in reverse order.
A `finally^:` clause in the same block runs first.
`dispose()` is also called when `return^` or error propagation leaves the block.

Passing a value without `dispose()` is a type error.
Names bound by `with^` cannot be reassigned, and `with^` cannot appear inside a function (`f^`) body.
Standard library files, regular expressions, threads, and more have `dispose()`, so they work with `with^`.
For the detailed rules, see [Cleanup in control flow](../reference/control-flow.md#cleanup).

## More examples

The [sample directory](https://github.com/SAM-tak/lhat/tree/main/sample) includes examples of
object composition, coroutines, concurrent tasks, and more.
For detailed rules, see the [L^ reference](../reference/index.md).
