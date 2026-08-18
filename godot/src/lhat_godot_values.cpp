#include "lhat_godot_values.h"

#include <string.h>

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <godot_cpp/variant/vector4i.hpp>

#include "lhat_godot_module.h"

namespace godot {
namespace host {
namespace {

// 8.9 reads a field straight out of the bytes, so the kind has to be the one
// the engine was built with. real_t is a float unless the build says
// otherwise, and the integer vectors are 32-bit whatever it says.
#ifdef REAL_T_IS_DOUBLE
const LhatHostValueFieldKind REAL_FIELD = LHAT_HVFIELD_F64;
#else
const LhatHostValueFieldKind REAL_FIELD = LHAT_HVFIELD_F32;
#endif
const LhatHostValueFieldKind INT_FIELD = LHAT_HVFIELD_I32;

// A colour's channels are floats however the engine was built -- Color does
// not follow real_t.
const LhatHostValueFieldKind COLOR_FIELD = LHAT_HVFIELD_F32;

// Reading a field out of the bytes is what 8.9 is for, and it only works
// where the members really are where offsetof says. Said here rather than
// found out at run time.
static_assert(sizeof(Vector2) == 2 * sizeof(real_t), "Vector2 is two reals");
static_assert(sizeof(Vector3) == 3 * sizeof(real_t), "Vector3 is three");
static_assert(sizeof(Vector4) == 4 * sizeof(real_t), "Vector4 is four");
static_assert(sizeof(Vector2i) == 2 * sizeof(int32_t), "Vector2i is two ints");
static_assert(sizeof(Vector3i) == 3 * sizeof(int32_t), "Vector3i is three");
static_assert(sizeof(Vector4i) == 4 * sizeof(int32_t), "Vector4i is four");
static_assert(sizeof(Color) == 4 * sizeof(float), "Color is four floats");
static_assert(sizeof(Quaternion) == 4 * sizeof(real_t), "Quaternion is four");
static_assert(sizeof(Rect2) == 4 * sizeof(real_t), "Rect2 is two Vector2");
static_assert(sizeof(Rect2i) == 4 * sizeof(int32_t), "Rect2i is two Vector2i");
static_assert(sizeof(AABB) == 6 * sizeof(real_t), "AABB is two Vector3");
static_assert(sizeof(Plane) == 4 * sizeof(real_t), "Plane is a Vector3 and d");
static_assert(sizeof(Transform2D) == 6 * sizeof(real_t), "three Vector2");
static_assert(sizeof(Basis) == 9 * sizeof(real_t), "three Vector3");
static_assert(sizeof(Transform3D) == 12 * sizeof(real_t), "a Basis and an origin");
static_assert(sizeof(Projection) == 16 * sizeof(real_t), "four Vector4");

// Which Variant a value type stands for, and what it is called in L^. The
// Variant kind is the index the module keeps its tags under, so a host
// crossing a Variant can find the tag from the type it read.
template <typename T>
struct Named;

#define LHAT_GODOT_VALUE(T_, K_)                          \
    template <>                                           \
    struct Named<T_> {                                    \
        static constexpr Variant::Type kind = K_;         \
        static const char *spelling() { return #T_; }     \
    }

LHAT_GODOT_VALUE(Vector2, Variant::VECTOR2);
LHAT_GODOT_VALUE(Vector2i, Variant::VECTOR2I);
LHAT_GODOT_VALUE(Vector3, Variant::VECTOR3);
LHAT_GODOT_VALUE(Vector3i, Variant::VECTOR3I);
LHAT_GODOT_VALUE(Vector4, Variant::VECTOR4);
LHAT_GODOT_VALUE(Vector4i, Variant::VECTOR4I);
LHAT_GODOT_VALUE(Color, Variant::COLOR);
LHAT_GODOT_VALUE(Quaternion, Variant::QUATERNION);
LHAT_GODOT_VALUE(Rect2, Variant::RECT2);
LHAT_GODOT_VALUE(Rect2i, Variant::RECT2I);
LHAT_GODOT_VALUE(AABB, Variant::AABB);
LHAT_GODOT_VALUE(Plane, Variant::PLANE);
LHAT_GODOT_VALUE(Transform2D, Variant::TRANSFORM2D);
LHAT_GODOT_VALUE(Basis, Variant::BASIS);
LHAT_GODOT_VALUE(Transform3D, Variant::TRANSFORM3D);
LHAT_GODOT_VALUE(Projection, Variant::PROJECTION);

#undef LHAT_GODOT_VALUE

const Godot *module_of(void *context)
{
    return (const Godot *)context;
}

template <typename T>
const LhatHostValueTag *tag_of(const Godot *module)
{
    return module != nullptr ? module->value_tags[Named<T>::kind] : nullptr;
}

// The value a host answers with. 8.9 copies the bytes into the machine's own
// room, so nothing here has to outlive the call.
template <typename T>
LhatValue answer(LhatMachine *machine, const Godot *module, const T &value)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, tag_of<T>(module), &value, &out)
               ? out
               : lhat_nil();
}

// The bytes back, checked against the tag: 8.9 makes that the same double
// check 8.8 makes over a pointer, which is what stops a Vector3 reaching the
// C that expects a Color.
template <typename T>
bool taken(LhatValue held, const Godot *module, T *out)
{
    const void *bytes = lhat_hostvalue_data(held, tag_of<T>(module));
    if (bytes == nullptr) {
        return false;
    }
    memcpy(out, bytes, sizeof(T));
    return true;
}

double number_of(LhatValue value)
{
    if (lhat_is_integer(value)) {
        return (double)lhat_as_integer(value);
    }
    return lhat_is_real(value) ? lhat_as_real(value) : 0.0;
}

// ---------------------------------------------------------------------------
// The operators, written once and registered per type

// The right side is another of the same for a paired operator and a plain
// number for a scaling one, so what each answers is left to the engine's own
// overloads rather than named here.
struct Add {
    template <typename A, typename B>
    static auto of(const A &a, const B &b) -> decltype(a + b)
    {
        return a + b;
    }
};
struct Sub {
    template <typename A, typename B>
    static auto of(const A &a, const B &b) -> decltype(a - b)
    {
        return a - b;
    }
};
struct Mul {
    template <typename A, typename B>
    static auto of(const A &a, const B &b) -> decltype(a * b)
    {
        return a * b;
    }
};
struct Div {
    template <typename A, typename B>
    static auto of(const A &a, const B &b) -> decltype(a / b)
    {
        return a / b;
    }
};

// f^self^, T -> T
template <typename T, typename Op>
LhatValue paired(LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    T right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module, Op::of(left, right));
}

// f^self^, number^ -> T
template <typename T, typename Op, typename C>
LhatValue scaled(LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    if (count < 2 || !taken(arguments[0], module, &left)) {
        return lhat_nil();
    }
    return answer(machine, module,
                  Op::of(left, (C)number_of(arguments[1])));
}

// 02 の 11.3改: the self^ written last, so this is the arm '2 * v' finds --
// a built-in number^ on the left carries no answer for a host value.
template <typename T, typename Op, typename C>
LhatValue scaled_last(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T right;
    if (count < 2 || !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module,
                  Op::of(right, (C)number_of(arguments[0])));
}

// f^self^ -> T
template <typename T>
LhatValue negated(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T held;
    if (count < 1 || !taken(arguments[0], module, &held)) {
        return lhat_nil();
    }
    return answer(machine, module, -held);
}

// 02 の 14.17: what a value says when it is written out.
template <typename T>
LhatValue as_text(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T held;
    if (count < 1 || !taken(arguments[0], module, &held)) {
        return lhat_nil();
    }
    CharString bytes = String(Variant(held)).utf8();
    LhatValue out = lhat_nil();
    return lhat_machine_make_string(machine, bytes.get_data(),
                                    (size_t)bytes.length(), &out)
               ? out
               : lhat_nil();
}

// ---------------------------------------------------------------------------
// The module functions that make one

template <typename T, typename C>
LhatValue make_xy(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count)
{
    if (count < 2) {
        return lhat_nil();
    }
    T made;
    made.x = (C)number_of(arguments[0]);
    made.y = (C)number_of(arguments[1]);
    return answer(machine, module_of(context), made);
}

template <typename T, typename C>
LhatValue make_xyz(LhatMachine *machine, void *context,
                   const LhatValue *arguments, size_t count)
{
    if (count < 3) {
        return lhat_nil();
    }
    T made;
    made.x = (C)number_of(arguments[0]);
    made.y = (C)number_of(arguments[1]);
    made.z = (C)number_of(arguments[2]);
    return answer(machine, module_of(context), made);
}

template <typename T, typename C>
LhatValue make_xyzw(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count)
{
    if (count < 4) {
        return lhat_nil();
    }
    T made;
    made.x = (C)number_of(arguments[0]);
    made.y = (C)number_of(arguments[1]);
    made.z = (C)number_of(arguments[2]);
    made.w = (C)number_of(arguments[3]);
    return answer(machine, module_of(context), made);
}

LhatValue make_color(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    if (count < 4) {
        return lhat_nil();
    }
    Color made;
    made.r = (float)number_of(arguments[0]);
    made.g = (float)number_of(arguments[1]);
    made.b = (float)number_of(arguments[2]);
    made.a = (float)number_of(arguments[3]);
    return answer(machine, module_of(context), made);
}


// ---------------------------------------------------------------------------
// The shapes built out of the others

// A transform applied to a vector answers the vector, not the transform, so
// this is the operator whose two sides differ.
struct Xform {
    template <typename A, typename B>
    static auto of(const A &a, const B &b) -> decltype(a.xform(b))
    {
        return a.xform(b);
    }
};

// f^self^, U -> R, where R is whatever the operation answers.
template <typename T, typename U, typename Op>
LhatValue mixed(LhatMachine *machine, void *context,
                const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    U right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module, Op::of(left, right));
}

// One part of a shape, read out. 8.9 hands a host function a copy of the
// bytes -- writing through them changes that copy and nothing the program
// sees -- so these read and there is no writing counterpart. A shape is
// changed by making another, which is what a value type means.
template <typename T, typename R, R (*Get)(const T &)>
LhatValue reads(LhatMachine *machine, void *context,
                const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T held;
    if (count < 1 || !taken(arguments[0], module, &held)) {
        return lhat_nil();
    }
    return answer(machine, module, Get(held));
}

Vector2 rect2_position(const Rect2 &r) { return r.position; }
Vector2 rect2_size(const Rect2 &r) { return r.size; }
Vector2i rect2i_position(const Rect2i &r) { return r.position; }
Vector2i rect2i_size(const Rect2i &r) { return r.size; }
Vector3 aabb_position(const AABB &b) { return b.position; }
Vector3 aabb_size(const AABB &b) { return b.size; }
Vector3 plane_normal(const Plane &p) { return p.normal; }
Vector2 t2d_x(const Transform2D &t) { return t.columns[0]; }
Vector2 t2d_y(const Transform2D &t) { return t.columns[1]; }
Vector2 t2d_origin(const Transform2D &t) { return t.columns[2]; }
Vector3 basis_x(const Basis &b) { return b.rows[0]; }
Vector3 basis_y(const Basis &b) { return b.rows[1]; }
Vector3 basis_z(const Basis &b) { return b.rows[2]; }
Basis t3d_basis(const Transform3D &t) { return t.basis; }
Vector3 t3d_origin(const Transform3D &t) { return t.origin; }
Vector4 proj_x(const Projection &p) { return p.columns[0]; }
Vector4 proj_y(const Projection &p) { return p.columns[1]; }
Vector4 proj_z(const Projection &p) { return p.columns[2]; }
Vector4 proj_w(const Projection &p) { return p.columns[3]; }

// A shape is made out of the parts it is made of, so these take values where
// the flat ones took numbers.
template <typename T, typename P>
LhatValue make_pair(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    P first;
    P second;
    if (count < 2 || !taken(arguments[0], module, &first) ||
        !taken(arguments[1], module, &second)) {
        return lhat_nil();
    }
    return answer(machine, module, T(first, second));
}

template <typename T, typename P>
LhatValue make_triple(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    P first;
    P second;
    P third;
    if (count < 3 || !taken(arguments[0], module, &first) ||
        !taken(arguments[1], module, &second) ||
        !taken(arguments[2], module, &third)) {
        return lhat_nil();
    }
    return answer(machine, module, T(first, second, third));
}

LhatValue make_plane(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    Vector3 normal;
    if (count < 2 || !taken(arguments[0], module, &normal)) {
        return lhat_nil();
    }
    return answer(machine, module,
                  Plane(normal, (real_t)number_of(arguments[1])));
}

LhatValue make_transform3d(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    Basis basis;
    Vector3 origin;
    if (count < 2 || !taken(arguments[0], module, &basis) ||
        !taken(arguments[1], module, &origin)) {
        return lhat_nil();
    }
    return answer(machine, module, Transform3D(basis, origin));
}

LhatValue make_projection(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    Vector4 columns[4];
    if (count < 4) {
        return lhat_nil();
    }
    for (size_t i = 0; i < 4; i++) {
        if (!taken(arguments[i], module, &columns[i])) {
            return lhat_nil();
        }
    }
    return answer(machine, module,
                  Projection(columns[0], columns[1], columns[2], columns[3]));
}
// ---------------------------------------------------------------------------
// Registration

// The signatures are the same shape for every type and differ only in the
// name, so they are built rather than written out sixteen times.
// A registered member borrows the name it was given rather than copying it
// (lhat_type_add_member), so a name built here has to outlive the call that
// built it. The module is where it goes: it lives as long as the program.
// The signatures are kept beside them for the same reason and no other -- one
// rule is easier to keep than two.
const char *kept(Godot *module, const String &text)
{
    module->texts.push_back(text.utf8());
    return module->texts.back()->get().get_data();
}

struct Signatures {
    const char *paired;
    const char *scaled;
    const char *scaled_last;
    const char *unary;
    const char *text;

    Signatures(Godot *module, const char *name)
    {
        String path = String("godot.") + name;
        paired = kept(module, String("f^self^, ") + path + " -> " + path + ";");
        scaled = kept(module, String("f^self^, number^ -> ") + path + ";");
        scaled_last = kept(module, String("f^number^, self^ -> ") + path + ";");
        unary = kept(module, String("f^self^ -> ") + path + ";");
        text = kept(module, String("f^self^ -> string^;"));
    }
};

bool member(LhatProgram *program, const char *type, const char *name,
            const char *signature, LhatHostFn call, void *context)
{
    return lhat_register_hostvalue_member(program, "godot", type, name,
                                          signature, call, context);
}

bool field(LhatProgram *program, const char *type, const char *name,
           size_t offset, LhatHostValueFieldKind kind)
{
    return lhat_register_hostvalue_field(program, "godot", type, name, offset,
                                         kind);
}

// '+', '-', unary '-', and '*' by another of the same -- what every one of
// these answers. Division by another is left to the ones that have it.
// `divides` says whether the engine gives this one a '/' by another of the
// same. A rotation has none, and asking for it at run time would still
// instantiate the body -- so it is a template argument and the arm the type
// does not have is never compiled.
template <typename T, typename C, bool divides>
bool arithmetic(LhatProgram *program, Godot *module)
{
    const char *name = Named<T>::spelling();
    Signatures said(module, name);
    bool ok = member(program, name, "+", said.paired, paired<T, Add>, module) &&
              member(program, name, "-", said.paired, paired<T, Sub>, module) &&
              member(program, name, "-", said.unary, negated<T>, module) &&
              member(program, name, "*", said.paired, paired<T, Mul>, module) &&
              member(program, name, "*", said.scaled, scaled<T, Mul, C>,
                     module) &&
              member(program, name, "*", said.scaled_last,
                     scaled_last<T, Mul, C>, module) &&
              member(program, name, "/", said.scaled, scaled<T, Div, C>,
                     module) &&
              member(program, name, "tostring", said.text, as_text<T>, module);
    if constexpr (divides) {
        ok = ok && member(program, name, "/", said.paired, paired<T, Div>,
                          module);
    }
    return ok;
}

// The type itself, and the tag kept where a Variant kind can find it.
template <typename T>
bool declare(LhatProgram *program, Godot *module)
{
    const LhatHostValueTag *tag = lhat_register_hostvalue_type(
        program, "godot", Named<T>::spelling(), sizeof(T));
    module->value_tags[Named<T>::kind] = tag;
    return tag != nullptr;
}

bool maker(LhatProgram *program, Godot *module, const char *name,
           const char *takes, const char *type, LhatHostFn call)
{
    const char *signature =
        kept(module, String("f^") + takes + " -> godot." + type + ";");
    return lhat_register_func(program, "godot", name, signature, call, module);
}


// A part read out, and the shapes whose parts are other shapes.
template <typename T, typename R, R (*Get)(const T &)>
bool part(LhatProgram *program, Godot *module, const char *name)
{
    const char *signature =
        kept(module, String("f^self^ -> godot.") + Named<R>::spelling() + ";");
    return member(program, Named<T>::spelling(), name, signature,
                  reads<T, R, Get>, module);
}

// '*' where the two sides differ: a transform applied to a vector.
template <typename T, typename U, typename Op, typename R>
bool applies(LhatProgram *program, Godot *module)
{
    const char *signature =
        kept(module, String("f^self^, godot.") + Named<U>::spelling() +
                         " -> godot." + Named<R>::spelling() + ";");
    return member(program, Named<T>::spelling(), "*", signature,
                  mixed<T, U, Op>, module);
}

// What every shape answers: its text, and -- where the engine gives it one
// -- itself times another of its own. `multiplies` is a template argument
// for the same reason `divides` is above: naming the arm a type does not
// have would still compile it.
template <typename T, bool multiplies>
bool composed(LhatProgram *program, Godot *module)
{
    const char *name = Named<T>::spelling();
    Signatures said(module, name);
    bool ok = member(program, name, "tostring", said.text, as_text<T>, module);
    if constexpr (multiplies) {
        ok = ok && member(program, name, "*", said.paired, paired<T, Mul>,
                          module);
    }
    return ok;
}


// ---------------------------------------------------------------------------
// Reading and writing a node's own fields
//
// 8.9 keeps a host value out of an any^, so godot.Object's `get` cannot carry
// one however the engine answers -- which is why these are one pair per type
// rather than one pair for everything. What is gained beside is that the type
// is settled where it is written: reading a Vector2 out of a field holding a
// Color is a mistake the checker sees.

bool name_of(LhatValue value, String *out)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(value);
    *out = String::utf8(text->text, (int)text->length);
    return true;
}

// f^self^, string^ -> T
template <typename T>
LhatValue object_get(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    String named;
    Object *object = count > 0 ? object_of(arguments[0], module) : nullptr;
    if (object == nullptr || count < 2 || !name_of(arguments[1], &named)) {
        return answer(machine, module, T());
    }
    // What the Variant reads as, which for a field of another type is
    // whatever the engine's own conversion makes of it -- a Vector2 read as
    // a Vector3 arrives with a zero z. There is no other answer to give: a
    // signature saying `-> godot.Vector2` has no room for nil^ (8.9), which
    // is the same reason these are one pair per type in the first place.
    return answer(machine, module, (T)object->get(named));
}

// p^self^, string^, T
template <typename T>
LhatValue object_set(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    String named;
    T value;
    Object *object = count > 0 ? object_of(arguments[0], module) : nullptr;
    if (object == nullptr || count < 3 || !name_of(arguments[1], &named) ||
        !taken(arguments[2], module, &value)) {
        return lhat_nil();
    }
    object->set(named, Variant(value));
    return lhat_nil();
}

template <typename T>
bool crossing(LhatProgram *program, Godot *module)
{
    String name = String(Named<T>::spelling());
    String path = String("godot.") + name;
    const char *reading = kept(module, String("f^self^, string^ -> ") + path + ";");
    const char *writing = kept(module, String("p^self^, string^, ") + path + ";");
    const char *reader = kept(module, String("get") + name);
    const char *writer = kept(module, String("set") + name);
    return lhat_register_member(program, "godot", "Object", reader, reading,
                                object_get<T>, module) &&
           lhat_register_member(program, "godot", "Object", writer, writing,
                                object_set<T>, module);
}
}  // namespace

bool register_values(LhatProgram *program, Godot *module)
{
    if (program == nullptr || module == nullptr) {
        return false;
    }

    return declare<Vector2>(program, module) &&
           field(program, "Vector2", "x", offsetof(Vector2, x), REAL_FIELD) &&
           field(program, "Vector2", "y", offsetof(Vector2, y), REAL_FIELD) &&
           arithmetic<Vector2, real_t, true>(program, module) &&
           maker(program, module, "vector2", "number^, number^", "Vector2",
                 make_xy<Vector2, real_t>) &&

           declare<Vector2i>(program, module) &&
           field(program, "Vector2i", "x", offsetof(Vector2i, x), INT_FIELD) &&
           field(program, "Vector2i", "y", offsetof(Vector2i, y), INT_FIELD) &&
           arithmetic<Vector2i, int32_t, true>(program, module) &&
           maker(program, module, "vector2i", "number^, number^", "Vector2i",
                 make_xy<Vector2i, int32_t>) &&

           declare<Vector3>(program, module) &&
           field(program, "Vector3", "x", offsetof(Vector3, x), REAL_FIELD) &&
           field(program, "Vector3", "y", offsetof(Vector3, y), REAL_FIELD) &&
           field(program, "Vector3", "z", offsetof(Vector3, z), REAL_FIELD) &&
           arithmetic<Vector3, real_t, true>(program, module) &&
           maker(program, module, "vector3", "number^, number^, number^",
                 "Vector3", make_xyz<Vector3, real_t>) &&

           declare<Vector3i>(program, module) &&
           field(program, "Vector3i", "x", offsetof(Vector3i, x), INT_FIELD) &&
           field(program, "Vector3i", "y", offsetof(Vector3i, y), INT_FIELD) &&
           field(program, "Vector3i", "z", offsetof(Vector3i, z), INT_FIELD) &&
           arithmetic<Vector3i, int32_t, true>(program, module) &&
           maker(program, module, "vector3i", "number^, number^, number^",
                 "Vector3i", make_xyz<Vector3i, int32_t>) &&

           declare<Vector4>(program, module) &&
           field(program, "Vector4", "x", offsetof(Vector4, x), REAL_FIELD) &&
           field(program, "Vector4", "y", offsetof(Vector4, y), REAL_FIELD) &&
           field(program, "Vector4", "z", offsetof(Vector4, z), REAL_FIELD) &&
           field(program, "Vector4", "w", offsetof(Vector4, w), REAL_FIELD) &&
           arithmetic<Vector4, real_t, true>(program, module) &&
           maker(program, module, "vector4",
                 "number^, number^, number^, number^", "Vector4",
                 make_xyzw<Vector4, real_t>) &&

           declare<Vector4i>(program, module) &&
           field(program, "Vector4i", "x", offsetof(Vector4i, x), INT_FIELD) &&
           field(program, "Vector4i", "y", offsetof(Vector4i, y), INT_FIELD) &&
           field(program, "Vector4i", "z", offsetof(Vector4i, z), INT_FIELD) &&
           field(program, "Vector4i", "w", offsetof(Vector4i, w), INT_FIELD) &&
           arithmetic<Vector4i, int32_t, true>(program, module) &&
           maker(program, module, "vector4i",
                 "number^, number^, number^, number^", "Vector4i",
                 make_xyzw<Vector4i, int32_t>) &&

           declare<Color>(program, module) &&
           field(program, "Color", "r", offsetof(Color, r), COLOR_FIELD) &&
           field(program, "Color", "g", offsetof(Color, g), COLOR_FIELD) &&
           field(program, "Color", "b", offsetof(Color, b), COLOR_FIELD) &&
           field(program, "Color", "a", offsetof(Color, a), COLOR_FIELD) &&
           arithmetic<Color, float, true>(program, module) &&
           maker(program, module, "color",
                 "number^, number^, number^, number^", "Color", make_color) &&

           // 8.9 with 11.8: a rotation multiplies another rotation, which is
           // what the paired '*' means here -- not a component-wise product.
           declare<Quaternion>(program, module) &&
           field(program, "Quaternion", "x", offsetof(Quaternion, x),
                 REAL_FIELD) &&
           field(program, "Quaternion", "y", offsetof(Quaternion, y),
                 REAL_FIELD) &&
           field(program, "Quaternion", "z", offsetof(Quaternion, z),
                 REAL_FIELD) &&
           field(program, "Quaternion", "w", offsetof(Quaternion, w),
                 REAL_FIELD) &&
           arithmetic<Quaternion, real_t, false>(program, module) &&
           maker(program, module, "quaternion",
                 "number^, number^, number^, number^", "Quaternion",
                 make_xyzw<Quaternion, real_t>) &&

           // A rectangle, a box and a plane hold other values and answer no
           // arithmetic of their own -- what they are for is being read.
           declare<Rect2>(program, module) &&
           part<Rect2, Vector2, rect2_position>(program, module, "position") &&
           part<Rect2, Vector2, rect2_size>(program, module, "size") &&
           composed<Rect2, false>(program, module) &&
           maker(program, module, "rect2", "godot.Vector2, godot.Vector2",
                 "Rect2", make_pair<Rect2, Vector2>) &&

           declare<Rect2i>(program, module) &&
           part<Rect2i, Vector2i, rect2i_position>(program, module,
                                                   "position") &&
           part<Rect2i, Vector2i, rect2i_size>(program, module, "size") &&
           composed<Rect2i, false>(program, module) &&
           maker(program, module, "rect2i", "godot.Vector2i, godot.Vector2i",
                 "Rect2i", make_pair<Rect2i, Vector2i>) &&

           declare<AABB>(program, module) &&
           part<AABB, Vector3, aabb_position>(program, module, "position") &&
           part<AABB, Vector3, aabb_size>(program, module, "size") &&
           composed<AABB, false>(program, module) &&
           maker(program, module, "aabb", "godot.Vector3, godot.Vector3",
                 "AABB", make_pair<AABB, Vector3>) &&

           declare<Plane>(program, module) &&
           field(program, "Plane", "d", offsetof(Plane, d), REAL_FIELD) &&
           part<Plane, Vector3, plane_normal>(program, module, "normal") &&
           composed<Plane, false>(program, module) &&
           maker(program, module, "plane", "godot.Vector3, number^", "Plane",
                 make_plane) &&

           // A transform multiplies another transform, and applied to a
           // vector it answers the vector -- 11.8's two arms of one name.
           declare<Transform2D>(program, module) &&
           part<Transform2D, Vector2, t2d_x>(program, module, "x") &&
           part<Transform2D, Vector2, t2d_y>(program, module, "y") &&
           part<Transform2D, Vector2, t2d_origin>(program, module, "origin") &&
           composed<Transform2D, true>(program, module) &&
           applies<Transform2D, Vector2, Xform, Vector2>(program, module) &&
           maker(program, module, "transform2d",
                 "godot.Vector2, godot.Vector2, godot.Vector2", "Transform2D",
                 make_triple<Transform2D, Vector2>) &&

           declare<Basis>(program, module) &&
           part<Basis, Vector3, basis_x>(program, module, "x") &&
           part<Basis, Vector3, basis_y>(program, module, "y") &&
           part<Basis, Vector3, basis_z>(program, module, "z") &&
           composed<Basis, true>(program, module) &&
           applies<Basis, Vector3, Xform, Vector3>(program, module) &&
           maker(program, module, "basis",
                 "godot.Vector3, godot.Vector3, godot.Vector3", "Basis",
                 make_triple<Basis, Vector3>) &&

           declare<Transform3D>(program, module) &&
           part<Transform3D, Basis, t3d_basis>(program, module, "basis") &&
           part<Transform3D, Vector3, t3d_origin>(program, module, "origin") &&
           composed<Transform3D, true>(program, module) &&
           applies<Transform3D, Vector3, Xform, Vector3>(program, module) &&
           maker(program, module, "transform3d", "godot.Basis, godot.Vector3",
                 "Transform3D", make_transform3d) &&

           declare<Projection>(program, module) &&
           part<Projection, Vector4, proj_x>(program, module, "x") &&
           part<Projection, Vector4, proj_y>(program, module, "y") &&
           part<Projection, Vector4, proj_z>(program, module, "z") &&
           part<Projection, Vector4, proj_w>(program, module, "w") &&
           composed<Projection, true>(program, module) &&
           applies<Projection, Vector4, Xform, Vector4>(program, module) &&
           maker(program, module, "projection",
                 "godot.Vector4, godot.Vector4, godot.Vector4, godot.Vector4",
                 "Projection", make_projection) &&

           // The same sixteen, as fields a node holds.
           crossing<Vector2>(program, module) &&
           crossing<Vector2i>(program, module) &&
           crossing<Vector3>(program, module) &&
           crossing<Vector3i>(program, module) &&
           crossing<Vector4>(program, module) &&
           crossing<Vector4i>(program, module) &&
           crossing<Color>(program, module) &&
           crossing<Quaternion>(program, module) &&
           crossing<Rect2>(program, module) &&
           crossing<Rect2i>(program, module) &&
           crossing<AABB>(program, module) &&
           crossing<Plane>(program, module) &&
           crossing<Transform2D>(program, module) &&
           crossing<Basis>(program, module) &&
           crossing<Transform3D>(program, module) &&
           crossing<Projection>(program, module);
}

}  // namespace host
}  // namespace godot
