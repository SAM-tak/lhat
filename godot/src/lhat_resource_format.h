// L^ (lhat) -- how a .lh file becomes a resource and goes back to disk.
//
// A Script only reaches the engine through the resource system: opening one
// in the editor, attaching one, or loading one at run time all go through
// ResourceLoader. So the language being registered is not enough on its own
// -- without these two, a .lh file is a file Godot has no opinion about.

#ifndef LHAT_GODOT_RESOURCE_FORMAT_H
#define LHAT_GODOT_RESOURCE_FORMAT_H

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class LhatScriptLoader : public ResourceFormatLoader {
    GDCLASS(LhatScriptLoader, ResourceFormatLoader)

protected:
    static void _bind_methods();

public:
    PackedStringArray _get_recognized_extensions() const override;
    bool _handles_type(const StringName &type) const override;
    String _get_resource_type(const String &path) const override;
    Variant _load(const String &path, const String &original_path,
                  bool use_sub_threads, int32_t cache_mode) const override;
};

class LhatScriptSaver : public ResourceFormatSaver {
    GDCLASS(LhatScriptSaver, ResourceFormatSaver)

protected:
    static void _bind_methods();

public:
    Error _save(const Ref<Resource> &resource, const String &path,
                uint32_t flags) override;
    bool _recognize(const Ref<Resource> &resource) const override;
    PackedStringArray _get_recognized_extensions(
        const Ref<Resource> &resource) const override;
};

}  // namespace godot

#endif  // LHAT_GODOT_RESOURCE_FORMAT_H
