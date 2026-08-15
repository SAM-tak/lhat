// L^ (lhat) -- the class the engine gets.
//
// It holds nothing yet. What it will hold is an LhatProgram and an
// LhatMachine, the pair 05 の 8.7 has a host make in that order; the two
// static answers here are what a build has to be able to do before any of
// that is worth writing -- reach the header, and reach the library.

#ifndef LHAT_GODOT_RUNTIME_H
#define LHAT_GODOT_RUNTIME_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

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
};

}  // namespace godot

#endif  // LHAT_GODOT_RUNTIME_H
