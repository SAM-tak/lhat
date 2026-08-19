#include "lhat_godot_handles.h"

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "lhat_godot_module.h"
#include "lhat_variant.h"

namespace godot {
namespace host {
namespace {

const Godot *module_of(void *context)
{
    return (const Godot *)context;
}

bool name_of(LhatValue value, String *out)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(value);
    *out = String::utf8(text->text, (int)text->length);
    return true;
}

LhatValue text_answer(LhatMachine *machine, const String &text)
{
    CharString bytes = text.utf8();
    LhatValue out = lhat_nil();
    return lhat_machine_make_string(machine, bytes.get_data(),
                                    (size_t)bytes.length(), &out)
               ? out
               : lhat_nil();
}

// ---------------------------------------------------------------------------
// Callable

const Callable *held_callable(LhatValue value, const Godot *module)
{
    return (const Callable *)lhat_hostdata_pointer(value,
                                                   module->callable_tag);
}

LhatValue callable_is_valid(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Callable *held =
        count > 0 ? held_callable(arguments[0], module_of(context)) : nullptr;
    return lhat_bool(held != nullptr && held->is_valid());
}

LhatValue callable_method(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    const Callable *held =
        count > 0 ? held_callable(arguments[0], module_of(context)) : nullptr;
    return text_answer(machine, held != nullptr ? String(held->get_method())
                                                : String());
}

LhatValue callable_object(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    const Callable *held = count > 0 ? held_callable(arguments[0], module)
                                     : nullptr;
    LhatValue out = lhat_nil();
    return make_object(machine, module,
                       held != nullptr ? held->get_object() : nullptr, &out)
               ? out
               : lhat_nil();
}

// 12.6's variadic tail: what a call writes after the receiver is what the
// Callable is handed.
LhatValue callable_call(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    const Callable *held = count > 0 ? held_callable(arguments[0], module)
                                     : nullptr;
    if (held == nullptr) {
        return lhat_nil();
    }
    Array passed;
    for (size_t i = 1; i < count; i++) {
        passed.push_back(to_variant(arguments[i], module));
    }
    LhatValue out = lhat_nil();
    return from_variant(machine, held->callv(passed), &out, module) ? out
                                                                   : lhat_nil();
}

LhatValue callable_dispose(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count == 0) {
        return lhat_nil();
    }
    Callable *held = (Callable *)lhat_hostdata_pointer(
        arguments[0], module_of(context)->callable_tag);
    if (held != nullptr) {
        memdelete(held);
    }
    return lhat_nil();
}

// f^godot.Object, string^ -> godot.Callable
LhatValue make_callable_of(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    String named;
    Object *object = count > 0 ? object_of(arguments[0], module) : nullptr;
    if (count < 2 || !name_of(arguments[1], &named)) {
        return lhat_nil();
    }
    LhatValue out = lhat_nil();
    return make_callable(machine, module, Callable(object, StringName(named)),
                         &out)
               ? out
               : lhat_nil();
}

// ---------------------------------------------------------------------------
// Signal

const Signal *held_signal(LhatValue value, const Godot *module)
{
    return (const Signal *)lhat_hostdata_pointer(value, module->signal_tag);
}

LhatValue signal_name(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    const Signal *held =
        count > 0 ? held_signal(arguments[0], module_of(context)) : nullptr;
    return text_answer(machine,
                       held != nullptr ? String(held->get_name()) : String());
}

LhatValue signal_object(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    const Signal *held = count > 0 ? held_signal(arguments[0], module)
                                   : nullptr;
    LhatValue out = lhat_nil();
    return make_object(machine, module,
                       held != nullptr ? held->get_object() : nullptr, &out)
               ? out
               : lhat_nil();
}

LhatValue signal_is_null(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Signal *held =
        count > 0 ? held_signal(arguments[0], module_of(context)) : nullptr;
    return lhat_bool(held == nullptr || held->is_null());
}

LhatValue signal_connect(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    const Signal *held = count > 0 ? held_signal(arguments[0], module)
                                   : nullptr;
    const Callable *to = count > 1 ? held_callable(arguments[1], module)
                                   : nullptr;
    if (held != nullptr && to != nullptr) {
        const_cast<Signal *>(held)->connect(*to);
    }
    return lhat_nil();
}

LhatValue signal_disconnect(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    const Signal *held = count > 0 ? held_signal(arguments[0], module)
                                   : nullptr;
    const Callable *from = count > 1 ? held_callable(arguments[1], module)
                                     : nullptr;
    if (held != nullptr && from != nullptr) {
        const_cast<Signal *>(held)->disconnect(*from);
    }
    return lhat_nil();
}

LhatValue signal_is_connected(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    const Signal *held = count > 0 ? held_signal(arguments[0], module)
                                   : nullptr;
    const Callable *to = count > 1 ? held_callable(arguments[1], module)
                                   : nullptr;
    return lhat_bool(held != nullptr && to != nullptr &&
                     held->is_connected(*to));
}

// A signal is emitted through the object that carries it -- Signal::emit is a
// template over C++ arguments and has no run-time arity, and emit_signal
// takes the name and the rest.
LhatValue signal_emit(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    const Signal *held = count > 0 ? held_signal(arguments[0], module)
                                   : nullptr;
    Object *object = held != nullptr ? held->get_object() : nullptr;
    if (object == nullptr) {
        return lhat_nil();
    }
    Array passed;
    passed.push_back(held->get_name());
    for (size_t i = 1; i < count; i++) {
        passed.push_back(to_variant(arguments[i], module));
    }
    object->callv("emit_signal", passed);
    return lhat_nil();
}

LhatValue signal_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count == 0) {
        return lhat_nil();
    }
    Signal *held = (Signal *)lhat_hostdata_pointer(
        arguments[0], module_of(context)->signal_tag);
    if (held != nullptr) {
        memdelete(held);
    }
    return lhat_nil();
}

// f^godot.Object, string^ -> godot.Signal
LhatValue make_signal_of(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    String named;
    Object *object = count > 0 ? object_of(arguments[0], module) : nullptr;
    if (object == nullptr || count < 2 || !name_of(arguments[1], &named)) {
        return lhat_nil();
    }
    LhatValue out = lhat_nil();
    return make_signal(machine, module, Signal(object, StringName(named)),
                       &out)
               ? out
               : lhat_nil();
}

}  // namespace

bool make_callable(LhatMachine *machine, const Godot *module,
                   const Callable &callable, LhatValue *out)
{
    if (module == nullptr) {
        return false;
    }
    Callable *held = memnew(Callable(callable));
    if (!lhat_machine_make_hostdata(machine, module->callable_tag, held, out)) {
        memdelete(held);
        return false;
    }
    return true;
}

bool make_signal(LhatMachine *machine, const Godot *module,
                 const Signal &signal, LhatValue *out)
{
    if (module == nullptr) {
        return false;
    }
    Signal *held = memnew(Signal(signal));
    if (!lhat_machine_make_hostdata(machine, module->signal_tag, held, out)) {
        memdelete(held);
        return false;
    }
    return true;
}

const Callable *callable_of(LhatValue value, const Godot *module)
{
    return module != nullptr ? held_callable(value, module) : nullptr;
}

const Signal *signal_of(LhatValue value, const Godot *module)
{
    return module != nullptr ? held_signal(value, module) : nullptr;
}

bool register_handles(LhatProgram *program, Godot *module)
{
    if (program == nullptr || module == nullptr) {
        return false;
    }
    module->callable_tag =
        lhat_register_hostdata_type(program, "godot", "Callable");
    module->signal_tag =
        lhat_register_hostdata_type(program, "godot", "Signal");
    if (module->callable_tag == nullptr || module->signal_tag == nullptr) {
        return false;
    }

    struct {
        const char *type;
        const char *name;
        const char *signature;
        LhatHostFn call;
    } members[] = {
        {"Callable", "isValid", "f^self^ -> bool^;", callable_is_valid},
        {"Callable", "getMethod", "f^self^ -> string^;", callable_method},
        {"Callable", "getObject", "f^self^ -> godot.Object;", callable_object},
        {"Callable", "call", "f^self^, ... -> any^;", callable_call},
        {"Callable", "dispose", "p^self^;", callable_dispose},

        {"Signal", "getName", "f^self^ -> string^;", signal_name},
        {"Signal", "getObject", "f^self^ -> godot.Object;", signal_object},
        {"Signal", "isNull", "f^self^ -> bool^;", signal_is_null},
        {"Signal", "connect", "p^self^, godot.Callable;", signal_connect},
        {"Signal", "disconnect", "p^self^, godot.Callable;",
         signal_disconnect},
        {"Signal", "isConnected", "f^self^, godot.Callable -> bool^;",
         signal_is_connected},
        {"Signal", "emit", "p^self^, ...;", signal_emit},
        {"Signal", "dispose", "p^self^;", signal_dispose},
    };
    for (const auto &member : members) {
        if (!lhat_register_member(program, "godot", member.type, member.name,
                                  member.signature, member.call, module)) {
            return false;
        }
    }

    // 14.15 with 8.8: neither is made out of nothing. A Callable names a
    // method on an object and a Signal one the object emits, so both are
    // made from an object and a name and there is no other way to have one.
    return lhat_register_func(program, "godot", "callable",
                              "f^godot.Object, string^ -> godot.Callable;",
                              make_callable_of, module) &&
           lhat_register_func(program, "godot", "signal",
                              "f^godot.Object, string^ -> godot.Signal;",
                              make_signal_of, module);
}

}  // namespace host
}  // namespace godot
