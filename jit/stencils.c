// L^ (lhat) -- the stencils: one instruction's work each, as Clang compiles
// it, kept as bytes by gen_stencils.py. Never linked into anything.
//
// Each is an LhatJitOp (jit.h) over the frame's payloads `v` and tags
// `t` (both already offset to the frame's base) and the machine's poll `p`.
// It ends by tail-calling the next instruction's code, by jumping to its
// target's, or by answering a pc: the instruction the interpreter goes on
// from. A stencil answers its own pc before it has written anything, so the
// interpreter runs that instruction whole -- which is how every case a
// stencil was not written for is handled: by the code that already handles
// it (12 の 7).
//
// The holes are the addresses of symbols nothing defines, compiled for the
// small code model without position independence: each is a 32-bit field in
// the instruction that uses it. A register's hole is two, its index into
// the tags (A) and its byte offset into the payloads (A8), so that either is
// a displacement the load or store carries rather than a value computed
// first. The helpers are the exception: they may be anywhere in the address
// space, so their addresses are 64-bit immediates (FAR).

#include "jit.h"
#include "lhat/object.h"

extern LhatJitOp _JIT_CONTINUE;
extern LhatJitOp _JIT_TARGET;
extern char _JIT_A[], _JIT_B[], _JIT_C[], _JIT_A8[], _JIT_B8[], _JIT_C8[];
extern char _JIT_K_LO[], _JIT_K_HI[], _JIT_KTAG[], _JIT_PC[];

#define HOLE(name) ((uintptr_t)_JIT_##name)

// Register r's payload and tag, and those of the ones after it.
#define V(r) VN(r, 0)
#define T(r) TN(r, 0)
#define VN(r, n) (*(LhatValueUnion *)((char *)v + HOLE(r##8) + 8 * (n)))
#define TN(r, n) (t[HOLE(r) + (n)])

// A helper's address, as a 64-bit immediate.
#define FAR(type, symbol)                                                   \
    ({                                                                      \
        type *far_;                                                         \
        __asm__("movabsq $" #symbol ", %0" : "=r"(far_));                   \
        far_;                                                               \
    })

#define INT LHAT_VALUE_INTEGER
#define REAL LHAT_VALUE_REAL
#define BOOL LHAT_VALUE_BOOL

#define STENCIL(name)                                                       \
    LHAT_JIT_ABI uintptr_t lhat_jit_s_##name(LhatValueUnion *v, uint8_t *t, \
                                             LhatJitContext *p)

#define NEXT() __attribute__((musttail)) return _JIT_CONTINUE(v, t, p)
#define JUMP() __attribute__((musttail)) return _JIT_TARGET(v, t, p)
#define LEAVE() return HOLE(PC)

// 02 § 15.15 at a jump back: a slice about to run out, or anything the
// traps word stands for, is the interpreter's to handle -- it reruns the
// jump and polls for itself. Asked before the instruction does anything.
#define POLL_OR_LEAVE()                                                     \
    do {                                                                    \
        if (*p->traps || *p->steps_left == 1) {                             \
            LEAVE();                                                        \
        }                                                                   \
    } while (0)
#define COUNT_TURN()                                                        \
    do {                                                                    \
        if (*p->steps_left != 0) {                                          \
            --*p->steps_left;                                               \
        }                                                                   \
    } while (0)

// The constant the two halves of the K hole carry.
static inline LhatValueUnion constant(void)
{
    LhatValueUnion k;
    k.integer = (int64_t)(((uint64_t)(uint32_t)HOLE(K_HI) << 32) |
                          (uint32_t)HOLE(K_LO));
    return k;
}

STENCIL(exit)
{
    LEAVE();
}

STENCIL(loadk)
{
    V(A) = constant();
    T(A) = (uint8_t)HOLE(KTAG);
    NEXT();
}

STENCIL(move)
{
    V(A) = V(B);
    T(A) = T(B);
    NEXT();
}

// 02 の 14.8: two integers stay integers unless the answer does not fit,
// two reals are reals, and anything else -- a mixed pair, an overflow that
// widens, an operator a type carries -- is the interpreter's.
#define ARITH_RR(name, overflows, oper)                                     \
    STENCIL(name)                                                           \
    {                                                                       \
        if (T(B) == INT && T(C) == INT) {                                   \
            int64_t r;                                                      \
            if (overflows(V(B).integer, V(C).integer, &r)) {                \
                LEAVE();                                                    \
            }                                                               \
            V(A).integer = r;                                               \
            T(A) = INT;                                                     \
            NEXT();                                                         \
        }                                                                   \
        if (T(B) == REAL && T(C) == REAL) {                                 \
            V(A).real = V(B).real oper V(C).real;                           \
            T(A) = REAL;                                                    \
            NEXT();                                                         \
        }                                                                   \
        LEAVE();                                                            \
    }

ARITH_RR(add, __builtin_add_overflow, +)
ARITH_RR(sub, __builtin_sub_overflow, -)
ARITH_RR(mul, __builtin_mul_overflow, *)

// The constant's kind is known when the code is laid out, so each of these
// is chosen for it: an integer constant meets an integer register, a real
// one a real register.
#define ARITH_RK_INT(name, overflows)                                       \
    STENCIL(name)                                                           \
    {                                                                       \
        int64_t r;                                                          \
        if (T(B) != INT ||                                                  \
            overflows(V(B).integer, constant().integer, &r)) {              \
            LEAVE();                                                        \
        }                                                                   \
        V(A).integer = r;                                                   \
        T(A) = INT;                                                         \
        NEXT();                                                             \
    }

#define ARITH_R_REAL(name, oper, right)                                     \
    STENCIL(name)                                                           \
    {                                                                       \
        if (T(B) != REAL || (right##_TAG) != REAL) {                        \
            LEAVE();                                                        \
        }                                                                   \
        V(A).real = V(B).real oper right##_VALUE;                           \
        T(A) = REAL;                                                        \
        NEXT();                                                             \
    }
#define KREAL_TAG REAL
#define KREAL_VALUE constant().real
#define RREAL_TAG T(C)
#define RREAL_VALUE V(C).real

ARITH_RK_INT(addk_int, __builtin_add_overflow)
ARITH_RK_INT(subk_int, __builtin_sub_overflow)
ARITH_RK_INT(mulk_int, __builtin_mul_overflow)
ARITH_R_REAL(addk_real, +, KREAL)
ARITH_R_REAL(subk_real, -, KREAL)
ARITH_R_REAL(mulk_real, *, KREAL)
ARITH_R_REAL(divk_real, /, KREAL)
// 04 の 11.2: '/' is real division whatever it is given, so two reals are
// the only pair this takes; integers go to the interpreter to be widened.
ARITH_R_REAL(div, /, RREAL)

// 02 の 14.8: two integers order exactly. A real orders with tolerance,
// which is the interpreter's. `fused` is the JUMP_FALSE after it that reads
// the answer (12 の 4.3), taken here: the target when it is false.
#define ORDER(name, oper, right)                                            \
    STENCIL(name)                                                           \
    {                                                                       \
        if (T(B) != INT || (right##_TAG) != INT) {                          \
            LEAVE();                                                        \
        }                                                                   \
        V(A).integer = V(B).integer oper right##_VALUE;                     \
        T(A) = BOOL;                                                        \
        NEXT();                                                             \
    }                                                                       \
    STENCIL(name##_fused)                                                   \
    {                                                                       \
        if (T(B) != INT || (right##_TAG) != INT) {                          \
            LEAVE();                                                        \
        }                                                                   \
        bool held = V(B).integer oper right##_VALUE;                        \
        V(A).integer = held;                                                \
        T(A) = BOOL;                                                        \
        if (!held) {                                                        \
            JUMP();                                                         \
        }                                                                   \
        NEXT();                                                             \
    }
#define KINT_TAG INT
#define KINT_VALUE constant().integer
#define RINT_TAG T(C)
#define RINT_VALUE V(C).integer

ORDER(lt, <, RINT)
ORDER(le, <=, RINT)
ORDER(gt, >, RINT)
ORDER(ge, >=, RINT)
ORDER(ltk, <, KINT)
ORDER(lek, <=, KINT)
ORDER(gtk, >, KINT)
ORDER(gek, >=, KINT)

STENCIL(jump)
{
    JUMP();
}

STENCIL(jump_back)
{
    POLL_OR_LEAVE();
    COUNT_TURN();
    JUMP();
}

STENCIL(jump_false)
{
    if (T(A) != BOOL) {
        LEAVE();
    }
    if (!V(A).boolean) {
        JUMP();
    }
    NEXT();
}

STENCIL(jump_false_back)
{
    if (T(A) != BOOL) {
        LEAVE();
    }
    if (!V(A).boolean) {
        POLL_OR_LEAVE();
        COUNT_TURN();
        JUMP();
    }
    NEXT();
}

// 12 の 4.2: integers all three, the loop counts in integers, and an
// overflow is past any integer bound -- the loop is over.
#define FORLOOP(name, overflows, oper)                                      \
    STENCIL(name)                                                           \
    {                                                                       \
        if (T(A) != INT || TN(A, 1) != INT || TN(A, 2) != INT) {            \
            LEAVE();                                                        \
        }                                                                   \
        POLL_OR_LEAVE();                                                    \
        int64_t focus;                                                      \
        if (overflows(V(A).integer, VN(A, 2).integer, &focus)) {            \
            NEXT();                                                         \
        }                                                                   \
        V(A).integer = focus;                                               \
        if (focus oper VN(A, 1).integer) {                                  \
            COUNT_TURN();                                                   \
            JUMP();                                                         \
        }                                                                   \
        NEXT();                                                             \
    }

FORLOOP(forloop, __builtin_add_overflow, <=)
FORLOOP(forloopd, __builtin_sub_overflow, >=)

// 5.4: the place a closure shares with the frame that made it, open or
// closed alike -- the location aims at whichever holds the value now.
STENCIL(getupval)
{
    LhatUpvalue *const *upvalues = ((const LhatClosure *)p->closure)->upvalues;
    const LhatUpvalue *shared =
        *(LhatUpvalue *const *)((const char *)upvalues + HOLE(B8));
    V(A) = *shared->location.value;
    T(A) = *shared->location.tag;
    NEXT();
}

// 03 の 5.8: a table's dense half, read at an integer it holds a value at.
// Anything else -- a hash key, a hole, a definition or a delegate to ask,
// a value that is not a table -- is the lookup the interpreter makes.
STENCIL(getindex)
{
    if (T(B) != LHAT_VALUE_OBJECT || T(C) != INT) {
        LEAVE();
    }
    const LhatObject *object = V(B).object;
    if (object == NULL || object->kind != LHAT_OBJECT_TABLE) {
        LEAVE();
    }
    const LhatTable *table = (const LhatTable *)object;
    uint64_t at = (uint64_t)V(C).integer;
    if (at >= table->array_count ||
        table->array.tags[at] == LHAT_VALUE_NIL) {
        LEAVE();
    }
    V(A) = table->array.values[at];
    T(A) = table->array.tags[at];
    NEXT();
}

// 05 の 8.6: one table per machine, so naming it is a move.
STENCIL(env)
{
    V(A).object = (LhatObject *)(uintptr_t)p->environment;
    T(A) = LHAT_VALUE_OBJECT;
    NEXT();
}

STENCIL(isnil)
{
    bool absent = T(B) == LHAT_VALUE_NIL;
    V(A).integer = absent;
    T(A) = BOOL;
    NEXT();
}

// 02 の 5.4: only a bool is a truth value; anything else is the
// interpreter's to refuse.
STENCIL(not)
{
    if (T(B) != BOOL) {
        LEAVE();
    }
    bool negated = !V(B).boolean;
    V(A).integer = negated;
    T(A) = BOOL;
    NEXT();
}

// 02 の 11.9 with 14.8: of the values that answer '=' for themselves, nil^,
// bool^ and an integer name themselves exactly, so two of them are equal
// when their tags and payloads are. A real (tolerance), a string, and
// anything that may carry an op^= are the interpreter's. `same` is what
// the instruction answers when the two are equal: true for EQ, false for NE.
// `right` names what B is compared with: register C, or the K constant.
#define EXACT_KIND(tag) ((tag) == LHAT_VALUE_NIL || (tag) == BOOL || (tag) == INT)
#define RIGHT_REG_EXACT EXACT_KIND(T(C))
#define RIGHT_REG_EQUAL                                                     \
    (T(B) == T(C) &&                                                        \
     (T(B) == LHAT_VALUE_NIL ||                                             \
      (T(B) == BOOL ? V(B).boolean == V(C).boolean                          \
                    : V(B).integer == V(C).integer)))
// Against an integer constant: equal only to an integer holding it, and
// unequal to every other value that names itself exactly.
#define RIGHT_KINT_EXACT true
#define RIGHT_KINT_EQUAL (T(B) == INT && V(B).integer == constant().integer)
#define EQUALITY(name, same, right)                                         \
    STENCIL(name)                                                           \
    {                                                                       \
        if (!EXACT_KIND(T(B)) || !(RIGHT_##right##_EXACT)) {                \
            LEAVE();                                                        \
        }                                                                   \
        bool held = (RIGHT_##right##_EQUAL) == (same);                      \
        V(A).integer = held;                                                \
        T(A) = BOOL;                                                        \
        NEXT();                                                             \
    }                                                                       \
    STENCIL(name##_fused)                                                   \
    {                                                                       \
        if (!EXACT_KIND(T(B)) || !(RIGHT_##right##_EXACT)) {                \
            LEAVE();                                                        \
        }                                                                   \
        bool held = (RIGHT_##right##_EQUAL) == (same);                      \
        V(A).integer = held;                                                \
        T(A) = BOOL;                                                        \
        if (!held) {                                                        \
            JUMP();                                                         \
        }                                                                   \
        NEXT();                                                             \
    }

EQUALITY(eq, true, REG)
EQUALITY(ne, false, REG)
EQUALITY(eqk, true, KINT)
EQUALITY(nek, false, KINT)

// An instruction whose work is C's -- a member read through the site's
// cache, a table write with its barrier -- done in place, or left whole.
// The step and the operands it is handed are indices, not offsets: what C
// reads, it reads through the context.
STENCIL(step)
{
    if (!FAR(LhatJitStep, _JIT_STEP)(p, HOLE(A), HOLE(B), HOLE(C),
                                      HOLE(PC))) {
        LEAVE();
    }
    NEXT();
}

// 02 の 15.10: the subroutine running, which the context keeps beside the
// frame it is running in.
STENCIL(this)
{
    V(A).object = (LhatObject *)(uintptr_t)p->closure;
    T(A) = LHAT_VALUE_OBJECT;
    NEXT();
}

// 5.3: a call and a return are the machine's frames moving, which is C's
// to do (jit.c). Done, the helper names the code of the frame now on top,
// and the stencil goes there with that frame's slots; anything it would
// not do is left where it stands.
#define MOVE_FRAME(helper, a, b, c)                                         \
    do {                                                                    \
        LhatJitOp *next = FAR(LhatJitHelper, helper)(p, a, b, c, HOLE(PC)); \
        if (next == NULL) {                                                 \
            return p->leave_pc;                                             \
        }                                                                   \
        __attribute__((musttail)) return next(p->values, p->tags, p);       \
    } while (0)

// The helper reads the instruction itself: a call's operands mean different
// things for each of the opcodes this stands for.
STENCIL(call)
{
    MOVE_FRAME(_JIT_CALL, 0, 0, 0);
}

STENCIL(return)
{
    MOVE_FRAME(_JIT_RETURN, HOLE(A), 0, 0);
}

STENCIL(return_nil)
{
    MOVE_FRAME(_JIT_RETURN, 0, 1, 0);
}
