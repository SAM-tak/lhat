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

## Multiple values and destructuring

`let^a, b = expression1, expression2` introduces multiple bindings.
With one tuple-returning expression, `let^a, b = call()` destructures its result.
The number of names must match the tuple's arity.
Tables are not implicitly destructured into names; access their elements by index.

`_^` discards a value without creating a readable name. Use `_^:Type` where an annotation is needed.

## Bindings introduced by syntax

Parameters can be reassigned inside their body. Names introduced by `with^` cannot.
The focus of a counted `for^i from^…` or names supplied by iteration also cannot be reassigned by the body.
A manually updated conditional-loop focus uses `for^var^i = …`.

Sources: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.stmt.define], [syntax.stmt.compound], [syntax.stmt.visible], [syntax.stmt.member], [syntax.stmt.let],
[syntax.type.destructure], [syntax.type.discard], [syntax.sub.kinds], [syntax.for.forms]);
[Lexical structure](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/01-lexical-structure.md) ([lex.hat.repeat], [lex.scope]).
