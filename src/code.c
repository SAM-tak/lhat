// L^ (lhat) -- bytecode: instructions and the chunk they live in.

#include "code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lhat_chunk_init(LhatChunk *chunk)
{
    memset(chunk, 0, sizeof *chunk);
}

void lhat_chunk_dispose(LhatChunk *chunk)
{
    free(chunk->code);
    free(chunk->constants);
    memset(chunk, 0, sizeof *chunk);
}

size_t lhat_chunk_emit(LhatChunk *chunk, LhatInstruction instruction)
{
    if (chunk->count == chunk->capacity) {
        size_t grown = chunk->capacity ? chunk->capacity * 2 : 16;
        LhatInstruction *bigger =
            (LhatInstruction *)realloc(chunk->code, grown * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        chunk->code = bigger;
        chunk->capacity = grown;
    }
    chunk->code[chunk->count] = instruction;
    return chunk->count++;
}

size_t lhat_chunk_constant(LhatChunk *chunk, LhatValue value)
{
    // Reused rather than appended: a loop testing the same literal each time
    // round would otherwise fill the pool, and 5.2 caps it at 65536.
    for (size_t i = 0; i < chunk->constant_count; i++) {
        if (chunk->constants[i].tag == value.tag &&
            lhat_value_equal(chunk->constants[i], value)) {
            return i;
        }
    }

    if (chunk->constant_count == chunk->constant_capacity) {
        size_t grown = chunk->constant_capacity ? chunk->constant_capacity * 2 : 8;
        LhatValue *bigger =
            (LhatValue *)realloc(chunk->constants, grown * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        chunk->constants = bigger;
        chunk->constant_capacity = grown;
    }
    if (chunk->constant_count > 0xFFFF) {
        return SIZE_MAX;
    }
    chunk->constants[chunk->constant_count] = value;
    return chunk->constant_count++;
}

void lhat_chunk_patch_here(LhatChunk *chunk, size_t at)
{
    if (at >= chunk->count) {
        return;
    }
    // 5.2: a jump counts instructions, not bytes, and is measured from the
    // one after itself.
    int32_t offset = (int32_t)(chunk->count - at - 1);
    LhatInstruction instruction = chunk->code[at];
    chunk->code[at] = lhat_encode_jump(lhat_op(instruction),
                                       lhat_a(instruction), offset);
}

const char *lhat_opcode_name(LhatOpcode op)
{
    switch (op) {
        case LHAT_BC_LOADK:       return "loadk";
        case LHAT_BC_LOADNIL:     return "loadnil";
        case LHAT_BC_LOADBOOL:    return "loadbool";
        case LHAT_BC_MOVE:        return "move";
        case LHAT_BC_ADD:         return "add";
        case LHAT_BC_SUB:         return "sub";
        case LHAT_BC_MUL:         return "mul";
        case LHAT_BC_DIV:         return "div";
        case LHAT_BC_IDIV:        return "idiv";
        case LHAT_BC_MOD:         return "mod";
        case LHAT_BC_POW:         return "pow";
        case LHAT_BC_NEG:         return "neg";
        case LHAT_BC_NOT:         return "not";
        case LHAT_BC_EQ:          return "eq";
        case LHAT_BC_NE:          return "ne";
        case LHAT_BC_LT:          return "lt";
        case LHAT_BC_LE:          return "le";
        case LHAT_BC_GT:          return "gt";
        case LHAT_BC_GE:          return "ge";
        case LHAT_BC_JUMP:        return "jump";
        case LHAT_BC_JUMP_FALSE:  return "jumpfalse";
        case LHAT_BC_RETURN:      return "return";
        case LHAT_BC_RETURN_NIL:  return "returnnil";
        case LHAT_BC_COUNT:       break;
    }
    return "?";
}

void lhat_chunk_print(const LhatChunk *chunk, size_t index, char *out,
                      size_t size)
{
    if (index >= chunk->count) {
        snprintf(out, size, "?");
        return;
    }

    LhatInstruction i = chunk->code[index];
    LhatOpcode op = lhat_op(i);
    const char *name = lhat_opcode_name(op);

    switch (op) {
        case LHAT_BC_LOADK:
            snprintf(out, size, "%-10s r%u k%u", name, lhat_a(i), lhat_bx(i));
            break;
        case LHAT_BC_JUMP:
            snprintf(out, size, "%-10s -> %zu", name,
                     index + 1 + (size_t)lhat_jump_offset(i));
            break;
        case LHAT_BC_JUMP_FALSE:
            snprintf(out, size, "%-10s r%u -> %zu", name, lhat_a(i),
                     index + 1 + (size_t)lhat_jump_offset(i));
            break;
        case LHAT_BC_LOADNIL:
        case LHAT_BC_RETURN:
            snprintf(out, size, "%-10s r%u", name, lhat_a(i));
            break;
        case LHAT_BC_RETURN_NIL:
            snprintf(out, size, "%s", name);
            break;
        case LHAT_BC_LOADBOOL:
        case LHAT_BC_MOVE:
        case LHAT_BC_NEG:
        case LHAT_BC_NOT:
            snprintf(out, size, "%-10s r%u r%u", name, lhat_a(i), lhat_b(i));
            break;
        default:
            snprintf(out, size, "%-10s r%u r%u r%u", name, lhat_a(i), lhat_b(i),
                     lhat_c(i));
            break;
    }
}
