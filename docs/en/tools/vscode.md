# Using VS Code

## Install the extension

Search for `lhat` in VS Code's Extensions view, or install
[L^ Language Support](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat) from the Marketplace.

Platform-specific Marketplace packages include the `lhatls` language server.
Editing support does not require the CLI.

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

## Edit a file

Open your working directory in VS Code and create a `.lh` file.
You can start with `hello.lh` from [Your first program](../guide/first-program.md).

The extension provides:

- Code completion and signature help
- Hover information
- Go to definition
- Diagnostics, including type checking
- Semantic highlighting

To inspect a diagnostic, hover over the affected code or open VS Code's Problems view.

## Run from the terminal

Open VS Code's terminal and run these commands from the directory containing your file.
These examples use the reference CLI; first [prepare the CLI](../guide/installation.md#try-the-reference-cli).

```sh
lhat hello.lh
lhat --check hello.lh
```

If `lhat` cannot be found, specify the executable's path directly.
You may need to restart VS Code after changing `PATH`.

## Debug a program

The extension supports breakpoints, stepping, variable inspection, and expression evaluation.
For executable paths and launch configuration, follow the [extension README](https://github.com/SAM-tak/lhat-vscode-extension).
Embedding in a host application also requires the host's type information and debug connection configuration.

## Visual programming

The extension can display and edit `.lh` source as a graph.
The graph presentation is still being improved, which limits practical use.
For current capabilities and usage, see the [extension documentation](https://github.com/SAM-tak/lhat-vscode-extension).
