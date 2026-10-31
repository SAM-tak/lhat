# Programs and units

Declarations: [program.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/program.h)
and [port.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/port.h).

## Creation and loaders

```c
typedef char *(*LhatProgramLoader)(void *context, const char *path,
                                  size_t *length);
LhatProgram *lhat_program_new(bool strict, LhatProgramLoader load, void *context);
void lhat_program_free(LhatProgram *program);
```

`strict` controls type-checking strictness. A NULL `load` reads no files.
The loader returns NULL for unavailable files, or a buffer the program frees with `lhat_free`.
Use `lhat_load_file` for the standard filesystem loader.

`lhat_program_set_lock` supplies lock/unlock callbacks and a context for shared program updates.
Set them before execution starts. A recursive lock is not required.
Create programs and register APIs serially.

## Checking, compiling, and installing

```c
const LhatUnit *lhat_program_check(LhatProgram *program, const char *path);
bool lhat_program_compile(LhatProgram *program);
bool lhat_program_install(const LhatProgram *program, LhatMachine *machine);
```

| API | Contract |
| --- | --- |
| `lhat_program_check` | Checks dependencies first; returns NULL if the file itself cannot be read. A non-NULL unit still requires diagnostic checks |
| `lhat_program_has_errors` | Reports whether the program or its units have problems |
| `lhat_unit_ok` | Checks a unit's checking result |
| `lhat_program_compile` | Compiles checked units not yet compiled; false means failure |
| `lhat_program_compile_status` / `lhat_program_compile_failure` | Compilation failure information |
| `lhat_compile_message_write` | Formats a compilation failure message |
| `lhat_program_install` | Places host registrations in the machine's `L^.modules` and related locations; false means failure |
| `lhat_unit_proto` | The unit's compiled entry body; do not retain beyond the program |

## Units and diagnostics

| API | Purpose |
| --- | --- |
| `lhat_program_units` / `lhat_unit_next` | Iterate loaded units |
| `lhat_unit_path` / `lhat_unit_module_name` / `lhat_unit_state` | Unit identity and state |
| `lhat_program_diagnostic_count` / `lhat_program_diagnostic` | Program-level diagnostics |
| `lhat_program_error_message` | Convert a program diagnostic code to a message |
| `lhat_unit_diagnostic_count` / `lhat_unit_diagnostic_write` | Enumerate and display per-file diagnostics |
| `lhat_unit_source` | Source associated with diagnostic positions |

`lhat_unit_diagnostic_write(unit, index, rich, out, capacity)` returns the required byte count.
`rich` selects output including source locations and the affected code.

## Export contracts and type information

```c
bool lhat_unit_export_conforms(const LhatUnit *unit, const char *name,
                               const char *signature);
```

Checks whether a public value conforms to the signature. A missing name or invalid type string also returns false.
Enumerate names with `lhat_unit_export_count` / `lhat_unit_export_name`.
`lhat_unit_export_type_text` returns a type spelling; `lhat_unit_export_type` returns a `LhatRuntimeType` descriptor.
The latter requires a compiled body and becomes invalid on reload or disposal.

## Reloading

```c
size_t lhat_reload(LhatProgram *program, const char *path,
                   LhatMachine *const *machines, size_t machine_count);
```

Pass every machine on which this program was installed. Call at a safe stopped boundary.
The result counts invalidated units: 0 means unchanged, and `SIZE_MAX` means an unknown path.
Check diagnostics and other status separately for rechecking failures.

| API | Purpose |
| --- | --- |
| `lhat_program_invalidate` | Invalidate a changed unit and its dependents without freeing old code |
| `lhat_machine_forget_unit` | Forget a registered module so subsequent execution can recreate it |
| `lhat_program_retired_count` | Count retired bodies waiting for reclamation |
| `lhat_program_discard_unheld` | Collect on all affected machines and reclaim unheld retired bodies; true when complete |
| `lhat_program_discard_retired` | Unconditionally free retired bodies; use only when the host guarantees no old code is running or held |

## Distribution and editor information

| API | Contract |
| --- | --- |
| `lhat_unit_write_binary` | Serialize a compiled unit; check `LhatWriteStatus` and free returned bytes with `lhat_free` |
| `lhat_program_is_binary_unit` | Identify a binary-unit buffer |
| `lhat_program_write_signatures` | Write a VM signature table after registering APIs in a full build |
| `lhat_program_read_signatures` | Read a signature table before API registration in a VM-only build |
| `lhat_program_dump_host_api` | Write registered APIs as editor JSON; measure with `(NULL, 0)` |

For compatibility requirements and distribution steps, see [Reloading and distribution](../embedding/deployment.md).
