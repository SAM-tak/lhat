#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "lhat_runtime.h"

using namespace godot;

// SCENE is the level at which a class a scene may hold becomes registrable,
// and is where everything L^ offers belongs -- nothing here is a server or an
// editor-only class.
void lhat_initialise_module(ModuleInitializationLevel level)
{
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(LhatRuntime);
}

void lhat_uninitialise_module(ModuleInitializationLevel level)
{
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
// The name demo/lhat.gdextension gives as entry_symbol. Everything the engine
// knows about this library it learns through the call below.
GDExtensionBool GDE_EXPORT lhat_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization *initialisation)
{
    GDExtensionBinding::InitObject init(get_proc_address, library,
                                        initialisation);

    init.register_initializer(lhat_initialise_module);
    init.register_terminator(lhat_uninitialise_module);
    init.set_minimum_library_initialization_level(
        MODULE_INITIALIZATION_LEVEL_SCENE);

    return init.init();
}
}
