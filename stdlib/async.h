// L^ (lhat) -- sample standard library: std.async.
//
// The two things a scheduler cannot write for itself: a wait, and a way to
// know what is ready. Everything else -- the task table, whose turn it is,
// when to sleep -- is written in L^ over these, the way 02 の 15.14 says it
// should be: the language knows what await^ means and nothing about who
// resumes.
//
//     let^ id = std.async.timer(0.2)     # ready when the clock says so
//     let^ id = std.async.external()     # ready when somebody says so
//     std.async.wait(0)                  # take what is ready, do not sleep
//     std.async.next()                   # seconds until the earliest deadline
//
// == What a host is promised ==
//
// - **`wait(0)` does not sleep.** A host with its own loop calls the L^
//   scheduler's poll every frame and never gives up its thread. `wait(s)`
//   with a positive s is the convenience for a program that has nothing else
//   to do -- it sleeps in short naps, so a completion pushed from another
//   thread is noticed within about 20ms
// - **A completion may be pushed from any thread.** lhatstdlib_async_complete
//   takes the id std.async.external answered. The table is locked; the id is
//   the whole of what crosses
// - **Deadlines are measured on a monotonic clock** (port/thread.h's
//   lhat_now_ms), so winding the wall clock moves nothing
// - **One scheduler per machine.** An id means nothing to another machine --
//   std.thread's spawn makes one, and the two share no table
//
// == What an embedder does ==
//
// Godot, a game loop, a server: the shape is the same.
//
//     void *waits = lhatstdlib_async_waits(program);   // once, after register
//     // every frame: run the L^ scheduler's poll(), which calls wait(0)
//     // when something the host owns finishes:
//     lhatstdlib_async_complete(waits, id);            // any thread
//
// The id the host holds came from a task that wrote `std.async.external()`
// and handed it over -- through a registration of the host's own, or through
// a table the host reads. Nothing here decides how the two meet.
//
// Needs port/thread.h for the clock, the sleep and the lock, the same as
// std.thread -- a host linking this must also link lhatthread.

#ifndef LHATSTDLIB_ASYNC_H
#define LHATSTDLIB_ASYNC_H

#include "lhat.h"

#include <stdint.h>

bool lhatstdlib_async_register(LhatProgram *program);

// The table this module registered, or NULL if it never was. Held by an
// embedder for as long as it may push completions -- it lives as long as the
// program does.
void *lhatstdlib_async_waits(LhatProgram *program);

// Says an external wait is ready. Any thread may call it. False when the id
// names no wait that is still outstanding -- one already taken, one dropped,
// or one that was never handed out.
bool lhatstdlib_async_complete(void *waits, int64_t id);

#endif  // LHATSTDLIB_ASYNC_H
