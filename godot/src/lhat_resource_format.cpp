#include "lhat_resource_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "lhat_script.h"

namespace godot {

void LhatScriptLoader::_bind_methods()
{
}

PackedStringArray LhatScriptLoader::_get_recognized_extensions() const
{
    PackedStringArray out;
    out.push_back("lh");
    return out;
}

bool LhatScriptLoader::_handles_type(const StringName &type) const
{
    return type == StringName("Script") || type == StringName("LhatScript");
}

String LhatScriptLoader::_get_resource_type(const String &path) const
{
    return path.get_extension().to_lower() == "lh" ? "LhatScript" : "";
}

Variant LhatScriptLoader::_load(const String &path, const String &original_path,
                                bool use_sub_threads,
                                int32_t cache_mode) const
{
    (void)original_path;
    (void)use_sub_threads;
    (void)cache_mode;

    if (!FileAccess::file_exists(path)) {
        return ERR_FILE_NOT_FOUND;
    }

    Ref<LhatScript> script;
    script.instantiate();
    script->set_source_code(FileAccess::get_file_as_string(path));
    script->set_path(path);
    // Checking here is what makes the editor's error list right the moment a
    // file is opened, rather than on the first edit.
    script->reload(false);
    return script;
}

void LhatScriptSaver::_bind_methods()
{
}

Error LhatScriptSaver::_save(const Ref<Resource> &resource, const String &path,
                             uint32_t flags)
{
    (void)flags;

    Ref<LhatScript> script = resource;
    if (script.is_null()) {
        return ERR_INVALID_PARAMETER;
    }

    Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
    if (file.is_null()) {
        return FileAccess::get_open_error();
    }
    file->store_string(script->get_source_code());
    file->close();
    return OK;
}

bool LhatScriptSaver::_recognize(const Ref<Resource> &resource) const
{
    return Object::cast_to<LhatScript>(resource.ptr()) != nullptr;
}

PackedStringArray LhatScriptSaver::_get_recognized_extensions(
    const Ref<Resource> &resource) const
{
    PackedStringArray out;
    if (Object::cast_to<LhatScript>(resource.ptr()) != nullptr) {
        out.push_back("lh");
    }
    return out;
}

}  // namespace godot
