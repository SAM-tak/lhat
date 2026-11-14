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
In result position, write a tuple as a bare comma list, `-> A, B`. Because `,` binds more loosely than `|`, parentheses are needed only when a union covers the whole tuple: `-> (A, B)|SomeError`. An error type cannot occupy a tuple position.

Host nominal types may have invariant type arguments, as in `std.task.Task<number^>`.
These arguments are also retained in runtime descriptors.

`t^{number^[3]}` requires at least three positions of the same type; `t^{number^[]}` is an indefinite sequence.
Fixed position types, an indefinite tail, named members, and dictionary constraints can be combined.
Tuple positions are restricted to results and coroutine input/output; tuples are not ordinary arguments or table values.

## Conformance, narrowing, and casts

`value fits^ Type` returns a boolean and narrows the value's type in branches.
`value as^ Type` checks conformance at runtime, returning the same value on success or `localerror^.CastFailure` on failure.
A cast does not convert values; handle failure locally with `catch^` or an appropriate clause.
Casts between syntactically disjoint types are rejected by the checker.

`fits^` is allowed even when conformance or disjointness is already known, but a right-hand type that collapses to `any^` is rejected as meaningless.
Known integer ranges from comparisons also contribute to branch narrowing.

## Types as values

`typeof^(expression)` returns a type descriptor; `.signature` provides its textual representation.
Checked code uses the type determined by the checker; unchecked code reports what runtime tags can establish.
Type notation such as `number^` or `t^{…}` can also be passed to APIs accepting type values.

Type values themselves have type `type^`, or `type^<T>` when the represented type T is retained.
`=` on descriptors checks structural equality.
`nil^` still denotes the absence value; use `typeof^(nil^)` for its type descriptor.
The right-hand types of `fits^` and `as^` are resolved statically, not chosen dynamically through runtime descriptors.
`let^T = TypeNotation`, an existing type name, or a parenthesized type name creates a type alias; `public^let^` exports it.
Binding a descriptor obtained through `var^`, a call, a conditional, indexing, or `typeof^` does not introduce a type name.

### Result-type attributes

Append `.ReturnType` to a named subroutine with a result, or a signature alias, to refer to its call-result type.
It works in type positions and as a type value. For a yieldable subroutine, it is the coroutine type itself.
It is not available on result-less procedures, overload groups, or call results such as `f().ReturnType`.
Attributes such as `ARG0.ResultType`, `ARG0.ReturnType`, and `ARG0.T0` let registered signatures derive results from argument types.

### Checking strictness

Strict mode reports unresolved inferred types, unresolved errors, and non-exhaustive expressions.
The host selects strictness when checking a unit; source declarations do not switch it. CLI files default to strict and the REPL to relaxed, overridable with `--strict` or `--relaxed`.
Relaxed mode does not disable syntax/type contradictions or host-value storage restrictions.
`any^` explicitly accepts a broad value; it does not mean inference failed.

Sources: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.obj.structure-repeat], [syntax.type.as-expr], [syntax.type.return-type], [syntax.type.fits-range], [syntax.type], [syntax.type.tuple-parens], [syntax.obj.structure], [syntax.obj.typeof], [syntax.op.as], [syntax.op.as-error]);
[Modules](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/05-modules.md) ([modules.label]).

Source: [Compilation pipeline](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/03-compilation-pipeline.md)
([pipeline.strict], [pipeline.infer], [pipeline.bc.capture]).
