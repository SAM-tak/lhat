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
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "lhat.h"
#include "lhat_godot_module.h"  // SignalEmitter, one per filled-in @signal
#include "lhat_host.h"

namespace godot {

class LhatScript : public ScriptExtension {
    GDCLASS(LhatScript, ScriptExtension)

    // 05 の 5.3: the world is the language's, and a rebuild is its to drive
    // -- it takes every script off, makes the world again, and puts them all
    // back. The three below are the halves of that, and nobody else's.
    friend class LhatLanguage;

    String source;
    bool checked = false;  // whether _reload has run since the text changed
    bool valid = false;

    // 05 の 5.3: the program, the machine and the loader's context all
    // belong to the language now -- one world for the process, so that two
    // scripts can hand each other a value. What is left here is what this
    // one .lh is: its unit of that program, and what was read off it.

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

    // 05 の 3.2: the unit named no module^, so it is a run of statements
    // rather than a place that declares a class. Godot's nearest thing is an
    // EditorScript -- a script the editor runs on being asked to and puts on
    // nothing -- so that is what this answers to be, and File > Run runs the
    // body. A unit that named one keeps every other answer it had.
    //
    // 02 の 8.2 lets a top-level statement do anything, so reading one of
    // these must not run it: the engine loads every .lh in the project as a
    // script resource, and reloads it whenever it is saved or asked about.
    bool editor_script = false;
    // The body, for when File > Run asks. Belongs to `program` and is let go
    // with it.
    const LhatProto *entry = nullptr;

    // What a fresh instance's @export fields hold, by name. Read once, off
    // the prototype the definition hangs under self^ (02 の 14.11) when the
    // class is found -- every default is already a value there, so nothing
    // is made and nothing runs. A field the writer's new fills has no key on
    // the prototype and so no default, which is what the inspector's revert
    // arrow should mean for it.
    Dictionary defaults;

    // 02 の 18: what @rpc says, as the engine wants it -- a Dictionary of
    // member name to the configuration for that member. Read once when the
    // class is found, so the vocabulary is judged in one place; the engine
    // asks for the whole of it (_get_rpc_config) rather than per member.
    Dictionary rpc_config;

    // 14.12's gdBaseClass, where the definition wrote one: the least engine
    // class a node must be for this script to fit. Empty for a definition
    // wrapping nothing, and _get_instance_base_type answers Object then.
    //
    // Not the same question className() answers. That one says what a node
    // actually is -- an AnimatedSprite2D wearing a class written for a
    // Sprite2D answers the former -- and this one is the floor it has to
    // clear.
    String base_class;
    // 02 の 18: whether the class a node wears carried @tool rather than
    // @game. Read once when the class is found, since _is_tool is const and
    // asked often -- walking the tree each time would be walking it per frame.
    bool tool_script = false;

    // Which machine the instances belong to. _reload disposes the one they
    // were made on, and a node goes on holding its script instance across
    // that -- so what it holds is a value out of freed memory, and the check
    // that it still looks like a table would be reading that memory to ask.
    //
    // Counted rather than chased: the script would otherwise have to keep a
    // list of live instances and reach into each, and an instance that
    // outlives its machine has nothing worth saying anyway. One number tells
    // it it is from the machine before.
    // The world's, not this script's: one machine means one number, and
    // every instance of every script is from the same rebuild.

    // What to say if somebody wears this file, and nothing if nobody does.
    // Godot reads every .lh in the project as a script resource, so a
    // complaint about which class a node would wear does not belong at load:
    // a library answering several classes (05 の 5.5) is not doing anything
    // wrong until it is put on a node.
    String unwearable;

    // Which objects are wearing this script, by id. Kept so a reload can put
    // it back on them: 14.3 fixes an instance's fields when it is made, and
    // _reload makes a new machine, so what was worn before the reload is not
    // a thing to keep -- the object has to be given the script again.
    //
    // A set rather than a list with a matching removal: an object that has
    // gone away leaves an id that answers nothing, and pruning it where it is
    // found is cheaper than a hook on every way an instance can end.
    HashSet<uint64_t> worn_by;

    // The placeholders this script made, so a reload can hand each the list
    // it should be showing. The editor builds one of these for a class it is
    // not to run (@game), and what it shows is whatever it was last given --
    // there is nothing in it that reads the script again.
    LocalVector<void *> placeholders;

    // 02 の 18.7改: one per @signal member this script filled in, holding the
    // name that member emits. The value in the definition table points at
    // one of these, so they live exactly as long as the machine does --
    // let_go frees them where it disposes it.
    LocalVector<host::SignalEmitter *> emitters;

    void let_go();
    void fill_signals();
    void fill_rpc();
    // What _reload does before the wearers are dealt with. Split off so the
    // dealing happens whatever the reading answered -- a text that no longer
    // checks leaves nodes holding instances of a machine that is gone.
    Error reload_now(bool keep_state);
    // 14.3 fixes an instance's fields when it is made and _reload makes a new
    // machine, so nothing worn from before survives one. Every object wearing
    // this is given it again, which is what builds a fresh instance -- or the
    // editor's placeholder, where the class is not one it may run.
    //
    // `keep_state` is what the engine always asks for: the fields are read off
    // each wearer first and written back after, so a save does not throw away
    // what the inspector put in them. Which is why it is two halves -- the
    // reading has to happen while the instances are still on the machine that
    // made them.
    void take_off(bool keep_state, LocalVector<uint64_t> *wearers,
                  LocalVector<Dictionary> *held);
    void put_back(const LocalVector<uint64_t> &wearers,
                  const LocalVector<Dictionary> &held);

protected:
    static void _bind_methods();

public:
    LhatScript();
    ~LhatScript();

    // What the instance table's functions work through. NULL until _reload
    // has made one.
    LhatMachine *lhat_machine() const;
    // Which world the caller's value came from. An instance keeps the number
    // it was made under and stops touching what it holds once the two part
    // company -- 14.3 fixes its fields when it is made, and a rebuild makes
    // a machine that knows nothing of them.
    uint64_t lhat_generation() const;
    LhatValue lhat_class() const { return klass; }
    // 05 の 3.2: whether this file is a run of statements rather than a
    // declaration of a class, which is what the instance side has to know to
    // answer _run with the body.
    bool is_editor_script() const { return editor_script; }
    // Runs the body once. What File > Run reaches, through the _run of the
    // instance the editor puts on its throwaway EditorScript.
    bool run_body();
    const LhatUnit *lhat_unit() const { return unit; }
    const String &lhat_class_name() const { return klass_name; }

    // 14.9's `new`, and the instance put where the collector reaches it.
    bool make_instance(Object *owner, LhatValue *out, int64_t *id);

    // That this object is wearing the script, so a reload can put it back.
    // Said by both ways of wearing one -- a real instance and the editor's
    // placeholder -- since both are built out of what a reload replaces.
    void worn_by_object(Object *owner);

    // The editor's placeholders, which have to be handed a new list of
    // fields whenever the text they were built from changed.
    void placeholder_made(void *placeholder);
    const LocalVector<void *> &placeholder_list() const { return placeholders; }

    // Reads the file again and checks it, then gives the script back to
    // every object that was wearing it. What the engine asks for when a .lh
    // changed outside the editor (Script::reload_from_file).
    void reload_from_disk(bool keep_state);

    // Every script this program has loaded, for the reload that names none.
    static const HashSet<LhatScript *> &all();

    // What the conversions need to carry a handle across.
    const host::Godot *godot() const;
    // What a fresh instance's fields hold, by name. Empty where one could
    // not be made.
    const Dictionary &lhat_defaults() const { return defaults; }
    // 02 の 18: what @icon named on the worn binding, or empty. Read by the
    // language's _get_global_class_name, which is what the editor asks
    // before it has loaded anything.
    String lhat_icon_path() const;

    // 02 の 14.7: a static member called from GDScript, since the engine
    // reaches none of its own on a script resource. Bound as a method of
    // this class. `new` is refused -- an instance is made by putting the
    // script on a node.
    Variant call_static(const StringName &member, const Array &arguments);

private:
    void read_base_class();
    // What the definition holds under a name, nil^ where it holds nothing.
    LhatValue member_named(const StringName &name) const;

public:
    void drop_instance(int64_t id);
    void read_defaults();

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
    // silence. A member's line is what a jump-to lands on, read off the
    // text; the doc class name is the one the class is known by, and is what
    // the editor files a help page under. Only the icon is empty, and it is
    // empty on purpose -- @icon is answered through the language's
    // _get_global_class_name, which is the door the editor knocks on first.
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

    // 02 の 18: whether the member of this name was marked @signal, and
    // where in the definition it was written.
    bool signal_member(const StringName &wanted, size_t *out_at) const;

    // Said once per _reload: what the host can see about a body that the
    // checker cannot, since only the host knows what @signal meant.
    void warn_about_signals() const;

    bool _has_script_signal(const StringName &signal) const override;
    TypedArray<Dictionary> _get_script_signal_list() const override;

    bool _has_property_default_value(const StringName &property) const override;
    Variant _get_property_default_value(
        const StringName &property) const override;
    void _update_exports() override;

    Dictionary _get_constants() const override;
    TypedArray<StringName> _get_members() const override;
    Variant _get_rpc_config() const override;

    // 01 の 6.4: the comment block above a thing is what it says about it,
    // and this is that put in the shape DocData reads. Only the editor asks
    // -- a headless run never does (EditorNode::is_cmdline_mode).
    TypedArray<Dictionary> _get_documentation() const override;
};

}  // namespace godot

#endif  // LHAT_GODOT_SCRIPT_H
