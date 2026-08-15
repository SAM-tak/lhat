// L^ (lhat) -- what the engine calls when it loads and unloads the library.

#ifndef LHAT_GODOT_REGISTER_TYPES_H
#define LHAT_GODOT_REGISTER_TYPES_H

#include <godot_cpp/core/class_db.hpp>

void lhat_initialise_module(godot::ModuleInitializationLevel level);
void lhat_uninitialise_module(godot::ModuleInitializationLevel level);

#endif  // LHAT_GODOT_REGISTER_TYPES_H
