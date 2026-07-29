# lhat

`L^` (lhat) — a new glue programming language experiment.

A bytecode-interpreted language in the spirit of Lua, written in C11 and built
with CMake.

## Requirements

- CMake 3.25 or later
- A C11 compiler
  - Windows: Visual Studio 2022 or later (MSVC)
  - Linux / macOS: GCC or Clang
- [Ninja](https://ninja-build.org/) — recommended; the Visual Studio generator
  is also supported on Windows

## Build

### Windows — Ninja + MSVC (recommended)

Ninja invokes `cl.exe` directly and does not locate the toolchain on its own,
so the MSVC environment has to be loaded into the shell first.
`scripts/devshell.ps1` does that via `vcvars64.bat`:

```powershell
. .\scripts\devshell.ps1      # note the leading dot: it must be dot-sourced
cmake --preset debug
cmake --build --preset debug
.\build\debug\lhat.exe
```

Substitute `release` for `debug` to get an optimized build.

If you build from VS Code with the CMake Tools extension, the environment is
set up by the selected kit and `devshell.ps1` is not needed.

### Windows — Visual Studio generator

The Visual Studio generator finds the toolchain by itself, so no `devshell.ps1`
is required. Use this when you want to debug inside the Visual Studio IDE.

```powershell
cmake --preset vs
cmake --build --preset vs-debug
.\build\vs\Debug\lhat.exe
```

### Linux / macOS

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat
```

## Tests

The suite is built by default and runs through CTest:

```powershell
ctest --test-dir build/debug --output-on-failure
```

Pass `-DLHAT_BUILD_TESTS=OFF` at configure time to skip it.

## Running

There is no virtual machine yet, so the driver only dumps the token stream:

```powershell
.\build\debug\lhat.exe path\to\file.lhat
```

## Presets

| Configure preset | Generator            | Build presets            | Binary directory |
| ---------------- | -------------------- | ------------------------ | ---------------- |
| `debug`          | Ninja                | `debug`                  | `build/debug`    |
| `release`        | Ninja                | `release`                | `build/release`  |
| `vs`             | Visual Studio (2026) | `vs-debug`, `vs-release` | `build/vs`       |

The Ninja presets are single-configuration: the build type is fixed at configure
time. The Visual Studio preset is multi-configuration, so the build type is
chosen by the build preset instead.

All presets set `CMAKE_EXPORT_COMPILE_COMMANDS`, but only the Ninja presets
actually emit `compile_commands.json` — the Visual Studio generator does not
support it. Point clangd at `build/debug/compile_commands.json` for editor
completion and diagnostics.

## Layout

```text
CMakeLists.txt        Build definition
CMakePresets.json     Configure / build presets
src/                  Implementation
  source.[ch]           Source loading and newline normalisation
  token.[ch]            Token definitions
  lexer.[ch]            Lexical analyser
  main.c                Command line driver
tests/                Test suite (CTest)
DesignDocuments/      Language design specifications
scripts/devshell.ps1  Loads the MSVC x64 environment (Windows, Ninja only)
Memo.md               Language design notes (brainstorming, not a spec)
```

## License

Apache License 2.0. See [LICENSE](LICENSE).
