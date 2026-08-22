// L^ (lhat) -- sample standard library: std.thread.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "thread" module.
//
// Needs an OS thread implementation: port/thread.h, the same one lsp/ uses in
// this codebase. A host linking this must also link lhatthread.
//
// spawn is written
//
//     let^ h = std.thread.spawn(p^ ... { ... }, 1, "two", true)
//
// -- fn takes '...' and nothing else and yields nothing; everything past it
// is handed to fn on the new machine. What crosses is what carry.h carries:
// the primitives, tables (a deep copy, cycles kept), and closures as their
// shared proto plus a snapshot of what they closed over -- fn itself goes
// that way, so it may close over things, and sees its own copies of them
// over there. Coroutines, def^ instances and a host's values do not cross;
// spawn answers BadArgument (or NotSpawnable, for fn) with carry's reason.
// fn's parameter list is '...' rather than one written out because the
// collector's any^ is the one shape every carried value conforms to:
// stdlib/thread.c's comment on the boundary says the rest.
//
// The machine a spawn starts is given the registering program's own
// registrations (lhat_program_install), so a spawned body reaches print, this
// module and everything else the host registered -- an LhatHost is an object
// on one machine's heap, so each machine needs its own. **What a registration
// was handed as its `context` is shared between them**: this module's own is
// read-only once registered, and a host whose context is not has to guard it
// itself.
//
// A ThreadHandle's dispose() blocks until the spawned thread finishes if it
// was never join()ed -- see stdlib/thread.c's thread_dispose comment for why.
//
// sleep is written
//
//     std.thread.sleep(0.2)
//
// -- **seconds**, and a real one is as good as a whole one. It stops the
// thread that called it, so a unit that never spawns anything can reach for
// it too; the module is where it lives because a thread is what it stops.
// Nothing to wait for (zero, a negative, a NaN) returns at once rather than
// answering an error, and there is no error to read: it is a p^.
//
// spawn/join answer a ThreadHandle, a ThreadError, or std.error.
// OutOfMemory for the one failure this module shares with every other
// stdlib module -- read the result with try^/catch^, or narrow with isa^
// against std.thread.ThreadHandle or an error kind (both supported).
// Naming std.error.OutOfMemory in an isa^ needs its own
// `import^ std.error` -- 8.1's "the language hands out no names" applies
// to a name reached through another module's registration too.

#ifndef LHATSTDLIB_THREAD_H
#define LHATSTDLIB_THREAD_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_thread_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_THREAD_H
