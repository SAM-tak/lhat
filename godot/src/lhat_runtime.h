// L^ (lhat) -- the class the engine gets.
//
// Everything is static and nothing is kept: a call makes a program, checks
// it, runs it and frees it. That is the shape 05 の 8.7 lays out, and it is
// as far as this goes until a script is a Script -- an LhatMachine that
// outlives a call is what a node's own state will need, and there are no
// nodes yet.

#ifndef LHAT_GODOT_RUNTIME_H
#define LHAT_GODOT_RUNTIME_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class LhatRuntime : public RefCounted {
    GDCLASS(LhatRuntime, RefCounted)

protected:
    static void _bind_methods();

public:
    // The version of the language this extension was built against.
    static String version();

    // What a run that stopped has to say, in the words 04 gives it.
    static String run_status_message(int status);

    // 03 の 1.1's stages over 05 の 6.2's graph: the unit at `path` and
    // everything it requires. Answers what they reported, empty when the lot
    // is clean. `path` is a Godot path -- "res://..." or "user://...".
    static PackedStringArray check(const String &path);

    // The same, and then runs it. Answers what stopped it, empty when it
    // ran; what the unit answered, if anything, is printed.
    static PackedStringArray run(const String &path);

    // Runs the unit and calls one member of what it answered -- a unit
    // answers a table of its public^ names (05 の 5.5), which is what makes
    // "the members of a unit" a thing to call at all.
    //
    // The unit runs afresh every time, so nothing it did last call is still
    // there. Keeping a machine between calls is what a Script instance will
    // do, and needs a way for a host to hold a value across a run that the
    // library does not have yet.
    //
    // Answers nil and pushes an error when anything went wrong: the unit did
    // not check, there is no such member, an argument had no shape in L^, or
    // the call stopped.
    static Variant call_member(const String &path, const String &member,
                               const Array &arguments);
};

}  // namespace godot

#endif  // LHAT_GODOT_RUNTIME_H
