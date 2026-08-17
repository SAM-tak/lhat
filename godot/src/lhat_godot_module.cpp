#include "lhat_godot_module.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "lhat_variant.h"

namespace godot {
namespace host {

namespace {

// The box a godot.Object points at. An id rather than a pointer, for the
// reason at the head of the header; the Ref is empty for anything the engine
// owns (a Node lives in the tree) and holds for a RefCounted, which nothing
// else would keep alive while L^ has it.
struct Handle {
    uint64_t id = 0;
    Ref<RefCounted> hold;
};

// Allocated once per register_godot and never freed -- program.h has no hook
// to run when its LhatProgram is disposed, which is the same trade
// stdlib/io.c takes for the same reason.
const Godot *module_of(void *context)
{
    return (const Godot *)context;
}

Object *resolve(const Handle *handle)
{
    if (handle == nullptr || handle->id == 0) {
        return nullptr;
    }
    return UtilityFunctions::instance_from_id((int64_t)handle->id);
}

Object *receiver(const LhatValue *arguments, size_t count, void *context)
{
    if (count == 0) {
        return nullptr;
    }
    const Godot *module = module_of(context);
    return object_of(arguments[0], module);
}

bool text_of(LhatValue value, String *out)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(value);
    *out = String::utf8(text->text, (int)text->length);
    return true;
}

// 04 の 11.3 spells "not there" nil^, and a freed object is the same kind of
// absence. A script that cares asks isValid^ first, the way GDScript asks
// is_instance_valid -- making every reach a try^ would cost more than it
// buys here.
LhatValue gone(const char *what, LhatValue self, const Godot *module)
{
    (void)self;
    (void)module;
    UtilityFunctions::push_error(
        String("L^: ") + String(what) +
        String(" reached an object that is not there any more"));
    return lhat_nil();
}

// ---------------------------------------------------------------------------

LhatValue godot_is_valid(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    return lhat_bool(receiver(arguments, count, context) != nullptr);
}

LhatValue godot_class_name(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    Object *object = receiver(arguments, count, context);
    if (object == nullptr) {
        return gone("className", arguments[0], module_of(context));
    }
    CharString text = object->get_class().utf8();
    LhatValue made = lhat_nil();
    lhat_machine_make_string(machine, text.get_data(), (size_t)text.length(),
                             &made);
    return made;
}

LhatValue godot_is_class(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    Object *object = receiver(arguments, count, context);
    String name;
    if (object == nullptr || count < 2 || !text_of(arguments[1], &name)) {
        return lhat_bool(false);
    }
    return lhat_bool(object->is_class(name));
}

LhatValue godot_get(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    Object *object = receiver(arguments, count, context);
    String name;
    if (object == nullptr) {
        return gone("get", arguments[0], module);
    }
    if (count < 2 || !text_of(arguments[1], &name)) {
        return lhat_nil();
    }
    LhatValue made = lhat_nil();
    from_variant(machine, object->get(name), &made, module);
    return made;
}

LhatValue godot_set(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    Object *object = receiver(arguments, count, context);
    String name;
    if (object == nullptr) {
        return gone("set", arguments[0], module);
    }
    if (count < 3 || !text_of(arguments[1], &name)) {
        return lhat_nil();
    }
    object->set(name, to_variant(arguments[2], module));
    return lhat_nil();
}

// 13.7: the signature ends in '...', so the tail arrives as the arguments
// after the name -- uncollected, which is what a C function wants.
LhatValue godot_call(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    Object *object = receiver(arguments, count, context);
    String name;
    if (object == nullptr) {
        return gone("call", arguments[0], module);
    }
    if (count < 2 || !text_of(arguments[1], &name)) {
        return lhat_nil();
    }

    Array passed;
    for (size_t i = 2; i < count; i++) {
        passed.push_back(to_variant(arguments[i], module));
    }
    LhatValue made = lhat_nil();
    from_variant(machine, object->callv(name, passed), &made, module);
    return made;
}

// The same shape as call, and separate because emitting is not calling: the
// name reaches every connection rather than one method. What a body marked
// @signal is written around.
LhatValue godot_emit(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    Object *object = receiver(arguments, count, context);
    String name;
    if (object == nullptr) {
        return gone("emit", arguments[0], module);
    }
    if (count < 2 || !text_of(arguments[1], &name)) {
        return lhat_nil();
    }

    Array passed;
    for (size_t i = 2; i < count; i++) {
        passed.push_back(to_variant(arguments[i], module));
    }
    // emit_signal is variadic where callv is not, so the arguments go through
    // as one call rather than being spread here.
    passed.push_front(name);
    object->callv("emit_signal", passed);
    return lhat_nil();
}

// 05 の 8.8: registering this is what makes the box the host's to hand over
// and L^'s to give back.
LhatValue godot_dispose(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count == 0) {
        return lhat_nil();
    }
    Handle *handle =
        (Handle *)lhat_hostdata_pointer(arguments[0],
                                        module_of(context)->object_tag);
    if (handle != nullptr) {
        memdelete(handle);
    }
    return lhat_nil();
}

}  // namespace

const Godot *register_godot(LhatProgram *program)
{
    Godot *module = memnew(Godot);
    module->object_tag =
        lhat_register_hostdata_type(program, "godot", "Object");
    if (module->object_tag == nullptr) {
        memdelete(module);
        return nullptr;
    }

    struct {
        const char *name;
        const char *signature;
        LhatHostFn call;
    } members[] = {
        // 02 の 14.15 with 14.11改: no way to make one out of nothing. Every
        // godot.Object here stands for an object the engine made, and the one
        // an override^ new is handed is where they all come from -- a value
        // meaning "no object" would be a wrapper whose isValid() is false by
        // construction, which is nothing a program has a use for.
        {"isValid", "f^self^ -> bool^;", godot_is_valid},
        {"className", "f^self^ -> string^;", godot_class_name},
        {"isClass", "f^self^, string^ -> bool^;", godot_is_class},
        {"get", "f^self^, string^ -> any^;", godot_get},
        {"set", "p^self^, string^, any^;", godot_set},
        {"call", "f^self^, string^, ... -> any^;", godot_call},
        {"emit", "p^self^, string^, ...;", godot_emit},
        {"dispose", "p^self^;", godot_dispose},
    };
    // 02 の 18: what a script may write above a declaration. The engine acts
    // on these; L^ only carries them (18.1), which is why registering them is
    // all the language needs to know.
    struct {
        const char *name;
        uint32_t targets;
        const char *signature;
    } annotations[] = {
        // Which def^ of this unit a node wears. Without it the engine would
        // need a rule of its own -- "the one public^ def^" was that rule, and
        // it made a unit publishing a second class unwearable for no reason
        // the language has.
        //
        // Public only: the engine reaches the class through the table the
        // unit answers with (05 の 5.5), so one kept private is not something
        // a node could wear however it is marked.
        {"prime", LHAT_ANNOTATION_PUBLIC, nullptr},
        {"tool", LHAT_ANNOTATION_UNIT, nullptr},
        {"icon", LHAT_ANNOTATION_BINDING, "p^ string^;"},
        {"export", LHAT_ANNOTATION_FIELD, nullptr},
        {"export_range", LHAT_ANNOTATION_FIELD, "p^ number^, number^, ...;"},
        // An enum wants at least one name; a file filter is optional and
        // there may be several, which is what a bare variadic says.
        {"export_enum", LHAT_ANNOTATION_FIELD, "p^ string^, ...:string^;"},
        {"export_file", LHAT_ANNOTATION_FIELD, "p^ ...:string^;"},
        {"export_multiline", LHAT_ANNOTATION_FIELD, nullptr},
        // Written on the member that emits it: what the engine has to be
        // told is a name and an argument list, and a member already is one.
        {"signal", LHAT_ANNOTATION_MEMBER, nullptr},
        {"rpc", LHAT_ANNOTATION_MEMBER, "p^ string^, ...;"},
    };
    for (const auto &annotation : annotations) {
        if (!lhat_register_annotation(program, "godot", annotation.name,
                                      annotation.targets,
                                      annotation.signature)) {
            memdelete(module);
            return nullptr;
        }
    }

    for (const auto &member : members) {
        if (!lhat_register_member(program, "godot", "Object", member.name,
                                  member.signature, member.call, module)) {
            memdelete(module);
            return nullptr;
        }
    }
    return module;
}

bool make_object(LhatMachine *machine, const Godot *module, Object *object,
                 LhatValue *out)
{
    if (module == nullptr) {
        return false;
    }
    Handle *handle = memnew(Handle);
    if (object != nullptr) {
        handle->id = object->get_instance_id();
        // Nothing else would keep a RefCounted alive while L^ holds it; a
        // Node belongs to the tree and is not held here on purpose.
        RefCounted *counted = Object::cast_to<RefCounted>(object);
        if (counted != nullptr) {
            handle->hold = Ref<RefCounted>(counted);
        }
    }
    if (!lhat_machine_make_hostdata(machine, module->object_tag, handle, out)) {
        memdelete(handle);
        return false;
    }
    return true;
}

Object *object_of(LhatValue value, const Godot *module)
{
    if (module == nullptr) {
        return nullptr;
    }
    return resolve(
        (const Handle *)lhat_hostdata_pointer(value, module->object_tag));
}

}  // namespace host
}  // namespace godot
