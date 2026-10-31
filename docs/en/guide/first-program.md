# Your first program

After [installing L^](./installation.md), create `hello.lh` in a working directory of your choice.
L^ source files use the `.lh` extension.

## Print a string

Save the following code.

<<< @/../sample/docs/hello.lh{text}

From the directory containing the file, run:

```sh
lhat hello.lh
```

```text
Hello, L^!
```

`print` is a procedure that displays values. Strings are enclosed in double quotes.
The same example is available at `sample/docs/hello.lh` in the repository.

## Check types without running the program

```sh
lhat --check hello.lh
```

If there are no problems, the command exits successfully. It does not execute `print`, so no greeting is displayed.
Use this to check a program before running it.

Files are checked in strict mode by default.
Unresolved types and unsafe operations are diagnosed before execution.

## Try the interactive prompt

Start `lhat` without arguments to enter the interactive prompt.

```sh
lhat
```

You can enter short snippets such as `print("Hello, L^!")`.
The prompt uses relaxed mode, checking unresolved types at runtime.
Its type checking is less strict than when running a file.

## Next steps

In [Language basics](./basics.md), you will work through a program that calculates values from a table.
To set up your editor first, continue to [Using VS Code](../tools/vscode.md).
