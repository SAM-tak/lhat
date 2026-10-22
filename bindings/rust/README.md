# L^ for Rust

The `lhat` crate embeds L^ through safe Rust functions and structs. Application
code needs no C calls, raw pointers, or `unsafe` blocks. There are no Cargo
dependencies; `build.rs` uses CMake to compile the bundled C11 runtime and a
private C bridge.

Use a path dependency in a checkout of this repository:

```toml
[dependencies]
lhat = { path = "../lhat/bindings/rust" }
```

```rust
use lhat::{Program, Scalar};

fn main() -> lhat::Result<()> {
    let mut program = Program::new(true)?;
    program.register_function("host", "double", "f^number^ -> number^;", |call| {
        match call.argument(0) {
            Some(Scalar::Integer(n)) => call.answer(n * 2),
            Some(Scalar::Real(n)) => call.answer(n * 2.0),
            _ => Err(lhat::Error::InvalidOperation),
        }
    })?;
    let machine = program.build()?;
    let result = machine.eval("main.lh", "import^ host\nreturn^ host.double(21)")?;
    assert_eq!(result.values[0].scalar(), Scalar::Integer(42));
    Ok(())
}
```

## API

- `Program` configures strict checking, module loading, the project root, Rust
  functions and primitive constants before `build()` creates a `Machine`.
  `Program::new` has no filesystem access. `with_filesystem` opts into the
  default loader; `with_loader` accepts a Rust closure returning source bytes.
- `Machine::load` checks and compiles source into a callable `Value`; `check`
  does the same for a loader-provided unit. `eval` compiles and calls source.
  `call`, `call_member` and `resume` accept rooted values as arguments.
- `RunResult::values` contains one value for an ordinary result and each
  position for a tuple. `suspended` is true when an execution budget expired;
  set the next budget and use `continue_run` until it becomes false. Other
  operations on a suspended machine return `Error::Suspended`.
- `Machine::value`, `table`, `table_get` and `table_set` exchange primitives
  and tables. `Value::scalar`, `kind`, `to_text`, `equals` and
  `coroutine_done` inspect values. `Scalar::Bytes` preserves arbitrary bytes,
  including NUL and invalid UTF-8. Names and paths must be NUL-free UTF-8;
  registered string constants must also be NUL-free.
- `Callback` reads owned primitive arguments and writes primitive or tuple
  answers. `answer_argument` forwards an existing argument, including a table
  or closure, without exposing its representation. Callbacks must follow their
  registered L^ signature. Returning `Err` or panicking produces a VM fault.
  A loader panic is treated as an unreadable unit.

## Ownership and execution

Every `Value` is a GC root. A clone shares its root; the final drop removes it.
Values remain usable across calls and garbage collections, and keep their
machine and program alive even after all `Machine` handles are dropped.
The VM is disposed before its program and Rust callback state. There is no
manual free/dispose API or publicly accessible FFI.

A machine rejects values from other machines before calling C. Programs,
machines and values are neither `Send` nor `Sync`. Independent machines can
be created on different Rust threads, but C API calls are serialized to protect
the runtime's process-wide registry. A callback cannot reenter an operation on
its running machine (`Error::Busy`). Capturing a machine strongly in its own
callback can form an ordinary `Rc` cycle; release that captured handle when
finished.

The wrapper currently covers source loading, compiled units, functions,
primitive constants, tables, tuple results, coroutines, budgets and GC. Custom
hostdata/inline value types, debugger hooks, hot reload, bytecode serialization,
the lexer and the optional sample standard library are not exposed yet.

## Build and validation

Install Rust, CMake 3.25 or newer and a C11 compiler. Native Windows/MSVC,
Linux/GCC and macOS/Clang builds are configured in CI. The C runtime uses the
release configuration even for Rust debug builds. Cross compilation and MSVC
static CRT builds are currently rejected. This crate is used from the checkout
and is not configured for publishing to crates.io.

```sh
cargo test --manifest-path bindings/rust/Cargo.toml
cargo clippy --manifest-path bindings/rust/Cargo.toml --all-targets -- -D warnings
cargo run --manifest-path bindings/rust/Cargo.toml --example embed
```

`CMAKE_GENERATOR` selects the CMake generator, and `CMAKE_C_COMPILER` (or `CC`)
selects the compiler. On Windows, the default Visual Studio generator discovers
the installed toolchain. If selecting Ninja, first load the MSVC environment
using `scripts/devshell.ps1`.
