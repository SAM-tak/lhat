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

## Reading signatures

In signatures, `f^` and `p^` indicate functions and procedures, and `self^` marks a receiver. `A|B` is a union, such as success and error results; handle errors with `try^`, `catch^`, or narrowing. Multiple signatures for a name are overloads.

`ARG0` and `ARG1` are the types of the zeroth and first arguments. `.T0` extracts the first type argument, `.ResultType` the final coroutine result, and `.ReturnType` the subroutine result. `self^.T0` is the receiver's first type argument.

See the [overview](../index.md) for language-core rules and the [guide](../../index.md) for examples.
