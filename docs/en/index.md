# About L^

**Modern & Better Lua with Visual Programming.**

L^ (elhat) is an embeddable scripting language for games and applications.
It inherits Lua's flexible tables, coroutines, and lightweight embedding, and adds static type checking and type inference.

::: warning Under development
L^ is in pre-1.0 development. Syntax and APIs may change.
This guide follows the repository's `main` branch.
If a released version behaves differently, check the version you are using and its release notes.
:::

## What you can do with this guide

**To embed L^ in your own application, start with the [embedding guide](./embedding/index.md).**
It covers a buildable host example, exposing APIs, GC and event loops, editor integration, and distribution.
Look up individual functions in the [C API reference](./api/index.md).
Look up language notation, types, and evaluation rules in the [L^ reference](./reference/index.md).

To try the language with the CLI, follow these steps:

1. [Install L^](./guide/installation.md) and make the `lhat` command available.
2. Write [your first program](./guide/first-program.md), run it, and check its types.
3. Learn about variables, tables, functions, and procedures in [Language basics](./guide/basics.md).
4. Use completion, diagnostics, and debugging in [VS Code](./tools/vscode.md).

## Language highlights

- Syntax keywords end in `^`, as in `if^` and `let^`.
- Tables serve as arrays, dictionaries, and objects. Array indices start at 0.
- Type inference reduces annotations while supporting type checks before execution.
- Functions (`f^`) perform calculations; procedures (`p^`) handle I/O and changes to external state.
- Typed errors, pattern matching, coroutines, and concurrent tasks are available.
- The C API lets a host application embed L^ and register functions and types.
- Text and visual programming are designed to share the same `.lh` files. VS Code can display and edit them as graphs, but the graph presentation is still being improved, which limits practical use.

## Using a game engine

This guide starts with the standalone CLI.
For engine-specific setup, see the integration projects.

| Integration | Documentation |
| --- | --- |
| Godot | [lhat-gdextension](https://github.com/SAM-tak/lhat-gdextension) |
| Unreal Engine (experimental) | [lhat-UE](https://github.com/SAM-tak/lhat-UE) |

For detailed specifications, see the [design documents](https://github.com/SAM-tak/lhat/tree/main/DesignDocuments) (Japanese).
For an embedding example, see the [C host program](https://github.com/SAM-tak/lhat/blob/main/tests/install_smoke/host.c).
