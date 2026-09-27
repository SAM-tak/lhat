// L^ (lhat) -- sample standard library: std.channel.
//
// One function a host calls once, before lhat_program_check, the same as any
// of program.h's own registrations (05 の 8.7). Needs an OS thread
// implementation: port/thread.h. A host linking this must also link
// lhatthread.
//
// A channel is a queue between machines -- what std.thread's spawn and join
// are not: those hand a value over once each way, and a worker that wants
// to be given work, or to answer more than once, needs somewhere to put it.
//
//     let^jobs = std.channel.named("jobs")     # the same channel in every
//     let^done = std.channel.named("results")  # machine of this process
//
//     # in the worker
//     repeat^{
//         let^job = jobs.demand()              # waits until there is one
//         if^job fits^ nil^{ break^}
//         done.push(work(job))
//     }
//
// What crosses is what carry.h carries: the primitives (an integer stays an
// integer), tables with their cycles, closures as their shared proto plus a
// snapshot of what they closed over, and -- 05 の 8.8改2 -- hostdata whose
// type declared it may. Anything else answers ChannelError.Refused with
// carry's own reason. A channel is itself such a type, so a channel may be
// pushed into a channel, or handed to std.thread.spawn as an argument.
//
// The two ways to reach one:
//
//   std.channel.new()          a channel of its own, reached only where it
//                              is handed
//   std.channel.named(name)    one the whole process shares under that
//                              name, made by the first ask
//   std.channel.new(T)         a typed channel, where T is a type value
//   std.channel.named(name,T) a shared typed channel; an existing name must
//                              already have exactly that element type
//
// For example new(number^) has static type Channel<number^> after handling
// its errors. Type values carry type^<T>; ARGn.T0 projects their represented
// type, and member signatures use self^.T0 for the receiver's element type.
// push/supply check T statically, and the shared channel also checks writes
// at runtime, including writes through an erased Channel or named(name).
// A conflicting type, or a typed request for an existing untyped name,
// answers ChannelError.Refused. An untyped request may access a typed name.
// The channel owns a copy of the descriptor graph. Declaration identities
// within it have the same lifetime requirements as queued values below.
// Element types must fit one transported value; tuple/direct-hostvalue
// descriptors are refused. Carry's existing restrictions still apply.
//
// And what it answers:
//
//   push(v) -> number^         puts it in, answers its number (1, 2, 3 ...)
//   supply(v) -> bool^         puts it in and waits until someone takes it
//   supply(v, seconds)         the same, giving up after that long
//   pop() -> any^              takes the first, or nil^ where there is none
//   demand() -> any^           waits until there is one
//   demand(seconds) -> any^    the same, nil^ when the wait runs out
//   peek() -> any^             what pop would take, left where it is
//   count() -> number^         how many are waiting
//   hasRead(id) -> bool^       whether that push has been taken
//   clear()                    empties it, and every supply waiting on it
//   atomic(fn)                 calls fn(self) with the channel held, so a
//                              read and the write that follows it are one
//   dispose()                  gives this hold back (12.5)
//
// For Channel<T>, pop/peek/demand return T|nil^, including demand() without
// a timeout: inside atomic it does not wait. take() preserves T|nil^ as its
// coroutine result (rebuilding a carried value may fail and return nil^).
//
// A nil^ pushed and an empty channel read the same through pop (04 の 11.3
// gives every read a nil^ arm, and a queue is a read); demand tells them
// apart by waiting, and count() answers what is there.
//
// INSIDE atomic THE WAITS DO NOT WAIT. supply and demand answer as push and
// pop do -- the lock is already held by the call that is running, and a wait
// under it could only be woken by a machine that cannot get in.
//
// A named channel outlives every program, so a closure left in one after its
// program is gone would be holding a body that has been freed (05 の 5.6 the
// other way round). lhatstdlib_channel_forget_named is what a host calls
// before it disposes of the program that filled them -- a restart, a
// teardown -- with nothing of that program still running.

#ifndef LHATSTDLIB_CHANNEL_H
#define LHATSTDLIB_CHANNEL_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_channel_register(LhatProgram *program);

// Empties and forgets every named channel (std.channel.named). For a host
// tearing a program down while the process lives on: call it with nothing
// of that program running, since what is in a channel may be a closure of
// its body. A channel a machine still holds keeps working; it is only the
// name that is given up, so the next `named` of it makes a new one.
void lhatstdlib_channel_forget_named(void);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_CHANNEL_H
