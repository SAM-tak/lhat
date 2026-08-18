// L^ (lhat) -- Godot's mathematical types, as 05 の 8.9's host values.
//
// A Vector2 is a pair of numbers and nothing else: no pointer, no reference,
// nothing the collector has to look into. 8.9 is the shape for exactly that
// -- the bytes ride the machine's stack, `v.x` reads and writes them with no
// host call, and arithmetic allocates nothing.
//
// What 8.9 charges for it is that such a value cannot go where it would
// outlive the frame: a table's element, a capture, an any^, a union, '...'
// or a def^ member. What goes there is the box the language gives every
// value type -- `godot.Vector2.Box^`, made with box^, read with get() and
// written with set().
//
// The engine's objects are not here and are not like this. Those are one
// nominal hostdata type (lhat_godot_module.h) with the class tree written in
// L^, because 971 classes in an eight deep tree have no nominal shape. A
// value type has no tree: each is its own and answers to nothing else.

#ifndef LHAT_GODOT_VALUES_H
#define LHAT_GODOT_VALUES_H

#include "lhat.h"

namespace godot {
namespace host {

struct Godot;

// Registers every value type into `program` -- the type, the fields read
// straight out of its bytes, the operators it answers and the module
// function that makes one. Belongs before any checking, with every other
// registration (05 の 8.7).
bool register_values(LhatProgram *program, Godot *module);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_VALUES_H
