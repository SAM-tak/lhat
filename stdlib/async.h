// L^ (lhat) -- sample standard library: std.async.
//
// The two things a scheduler cannot write for itself: a deadline, and a way
// to be idle until one arrives. Everything else -- the task table, spawning,
// gathering, structured concurrency -- is written in L^ over these, the way
// 02 の 15.14 says it should be: the language knows what await^ means and
// nothing about who resumes.
//
//     std.async.timer(0.2)      # arms one, answers its id
//     std.async.wait(1.0)       # sleeps until an id is ready, or that long
//
// `wait` is the one place a scheduler blocks. It answers the id of the
// deadline that came due, or nil^ when the time it was given ran out first
// -- which is how a loop watching something else as well (a std.thread
// handle, a host's own queue) keeps its hand on how often it looks.
//
// 02 の 15.14: what is awaited is a coroutine, so the L^ side wraps an id in
// one. This module deliberately answers plain numbers: a coroutine made here
// would be a scheduler written in C, which is the half that belongs in L^.
//
// Needs port/thread.h for the clock and the sleep, the same as std.thread --
// a host linking this must also link lhatthread.

#ifndef LHATSTDLIB_ASYNC_H
#define LHATSTDLIB_ASYNC_H

#include "lhat.h"

bool lhatstdlib_async_register(LhatProgram *program);

#endif  // LHATSTDLIB_ASYNC_H
