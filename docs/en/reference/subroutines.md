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
Explicit call-argument expansion uses a trailing `...` after the argument.
Return a value with `return^expression`, or multiple values with `return^expression1, expression2`.
Multiple results form a tuple and are destructured into a matching number of names.
`pack^call()` collects multiple results into an array table. Results are not implicitly expanded into another call.

If the entire function body is one expression, that expression is returned.
This does not extend to the last expression of a multi-statement body or to procedures.
Multi-statement functions use an explicit `return^`.
A procedure with no result has a signature such as `p^ArgumentType;`, without a result clause.

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
([syntax.type.signature], [syntax.type.variadic], [syntax.type.tuple], [syntax.sub.kinds], [syntax.sub.table-write],
[syntax.sub.fresh], [syntax.sub.mutable-self], [syntax.sub.expr-body], [syntax.sub.this], [syntax.obj.self]).
