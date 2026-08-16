// L^ (lhat) -- a .lh file, as the engine holds it.
//
// A Script in Godot is a Resource that can also make instances. This is the
// first half only: the text, whether it checks, and enough of the rest for
// the editor to open and save it. **It instantiates nothing** -- what a node
// with an L^ script attached would do needs values crossing between L^ and
// Variant, and that layer is not written.
//
// So _can_instantiate answers false and every question about members and
// methods answers empty. That is honest rather than provisional: an empty
// answer is what a script with no instances has.

#ifndef LHAT_GODOT_SCRIPT_H
#define LHAT_GODOT_SCRIPT_H

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>  // _get_language answers one
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class LhatScript : public ScriptExtension {
    GDCLASS(LhatScript, ScriptExtension)

    String source;
    bool checked = false;  // whether _reload has run since the text changed
    bool valid = false;

protected:
    static void _bind_methods();

public:
    // 03 の 1.1 over the graph: the text is checked with the units it
    // requires, read through res://. Answers OK, or ERR_PARSE_ERROR with
    // what went wrong pushed as errors.
    Error _reload(bool keep_state) override;

    bool _has_source_code() const override;
    String _get_source_code() const override;
    void _set_source_code(const String &code) override;
    bool _is_valid() const override;
    bool _editor_can_reload_from_file() override;

    ScriptLanguage *_get_language() const override;

    // Nothing here yet -- see the head of this file.
    bool _can_instantiate() const override;
    void *_instance_create(Object *for_object) const override;
    void *_placeholder_instance_create(Object *for_object) const override;
    bool _instance_has(Object *object) const override;
    void _placeholder_erased(void *placeholder) override;
    bool _is_placeholder_fallback_enabled() const override;

    Ref<Script> _get_base_script() const override;
    StringName _get_global_name() const override;
    bool _inherits_script(const Ref<Script> &script) const override;
    StringName _get_instance_base_type() const override;

    bool _is_tool() const override;
    bool _is_abstract() const override;

    bool _has_method(const StringName &method) const override;
    bool _has_static_method(const StringName &method) const override;
    Dictionary _get_method_info(const StringName &method) const override;
    Variant _get_script_method_argument_count(
        const StringName &method) const override;
    TypedArray<Dictionary> _get_script_method_list() const override;
    TypedArray<Dictionary> _get_script_property_list() const override;

    bool _has_script_signal(const StringName &signal) const override;
    TypedArray<Dictionary> _get_script_signal_list() const override;

    bool _has_property_default_value(const StringName &property) const override;
    Variant _get_property_default_value(
        const StringName &property) const override;
    void _update_exports() override;

    Dictionary _get_constants() const override;
    TypedArray<StringName> _get_members() const override;
    Variant _get_rpc_config() const override;
    TypedArray<Dictionary> _get_documentation() const override;
};

}  // namespace godot

#endif  // LHAT_GODOT_SCRIPT_H
