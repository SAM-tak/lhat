# Tables and objects

## What is a table?

A table is a **data structure combining an array and a dictionary**.
The same table can hold an ordered sequence accessed by index, or associate keys such as names with values.
There is no need to choose separate array and dictionary kinds; one table can also mix both kinds of elements.

```lhat
let^array = {10, 20, 30}
let^dictionary = {name = "L^", version = 1}
let^mixed = {10, 20, name = "scores"}

print(array[0])           # 10
print(dictionary.name)    # L^
print(dictionary["name"]) # L^: same key as .name
print(mixed[1], mixed.name) # 20 scores
```

Array indices start at 0. Access dictionary entries with `t.name` or `t[key]`.
`t.name` and `t["name"]` refer to the same entry.

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
Real indices inside the existing array part round with `floor(x + 0.5)`.
Integral `2.0` and `2` are the same key; non-integral keys outside the array are not always array positions.

## Adding and updating members after construction

To add a named member to an existing table, write `var^t.bbb = "bbb"`.
`t.bbb := "bbb"` updates an existing member, so the checker rejects it if `bbb` does not yet exist.

| Form | Member absent | Member present |
| --- | --- | --- |
| `var^t.bbb = value` | Add | Redefinition error |
| `t.bbb := value` | Missing-member error | Update |
| `var^t.bbb := value` | Add | Update |
| `let^t.bbb = value` | Not allowed | Not allowed |

`let^` binds names and cannot introduce members.
However, a table bound with `let^t = {aaa = 1}` can still receive a member through `var^t.bbb = …`.
Here `var^` introduces `bbb`; it does not reassign the name `t`.
Updates must still conform to the existing member's type.

<<< @/../sample/docs/reference/table-members.lh{lhat}

For a path such as `var^t.config.window.width = 800`, missing intermediate tables are created and existing ones are traversed.
Adding `height` afterward preserves `width`. With `=`, the final member must be new.

These operations apply to ordinary tables. Definition/instance structure, access through type annotations, and mutation inside functions have their respective restrictions.
See [Introducing and updating members](./bindings.md#introducing-and-updating-members) for bindings, and [Built-in table operations](#built-in-table-operations) for array insertion and removal.

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
| `iterate^()`, `keys^()`, `values^()` | Iterate key/value pairs, all keys, or all values |

Mutating operations change their receiver. `pop^` and `remove^` return the extracted value or `nil^`;
the other mutating operations listed above return the receiver.
Mutation inside functions follows the [effect rules](./subroutines.md).

### Arguments and boundaries

- `clone^(policy)` applies a `f^any^ -> any^;` policy to each array and dictionary value, preserving keys. Recursive copying is the policy's responsibility.
- `join^()` can omit the separator. Elements must be strings or numbers. `indexof^` and `contains^` use built-in equality without calling user `op^=`.
- `slice^(begin)` extends to the end; an invalid range returns an empty table.
- `insert^(position, value)` inserts before a non-negative position or after a negative one: `0` prepends and `-1` appends. One position outside either end is permitted; anything farther is a fault.
- Negative `remove^` positions count from the end. Out-of-range or empty `remove^`/`pop^` returns `nil^` without mutation.
- `sort^(comparator)` and `stablesort^(comparator)` accept a negative/zero/positive numeric comparator. Without it, built-in numeric/string ordering applies. Only `stablesort^` guarantees order among equal elements.
- `move^(from, to)` relocates one element. `move^(from, end, to)` copies a range within the receiver; prefixing the arguments with another table copies from that table. Overlap is handled correctly.
- `resize^(length)` requires an element type accepting `nil^`; an explicit fill value can also be supplied. Shrinking discards the tail; negative lengths fault.
- `assign^` replaces contents shallowly, removing keys absent from the source. It also works between instances of the same definition, preserving receiver identity and definition.
- `extend^(self)` appends the array as it stood at the call. `clear^` clears both array and dictionary parts.

## Definitions and construction

### Objects in L^

An object (instance) created from `def^` is also a kind of table.
`def^{…}` defines its field template together with members shared among instances.

Write the template in `self^{…}`. Ordinary member methods go outside that section and take `self^` as their first parameter.
Within a method, access its fields through `self^.fieldName`.
Calling `instance.method(arguments)` automatically passes the instance as `self^`.

Functions and procedures without a `self^` parameter act as static member functions.
Call them as `Definition.function(arguments)` from outside, or `def^.function(arguments)` within the definition.
They receive no instance, so cannot access its fields through `self^`.

<<< @/../sample/docs/reference/objects.lh{lhat}

Here `value` is a per-instance field, `add` and `doubled` are shared methods, and `double` is a static member function.
`c.value` accesses a field, `c.doubled()` calls a method, and `Counter.double(4)` calls a static member function.

Within `doubled`, `self^.value` accesses the field and `def^.double` accesses the function on the definition.
Static members are not local variables, so bare `double(…)` does not reference that member. Use `def^.double(…)` through the enclosing definition.
`self^` refers to the receiving instance; `def^` refers to the definition in which the method was written.

### Definition and construction rules

`def^{…}` creates a definition with an instance template and shared members.
`self^{field = initializer}` declares per-instance fields.
Shared methods belong on the definition and have `self^` as their first parameter.
The `self^{…}` section can also contain callback fields or instance-specific methods.
Fields and shared methods cannot have the same name.

`new^Type{field = value}` constructs a value with an explicit type.
Structural construction retains extra fields; definition defaults are copied from the template for each construction.
Default construction also supports `Definition.new(...)`.
If `override^new` replaces construction, use `Definition.new(...)`.
Instances contain fields and receiver methods; static members belong to the definition.

Inside a definition, `def^` refers to that definition; value-position `Self^` does too.
In type positions, `Self^` means the instance type.
Members of the same definition are visible regardless of textual order.
Table and box defaults are copied per instance, without sharing mutable defaults between instances.
Replacing the default `new` requires `override^`; adding a call alternative requires `overload^`.

## Composition and member conflicts

`A .. B` (also `A :: B`) composes definitions. Delegation targets and order are fixed at definition time.
Duplicate members conflict by default. Use `override^` for replacement and `overload^` for type-based overloads.
Replacement must preserve the original contract: it cannot narrow inputs or widen outputs.
`override^new`, which replaces definition construction, is exempt from that substitutability check.
An overload's type is an intersection; alternatives must be distinguishable by argument types or arity.
`abstract^name:Type` requires a member from a composed implementation; unresolved abstract members prevent construction.
`super^` refers to the corresponding implementation before overriding. There is no multi-level `super^^`.

When named definitions are composed, same-name members without an unambiguous choice become ambiguous.
Select a named part explicitly or resolve the conflict with an override in the composition.
An abstract member cannot be fulfilled by another declaration in the same definition; a composition must fulfill it.
Unimplemented abstract fields can also be supplied during construction by `override^new`.
Delegation alone does not fulfill abstract declarations. An `override^` without a matching implementation can wait for composition, but unresolved requirements prevent construction.
Overload resolution does not use result types or rank alternatives with overlapping argument domains.

### From abstract to concrete types

`abstract^name:Type` declares a required member without supplying an implementation or initial value.
A definition with unimplemented abstract members is an abstract type. **Defining it or composing it while requirements remain unresolved is not itself an error.**
Attempting to construct an instance with `Definition.new(…)` or another construction form is an error while those requirements remain unresolved.

Here `HasCount` is abstract because it requires a `count` field.
Fulfilling that requirement in a composition produces a concrete type that can be constructed.

<<< @/../sample/docs/reference/abstract.lh{lhat}

`FixedCount` fulfills the abstract field with `self^{ count = 10 }` in a later composition part.
The name must match and the value's type must conform to the required type; exact type equality is unnecessary.
Adding a field that fulfills an abstract declaration does not require `override^`.

`GivenCount` supplies the field at construction time through `self^{ count = initial }` inside `override^new`.
The original abstract declaration can remain in the source: the requirement is fulfilled, so construction is allowed.
If `new` contains multiple `self^{…}` constructions, every one must supply the field.
`overload^new` retains the default `new()`, so it cannot fulfill abstract fields this way.

Construction can fulfill abstract fields declared inside `self^{…}`. Abstract methods and other members outside it require implementations supplied through composition.
Fulfilling only some requirements leaves the type abstract if other unimplemented abstract members remain.

## Delegation with delegate^

`delegate^self^.field` inside `def^` exposes receiver methods of a held object as members of the wrapper.
The wrapper's own members win; otherwise lookup follows the delegation target.
A delegated method's `self^` is the held object. Fields, static members, and `new` are not delegated.
Delegation can chain, stopping at the first matching name; an unsuccessful cycle terminates lookup.
A later composition part specifying delegation replaces the earlier target; omitting it preserves the target.
Members are not copied. Replacing the held field changes subsequent delegation.

Type checking uses the same lookup rules.
See [Host types and boxes](./host-types.md) for host-data references, host-value copies, and boxing.

## Enums

`enum^Name { A, B, C = 10 }` declares a nominal enum.
Members are singleton values; `=` and `is^` test the same identity.
`.value` holds the explicit value, or an ordinal starting at 0; explicit integer values reset the next ordinal.
`.enum` identifies the owning enum; `.tostring()` gives the member name.
In type positions, the enum name denotes any member and a member name denotes that singleton.
Listing all members establishes exhaustive pattern matching.
Members cannot be added or replaced after declaration.

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.obj.keys-round], [syntax.obj.delegate], [syntax.obj.members-order], [syntax.obj.abstract-new], [syntax.obj.abstract-override], [syntax.obj.new-default], [syntax.obj], [syntax.obj.keys], [syntax.obj.count], [syntax.obj.chars], [syntax.obj.table-ops], [syntax.enum]).
