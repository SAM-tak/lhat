# Installation

Like Lua, L^ is designed to be embedded as a library in your own application.
The embedding host application runs scripts; the `lhat` CLI (`lhat.exe` on Windows) is a reference implementation.
See the [embedding guide](../embedding/index.md) to integrate L^ into an application.

This page sets up the VS Code extension for editing L^ source.

## Install the VS Code extension

1. Open VS Code's Extensions view.
2. Search the Marketplace for `lhat`.
3. Install **L^(Lhat) Language Support**, published by **SAMtak**.
4. Open your working folder and create or open a `.lh` file.

You can also install it from the [Marketplace extension page](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat).

The platform-specific Marketplace package **includes the `lhatls` language server**.
No separate language-server installation is needed for completion, hover information, go to definition, or type-checking diagnostics.
These editing features do not require the `lhat` CLI either.
See [Using VS Code](../tools/vscode.md) for editing, debugging, and visual programming.

## Use your own language server

To use a different language server, such as a locally built `lhatls`, set **`lhat.serverPath`** in VS Code to its executable path.

Example `settings.json` on Windows:

```json
{
  "lhat.serverPath": "C:\\path\\to\\lhat\\build\\release\\lhatls.exe"
}
```

On Linux or macOS, use a path such as `/path/to/lhat/build/release/lhatls`.
An explicit path takes precedence over the bundled server. Clear the setting to use the bundled server again.
When a package has no bundled server and the setting is empty, the extension looks for `lhatls` on `PATH`.

After configuring the path, run **L^: Restart Language Server** from the Command Palette.
See the [extension README](https://github.com/SAM-tak/lhat-vscode-extension) for server build instructions and detailed settings.

## Try the reference CLI

To run guide examples without preparing a host application, you can use the `lhat` CLI.
The extension does not include it. Prepare it when trying standalone execution or debugging that launches the CLI.

1. Download the `lhat` archive for your OS and CPU from [Releases](https://github.com/SAM-tak/lhat/releases).
2. Extract it, keeping the `messages` directory alongside the executable if one is included.
3. Add the directory to `PATH`, or invoke the executable by its path.

```sh
lhat --help
```

From the extracted directory, use `./lhat.exe` in PowerShell or `./lhat` on Linux/macOS.
If no suitable release is available or you want to build from source, see the [repository README](https://github.com/SAM-tak/lhat#building-and-testing).

In the following guides, `lhat` names this reference CLI. Substitute its executable path if it is not on `PATH`.
To debug with the CLI, set `lhat.runtimePath` when needed. This is separate from the language-server setting `lhat.serverPath`.

Continue to [Your first program](./first-program.md) to try the CLI, or [Language basics](./basics.md) to learn the notation.
