# About L^

L^ (elhat) is an embeddable scripting language for games and applications.
It builds on Lua's flexible tables and coroutines with static type checking and type inference.

Text and visual programming in L^ share the same `.lh` files.
You can view a text program as a graph and review changes using ordinary source diffs.

::: warning Under development
L^ is in pre-1.0 development. Syntax and APIs may change.
This guide follows the repository's `main` branch.
If a released version behaves differently, check the version you are using and its release notes.
:::

## What you can do with this guide

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

## Using a game engine

This guide starts with the standalone CLI.
For engine-specific setup, see the integration projects.

| Integration | Documentation |
| --- | --- |
| Godot | [lhat-gdextension](https://github.com/SAM-tak/lhat-gdextension) |
| LÖVE / LÔVE | [lhat-love](https://github.com/SAM-tak/lhat-love) |
| Unreal Engine (experimental) | [lhat-UE](https://github.com/SAM-tak/lhat-UE) |

For detailed specifications, see the [design documents](https://github.com/SAM-tak/lhat/tree/main/DesignDocuments) (Japanese).
For an embedding example, see the [C host program](https://github.com/SAM-tak/lhat/blob/main/tests/install_smoke/host.c).
