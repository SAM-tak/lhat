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
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "lhat.h"

namespace godot {
namespace host {

// What one program's registration made. Belongs to the program and lives as
// long as it does.
struct Godot {
    const LhatHostDataTag *object_tag;
    // 05 の 8.8: a Callable and a Signal hold references rather than bytes,
    // so neither is one of 8.9's values (lhat_godot_handles.h).
    const LhatHostDataTag *callable_tag = nullptr;
    const LhatHostDataTag *signal_tag = nullptr;
    // 05 の 8.8: one tag per packed array, kept under the Variant kind it
    // stands for (lhat_godot_packed.h).
    const LhatHostDataTag *packed_tags[Variant::VARIANT_MAX] = {};

    // A registered name is borrowed rather than copied
    // (lhat_type_add_member keeps the pointer it is given), and the names
    // and signatures of the value types are built rather than written out.
    // So they are kept here -- and since this module is the process's, they
    // outlive every program that borrows them.
    //
    // The index is what makes a second registration cost nothing: the same
    // spelling hands back the same pointer, so registering into a fresh
    // program builds no string it built before.
    List<CharString> texts;
    HashMap<String, const char *> index;

    // 05 の 8.9: one tag per value type, kept under the Variant kind it
    // stands for -- so a host that has read a Variant can find the tag from
    // the type it found. NULL where nothing was registered.
    const LhatHostValueTag *value_tags[Variant::VARIANT_MAX] = {};
};

// Registers the module into `program`, before any checking (05 の 8.7).
// NULL when there was no memory or a name was taken.
//
// One module for the process. 05 の 8.7 makes a registration a declaration
// and interns what it declares, so a second program comes away with the very
// tags the first did -- there is nothing per-program in here to rebuild.
// What each program does keep is the checker's side of the registration,
// which is why the calls are made again for each; the strings they borrow
// are the ones built the first time.
const Godot *register_godot(LhatProgram *program);

// A registered name is borrowed rather than copied (lhat_type_add_member
// keeps the pointer it is given), and the value types' names and signatures
// are built rather than written out. This is where a built one goes: the
// module outlives every program, and the same spelling asked for twice hands
// back the one pointer, so registering into a second program builds nothing.
const char *kept(Godot *module, const String &text);

// Gives the module back. Only when no LhatProgram still borrows its strings,
// which means after every program is free -- the same window
// lhat_registry_dispose asks for, and the call belongs just before it.
void dispose_godot();

// A value of godot.Object standing for `object`, or the one standing for
// nothing when it is NULL. False only when the machine ran out of memory.
bool make_object(LhatMachine *machine, const Godot *module, Object *object,
                 LhatValue *out);

// The object a value of godot.Object stands for, or NULL when the value is
// not one of ours, stands for nothing, or names something already freed.
Object *object_of(LhatValue value, const Godot *module);

// 02 の 18.7改: what a member marked @signal with an empty body is filled
// with. The name it emits and the module it converts arguments through --
// one of these per such member, made when a script is read and living as
// long as the machine holding the value that points at it.
struct SignalEmitter {
    const Godot *module = nullptr;
    StringName name;
};

// The value to put under that member's name: called like the member it
// replaces (`parameters` is what a caller writes, self^ not among them --
// 13.4), it emits `emitter`'s name off the receiver with whatever it was
// handed. The receiver is an instance of a class composed onto Godot.Object,
// so the engine object comes off its gdobj field.
//
// `emitter` is borrowed and has to outlive the value. False only when the
// machine ran out of memory.
bool make_signal_emitter(LhatMachine *machine, const SignalEmitter *emitter,
                         uint8_t parameters, bool variadic, LhatValue *out);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_MODULE_H
