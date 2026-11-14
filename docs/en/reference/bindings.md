# Bindings and scope

## Declarations and reassignment

| Form | Meaning |
| --- | --- |
| `let^name = expression` | Introduce a binding that cannot be reassigned |
| `var^name = expression` | Introduce a binding that can be reassigned |
| `let^name:Type = expression`, `var^name:Type = expression` | Introduce an annotated binding |
| `name := expression` | Reassign an existing binding |
| `target += expression`, etc. | Read, operate, and write back |

`=` introduces an initializer or table element; in expressions it tests equality.
Reassignment uses `:=` and does not create undeclared names.
Compound assignments are `+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `**=`, and `..=`.
For a member or indexed target, the receiver and index are evaluated once.
Nil-checked assignments such as `?:=` and `?+=` evaluate the right side and write only when the target is not `nil^`.
If absent, the right side is not evaluated. Multiple-target assignments evaluate right sides using the values before writing.

`let^` restricts reassignment of the binding. Mutation of the referenced table is governed separately
by its type, sealing, and function effect rules.

## Visibility and name resolution

Names have lexical scope. A declaration's initializer does not see the new binding it creates.
Use `this^` for self-recursion. Other declarations are visible throughout their scope,
allowing mutually recursive deferred function bodies. Direct reads before initialization are invalid.
The same frame cannot declare a name twice.
Access to an outer name does not imply permission to change it; in particular, `f^` cannot assign to outer names.

`it^` is the focus introduced by `for^` or `catch^`; `self^` is a method receiver.
`it^^`, `self^^`, and `this^^` refer to the corresponding outer contexts.
`this^^` follows lexical nesting, not the caller's stack.

| Scope prefix | Starting point for lookup |
| --- | --- |
| `$name` | Translation-unit root |
| `$$name` | One scope inward from the root |
| `$^name` | One scope outward from the current scope |
| `$^^name` | Two scopes outward |

Do not separate the prefix from its name. If absent there, lookup continues outward.
Depth counts blocks introducing name scopes, not table literals or interpolation holes.
Referring beyond existing scopes is an error.

## Introducing and updating members

`var^ t.member = value` introduces a member; `t.member := value` updates an existing one.
Table literals use `member = value` and `[keyExpression] = value`.
Mutation is visible through other references to the same table.
`let^t.member = value` is invalid. See [Adding and updating members after construction](./objects.md#adding-and-updating-members-after-construction) for examples, including tables bound with `let^`.

## Introducing or updating a member

`var^t.member := value` introduces an absent member or updates an existing one.
`var^t.member = value` only introduces it; bare `t.member := value` does not declare a new member.
This extends ordinary tables, not the structure of definitions or instances.
Through an annotated structural type, validity follows that annotation.

## Multiple-assignment evaluation

All receivers, indices, and right-hand values are read before writes occur. `a, b := b, a` swaps values.
`a, b += x, y` applies the operation at each position. `?:=` and `?+=` check absence separately at each position, allowing other positions to update.

## Multiple values and destructuring

`let^a, b = expression1, expression2` introduces multiple bindings.
With one tuple-returning expression, `let^a, b = call()` destructures its result.
The number of names must match the tuple's arity.
Tables are not implicitly destructured into names; access their elements by index.
Nested destructuring, implicit truncation of extra values, and automatic padding of missing values are not supported.

`_^` discards a value without creating a readable name. Use `_^:Type` where an annotation is needed.

## Bindings introduced by syntax

Parameters can be reassigned inside their body. Names introduced by `with^` cannot.
The focus of a counted `for^i from^…` or names supplied by iteration also cannot be reassigned by the body.
A manually updated conditional-loop focus uses `for^var^i = …`.

The language supplies no implicit global variables. Use unit-level names, imported names, and host initial bindings.

Sources: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.stmt.member-upsert], [syntax.stmt.define], [syntax.stmt.compound], [syntax.stmt.visible], [syntax.stmt.member], [syntax.stmt.let],
[syntax.type.destructure], [syntax.type.discard], [syntax.sub.kinds], [syntax.for.forms]);
[Lexical structure](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/01-lexical-structure.md) ([lex.hat.repeat], [lex.scope]).
