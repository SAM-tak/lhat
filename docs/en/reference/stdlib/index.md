# Standard library

Reference for standard modules registered and exposed by the host. Each page lists its purpose, types, functions, methods, constants, and registered signatures.

This list covers modules registered by the CLI. Register the modules you need in a custom host. Availability may vary with build options.

| Module | Purpose |
| --- | --- |
| [std.math](./math.md) | Mathematical functions and constants |
| [std.math.complex](./math-complex.md) | Complex numbers |
| [std.math.vector2](./math-vector2.md) | 2D vectors |
| [std.math.vector3](./math-vector3.md) | 3D vectors |
| [std.math.vector4](./math-vector4.md) | 4D vectors |
| [std.math.quaternion](./math-quaternion.md) | Quaternions |
| [std.io](./io.md) | Standard I/O and files |
| [std.json](./json.md) | JSON encoding and decoding |
| [std.lton](./lton.md) | Configuration data using L^ notation |
| [std.regex](./regex.md) | Regular expressions |
| [std.random](./random.md) | Random numbers and reproducible state |
| [std.task](./task.md) | Concurrent tasks on a worker pool |
| [std.thread](./thread.md) | Individual OS threads |
| [std.channel](./channel.md) | Value queues between machines |
| [std.async](./async.md) | Timers and external completion waits |
| [std.binary](./binary.md) | Binary data and fixed-bit formats |
| [std.stream](./stream.md) | In-memory byte streams |
| [std.net](./net.md) | Nonblocking UDP networking |
| [std.load](./load.md) | Runtime source loading |
| [std.sys](./sys.md) | Shared system errors |
| [std.debug](./debug.md) | Debug output and stack information |

## Constructing host values

The standard library provides the following five host-value types. To construct them from components, use `new` directly on their module. Type names are used in annotations and similar contexts; construction with `Type.new(...)` or `new^Type{…}` is not supported.

| Type | Constructor |
| --- | --- |
| [`std.math.complex.Complex`](./math-complex.md) | `std.math.complex.new(...)` |
| [`std.math.vector2.Vector2`](./math-vector2.md) | `std.math.vector2.new(...)` |
| [`std.math.vector3.Vector3`](./math-vector3.md) | `std.math.vector3.new(...)` |
| [`std.math.quaternion.Quaternion`](./math-quaternion.md) | `std.math.quaternion.new(...)` |
| [`std.math.vector4.Vector4`](./math-vector4.md) | `std.math.vector4.new(...)` |

Aliasing a module with `let^vec2 = import^std.math.vector2` allows `vec2.new(...)`. In contrast, `let^Vector2 = std.math.vector2.Vector2` aliases the type. Individual modules also offer other value factories such as `zero()`, `one()`, and `identity()`.

## Reading signatures

In signatures, `f^` and `p^` indicate functions and procedures, and `self^` marks a receiver. `A|B` is a union, such as success and error results; handle errors with `try^`, `catch^`, or narrowing. Multiple signatures for a name are overloads.

`ARG0` and `ARG1` are the types of the zeroth and first arguments. `.T0` extracts the first type argument, `.ResultType` the final coroutine result, and `.ReturnType` the subroutine result. `self^.T0` is the receiver's first type argument.

See the [overview](../index.md) for language-core rules and the [guide](../../index.md) for examples.
