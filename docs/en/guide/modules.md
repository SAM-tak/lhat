# Imports and modules

Use `import^` to access standard libraries and `require^` to access another L^ file.
To use your own file as a module, declare its name with `module^` and mark the symbols
you want to expose with `public^`.

## Import a standard library

Writing `import^std.math` makes the functions and types in `std.math` available in that translation unit.
For example, `std.math.sqrt` calculates a square root.

<<< @/../sample/docs/modules/import.lh{lhat}

You can also bind the module to a name of your choice, as in `let^m = import^std.math`.
Then you can write `m.sqrt(...)`. The example above prints `9.0`, then `12.0`.

`import^` accesses modules registered by the CLI or the embedding host.
The CLI provides the standard libraries. To load your own L^ file, use `require^` as shown below.

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
