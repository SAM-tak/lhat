# Installation

Use the `lhat` CLI to run L^ programs.
The VS Code extension provides editing support; running and debugging require a separate `lhat` executable.

## Use a release binary

1. Open [Releases](https://github.com/SAM-tak/lhat/releases).
2. Download the `lhat` archive for your OS and CPU. `lhatls` is the language server.
3. Extract the archive. If it includes a `messages` directory for translated diagnostics, keep it alongside the executable.
4. Add the extracted directory to `PATH`, or invoke the executable by its path.

Run the following command in a terminal and check that it displays help.

```sh
lhat --help
```

In PowerShell, use `./lhat.exe` instead of `lhat` when running from the extracted directory.
On Linux or macOS, use `./lhat`. If needed, make it executable with `chmod +x ./lhat`.

If no suitable release binary is available, or you want to try the latest source, build it as follows.

## Build from source

You need Git, a C11 compiler, CMake 3.25 or later, and Ninja.
On Windows, also install the Visual Studio C++ build tools.

First, clone the repository.

```sh
git clone https://github.com/SAM-tak/lhat.git
cd lhat
```

### Windows

Run these commands in PowerShell. `devshell.ps1` sets up the MSVC build environment in the current shell.

```powershell
. ./scripts/devshell.ps1
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat.exe --help
```

### Linux / macOS

Install GCC or Clang, CMake, and Ninja, then run:

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat --help
```

### Use the executable you built

The following pages refer to the executable as `lhat`.
If it is not on `PATH`, substitute `./build/debug/lhat.exe` on Windows or
`./build/debug/lhat` on Linux and macOS.
These relative paths assume you are running from the repository root.

For an optimized build, change both the configure and build presets to `release`.
See the [repository README](https://github.com/SAM-tak/lhat#building-and-testing) for build options.

Once ready, continue to [Your first program](./first-program.md).
