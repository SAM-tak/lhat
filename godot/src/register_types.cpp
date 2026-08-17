#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/script.hpp>  // editor_plugin.hpp holds Ref<Script>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#include "lhat_highlighter.h"
#include "lhat_language.h"
#include "lhat_resource_format.h"
#include "lhat_runtime.h"
#include "lhat_script.h"

using namespace godot;

namespace {

// A ScriptLanguage is an Object rather than a RefCounted, so it is held and
// freed here; the two resource formats are Ref-counted and only need letting
// go of.
LhatLanguage *language = nullptr;
Ref<LhatScriptLoader> loader;
Ref<LhatScriptSaver> saver;

}  // namespace

// SCENE is the level at which a class a scene may hold becomes registrable,
// and is where everything L^ offers belongs -- nothing here is a server or
// an editor-only class.
void lhat_initialise_module(ModuleInitializationLevel level)
{
    // 07 の 4 章: what colours a .lh file. The level is reached while the
    // editor is still being built -- the ScriptEditor a highlighter is
    // registered with does not exist yet -- so what is registered here is a
    // plugin, and the plugin does the registering once it is in the tree.
    //
    // The level is also reached where there is no editor at all: a game
    // started from the command line passes through it. Nothing here asks for
    // one, so nothing has to guard against that.
    if (level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
        GDREGISTER_CLASS(LhatHighlighter);
        GDREGISTER_CLASS(LhatEditorPlugin);
        EditorPlugins::add_by_type<LhatEditorPlugin>();
        return;
    }
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    GDREGISTER_CLASS(LhatRuntime);
    GDREGISTER_CLASS(LhatScript);
    GDREGISTER_CLASS(LhatLanguage);
    GDREGISTER_CLASS(LhatScriptLoader);
    GDREGISTER_CLASS(LhatScriptSaver);

    language = memnew(LhatLanguage);
    Engine::get_singleton()->register_script_language(language);

    // Both halves are needed: the language says what a .lh file is, and
    // these say how one is read and written. Without them a .lh file is a
    // file the engine has no opinion about.
    loader.instantiate();
    ResourceLoader::get_singleton()->add_resource_format_loader(loader);
    saver.instantiate();
    ResourceSaver::get_singleton()->add_resource_format_saver(saver);
}

void lhat_uninitialise_module(ModuleInitializationLevel level)
{
    if (level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
        // The plugin took the highlighter out of the script editor when it
        // left the tree; what is left is the class registration.
        EditorPlugins::remove_by_type<LhatEditorPlugin>();
        return;
    }
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    if (saver.is_valid()) {
        ResourceSaver::get_singleton()->remove_resource_format_saver(saver);
        saver.unref();
    }
    if (loader.is_valid()) {
        ResourceLoader::get_singleton()->remove_resource_format_loader(loader);
        loader.unref();
    }
    if (language != nullptr) {
        Engine::get_singleton()->unregister_script_language(language);
        memdelete(language);
        language = nullptr;
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
