# Errors

## Failure as a value

Fallible operations return `SuccessType|ErrorType`. Error values do not propagate implicitly.
Receive them as ordinary results, explicitly inspecting, replacing, or propagating them.
Absence represented by `nil^` differs from an error.

## Declaration and construction

```text
errordef^ Name {
    Kind,
    DetailedKind { field:Type, defaulted = value },
}
new^Name.DetailedKind { field = value }
```

Error kinds are nominal types; ordinary tables with matching fields cannot replace them.
`Name.Kind` denotes a kind and `Name` their union, below `error^`.
Provide fields without defaults at construction; defaulted fields can be omitted.
Additional fields are permitted. Legacy `error^Kind{…}` remains a compatibility construction form.

| Common member | Type and default |
| --- | --- |
| `message` | `string^`, empty string |
| `cause` | `error^\|localerror^\|nil^`, `nil^` |

## Handle locally

`expression catch^ fallback` evaluates the right side only if the left returns an error.
`it^` is that error inside the right side. On success, the left value is the result.
Applying `catch^` to an infallible expression is a type error.
`expression catch^ nil^` replaces failure with absence.
`expression catch^ panic^it^` explicitly stops on failure.

A block clause such as `do^{… catch^ Type: …}` selects by type among errors
forwarded from the body by `try^` or similar control flow.
If a result is bound to a name, `fits^` or pattern matching can distinguish success and failure.

## Propagation

`try^expression` produces the success value, or returns an error from the current subroutine.
An applicable `catch^` clause receives the error instead.
The result type must permit propagated errors.
On an expression without errors, `try^` preserves its value and type.
Propagation still runs `finally^` and `with^` cleanup when exiting scopes.

## Multiple values and errors

A subroutine that returns either several values or an error writes its result type as `(Type1, Type2)|ErrorType`.

```lhat
errordef^MathError { DivisionByZero }

let^divmod = f^a:number^, b:number^ -> (number^, number^)|MathError {
    if^b = 0 {
        return^new^MathError.DivisionByZero{}
    }
    return^a // b, a % b
}

let^q, r = divmod(7, 0) catch^(0, 0)
let^show = p^a:number^, b:number^ -> nil^|MathError {
    let^x, y = try^divmod(a, b)
    print(x, y)
}
```

- `,` binds more loosely than `|`. Without parentheses, `-> number^, number^|MathError` is a two-position tuple
  of `number^` and `number^|MathError`. An error type cannot occupy a tuple position, so this is a type error.
  An error is returned instead of the whole tuple, not as one of its positions.
- Parentheses are needed only when a union covers the whole tuple. Write results without errors bare, as in `-> number^, number^`.
- Only error types and `nil^` can form a union with a whole tuple. A union such as `(number^, number^)|number^` cannot be written.
  For unions with `nil^`, see [Functions and procedures](./subroutines.md#parentheses-change-what-a-union-with-nil-means).
- `try^call()` passes the tuple to destructuring on success and propagates an error otherwise.
- A `catch^` fallback is a tuple of the same width (`catch^(0, 0)`). `catch^panic^it^` also works.
- Like a tuple, a union containing a tuple cannot be bound to a single name.
  You cannot bind the result and narrow it with `fits^`; receive it with `try^` or `catch^`.

## Local errors

`localerrordef^` declares kinds below `localerror^`, disjoint from `error^`.
They cannot occur in subroutine result or yield types, including nested unions and structures.
Errors such as `localerror^.CastFailure` from `as^` must be handled locally with `catch^` or a corresponding clause.

## Dropped errors and panic

Passing an error-containing value to a success-only type is a type error.
Calls used as statements also require explicit handling if their discarded results can contain errors.
Declared unions and narrowing provide exhaustiveness checks for error values.

A panic indicates a runtime contract violation, such as a promised member being absent.
It is distinct from an ordinary error value and cannot be caught with L^ `catch^`; it is reported to the host as an execution result.
Integer overflow and division by zero widen to floating point rather than producing error values.

Source: [Errors](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/04-errors.md)
([errors.kind], [errors.catch], [errors.try], [errors.exhaustive], [errors.dropped], [errors.runtime]);
[Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md) ([syntax.type.tuple], [syntax.type.tuple-parens]).
