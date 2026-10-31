# Using VS Code

## Install the extension

Search for `SAMtak.lhat` in VS Code's Extensions view, or install
[L^ Language Support](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat) from the Marketplace.

Platform-specific Marketplace packages include the `lhatls` language server.
For running and debugging programs, install the `lhat` executable separately as described in [Installation](../guide/installation.md).

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

The extension also includes work on displaying and editing `.lh` source as a graph.
The visual editor is still under development, with limitations in its interface and practical editing experience.
For current capabilities and usage, see the [extension documentation](https://github.com/SAM-tak/lhat-vscode-extension).
