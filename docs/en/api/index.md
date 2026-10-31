# C API reference

This reference describes the roles, call order, and ownership rules of the main APIs used for embedding.
If starting an integration, read the [embedding guide](../embedding/index.md) first.
For complete declarations and detailed contracts, see the [public headers](https://github.com/SAM-tak/lhat/tree/main/include/lhat).

| Category | Contents |
| --- | --- |
| [Programs and units](./program.md) | Loaders, checking, compilation, diagnostics, reloading, and binaries |
| [Registering host APIs](./registration.md) | Functions, reference types, value types, constants, errors, and annotations |
| [Machines and values](./machine.md) | Calls, coroutines, GC, roots, errors, and value creation |

## Headers and linking

An ordinary host includes `"lhat.h"`.
It brings together `program.h`, `vm.h`, value and object definitions, diagnostics, debugging, and the public port API.
With an installed SDK, link the CMake targets `lhat::lhat` and the standard port `lhat::lhatport`.
For standard library modules, add `lhat::lhatstdlib` and the required `<lhatstdlib/...h>` headers.

## Ownership basics

| Data | Owner / lifetime |
| --- | --- |
| `LhatProgram *` | Created by the host; destroyed with `lhat_program_free` |
| `const LhatUnit *` and compiled protos | Owned by the program; account for retired code lifetimes during reloads |
| `LhatMachine *` | Created by the host; destroyed with `lhat_machine_dispose` |
| Machine strings, tables, and closures | Managed by the machine's GC; C variables alone do not retain them |
| Loader result buffer | Allocated with `lhat_alloc`, then transferred to the program |
| Binary and signature-table output buffers | Returned to the caller, which frees them with `lhat_free` |
| Registered `context` and hostdata targets | Lifetime managed by the host |

## Results and output conventions

- Check each `bool` according to its contract. Table writes require checking both the result and `refused`.
- First inspect a `LhatRunResult`'s `status` to distinguish success, budget suspension, and failure.
- Many text-output functions return the required byte count excluding the terminating NUL; pass a buffer of that size plus one.
- Check a `LhatValue`'s kind before using its getter.

Paths and strings use UTF-8. Unless stated otherwise, lengths are byte counts.
The language's `number^` type has both integer and real representations.
