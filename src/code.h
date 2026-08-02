// L^ (lhat) -- bytecode: instructions and the chunk they live in.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// 5.2: a register machine with one 32-bit word per instruction. Decoding is a
// shift and a mask, and a jump target is an instruction count rather than a
// byte offset, because every instruction is the same size.
//
// 5.1: every instruction here checks the types of what it is given. The
// specialised forms that skip the check come later, once the generic ones
// have settled, so nothing in this file assumes the checker ran.

#ifndef LHAT_CODE_H
#define LHAT_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object.h"
#include "value.h"

typedef uint32_t LhatInstruction;

typedef enum {
    // A = destination unless said otherwise.
    LHAT_BC_LOADK,      // A Bx  R[A] = K[Bx]
    LHAT_BC_LOADNIL,    // A     R[A] = nil^
    LHAT_BC_LOADBOOL,   // A B   R[A] = (B != 0)
    LHAT_BC_MOVE,       // A B   R[A] = R[B]

    LHAT_BC_ADD,        // A B C R[A] = R[B] + R[C]
    LHAT_BC_SUB,
    LHAT_BC_MUL,
    LHAT_BC_DIV,        // 04 の 11.2: real division, so this cannot fail
    LHAT_BC_IDIV,       // 04 の 11.2: fails on zero
    LHAT_BC_MOD,        // the same
    LHAT_BC_POW,
    LHAT_BC_CONCAT,     // A B C R[A] = R[B] .. R[C]  (02 の 11.2)
    LHAT_BC_NEG,        // A B   R[A] = -R[B]
    LHAT_BC_NOT,        // A B   R[A] = !R[B]

    LHAT_BC_EQ,         // A B C R[A] = R[B] = R[C]
    LHAT_BC_NE,
    LHAT_BC_LT,
    LHAT_BC_LE,
    LHAT_BC_GT,
    LHAT_BC_GE,

    LHAT_BC_JUMP,       // Bx    signed, relative to the next instruction
    LHAT_BC_JUMP_FALSE, // A Bx  jump when R[A] is false

    // 5.3: arguments sit above the callee in the caller's frame, and exactly
    // one value comes back -- 02 の 13.8 left nothing to reconcile.
    LHAT_BC_CLOSURE,    // A Bx  R[A] = closure of protos[Bx]
    LHAT_BC_CALL,       // A B   R[A] = R[A](R[A+1] .. R[A+B])
    LHAT_BC_GETUPVAL,   // A B   R[A] = *upvalue[B]
    LHAT_BC_SETUPVAL,   // A B   *upvalue[B] = R[A]
    LHAT_BC_CLOSE,      // A     the places at R[A] and above stop being shared
    LHAT_BC_THIS,       // A     R[A] = the subroutine running (02 の 15.10)

    // 02 の 14 章: the one data structure. 04 の 11.3 makes a missing key
    // nil^ rather than a failure, so GETINDEX cannot fail on that account.
    LHAT_BC_NEWTABLE,   // A     R[A] = { }
    LHAT_BC_GETINDEX,   // A B C R[A] = R[B][R[C]]
    LHAT_BC_SETINDEX,   // A B C R[A][R[B]] = R[C]
    LHAT_BC_ADDOVERLOAD,// A B C R[A][R[B]] gains R[C] as another way to call it

    // 04. An error carries its kind and a table of fields, and the tests are
    // instructions because 5.1 keeps the machine independent of the checker.
    LHAT_BC_NEWERROR,   // A B   R[A] = an error of the kind in R[B]
    LHAT_BC_ISERROR,    // A B   R[A] = R[B] is an error of any kind
    LHAT_BC_ISKIND,     // A B C R[A] = R[B] is an error of the kind in R[C]
    LHAT_BC_ISNIL,      // A B   R[A] = R[B] is nil^   (02 の 11.7)

    // 02 の 14 章. A definition is a table of shared members; an instance is
    // a table that reads them through a link fixed when it was made (14.2).
    LHAT_BC_NEWINSTANCE,  // A B   R[A] = an instance of the definition R[B]
    LHAT_BC_CALLMETHOD,   // A B   R[A] = R[A](R[A+1] .. R[A+B]), where R[A+1]
                          //       is the receiver and is passed only when the
                          //       callee takes self^ (14.4)

    // 5.5: the frame holds the cleanups it has not run, and every exit drains
    // them. One instruction pushes, one drains, one ends a cleanup body -- so
    // a finally^ and a with^ are the same thing to the machine.
    LHAT_BC_PUSHCLEANUP,  // Bx  remember the body at Bx (an instruction index)
    LHAT_BC_POPCLEANUP,   // A   run the cleanups above depth A, innermost first
    LHAT_BC_ENDCLEANUP,   //     the end of a cleanup body

    // 02 の 15.4: bidirectional. The value goes out and the one the resume
    // supplies comes back into the same register.
    LHAT_BC_YIELD,      // A     yield R[A]; R[A] = what the resume sent

    // 02 の 15.8: delegation, which compiles to the loop 5.7 writes out.
    LHAT_BC_RESUME,     // A B   R[A] = resume the coroutine R[B], sending R[A]
    LHAT_BC_ISDONE,     // A B   R[A] = the coroutine R[B] has finished

    LHAT_BC_RETURN,     // A     return R[A]
    LHAT_BC_RETURN_NIL,

    LHAT_BC_COUNT
} LhatOpcode;

// 5.2. Bx is signed for a jump and unsigned everywhere else; the two are read
// through different accessors rather than stored differently.
#define LHAT_BX_BIAS 32768

static inline LhatInstruction lhat_encode_abc(LhatOpcode op, uint8_t a,
                                              uint8_t b, uint8_t c)
{
    return (LhatInstruction)op | ((LhatInstruction)a << 8) |
           ((LhatInstruction)b << 16) | ((LhatInstruction)c << 24);
}

static inline LhatInstruction lhat_encode_abx(LhatOpcode op, uint8_t a,
                                              uint16_t bx)
{
    return (LhatInstruction)op | ((LhatInstruction)a << 8) |
           ((LhatInstruction)bx << 16);
}

static inline LhatInstruction lhat_encode_jump(LhatOpcode op, uint8_t a,
                                               int32_t offset)
{
    return lhat_encode_abx(op, a, (uint16_t)(offset + LHAT_BX_BIAS));
}

static inline LhatOpcode lhat_op(LhatInstruction i) { return (LhatOpcode)(i & 0xFF); }
static inline uint8_t lhat_a(LhatInstruction i)  { return (uint8_t)(i >> 8); }
static inline uint8_t lhat_b(LhatInstruction i)  { return (uint8_t)(i >> 16); }
static inline uint8_t lhat_c(LhatInstruction i)  { return (uint8_t)(i >> 24); }
static inline uint16_t lhat_bx(LhatInstruction i) { return (uint16_t)(i >> 16); }
static inline int32_t lhat_jump_offset(LhatInstruction i)
{
    return (int32_t)lhat_bx(i) - LHAT_BX_BIAS;
}

// One compiled body: its instructions, the constants they name, and how many
// registers a frame needs. 5.2 fixes the frame size at compile time.
typedef struct {
    LhatInstruction *code;
    size_t count;
    size_t capacity;

    LhatValue *constants;
    size_t constant_count;
    size_t constant_capacity;

    // The strings the constants name. A constant lives as long as the chunk,
    // so the chunk owns them rather than the machine that runs it.
    LhatHeap heap;

    uint8_t registers;
} LhatChunk;

// 5.4: how a closure gets each of the places it shares. Either a register of
// the frame making it, or one of that frame's own upvalues -- the second is
// what carries a name down through more than one level of nesting.
typedef struct {
    bool from_parent_register;
    uint8_t index;
} LhatUpvalueDesc;

// One compiled subroutine: its own code, the bodies written inside it, and
// what it captures. 02 の 14.9 keeps the name out of this; a proto is the
// shape of a body, not a named thing.
typedef struct LhatProto {
    LhatChunk chunk;

    // 03 の 4.3: how many registers already hold values when this unit
    // starts. Zero everywhere but the second and later inputs of a REPL,
    // where the top-level names of the earlier ones are still in them.
    uint8_t reserved;

    uint8_t parameters;
    bool is_function;  // f^ rather than p^ (02 の 15 章)
    bool yields;       // 02 の 15.2: the body contains yield^, so calling it
                       // answers a coroutine rather than running it (15.5)
    bool takes_self;   // 02 の 14.4: the first parameter is written self^,
                       // which is what makes it an instance method

    struct LhatProto **protos;
    size_t proto_count;
    size_t proto_capacity;

    LhatUpvalueDesc *upvalues;
    size_t upvalue_count;
    size_t upvalue_capacity;

    // 02 の 14.12: what each parameter was declared to take, kept so that an
    // overloaded call can find the candidate that fits. NULL where nothing
    // was written, which asks nothing.
    struct LhatRuntimeType **parameter_types;
} LhatProto;

LhatProto *lhat_proto_new(void);
void lhat_proto_free(LhatProto *proto);

// Returns the index of the nested body, or SIZE_MAX when out of memory.
size_t lhat_proto_add(LhatProto *parent, LhatProto *child);
size_t lhat_proto_add_upvalue(LhatProto *proto, bool from_parent_register,
                              uint8_t index);

void lhat_chunk_init(LhatChunk *chunk);
void lhat_chunk_dispose(LhatChunk *chunk);

// Returns the index of the instruction, so a jump written later can be
// patched by the caller that recorded it.
size_t lhat_chunk_emit(LhatChunk *chunk, LhatInstruction instruction);

// Adds a constant, reusing an equal one rather than storing it twice.
// Returns the index, or SIZE_MAX when the pool is full or out of memory.
size_t lhat_chunk_constant(LhatChunk *chunk, LhatValue value);

// The same for a string literal: the bytes are copied into a string the chunk
// owns. Two literals spelling the same thing share one constant, which is
// what makes t.foo and t["foo"] one key.
size_t lhat_chunk_string(LhatChunk *chunk, const char *text, size_t length);

// Rewrites the Bx of the jump at `at` so that it lands on the instruction
// after the last one emitted. Used for a branch whose target is not known
// until its body has been compiled.
void lhat_chunk_patch_here(LhatChunk *chunk, size_t at);

const char *lhat_opcode_name(LhatOpcode op);

// Writes one instruction in a readable form. `index` is what a jump is
// measured against.
void lhat_chunk_print(const LhatChunk *chunk, size_t index, char *out,
                      size_t size);

#endif  // LHAT_CODE_H
