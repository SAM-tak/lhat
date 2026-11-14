# Overview

This reference covers the settled L^ language-core specification and public standard-library APIs.
It describes source notation, type rules, evaluation, and control flow.
For a walkthrough, see [Language basics](../guide/basics.md); for host integration, see the [embedding guide](../embedding/index.md).

## Topics

| Topic | Contents |
| --- | --- |
| [Lexical structure and source](./lexical.md) | Encoding, names, hats, literals, comments, input forms |
| [Bindings and scope](./bindings.md) | Declarations, reassignment, members, name resolution, destructuring |
| [Types](./types.md) | Basic and structural types, unions, signatures, conformance, type descriptors |
| [Expressions and operators](./expressions.md) | Precedence, comparisons, logic, concatenation, nil, casts |
| [Numbers and strings](./numbers-and-strings.md) | Numeric comparison, formats, conversion, bit operations, Unicode, search and splitting |
| [Control flow](./control-flow.md) | Conditions, pattern matching, iteration, exits, cleanup |
| [Functions and procedures](./subroutines.md) | Definitions, arguments, results, effects, recursion |
| [Coroutines](./coroutines.md) | Creation, starting, resuming, yield, await, types |
| [Tables and objects](./objects.md) | Tables, built-in members, definitions, construction, composition, enums |
| [Host types and boxes](./host-types.md) | Host data, host values, copying and storage restrictions, mutable and sealed boxes |
| [Errors](./errors.md) | Error types, construction, handling, propagation, panic |
| [Modules](./modules.md) | Translation units, exports, require, host bindings |
| [Annotations](./annotations.md) | Syntax, arguments, placement, host validation |
| [Standard library](./stdlib/index.md) | Module APIs for math, I/O, data formats, concurrency, and more |

## Specification scope

The published reference follows the repository's `main` branch, rather than a fixed release.
Each page identifies its source design documents and specification labels.
Design rationale, revision history, proposals, and unresolved issues are excluded from the rules.
Where the source explicitly identifies an implementation limitation, it is noted here too.

The availability of host APIs such as `print` and `std.task` depends on the execution environment.
See the [standard library](./stdlib/index.md) for API details, the [guide](../index.md) for usage,
and the [C API reference](../api/index.md) for registration and execution APIs.
