# Tables and objects

## Table literals and elements

Tables have an array part and a keyed part.

| Element form | Meaning |
| --- | --- |
| `{value1, value2}` | Array positions starting at 0 |
| `{name = value}` | Named key |
| `{[expression] = value}` | Computed key |
| `{name:Type = value}` | Annotated element value |

Non-negative integer indices address array positions; other indices are keys.
`t[-1]` refers to the key `-1`, not the last array element.
Keys cannot be `nil^` or NaN. Storing a host-value key requires a sealed box.
Assigning `nil^` to an existing dictionary key removes it. An array `nil^` keeps its position and does not shorten the array.
Access without guaranteed presence depends on the type; it does not always silently return `nil^`.

## Counts and strings

| Member | Meaning |
| --- | --- |
| `t.len^`, `t.length^` | Array length, including `nil^` positions |
| `t.count^` | Total array and keyed elements |
| `s.len`, `s.length` | Extended grapheme-cluster count |
| `s.size` | Byte count |

These are properties, without call parentheses.
Table built-ins require hats to distinguish them from user keys; string built-ins use bare names.
`s.substring(...)`, `s.substr(...)`, `s.sub(...)`, and `s.at(...)` operate on characters.
`s.chars()` iterates characters; `s.codepoints()` iterates Unicode scalar values.
`s.sliceBytes(begin, end)` uses the half-open byte interval `[begin, end)`.

## Built-in table operations

| Operation | Purpose |
| --- | --- |
| `clone^()` | Shallow copy |
| `slice^(start, end)` | Inclusive shallow range copy; negative positions count from the end |
| `join^(separator)` | Join string and numeric elements |
| `indexof^(value)`, `contains^(value)` | Search or test membership |
| `push^(value)`, `pop^()` | Append or remove the final element |
| `insert^(position, value)`, `remove^(position)` | Insert or remove, shifting later elements |
| `extend^(array)` | Append the other array part |
| `sort^()`, `stablesort^()` | Sort |
| `reverse^()`, `move^(…)` | Reverse or move elements |
| `resize^(length, value)` | Resize the array |
| `clear^()`, `assign^(other)` | Clear all content or replace it |
| `range^(…)`, `reverse_range^(…)`, `range_downto^(…)`, `reverse_iterate^()` | Iterate original array indices and values |

Mutating operations change their receiver. `pop^` and `remove^` return the extracted value or `nil^`;
the other mutating operations listed above return the receiver.
Mutation inside functions follows the [effect rules](./subroutines.md).

## Definitions and construction

`def^{…}` creates a definition with an instance template and shared members.
`self^{field = initializer}` declares per-instance fields.
Shared methods belong on the definition and have `self^` as their first parameter.
The `self^{…}` section can also contain callback fields or instance-specific methods.
Fields and shared methods cannot have the same name.

`new^Type{field = value}` constructs a value with an explicit type.
Structural construction retains extra fields; definition defaults are evaluated for each construction.
Default construction also supports `Definition.new(...)`.
If `override^new` replaces construction, use `Definition.new(...)`.
Instances contain fields and receiver methods; static members belong to the definition.

## Composition and member conflicts

`A .. B` (also `A :: B`) composes definitions. Delegation targets and order are fixed at definition time.
Duplicate members conflict by default. Use `override^` for replacement and `overload^` for type-based overloads.
Replacement must preserve the original contract: it cannot narrow inputs or widen outputs.
`override^new`, which replaces definition construction, is exempt from that substitutability check.
An overload's type is an intersection; alternatives must be distinguishable by argument types or arity.
`abstract^name:Type` requires a member from a composed implementation; unresolved abstract members prevent construction.
`super^` refers to the corresponding implementation before overriding. There is no multi-level `super^^`.

## Enums

`enum^Name { A, B, C = 10 }` declares a nominal enum.
Members are singleton values; `=` and `is^` test the same identity.
`.value` holds the explicit value, or an ordinal starting at 0; explicit integer values reset the next ordinal.
`.enum` identifies the owning enum; `.tostring()` gives the member name.
In type positions, the enum name denotes any member and a member name denotes that singleton.
Listing all members establishes exhaustive pattern matching.
Members cannot be added or replaced after declaration.

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.obj], [syntax.obj.keys], [syntax.obj.count], [syntax.obj.chars], [syntax.obj.table-ops], [syntax.enum]).
