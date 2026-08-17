#include "lhat_script.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "lhat.h"
#include "lhat_godot_module.h"
#include "lhat_host.h"
#include "lhat_instance.h"
#include "lhat_language.h"

namespace godot {

void LhatScript::_bind_methods()
{
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
        // Counted together: they are two answers to one question, so a second
        // of either is a second answer.
        if (as_tool || marked_with(unit, spelt, "game")) {
            marks++;
            if (marks > 1) {
                // In practice this is @game and @tool together: 18.5's
                // FILEUNIQUE counts each name on its own, so the checker has
                // already refused a second of either where it stands. What
                // is left is the pair, which no flag on one of them can see.
                *said = String(
                    "@game and @tool are two answers to one question: a node "
                    "wears one class, so write whichever of them says when "
                    "it runs and not both");
                return false;
            }
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

LhatScript::~LhatScript()
{
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
}

Error LhatScript::_reload(bool keep_state)
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
    warn_about_signals();
    return OK;
}

// 14.9's `new` is an ordinary member of the definition and takes no self^,
// so it is called through the definition the way a written `Enemy.new()` is.
//
// The node is handed to it rather than written in afterwards. The engine
// builds the Object first and only then asks the script for an instance
// (_instance_create takes it), so the real thing is in hand before anything
// runs -- and 14.11 runs the self^ initialisers inside `new`, which is
// exactly where a constructor that wants its node would look. Filling the
// field on the way out would leave every one of them reading a placeholder.
//
// A unit that wears a node without wrapping one keeps 14.11's default `new`
// and takes nothing, so the call is tried both ways.
bool LhatScript::make_instance(Object *owner, LhatValue *out, int64_t *id)
{
    if (!runnable) {
        return false;
    }

    LhatValue handle = lhat_nil();
    size_t count = 0;
    if (owner != nullptr &&
        host::make_object(machine, units.godot, owner, &handle)) {
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
        UtilityFunctions::push_error(
            host::problem(get_path(), lhat_run_status_message(made.status)));
        return false;
    }

    LhatTable *table = (LhatTable *)lhat_as_object(instances);
    bool refused = false;
    if (!lhat_table_set(table, lhat_integer(next_id), made.value, &refused) ||
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
    lhat_table_set(table, lhat_integer(id), lhat_nil(), &refused);
}

bool LhatScript::has_lhat_method(const StringName &method) const
{
    if (!runnable) {
        return false;
    }
    CharString name = String(method).utf8();
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, name.get_data(),
                                  (size_t)name.length(), &key)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(klass);
    return lhat_is_object_kind(lhat_table_get(table, key),
                               LHAT_OBJECT_SUBROUTINE);
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

// 05 の 5.5 gives a unit no base to inherit from, so a script says nothing
// about what it may be worn by. Object is every object, which is the honest
// answer until a unit has a way to name what it is a script for.
StringName LhatScript::_get_instance_base_type() const
{
    return runnable ? StringName("Object") : StringName();
}

StringName LhatScript::_get_doc_class_name() const
{
    return StringName();
}

String LhatScript::_get_class_icon_path() const
{
    return String();
}

int32_t LhatScript::_get_member_line(const StringName &member) const
{
    (void)member;
    return -1;
}

// 02 の 18: @tool on the class a node wears. Additive rather than exclusive,
// the way Unity's [ExecuteAlways] is -- a @tool class runs while the scene is
// being edited *and* when the game runs. @game is the same class without the
// first half, and is what an unmarked one is.
bool LhatScript::_is_tool() const
{
    return tool_script;
}

bool LhatScript::_is_abstract() const
{
    return false;
}

// What Node reads before it turns processing on for a script -- so a unit
// that declares _process gets one, and one that does not costs nothing.
bool LhatScript::_has_method(const StringName &method) const
{
    return has_lhat_method(method);
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
