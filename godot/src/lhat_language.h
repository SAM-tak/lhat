// L^ (lhat) -- L^ as one of the engine's script languages.
//
// Registering this is what puts L^ in the editor's Create Script dialog and
// makes a .lh file something Godot knows how to open. Most of what
// ScriptLanguageExtension asks for is about running a script, and that is
// not written yet -- those answer emptily, on purpose.
//
// The one that carries weight is _validate: it is the type checker, over the
// buffer the editor holds rather than the file on disk, and it is why a
// mistake shows up before anything runs at all (03 の 1.1).

#ifndef LHAT_GODOT_LANGUAGE_H
#define LHAT_GODOT_LANGUAGE_H

#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class LhatLanguage : public ScriptLanguageExtension {
    GDCLASS(LhatLanguage, ScriptLanguageExtension)

    static LhatLanguage *singleton;

protected:
    static void _bind_methods();

public:
    LhatLanguage();
    ~LhatLanguage();

    // The one the engine registered, for a script asked which language it is.
    static LhatLanguage *get_singleton();

    String _get_name() const override;
    String _get_type() const override;
    String _get_extension() const override;
    PackedStringArray _get_recognized_extensions() const override;
    void _init() override;
    void _finish() override;

    // 03 の 1.1 over the text as it stands. What comes back is what the
    // editor underlines.
    Dictionary _validate(const String &script, const String &path,
                         bool validate_functions, bool validate_errors,
                         bool validate_warnings,
                         bool validate_safe_lines) const override;
    String _validate_path(const String &path) const override;

    Object *_create_script() const override;
    Ref<Script> _make_template(const String &tmpl, const String &class_name,
                               const String &base_class_name) const override;
    bool _is_using_templates() override;

    // 02 の 2 章: every keyword is a word with a hat on it, and the lexer
    // tells none of them apart -- these are for the editor's highlighting
    // and are a convenience, not a lexical distinction the language makes.
    PackedStringArray _get_reserved_words() const override;
    bool _is_control_flow_keyword(const String &keyword) const override;
    PackedStringArray _get_comment_delimiters() const override;
    PackedStringArray _get_doc_comment_delimiters() const override;
    PackedStringArray _get_string_delimiters() const override;

    // 05 の 5.5: a unit is a body, not a class -- there is nothing to inherit
    // from and no name to register globally.
    bool _has_named_classes() const override;
    bool _can_inherit_from_file() const override;
    bool _supports_builtin_mode() const override;
    bool _supports_documentation() const override;
    bool _handles_global_class_type(const String &type) const override;
    Dictionary _get_global_class_name(const String &path) const override;
    ScriptLanguage::ScriptNameCasing _preferred_file_name_casing()
        const override;

    // Editing, which the language server already answers elsewhere.
    Dictionary _complete_code(const String &code, const String &path,
                              Object *owner) const override;
    Dictionary _lookup_code(const String &code, const String &symbol,
                            const String &path, Object *owner) const override;
    String _auto_indent_code(const String &code, int32_t from_line,
                             int32_t to_line) const override;
    int32_t _find_function(const String &function,
                           const String &code) const override;
    String _make_function(const String &class_name,
                          const String &function_name,
                          const PackedStringArray &args) const override;
    bool _can_make_function() const override;
    bool _overrides_external_editor() override;

    // Nothing runs yet, so nothing here has anything to say.
    void _add_global_constant(const StringName &name,
                              const Variant &value) override;
    void _add_named_global_constant(const StringName &name,
                                    const Variant &value) override;
    void _remove_named_global_constant(const StringName &name) override;
    void _thread_enter() override;
    void _thread_exit() override;
    void _frame() override;
    void _reload_all_scripts() override;
    void _reload_tool_script(const Ref<Script> &script,
                             bool soft_reload) override;
    TypedArray<Dictionary> _get_public_functions() const override;
    Dictionary _get_public_constants() const override;
    TypedArray<Dictionary> _get_public_annotations() const override;
    void _profiling_start() override;
    void _profiling_stop() override;
    void _profiling_set_save_native_calls(bool enable) override;
    String _debug_get_error() const override;
    int32_t _debug_get_stack_level_count() const override;
    int32_t _debug_get_stack_level_line(int32_t level) const override;
    String _debug_get_stack_level_function(int32_t level) const override;
    String _debug_get_stack_level_source(int32_t level) const override;
    Dictionary _debug_get_stack_level_locals(int32_t level, int32_t max_subitems,
                                             int32_t max_depth) override;
    Dictionary _debug_get_stack_level_members(int32_t level,
                                              int32_t max_subitems,
                                              int32_t max_depth) override;
    void *_debug_get_stack_level_instance(int32_t level) override;
    Dictionary _debug_get_globals(int32_t max_subitems,
                                  int32_t max_depth) override;
    TypedArray<Dictionary> _debug_get_current_stack_info() override;
};

}  // namespace godot

#endif  // LHAT_GODOT_LANGUAGE_H
