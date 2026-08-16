// L^ (lhat) -- the `godot` module: one host type for every engine object.
//
// 05 の 8.8 makes a host type nominal, and 971 engine classes in an eight
// deep tree have no nominal shape L^ could be given: two host types are
// related by nothing, so `add_child(Node)` could never take a Sprite2D. So
// there is **one** host type here, and the hierarchy is written in L^
// instead -- a def^ per engine class, composed with `..`, which 14.10's
// width subtyping already relates the right way round.
//
// What the type carries is an instance id and not a pointer. A node the
// program still holds may have been freed, and ObjectDB answers that
// question where a raw pointer would only be wrong: the id has a generation
// in it, so a recycled slot never answers to the old one. A RefCounted also
// gets a Ref held beside the id, since nothing else would keep it alive.

#ifndef LHAT_GODOT_MODULE_H
#define LHAT_GODOT_MODULE_H

#include <godot_cpp/classes/object.hpp>

#include "lhat.h"

namespace godot {
namespace host {

// What one program's registration made. Belongs to the program and lives as
// long as it does.
struct Godot {
    const LhatHostDataTag *object_tag;
};

// Registers the module into `program`, before any checking (05 の 8.7).
// NULL when there was no memory or a name was taken.
const Godot *register_godot(LhatProgram *program);

// A value of godot.Object standing for `object`, or the one standing for
// nothing when it is NULL. False only when the machine ran out of memory.
bool make_object(LhatMachine *machine, const Godot *module, Object *object,
                 LhatValue *out);

// The object a value of godot.Object stands for, or NULL when the value is
// not one of ours, stands for nothing, or names something already freed.
Object *object_of(LhatValue value, const Godot *module);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_MODULE_H
