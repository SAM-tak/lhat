# Data files (LTON)

Pure data such as settings or game data can be written in LTON (L^ Table Object Notation, extension `.lton`) instead of `.lh`.
LTON contains only what goes inside a table literal's braces. Comments, strings, numbers, and expressions are written as in L^, and the same lexer and parser read them.

## Writing data

`sample/docs/modules/settings.lton` holds window settings.

<<< @/../sample/docs/modules/settings.lton{lhat}

Omit the outer `{` `}` and `return^`. Separate elements with `,`; a trailing `,` is allowed.
Elements take the same forms as in a table literal: positional elements, `name = value`, and `[key] = value`.
An empty file is an empty table.

Values need not be precomputed. You can write arithmetic such as `480 * 2` and string concatenation with `..`.

## Reading

`std.lton.load` reads a file and returns a table.

<<< @/../sample/docs/modules/settings.lh{lhat}

The returned table has type `t^{}`. Its contents are unknown at check time, so you cannot write `conf.width` directly.
Read with an index such as `conf["width"]`, or check the shape with `fits^` and use members inside that branch.

The host's loader resolves the path, not relative to the reading file. In the CLI, paths are relative to the working directory.
Run this command from the repository root to print `960`, then `demo 518400`:

```sh
lhat sample/docs/modules/settings.lh
```

To read from a string, use `std.lton.parse`. Both are functions (`f^`); reading itself has no side effects.
An unreadable file returns `LtonError.CannotRead`, and a body that fails checking returns `LtonError.Rejected`.

## Choosing between `.lh` and LTON

The same data can also be written in an `.lh` module that exposes values with `public^`.

```lhat
module^demo.settings
public^let^window = { title = "demo", width = 480 * 2, height = 540 }
```

After `require^`, you can write `settings.window.width` directly, because the checker builds a type from the literal's contents.
LTON differs in these ways:

- **No type is generated.** The result is always `t^{}`. However large the data grows, the types handled by the checker and the editor do not.
  In `.lh`, the shape of the data becomes its type.
- **No effects.** The body is checked as the body of a function (`f^`), so procedures (`p^`) cannot be called.
  Names bound by the host are not visible either.
- **Read at run time, and writable.** The path is a string value, so the file to read can be chosen at run time.
  `std.lton.stringify` and `std.lton.save` write a table as LTON, and reading it back yields a table with the same contents.
- **The editor checks syntax only.** The language server checks only the syntax of open `.lton` files and builds no type information.

Use `.lh` for small constants that code should see as typed members; use LTON for settings replaced from outside and for large data that tools read and write.

## Writing enums

An LTON file may begin with `import^` and `require^`, so you can use enums and constants of the imported modules as values.

```lhat
import^game.code

{ op = game.code.Instruction.Push, arg = 1 },
{ op = game.code.Instruction.Load, arg = "x" },
```

Imports may appear only before the elements. They can be aliased, as in `let^c = import^game.code`.
No name that was not imported is visible.
When `std.lton.stringify` writes a table containing enums, it emits the necessary imports at the top automatically.

## Further reading

- See [std.lton](../reference/stdlib/lton.md) for the API and writing rules.
- See `stdlib/lton.h` for reading directly from a C host.
