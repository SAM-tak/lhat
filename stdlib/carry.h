// L^ (lhat) -- sample standard library: values carried between machines.
//
// Two machines share nothing on the heap (05 の 8.7: a machine is the
// caller's, and std.thread gives every thread one of its own), so what
// crosses between them has to be taken apart into a form that belongs to
// neither and put back together on the far side. That form is an
// LhatCarried: a graph of nodes -- primitives, strings, tables, closures and
// the cells closures capture -- with every object visited once, so a cycle
// or a shared sub-table comes back as a cycle or a shared sub-table.
//
// What crosses, and what comes back:
//
//   nil^ bool^ number^ string^   themselves
//   a table                      a deep copy, both halves, cycles kept
//   a closure                    the same proto (shared, unwritten data --
//                                vm.h on lhat_machine_make_closure) with
//                                every captured place copied; two closures
//                                that captured one place share one cell
//                                again on the far side. The copy is a
//                                snapshot: a write over there does not
//                                reach back here, which is what a machine
//                                of its own means (Erlang's funs cross the
//                                same way)
//
// What does not, with the reason lhat_carry answers:
//
//   a started coroutine          a frame belongs to the machine that runs
//                                it. One whose body has NOT started
//                                crosses (05 の 8.8改3): 02 の 15.5 runs
//                                nothing at the call, so it is the closure
//                                and the arguments laid out, and the copy
//                                is a second coroutine both machines may
//                                start. A table's walk and a walk the host
//                                wrote stay where they are
//   a def^ or its instance       the definition's identity is the other
//                                machine's to find again -- later
//   a host value                 a pointer's ownership (dispose^) is the
//                                host's contract, not settled here --
//                                and hostdata whose type declared that
//                                contract (05 の 8.8改2,
//                                lhat_register_hostdata_shared) crosses as
//                                its pointer, held once for the tree and
//                                once per wrapper rebuilt; the rest stays
//   a host subroutine            installed per machine, not carried
//   a loaded script's closure    its body belongs to the machine that
//                                loaded it (05 の 5.6)
//   an error value, a module     later, if wanted
//
// std.thread carries spawn's arguments and join's answer this way; a host's
// own channel between machines is the same two calls.

#ifndef LHATSTDLIB_CARRY_H
#define LHATSTDLIB_CARRY_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LhatCarried LhatCarried;

// Takes `value` apart. Answers false with *refused aimed at a static reason
// when something in the graph cannot cross, or out of memory (then
// *refused is "out of memory"). The carried form holds copies of the
// strings and borrows the protos; it belongs to no machine and may be
// handed to any thread.
bool lhat_carry(LhatValue value, LhatCarried **out, const char **refused);

// Puts it back together on `machine`. No L^ code runs while it does, so
// nothing here needs rooting beyond *out. A carried form may be uncarried
// more than once, on more than one machine.
bool lhat_uncarry(LhatMachine *machine, const LhatCarried *carried,
                  LhatValue *out);

void lhat_carried_free(LhatCarried *carried);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_CARRY_H
