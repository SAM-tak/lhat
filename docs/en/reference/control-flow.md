# Control flow

## Statements and expressions

Statements execute code; expressions produce values. A bare calculation cannot stand as a statement in an ordinary block.
Use calls, declarations, assignments, and control statements.
A function whose entire body is one expression returns that expression's value.
`do^{…}` is a statement block.

## if^ statements and expressions

```text
if^ condition {
    statements
elif^ condition:
    statements
else^:
    statements
}

if^ condition: value el^ condition: value el^: value;
```

- Conditions require `bool^`, without implicit conversion from numbers or `nil^`.
- A statement encloses all branches in one pair of braces and returns no value. Its unconditional final branch is optional.
- An expression starts with `if^ condition:` and closes with `;`, returning the selected value.
- An expression requires a final unconditional branch. A sequence of conditions does not prove exhaustiveness.
- Conditional branch spellings are `elseif^`, `else^if^`, `elsif^`, `elif^`, `ei^`, and conditional `el^`.
  Unconditional branches use `else^` or `el^`.

## Pattern matching

```text
for^ subject {
when^ pattern:
    statements
other^:
    statements
}

for^ subject: when^ pattern: value other^: value;
```

The subject is evaluated once and the first matching branch wins.
Refer to the focus with `it^`, or name it with `for^let^name = subject`.
Statements close with one pair of braces; expressions close with a final `;`.

| Pattern | Test |
| --- | --- |
| `when^ value:` | Equality |
| `when^ lower to^ upper:` | Inclusive range |
| `when^ fits^ Type:` | Type conformance and narrowing |
| `when^ value1, value2:` | Either matches |
| `other^:` | Everything remaining; also `el^:` or `else^:` |

A statement can omit `other^` and does nothing if no branch matches.
Expressions must produce a value and are checked for exhaustiveness. `other^` can be omitted when
type patterns, both boolean values, or all enum members prove complete coverage.
Numeric and range patterns alone do not prove coverage.
Non-exhaustive expressions are strict type errors; if executed without checking and no branch matches, they panic.
Patterns do not support destructuring or additional guard clauses.

## Iteration and focus

| Form | Meaning |
| --- | --- |
| `for^i from^ a to^ b {…}` | Inclusive ascending numeric iteration |
| `for^i from^ b downto^ a {…}` | Inclusive descending numeric iteration |
| Either form with `step^ amount` | Specify the step size |
| `for^value in^ subject {…}` | Iterator traversal |
| `for^key, value in^ table {…}` | Traverse keys and values |
| `for^var^i = initial while^ condition next^ update {…}` | Manually advance a conditional-loop focus |
| `repeat^ n {…}` | Repeat n times |
| `repeat^{…}` | Repeat until an explicit exit |
| `repeat^while^ condition {…}` | Repeat while true |
| `repeat^until^ condition {…}` | Repeat while false |

`for^` introduces a focus; not every form iterates. `repeat^` introduces no focus.
Both `while^` and `until^` test before the body. `to^` and `downto^` explicitly specify direction.

## Clauses and exits

Loop clauses have the order `prolog^` → `pre^` → `first^` → `main^` → `catch^` → `last^` → `epilog^` → `finally^`.
A body with no headings is an implicit `main^`.

| Clause | Execution |
| --- | --- |
| `prolog^:` | Once before iteration |
| `pre^:` | Before each condition test |
| `first^:` | At the start of the first successful iteration |
| `main^:` | Each successful iteration |
| `last^:` | On normal completion, using the last successful state |
| `epilog^:` | On normal completion, regardless of iteration count |
| `finally^:` | Cleanup, including early exits |

`next^` continues with the next iteration; `break^` exits the loop. `break^^` exits two loop levels.
`next^^` can also target an outer loop; numeric forms `break^[2]` and `next^[2]` are equivalent.
The loop targeted by a break also runs `last^` and `epilog^`.
Inner loops crossed by a multi-level exit, and loops exited by `return^` or error propagation, run only `finally^` cleanup.
`return^` exits the current subroutine.

## Cleanup

`finally^:` is a block's final clause, executed on normal completion, `break^`, `return^`, error propagation, and coroutine disposal.
It cannot change the return value or appear inside an `f^` body.

`with^name = expression {…}` binds a resource with `dispose()` and calls `dispose()` when the block ends.
Consecutive `with^name = expression` lines share the following block and are disposed in reverse declaration order.

```lhat
with^r = open("data")
with^w = create("out") {
    copy(r, w)
}   # w.dispose(), then r.dispose()
```

- Like `finally^`, disposal runs on normal completion, `break^`, `return^`, error propagation, and coroutine disposal.
- In the same block, `finally^` runs first and `dispose()` after it.
- The value must have a `dispose()` that takes no arguments and returns nothing. The check is structural; a value that fails it is a type error in strict mode.
- As with `let^`, the bound name cannot be reassigned, and `with^name := expression` is not allowed.
- `with^` cannot appear inside an `f^` body.
- `dispose()` does not return errors. Call fallible cleanup, such as `flush()`, as a separate operation beforehand.
- Coroutines have `dispose()`, so passing one to `with^` disposes it when the block ends.

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.stmt], [syntax.if], [syntax.loop], [syntax.finally], [syntax.with], [syntax.for], [syntax.match]).
