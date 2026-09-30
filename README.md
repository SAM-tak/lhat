# The Programming Language L^

![L^ Logo](media/lhat-logo.svg)

**Read this in [日本語](README.ja.md).**

`L^` (elhat) — a bytecode-interpreted glue language with a static type
checker, written in C11 and built with CMake.

It is Lua's runtime model — tagged values, one data structure, coroutines — with
a type checker between the parser and the code generator. Strict checking is the
default for a file, and a mistake is a diagnostic rather than a fault at run
time:

```text
$ lhat --check todo.lh
todo.lh:2:1: error: this name was bound by a let^ and is not reassigned; write var^ where the name has to change: done
todo.lh:2:9: error: this value does not fit where it is written
```

It is meant to be embedded. The language is a library reached through one
header, it allocates only through a handful of functions a host can replace,
and it does not touch a file system unless a host hands it a way to.

- **Version** 0.3.10 — pre-1.0, and no backward compatibility is promised
- **License** Apache 2.0

## What it is for

Scripting a host you did not write: a game engine, a build tool, a data
pipeline, an editor. The host decides what a program can see, the program is
checked before it runs, and the whole thing links as a library with one header.

## The rule the design follows

Every decision in the specification comes back to one sentence: **report a
mistake where it was written, and never keep two implementations of the same
language rule.**

`=` is a comparison, so nothing that *looks* like an equation is one. A
call's `(` must sit on the callee's line, so a misread is impossible rather
than merely unlikely. Reassignment is prefix, so a statement can never begin
with `-`. An `override^` that is not substitutable is reported at the `..` where
the promise was made. The language server asks the checker instead of
re-deriving scopes, because two implementations disagree precisely where the
rules are hard.

The corollary is that a lot of familiar machinery is simply absent. What
follows is what that buys.

### There are no reserved words

A keyword is always `word^`, and `^` is used for nothing else. `if` is an
ordinary identifier:

```lhat
var^ if = 1
print(if)          # 1
```

So the lexer has no keyword table at all — it returns one token kind for every
hat identifier and leaves the decision to the parser. Adding a keyword never
breaks an existing identifier, which is the whole point of the `^` convention.

### `=` compares, `:=` reassigns, and there is no `==`

```lhat
var^ n = 0
n := n + 1
print(n = 1)       # true
```

`i = i + 1` looks like an equation and means something else, so it is not
written that way. The two ways to change a name are `:=` (this one) and
`let^`/`var^` (a new one), and the checker says which:

```text
error: this name was bound by a let^ and is not reassigned; write var^ where the name has to change: n
```

### Newlines mean nothing

Not even automatic semicolon insertion. The lexer emits no newline token; every
token carries a "there was a line break just before me" flag, and exactly one
rule reads it: a call's or index's `(` must be on the same line as the callee.

That is what stops this from parsing as a call:

```lhat
let^ f = twice
(21)               # error: an expression on its own is not a statement
```

In Lua this reads as `f(21)`, and the failure it eventually produces points
somewhere else entirely. Here the misread cannot happen at all, and a
practical consequence follows: pasting into the prompt behaves exactly like a
file.

### Errors are types

There is no exception mechanism. A fallible operation returns a union of the
value and the ways it can fail, and the union *is* the type:

```lhat
errordef^ ParseError {
    Syntax { line : number^, column : number^ },
    Eof,
}

let^parse : f^string^ -> number^|ParseError; = f^text:string^ {
    if^ text = "" { return^error^ParseError.Eof{} }
    return^error^ParseError.Syntax{ line := 1, column := 1 }
}
```

Three ways to handle it — substitute, hand back, or catch a block — and a
fourth, which is receiving the union and narrowing it:

```lhat
let^r = parse("hi")
if^ r fits^ ParseError.Syntax {
    print($"syntax error at {r.line}:{r.column}")   # r.line is number^ here
el^:
    if^ r fits^ ParseError.Eof {
        print("end of input")
    }
}
```

Once every kind is accounted for, what is left is the success type. That is
exhaustiveness, and it needs no dedicated mechanism: `when^` lowers to the same
`if^` chain, and the same narrowing does the rest.

Error *kinds* are declared as types, which is what makes a Zig-style error set
need no syntax of its own — `|` was already there. It is the one place L^
reaches for nominal identity, because "`NotFound` from the standard library" and
"`NotFound` from your own code" must not be the same kind, and no amount of
shape can tell them apart.

A failure cannot be dropped, and the diagnostic says what to write instead:

```text
$ lhat --check risky.lh
risky.lh:3:1: error: this can fail, and dropping the answer drops the failure with it; write try^ to hand it back, catch^ to answer instead, or a name to bind it and narrow
```

### `f^` is pure, `p^` is a procedure

The two are different kinds, and the difference is checked:

| | may call | may assign to | `yield^` |
| --- | --- | --- | --- |
| `f^` function | functions only | its own locals and the tables it made | yes |
| `p^` procedure | both | anything | yes |

```lhat
let^pure = f^t:t^{ x:number^ } -> number^ {
    let^u = { x := 0 }
    u.x := 1        # fine: this body made u
    return^u.x
}
```

`t.x := 3` in the same body is an error, as is calling a `p^` from an `f^` —
which includes a user-defined operator, so an operator has no side effect to
have.

The second half is `let^`. An introducer always carries a value, so there is
no "declare now, assign once later" form, and the definite-initialization
analysis that Swift and Java need does not exist here: checking `let^` is
looking for a `:=` on that name.

### Types are structural, including across modules

Names are labels for diagnostics. Identity is shape:

```lhat
let^needs_writer = t^{ write : p^self^, string^; }
let^use = p^s:needs_writer { s.write("ok") }
```

A nominal type could not say that. Two `Point`s of the same shape in different
units *are* the same type — a module boundary is not a nominal boundary. There
are exactly three nominal islands: error kinds, `enum^`, and host-registered
types, the last because an opaque host type has no structure to compare.

### `def^` is an expression, and `..` is two things

`..` is the general concatenation operator, applied to whichever kind is on the
left:

```lhat
"abc" .. "def"      # strings
{1, 2} .. {3, 4}    # tables
Base .. def^{ ... } # definitions
```

So composition reads the way it does with strings, and there is no `class^`:
`def^` is the only user-defined-type mechanism, and it covers a concrete type,
an abstract one, a protocol, an object template and an aspect at once.

```lhat
let^Shape = def^{ self^{ label = "shape" }, area = f^self^ -> number^ { return^ 0 } }
let^Square = Shape .. def^{
    self^{ side = 2 },
    override^area = f^self^ { return^self^.side * self^.side }
}
let^s = Square.new()
print($"{s.label} area = {s.area()}")   # shape area = 4
```

An `abstract^` member is the interface role, and the composition is rejected at
the `..` if the result would not be substitutable — the error lands where the
promise was made, not where it is used.

`delegate^` borrows members instead of forwarding them one by one, which is what
makes binding a few hundred engine classes tractable: the delegated members
join the type, and no wrapper procedure is generated.

### Operators are members, and all of them are pure

A member literally named `..` or `+`, and where `self^` sits in the parameter
list decides which side receives the call — so there is no `__radd__`:

```lhat
let^Vec = def^{
    self^{ x = 0, y = 0 },
    override^new = f^x:number^, y:number^ { self^{ x = x, y = y } },
    op^* = f^self^, k:number^ -> Self^ { def^.new(self^.x * k, self^.y * k) },
    overload^op^* = f^k:number^, self^ -> Self^ { def^.new(k * self^.x, k * self^.y) },
    op^+ = f^self^, o:Self^ -> Self^ { def^.new(self^.x + o.x, self^.y + o.y) },
    tostring = f^self^ -> string^ { $"({self^.x}, {self^.y})" },
}
let^sum = Vec.new(1, 2) + Vec.new(10, 20) * 3
print($"sum = {sum}")   # sum = (31, 62)
print(3 * Vec.new(1, 2))
```

A type writes one comparison, `op^<=>`, and `<`, `>`, `≦`, `≧`, `=` and `≠` are
all read off it. A set, a complex number, a colour or a handle answers `op^=`
instead, because it can say what is equal without saying what comes first.

Overloads are resolved by *search*, not by ranking: under structural typing
"more specific" is not a defined relation, so at most one candidate fits a call
and overlapping signatures are rejected where they are written.

### Coroutines are inferred, not annotated

Writing `yield^` makes a procedure yieldable. There is no `async` marker to
propagate and no `Task`/`Future` type — a coroutine saves one frame, not a
stack, and calling one does not suspend the caller. `await^` is delegation, so
it reaches as deep as it needs to.

`yield^` is an expression: it hands a value out and takes one back on resume.

```lhat
let^count_to = f^n:number^{
    var^i = 0
    repeat^until^i >= n {
        var^step:number^|nil^ = yield^i
        i += step ?? 1
    }
}

let^co = count_to(10)
var^got = co.start()
repeat^until^co.done() {
    print(got)
    got := co.resume(2) ?? 0
}
```

And a call whose result is a coroutine is a **compile error** as a statement,
because the type already proves the statement would do nothing.

The scheduler is not in the language. `std.task` keeps N worker machines
standing over OS threads and hands each a job to run to completion in slices,
`std.channel` is an MPMC queue over them, and a cooperative budget is what lets
a machine that never wrote a `yield^` be interrupted anyway — see
[sample/async.lh](sample/async.lh), which is a whole task scheduler written in
L^, borrowing only a timer and a wait from the host.

### `nil^` has one family, and narrowing knows about ranges

`?.` guards a whole postfix chain, `?` asks whether a value is present, `??`
substitutes, and `?op=` applies an operator only if present:

```lhat
var^ t : t^{ string^,string^,string^ }|nil^ = nil^
let^a = t?[0] ?? "100"        # "100"
```

```lhat
var^ count : t^{ number^[] } = { 0, 0, 0 }
var^ i = 0
count[i] += 1     # error: this may be nil^, and nil^ answers no operator
count[i] ?+= 1    # fine
```

Narrowing follows `fits^`, a comparison with `nil^`, `?`, loop bounds and
orderings, and a guard that exits. A table type with a fixed position count
gives the bounds something to check against:

```lhat
let^bump = p^t:t^{ number^[9] }, d:number^ {
    if^ 1 <= d <= 9 { t[d - 1] += 1 }   # d is 1..9, so d - 1 is 0..8
}
```

One end alone is not enough, and what a branch knew does not outlive it — both
of these are errors:

```lhat
if^ 0 <= d { var^ n : number^ = t[d] }          # the upper end is still open
if^ 0 <= d <= 8 { } var^ n : number^ = t[d]     # and the branch is over
```

### Narrowing is not all of the inference

Types are written where the value is, and the annotation is a requirement the
body is checked against. `strict` and `relaxed` change *when* an undecided type
is reported and nothing else — the source text is identical in both, so there
are no two dialects, and code that passes `strict` behaves the same under
`relaxed`. The one-way guarantee is the useful direction: `relaxed` is a step
up to `strict`, not a way out of it. A file is checked `strict` by default; the
prompt is `relaxed`, so a half-typed line can be sent before it is finished.

### Nothing is ambient

There is no global scope. A name is visible only if a unit brought it in with
`require^` or the host registered it with `import^` — and `print` is no
exception: an unqualified `print("...")` works because the host bound it as an
initial name, not because the language owns it. `L^` is the machine's own
table, which a program can read and cannot write.

`require^` binds the one name the importer chooses, and a module says what it
publishes with `public^` on each declaration rather than with a `return` at the
end of the file — so the export set is known from parsing alone, without running
anything:

```lhat
module^ lib.greet

public^ let^hello = p^who:string^ { return^ $"hello, {who}" }
let^secret = 1                     # not visible to an importer
```

### The host writes host types in the language's own grammar

A registration is a type written out in L^ syntax, read by the checker before
anything runs:

```c
lhat_register_func(program, "std.io", "print", "p^string^;", print_fn, NULL);
lhat_register_global(program, "twice", "f^number^ -> number^;", host_twice, NULL);
lhat_bind_initial(program, "twice", "L^.twice");
```

Declaration and implementation cannot drift apart, because they are one thing.
Arguments arrive as an array whose count and types were settled at check time,
and an error comes back as a value, so there is no unwinding to arrange.

## What it leaves out

Each of these is a decision, not an omission, and the specification says why.

| Not this | Because |
| --- | --- |
| Exceptions | errors are values, so "what if one is thrown mid-cleanup" never arises |
| Truthiness | only `bool^` goes in a condition; narrowing is the mechanism instead |
| Metatables | a statically checked language cannot let the type system be rewritten at run time |
| `==`, `++`, `!=` | `=` compares, `:=` reassigns, and `=` plus `!` is one spelling per idea |
| ASI | exactly one rule about call parentheses, instead of a set of exceptions |
| Nominal identity | three named exceptions, and each has a reason |
| Ranked overloads | structural typing has no "more specific" for a ranking to use |
| Destructuring patterns | matching is the type's job; getting at contents is the name's |
| Generics on the type system | `template^` covers the parametric case; variadic generics are heavy |
| A process-wide locale | anything a program can read is byte-identical whatever language the host is set to |

## Samples

[sample/](sample/) holds seventeen programs. The ones worth reading first:

### Factorial

A `f^` calls itself through `this^` — the enclosing subroutine's own signature,
so the recursive call is checked like any other. One line:

![sample/factorial.lh — print(f^n:number^{if^n < 2: 1 el^: n * this^(n - 1);}(10))](media/readme-factorial.svg)

### Composition

[sample/composition.lh](sample/composition.lh) builds a cache over a logging
layer over storage, with `delegate^` passing the members each wrapper does not
care about, and then checks the whole thing — including that a cache hit does
*not* reach the logger — against a test double that satisfies `Store`
structurally without inheriting from anything. Both wrappers are written
against table types, so neither knows the concrete storage exists.

### Tasks

[sample/hanoi3.lh](sample/hanoi3.lh) is Towers of Hanoi with each leaf problem
a job: `std.task` starts six workers, every leaf is a coroutine, and the
`Task<number^>` values come back through a table. The first `depth` levels of
recursion stay on the calling machine — the split itself moves no task, so the
harness counts those moves itself.

### A scheduler, in the language

[sample/async.lh](sample/async.lh) is a full cooperative scheduler — task table,
wait table, `poll()` for a host's frame loop, `run()` for one that owns the
loop — written with `def^`, `yield^` and `await^`. The host lends it a timer and
a wait and nothing else.

### 24 Game

[sample/24.lh](sample/24.lh): [Rosetta Code's 24 game](http://rosettacode.org/wiki/24_game) —
four digits are dealt and the player writes an expression using each of them
once that comes to 24. The reader is a recursive-descent parser written as a
`def^`.

### Data as text — LTON

A `.lton` file is a table written as text, read by the same lexer as source and
evaluated in a context where only pure functions can be called, so the format
cannot have effects. Expressions work, which is the point:

```lton
# conf.lton
identity = "lhatove-suite",
window = { title = "test suite", width = 480 },
width = 480 * 2,               # 960, and you did not have to write it
name = "lhat" .. "ove",
```

```lhat
import^std.lton
let^conf = std.lton.load("conf.lton") catch^ panic^it^
let^text = try^std.lton.stringify(conf)
try^std.lton.save("conf-copy.lton", conf)
```

No check was written for LTON. The language already had the rule that makes it
safe, and that rule is the boundary.

## Language bindings

- [Godot](https://github.com/SAM-tak/lhat-gdextension)
- [LOVE 2D](https://github.com/SAM-tak/lhat-love)
- [Unreal Engine](https://github.com/SAM-tak/lhat-UE) (early experimental)

## Tooling

Two binaries ship from the same revision. `lhat` is the driver; `lhatls` is the
language server.

### `lhatls` — the language server

Hover, completion, go-to-definition, references, signature help, document
symbols, semantic tokens, quick fixes with machine-applicable/needs-review
confidence, and a syntax-tree view.

The organizing decision is that **the server never re-derives a type**. The
checker records what each name resolved to, and the server reads that table. A
second implementation of scoping would disagree with the checker precisely in
the hard cases, and would have to be kept in step forever. The corollary is a
stronger promise than "usually right": the candidates the server offers are the
candidates the checker accepts.

Turning `LHAT_WITH_RESOLUTIONS` off removes the recording and the server with
it; a host that embeds the language and no tooling pays for neither.

### Debugger

`lhat --dap=PORT` runs a program under a Debug Adapter Protocol session over a
loopback socket — breakpoints, stepping, frame and binding inspection,
expression evaluation in the frame's scope, tracebacks, and watchpoints on
machines. It sits on a line hook compiled into the VM
(`LHAT_WITH_DEBUGGER`), costs about 50 ns per loop iteration with the hook on,
and its "thread" is an L^ machine, not an OS thread.

The debugger is a C-side product on purpose. A `debug` library callable from
the script would be a hole in `f^`'s purity and in the static types; the
equivalent is this public C API.

### Editors

- [VS Code extension](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)
  — language client, graph view and debug client, all over `lhatls` and
  `lhat --dap`.

**Graph authoring.** The visual editor is a separate front-end over the same
language server, not a separate language: it reads the same syntax tree the
checker does, and the graph is a view of a program that is still text. Chapter
06 of the design documents moved to the extension repository, since a tool that
uses the implementation is not part of the implementation's specification.
[media/factorial-graph.svg](media/factorial-graph.svg) is
[sample/factorial.lh](sample/factorial.lh) as a graph.

Comments are kept and attached to the syntax tree for this reason — a graph
whose nodes cannot carry comments is far poorer than the text it came from.
A host that embeds the language and no tooling turns that off with
`LHAT_WITH_COMMENTS=OFF`.

## Requirements

- CMake 3.25 or later
- A C11 compiler
  - Windows: Visual Studio 2022 or later for the Ninja presets; the `vs` preset
    needs Visual Studio 2026
  - Linux / macOS: GCC or Clang
- [Ninja](https://ninja-build.org/) — recommended; the Visual Studio generator
  is also supported on Windows

## Build

### Windows — Ninja + MSVC (recommended)

Ninja invokes `cl.exe` directly and does not locate the toolchain on its own,
so the MSVC environment has to be loaded into the shell first.
`scripts/devshell.ps1` does that via `vcvars64.bat`:

```powershell
. .\scripts\devshell.ps1      # note the leading dot: it must be dot-sourced
cmake --preset debug
cmake --build --preset debug
.\build\debug\lhat.exe
```

Substitute `release` for `debug` to get an optimized build.

If you build from VS Code with the CMake Tools extension, the environment is
set up by the selected kit and `devshell.ps1` is not needed.

### Windows — Visual Studio generator

The Visual Studio generator finds the toolchain by itself, so no
`devshell.ps1` is required. Use this when you want to debug inside the Visual
Studio IDE.

```powershell
cmake --preset vs
cmake --build --preset vs-debug
.\build\vs\Debug\lhat.exe
```

### Linux / macOS

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat
```

## Tests

The suite is built by default and runs through CTest — 85 tests, in seven
groups:

```powershell
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/debug -L check      # core, check, vm, stdlib, lsp, dap, e2e
```

`core` is the language itself, `check` and `vm` the checker and the machine,
`stdlib` the sample standard library, `lsp` and `dap` the tooling, and `e2e`
whole programs — including `install_smoke`, which installs the tree, builds a
host against the installed headers with `find_package(lhat CONFIG)`, and fails
unless it answers 42. That one is disabled in a Debug tree, where the
install step is not worth the run; 84 of the 85 run there.

Pass `-DLHAT_BUILD_TESTS=OFF` at configure time to skip it.

CI (`.github/workflows/`) builds with MSVC, GCC and Clang and runs the suite on
each, plus a Clang build under ASan and UBSan.

## Running

With no file, the driver is a prompt. An expression on its own is answered,
and a construct that has not finished reads on:

```text
L^ (lhat) 0.3.8
an expression on its own is answered; an unfinished construct reads on
ctrl-d or an empty line ends
> 2 + 3
5
> let^greet = f^n:string^ { $"hi {n}" }
> greet("there")
"hi there"
> let^add = f^a:number^, b:number^ {
.     return^a + b
. }
> add(2, 3)
5
```

With a file, the default checks the whole program — the unit and everything it
requires — and runs it:

```powershell
.\build\debug\lhat.exe path\to\file.lh
```

| Option | What it does |
| --- | --- |
| *(no file)* | Read from a prompt |
| *(default)* | Check the program and run it |
| `--run` | Explicitly select program execution; what follows the file is the script's `...` |
| `--check` | Type check and report, without running |
| `--ast` | Print the syntax tree |
| `--tokens` | Print the token stream instead |
| `--dump-bytecode` | Print what the unit compiles to |
| `--command` | Read the input as the command form (`foo 1 2` calls) |
| `--strict` | Report a type error at compile time (default for a file) |
| `--relaxed` | Leave an undecided type to a run-time check (default for the prompt) |
| `--compile -o DIR` | Check and compile the whole program, writing every unit to `DIR` as bytes; a `.lton` compiles on its own |
| `--strip-debug` | Leave the local and captured names out of what `--compile` writes |
| `--dump-signatures FILE` | Write the signature table this driver's registrations make |
| `--signatures FILE` | Read a signature table before registering — what a build without the front end registers by |
| `--dump-host-api [file]` | Write what this driver registers as JSON, for `lhatls` — see the checked-in [sample/lhat-host.json](sample/lhat-host.json) |
| `--dump-messages DIR` | Write the English messages under `DIR`, one file per source, as the catalogs a translation is made from |
| `--messages DIR` | Read the catalogs from `DIR` rather than from beside the executable |
| `--language TAG` | Say things in that language rather than the one the system reads |
| `--dap=PORT` | Run under a debugger over DAP on that loopback port (implies `--run`) |
| `-h`, `--help` | Show the usage and do nothing else |
| `-v`, `--version` | Show the version and do nothing else |

`--ast`, `--tokens`, `--dump-bytecode` and `--command` all read a file; with no
file they print the usage.

### Localization

`messages/ja/` holds the Japanese catalogs. A message has a stable string ID
and holes, the English text is the reference, and every other language is a
translation of it — `--dump-messages` writes the English out for a translator.
The selection is per program, never process-wide.

The invariant worth stating: **anything a program can read is the same bytes
whichever language the host is set to.** That covers `tostring`'s answer, how a
number is written, `typeof^`'s spelling, and LTON's reading and writing. Two
programs that compare, store or send those cannot be made to behave differently
by who runs them.

## Embedding

The language is `lhat.lib`; `lhatport.lib` is only where memory comes from and
how a unit's text is read. A host puts `include/` on its path and names one
header:

```c
#include "lhat.h"
```

`src/` holds names like `parser.h` and `type.h` — too ordinary to put on
somebody else's include path, so **nothing** in `src/` is installed. A host
that colours L^ in an editor names `lhat/lexer.h` as well, which is the only
other public header; one that only runs bytecode never sees it.

A host checks, compiles, installs and runs one unit. This is
[tests/install_smoke/host.c](tests/install_smoke/host.c) in full, minus the
error handling:

```c
LhatProgram *program = lhat_program_new(/*strict=*/true, load_main, NULL);
lhat_register_global(program, "twice", "f^number^ -> number^;", host_twice, NULL);
lhat_bind_initial(program, "twice", "L^.twice");

const LhatUnit *root = lhat_program_check(program, "main.lh");

LhatMachine *machine = lhat_program_compile(program) ? lhat_machine_new() : NULL;
lhat_program_install(program, machine);
LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));

lhat_machine_dispose(machine);
lhat_program_free(program);
```

Registering comes before checking, because the checker has to know what a
signature says. Installing comes before running, because that is when what was
registered reaches `L^`.

Beyond this shape the API covers what a host actually needs: two dozen
registration calls (types, members, host data, host values, enums, error kinds,
annotations, constants, instantiation checks), coroutines and scheduling
(`lhat_machine_resume`, `lhat_machine_set_budget`, `lhat_machine_call`), the
debugger, unit and export introspection, binary units and signature tables, and
the allocator.

### Hot reload

An editor's save is one call:

```c
lhat_reload(program, "lib.lh", machines, machine_count);
```

It invalidates, forgets the unit on each machine, rechecks, recompiles, and
frees the retired bodies only once it has seen that no machine still holds a
closure of one. The steps are public for a host that wants the timing in its
own hands.

### Replacing the port

`lhatport` is only where memory comes from and how a unit's text is read.

A static host with its own copies `port/alloc.c` and `port/loader.c`, changes
the four functions, and leaves the library out of the link — the core resolves
`lhat_alloc` and friends against whatever is there, with no indirection and
nothing to register.

A shared build cannot use that seam, since a DLL is linked before the host
sees it, so the default also takes an allocator through `lhat_set_allocator`.
It must be called before anything has been allocated, and says so by answering
false if it was not.

The loader is never defaulted to: `lhat_program_new` takes one, and `NULL`
means no unit can be read — so nothing embedded reaches a file system unless it
was told to. See [include/lhat/port.h](include/lhat/port.h) and 05 §8.9.

## Presets

| Configure preset | Generator | Build presets | Binary directory |
| --- | --- | --- | --- |
| `debug` | Ninja, `Debug` | `debug` | `build/debug` |
| `release` | Ninja, `Release` | `release` | `build/release` |
| `asan` | Ninja, `Debug` + sanitizers | `asan` | `build/asan` |
| `pgo` | Ninja, `Release` + PGO instrumentation | `pgo` | `build/pgo` |
| `vs` | Visual Studio 2026 (multi-config) | `vs-debug`, `vs-release` | `build/vs` |
| `vmonly` | Ninja, `Release`, VM only | `vmonly` | `build/vmonly` |

`ctest` presets exist for `debug`, `release` and `asan`, with
`outputOnFailure` already set.

The Ninja presets are single-configuration: the build type is fixed at
configure time. The Visual Studio preset is multi-configuration, so the build
type is chosen by the build preset instead.

All presets set `CMAKE_EXPORT_COMPILE_COMMANDS`, but only the Ninja presets
actually emit `compile_commands.json` — the Visual Studio generator does not
support it. Point clangd at `build/debug/compile_commands.json` for editor
completion and diagnostics.

`vmonly` builds the core with no front end at all: no lexer, parser, checker or
compiler. It reads binary units written by `--compile` and registers a host by
a signature table, so a shipping runtime carries neither the source-language
machinery nor the text. On this tree with MSVC Release that takes `lhat.exe`
from 685 KB to 422 KB and `lhat.lib` from 2.1 MB to 969 KB.

`scripts/pgo.ps1` drives the two PGO phases — `GENERATE` instruments and a
training run in `bench/train/` writes the profile, `USE` relinks with it.

## Build options

| Option | Default | What it controls |
| --- | --- | --- |
| `LHAT_WITH_FRONTEND` | `ON` | The lexer, parser, checker and compiler. Off is `vmonly`; the LSP and the suite need it on |
| `LHAT_WITH_DEBUGGER` | `ON` | The VM line hook, frame inspection and machine watching. `LHAT_BUILD_DAP=ON` needs it on |
| `LHAT_WITH_COMMENTS` | `ON` | Keeping comments and attaching them to the syntax tree |
| `LHAT_WITH_RESOLUTIONS` | `ON` | Recording what each name resolved to. `LHAT_BUILD_LSP=ON` needs it on |
| `LHAT_BUILD_STDLIB` | `ON` | The sample standard library in `stdlib/` |
| `LHAT_BUILD_LSP` | `ON` | The language server |
| `LHAT_BUILD_DAP` | `ON` | The debug adapter folded into the driver |
| `LHAT_BUILD_CLI` | `ON` | The command line driver |
| `LHAT_BUILD_TESTS` | `ON` | The test suite |
| `LHAT_BUILD_BENCH` | `OFF` | The member-read and checker-cost benchmarks |
| `LHAT_SANITIZE` | `OFF` | AddressSanitizer, plus UBSan where the compiler has it |
| `LHAT_PGO` | `OFF` | `OFF`, `GENERATE` or `USE` |

## Layout

```text
CMakeLists.txt        Build definition
CMakePresets.json     Configure / build / test presets

include/lhat.h        The one header a host names
include/lhat/         The rest of the public surface, and the generated version.h

src/                  The language                        -> lhat.lib
  source.c              Reading a unit; newline and BOM normalisation
  error.c               One shape for what every stage reports
  message.[ch]          Message IDs, holes and rendered text
  number.[ch]           Integers and reals: one type, two representations
  token.c               Token definitions
  lexer.c               Lexical analyser
  ast.[ch]              Syntax tree nodes and their arena
  parser.[ch]           Parser
  type.[ch]             Types: construction and conformance
  check*.[ch]           Type checker: expressions, statements, initialization
  semantic.c            Names, types and members of a unit
  completion.c          What may follow the cursor
  fix.c                 Quick fixes, with what each one applies to
  program.c             The unit graph, and what a host registers
  registry.[ch]         Host registrations
  rttype.[ch]           Runtime type descriptions
  code.[ch]             Bytecode, chunks and compiled units
  compile.[ch]          Syntax tree to bytecode
  serialize.[ch]        Binary units and the signature table
  vm*.c                 Code generation and the machine
  machine.h             The inside of a machine: stack, frames, heap
  gc.[ch]               The collector: mark and sweep, a step at a time
  value.c               Runtime values
  object.c              Heap values
  debug.c               The line hook and frame inspection
  port.h                What the language asks of its surroundings

port/                 Default memory, file, thread and socket access
  alloc.c               malloc, and the registration a DLL needs
  loader.c              Reading a unit from a file
  thread.[ch]           OS threads, for what stands beside the core
  socket.[ch]           Loopback sockets, for the debug adapter
  -> lhatport.lib, lhatthread.lib, lhatsocket.lib

stdlib/               Sample standard library, in C       -> lhatstdlib.lib
  io, json, thread, random, regex, math (+ complex, quaternion, vector2/3/4),
  debug, async, channel, task, lton, load, error, carry

cli/main.c            Command line driver and prompt      -> lhat.exe
lsp/                  Language server                     -> lhatls.exe
dap/                  Debug adapter, DAP over a socket    -> lhatdap.lib
transport/            Content-Length framing over a stream
vendor/cjson/         JSON, for the two above
messages/ja/          Message catalogs
bench/                Member-read and checker-cost benchmarks, and PGO training
tests/                Test suite (CTest), including install_smoke
sample/               17 sample programs
DesignDocuments/      Language design specifications (Japanese)
media/                Logo and sample renderings
scripts/              devshell.ps1, pgo.ps1, install_smoke.cmake
cmake/                The CMake package config template
Memo.md               Language design notes (brainstorming, not a spec)
```

`source.[ch]`, `error.[ch]`, `token.[ch]`, `lexer.[ch]`, `value.[ch]`,
`object.[ch]` and `port.h` are listed by their `.c`; their headers are public
and live under `include/lhat/`, because a host that colours L^ needs the lexer
and one that reads a unit needs the source rules.

The pipeline runs left to right: `source` → `lexer` → `parser` → `check` →
`compile` → `vm`, with `program` walking the unit graph so that a unit is
checked after everything it requires. The syntax tree is not optional: type
inference needs information from behind the source, so a single pass that
emits bytecode as it reads cannot work.

`vm_internal.h` keeps `LhatMachine` opaque, so the files that are the machine
— `vm*.c`, which runs it, and `gc.c`, which has to see the roots — share
`machine.h` between them.

## Design documents

The specifications are the authoritative description of the language; the
source cites them by section number throughout. They are written in Japanese,
and [their index](DesignDocuments/README.md) lists what is still undecided.

| Document | Covers |
| --- | --- |
| [01-lexical-structure.md](DesignDocuments/01-lexical-structure.md) | Characters, tokens, literals, comments, scope specifiers |
| [02-syntax.md](DesignDocuments/02-syntax.md) | Statements, operators, types, the object model, subroutines, coroutines, pattern matching |
| [03-compilation-pipeline.md](DesignDocuments/03-compilation-pipeline.md) | The four stages, strictness, inference, value representation, bytecode, the collector |
| [04-errors.md](DesignDocuments/04-errors.md) | `errordef^`, `try^`, `catch^`, exhaustiveness, dropped failures |
| [05-modules.md](DesignDocuments/05-modules.md) | Units, `require^`, `import^`, `L^`, and what a host provides |
| [07-language-server.md](DesignDocuments/07-language-server.md) | `lhatls`: what it answers, and from what |
| [08-lton.md](DesignDocuments/08-lton.md) | LTON, the table-as-text format, and why it is safe to read |
| [09-debugger.md](DesignDocuments/09-debugger.md) | The line hook, frame and binding inspection, the DAP adapter |
| [10-localization.md](DesignDocuments/10-localization.md) | The observable-text invariant, message IDs, catalogs, language selection |

06 is a skipped number: the visual editor's design moved to the extension
repository, next to the tool that implements it.

## License

Apache License 2.0. See [LICENSE](LICENSE).
