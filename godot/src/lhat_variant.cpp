#include "lhat_variant.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "lhat_godot_handles.h"
#include "lhat_godot_packed.h"
#include "lhat_godot_values.h"
#include "lhat_godot_module.h"

namespace godot {
namespace host {

namespace {

// 02 の 14 lets a table hold itself, so something has to stop the walk. The
// same reasoning LHAT_TYPE_WRITE_DEPTH is there for, and about as deep as a
// value worth handing to the engine ever gets.
const int MAX_DEPTH = 32;

Variant to_variant_at(LhatValue value, const Godot *module, int depth);
bool from_variant_at(LhatMachine *machine, const Variant &value,
                     const Godot *module, int depth, LhatValue *out);

// 02 の 14: one structure serves as both a sequence and a mapping, and Godot
// has two. A table whose keys are exactly 1..n is the sequence -- which
// makes an empty table an empty Array, since it satisfies that with n = 0.
bool is_sequence(const LhatTable *table)
{
    return lhat_table_length(table) == lhat_table_count(table);
}

Variant table_to_variant(const LhatTable *table, const Godot *module,
                         int depth)
{
    size_t length = lhat_table_length(table);
    if (is_sequence(table)) {
        Array out;
        for (size_t i = 1; i <= length; i++) {
            out.push_back(
                to_variant_at(lhat_table_get(table, lhat_integer((int64_t)i)),
                              module, depth + 1));
        }
        return out;
    }

    Dictionary out;
    for (size_t i = 0; i < table->array_count; i++) {
        LhatValue held = lhat_slots_get(table->array, i);
        out[(int64_t)(i + 1)] = to_variant_at(held, module, depth + 1);
    }
    // An entry is live when its key is not nil^ -- free and tombstone alike
    // carry a nil^ key (object.h, at LhatTable).
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;
        }
        out[to_variant_at(entry->key, module, depth + 1)] =
            to_variant_at(entry->value, module, depth + 1);
    }
    return out;
}

Variant to_variant_at(LhatValue value, const Godot *module, int depth)
{
    if (depth > MAX_DEPTH) {
        return Variant();
    }
    if (lhat_is_bool(value)) {
        return lhat_as_bool(value);
    }
    if (lhat_is_integer(value)) {
        return lhat_as_integer(value);
    }
    if (lhat_is_real(value)) {
        return lhat_as_real(value);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *text = (const LhatString *)lhat_as_object(value);
        return String::utf8(text->text, (int)text->length);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return table_to_variant((const LhatTable *)lhat_as_object(value),
                                module, depth);
    }
    // 05 の 8.8: a handle crosses back as what it stands for, when there is a
    // registration to read it against.
    Object *object = object_of(value, module);
    if (object != nullptr) {
        return object;
    }
    const Callable *callable = callable_of(value, module);
    if (callable != nullptr) {
        return *callable;
    }
    const Signal *signal = signal_of(value, module);
    if (signal != nullptr) {
        return *signal;
    }
    bool packed = false;
    Variant run = packed_variant(value, module, &packed);
    if (packed) {
        return run;
    }
    // 05 の 8.9: a value type reaches anything that outlives a frame in its
    // box, so a box is what arrives wherever the engine is handed one.
    bool boxed = false;
    Variant held = boxed_variant(value, module, &boxed);
    if (boxed) {
        return held;
    }
    // nil^, and everything with nowhere to land.
    return Variant();
}

bool make_table_from(LhatMachine *machine, const Array &items,
                     const Godot *module, int depth, LhatValue *out)
{
    if (!lhat_machine_make_table(machine, out)) {
        return false;
    }
    LhatTable *table = (LhatTable *)lhat_as_object(*out);
    for (int i = 0; i < items.size(); i++) {
        LhatValue held = lhat_nil();
        if (!from_variant_at(machine, items[i], module, depth + 1, &held)) {
            return false;
        }
        // 02 の 14.1: the keys of a sequence start at one.
        bool refused = false;
        if (!lhat_table_set(table, lhat_integer((int64_t)i + 1), held,
                            &refused) ||
            refused) {
            return false;
        }
    }
    return true;
}

bool make_table_from(LhatMachine *machine, const Dictionary &pairs,
                     const Godot *module, int depth, LhatValue *out)
{
    if (!lhat_machine_make_table(machine, out)) {
        return false;
    }
    LhatTable *table = (LhatTable *)lhat_as_object(*out);
    Array keys = pairs.keys();
    for (int i = 0; i < keys.size(); i++) {
        LhatValue key = lhat_nil();
        LhatValue held = lhat_nil();
        if (!from_variant_at(machine, keys[i], module, depth + 1, &key) ||
            !from_variant_at(machine, pairs[keys[i]], module, depth + 1,
                             &held)) {
            return false;
        }
        // 04 の 11.3: nil^ means "not there", so it cannot also be a key.
        bool refused = false;
        if (!lhat_table_set(table, key, held, &refused) || refused) {
            return false;
        }
    }
    return true;
}

bool from_variant_at(LhatMachine *machine, const Variant &value,
                     const Godot *module, int depth, LhatValue *out)
{
    if (depth > MAX_DEPTH) {
        return false;
    }
    switch (value.get_type()) {
        case Variant::NIL:
            *out = lhat_nil();
            return true;
        case Variant::BOOL:
            *out = lhat_bool((bool)value);
            return true;
        case Variant::INT:
            *out = lhat_integer((int64_t)value);
            return true;
        case Variant::FLOAT:
            *out = lhat_real((double)value);
            return true;
        case Variant::STRING:
        case Variant::STRING_NAME:
        case Variant::NODE_PATH: {
            CharString text = String(value).utf8();
            return lhat_machine_make_string(machine, text.get_data(),
                                            (size_t)text.length(), out);
        }
        case Variant::ARRAY:
            return make_table_from(machine, (Array)value, module, depth, out);
        case Variant::DICTIONARY:
            return make_table_from(machine, (Dictionary)value, module, depth,
                                   out);
        case Variant::OBJECT:
            // 05 の 8.8: one host type carries every engine object, and the
            // hierarchy is written in L^ instead (lhat_godot_module.h).
            return make_object(machine, module, (Object *)value, out);
        // Both hold references rather than bytes, so both are 8.8's data
        // rather than 8.9's values (lhat_godot_handles.h).
        case Variant::CALLABLE:
            return make_callable(machine, module, (Callable)value, out);
        case Variant::SIGNAL:
            return make_signal(machine, module, (Signal)value, out);
        // 8.8 again: copied on write, counted by reference, and four of the
        // ten hold values a table could not (lhat_godot_packed.h).
        case Variant::PACKED_BYTE_ARRAY:
        case Variant::PACKED_INT32_ARRAY:
        case Variant::PACKED_INT64_ARRAY:
        case Variant::PACKED_FLOAT32_ARRAY:
        case Variant::PACKED_FLOAT64_ARRAY:
        case Variant::PACKED_STRING_ARRAY:
        case Variant::PACKED_VECTOR2_ARRAY:
        case Variant::PACKED_VECTOR3_ARRAY:
        case Variant::PACKED_VECTOR4_ARRAY:
        case Variant::PACKED_COLOR_ARRAY:
            return make_packed(machine, module, value, out);
        default:
            // 05 の 8.8 and 8.9 are where an Object and a Vector2 will land,
            // once there is a registered type for each to be.
            return false;
    }
}

}  // namespace

Variant to_variant(LhatValue value, const Godot *module)
{
    return to_variant_at(value, module, 0);
}

bool from_variant(LhatMachine *machine, const Variant &value,
                  LhatValue *out, const Godot *module)
{
    return from_variant_at(machine, value, module, 0, out);
}

}  // namespace host
}  // namespace godot
