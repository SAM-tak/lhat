// L^ (lhat) -- one node's worth of a script.
//
// The engine reaches a scripted object through a table of function pointers
// rather than through a class, so this is not a godot-cpp class at all: it is
// a GDExtensionScriptInstanceInfo3 and the data it is handed back with.
//
// What sits behind it is one instance of the script's def^ (02 の 14.3) --
// the members shared with every other node running the same script, the
// self^ fields its own. That is why a node costs an instance and not a
// machine: 14.3 is what Godot's "one script, many nodes" already is.

#ifndef LHAT_GODOT_INSTANCE_H
#define LHAT_GODOT_INSTANCE_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/object.hpp>

#include "lhat.h"

namespace godot {

class LhatScript;

// NULL when the script has nothing to instantiate, or the def^'s `new`
// stopped. The engine owns what comes back and frees it through the table.
void *lhat_instance_create(LhatScript *script, Object *owner);

// What a node wearing a @game class gets while its scene is only being
// edited: the engine's own PlaceHolderScriptInstance, told what the class
// declares so the inspector still has fields to show. Nothing of the class
// runs -- that is the whole point of not making a real one.
//
// Told from the tree rather than from an instance, since the reason there is
// a placeholder is that no instance was made. 18 says which fields to show
// and program.h's LhatUnitMember what each is called and shaped like.
void *lhat_placeholder_create(LhatScript *script, Object *owner);

}  // namespace godot

#endif  // LHAT_GODOT_INSTANCE_H
