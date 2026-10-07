// L^ (lhat) -- the stencils: one instruction's work each, as Clang compiles
// it, kept as bytes by gen_stencils.py. Never linked into anything.
//
// Each is a preserve_none function over the frame's payloads `v` and tags
// `t` (both already offset to the frame's base) and the machine's poll `p`.
// It ends by tail-calling the next instruction's code, by jumping to its
// target's, or by answering a pc: the instruction the interpreter goes on
// from. A stencil answers its own pc before it has written anything, so the
// interpreter runs that instruction whole -- which is how every case a
// stencil was not written for is handled: by the code that already handles
// it (03 の 5.1).
//
// The holes are the addresses of symbols nothing defines. Under the large
// code model each one is a 64-bit immediate the layout writes (jit.c).

#define LHAT_JIT_STENCIL_SOURCE
#include "jit.h"

extern LhatJitOp _JIT_CONTINUE;
extern LhatJitOp _JIT_TARGET;
extern char _JIT_A, _JIT_B, _JIT_C, _JIT_K, _JIT_KTAG, _JIT_PC;

#define HOLE(name) ((uintptr_t)&_JIT_##name)

#define INT LHAT_VALUE_INTEGER
#define REAL LHAT_VALUE_REAL
#define BOOL LHAT_VALUE_BOOL

#define STENCIL(name)                                                       \
    LHAT_JIT_CC uintptr_t lhat_jit_s_##name(LhatValueUnion *v, uint8_t *t,  \
                                            LhatJitPoll *p)

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

// The constant hole as the value it carries.
static inline LhatValueUnion constant(void)
{
    LhatValueUnion k;
    k.integer = (int64_t)HOLE(K);
    return k;
}

// The way in: the ordinary convention on one side, preserve_none on the
// other. Copied into the code like any other stencil (jit.h says why).
uintptr_t lhat_jit_s_trampoline(LhatValueUnion *v, uint8_t *t, LhatJitPoll *p,
                                LhatJitOp *entry)
{
    return entry(v, t, p);
}

STENCIL(exit)
{
    LEAVE();
}

STENCIL(loadk)
{
    uintptr_t a = HOLE(A);
    v[a] = constant();
    t[a] = (uint8_t)HOLE(KTAG);
    NEXT();
}

STENCIL(move)
{
    uintptr_t a = HOLE(A);
    uintptr_t b = HOLE(B);
    v[a] = v[b];
    t[a] = t[b];
    NEXT();
}

// 02 の 14.8: two integers stay integers unless the answer does not fit,
// two reals are reals, and anything else -- a mixed pair, an overflow that
// widens, an operator a type carries -- is the interpreter's.
#define ARITH_RR(name, overflows, oper)                                     \
    STENCIL(name)                                                           \
    {                                                                       \
        uintptr_t a = HOLE(A), b = HOLE(B), c = HOLE(C);                    \
        if (t[b] == INT && t[c] == INT) {                                   \
            int64_t r;                                                      \
            if (overflows(v[b].integer, v[c].integer, &r)) {                \
                LEAVE();                                                    \
            }                                                               \
            v[a].integer = r;                                               \
            t[a] = INT;                                                     \
            NEXT();                                                         \
        }                                                                   \
        if (t[b] == REAL && t[c] == REAL) {                                 \
            v[a].real = v[b].real oper v[c].real;                           \
            t[a] = REAL;                                                    \
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
        uintptr_t a = HOLE(A), b = HOLE(B);                                 \
        int64_t r;                                                          \
        if (t[b] != INT ||                                                  \
            overflows(v[b].integer, constant().integer, &r)) {              \
            LEAVE();                                                        \
        }                                                                   \
        v[a].integer = r;                                                   \
        t[a] = INT;                                                         \
        NEXT();                                                             \
    }

#define ARITH_R_REAL(name, oper, right)                                     \
    STENCIL(name)                                                           \
    {                                                                       \
        uintptr_t a = HOLE(A), b = HOLE(B);                                 \
        if (t[b] != REAL || (right##_TAG) != REAL) {                        \
            LEAVE();                                                        \
        }                                                                   \
        v[a].real = v[b].real oper right##_VALUE;                           \
        t[a] = REAL;                                                        \
        NEXT();                                                             \
    }
#define KREAL_TAG REAL
#define KREAL_VALUE constant().real
#define RREAL_TAG t[HOLE(C)]
#define RREAL_VALUE v[HOLE(C)].real

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
// the answer (03 の 5.1改5), taken here: the target when it is false.
#define ORDER(name, oper, right)                                            \
    STENCIL(name)                                                           \
    {                                                                       \
        uintptr_t a = HOLE(A), b = HOLE(B);                                 \
        if (t[b] != INT || (right##_TAG) != INT) {                          \
            LEAVE();                                                        \
        }                                                                   \
        v[a].integer = v[b].integer oper right##_VALUE;                     \
        t[a] = BOOL;                                                        \
        NEXT();                                                             \
    }                                                                       \
    STENCIL(name##_fused)                                                   \
    {                                                                       \
        uintptr_t a = HOLE(A), b = HOLE(B);                                 \
        if (t[b] != INT || (right##_TAG) != INT) {                          \
            LEAVE();                                                        \
        }                                                                   \
        bool held = v[b].integer oper right##_VALUE;                        \
        v[a].integer = held;                                                \
        t[a] = BOOL;                                                        \
        if (!held) {                                                        \
            JUMP();                                                         \
        }                                                                   \
        NEXT();                                                             \
    }
#define KINT_TAG INT
#define KINT_VALUE constant().integer
#define RINT_TAG t[HOLE(C)]
#define RINT_VALUE v[HOLE(C)].integer

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
    uintptr_t a = HOLE(A);
    if (t[a] != BOOL) {
        LEAVE();
    }
    if (!v[a].boolean) {
        JUMP();
    }
    NEXT();
}

STENCIL(jump_false_back)
{
    uintptr_t a = HOLE(A);
    if (t[a] != BOOL) {
        LEAVE();
    }
    if (!v[a].boolean) {
        POLL_OR_LEAVE();
        COUNT_TURN();
        JUMP();
    }
    NEXT();
}

// 03 の 5.1改6: integers all three, the loop counts in integers, and an
// overflow is past any integer bound -- the loop is over.
#define FORLOOP(name, overflows, oper)                                      \
    STENCIL(name)                                                           \
    {                                                                       \
        uintptr_t a = HOLE(A);                                              \
        if (t[a] != INT || t[a + 1] != INT || t[a + 2] != INT) {            \
            LEAVE();                                                        \
        }                                                                   \
        POLL_OR_LEAVE();                                                    \
        int64_t focus;                                                      \
        if (overflows(v[a].integer, v[a + 2].integer, &focus)) {            \
            NEXT();                                                         \
        }                                                                   \
        v[a].integer = focus;                                               \
        if (focus oper v[a + 1].integer) {                                  \
            COUNT_TURN();                                                   \
            JUMP();                                                         \
        }                                                                   \
        NEXT();                                                             \
    }

FORLOOP(forloop, __builtin_add_overflow, <=)
FORLOOP(forloopd, __builtin_sub_overflow, >=)
