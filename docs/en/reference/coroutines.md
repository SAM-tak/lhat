# Coroutines

## Creation and states

Calling a subroutine containing `yield^` returns an unstarted coroutine.
No body code runs at the call, including code before the first yield.
Definitions still use `f^` and `p^`; `c^` is type notation.

| Operation | Meaning |
| --- | --- |
| `c.start()` | Start an unstarted body |
| `c.resume(value)` | Resume, passing a value to the suspension point |
| `c.started()` | Test whether execution has started |
| `c.done()` | Test whether execution has finished |
| `c.dispose()` | Dispose and run pending cleanup |

States are unstarted, suspended, and finished. Resuming before starting or starting again is a runtime fault.
The coroutine type determines whether resume arguments are required.

## Yield and final results

`yield^expression` suspends execution and sends a value to the driver.
The value sent on resumption can be received as the result of `yield^`.
The type system distinguishes yielded values from the final result of `return^`.

`start()` and `resume()` return the union of yield and final-result types. When these coincide, inspect `done()` to distinguish completion.
Finishing without a value also returns `nil^` to the driver.
When yielding and returning tuples, the driving result uses their maximum width, padding missing positions with `nil^`.
This does not make ordinary tuple destructuring pad missing values.

`_yield^expression` supplies the same type information but emits no suspension or expression evaluation at runtime.
It can make a call produce a coroutine without actually suspending in the body.
Only `_^` can be written as its receive binding. There is no `_await^` or `yieldall^` syntax.

## Three type positions

```text
c^{p^R -> Y -> T}
```

`R` is the resume input, `Y` the yielded value, and `T` the final result.
An optional `f^` or `p^` specifies the body's kind; omission permits both.
An empty position means none; `*` constrains neither type nor arity.
`any^` is a single-value type, distinct from `*`.
`c^` erases these three positions. `.ResultType` extracts the final result type in type positions.

Only `->` separates positions. `c^{p^-> Y}` has no input, and `c^{p^-> -> T}` has neither input nor yields.
A final `-` means the coroutine cannot finish, adding no final-result alternative to `start()`/`resume()` results.
All three positions accept tuples. A two-position input is sent with `resume(a, b)` and received with `let^a, b = yield^value`.

`R` and `Y` must be uniform across suspension points; incompatible types are not implicitly merged into unions.
If no binding receives resume input, there is no input type.

## Delegation with await^

`await^coroutine` delegates execution and produces the inner coroutine's final result `T`.
Inner yields reach the outer driver, and resume values are forwarded inward.
An ordinary call does not perform delegation. `await^` rejects non-coroutine values.
Calls returning coroutines cannot simply be discarded as call statements or through `_^` bindings.
Explicitly retain and start the coroutine, or delegate with `await^`.

`await^` is language syntax; `std.task.await(task)` is a library API for collecting task results.
See [Concurrency](../guide/tasks.md) for worker-thread submission.

## Restrictions inside functions

A function can advance function coroutines created by its own body; advancing external coroutines cannot introduce effects.
Returning a coroutine while retaining its function-coroutine type is also restricted.
A function-body coroutine conforms to a compatible procedure-coroutine type.
Advancing it through that procedure type is a procedure operation.

## Saving continuations

`c.save()` returns a frame snapshot of type `costate^`; `c.load(snapshot)` restores it.
It includes locals, suspension positions, pending cleanup, and coroutines delegated to by `await^`.
It does not rewind heap objects or host resources, or run cleanup in frames discarded by restoration.
A snapshot can only be loaded into its original coroutine. Unstarted, suspended, and finished states can be saved;
running coroutines and bodyless iterators cannot.
Ordinary iteration and task execution use starting, resuming, and delegation.

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.sub.coroutine-members], [syntax.type.tuple], [syntax.type.coroutine], [syntax.sub.yieldable], [syntax.sub.yieldable-f], [syntax.sub.yield-both],
[syntax.sub.no-contagion], [syntax.sub.await], [syntax.sub.phantom-yield], [syntax.sub.await-done], [syntax.sub.save-load]).
