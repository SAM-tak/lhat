#include "lhat_script.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "lhat.h"
#include "lhat_godot_module.h"
#include "lhat_host.h"
#include "lhat_instance.h"
#include "lhat_language.h"
#include "lhat_variant.h"

namespace godot {

// 02 の 14.7's static members, reachable from GDScript. Bound here because
// the engine has no path of its own to them (see call_static).
void LhatScript::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("call_static", "member", "arguments"),
                         &LhatScript::call_static, DEFVAL(Array()));
}

namespace {

// 02 の 18: whether the binding of this name carries this mark.
bool marked_with(const LhatUnit *unit, const String &name, const char *mark)
{
    CharString spelt = name.utf8();
    size_t written = lhat_unit_annotation_count(unit, spelt.get_data(), NULL);
    for (size_t i = 0; i < written; i++) {
        LhatAnnotation at =
            lhat_unit_annotation(unit, spelt.get_data(), NULL, i);
        if (at.name != NULL &&
            String::utf8(at.name, (int)at.name_length) == mark) {
            return true;
        }
    }
    return false;
}

// Whether a call could reach this value. 14.12's overload^ puts a group in
// the table rather than a subroutine, so a member with two arms is callable
// without being one -- which is the case a kind test written out by hand
// keeps missing.
bool callable(LhatValue value)
{
    return lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE) ||
           lhat_is_object_kind(value, LHAT_OBJECT_OVERLOAD) ||
           lhat_is_object_kind(value, LHAT_OBJECT_HOST);
}

// 05 の 5.5: what a module^ unit answers is the table of its public^ names,
// and 14.9 marks a table a def^ made -- so the classes a unit publishes are
// there to find without a naming convention. Which of them a node wears is
// what @game and @tool say, and which of the two says it is when it runs.
//
// Two marks, one question. @tool is @game plus the editor (Unity spells the
// same thing [ExecuteAlways]), so a class is one or the other and never both.
// Splitting them into "which class" and "when" instead would have every
// wearable file carry two marks, and the second would mean nothing anywhere
// but on the class the first picked.
//
// A unit publishing one class need name neither. That is not a further rule
// so much as the first one asked of a shorter list: with one candidate there
// is nothing to choose. Saying it where there are several is what the engine
// could not otherwise know, and 18.1 is where a host's question belongs --
// "the one public^ def^" was a rule the language had no part in, and it made
// publishing a second class cost the file its script.
//
// 18.4改 registers both for a published binding only, so a mark on one kept
// private is refused where it is written rather than passed over here.
//
// Answers false when nothing is wearable, with `said` set where a writer has
// something to fix. `when_worn` tells the two kinds of trouble apart: a file
// contradicting itself is wrong however it is used, while a file that has not
// said which class a node wears is only a file that does not answer that
// question -- which is what every library looks like.
bool worn_definition(const LhatUnit *unit, LhatValue answered, LhatValue *out,
                     String *name, bool *tool, String *said, bool *when_worn)
{
    *when_worn = false;
    *tool = false;
    if (!lhat_is_object_kind(answered, LHAT_OBJECT_TABLE)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(answered);
    size_t definitions = 0;
    size_t marks = 0;
    LhatValue only = lhat_nil();
    String only_name;
    bool only_tool = false;

    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->value, LHAT_OBJECT_TABLE) ||
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING)) {
            continue;
        }
        const LhatTable *held = (const LhatTable *)lhat_as_object(entry->value);
        if (!held->is_definition) {
            continue;
        }
        const LhatString *key = (const LhatString *)lhat_as_object(entry->key);
        String spelt = String::utf8(key->text, (int)key->length);

        definitions++;
        bool as_tool = marked_with(unit, spelt, "tool");
        // Counted together, since they are two answers to one question. That
        // there is never more than one of them is the checker's to say now
        // (18.5.1's exclusion, registered in lhat_godot_module.cpp), and it
        // has said it before this runs -- so counting here only picks the
        // class.
        if (as_tool || marked_with(unit, spelt, "game")) {
            marks++;
            only = entry->value;
            only_name = spelt;
            only_tool = as_tool;
            continue;
        }
        // Kept in case nothing is marked and this turns out to be the only
        // one. A marked one already found is not written over.
        if (marks == 0) {
            only = entry->value;
            only_name = spelt;
        }
    }

    if (marks == 0 && definitions > 1) {
        // Not said at load. Godot reads every .lh in the project as a script
        // resource, so this would fire on a library nobody is wearing --
        // 05 の 5.5 has a unit answer its public names, and answering several
        // classes is what a library of them looks like. Measured: the engine
        // does not screen the list a node's Script field offers either, and a
        // GDScript that cannot be worn is found out the same way.
        *said = String(
            "this unit publishes more than one def^, so which one a node "
            "wears has to be written: put @game above it, or @tool for one "
            "that runs while the scene is being edited too");
        *when_worn = true;
        return false;
    }
    if (definitions == 0) {
        return false;  // a library, not a script. Not an error (see _reload)
    }
    *out = only;
    *name = only_name;
    // Unmarked is @game: what runs while somebody is editing their scene is
    // chosen out loud, never fallen into.
    *tool = only_tool;
    return true;
}

}  // namespace


// Every script this program has loaded. A file changed on disk names the
// scripts to reload where it can (Script::reload_from_file), but the reload
// that names none has to be able to find them.
namespace {
HashSet<LhatScript *> loaded;
}  // namespace

const HashSet<LhatScript *> &LhatScript::all()
{
    return loaded;
}

LhatScript::LhatScript()
{
    loaded.insert(this);
}

void LhatScript::worn_by_object(Object *owner)
{
    if (owner != nullptr) {
        worn_by.insert((uint64_t)owner->get_instance_id());
    }
}

void LhatScript::placeholder_made(void *placeholder)
{
    if (placeholder != nullptr) {
        placeholders.push_back(placeholder);
    }
}

// The file changed under us, which the editor is told about and passes on
// (Script::reload_from_file). What is held is the text, so it is read again
// and the reload below does the rest -- putting the script back on whatever
// was wearing it included.
void LhatScript::reload_from_disk(bool keep_state)
{
    String path = get_path();
    if (path.is_empty() || path.contains("::")) {
        return;  // built into a scene: there is no file to read
    }
    if (!FileAccess::file_exists(path)) {
        return;  // removed under us; what is held is better than nothing
    }
    set_source_code(FileAccess::get_file_as_string(path));
    _reload(keep_state);  // which puts the script back on its wearers
}

// Read while the instances are still on the machine that made them: once
// reload_now has let it go, an instance answers nothing and there would be
// nothing left to keep.
//
// Off the object rather than out of L^ -- the wearer may be holding a
// placeholder, whose values are the editor's and live nowhere else.
void LhatScript::take_off(bool keep_state, LocalVector<uint64_t> *wearers,
                          LocalVector<Dictionary> *held)
{
    for (const uint64_t &id : worn_by) {
        wearers->push_back(id);
    }
    worn_by.clear();

    for (const uint64_t &id : *wearers) {
        Dictionary was;
        Object *object = ObjectDB::get_instance(ObjectID(id));
        if (object != nullptr && keep_state) {
            TypedArray<Dictionary> said = object->get_property_list();
            for (int i = 0; i < said.size(); i++) {
                Dictionary one = said[i];
                if (((int64_t)one["usage"] & PROPERTY_USAGE_SCRIPT_VARIABLE) !=
                    0) {
                    StringName name = one["name"];
                    was[name] = object->get(name);
                }
            }
        }
        held->push_back(was);
    }
}

void LhatScript::put_back(const LocalVector<uint64_t> &wearers,
                          const LocalVector<Dictionary> &held)
{
    Ref<Script> self(this);
    for (uint32_t at = 0; at < wearers.size(); at++) {
        Object *object = ObjectDB::get_instance(ObjectID(wearers[at]));
        if (object == nullptr) {
            continue;  // gone since it put the script on
        }
        // Taken off first: Object::set_script answers nothing when handed the
        // script that is already there, and this is the same resource with a
        // new machine behind it.
        object->set_script(Variant());
        object->set_script(self);

        // 14.3 again: a field the new text no longer declares is not this
        // instance's to take, and instance_set answers false for it rather
        // than making one. So every name is offered and the ones that went
        // away are refused where they land.
        const Dictionary &was = held[at];
        Array names = was.keys();
        for (int i = 0; i < names.size(); i++) {
            object->set(names[i], was[names[i]]);
        }
    }
}

LhatScript::~LhatScript()
{
    loaded.erase(this);
    let_go();
}

void LhatScript::let_go()
{
    if (machine != nullptr) {
        // Every instance made on it is now holding freed memory. Nothing here
        // can reach them -- a node owns its script instance -- so instead the
        // number they were made under stops matching, and each finds that out
        // before touching what it holds.
        generation++;
        lhat_machine_dispose(machine);
        machine = nullptr;
    }
    if (program != nullptr) {
        lhat_program_free(program);
        program = nullptr;
    }
    klass = lhat_nil();
    instances = lhat_nil();
    unit = nullptr;
    klass_name = String();
    unwearable = String();
    runnable = false;
    tool_script = false;
    editor_script = false;
    entry = nullptr;  // the program it belonged to is gone
    defaults.clear();
    base_class = String();
    // 18.7改: the values pointing at these died with the machine above.
    for (host::SignalEmitter *emitter : emitters) {
        memdelete(emitter);
    }
    emitters.clear();
}

// 02 の 18.7改: a member marked @signal whose body is empty says there is
// nothing here for the language to run, and this is the host taking it up.
// What goes under the name emits the member's own name off the receiver,
// with the arguments the member declares -- which is the one line every such
// body was written as, and the one line every such body could get wrong.
//
// A member that wrote a body keeps it. What replaces a value has to be what
// nothing was written for, or a reader would be looking at code that does
// not run.
void LhatScript::fill_signals()
{
    if (unit == nullptr || !lhat_is_object_kind(klass, LHAT_OBJECT_TABLE)) {
        return;
    }
    CharString spelt = klass_name.utf8();
    LhatTable *table = (LhatTable *)lhat_as_object(klass);
    size_t count = lhat_unit_member_count(unit, spelt.get_data());
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember member = lhat_unit_member(unit, spelt.get_data(), i);
        if (member.name == NULL || !member.empty_body) {
            continue;
        }
        StringName name(String::utf8(member.name, (int)member.name_length));
        if (!signal_member(name, nullptr)) {
            continue;  // an empty body says nothing on its own
        }

        // 13.7: a '...' tail makes the count a floor rather than an exact
        // number, the same way a registration's signature would.
        bool variadic = false;
        for (size_t a = 0; a < member.parameter_count; a++) {
            variadic = variadic || lhat_unit_member_parameter(
                                       unit, spelt.get_data(), i, a)
                                       .variadic;
        }

        host::SignalEmitter *emitter = memnew(host::SignalEmitter);
        emitter->module = units.godot;
        emitter->name = name;

        LhatValue emit = lhat_nil();
        bool refused = false;
        LhatValue key = lhat_nil();
        if (!host::make_signal_emitter(machine, emitter,
                                       (uint8_t)member.parameter_count,
                                       variadic, &emit) ||
            !lhat_machine_make_string(machine, member.name, member.name_length,
                                      &key) ||
            !lhat_machine_table_set(machine, table, key, emit, &refused) ||
            refused) {
            memdelete(emitter);
            UtilityFunctions::push_error(
                host::problem(get_path(), String("could not fill @signal ") +
                                              String(name)));
            return;
        }
        emitters.push_back(emitter);
    }
}

// What the engine asks for whenever the text may have changed: the editor
// on every save, run_editor_script before it runs anything, and
// reload_from_disk for a file that changed underneath.
//
// Reading is one half and the wearers are the other. A reload makes a new
// machine, so every instance made on the last one is holding freed memory
// and every placeholder is showing a list built from text that is gone --
// and whether a node should have an instance at all can change with the
// same edit, since that is what @game and @tool decide.
//
// So the wearers are taken off first -- while their fields can still be read
// -- then the text is read, then they are put back. The middle one answers,
// and the other two happen whatever it answered: a text that no longer
// checks still leaves nodes holding instances of a machine that is gone.
Error LhatScript::_reload(bool keep_state)
{
    if (reloading) {
        return OK;  // put_back writes scripts, and a write can ask again
    }
    reloading = true;
    LocalVector<uint64_t> wearers;
    LocalVector<Dictionary> held;
    take_off(keep_state, &wearers, &held);
    Error said = reload_now(keep_state);
    put_back(wearers, held);
    reloading = false;
    return said;
}

Error LhatScript::reload_now(bool keep_state)
{
    (void)keep_state;
    checked = true;
    valid = false;
    let_go();

    // The text as it stands, not the file: the editor reloads while the
    // buffer is unsaved, which is exactly when the answer matters.
    units = host::units_for(get_path());
    host::hold(&units, source);

    program = host::program_for(&units);
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
        return ERR_PARSE_ERROR;
    }

    // Checking is the whole of what a script has to do to be valid. Running
    // it is what makes it wearable, and a script that checks but declares no
    // class is still a fine thing to have open in the editor -- so what
    // follows only decides `runnable`.
    size_t count = 0;
    const LhatModule *modules = lhat_program_compile(program, &count);
    machine = modules != nullptr ? lhat_machine_new() : nullptr;
    if (machine == nullptr) {
        UtilityFunctions::push_error(
            modules == nullptr ? host::compile_failure(program, get_path())
                               : host::problem(get_path(), "out of memory"));
        return OK;
    }
    lhat_machine_set_modules(machine, modules, count);
    lhat_program_install(program, machine);

    // 05 の 3.2: a unit that named no module^ registers nothing and declares
    // no class -- its body is a run of statements. Running one here would run
    // it on every load and every save, since the engine reads every .lh in
    // the project as a script resource; what asks for it instead is File >
    // Run, through _run below. The unit is kept, so the editor still shows
    // what the checker said about it.
    if (modules[lhat_unit_index(root)].module_name == NULL) {
        editor_script = true;
        entry = modules[lhat_unit_index(root)].proto;
        unit = root;
        return OK;
    }

    LhatRunResult ran =
        lhat_run(machine, modules[lhat_unit_index(root)].proto);
    if (ran.status != LHAT_RUN_OK) {
        UtilityFunctions::push_error(
            host::problem(get_path(), lhat_run_status_message(ran.status)));
        return OK;
    }

    unit = root;
    String name;
    String said;
    bool when_worn = false;
    if (!worn_definition(unit, ran.value, &klass, &name, &tool_script, &said,
                         &when_worn)) {
        // A unit that declares no class is a library, and require^ is what
        // reaches it -- not an error, only unwearable. `said` is set where
        // the writer has something to fix instead, and `when_worn` where that
        // is only true of somebody trying to wear it.
        if (when_worn) {
            unwearable = said;
        } else if (!said.is_empty()) {
            UtilityFunctions::push_error(host::problem(get_path(), said));
        }
        klass = lhat_nil();
        return OK;
    }

    // 05 の 8.6: the instances go where the collector reaches them. Under
    // L^.modules rather than in L^ itself, so nothing a script writes can
    // name it by accident.
    if (!lhat_machine_make_table(machine, &instances) ||
        !lhat_machine_register(machine, "godot.script", nullptr, "instances",
                               instances)) {
        UtilityFunctions::push_error(
            host::problem(get_path(), "out of memory"));
        instances = lhat_nil();
        return OK;
    }

    klass_name = name;
    runnable = true;
    read_base_class();
    fill_signals();
    read_defaults();
    warn_about_signals();
    // The editor is showing a list built from the text before this one.
    lhat_refresh_placeholders(this);
    return OK;
}

// What engine class a node must be for this script to fit: the least one,
// not the exact one. A class wrapping a Sprite2D goes on an
// AnimatedSprite2D too, and Godot reads the answer as a floor.
//
// Written on the definition rather than read out of `Godot.Sprite2D..def^`:
// the spelling is a name in L^ and says nothing about the engine, and only
// the wrapper knows which class it stands for. 14.12 has a derived one write
// override^ to replace it, and compile_def flattens the chain into one
// table, so what is read here is the nearest one written.
//
// Empty for a definition carrying none -- one that wraps nothing -- and
// _get_instance_base_type falls back to Object, which is every object and
// so refuses nothing.
void LhatScript::read_base_class()
{
    base_class = String();
    if (!lhat_is_object_kind(klass, LHAT_OBJECT_TABLE)) {
        return;
    }
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, "gdBaseClass",
                                  strlen("gdBaseClass"), &key)) {
        return;
    }
    LhatValue said =
        lhat_table_get((const LhatTable *)lhat_as_object(klass), key);
    if (!lhat_is_object_kind(said, LHAT_OBJECT_STRING)) {
        return;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(said);
    String named = String::utf8(text->text, (int)text->length);
    // A name the engine does not have is worse than none: the editor would
    // refuse the script everywhere rather than allow it anywhere.
    if (ClassDB::class_exists(named)) {
        base_class = named;
    }
}

// 02 の 14.11: a definition hangs its prototype under self^ -- the table
// every instance starts as a copy of, with each field's default already a
// value. What a fresh one holds is read straight off it, with nothing made
// and nothing run. A field new fills (14.15) has no key there and so no
// default, which is exactly what the editor should say about it.
void LhatScript::read_defaults()
{
    defaults.clear();
    if (!lhat_is_object_kind(klass, LHAT_OBJECT_TABLE)) {
        return;
    }
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, "self^", strlen("self^"), &key)) {
        return;
    }
    LhatValue held =
        lhat_table_get((const LhatTable *)lhat_as_object(klass), key);
    if (!lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
        return;
    }
    const LhatTable *fields = (const LhatTable *)lhat_as_object(held);
    for (size_t i = 0; i < fields->entry_capacity; i++) {
        const LhatTableEntry *entry = &fields->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING)) {
            continue;
        }
        const LhatString *field =
            (const LhatString *)lhat_as_object(entry->key);
        defaults[String::utf8(field->text, (int)field->length)] =
            host::to_variant(entry->value, units.godot);
    }
}

// 05 の 3.2: the body of a unit that named no module^, run once. This is the
// whole of what File > Run does to one -- there is no _run to write, since
// the statements at the top are already what a writer meant by "run this".
//
// EditorNode::run_editor_script hard-reloads before it asks, so the machine
// this runs on was made a moment ago and nothing has run on it yet.
bool LhatScript::run_body()
{
    if (!editor_script || machine == nullptr || entry == nullptr) {
        return false;
    }
    LhatRunResult ran = lhat_run(machine, entry);
    if (ran.status == LHAT_RUN_OK) {
        return true;
    }
    // 04 の 11.6改: a panic carries what it was written with, and the status
    // alone would drop the part that says anything.
    String said = String::utf8(lhat_run_status_message(ran.status));
    if (ran.status == LHAT_RUN_PANIC) {
        said += ": " + host::text_of(ran.value);
    }
    UtilityFunctions::push_error(host::problem(get_path(), said));
    return false;
}

// 14.9's `new` is an ordinary member of the definition and takes no self^,
// so it is called through the definition the way a written `Enemy.new()` is.
//
// The node is handed to it rather than written in afterwards. The engine
// builds the Object first and only then asks the script for an instance
// (_instance_create takes it), so the real thing is in hand before anything
// runs -- construction copies the prototype and the new body writes the
// node onto the copy (14.11), which is exactly where a constructor that
// wants its node would look. Filling the field on the way out would leave
// every one of them reading a placeholder.
//
// A unit that wears a node without wrapping one keeps 14.11's default `new`
// and takes nothing, so the call is tried both ways.
bool LhatScript::make_instance(Object *owner, LhatValue *out, int64_t *id)
{
    if (!runnable) {
        return false;
    }

    // A class wrapping a node takes the handle as new's argument. The engine
    // has built the Object before it asks (_instance_create), so the real
    // thing is in hand here.
    LhatValue handle = lhat_nil();
    size_t count = 0;
    if (host::make_object(machine, units.godot, owner, &handle)) {
        count = 1;
    }
    LhatRunResult made =
        lhat_machine_call_member(machine, klass, "new", 3, &handle, count);
    // 14.11改: `new(obj)` is written with override^ or overload^, and a unit
    // that wrote neither answers the arity rather than a fault of its own.
    if (count == 1 && (made.status == LHAT_RUN_ARITY ||
                       made.status == LHAT_RUN_NO_CANDIDATE)) {
        made = lhat_machine_call_member(machine, klass, "new", 3, nullptr, 0);
    }
    if (made.status != LHAT_RUN_OK) {
        UtilityFunctions::push_error(host::problem(
            get_path(), lhat_run_status_message(made.status)));
        return false;
    }

    LhatTable *table = (LhatTable *)lhat_as_object(instances);
    bool refused = false;
    if (!lhat_machine_table_set(machine, table, lhat_integer(next_id),
                                made.value, &refused) ||
        refused) {
        return false;
    }
    *out = made.value;
    *id = next_id++;
    return true;
}

void LhatScript::drop_instance(int64_t id)
{
    if (lhat_is_nil(instances)) {
        return;
    }
    // 04 の 11.3: storing nil^ is how a key stops being there.
    LhatTable *table = (LhatTable *)lhat_as_object(instances);
    bool refused = false;
    lhat_machine_table_set(machine, table, lhat_integer(id), lhat_nil(),
                           &refused);
}

// What the definition holds under this name, or nil^ where it holds nothing.
// Asked of the definition rather than of an instance on purpose: 14.7改 has
// the search through an instance answer only the members taking a receiver,
// and a static one is exactly what that would hide.
//
// The table is the flattened chain (compile_def), so what a base wrote is
// here too -- which is why this reads the value rather than the tree, whose
// members are only the ones this file wrote.
LhatValue LhatScript::member_named(const StringName &name) const
{
    if (!runnable) {
        return lhat_nil();
    }
    CharString spelt = String(name).utf8();
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, spelt.get_data(),
                                  (size_t)spelt.length(), &key)) {
        return lhat_nil();
    }
    return lhat_table_get((const LhatTable *)lhat_as_object(klass), key);
}

bool LhatScript::has_lhat_method(const StringName &method) const
{
    return callable(member_named(method));
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

// The line GDScript draws, drawn the same way: `valid && (tool ||
// ScriptServer::is_scripting_enabled())`. What runs in the editor is what
// said it should, and everything else is given a placeholder instead.
//
// Without this every .lh was a tool script -- a writer's _process ran in the
// 2D view of a scene they were only editing, which is the whole of what @tool
// exists to keep from happening.
bool LhatScript::_can_instantiate() const
{
    // 05 の 3.2: File > Run makes an EditorScript and puts this on it
    // (EditorNode::run_editor_script), and Object::set_script asks this
    // first -- answering false there would get a placeholder instead, which
    // has no _run to call. Only in the editor: EditorScript is an editor
    // class and a game has no such object to put one on.
    if (editor_script) {
        return Engine::get_singleton()->is_editor_hint();
    }
    if (!runnable) {
        return false;
    }
    // True in the editor alone; a game launched from it answers false, so
    // running the scene runs everything as it should.
    if (Engine::get_singleton()->is_editor_hint()) {
        return tool_script;
    }
    return true;
}

void *LhatScript::_instance_create(Object *for_object) const
{
    // Somebody is wearing this file, which is the first moment a question
    // only wearers ask is worth answering. Held rather than said at load
    // because every .lh in the project is read then, wearer or not.
    if (!unwearable.is_empty()) {
        UtilityFunctions::push_error(host::problem(get_path(), unwearable));
    }
    // Asked of every path the engine reaches an instance by, rather than
    // trusting it to ask _can_instantiate first: a @game class getting a real
    // instance in the editor is exactly the thing being stopped, and it is
    // cheaper to be sure here than to be right about which door was used.
    if (!_can_instantiate()) {
        return lhat_placeholder_create(const_cast<LhatScript *>(this),
                                       for_object);
    }
    // const because the engine asks it that way; making an instance writes
    // to the table this script owns, which is what the cast is for.
    return lhat_instance_create(const_cast<LhatScript *>(this), for_object);
}

void *LhatScript::_placeholder_instance_create(Object *for_object) const
{
    return lhat_placeholder_create(const_cast<LhatScript *>(this), for_object);
}

bool LhatScript::_instance_has(Object *object) const
{
    (void)object;
    return false;
}

// The editor is done with one. Forgotten here rather than left to be found
// dead later: what is kept is a bare pointer, and asking a freed one for
// anything is the mistake this hook exists to prevent.
void LhatScript::_placeholder_erased(void *placeholder)
{
    for (uint32_t i = 0; i < placeholders.size(); i++) {
        if (placeholders[i] == placeholder) {
            placeholders.remove_at(i);
            return;
        }
    }
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

// The name the class is known by across the project, which is what an icon
// and a Create Node entry hang on -- and nothing at all unless the class
// asked for one.
//
// 05 の 3.2 put the two kinds of .lh side by side, and this is the same
// posture GDScript takes within one of them: a script is anonymous unless it
// writes class_name, and is worn by path either way. A name is what a writer
// puts into the project's one flat namespace, and taking a room in it is not
// something to fall into by having written a binding.
//
// The binding's own name, and no argument to say otherwise: Godot's registry
// has no nesting, so a module^ path handed to it would be a name with dots
// in it that GDScript could not write as a type -- and a second spelling
// would be a name that can disagree with itself.
//
// Nothing here makes it unique. Two files marking a Spinner both claim it,
// the way two GDScripts writing `class_name Spinner` do; the engine's own
// names are the one collision worth refusing, since taking Node from under
// the editor breaks more than a name.
StringName LhatScript::_get_global_name() const
{
    if (!runnable || unit == nullptr || klass_name.is_empty() ||
        ClassDB::class_exists(klass_name) ||
        !marked_with(unit, klass_name, "export_class")) {
        return StringName();
    }
    return StringName(klass_name);
}

// 02 の 18: @icon on the same binding, as written. What the path means is
// Godot's -- 18.3 keeps an argument a literal, so what arrives is the
// spelling and nothing has resolved it.
String LhatScript::lhat_icon_path() const
{
    if (unit == nullptr || klass_name.is_empty()) {
        return String();
    }
    CharString spelt = klass_name.utf8();
    size_t written = lhat_unit_annotation_count(unit, spelt.get_data(), NULL);
    for (size_t i = 0; i < written; i++) {
        LhatAnnotation at =
            lhat_unit_annotation(unit, spelt.get_data(), NULL, i);
        if (at.name == NULL ||
            String::utf8(at.name, (int)at.name_length) != "icon") {
            continue;
        }
        LhatAnnotationArgument said = lhat_annotation_argument(at, 0);
        if (said.text != NULL) {
            return String::utf8(said.text, (int)said.length);
        }
    }
    return String();
}

bool LhatScript::_inherits_script(const Ref<Script> &script) const
{
    (void)script;
    return false;
}

// What the definition said it wraps (read_base_class), which is what puts a
// class in the Create Node dialog: that list is what derives from Node, and
// a script answering Object is not a node however it is written.
//
// Object where nothing said. 05 の 5.5 gives a unit no base to inherit from,
// so a definition wrapping nothing says nothing about what may wear it, and
// every object is the honest floor.
StringName LhatScript::_get_instance_base_type() const
{
    // 05 の 3.2: what File > Run asks before it will run anything
    // (is_parent_class(this, "EditorScript")). A unit that named no module^
    // is a run of statements, and an EditorScript is the engine's word for
    // one -- a script it runs on being asked to and puts on no node.
    if (editor_script) {
        return StringName("EditorScript");
    }
    if (!runnable) {
        return StringName();
    }
    return StringName(base_class.is_empty() ? String("Object") : base_class);
}

// Under which name the editor files what _get_documentation answers, and
// what it looks a member's description up by -- the inspector's tooltips
// reach the docs through this name. The same one the class is known by
// across the project; a file nothing wears has no docs to file.
StringName LhatScript::_get_doc_class_name() const
{
    return _get_global_name();
}

// The icon is answered through the language's _get_global_class_name, which
// is what the editor asks before it has loaded anything (18: @icon). This
// second door would say the same thing later, so it stays shut.
String LhatScript::_get_class_icon_path() const
{
    return String();
}

// Where to put the caret when somebody asks to be taken to a member -- what
// double-clicking a connection in the Node dock's signal list does.
//
// 14.3 writes a member as a name bound to a value, so what is looked for is
// the name at the head of a line with an '=' after it. Read off the text
// rather than the tree: the buffer being shown is what the line numbers have
// to agree with, and it is asked of a script that may not have checked.
int32_t LhatScript::_get_member_line(const StringName &member) const
{
    return lhat_member_line(source, String(member));
}

// 02 の 18: @tool on the class a node wears. Additive rather than exclusive,
// the way Unity's [ExecuteAlways] is -- a @tool class runs while the scene is
// being edited *and* when the game runs. @game is the same class without the
// first half, and is what an unmarked one is.
bool LhatScript::_is_tool() const
{
    // 05 の 3.2: the other half of what File > Run asks. An editor script is
    // a tool script by what it is -- the editor is the only place it runs.
    return editor_script || tool_script;
}

bool LhatScript::_is_abstract() const
{
    return false;
}

// What Node reads before it turns processing on for a script -- so a unit
// that declares _process gets one, and one that does not costs nothing.
bool LhatScript::_has_method(const StringName &method) const
{
    // 05 の 3.2: EditorScript::run is GDVIRTUAL_CALL(_run), which reaches the
    // instance by that name. The writer wrote no such member -- the body is
    // what it stands for -- so the one method an editor script has is said
    // here rather than looked for in a definition it does not have.
    if (editor_script) {
        return method == StringName("_run");
    }
    return has_lhat_method(method);
}

// 02 の 14.7: a member taking no receiver belongs to the definition and not
// to an instance -- `A.somestatic()` in the language, and a static method
// here. What Object::has_method answers when it is asked of the script
// resource rather than of a node wearing it.
//
// 14.11's `new` is left out. The engine has its own way to make an instance
// of a script -- putting it on a node, which is what _instance_create is for
// -- and an instance made through a second door would hold no node and sit
// in no instance table. call_static refuses the name for the same reason.
bool LhatScript::_has_static_method(const StringName &method) const
{
    if (method == StringName("new")) {
        return false;
    }
    LhatValue held = member_named(method);
    return callable(held) && !lhat_takes_receiver(held);
}

// The other half of the answer above. ScriptExtension is given no door for a
// call made on the script resource itself -- Object::callp reaches ClassDB
// and never asks the language -- so saying a static member is there and
// leaving no way to reach it would be half an answer. This is the way.
//
// 14.7's search is the one lhat_machine_call_member makes, which is what a
// compiled `A.somestatic()` makes too: an overload^ group is resolved the way
// a call site resolves it, and a member taking no receiver is simply called.
Variant LhatScript::call_static(const StringName &member,
                                const Array &arguments)
{
    if (!_has_static_method(member)) {
        UtilityFunctions::push_error(host::problem(
            get_path(),
            member == StringName("new")
                ? String("new is not called this way -- an instance is made "
                         "by putting the script on a node")
                : String("there is no static member ") + String(member)));
        return Variant();
    }

    // Nothing runs from here until the call, which is what keeps the values
    // built below reachable -- see lhat_variant.h.
    LocalVector<LhatValue> converted;
    for (int64_t i = 0; i < arguments.size(); i++) {
        LhatValue held = lhat_nil();
        if (!host::from_variant(machine, arguments[i], &held, units.godot)) {
            UtilityFunctions::push_error(host::problem(
                get_path(), String("argument ") + String::num_int64(i + 1) +
                                String(" has no shape in L^")));
            return Variant();
        }
        converted.push_back(held);
    }

    CharString name = String(member).utf8();
    LhatRunResult called = lhat_machine_call_member(
        machine, klass, name.get_data(), (size_t)name.length(),
        converted.ptr(), converted.size());
    if (called.status != LHAT_RUN_OK) {
        UtilityFunctions::push_error(
            host::problem(get_path() + String(".") + String(member),
                          lhat_run_status_message(called.status)));
        return Variant();
    }
    return host::to_variant(called.value, units.godot);
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

// What the class declares that can be called, asked of the script rather than
// of a node wearing it. The connect dialog reads it to offer the receivers a
// signal could go to, and the Node dock to say which of them exist.
//
// From the tree: 14.3 has a member declared whether or not anything ran, and
// this is asked of a script that has only been read. A member holding a p^ or
// an f^ is what a caller can reach -- program.h's LhatUnitMember counts the
// parameters a call writes, 13.4 having left self^ out of them.
TypedArray<Dictionary> LhatScript::_get_script_method_list() const
{
    TypedArray<Dictionary> out;
    // 05 の 3.2: the one method an editor script has, and the writer wrote
    // none of it -- _has_method above says the same thing.
    if (editor_script) {
        Dictionary run;
        run["name"] = StringName("_run");
        run["args"] = Array();
        run["flags"] = (int64_t)METHOD_FLAG_NORMAL;
        out.push_back(run);
        return out;
    }
    if (unit == nullptr || klass_name.is_empty()) {
        return out;
    }
    CharString klass = klass_name.utf8();
    size_t count = lhat_unit_member_count(unit, klass.get_data());
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember member = lhat_unit_member(unit, klass.get_data(), i);
        if (member.name == NULL) {
            continue;
        }
        String name = String::utf8(member.name, (int)member.name_length);
        if (!has_lhat_method(StringName(name))) {
            continue;  // a field, not something to call
        }

        Array taken;
        for (size_t at = 0; at < member.parameter_count; at++) {
            LhatUnitParameter said =
                lhat_unit_member_parameter(unit, klass.get_data(), i, at);
            Dictionary one;
            one["name"] = said.name != NULL
                              ? String::utf8(said.name, (int)said.name_length)
                              : String("arg") + String::num_int64((int64_t)at);
            one["class_name"] = StringName();
            one["type"] = Variant::NIL;
            one["hint"] = PROPERTY_HINT_NONE;
            one["hint_string"] = String();
            one["usage"] = PROPERTY_USAGE_NIL_IS_VARIANT;
            taken.push_back(one);
        }

        Dictionary info;
        info["name"] = name;
        info["args"] = taken;
        info["flags"] = METHOD_FLAG_NORMAL;
        out.push_back(info);
    }
    return out;
}

// What the class declares, asked of the script rather than of a node wearing
// it -- the inspector reads this for a .lh that is not on anything, and the
// scene's saved values are compared against it.
TypedArray<Dictionary> LhatScript::_get_script_property_list() const
{
    Array found;
    lhat_exported_properties(this, &found, nullptr);
    TypedArray<Dictionary> out;
    for (int64_t i = 0; i < found.size(); i++) {
        out.push_back(found[i]);
    }
    return out;
}

// 02 の 18: a signal is a member the writer marked @signal. The member's own
// parameters are its arguments -- one declaration, read once, so what a
// caller writes and what the engine checks an emit against cannot disagree.
bool LhatScript::signal_member(const StringName &wanted, size_t *out_at) const
{
    if (unit == nullptr) {
        return false;
    }
    CharString klass = klass_name.utf8();
    CharString name = String(wanted).utf8();
    size_t count = lhat_unit_member_count(unit, klass.get_data());
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember member = lhat_unit_member(unit, klass.get_data(), i);
        if (member.name == NULL ||
            String::utf8(member.name, (int)member.name_length) !=
                String(wanted)) {
            continue;
        }
        size_t written =
            lhat_unit_annotation_count(unit, klass.get_data(), name.get_data());
        for (size_t a = 0; a < written; a++) {
            LhatAnnotation at = lhat_unit_annotation(unit, klass.get_data(),
                                                     name.get_data(), a);
            if (String::utf8(at.name, (int)at.name_length) == "signal") {
                if (out_at != nullptr) {
                    *out_at = i;
                }
                return true;
            }
        }
        return false;  // the name is this member's, and it is not a signal
    }
    return false;
}

bool LhatScript::_has_script_signal(const StringName &signal) const
{
    return signal_member(signal, nullptr);
}

namespace {

// What Godot shows a reader in the connection dialog. A type it has no
// reading for is left a Variant rather than guessed at.
Variant::Type variant_type_of(LhatUnitTypeKind kind)
{
    switch (kind) {
        case LHAT_UNIT_TYPE_NUMBER:
            return Variant::FLOAT;
        case LHAT_UNIT_TYPE_STRING:
            return Variant::STRING;
        case LHAT_UNIT_TYPE_BOOL:
            return Variant::BOOL;
        default:
            return Variant::NIL;
    }
}

}  // namespace

TypedArray<Dictionary> LhatScript::_get_script_signal_list() const
{
    TypedArray<Dictionary> out;
    if (unit == nullptr) {
        return out;
    }
    CharString klass = klass_name.utf8();
    size_t count = lhat_unit_member_count(unit, klass.get_data());
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember member = lhat_unit_member(unit, klass.get_data(), i);
        if (member.name == NULL) {
            continue;
        }
        StringName name(String::utf8(member.name, (int)member.name_length));
        if (!signal_member(name, nullptr)) {
            continue;
        }

        // 13.4 leaves self^ out of the count, which is what makes this the
        // list a caller writes -- and the parameters carry their own names,
        // so nothing here has to be numbered.
        Array args;
        for (size_t a = 0; a < member.parameter_count; a++) {
            LhatUnitParameter param =
                lhat_unit_member_parameter(unit, klass.get_data(), i, a);
            Variant::Type type = variant_type_of(param.type);
            Dictionary argument;
            argument["name"] =
                param.name != NULL
                    ? String::utf8(param.name, (int)param.name_length)
                    : String("arg") + String::num_int64((int64_t)a);
            argument["type"] = (int64_t)type;
            argument["usage"] = type == Variant::NIL
                                    ? (int64_t)PROPERTY_USAGE_NIL_IS_VARIANT
                                    : (int64_t)PROPERTY_USAGE_DEFAULT;
            args.push_back(argument);
        }

        Dictionary declared;
        declared["name"] = name;
        declared["args"] = args;
        declared["flags"] = (int64_t)METHOD_FLAG_NORMAL;
        out.push_back(declared);
    }
    return out;
}

// 18.1 leaves what an annotation means entirely to the host, so the checker
// cannot say a body disagrees with it. This is where it is said: a member
// marked @signal that never writes its own name as a call argument is
// emitting something else, or nothing, and either way the declaration and the
// body have drifted apart.
void LhatScript::warn_about_signals() const
{
    if (unit == nullptr) {
        return;
    }
    CharString klass = klass_name.utf8();
    size_t count = lhat_unit_member_count(unit, klass.get_data());
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember member = lhat_unit_member(unit, klass.get_data(), i);
        if (member.name == NULL) {
            continue;
        }
        // 18.7改: an empty body is fill_signals' to fill, so there is no
        // body left to have drifted from anything.
        if (member.empty_body) {
            continue;
        }
        String spelt = String::utf8(member.name, (int)member.name_length);
        if (!signal_member(StringName(spelt), nullptr)) {
            continue;
        }

        bool named = false;
        size_t written =
            lhat_unit_member_written_name_count(unit, klass.get_data(), i);
        for (size_t a = 0; a < written && !named; a++) {
            LhatUnitText wrote =
                lhat_unit_member_written_name(unit, klass.get_data(), i, a);
            named = wrote.text != NULL &&
                    String::utf8(wrote.text, (int)wrote.length) == spelt;
        }
        if (!named) {
            UtilityFunctions::push_warning(
                get_path() + String(": @signal ") + spelt +
                String(" is never emitted by name in its own body"));
        }
    }
}

// What a fresh instance holds, which is what the inspector's revert arrow
// means by "back to the default". Read once at load (read_defaults); a field
// missing from it is one no instance could be made to show, and answering
// false is what keeps the arrow from offering a value nothing would give.
bool LhatScript::_has_property_default_value(const StringName &property) const
{
    return defaults.has(String(property));
}

Variant LhatScript::_get_property_default_value(
    const StringName &property) const
{
    return defaults.get(String(property), Variant());
}

// What the script editor calls once the text it holds has changed: the
// buffer is the script's now, and everything read off the tree -- 14.3's
// members, 18's annotations, 6.4's descriptions, the @export fields -- would
// otherwise still be the tree from before the edit. GDScript re-parses here
// for the same reason.
//
// _reload is the check, so a text already checked is already current and
// costs nothing. Nor is the cost loose: the editor calls this on a save, and
// otherwise only where _validate has just said the text is sound and the
// script is not a @tool one (script_text_editor.cpp) -- so nothing is
// rebuilt from a text that would not compile, and no tool script is reloaded
// under a writer mid-keystroke.
void LhatScript::_update_exports()
{
    if (!checked) {
        _reload(true);
    }
}

// 02 の 14.7改 calls a value member of a definition a static constant, and
// that is what this answers -- everything the definition holds that is
// neither its prototype nor something to call. `gdBaseClass` is one, and a
// derived class shows the one it inherited, since the table is flattened.
//
// Read when asked rather than kept: the callers are few and none is per
// frame -- the remote inspector's Constants section while a game runs, and
// Script.get_script_constant_map() from GDScript.
Dictionary LhatScript::_get_constants() const
{
    Dictionary out;
    if (!lhat_is_object_kind(klass, LHAT_OBJECT_TABLE)) {
        return out;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(klass);
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING) ||
            callable(entry->value)) {
            continue;
        }
        const LhatString *name =
            (const LhatString *)lhat_as_object(entry->key);
        String spelt = String::utf8(name->text, (int)name->length);
        if (spelt == String("self^")) {
            continue;  // 14.11's prototype, which _get_members reads instead
        }
        // A value with nowhere to land in a Variant answers nil, and a
        // constant the engine cannot show is not one worth naming.
        Variant held = host::to_variant(entry->value, units.godot);
        if (held.get_type() == Variant::NIL && !lhat_is_nil(entry->value)) {
            continue;
        }
        out[spelt] = held;
    }
    return out;
}

// The fields an instance carries, which 14.11 fixes at the prototype -- so
// the names are the ones read_defaults already read off it. What the remote
// inspector lists under Members while a game runs, reading each off the
// object by name (lhat_instance.cpp's instance_get).
//
// 14.15's abstract^ fields have no key on the prototype and so none here.
// They hold whatever `new` wrote, which is a thing to ask an instance and
// not the definition.
TypedArray<StringName> LhatScript::_get_members() const
{
    TypedArray<StringName> out;
    Array named = defaults.keys();
    for (int64_t i = 0; i < named.size(); i++) {
        out.push_back(StringName(String(named[i])));
    }
    return out;
}

Variant LhatScript::_get_rpc_config() const
{
    return Variant();
}

namespace {

// 01 の 6.4: the comment block written above one thing, as prose. Asked of
// the tree, so it is there whether or not anything ran.
String documentation_of(const LhatUnit *unit, const char *definition,
                        const char *name)
{
    if (unit == nullptr) {
        return String();
    }
    size_t needed = lhat_unit_documentation(unit, definition, name, NULL, 0);
    if (needed == 0) {
        return String();
    }
    CharString said;
    said.resize((int64_t)needed + 1);
    lhat_unit_documentation(unit, definition, name, said.ptrw(), needed + 1);
    return String::utf8(said.get_data(), (int)needed);
}

// What Godot shows where a whole description will not fit -- a class list,
// a member row. 6.4 leaves the block one piece, so the first line is what
// stands for it.
String brief_of(const String &whole)
{
    int64_t stop = whole.find("\n");
    return stop < 0 ? whole : whole.substr(0, stop);
}

}  // namespace

// 01 の 6.4: what a class and its members say about themselves, in the shape
// DocData::ClassDoc::from_dict reads. This is what the editor's help page
// shows (F1) and where the inspector finds a field's tooltip.
//
// One class: the one a node wears. 05 の 5.5 lets a unit publish more, but
// only the worn one has a name the engine can look a page up by, so a second
// entry would be filed where nothing asks.
TypedArray<Dictionary> LhatScript::_get_documentation() const
{
    TypedArray<Dictionary> out;
    StringName filed = _get_doc_class_name();
    if (unit == nullptr || String(filed).is_empty()) {
        return out;
    }
    CharString klass = klass_name.utf8();

    // What was written above the class, and failing that above the file --
    // a unit holding one class often says what it is at the head instead of
    // over the binding.
    String whole = documentation_of(unit, klass.get_data(), NULL);
    if (whole.is_empty()) {
        whole = documentation_of(unit, NULL, NULL);
    }

    Dictionary said;
    said["name"] = String(filed);
    said["inherits"] = base_class.is_empty() ? String("Object") : base_class;
    said["brief_description"] = brief_of(whole);
    said["description"] = whole;
    said["is_script_doc"] = true;
    said["script_path"] = get_path();

    // The lists are the ones the editor already gets elsewhere, with each
    // member's own block put on it. Written once here rather than walked
    // again: what a class declares is one question (14.3).
    Array methods;
    TypedArray<Dictionary> declared = _get_script_method_list();
    for (int64_t i = 0; i < declared.size(); i++) {
        Dictionary one = declared[i];
        CharString member = String(one["name"]).utf8();
        one["description"] =
            documentation_of(unit, klass.get_data(), member.get_data());
        one["return_type"] = String("Variant");
        methods.push_back(one);
    }
    said["methods"] = methods;

    Array signals;
    TypedArray<Dictionary> emitted = _get_script_signal_list();
    for (int64_t i = 0; i < emitted.size(); i++) {
        Dictionary one = emitted[i];
        CharString member = String(one["name"]).utf8();
        one["description"] =
            documentation_of(unit, klass.get_data(), member.get_data());
        signals.push_back(one);
    }
    said["signals"] = signals;

    Array shown;
    TypedArray<Dictionary> exported = _get_script_property_list();
    for (int64_t i = 0; i < exported.size(); i++) {
        Dictionary field = exported[i];
        // The list opens with the class's own category (lhat_instance.cpp),
        // which is a header and not a field.
        if ((int64_t)field.get("usage", 0) & PROPERTY_USAGE_CATEGORY) {
            continue;
        }
        String name = field["name"];
        CharString member = name.utf8();
        Dictionary one;
        one["name"] = name;
        one["type"] = Variant::get_type_name(
            (Variant::Type)(int)field.get("type", Variant::NIL));
        one["description"] =
            documentation_of(unit, klass.get_data(), member.get_data());
        if (defaults.has(name)) {
            one["default_value"] = String(defaults[name]);
        }
        shown.push_back(one);
    }
    said["properties"] = shown;

    Array held;
    Dictionary constants = _get_constants();
    Array named = constants.keys();
    for (int64_t i = 0; i < named.size(); i++) {
        String name = named[i];
        CharString member = name.utf8();
        Dictionary one;
        one["name"] = name;
        one["value"] = String(constants[name]);
        one["is_value_valid"] = true;
        one["description"] =
            documentation_of(unit, klass.get_data(), member.get_data());
        held.push_back(one);
    }
    said["constants"] = held;

    out.push_back(said);
    return out;
}

}  // namespace godot
