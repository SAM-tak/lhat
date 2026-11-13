# Imports and modules

`import^` brings in modules implemented in the host application's language and bound and exposed to L^.
Your application's own APIs and standard libraries such as `std.math` use the same mechanism.
Use `require^` to access another L^ file.
To use your own file as a module, declare its name with `module^` and mark the symbols
you want to expose with `public^`.

## Use features provided by the host application

Use `import^moduleName` to access functions and types exposed by the host, such as APIs
for manipulating game objects or passing computation results back to an application.
The embedding application determines which module names and APIs are available.

For example, the [minimal host](../embedding/getting-started.md) exposes a procedure implemented in C
as `report` in the `app` module. This script runs inside that host:

<<< @/../sample/embedding/main.lh{lhat}

`let^app = import^app` imports the host's module, and `app.report(elapsed)` calls its procedure.
You can choose an alias: with `let^host = import^app`, the call becomes `host.report(elapsed)`.
Exporting `update` with `public^` lets the host call back into the script, so the host and script can use each other's features.

This `app.report` API is provided by the sample host. Running the script in the CLI would require the same registration.
See [Exposing host APIs and types](../embedding/host-api.md) for how to bind and register your own APIs on the host side.
Registered signatures also support L^ type checking and editor completion.

## Use standard libraries through the same mechanism

Writing `import^std.math` makes the functions and types in `std.math` available in that translation unit.
For example, `std.math.sqrt` calculates a square root.

<<< @/../sample/docs/modules/import.lh{lhat}

You can also bind the module to a name of your choice, as in `let^m = import^std.math`.
Then you can write `m.sqrt(...)`. The example above prints `9.0`, then `12.0`.

Standard libraries are another example of modules registered and exposed by the host.
The CLI provides them; in your own host, register the libraries you want to use.
To load your own L^ file, use `require^` as shown below.

## Declare a module and export symbols

`sample/docs/modules/arithmetic.lh` is a module that exports a function for doubling a number.

<<< @/../sample/docs/modules/arithmetic.lh{lhat}

Write `module^moduleName` at the start of the file. Names can contain dot-separated components,
as in `demo.arithmetic`, and are independent of the file's location.
A source unit that is loaded and checked separately, such as this file, is called a translation unit.

Declarations are private by default. Other translation units can access `double` because it is marked
`public^let^`; `factor` is only available inside this module.
Exported symbols automatically become module members, so you do not need to return an export table at the end.

## Load a module from another file

Use `require^` in `main.lh`, in the same directory, to load the module.

<<< @/../sample/docs/modules/main.lh{lhat}

`let^calc = require^"arithmetic.lh"` returns a table whose members are the exported symbols
and binds it to the alias `calc`. The private `factor` is not imported.
Specify the path as a string literal; it is resolved relative to the file requesting the module.

Writing `require^"arithmetic.lh"` without a binding lets you refer to
`demo.arithmetic.double(...)` using the declared module name. Loading the same module again
returns the same table; initialization only runs once.
Because `require^` runs initialization code, it cannot appear inside a function (`f^`) body.

Run this command from the repository root to print `42`, then `10`:

```sh
lhat sample/docs/modules/main.lh
```
