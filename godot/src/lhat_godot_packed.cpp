#include "lhat_godot_packed.h"

#include <type_traits>

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include "lhat_godot_module.h"
#include "lhat_godot_values.h"

namespace godot {
namespace host {
namespace {

const Godot *module_of(void *context)
{
    return (const Godot *)context;
}

// A registered name is borrowed rather than copied (lhat_type_add_member),
// and these are built -- so the module keeps them, as it does the value
// types' (lhat_godot_values.cpp says the same thing at more length).
const char *kept(Godot *module, const String &text)
{
    module->texts.push_back(text.utf8());
    return module->texts.back()->get().get_data();
}

// ---------------------------------------------------------------------------
// What each of the ten is called, and what its elements are

template <typename P>
struct Packed;

#define LHAT_GODOT_PACKED(P_, K_, E_, W_)                    \
    template <>                                              \
    struct Packed<P_> {                                      \
        using Element = E_;                                  \
        static constexpr Variant::Type kind = K_;            \
        static const char *spelling() { return #P_; }        \
        static const char *written() { return W_; }          \
    }

LHAT_GODOT_PACKED(PackedByteArray, Variant::PACKED_BYTE_ARRAY, int64_t,
                  "number^");
LHAT_GODOT_PACKED(PackedInt32Array, Variant::PACKED_INT32_ARRAY, int32_t,
                  "number^");
LHAT_GODOT_PACKED(PackedInt64Array, Variant::PACKED_INT64_ARRAY, int64_t,
                  "number^");
LHAT_GODOT_PACKED(PackedFloat32Array, Variant::PACKED_FLOAT32_ARRAY, float,
                  "number^");
LHAT_GODOT_PACKED(PackedFloat64Array, Variant::PACKED_FLOAT64_ARRAY, double,
                  "number^");
LHAT_GODOT_PACKED(PackedStringArray, Variant::PACKED_STRING_ARRAY, String,
                  "string^");
LHAT_GODOT_PACKED(PackedVector2Array, Variant::PACKED_VECTOR2_ARRAY, Vector2,
                  "godot.Vector2");
LHAT_GODOT_PACKED(PackedVector3Array, Variant::PACKED_VECTOR3_ARRAY, Vector3,
                  "godot.Vector3");
LHAT_GODOT_PACKED(PackedVector4Array, Variant::PACKED_VECTOR4_ARRAY, Vector4,
                  "godot.Vector4");
LHAT_GODOT_PACKED(PackedColorArray, Variant::PACKED_COLOR_ARRAY, Color,
                  "godot.Color");

#undef LHAT_GODOT_PACKED

// ---------------------------------------------------------------------------
// One element across the seam. A number and a string are ordinary L^ values;
// the other four are 8.9's, and lhat_godot_values.h is where those cross.

// 02 の 10.3: number^ is one type with two representations, and which one a
// value has is what it was made as. A run of bytes holds integers and a run
// of floats holds reals, so each answers as itself.
template <typename E>
LhatValue element_out(LhatMachine *machine, const Godot *module, const E &value)
{
    (void)machine;
    (void)module;
    if constexpr (std::is_integral_v<E>) {
        return lhat_integer((int64_t)value);
    } else {
        return lhat_real((double)value);
    }
}

template <>
LhatValue element_out<String>(LhatMachine *machine, const Godot *module,
                              const String &value)
{
    (void)module;
    CharString bytes = value.utf8();
    LhatValue out = lhat_nil();
    return lhat_machine_make_string(machine, bytes.get_data(),
                                    (size_t)bytes.length(), &out)
               ? out
               : lhat_nil();
}

template <>
LhatValue element_out<Vector2>(LhatMachine *machine, const Godot *module,
                               const Vector2 &value)
{
    return value_answer(machine, module, value);
}
template <>
LhatValue element_out<Vector3>(LhatMachine *machine, const Godot *module,
                               const Vector3 &value)
{
    return value_answer(machine, module, value);
}
template <>
LhatValue element_out<Vector4>(LhatMachine *machine, const Godot *module,
                               const Vector4 &value)
{
    return value_answer(machine, module, value);
}
template <>
LhatValue element_out<Color>(LhatMachine *machine, const Godot *module,
                             const Color &value)
{
    return value_answer(machine, module, value);
}

template <typename E>
bool element_in(LhatValue held, const Godot *module, E *out)
{
    (void)module;
    if (lhat_is_integer(held)) {
        *out = (E)lhat_as_integer(held);
        return true;
    }
    if (lhat_is_real(held)) {
        *out = (E)lhat_as_real(held);
        return true;
    }
    return false;
}

template <>
bool element_in<String>(LhatValue held, const Godot *module, String *out)
{
    (void)module;
    if (!lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(held);
    *out = String::utf8(text->text, (int)text->length);
    return true;
}

template <>
bool element_in<Vector2>(LhatValue held, const Godot *module, Vector2 *out)
{
    return value_taken(held, module, out);
}
template <>
bool element_in<Vector3>(LhatValue held, const Godot *module, Vector3 *out)
{
    return value_taken(held, module, out);
}
template <>
bool element_in<Vector4>(LhatValue held, const Godot *module, Vector4 *out)
{
    return value_taken(held, module, out);
}
template <>
bool element_in<Color>(LhatValue held, const Godot *module, Color *out)
{
    return value_taken(held, module, out);
}

// ---------------------------------------------------------------------------
// The members, written once and registered ten times

template <typename P>
P *held_packed(LhatValue value, const Godot *module)
{
    return (P *)lhat_hostdata_pointer(value, module->packed_tags[Packed<P>::kind]);
}

template <typename P>
LhatValue packed_size(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    (void)machine;
    const P *held = count > 0 ? held_packed<P>(arguments[0], module_of(context))
                              : nullptr;
    return lhat_integer(held != nullptr ? held->size() : 0);
}

// 04 の 11.3: an index that is not there answers nothing rather than
// stopping -- the same reading a table gives one.
template <typename P>
LhatValue packed_at(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count)
{
    using E = typename Packed<P>::Element;
    const Godot *module = module_of(context);
    const P *held = count > 0 ? held_packed<P>(arguments[0], module) : nullptr;
    int64_t at = count > 1 && lhat_is_integer(arguments[1])
                     ? lhat_as_integer(arguments[1])
                     : 0;
    if (held == nullptr || at < 1 || at > held->size()) {
        return element_out<E>(machine, module, E());
    }
    // 02 の 14: a sequence is written from 1, so that is what is read here.
    return element_out<E>(machine, module, (*held)[at - 1]);
}

template <typename P>
LhatValue packed_set(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    using E = typename Packed<P>::Element;
    (void)machine;
    const Godot *module = module_of(context);
    P *held = count > 0 ? held_packed<P>(arguments[0], module) : nullptr;
    E value;
    int64_t at = count > 1 && lhat_is_integer(arguments[1])
                     ? lhat_as_integer(arguments[1])
                     : 0;
    if (held == nullptr || count < 3 || !element_in<E>(arguments[2], module, &value) ||
        at < 1 || at > held->size()) {
        return lhat_nil();
    }
    held->set(at - 1, value);
    return lhat_nil();
}

template <typename P>
LhatValue packed_append(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    using E = typename Packed<P>::Element;
    (void)machine;
    const Godot *module = module_of(context);
    P *held = count > 0 ? held_packed<P>(arguments[0], module) : nullptr;
    E value;
    if (held == nullptr || count < 2 ||
        !element_in<E>(arguments[1], module, &value)) {
        return lhat_nil();
    }
    held->push_back(value);
    return lhat_nil();
}

template <typename P>
LhatValue packed_clear(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count)
{
    (void)machine;
    P *held = count > 0 ? held_packed<P>(arguments[0], module_of(context))
                        : nullptr;
    if (held != nullptr) {
        held->clear();
    }
    return lhat_nil();
}

template <typename P>
LhatValue packed_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    P *held = count > 0 ? held_packed<P>(arguments[0], module_of(context))
                        : nullptr;
    if (held != nullptr) {
        memdelete(held);
    }
    return lhat_nil();
}

// 02 の 16.3 with 05 の 8.8: the walk `for^ x in^ a` runs. One walk of one
// array, made per iterate() call and freed by the release; the array rides
// as the coroutine's `held`, which is what keeps the hostdata alive for the
// walk's whole life. The cursor re-reads the array each step, so an append
// mid-walk is seen -- and a dispose mid-walk is the writer's own 10.7
// error, exactly as it is at at().
template <typename P>
struct PackedWalk {
    const Godot *module;
    LhatValue over;  // the hostdata, re-read via held_packed each step
    int64_t at;      // 1-origin, the next element to hand over
};

template <typename P>
bool packed_step(LhatMachine *machine, void *context, LhatValue sent,
                 LhatValue *out)
{
    using E = typename Packed<P>::Element;
    (void)sent;  // the loops send nothing in
    PackedWalk<P> *walk = (PackedWalk<P> *)context;
    const P *held = held_packed<P>(walk->over, walk->module);
    if (held == nullptr || walk->at > held->size()) {
        return false;
    }
    // The four value-element arrays answer 8.9's pointer form here, and the
    // machine writes it out whole into the focus -- the same crossing a
    // host call's answer makes.
    *out = element_out<E>(machine, walk->module, (*held)[walk->at - 1]);
    walk->at++;
    return true;
}

// Under the dispose^ contract: once, and never reaching into the L^ API --
// the sweep may be the caller.
template <typename P>
LhatValue packed_walk_release(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    memdelete((PackedWalk<P> *)context);
    return lhat_nil();
}

template <typename P>
LhatValue packed_iterate(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    const P *held =
        count > 0 ? held_packed<P>(arguments[0], module) : nullptr;
    if (held == nullptr) {
        return lhat_nil();
    }
    PackedWalk<P> *walk = memnew(PackedWalk<P>);
    walk->module = module;
    walk->over = arguments[0];
    walk->at = 1;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_coroutine(machine, packed_step<P>, walk,
                                     packed_walk_release<P>, arguments[0],
                                     &out)) {
        memdelete(walk);
        return lhat_nil();
    }
    return out;
}

template <typename P>
bool answer_packed(LhatMachine *machine, const Godot *module, const P &from,
                   LhatValue *out)
{
    P *held = memnew(P(from));
    if (!lhat_machine_make_hostdata(machine, module->packed_tags[Packed<P>::kind],
                                    held, out)) {
        memdelete(held);
        return false;
    }
    return true;
}

// f^ -> godot.Packed*Array: an empty one, which is the only one there is to
// make. What the engine already holds arrives through get() instead.
template <typename P>
LhatValue make_packed_of(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)arguments;
    (void)count;
    const Godot *module = module_of(context);
    LhatValue out = lhat_nil();
    return answer_packed(machine, module, P(), &out) ? out : lhat_nil();
}

template <typename P>
bool declare_packed(LhatProgram *program, Godot *module, const char *maker)
{
    const char *name = Packed<P>::spelling();
    const char *element = Packed<P>::written();
    const LhatHostDataTag *tag =
        lhat_register_hostdata_type(program, "godot", name);
    module->packed_tags[Packed<P>::kind] = tag;
    if (tag == nullptr) {
        return false;
    }

    struct {
        const char *name;
        const char *signature;
        LhatHostFn call;
    } members[] = {
        {"size", kept(module, "f^self^ -> number^;"), packed_size<P>},
        {"at", kept(module, String("f^self^, number^ -> ") + element + ";"),
         packed_at<P>},
        {"set", kept(module, String("p^self^, number^, ") + element + ";"),
         packed_set<P>},
        {"append", kept(module, String("p^self^, ") + element + ";"),
         packed_append<P>},
        {"clear", kept(module, "p^self^;"), packed_clear<P>},
        {"dispose", kept(module, "p^self^;"), packed_dispose<P>},
        // Bare `iterate`: on a host type the two spellings name one member
        // (14.17改), and every name here is the library's.
        {"iterate",
         kept(module, String("f^self^ -> c^{p^ -> ") + element + ";, };"),
         packed_iterate<P>},
    };
    for (const auto &member : members) {
        if (!lhat_register_member(program, "godot", name, member.name,
                                  member.signature, member.call, module)) {
            return false;
        }
    }
    return lhat_register_func(
        program, "godot", maker,
        kept(module, String("f^ -> godot.") + name + ";"), make_packed_of<P>,
        module);
}

}  // namespace

bool register_packed(LhatProgram *program, Godot *module)
{
    if (program == nullptr || module == nullptr) {
        return false;
    }
    return declare_packed<PackedByteArray>(program, module,
                                           "packedByteArray") &&
           declare_packed<PackedInt32Array>(program, module,
                                            "packedInt32Array") &&
           declare_packed<PackedInt64Array>(program, module,
                                            "packedInt64Array") &&
           declare_packed<PackedFloat32Array>(program, module,
                                              "packedFloat32Array") &&
           declare_packed<PackedFloat64Array>(program, module,
                                              "packedFloat64Array") &&
           declare_packed<PackedStringArray>(program, module,
                                             "packedStringArray") &&
           declare_packed<PackedVector2Array>(program, module,
                                              "packedVector2Array") &&
           declare_packed<PackedVector3Array>(program, module,
                                              "packedVector3Array") &&
           declare_packed<PackedVector4Array>(program, module,
                                              "packedVector4Array") &&
           declare_packed<PackedColorArray>(program, module,
                                            "packedColorArray");
}

bool make_packed(LhatMachine *machine, const Godot *module,
                 const Variant &held, LhatValue *out)
{
    if (module == nullptr) {
        return false;
    }
    switch (held.get_type()) {
#define LHAT_GODOT_MAKE(P_)                                              \
    case Packed<P_>::kind:                                               \
        return answer_packed<P_>(machine, module, (P_)held, out)
        LHAT_GODOT_MAKE(PackedByteArray);
        LHAT_GODOT_MAKE(PackedInt32Array);
        LHAT_GODOT_MAKE(PackedInt64Array);
        LHAT_GODOT_MAKE(PackedFloat32Array);
        LHAT_GODOT_MAKE(PackedFloat64Array);
        LHAT_GODOT_MAKE(PackedStringArray);
        LHAT_GODOT_MAKE(PackedVector2Array);
        LHAT_GODOT_MAKE(PackedVector3Array);
        LHAT_GODOT_MAKE(PackedVector4Array);
        LHAT_GODOT_MAKE(PackedColorArray);
#undef LHAT_GODOT_MAKE
        default:
            return false;
    }
}

Variant packed_variant(LhatValue value, const Godot *module, bool *found)
{
    *found = false;
    if (module == nullptr) {
        return Variant();
    }
#define LHAT_GODOT_READ(P_)                                              \
    do {                                                                 \
        const P_ *held = held_packed<P_>(value, module);                 \
        if (held != nullptr) {                                           \
            *found = true;                                               \
            return *held;                                                \
        }                                                                \
    } while (0)
    LHAT_GODOT_READ(PackedByteArray);
    LHAT_GODOT_READ(PackedInt32Array);
    LHAT_GODOT_READ(PackedInt64Array);
    LHAT_GODOT_READ(PackedFloat32Array);
    LHAT_GODOT_READ(PackedFloat64Array);
    LHAT_GODOT_READ(PackedStringArray);
    LHAT_GODOT_READ(PackedVector2Array);
    LHAT_GODOT_READ(PackedVector3Array);
    LHAT_GODOT_READ(PackedVector4Array);
    LHAT_GODOT_READ(PackedColorArray);
#undef LHAT_GODOT_READ
    return Variant();
}

}  // namespace host
}  // namespace godot
