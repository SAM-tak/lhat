# Embedding in an application

When embedding L^, the host application owns startup, the event loop, and resource lifetimes.
It exposes application APIs to L^ and calls script code when needed.

This guide is for developers integrating L^ into a C or C++ application for the first time.
You can build a working host before learning the entire language, then add features as needed.

## What you will build

[Your first host application](./getting-started.md) implements this round trip:

```text
Host registers app.report
    ↓
Check, compile, and initialize main.lh
    ↓
Host frame loop → script update(dt)
    ↓
Script → host app.report(elapsed)
```

The example does not depend on the standard library. It reads a file, registers an API, runs three frames, and exits.
Its source is in [sample/embedding](https://github.com/SAM-tak/lhat/tree/main/sample/embedding).

## Integration steps

| Stage | Host responsibilities | Read |
| --- | --- | --- |
| 1 | Link the library, check source, and run it | [Your first host](./getting-started.md) |
| 2 | Expose functions, engine objects, and value types | [Host APIs and types](./host-api.md) |
| 3 | Manage callbacks, GC, execution budgets, and shutdown | [Execution and lifetimes](./runtime.md) |
| 4 | Supply host types to the editor and display errors | [Tooling and diagnostics](./tooling.md) |
| 5 | Reload scripts and choose a distribution format | [Reloading and distribution](./deployment.md) |

For actual C binding code, see the [standard library binding examples](./binding-examples.md).
Look up individual functions in the [C API reference](../api/index.md).

## Two objects owned by the host

`LhatProgram` owns loaded source, type-checking results, compiled code, and host API registrations.
`LhatMachine` owns the execution stack and garbage-collected values.
Multiple machines can run code from the same program, but values allocated by one machine cannot simply be passed to another.

The basic sequence is **create Program → register APIs → check → compile → create Machine → install → run**.
Destroy the **Machine before the Program**. Engine objects and callback contexts must remain alive until machine cleanup has finished.

This guide and the C API follow `main`. Use matching versions of the L^ headers, library, and host integration.
