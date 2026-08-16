#include "lhat_script.h"

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

// 05 の 5.5: what a module^ unit answers is the table of its public^ names.
// The one that is a definition is the class this file declares -- 14.9 marks
// a table a def^ made, so no naming convention is needed to find it.
bool sole_definition(LhatValue answered, LhatValue *out, String *name)
{
    if (!lhat_is_object_kind(answered, LHAT_OBJECT_TABLE)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(answered);
    size_t found = 0;
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->value, LHAT_OBJECT_TABLE)) {
            continue;
        }
        const LhatTable *held = (const LhatTable *)lhat_as_object(entry->value);
        if (!held->is_definition) {
            continue;
        }
        found++;
        *out = entry->value;
        if (lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING)) {
            const LhatString *key =
                (const LhatString *)lhat_as_object(entry->key);
            *name = String::utf8(key->text, (int)key->length);
        }
    }
    return found == 1;
}

}  // namespace

LhatScript::~LhatScript()
{
    let_go();
}

void LhatScript::let_go()
{
    if (machine != nullptr) {
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
    runnable = false;
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
    if (!sole_definition(ran.value, &klass, &name)) {
        // Not an error: a unit that declares no class is a library, and
        // require^ is what reaches it. Only wearing it is out.
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
    return OK;
}

// 14.9's `new` is an ordinary member of the definition and takes no self^,
// so it is called through the definition the way a written `Enemy.new()` is.
bool LhatScript::make_instance(Object *owner, LhatValue *out, int64_t *id)
{
    if (!runnable) {
        return false;
    }
    LhatRunResult made =
        lhat_machine_call_member(machine, klass, "new", 3, nullptr, 0);
    if (made.status != LHAT_RUN_OK) {
        UtilityFunctions::push_error(
            host::problem(get_path(), lhat_run_status_message(made.status)));
        return false;
    }

    // The field the wrapper library declares abstract^ and a concrete class
    // fills with godot.Object.default() -- 14.11 ran that initialiser just
    // now, and this is what puts the real object in its place. A unit that
    // wants nothing to do with its node simply has no such field.
    if (owner != nullptr && lhat_is_object_kind(made.value, LHAT_OBJECT_TABLE)) {
        // Spelled once: the length is taken from the name so the two cannot
        // drift apart, which is a rename away from writing a key nothing
        // answers to and leaving the placeholder where the object goes.
        static const char OWNER_FIELD[] = "gdobj";
        LhatTable *fields = (LhatTable *)lhat_as_object(made.value);
        LhatValue key = lhat_nil();
        LhatValue handle = lhat_nil();
        if (lhat_machine_make_string(machine, OWNER_FIELD,
                                     sizeof OWNER_FIELD - 1, &key) &&
            !lhat_is_nil(lhat_table_get(fields, key)) &&
            host::make_object(machine, units.godot, owner, &handle)) {
            bool taken = false;
            lhat_table_set(fields, key, handle, &taken);
        }
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

bool LhatScript::_can_instantiate() const
{
    return runnable;
}

void *LhatScript::_instance_create(Object *for_object) const
{
    // const because the engine asks it that way; making an instance writes
    // to the table this script owns, which is what the cast is for.
    return lhat_instance_create(const_cast<LhatScript *>(this), for_object);
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

// 05 の 5.5 gives a unit no base to inherit from, so a script says nothing
// about what it may be worn by. Object is every object, which is the honest
// answer until a unit has a way to name what it is a script for.
StringName LhatScript::_get_instance_base_type() const
{
    return runnable ? StringName("Object") : StringName();
}

bool LhatScript::_is_tool() const
{
    return false;
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
