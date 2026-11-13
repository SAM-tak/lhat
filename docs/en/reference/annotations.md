# Annotations

## Notation and meaning

Annotations attach metadata to declarations for the host.
The host defines their meaning; L^ validates registered contracts and retains the metadata.
Annotations do not execute and rewrite declarations.

```text
@name
@name(argument, argument)
declaration
```

There is no whitespace between `@` and its name.
Names are simple identifiers, not dotted forms such as `@game.export`.

## Arguments

| Form | Data passed to the host |
| --- | --- |
| `0`, `-50`, `1.5` | Number |
| `"text"` | String |
| `true^`, `false^` | Boolean |
| `NAME` | Name spelling, without value resolution |

General expressions, runtime calculations, and named arguments are not supported.
A name argument does not evaluate a program constant.

## Placement and validation

Annotation locations are the unit itself, top-level bindings, fields in `self^{…}`, and members of `def^`.
Each host registration specifies which of these locations are permitted; arbitrary statements cannot be annotated.
Annotations exposing a value to the host require a public binding.
The checker validates names, argument kinds and counts, placement, multiplicity, exclusions, and required combinations.
Unregistered annotations produce errors rather than being silently ignored.

Editor exposure and RPC in game engines are examples of host-defined meanings.
Consult the embedding application's API for available annotations.

Source: [Syntax](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)
([syntax.annot], [syntax.annot.spelling], [syntax.annot.literal-args], [syntax.annot.where], [syntax.annot.host]).
