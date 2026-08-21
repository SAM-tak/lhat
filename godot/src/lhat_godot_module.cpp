#include "lhat_godot_module.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "lhat_godot_handles.h"
#include "lhat_godot_packed.h"
#include "lhat_godot_values.h"
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

// 02 の 18.7改: the engine object a receiver stands for. A godot.Object
// answers for itself; anything else is an instance of a class composed onto
// Godot.Object, and 14.15's declared field is where that class keeps the
// object -- the same `self^.gdobj` a hand-written body reaches through.
Object *engine_object_of(LhatMachine *machine, LhatValue self,
                         const Godot *module)
{
    Object *direct = object_of(self, module);
    if (direct != nullptr || !lhat_is_object_kind(self, LHAT_OBJECT_TABLE)) {
        return direct;
    }
    // Made per call, the way instance_get and instance_set make theirs: a
    // key kept between calls would be a value with no root, and the
    // collector reaches nothing a host holds.
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, "gdobj", 5, &key)) {
        return nullptr;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(self);
    return object_of(lhat_table_get(table, key), module);
}

// What an empty-bodied @signal member is filled with. The same road
// godot_emit takes, with the name coming from the registration rather than
// from an argument -- which is the whole of what writing the body did.
LhatValue signal_emit(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    const SignalEmitter *emitter = (const SignalEmitter *)context;
    if (emitter == nullptr || count == 0) {
        return lhat_nil();
    }
    Object *object = engine_object_of(machine, arguments[0], emitter->module);
    if (object == nullptr) {
        return gone("emit", arguments[0], emitter->module);
    }

    // 13.4 leaves self^ out of what a caller writes, and arguments[0] is it,
    // so the signal's arguments are the rest.
    Array passed;
    passed.push_back(emitter->name);
    for (size_t i = 1; i < count; i++) {
        passed.push_back(to_variant(arguments[i], emitter->module));
    }
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

// True in the editor and false in a game, including one launched from it.
// What a @tool class asks when it wants to do one thing while a scene is
// being edited and another while it is played.
LhatValue godot_is_editor_hint(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_bool(Engine::get_singleton()->is_editor_hint());
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
        const char *excludes;
        const char *needs;
    } annotations[] = {
        // Which def^ of this unit a node wears, and when it runs. Without a
        // mark the engine would need a rule of its own -- "the one public^
        // def^" was that rule, and it made a unit publishing a second class
        // unwearable for no reason the language has.
        //
        // Two marks rather than one because there are two answers, not
        // because there are two questions: @tool is @game plus the editor.
        // A unit writes at most one of them, and a unit publishing a single
        // class need write neither -- with one candidate there is nothing to
        // choose, and running in the editor is always chosen out loud.
        //
        // Public only: the engine reaches the class through the table the
        // unit answers with (05 の 5.5), so one kept private is not something
        // a node could wear however it is marked.
        //
        // FILEUNIQUE counts each name on its own, which catches a second
        // @game and a second @tool. That the two are one choice is 18.5.1's
        // exclusion, said here from both sides -- either would do,
        // and both is how the pair reads as a pair.
        {"game", LHAT_ANNOTATION_PUBLIC | LHAT_ANNOTATION_FILEUNIQUE, nullptr,
         "tool", nullptr},
        {"tool", LHAT_ANNOTATION_PUBLIC | LHAT_ANNOTATION_FILEUNIQUE, nullptr,
         "game", nullptr},
        // 05 の 3.2's opposite number: GDScript is anonymous unless it writes
        // class_name, and so is a .lh unless it writes this. Spelt with the
        // rest of the @export family, since what it does is what they do --
        // hand something of the writer's over to the engine, where nothing
        // unmarked goes.
        //
        // What the project registers is the binding's own name; there is no
        // argument, because a name written twice is a name that can disagree
        // with itself. One per file, which is all the engine keeps per path.
        {"export_class", LHAT_ANNOTATION_PUBLIC | LHAT_ANNOTATION_FILEUNIQUE,
         nullptr, nullptr, nullptr},
        // 18.5.2: an icon hangs on a global class name, so on the mark that
        // asks for one. Its targets are that mark's for the same reason --
        // a class kept private can never have a name for an icon to hang on,
        // and one file registers one name and so wears one icon.
        {"icon", LHAT_ANNOTATION_PUBLIC | LHAT_ANNOTATION_FILEUNIQUE,
         "p^ string^;", nullptr, "export_class"},
        {"export", LHAT_ANNOTATION_FIELD, nullptr, nullptr, nullptr},
        {"export_range", LHAT_ANNOTATION_FIELD, "p^ number^, number^, ...;",
         nullptr, nullptr},
        // An enum wants at least one name; a file filter is optional and
        // there may be several, which is what a bare variadic says.
        {"export_enum", LHAT_ANNOTATION_FIELD, "p^ string^, ...:string^;",
         nullptr, nullptr},
        {"export_file", LHAT_ANNOTATION_FIELD, "p^ ...:string^;", nullptr,
         nullptr},
        {"export_multiline", LHAT_ANNOTATION_FIELD, nullptr, nullptr,
         nullptr},
        // Written on the member that emits it: what the engine has to be
        // told is a name and an argument list, and a member already is one.
        {"signal", LHAT_ANNOTATION_MEMBER, nullptr, nullptr, nullptr},
        // The words are strings and the channel is a number, so the tail
        // takes no type -- and none of them is required, since every part of
        // the configuration has a default (_get_rpc_config).
        {"rpc", LHAT_ANNOTATION_MEMBER, "p^ ...;", nullptr, nullptr},
    };
    for (const auto &annotation : annotations) {
        if (!lhat_register_annotation(program, "godot", annotation.name,
                                      annotation.targets) ||
            (annotation.signature != nullptr &&
             !lhat_register_annotation_signature(program, annotation.name,
                                                 annotation.signature)) ||
            (annotation.excludes != nullptr &&
             !lhat_register_annotation_exclusive(program, annotation.name,
                                                 annotation.excludes)) ||
            (annotation.needs != nullptr &&
             !lhat_register_annotation_requisite(program, annotation.name,
                                                 annotation.needs))) {
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

    // 02 の 18: @tool is additive -- a class marked with it runs while a scene
    // is being edited *and* when the game runs. So a writer needs to be able
    // to tell the two apart, the way GDScript's `if Engine.is_editor_hint():`
    // does. A subroutine of the module rather than a member of Object: it is
    // not about any object.
    if (!lhat_register_func(program, "godot", "isEditorHint", "f^ -> bool^;",
                            godot_is_editor_hint, module)) {
        memdelete(module);
        return nullptr;
    }

    // 05 の 8.9: the mathematical types, which are values rather than objects
    // and so nothing the one hostdata type above could stand for.
    if (!register_values(program, module)) {
        memdelete(module);
        return nullptr;
    }

    // 05 の 8.8: the two that look like values and are not, and the ten runs
    // of them that are copied on write.
    if (!register_handles(program, module) ||
        !register_packed(program, module)) {
        memdelete(module);
        return nullptr;
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

bool make_signal_emitter(LhatMachine *machine, const SignalEmitter *emitter,
                         uint8_t parameters, bool variadic, LhatValue *out)
{
    if (machine == nullptr || emitter == nullptr) {
        return false;
    }
    // 14.4: reached through an instance, so it is handed a receiver -- which
    // is what makes it stand in for the member it replaces rather than for a
    // static one. Nothing is registered under an overloaded name here, so the
    // parameter types a search would ask for are left unbuilt (14.12).
    return lhat_machine_make_host(machine, signal_emit, (void *)emitter,
                                  parameters, variadic, /*takes_self=*/true,
                                  /*self_last=*/false, nullptr, out);
}

}  // namespace host
}  // namespace godot
