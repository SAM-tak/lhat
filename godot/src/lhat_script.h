// L^ (lhat) -- a .lh file, as the engine holds it.
//
// A Script in Godot is a Resource that can also make instances, and both
// halves are here. Checking is 03 の 1.1 over the graph; instantiating is
// 02 の 14.3, which already says what "one script, many nodes" means: the
// members are shared and each instance holds its own self^ fields.
//
// **A script a node can wear has to be a module^ unit with exactly one
// public^ def^ in it.** Both halves of that earn their keep. module^ is what
// puts the unit's answer in L^.modules, which is a root the collector
// reaches -- without it the definition would be swept while the engine still
// held nodes wearing it. And one def^ is what makes "the class this file
// declares" a thing to find without a naming convention.
//
// One program and one machine per script, shared by every node wearing it.
// The instances live in a table under L^.modules, so they are rooted for the
// same reason the definition is.

#ifndef LHAT_GODOT_SCRIPT_H
#define LHAT_GODOT_SCRIPT_H

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>  // _get_language answers one
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "lhat.h"
#include "lhat_host.h"

namespace godot {

class LhatScript : public ScriptExtension {
    GDCLASS(LhatScript, ScriptExtension)

    String source;
    bool checked = false;  // whether _reload has run since the text changed
    bool valid = false;

    // The loader's context outlives the program, which reads through it.
    host::Units units;
    LhatProgram *program = nullptr;
    LhatMachine *machine = nullptr;

    // The one public^ def^, and the table the instances are rooted in. Both
    // reachable from L^, so neither is held here for the collector's sake --
    // only for the host's.
    LhatValue klass = lhat_nil();
    LhatValue instances = lhat_nil();
    // 02 の 18: an annotation is read off the tree, which the program still
    // holds -- so what a field was written with is asked for by name, and
    // these two are what names it.
    const LhatUnit *unit = nullptr;
    String klass_name;
    int64_t next_id = 1;
    bool runnable = false;

    void let_go();

protected:
    static void _bind_methods();

public:
    ~LhatScript();

    // What the instance table's functions work through. NULL until _reload
    // has made one.
    LhatMachine *lhat_machine() const { return machine; }
    LhatValue lhat_class() const { return klass; }
    const LhatUnit *lhat_unit() const { return unit; }
    const String &lhat_class_name() const { return klass_name; }

    // 14.9's `new`, and the instance put where the collector reaches it.
    bool make_instance(Object *owner, LhatValue *out, int64_t *id);

    // What the conversions need to carry a handle across.
    const host::Godot *godot() const { return units.godot; }
    void drop_instance(int64_t id);

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

    // The engine asks for these before it will show a script at all, so
    // leaving them to a default is an error at every call rather than a
    // silence. L^ has no documentation form and no class icon, and a member's
    // line is what a jump-to would use -- none of the three has an answer
    // here yet, and each says so in its own empty way.
    StringName _get_doc_class_name() const override;
    String _get_class_icon_path() const override;
    int32_t _get_member_line(const StringName &member) const override;

    bool _is_tool() const override;
    bool _is_abstract() const override;

    // 14.7: what an instance sees is what its definition holds, so this is
    // the same question the instance table's has_method asks.
    bool has_lhat_method(const StringName &method) const;

    bool _has_method(const StringName &method) const override;
    bool _has_static_method(const StringName &method) const override;
    Dictionary _get_method_info(const StringName &method) const override;
    Variant _get_script_method_argument_count(
        const StringName &method) const override;
    TypedArray<Dictionary> _get_script_method_list() const override;
    TypedArray<Dictionary> _get_script_property_list() const override;

    // 18.7: whether an extern^ of this name was marked @signal, and how
    // many arguments its declared type wrote.
    bool signal_named(const StringName &wanted, size_t *out_arguments) const;

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
