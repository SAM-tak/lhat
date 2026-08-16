#include "lhat_instance.h"

#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "lhat.h"
#include "lhat_host.h"
#include "lhat_language.h"
#include "lhat_script.h"
#include "lhat_variant.h"

namespace godot {

namespace {

// One node's worth. The instance itself lives on the machine and is rooted
// in the script's table; what is here is the pairing, and the id that undoes
// it when the node goes.
struct Instance {
    Object *owner = nullptr;
    Ref<LhatScript> script;
    LhatValue self = lhat_nil();
    int64_t id = 0;
};

Instance *of(GDExtensionScriptInstanceDataPtr data)
{
    return (Instance *)data;
}

const StringName &name_of(GDExtensionConstStringNamePtr from)
{
    return *reinterpret_cast<const StringName *>(from);
}

// ---------------------------------------------------------------------------
// Properties: an instance's self^ fields, read and written by name

GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr data,
                             GDExtensionConstStringNamePtr name,
                             GDExtensionConstVariantPtr value)
{
    Instance *it = of(data);
    LhatMachine *machine = it->script->lhat_machine();
    if (machine == nullptr || !lhat_is_object_kind(it->self, LHAT_OBJECT_TABLE)) {
        return false;
    }

    CharString spelt = String(name_of(name)).utf8();
    LhatValue key = lhat_nil();
    LhatValue held = lhat_nil();
    if (!lhat_machine_make_string(machine, spelt.get_data(),
                                  (size_t)spelt.length(), &key)) {
        return false;
    }
    LhatTable *table = (LhatTable *)lhat_as_object(it->self);
    // 14.3 fixes an instance's fields when it is made, so a name that is not
    // already there is not this instance's to take -- the engine asks about
    // every name it can think of and reads `false` as "not mine".
    if (lhat_is_nil(lhat_table_get(table, key))) {
        return false;
    }
    if (!host::from_variant(machine,
                            *reinterpret_cast<const Variant *>(value), &held,
                            it->script->godot())) {
        return false;
    }
    bool refused = false;
    return lhat_table_set(table, key, held, &refused) && !refused;
}

GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr data,
                             GDExtensionConstStringNamePtr name,
                             GDExtensionVariantPtr answer)
{
    Instance *it = of(data);
    LhatMachine *machine = it->script->lhat_machine();
    if (machine == nullptr || !lhat_is_object_kind(it->self, LHAT_OBJECT_TABLE)) {
        return false;
    }

    CharString spelt = String(name_of(name)).utf8();
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, spelt.get_data(),
                                  (size_t)spelt.length(), &key)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(it->self);
    LhatValue held = lhat_table_get(table, key);
    if (lhat_is_nil(held)) {
        return false;
    }
    *reinterpret_cast<Variant *>(answer) = host::to_variant(held, it->script->godot());
    return true;
}

// The arguments as one comma-joined string, which is the shape Godot reads a
// hint in. A name (18.3) is spelt out the same as a string: what it meant was
// never the language's to say.
String joined(LhatAnnotation at)
{
    String out;
    for (size_t i = 0; i < at.argument_count; i++) {
        LhatAnnotationArgument argument = lhat_annotation_argument(at, i);
        if (argument.text == NULL) {
            continue;
        }
        if (!out.is_empty()) {
            out += String(",");
        }
        out += String::utf8(argument.text, (int)argument.length);
    }
    return out;
}

// 02 の 18: a field the writer marked. What @export means is Godot's, and
// the language only carried it here -- so this is where the meaning is put
// back on, and nowhere else.
bool exported_as(const LhatScript *script, const String &field,
                 String *hint_string, uint32_t *hint)
{
    const LhatUnit *unit = script->lhat_unit();
    if (unit == nullptr) {
        return false;
    }
    CharString klass = script->lhat_class_name().utf8();
    CharString name = field.utf8();
    size_t written =
        lhat_unit_annotation_count(unit, klass.get_data(), name.get_data());

    for (size_t i = 0; i < written; i++) {
        LhatAnnotation at = lhat_unit_annotation(unit, klass.get_data(),
                                                 name.get_data(), i);
        String spelt = String::utf8(at.name, (int)at.name_length);
        if (spelt == "export") {
            *hint = PROPERTY_HINT_NONE;
            *hint_string = String();
            return true;
        }
        if (spelt == "export_range" && at.argument_count >= 2) {
            *hint = PROPERTY_HINT_RANGE;
            *hint_string =
                String::num(lhat_annotation_argument(at, 0).number) +
                String(",") +
                String::num(lhat_annotation_argument(at, 1).number);
            return true;
        }
        if (spelt == "export_multiline") {
            *hint = PROPERTY_HINT_MULTILINE_TEXT;
            *hint_string = String();
            return true;
        }
        // Both of these are a comma-joined list to the engine, and both take
        // their pieces as written -- 18.3 keeps an argument a literal, so
        // there is nothing to resolve on the way.
        if (spelt == "export_enum" && at.argument_count >= 1) {
            *hint = PROPERTY_HINT_ENUM;
            *hint_string = joined(at);
            return true;
        }
        if (spelt == "export_file") {
            // No filter means any file, which is what an empty hint says.
            *hint = PROPERTY_HINT_FILE;
            *hint_string = joined(at);
            return true;
        }
    }
    return false;
}

const GDExtensionPropertyInfo *instance_property_list(
    GDExtensionScriptInstanceDataPtr data, uint32_t *count)
{
    Instance *it = of(data);
    *count = 0;
    if (!lhat_is_object_kind(it->self, LHAT_OBJECT_TABLE)) {
        return nullptr;
    }

    // 14.3 fixes an instance's fields when it is made, so walking the
    // instance is walking what the writer declared -- and 18 says which of
    // them the engine is to show.
    const LhatTable *fields = (const LhatTable *)lhat_as_object(it->self);
    LocalVector<GDExtensionPropertyInfo> found;
    for (size_t i = 0; i < fields->entry_capacity; i++) {
        const LhatTableEntry *entry = &fields->entries[i];
        if (lhat_is_nil(entry->key) ||
            !lhat_is_object_kind(entry->key, LHAT_OBJECT_STRING)) {
            continue;
        }
        const LhatString *key = (const LhatString *)lhat_as_object(entry->key);
        String name = String::utf8(key->text, (int)key->length);

        String hint_string;
        uint32_t hint = PROPERTY_HINT_NONE;
        if (!exported_as(it->script.ptr(), name, &hint_string, &hint)) {
            continue;
        }

        // The type is what the field holds now: 14.11 ran its initialiser,
        // so there is always a value to read one off.
        Variant held = host::to_variant(entry->value, it->script->godot());

        GDExtensionPropertyInfo info;
        info.type = (GDExtensionVariantType)held.get_type();
        info.name = memnew(StringName(name));
        info.class_name = memnew(StringName());
        info.hint = hint;
        info.hint_string = memnew(String(hint_string));
        info.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
        found.push_back(info);
    }
    if (found.is_empty()) {
        return nullptr;
    }

    GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo,
                                               found.size());
    for (uint32_t i = 0; i < found.size(); i++) {
        list[i] = found[i];
    }
    *count = (uint32_t)found.size();
    return list;
}

void instance_free_property_list(GDExtensionScriptInstanceDataPtr data,
                                 const GDExtensionPropertyInfo *list,
                                 uint32_t count)
{
    (void)data;
    if (list == NULL) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        memdelete((StringName *)list[i].name);
        memdelete((StringName *)list[i].class_name);
        memdelete((String *)list[i].hint_string);
    }
    memdelete_arr((GDExtensionPropertyInfo *)list);
}

GDExtensionVariantType instance_property_type(
    GDExtensionScriptInstanceDataPtr data, GDExtensionConstStringNamePtr name,
    GDExtensionBool *is_valid)
{
    (void)data;
    (void)name;
    *is_valid = false;
    return GDEXTENSION_VARIANT_TYPE_NIL;
}

GDExtensionBool instance_validate_property(
    GDExtensionScriptInstanceDataPtr data, GDExtensionPropertyInfo *property)
{
    (void)data;
    (void)property;
    return false;
}

GDExtensionBool instance_property_can_revert(
    GDExtensionScriptInstanceDataPtr data, GDExtensionConstStringNamePtr name)
{
    (void)data;
    (void)name;
    return false;
}

GDExtensionBool instance_property_get_revert(
    GDExtensionScriptInstanceDataPtr data, GDExtensionConstStringNamePtr name,
    GDExtensionVariantPtr answer)
{
    (void)data;
    (void)name;
    (void)answer;
    return false;
}

void instance_property_state(GDExtensionScriptInstanceDataPtr data,
                             GDExtensionScriptInstancePropertyStateAdd add,
                             void *userdata)
{
    (void)data;
    (void)add;
    (void)userdata;
}

// ---------------------------------------------------------------------------
// Methods

const GDExtensionMethodInfo *instance_method_list(
    GDExtensionScriptInstanceDataPtr data, uint32_t *count)
{
    (void)data;
    *count = 0;
    return nullptr;
}

void instance_free_method_list(GDExtensionScriptInstanceDataPtr data,
                               const GDExtensionMethodInfo *list,
                               uint32_t count)
{
    (void)data;
    (void)list;
    (void)count;
}

// What Node reads before it turns processing on, so this is what makes a
// unit that wrote _process get one.
GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr data,
                                    GDExtensionConstStringNamePtr name)
{
    return of(data)->script->has_lhat_method(name_of(name));
}

GDExtensionInt instance_method_argument_count(
    GDExtensionScriptInstanceDataPtr data, GDExtensionConstStringNamePtr name,
    GDExtensionBool *is_valid)
{
    (void)data;
    (void)name;
    *is_valid = false;
    return 0;
}

// 02 の 14.4 の host 版: the receiver is this node's instance, and the member
// is looked up through its definition the way a written call looks it up.
void instance_call(GDExtensionScriptInstanceDataPtr data,
                   GDExtensionConstStringNamePtr method,
                   const GDExtensionConstVariantPtr *arguments,
                   GDExtensionInt count, GDExtensionVariantPtr answer,
                   GDExtensionCallError *error)
{
    Instance *it = of(data);
    LhatMachine *machine = it->script->lhat_machine();
    *reinterpret_cast<Variant *>(answer) = Variant();

    if (machine == nullptr || !it->script->has_lhat_method(name_of(method))) {
        error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }

    // Every argument is converted before anything runs: a collection happens
    // inside the interpreter loop, and what is built here is reachable only
    // once it is handed over (see lhat_variant.h).
    LocalVector<LhatValue> converted;
    for (GDExtensionInt i = 0; i < count; i++) {
        LhatValue held = lhat_nil();
        if (!host::from_variant(
                machine, *reinterpret_cast<const Variant *>(arguments[i]),
                &held, it->script->godot())) {
            error->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
            error->argument = (int32_t)i;
            error->expected = 0;
            return;
        }
        converted.push_back(held);
    }

    CharString spelt = String(name_of(method)).utf8();
    LhatRunResult ran = lhat_machine_call_member(
        machine, it->self, spelt.get_data(), (size_t)spelt.length(),
        converted.ptr(), converted.size());
    if (ran.status == LHAT_RUN_ARITY) {
        error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
        error->argument = (int32_t)count;
        error->expected = (int32_t)count;
        return;
    }
    if (ran.status != LHAT_RUN_OK) {
        // 04 の 11.6改: a panic carries what it was written with, and the
        // status alone would drop the part that says anything.
        String said = String::utf8(lhat_run_status_message(ran.status));
        if (ran.status == LHAT_RUN_PANIC) {
            said += ": " + host::text_of(ran.value);
        }
        UtilityFunctions::push_error(
            host::problem(it->script->get_path() + String(".") +
                              String(name_of(method)),
                          said));
        error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }

    error->error = GDEXTENSION_CALL_OK;
    *reinterpret_cast<Variant *>(answer) = host::to_variant(ran.value, it->script->godot());
}

// 02 の 8.8: a notification is a number the engine sends, and L^ has no form
// for one. A unit that wants NOTIFICATION_READY writes _ready, which the
// engine calls through instance_call above.
void instance_notification(GDExtensionScriptInstanceDataPtr data, int32_t what,
                           GDExtensionBool reversed)
{
    (void)data;
    (void)what;
    (void)reversed;
}

void instance_to_string(GDExtensionScriptInstanceDataPtr data,
                        GDExtensionBool *is_valid, GDExtensionStringPtr out)
{
    Instance *it = of(data);
    *reinterpret_cast<String *>(out) =
        String("[L^ ") + it->script->get_path() + String("]");
    *is_valid = true;
}

// ---------------------------------------------------------------------------
// What the instance belongs to

void instance_refcount_incremented(GDExtensionScriptInstanceDataPtr data)
{
    (void)data;
}

GDExtensionBool instance_refcount_decremented(
    GDExtensionScriptInstanceDataPtr data)
{
    (void)data;
    return true;  // nothing here holds the owner up
}

GDExtensionObjectPtr instance_owner(GDExtensionScriptInstanceDataPtr data)
{
    return of(data)->owner ? of(data)->owner->_owner : nullptr;
}

GDExtensionObjectPtr instance_script(GDExtensionScriptInstanceDataPtr data)
{
    return of(data)->script.ptr() ? of(data)->script->_owner : nullptr;
}

GDExtensionScriptLanguagePtr instance_language(
    GDExtensionScriptInstanceDataPtr data)
{
    (void)data;
    LhatLanguage *language = LhatLanguage::get_singleton();
    return language != nullptr ? language->_owner : nullptr;
}

GDExtensionBool instance_is_placeholder(GDExtensionScriptInstanceDataPtr data)
{
    (void)data;
    return false;
}

void instance_free(GDExtensionScriptInstanceDataPtr data)
{
    Instance *it = of(data);
    if (it->script.is_valid()) {
        it->script->drop_instance(it->id);
    }
    memdelete(it);
}

const GDExtensionScriptInstanceInfo3 instance_info = {
    instance_set,
    instance_get,
    instance_property_list,
    instance_free_property_list,
    nullptr,  // get_class_category: the default will do

    instance_property_can_revert,
    instance_property_get_revert,

    instance_owner,
    instance_property_state,

    instance_method_list,
    instance_free_method_list,
    instance_property_type,
    instance_validate_property,

    instance_has_method,

    instance_method_argument_count,

    instance_call,
    instance_notification,

    instance_to_string,

    instance_refcount_incremented,
    instance_refcount_decremented,

    instance_script,

    instance_is_placeholder,

    nullptr,  // set_fallback
    nullptr,  // get_fallback

    instance_language,

    instance_free,
};

}  // namespace

void *lhat_instance_create(LhatScript *script, Object *owner)
{
    if (script == nullptr) {
        return nullptr;
    }
    LhatValue self = lhat_nil();
    int64_t id = 0;
    if (!script->make_instance(owner, &self, &id)) {
        return nullptr;
    }

    Instance *it = memnew(Instance);
    it->owner = owner;
    it->script = Ref<LhatScript>(script);
    it->self = self;
    it->id = id;

    return internal::gdextension_interface_script_instance_create3(
        &instance_info, it);
}

}  // namespace godot
