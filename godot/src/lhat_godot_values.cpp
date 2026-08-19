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
#include <godot_cpp/variant/rid.hpp>
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
static_assert(sizeof(RID) == 8, "a RID is one opaque word");

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
// 8.8 or 8.9? A RID is eight opaque bytes naming something a server holds --
// no reference of its own, so it is a value, and the server it names is not
// L^'s to keep alive either way.
LHAT_GODOT_VALUE(RID, Variant::RID);

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


// 8.9 with 8.8: what a RID names lives in a server and is reached by handing
// the RID back, so there is nothing here to read but the number itself.
LhatValue rid_id(LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    RID held;
    if (count < 1 || !taken(arguments[0], module, &held)) {
        return lhat_integer(0);
    }
    return lhat_integer(held.get_id());
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
// What each type answers about itself
//
// The engine's own methods, one wrapper per shape of answer. Written as free
// functions and named as template arguments so that the body below is written
// once and the list at the bottom stays a list.

// f^self^ -> number^
template <typename T, double (*Get)(const T &)>
LhatValue measures(LhatMachine *machine, void *context,
                   const LhatValue *arguments, size_t count)
{
    (void)machine;
    T held;
    if (count < 1 || !taken(arguments[0], module_of(context), &held)) {
        return lhat_real(0.0);
    }
    return lhat_real(Get(held));
}

// f^self^, T -> number^
template <typename T, double (*Get)(const T &, const T &)>
LhatValue measures_same(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    T left;
    T right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_real(0.0);
    }
    return lhat_real(Get(left, right));
}

// f^self^, U -> number^
template <typename T, typename U, double (*Get)(const T &, const U &)>
LhatValue measures_other(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    T left;
    U right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_real(0.0);
    }
    return lhat_real(Get(left, right));
}

// f^self^, U -> bool^
template <typename T, typename U, bool (*Get)(const T &, const U &)>
LhatValue asks_other(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    const Godot *module = module_of(context);
    T left;
    U right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_bool(false);
    }
    return lhat_bool(Get(left, right));
}

// f^self^, number^ -> T
template <typename T, T (*Get)(const T &, double)>
LhatValue turned(LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T held;
    if (count < 2 || !taken(arguments[0], module, &held)) {
        return lhat_nil();
    }
    return answer(machine, module, Get(held, number_of(arguments[1])));
}

// f^self^, T, number^ -> T
template <typename T, T (*Get)(const T &, const T &, double)>
LhatValue blended(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    T right;
    if (count < 3 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module,
                  Get(left, right, number_of(arguments[2])));
}

// f^self^, T -> T, named rather than spelled as an operator.
template <typename T, T (*Get)(const T &, const T &)>
LhatValue joined(LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    T right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module, Get(left, right));
}

// f^self^, U -> U
template <typename T, typename U, U (*Get)(const T &, const U &)>
LhatValue applied(LhatMachine *machine, void *context,
                  const LhatValue *arguments, size_t count)
{
    const Godot *module = module_of(context);
    T left;
    U right;
    if (count < 2 || !taken(arguments[0], module, &left) ||
        !taken(arguments[1], module, &right)) {
        return lhat_nil();
    }
    return answer(machine, module, Get(left, right));
}

// The engine's methods, reached through a pointer a template can name.
template <typename T> double v_length(const T &v) { return v.length(); }
template <typename T> double v_length_squared(const T &v)
{
    return v.length_squared();
}
template <typename T> T v_normalized(const T &v) { return v.normalized(); }
template <typename T> double v_dot(const T &a, const T &b) { return a.dot(b); }
template <typename T> double v_distance(const T &a, const T &b)
{
    return a.distance_to(b);
}
template <typename T> T v_lerp(const T &a, const T &b, double w)
{
    return a.lerp(b, (real_t)w);
}

double v2_angle(const Vector2 &v) { return v.angle(); }
double v2_angle_to(const Vector2 &a, const Vector2 &b) { return a.angle_to(b); }
double v2_cross(const Vector2 &a, const Vector2 &b) { return a.cross(b); }
Vector2 v2_rotated(const Vector2 &v, double by)
{
    return v.rotated((real_t)by);
}
Vector3 v3_cross(const Vector3 &a, const Vector3 &b) { return a.cross(b); }

Color color_inverted(const Color &c) { return c.inverted(); }
double color_luminance(const Color &c) { return c.get_luminance(); }

Quaternion q_inverse(const Quaternion &q) { return q.inverse(); }
Quaternion q_slerp(const Quaternion &a, const Quaternion &b, double w)
{
    return a.slerp(b, (real_t)w);
}

Plane plane_normalized(const Plane &p) { return p.normalized(); }
double plane_distance(const Plane &p, const Vector3 &at)
{
    return p.distance_to(at);
}
bool plane_has(const Plane &p, const Vector3 &at) { return p.has_point(at); }

bool rect2_has(const Rect2 &r, const Vector2 &at) { return r.has_point(at); }
bool rect2_intersects(const Rect2 &a, const Rect2 &b)
{
    return a.intersects(b);
}
Vector2 rect2_center(const Rect2 &r) { return r.get_center(); }
Rect2 rect2_merge(const Rect2 &a, const Rect2 &b) { return a.merge(b); }
bool rect2i_has(const Rect2i &r, const Vector2i &at) { return r.has_point(at); }
bool aabb_has(const AABB &b, const Vector3 &at) { return b.has_point(at); }
Vector3 aabb_center(const AABB &b) { return b.get_center(); }

Transform2D t2d_inverse(const Transform2D &t) { return t.affine_inverse(); }
double t2d_rotation(const Transform2D &t) { return t.get_rotation(); }
Vector2 t2d_scale(const Transform2D &t) { return t.get_scale(); }
Vector2 t2d_basis_xform(const Transform2D &t, const Vector2 &v)
{
    return t.basis_xform(v);
}
Transform2D t2d_rotated(const Transform2D &t, double by)
{
    return t.rotated((real_t)by);
}
Basis basis_inverse(const Basis &b) { return b.inverse(); }
Basis basis_transposed(const Basis &b) { return b.transposed(); }
double basis_determinant(const Basis &b) { return b.determinant(); }
Transform3D t3d_inverse(const Transform3D &t) { return t.affine_inverse(); }
Projection proj_inverse(const Projection &p) { return p.inverse(); }
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

// One method, by the shape of its answer. The name is the L^ one; the engine's
// is in the function the template names.
template <typename T, double (*Get)(const T &)>
bool measure(LhatProgram *program, Godot *module, const char *name)
{
    return member(program, Named<T>::spelling(), name,
                  kept(module, "f^self^ -> number^;"), measures<T, Get>,
                  module);
}

template <typename T, double (*Get)(const T &, const T &)>
bool measure_same(LhatProgram *program, Godot *module, const char *name)
{
    String path = String("godot.") + Named<T>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + path + " -> number^;"),
                  measures_same<T, Get>, module);
}

template <typename T, typename U, double (*Get)(const T &, const U &)>
bool measure_other(LhatProgram *program, Godot *module, const char *name)
{
    String other = String("godot.") + Named<U>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + other + " -> number^;"),
                  measures_other<T, U, Get>, module);
}

template <typename T, typename U, bool (*Get)(const T &, const U &)>
bool ask_other(LhatProgram *program, Godot *module, const char *name)
{
    String other = String("godot.") + Named<U>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + other + " -> bool^;"),
                  asks_other<T, U, Get>, module);
}

template <typename T, T (*Get)(const T &, double)>
bool turn(LhatProgram *program, Godot *module, const char *name)
{
    String path = String("godot.") + Named<T>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, number^ -> ") + path + ";"),
                  turned<T, Get>, module);
}

template <typename T, T (*Get)(const T &, const T &, double)>
bool blend(LhatProgram *program, Godot *module, const char *name)
{
    String path = String("godot.") + Named<T>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + path +
                                   ", number^ -> " + path + ";"),
                  blended<T, Get>, module);
}

template <typename T, T (*Get)(const T &, const T &)>
bool join_same(LhatProgram *program, Godot *module, const char *name)
{
    String path = String("godot.") + Named<T>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + path + " -> " + path + ";"),
                  joined<T, Get>, module);
}

template <typename T, typename U, U (*Get)(const T &, const U &)>
bool apply(LhatProgram *program, Godot *module, const char *name)
{
    String other = String("godot.") + Named<U>::spelling();
    return member(program, Named<T>::spelling(), name,
                  kept(module, String("f^self^, ") + other + " -> " + other +
                                   ";"),
                  applied<T, U, Get>, module);
}

// What every vector answers, real or integer -- an integer one has a length
// like any other, and answering it as a number^ costs nothing.
template <typename T>
bool measured_vector(LhatProgram *program, Godot *module)
{
    return measure<T, v_length<T>>(program, module, "length") &&
           measure<T, v_length_squared<T>>(program, module, "lengthSquared");
}

// And what only a real one does: there is no unit vector among the integers,
// and 8.9 has no way to say "a Vector2 whose length is one" anyway.
template <typename T>
bool real_vector(LhatProgram *program, Godot *module)
{
    return measured_vector<T>(program, module) &&
           part<T, T, v_normalized<T>>(program, module, "normalized") &&
           measure_same<T, v_dot<T>>(program, module, "dot") &&
           measure_same<T, v_distance<T>>(program, module, "distanceTo") &&
           blend<T, v_lerp<T>>(program, module, "lerp");
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
           crossing<Projection>(program, module) &&

           // 8.9: no field to read -- the bytes name something a server
           // holds and mean nothing on their own -- so a RID answers its
           // number and is otherwise carried about whole.
           declare<RID>(program, module) &&
           member(program, "RID", "getId", kept(module, "f^self^ -> number^;"),
                  rid_id, module) &&
           composed<RID, false>(program, module) &&
           crossing<RID>(program, module) &&

           // ---------------------------------------------------------------
           // What each answers about itself. The engine's own methods, under
           // L^'s spelling: a name is one word and the parts of it are not
           // separated by underscores here (01 の 2.1 leaves the choice open,
           // and the members registered above already read this way).
           real_vector<Vector2>(program, module) &&
           real_vector<Vector3>(program, module) &&
           real_vector<Vector4>(program, module) &&
           measured_vector<Vector2i>(program, module) &&
           measured_vector<Vector3i>(program, module) &&
           measured_vector<Vector4i>(program, module) &&

           // 2D turns about a point, so an angle is a thing a Vector2 has;
           // 3D turns about an axis, and there is no one angle to answer.
           measure<Vector2, v2_angle>(program, module, "angle") &&
           measure_same<Vector2, v2_angle_to>(program, module, "angleTo") &&
           measure_same<Vector2, v2_cross>(program, module, "cross") &&
           turn<Vector2, v2_rotated>(program, module, "rotated") &&
           join_same<Vector3, v3_cross>(program, module, "cross") &&

           part<Color, Color, color_inverted>(program, module, "inverted") &&
           measure<Color, color_luminance>(program, module, "luminance") &&
           blend<Color, v_lerp<Color>>(program, module, "lerp") &&

           measure<Quaternion, v_length<Quaternion>>(program, module,
                                                     "length") &&
           part<Quaternion, Quaternion, v_normalized<Quaternion>>(
               program, module, "normalized") &&
           measure_same<Quaternion, v_dot<Quaternion>>(program, module,
                                                       "dot") &&
           part<Quaternion, Quaternion, q_inverse>(program, module,
                                                   "inverse") &&
           blend<Quaternion, q_slerp>(program, module, "slerp") &&

           part<Plane, Plane, plane_normalized>(program, module,
                                                "normalized") &&
           measure_other<Plane, Vector3, plane_distance>(program, module,
                                                         "distanceTo") &&
           ask_other<Plane, Vector3, plane_has>(program, module, "hasPoint") &&

           ask_other<Rect2, Vector2, rect2_has>(program, module, "hasPoint") &&
           ask_other<Rect2, Rect2, rect2_intersects>(program, module,
                                                     "intersects") &&
           part<Rect2, Vector2, rect2_center>(program, module, "center") &&
           join_same<Rect2, rect2_merge>(program, module, "merged") &&
           ask_other<Rect2i, Vector2i, rect2i_has>(program, module,
                                                   "hasPoint") &&
           ask_other<AABB, Vector3, aabb_has>(program, module, "hasPoint") &&
           part<AABB, Vector3, aabb_center>(program, module, "center") &&

           part<Transform2D, Transform2D, t2d_inverse>(program, module,
                                                       "affineInverse") &&
           measure<Transform2D, t2d_rotation>(program, module, "rotation") &&
           part<Transform2D, Vector2, t2d_scale>(program, module, "scale") &&
           apply<Transform2D, Vector2, t2d_basis_xform>(program, module,
                                                        "basisXform") &&
           turn<Transform2D, t2d_rotated>(program, module, "rotated") &&

           part<Basis, Basis, basis_inverse>(program, module, "inverse") &&
           part<Basis, Basis, basis_transposed>(program, module,
                                                "transposed") &&
           measure<Basis, basis_determinant>(program, module, "determinant") &&
           part<Transform3D, Transform3D, t3d_inverse>(program, module,
                                                       "affineInverse") &&
           part<Projection, Projection, proj_inverse>(program, module,
                                                      "inverse");
}

// What a Packed*Array's elements are handed over as. Written out rather than
// templated: the four are all there are, and a caller in another translation
// unit would otherwise need the traits above.
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector2 &value)
{
    return answer(machine, module, value);
}
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector3 &value)
{
    return answer(machine, module, value);
}
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Vector4 &value)
{
    return answer(machine, module, value);
}
LhatValue value_answer(LhatMachine *machine, const Godot *module,
                       const Color &value)
{
    return answer(machine, module, value);
}

bool value_taken(LhatValue held, const Godot *module, Vector2 *out)
{
    return taken(held, module, out);
}
bool value_taken(LhatValue held, const Godot *module, Vector3 *out)
{
    return taken(held, module, out);
}
bool value_taken(LhatValue held, const Godot *module, Vector4 *out)
{
    return taken(held, module, out);
}
bool value_taken(LhatValue held, const Godot *module, Color *out)
{
    return taken(held, module, out);
}

}  // namespace host
}  // namespace godot
