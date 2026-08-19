// L^ (lhat) -- Godot's packed arrays, as 05 の 8.8's host data.
//
// A PackedVector2Array is a run of bytes the engine copies on write and
// counts references to, so it is not one of 8.9's values however its
// elements look. And it cannot be written out as an L^ table either: four of
// the ten hold host values, and 8.9 keeps one of those out of a table.
//
// So all ten are the same thing -- an opaque structure with a length and an
// index. `at` answers an element, which for those four is a host value, and
// answering one is what a host may do wherever a signature says so. Nothing
// is copied on the way in or out: a thousand-element array costs a call per
// element read rather than a table of a thousand.

#ifndef LHAT_GODOT_PACKED_H
#define LHAT_GODOT_PACKED_H

#include <godot_cpp/variant/variant.hpp>

#include "lhat.h"

namespace godot {
namespace host {

struct Godot;

// Registers all ten, their members and the module functions that make an
// empty one. Before any checking (05 の 8.7).
bool register_packed(LhatProgram *program, Godot *module);

// A value of the matching godot.Packed*Array, made from `held` -- which is
// the Variant the engine answered. False when it is none of the ten or the
// machine ran out of memory.
bool make_packed(LhatMachine *machine, const Godot *module,
                 const Variant &held, LhatValue *out);

// The Variant one of ours stands for, or a nil Variant when the value is not
// one. `found` says which of the two it was.
Variant packed_variant(LhatValue value, const Godot *module, bool *found);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_PACKED_H
