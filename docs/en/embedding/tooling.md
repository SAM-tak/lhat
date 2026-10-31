# Connecting tooling and diagnostics

## Supply host types to the editor

Host-specific types such as `app.Node` are not initially known to the standard CLI or language server.
Use `lhat_program_dump_host_api` on a program with the same registrations as the runtime to generate JSON.
The language server reads its types, signatures, and initial bindings without starting the host.
Callback implementations and host pointers are not included.

The [minimal host](./getting-started.md) includes a dump mode.
From the repository root, write its output into the directory containing the scripts you are editing.

```sh
./build/embedding/embedding_host --dump-host-api > sample/embedding/lhat-host.json
```

Use `embedding_host.exe` on Windows. Use a shell that preserves UTF-8 when redirecting output.
If you implement a file-output mode in the host, write the UTF-8 bytes unchanged.

Open `sample/embedding` as a folder in VS Code to use its `lhat-host.json`.
Regenerate the file whenever API registrations change. This repository ignores it as generated output.
The regular `lhat --dump-host-api` command describes the CLI's APIs, not your application's APIs.

## Display type-checking diagnostics

There are two diagnostic entry points:

| Entry point | Contents |
| --- | --- |
| `lhat_program_diagnostic_count` / `lhat_program_diagnostic` | Program problems such as unreadable files and dependency cycles |
| `lhat_program_units` → `lhat_unit_diagnostic_count` / `lhat_unit_diagnostic_write` | Syntax, type, and other per-file problems |

A non-NULL result from `lhat_program_check` does not by itself mean success.
Also check `lhat_program_has_errors` and `lhat_unit_ok`.
Report compilation failures with `lhat_program_compile_failure` and `lhat_compile_message_write`.

Many output functions accept `(NULL, 0)` to measure the required byte count.
The result excludes the terminating NUL, so allocate `needed + 1` bytes and call again.
If using a small fixed-size buffer, account for truncation.

## Display runtime errors

`lhat_run_error_write` formats the location and message of an execution failure.
For a custom UI, obtain frames with `lhat_machine_fault_depth` / `lhat_machine_fault_frame`.
This information changes on subsequent execution, so copy it into host-owned text or data first.

## Script contracts and inspectors

Use `lhat_unit_export_conforms` to check entry-point types before execution.
`lhat_unit_export_type` returns a compiled public value's type as a `LhatRuntimeType` descriptor.
Use it to inspect object members for an inspector or property editor.
Do not retain descriptors across reloads that invalidate them.
If accepting annotations, register their names and allowed targets with `lhat_register_annotation`.

For embedding DAP debugging in a host, see the [debugger design](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/09-debugger.md) (Japanese).
Installing the VS Code extension alone does not automatically configure a connection to an arbitrary host.
