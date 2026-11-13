# Types

## Basic notation

| Type | Values |
| --- | --- |
| `number^` | Integers and floating-point numbers |
| `string^` | Strings |
| `bool^` | `true^` and `false^` |
| `nil^` | The absence value `nil^` |
| `any^` | Explicitly accepts any value |
| `t^{…}` | Structural table type |
| `f^…;`, `p^…;` | Function and procedure signatures |
| `c^{…}`, `c^` | Specific and erased coroutine types |
| `error^`, `localerror^` | Supertypes of propagatable and locally handled errors |

`any^` is an explicitly chosen type, distinct from an unresolved inference result.
Use a union such as `number^|nil^` to include absence.

## Structural types and unions

`t^{x:number^, label:string^}` requires at least those members with those types; extra members are allowed.
`t^{}` accepts every table and no non-table values.
Use `t^{number^[]}` for an array part and `t^{[string^]:number^}` for a dictionary part.

`A|B` accepts values conforming to either type. A member accessible on a union must be available on every alternative.
`A&B` denotes intersection, also used in the displayed type of composed definitions.
Ordinary structurally matching tables do not require matching declaration names.
Error kinds, enums, and host nominal types also distinguish declaration or registration identity.

## Subroutine and coroutine types

```text
f^ArgumentType, ArgumentType -> ResultType;
p^ArgumentType;
f^ArgumentType -> ResultType1, ResultType2;
c^{p^ResumeType -> YieldType -> FinalResultType}
```

Signature semicolons are required. A procedure with no result omits `->`.
Parameter names and defaults are not part of a type. `self^` marks a receiver rather than naming a type.
`Self^` inside a type literal refers to its own type.
Multiple return values form a tuple, distinct from a single ordinary variable or table.

Host nominal types may have invariant type arguments, as in `std.task.Task<number^>`.
These arguments are also retained in runtime descriptors.

## Conformance, narrowing, and casts

`value fits^ Type` returns a boolean and narrows the value's type in branches.
`value as^ Type` checks conformance at runtime, returning the same value on success or `localerror^.CastFailure` on failure.
A cast does not convert values; handle failure locally with `catch^` or an appropriate clause.
Casts between syntactically disjoint types are rejected by the checker.

## Types as values

`typeof^(expression)` returns a type descriptor; `.signature` provides its textual representation.
Checked code uses the type determined by the checker; unchecked code reports what runtime tags can establish.
Type notation such as `number^` or `t^{…}` can also be passed to APIs accepting type values.

Sources: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.type], [syntax.obj.structure], [syntax.obj.typeof], [syntax.op.as], [syntax.op.as-error]);
[Modules](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/05-modules.md) ([modules.label]).
