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

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include "lhat.h"

namespace godot {
namespace host {

struct Godot;

// Registers every value type into `program` -- the type, the fields read
// straight out of its bytes, the operators it answers and the module
// function that makes one. Belongs before any checking, with every other
// registration (05 の 8.7).
bool register_values(LhatProgram *program, Godot *module);

// 05 の 8.9: one of the value types as an L^ value, and the bytes back.
// Only the four a Packed*Array holds -- the rest are reached through their
// own members. Overloaded rather than templated so a caller needs no traits
// of its own.
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector2 &value);
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector3 &value);
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector4 &value);
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Color &value);
bool value_taken(LhatValue held, const Godot *module, Vector2 *out);
bool value_taken(LhatValue held, const Godot *module, Vector3 *out);
bool value_taken(LhatValue held, const Godot *module, Vector4 *out);
bool value_taken(LhatValue held, const Godot *module, Color *out);

// 05 の 8.9: what a box of one of the value types holds, as a Variant, and
// the other way. `found` says whether the value was a box of one of ours.
//
// A box is where a host value goes when it has to outlive the frame -- a
// table's element, an any^, the tail of a call -- so it is what arrives
// wherever the engine is handed one. Reading it is the whole of crossing;
// there is no making one from here, since the box is a heap object and the
// heap is the machine's (box^ is the spelling that makes one, and 8.9 wants
// keeping something to be visible in the spelling).
Variant boxed_variant(LhatValue value, const Godot *module, bool *found);

// Writes a Variant of the matching kind over what a box holds. False where
// the value is not a box of ours, the Variant is of another kind, or the box
// is the sealed one a definition keeps (14.11).
bool box_takes_variant(LhatValue value, const Godot *module,
                       const Variant &held);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_VALUES_H
