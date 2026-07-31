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

    uint8_t registers;
} LhatChunk;

void lhat_chunk_init(LhatChunk *chunk);
void lhat_chunk_dispose(LhatChunk *chunk);

// Returns the index of the instruction, so a jump written later can be
// patched by the caller that recorded it.
size_t lhat_chunk_emit(LhatChunk *chunk, LhatInstruction instruction);

// Adds a constant, reusing an equal one rather than storing it twice.
// Returns the index, or SIZE_MAX when the pool is full or out of memory.
size_t lhat_chunk_constant(LhatChunk *chunk, LhatValue value);

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
