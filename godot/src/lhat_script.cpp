#include "lhat_script.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "lhat.h"
#include "lhat_host.h"
#include "lhat_language.h"

namespace godot {

void LhatScript::_bind_methods()
{
}

Error LhatScript::_reload(bool keep_state)
{
    (void)keep_state;
    checked = true;
    valid = false;

    // The text as it stands, not the file: the editor reloads while the
    // buffer is unsaved, which is exactly when the answer matters.
    host::Units units = host::units_for(get_path());
    host::hold(&units, source);

    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        UtilityFunctions::push_error(
            host::problem(get_path(), "out of memory"));
        return ERR_OUT_OF_MEMORY;
    }

    const LhatUnit *root =
        lhat_program_check(program, units.path.utf8().get_data());
    valid = root != nullptr && !lhat_program_has_errors(program);
    if (!valid) {
        PackedStringArray said;
        host::diagnostics_into(program, said);
        for (int i = 0; i < said.size(); i++) {
            UtilityFunctions::push_error(said[i]);
        }
    }
    lhat_program_free(program);

    return valid ? OK : ERR_PARSE_ERROR;
}

bool LhatScript::_has_source_code() const
{
    return !source.is_empty();
}

String LhatScript::_get_source_code() const
{
    return source;
}

void LhatScript::_set_source_code(const String &code)
{
    source = code;
    checked = false;
    valid = false;
}

// What the editor asks before it will show the script as sound. Text that
// has never been checked is not yet wrong, so it is not yet valid either.
bool LhatScript::_is_valid() const
{
    return checked && valid;
}

bool LhatScript::_editor_can_reload_from_file()
{
    return true;
}

ScriptLanguage *LhatScript::_get_language() const
{
    return LhatLanguage::get_singleton();
}

bool LhatScript::_can_instantiate() const
{
    return false;
}

void *LhatScript::_instance_create(Object *for_object) const
{
    (void)for_object;
    return nullptr;
}

void *LhatScript::_placeholder_instance_create(Object *for_object) const
{
    (void)for_object;
    return nullptr;
}

bool LhatScript::_instance_has(Object *object) const
{
    (void)object;
    return false;
}

void LhatScript::_placeholder_erased(void *placeholder)
{
    (void)placeholder;
}

bool LhatScript::_is_placeholder_fallback_enabled() const
{
    return false;
}

// 02 の 8.1 hands out no names and 05 の 5.5 gives a unit no base to inherit
// from -- a unit is a body, not a class, so there is nothing above it.
Ref<Script> LhatScript::_get_base_script() const
{
    return Ref<Script>();
}

StringName LhatScript::_get_global_name() const
{
    return StringName();
}

bool LhatScript::_inherits_script(const Ref<Script> &script) const
{
    (void)script;
    return false;
}

StringName LhatScript::_get_instance_base_type() const
{
    return StringName();
}

bool LhatScript::_is_tool() const
{
    return false;
}

bool LhatScript::_is_abstract() const
{
    return false;
}

bool LhatScript::_has_method(const StringName &method) const
{
    (void)method;
    return false;
}

bool LhatScript::_has_static_method(const StringName &method) const
{
    (void)method;
    return false;
}

Dictionary LhatScript::_get_method_info(const StringName &method) const
{
    (void)method;
    return Dictionary();
}

Variant LhatScript::_get_script_method_argument_count(
    const StringName &method) const
{
    (void)method;
    return Variant();
}

TypedArray<Dictionary> LhatScript::_get_script_method_list() const
{
    return TypedArray<Dictionary>();
}

TypedArray<Dictionary> LhatScript::_get_script_property_list() const
{
    return TypedArray<Dictionary>();
}

bool LhatScript::_has_script_signal(const StringName &signal) const
{
    (void)signal;
    return false;
}

TypedArray<Dictionary> LhatScript::_get_script_signal_list() const
{
    return TypedArray<Dictionary>();
}

bool LhatScript::_has_property_default_value(const StringName &property) const
{
    (void)property;
    return false;
}

Variant LhatScript::_get_property_default_value(
    const StringName &property) const
{
    (void)property;
    return Variant();
}

void LhatScript::_update_exports()
{
}

Dictionary LhatScript::_get_constants() const
{
    return Dictionary();
}

TypedArray<StringName> LhatScript::_get_members() const
{
    return TypedArray<StringName>();
}

Variant LhatScript::_get_rpc_config() const
{
    return Variant();
}

TypedArray<Dictionary> LhatScript::_get_documentation() const
{
    return TypedArray<Dictionary>();
}

}  // namespace godot
