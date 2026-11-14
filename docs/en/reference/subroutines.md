# Functions and procedures

## Definitions and signatures

```text
f^argument:Type, argument:Type = default -> ResultType { body }
p^argument:Type -> ResultType { body }
```

`f^` defines a function and `p^` a procedure. Both are values that can be bound, passed, or returned.
Annotations can be omitted where inference succeeds. Function and procedure types require a final `;`;
definition bodies close with braces.

## Calls and effects

| Rule | Function `f^` | Procedure `p^` |
| --- | --- | --- |
| Callable subroutines | Functions | Functions and procedures |
| Assignment to names | Names and parameters created by its own body | Also outer mutable names |
| Table mutation | Within the portion created by its own body | Also external tables |

Putting an external table into a local table does not grant permission to mutate the external one.
The checker also tracks the origin of nested values.
`fresh^` promises a fresh result; `mutable^self^` marks a function method that mutates its receiver.
Whether such methods can be called inside a function also depends on the value's origin.

An `f^` value conforms to a compatible `p^` type because that contract permits effects.
The reverse is invalid. Calling a `p^`-typed value inside a function requires narrowing it to a function type.

## Arguments and results

Parameters are body-local names and can be reassigned. Parameter names and defaults are not part of signature types.
Variadic parameters use `...:Type`; omitting the type gives `any^`.
Variadic parameters must come last; inside the body, `...` is one table collecting the received values.
Return a value with `return^expression`.

If the entire function body is one expression, that expression is returned.
This does not extend to the last expression of a multi-statement body or to procedures.
Multi-statement functions use an explicit `return^`.
A procedure with no result has a signature such as `p^ArgumentType;`, without a result clause.

## Multiple results

```lhat
let^divmod = f^a:number^, b:number^ -> number^, number^ {
    return^a // b, a % b
}
let^q, r = divmod(7, 2)   # 3, 1
let^_^, m = divmod(9, 4)  # use only the 1
```

`return^expression1, expression2` returns several values. Together they form a single type, a tuple.

- Write the result type as a comma list, `-> Type1, Type2`. `-> (Type1, Type2)` is the same type.
  A union with an error needs parentheses; see [Multiple values and errors](./errors.md#multiple-values-and-errors).
- Receive the values by destructuring, `let^q, r = call()`. A mismatch between the number of names and positions is a type error.
  Discard unused positions with `_^`. A call statement may also discard all results.
- A tuple can appear only in result position. It cannot be bound to a single name, passed as an argument,
  stored as a table element, nested in another tuple, or placed in a string interpolation hole. There is no one-element tuple; a bare single type is just that type.
- When the types match, `return^call()` returns another subroutine's tuple as is.
- `pack^call()` collects the results into an array table of type `t^{ Type1, Type2 }`, which differs from the tuple type.
  Conversely, a table is never implicitly destructured into a list of names.
- Unlike Lua, calls in the middle of a list are not truncated to one value, and a trailing call is not implicitly expanded.

### Parentheses change what a union with `nil^` means

Because `,` binds more loosely than `|`, parentheses decide how much `nil^` covers.

| Result type | Meaning | How to receive it |
| --- | --- | --- |
| `-> string^, number^\|nil^` | Always two values; only the second position may be `nil^` | Destructure directly with `let^s, n = call()` |
| `-> (string^, number^)\|nil^` | Two values, or a single `nil^` | Supply a fallback tuple with `??`, then destructure |

```lhat
let^find = f^key:string^ -> (string^, number^)|nil^ {
    if^key = "a" {
        return^"a", 1
    }
    return^nil^
}
let^name, value = find("b") ?? ("none", 0)
```

The second form cannot be bound to a single name or destructured as is, because the number of values varies from call to call.
Receive it with a construct that tells `nil^` apart, such as `??` or a `for^` walk.
A table walk's `start()` and `resume()` return this kind of type.
Only `nil^` and error types can form a union with a whole tuple. For error types, see [Multiple values and errors](./errors.md#multiple-values-and-errors).

## Expanding into arguments

Writing `...` after a call argument expands the value into several arguments.
Nothing is expanded without `...`, even when the callee is variadic.

```lhat
let^three = f^a:number^, b:number^, c:number^ { a + b + c }
three(divmod(7, 2)..., 10)   # three(3, 1, 10)
three(1, divmod(7, 2)...)    # three(1, 3, 1)
```

| Expanded value | Where it may appear | Parameters it fills |
| --- | --- | --- |
| Tuple | Anywhere, any number of times in one call | Fixed and variadic |
| Table whose sequence element type is known (`t^{ T[] }`) | Last argument only | Variadic only |
| The body's `...` (forwarding received variadic arguments) | Last argument only | Variadic only |

A tuple's width is fixed by its type, so expanding it is the same as writing its positions in place.
Each position is checked against the parameter it fills. A mismatched count is an argument-count error.
A table or forwarded `...` has a length unknown until run time, so it can flow only into the variadic part and no argument can follow it.
On the receiving side, `...` is always one table, however the values were passed.

## Recursion and receivers

`this^` is the currently executing subroutine, enabling self-recursion without a binding name.
`this^^` refers to the lexically enclosing subroutine.
`self^` as the first parameter marks a method receiver, supplied automatically by `value.method()`.
The type reference `Self^` differs from the receiver value `self^`.

## Yieldable subroutines

Yieldability is inferred from `yield^`, `_yield^`, and `await^`.
A yieldable call returns a coroutine without running its body.
Functions can also be yieldable, subject to restrictions including the origin of coroutines advanced inside a function.
See [Coroutines](./coroutines.md).

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.type.signature], [syntax.type.variadic], [syntax.type.tuple], [syntax.type.tuple-parens], [syntax.op.default], [syntax.sub.kinds], [syntax.sub.table-write],
[syntax.sub.fresh], [syntax.sub.mutable-self], [syntax.sub.expr-body], [syntax.sub.this], [syntax.obj.self]).
