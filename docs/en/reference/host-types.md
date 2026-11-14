# Host types and boxes

## Host data and host values

The host can register types as well as functions. Import their names and signatures with `import^`.

| Kind | Behavior in L^ |
| --- | --- |
| Host data (hostdata) | Reference to a host-owned object, such as a file or channel |
| Host value (hostvalue) | Copied value, such as a vector or complex number |

Host-data types have nominal identity and also conform to their single registered base type, if any.
Different wrappers with the same tag and underlying pointer are the same object and key.
A registered `[]` provides host-data index reads; a returned host value is a copy.
These base-type and index-registration rules do not apply to host values.
Lifetime, sharing, and transfer across threads follow the registration contract.

## Host-value construction and copying

A host value's type and width are statically known. Assignment and passing copy its contents.
Standard mathematical types use [standard-library constructor functions](./stdlib/index.md#constructing-host-values), not `new^Type{…}`.
Fields such as `v.x` read and write registered fields.
Writing to a temporary copy returned by a call or host-data indexing is rejected.
Bind it to a local variable to modify it; use the host API to write back when needed.

## Where values can be stored

Host values can occupy statically typed locals, fixed parameters, and single results.
They cannot directly escape into table elements, captured variables, `any^`, variadic arguments, definition fields,
tuple positions, persistent REPL top-level bindings, or the program's final result. These restrictions also apply in relaxed mode.

`T|nil^` and `T|ErrorKind` are supported unions distinguished by their leading tag.
Unions with a different host-value type, a tuple, or `string^`, and widening to `any^`, are not supported.
Narrow to a specific T with `fits^` or an absence check before boxing.

## Mutable boxes

`box^value` creates `T.Box^`, an ordinary heap object that can be stored in tables, captured, or widened to `any^`.

| Operation | Rule |
| --- | --- |
| `b.get()` | Copy out the host value |
| `b.set(value)` | Replace contents with a value of the same type |
| `a = b` | Same type and content bytes |
| `a is^ b` | Same box object |

Modifying an extracted copy does not change the box; write it back with `set()`.
Boxes have no `dispose()`.

## Sealed boxes and keys

`constbox^value` makes a sealed copy of type `T.ConstBox^`, including when the operand is already a box.
It has `get()` but no `set()`.
`T.Box^` also conforms to a read-only `T.ConstBox^` requirement, so that static type alone does not guarantee sealing.

Only an actually sealed box can be inserted as a table key.
Lookups can also use a mutable box or unboxed host value, matching its current contents.
Attempts to insert an unsealed box through a type annotation are still rejected at runtime.
Box defaults in definitions are copied independently for each instance.

## Example

<<< @/../sample/docs/reference/host-types.lh{lhat}

Source: [Modules](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/05-modules.md)
([modules.hostdata], [modules.hostdata.base], [modules.hostdata.index], [modules.hostdata.sharing], [modules.hostvalue], [modules.hostvalue.union]).
