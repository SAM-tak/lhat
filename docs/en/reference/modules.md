# Modules

## Translation units and exports

`module^name` declares a translation unit's name at the start of a file, at most once per file.
Names can be dotted, such as `game.player`, independently of file paths.
Scripts without module declarations can also be loaded.

Declarations are private by default. Top-level symbols marked with `public^let^`, `public^errordef^`,
or `public^enum^`, for example, become module members.
`public^var^` is invalid. Exported bindings cannot be reassigned, but exporting does not guarantee deep immutability of tables.
There is no need to construct an export table with a final `return^`.

## require^

| Form | Binding |
| --- | --- |
| `let^m = require^"path.lh"` | A name chosen by the importer, `m` |
| `require^"path.lh"` | The loaded unit's declared `module^` name |

Paths are string literals, normally relative to the requesting file.
`"$src/file.lh"` resolves from the project root configured by the host; `$` is not environment-variable expansion.
The unbound shorthand requires the loaded unit to declare `module^`.

`require^` returns a table of exported members. Initialization runs once per unit and repeated requests return the same table.
Type checking follows dependencies statically; circular dependencies are errors.
Because it runs initialization, `require^` cannot appear inside an `f^` body.
Loading source determined at runtime belongs to APIs such as host-provided `std.load`.

## import^ and host bindings

`import^` accesses modules implemented in the host's language and registered and exposed to L^.
Custom application APIs and standard libraries such as `std.math` use this same mechanism.

| Form | Access |
| --- | --- |
| `import^app` | `app.report(...)`, etc. |
| `let^host = import^app` | `host.report(...)`, etc. |
| `let^m = import^std.math` | `m.sqrt(...)`, etc. |

Available names and types depend on host registration. Registered signatures are also used by the checker.
`import^` itself does not execute L^ unit initialization.
Import each child module explicitly where needed.
Initial bindings such as `print` are host configuration, rather than implicit language facilities in every environment.

See [Imports and modules](../guide/modules.md) for usage and
[Exposing host APIs and types](../embedding/host-api.md) for registering bindings.

Sources: [Modules](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/05-modules.md)
([modules.unit], [modules.public], [modules.require], [modules.deps], [modules.host.initial], [modules.host.import]);
[Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md) ([syntax.enum.declare]).
