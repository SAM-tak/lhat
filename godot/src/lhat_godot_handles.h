// L^ (lhat) -- a Callable and a Signal, as 05 の 8.8's host data.
//
// Both look like values in Godot and are not: what a Callable holds is a
// reference to whatever it was bound to, and a Signal holds the object it
// belongs to. 8.9's host values are raw bytes the collector never looks
// into, which is why these cannot be ones -- so they are held behind a
// pointer, with a dispose^ that gives the memory back (02 の 12.5).
//
// Together they are what lets a script connect one thing to another from L^
// rather than through the engine's own dialog: a Callable names a method on
// an object, a Signal names one it emits, and connect puts the two together.

#ifndef LHAT_GODOT_HANDLES_H
#define LHAT_GODOT_HANDLES_H

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/signal.hpp>

#include "lhat.h"

namespace godot {
namespace host {

struct Godot;

// Registers godot.Callable and godot.Signal, their members and the module
// functions that make one. Before any checking (05 の 8.7).
bool register_handles(LhatProgram *program, Godot *module);

// A value of godot.Callable or godot.Signal standing for this one. False
// only when the machine ran out of memory.
bool make_callable(LhatMachine *machine, const Godot *module,
                   const Callable &callable, LhatValue *out);
bool make_signal(LhatMachine *machine, const Godot *module,
                 const Signal &signal, LhatValue *out);

// The other way, or NULL when the value is not one of ours.
const Callable *callable_of(LhatValue value, const Godot *module);
const Signal *signal_of(LhatValue value, const Godot *module);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_HANDLES_H
