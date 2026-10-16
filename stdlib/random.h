// L^ (lhat) -- sample standard library: std.random.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Nothing here is required --
// a host that never calls this never sees a "random" module, the same way a
// host that leaves port/ out never sees the default allocator.
//
// Random.save() returns the current xorshift64* state as a signed 64-bit
// integer without advancing it. Random.load(state) restores that state and
// returns nil, or Error.InvalidState for zero, fractional, or out-of-range
// values, leaving the state unchanged. Negative states are valid. Persist
// states as integers: conversion through floating point can lose bits.
// Saved states depend on the generator algorithm used by this version.
//
// Random.rangeint(lo, hi) samples integers in [lo, hi], using rejection to
// avoid modulo bias. Bounds must be ordered, representable 64-bit integers.
// Random.uniform(lo, hi) samples reals in [lo, hi), including after rounding.
// Bounds must be finite and ordered. Both return Error.InvalidArgument for
// invalid bounds without advancing, and return lo without advancing if equal.
//
// Random.normal(mean, standard_deviation) returns a normally distributed
// number, or Error.InvalidArgument for nonfinite arguments or a negative
// standard deviation. Zero deviation returns mean without consuming a draw;
// invalid arguments also leave the stream unchanged. Box-Muller discards its
// second sample, so save/load reproduce normal draws without extra state.
// Extreme finite arguments can overflow the result. Math-library rounding
// can differ between platforms, so bitwise cross-platform replay is not promised.
//
// std.random.new answers std.random.Random|std.error.OutOfMemory -- read
// it with try^/catch^, or narrow with fits^ against std.random.Random or
// std.error.OutOfMemory (05 の 8.8's hostdata path and 04 の 2.4's error
// kind path, both supported). Naming std.error.OutOfMemory in an fits^
// (rather than just letting try^/catch^ read it) needs its own
// `import^ std.error` -- 8.1's "the language hands out no names" applies
// to a name reached through another module's registration too.

#ifndef LHATSTDLIB_RANDOM_H
#define LHATSTDLIB_RANDOM_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_random_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_RANDOM_H
