# Exposing host APIs and types

Register APIs before `lhat_program_check`.
One registration connects the signature read by the type checker to the C callback used at runtime.
For a large API with interdependent types, use two phases: **register all types, then register functions and members**.

Read `stdlib/` for practical implementations: `math.c` for host functions, `io.c` for hostdata,
and `mathcomplex.c` with `mathvalue_impl.h` for hostvalue types.
The [standard library binding examples](./binding-examples.md) quote actual code to explain how registration connects to callbacks.

## Functions and procedures

The [minimal host](./getting-started.md) exposes this procedure:

```c
lhat_register_func(program, "app", "report", "p^number^;", report, &app);
```

`module` and `name` correspond to `app.report` in scripts. `context` is passed unchanged to the callback.
Check the registration result and stop before checking or running source if registration fails.

| Example signature | Contract |
| --- | --- |
| `p^number^;` | Procedure taking one number |
| `f^number^ -> number^;` | Function taking and returning a number |
| `p^self^, number^;` | Instance method taking a receiver and a number |

Use `p^` for I/O or changes to external state. The host is responsible for honoring its declared signatures.
Registering a callback as `f^` does not make L^ analyze its C implementation to prevent side effects.

```c
static void callback(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count);
```

Write return values into `answers` and set `*answer_count`. An ordinary `p^` writes no return values.
A `number^` can have integer or real representation. Use helpers such as `lhat_number_as_real` for general numbers
or `lhat_number_as_whole` when an integer is required. Check the kind and required range before conversion.
Catch C++ exceptions inside callbacks and translate them to L^ error values or `lhat_machine_panic_text`.

## Engine objects as hostdata

Expose references to host-owned nodes, windows, textures, and similar objects as hostdata.

```c
const LhatHostDataTag *node_tag =
    lhat_register_hostdata_type(program, "app", "Node");
/* Register node_tag's members after checking that it is non-NULL. */
lhat_register_member(program, "app", "Node", "move",
                     "p^self^, number^;", move_node, context);
```

At runtime, create a wrapper with `lhat_machine_make_hostdata(machine, node_tag, pointer, &value)`.
Inside a callback, retrieve the receiver with `lhat_hostdata_pointer(arguments[0], node_tag)`.
Handle NULL and references to engine objects that have already become invalid.

Wrapper collection and engine object lifetime are separate concerns.
For objects that the engine may delete first, one design is to store a stable ID and generation and validate them on access.
For reference-counted objects, pair retaining at wrapper creation with releasing in a `dispose` member.
Without a registered `dispose`, L^ does not free the object behind the pointer.

Release hooks may run during GC and must not reenter the L^ API.
When called through GC, `answers` and `answer_count` are NULL.
If multiple wrappers can point to the same object, choose ownership rules that prevent double release.
To reuse wrappers, use the [VM's weak cache](../api/machine.md#host-roots-and-weak-references) rather than a raw host-side pointer cache.

## Numeric data as hostvalue

Hostvalue types store small numeric data directly in consecutive VM slots.
The standard library's [mathvector3.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.c) uses this approach.

```c
typedef struct { float x, y, z; } Vec3;
const LhatHostValueTag *vec3_tag =
    lhat_register_hostvalue_type(program, "app", "Vec3", sizeof(Vec3));
/* After checking vec3_tag, register each field and check every result. */
lhat_register_hostvalue_field(program, "app", "Vec3", "x",
                              offsetof(Vec3, x), LHAT_HVFIELD_F32);
```

Register `y` and `z` similarly. Expose operators and methods with `lhat_register_hostvalue_member`.
Value types cannot be stored directly in tables, closure captures, or `any^`.
If they must be retained longer, design a host-side boxing API.

## Adding standard library modules

CLI registrations such as `print` and `std.io` are not automatically added to a host.
Select and register the modules you need. For example, for scalar math:

```c
#include <lhatstdlib/math.h>
/* Before checking source; check the return value. */
lhatstdlib_math_register(program);
```

With an installed SDK, also link `lhat::lhatstdlib`.
See the [registration reference](../api/registration.md) for API details.
