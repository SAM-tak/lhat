# Registering host APIs

Declarations: [program.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/program.h).
Callback type: [object.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/object.h).
Complete registrations before the first `lhat_program_check`.

## Functions and initial bindings

```c
bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context);
```

Registers a function or procedure as a member of `module`. `signature` is an L^ type string.
`call` and `context` are used at runtime. The context is borrowed; keep it alive until execution and release hooks are finished with it.

| API | Purpose |
| --- | --- |
| `lhat_register_global` | Register a host function directly under `L^` |
| `lhat_bind_initial` | Bind `L^.<member>` to an unqualified initial name; separate from function registration |
| `lhat_register_const_integer` / `real` / `bool` / `string` | Expose constants on modules or registered types |

For constants, a NULL `type` means the module itself; a type name selects that type's static members.
For example, `lhat_register_const_real` is the actual function name. Abbreviated names in this row share the same prefix.

## C callbacks

```c
typedef void (*LhatHostFn)(struct LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count);
```

Arguments arrive as an array. Members taking `self^` receive the receiver as an argument too.
Write return values into `answers` and set `*answer_count`.
The result area for ordinary calls is scanned by GC, so further allocations can follow writing a result.
For release hooks, both result pointers are NULL; do not write through them.

## Host types

| API | Result / purpose |
| --- | --- |
| `lhat_register_hostdata_type` | `const LhatHostDataTag *`; nominal reference type wrapping a host pointer |
| `lhat_register_hostdata_subtype` | Derived reference type; the host guarantees pointer compatibility with the base |
| `lhat_register_member` | Register a reference-type member; leading `self^` denotes an instance receiver |
| `lhat_register_hostdata_shared` | Register a contract for sharing pointers across machines, with retain/release |
| `lhat_register_hostvalue_type` | `const LhatHostValueTag *`; inline value type of a specified size |
| `lhat_register_hostvalue_member` | Register a value-type method or operator |
| `lhat_register_hostvalue_field` | Expose a numeric field using its byte offset and field kind |

A NULL tag or false boolean result means failure.
Tags for the same declaration are shared within a process, while callback contexts remain program-specific.
Incompatible sizes or release contracts for the same declaration are rejected.

## Enums, errors, and annotations

| API | Purpose |
| --- | --- |
| `lhat_register_enum` / `lhat_register_enum_valued` | Enumerations with sequential or explicit integer values |
| `lhat_register_error_kind` | Register a propagatable typed-error declaration and its variants |
| `lhat_register_local_error_kind` | Register a local-error declaration |
| `lhat_register_annotation` | Register an annotation name and allowed targets |
| `lhat_register_annotation_signature` | Register annotation argument types |
| `lhat_register_annotation_exclusive` / `lhat_register_annotation_requisite` | Register exclusions and requirements |
| `lhat_register_instantiation_check_handler` | Register an additional host check for type instantiation |

Construct typed errors with `lhat_machine_make_error` and return them as ordinary values.
Include the error-return contract in the signature.
Use `lhat_machine_panic` / `lhat_machine_panic_text` to turn an unrecoverable callback failure into a runtime error.

For examples and lifetime design, see [Host APIs and types](../embedding/host-api.md).
