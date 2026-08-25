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

#include "lhat.h"
#include "lhat_host.h"

namespace godot {

// Which hatted spellings this host draws apart, and the one table saying so.
//
// A guess, and knowingly one. 01 の 2.1 reserves no word: lhat_scan answers
// every hatted word as a name because the language settles nothing about the
// spelling, and a member somebody called `main^` really is a member. What is
// left is that `def^` almost always is the def^, and colour is all that rides
// on it. Both the things wanting the guess are here -- the reserved-word list
// the editor asks for, and lhat_highlighter.cpp's colours -- so it is made
// once rather than in each.
enum LhatWordKind {
    LHAT_WORD_NONE,     // 14.17's tostring^ and the like: the writer's own
    LHAT_WORD_CONTROL,  // 02 の 9/10 章: the ones that branch or leave
    LHAT_WORD_DECLARE,  // what declares, and the operators spelled as words
    LHAT_WORD_TYPE,     // number^, string^, t^, Self^ ...
    LHAT_WORD_VALUE,    // self^, this^, L^ ...: a hat word naming a value
    LHAT_WORD_CLAUSE    // 02 の 9.2's clause words: main^, id^, typeof^ ...
};

// `text` is the word as written, hat and all. 01 の 2.3: a spelling with more
// than one hat is that same name reached further out, so the hats are folded
// to one before the comparison.
LhatWordKind lhat_word_kind(const char *text, size_t length);

// Which line of `code` writes the member `member`, 1-based, or -1. What a
// jump-to-member lands on, and what the editor asks when it looks for a
// function by name.
int32_t lhat_member_line(const String &code, const String &member);

// Every subroutine the text binds, as "name:line" with the line 1-based --
// what the script editor lists down the side of the code.
PackedStringArray lhat_subroutine_lines(const String &code);

// The spellings of one kind, NULL terminated, for a caller that has to name
// them rather than recognise them.
const char *const *lhat_words_of(LhatWordKind kind);

class LhatLanguage : public ScriptLanguageExtension {
    GDCLASS(LhatLanguage, ScriptLanguageExtension)

    static LhatLanguage *singleton;

    // 05 の 5.3 with 8.6: the world this process runs in -- one program, one
    // machine, and every .lh a unit of it. What made this one rather than a
    // set of them is not the cost of the set (measured: a save is milli-
    // seconds either way) but what a set cannot do: values live on a
    // machine's heap, so two scripts on two machines can never hand each
    // other a table, however alike their types are spelt.
    //
    // A game runs in a process of its own (the editor spawns it), so the
    // editor's world and a game's world never meet and one of these is
    // always enough.
    host::Units units;
    LhatProgram *program = nullptr;
    LhatMachine *machine = nullptr;

    // 14.3 fixes an instance's fields when it is made, so an instance made
    // before a rebuild is not this world's. Bumped where the machine goes,
    // and an instance carries the number it was born under.
    uint64_t generation = 1;
    bool rebuilding = false;  // put_back writes scripts, and a write asks again

protected:
    static void _bind_methods();

public:
    LhatLanguage();
    ~LhatLanguage();

    // The one the engine registered, for a script asked which language it is.
    static LhatLanguage *get_singleton();

    // The world, made on the first ask. NULL when there was no memory.
    LhatProgram *world_program();
    LhatMachine *world_machine() const { return machine; }
    host::Units *world_units() { return &units; }
    uint64_t world_generation() const { return generation; }

    // Everything a reload does, for every script at once. One .lh saved
    // retires the whole world, because a unit compiled against another's
    // published names is compiled against the text that published them --
    // so the scripts come off, the world is made again, and they go back on.
    Error rebuild_world(bool keep_state);

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
    TypedArray<Dictionary> _get_built_in_templates(
        const StringName &object) const override;
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
    void _reload_scripts(const Array &scripts, bool soft_reload) override;
    void _reload_tool_script(const Ref<Script> &script,
                             bool soft_reload) override;
    TypedArray<Dictionary> _get_public_functions() const override;
    Dictionary _get_public_constants() const override;
    TypedArray<Dictionary> _get_public_annotations() const override;
    void _profiling_start() override;
    void _profiling_stop() override;
    void _profiling_set_save_native_calls(bool enable) override;
    int32_t _profiling_get_accumulated_data(
        ScriptLanguageExtensionProfilingInfo *info, int32_t max) override;
    int32_t _profiling_get_frame_data(
        ScriptLanguageExtensionProfilingInfo *info, int32_t max) override;
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
