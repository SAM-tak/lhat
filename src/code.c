// L^ (lhat) -- bytecode: instructions and the chunk they live in.

#include "code.h"

#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LhatProto *lhat_proto_new(void)
{
    LhatProto *proto = (LhatProto *)calloc(1, sizeof *proto);
    return proto;
}

void lhat_proto_free(LhatProto *proto)
{
    if (proto == NULL) {
        return;
    }
    for (size_t i = 0; i < proto->proto_count; i++) {
        lhat_proto_free(proto->protos[i]);
    }
    free(proto->protos);
    free(proto->upvalues);
    lhat_chunk_dispose(&proto->chunk);
    free(proto);
}

size_t lhat_proto_add(LhatProto *parent, LhatProto *child)
{
    if (parent->proto_count == parent->proto_capacity) {
        size_t grown = parent->proto_capacity ? parent->proto_capacity * 2 : 4;
        LhatProto **bigger =
            (LhatProto **)realloc(parent->protos, grown * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        parent->protos = bigger;
        parent->proto_capacity = grown;
    }
    if (parent->proto_count > 0xFFFF) {
        return SIZE_MAX;
    }
    parent->protos[parent->proto_count] = child;
    return parent->proto_count++;
}

size_t lhat_proto_add_upvalue(LhatProto *proto, bool from_parent_register,
                              uint8_t index)
{
    // Reused rather than appended: a name read twice is one shared place, not
    // two, which is what 5.4 means by sharing the location.
    for (size_t i = 0; i < proto->upvalue_count; i++) {
        if (proto->upvalues[i].from_parent_register == from_parent_register &&
            proto->upvalues[i].index == index) {
            return i;
        }
    }

    if (proto->upvalue_count == proto->upvalue_capacity) {
        size_t grown = proto->upvalue_capacity ? proto->upvalue_capacity * 2 : 4;
        LhatUpvalueDesc *bigger =
            (LhatUpvalueDesc *)realloc(proto->upvalues, grown * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        proto->upvalues = bigger;
        proto->upvalue_capacity = grown;
    }
    if (proto->upvalue_count > 0xFF) {
        return SIZE_MAX;
    }
    proto->upvalues[proto->upvalue_count].from_parent_register =
        from_parent_register;
    proto->upvalues[proto->upvalue_count].index = index;
    return proto->upvalue_count++;
}

void lhat_chunk_init(LhatChunk *chunk)
{
    memset(chunk, 0, sizeof *chunk);
}

void lhat_chunk_dispose(LhatChunk *chunk)
{
    free(chunk->code);
    free(chunk->constants);
    lhat_object_free_all(&chunk->objects);
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

size_t lhat_chunk_string(LhatChunk *chunk, const char *text, size_t length)
{
    // Looked for before it is made, so a literal written twice does not put
    // two strings on the chunk. lhat_chunk_constant would find the duplicate
    // anyway, but only after allocating the one it would then abandon.
    for (size_t i = 0; i < chunk->constant_count; i++) {
        LhatValue held = chunk->constants[i];
        if (!lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
            continue;
        }
        const LhatString *string = (const LhatString *)lhat_as_object(held);
        if (string->length == length &&
            memcmp(string->text, text, length) == 0) {
            return i;
        }
    }

    LhatString *string = lhat_string_new(&chunk->objects, text, length);
    if (string == NULL) {
        return SIZE_MAX;
    }
    return lhat_chunk_constant(chunk, lhat_object((LhatObject *)string));
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
        case LHAT_BC_CLOSURE:     return "closure";
        case LHAT_BC_CALL:        return "call";
        case LHAT_BC_GETUPVAL:    return "getupval";
        case LHAT_BC_SETUPVAL:    return "setupval";
        case LHAT_BC_CLOSE:       return "close";
        case LHAT_BC_NEWTABLE:    return "newtable";
        case LHAT_BC_GETINDEX:    return "getindex";
        case LHAT_BC_SETINDEX:    return "setindex";
        case LHAT_BC_NEWERROR:    return "newerror";
        case LHAT_BC_ISERROR:     return "iserror";
        case LHAT_BC_ISKIND:      return "iskind";
        case LHAT_BC_ISNIL:       return "isnil";
        case LHAT_BC_NEWINSTANCE: return "newinstance";
        case LHAT_BC_CALLMETHOD:  return "callmethod";
        case LHAT_BC_PUSHCLEANUP: return "pushcleanup";
        case LHAT_BC_POPCLEANUP:  return "popcleanup";
        case LHAT_BC_ENDCLEANUP:  return "endcleanup";
        case LHAT_BC_JUMP:        return "jump";
        case LHAT_BC_JUMP_FALSE:  return "jumpfalse";
        case LHAT_BC_YIELD:       return "yield";
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
        case LHAT_BC_CLOSURE:
            snprintf(out, size, "%-10s r%u p%u", name, lhat_a(i), lhat_bx(i));
            break;
        case LHAT_BC_CALL:
        case LHAT_BC_CALLMETHOD:
            snprintf(out, size, "%-10s r%u (%u args)", name, lhat_a(i),
                     lhat_b(i));
            break;
        case LHAT_BC_GETUPVAL:
        case LHAT_BC_SETUPVAL:
            snprintf(out, size, "%-10s r%u u%u", name, lhat_a(i), lhat_b(i));
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
        case LHAT_BC_CLOSE:
        case LHAT_BC_NEWTABLE:
        case LHAT_BC_POPCLEANUP:
        case LHAT_BC_YIELD:
        case LHAT_BC_RETURN:
            snprintf(out, size, "%-10s r%u", name, lhat_a(i));
            break;
        case LHAT_BC_RETURN_NIL:
        case LHAT_BC_ENDCLEANUP:
            snprintf(out, size, "%s", name);
            break;
        case LHAT_BC_PUSHCLEANUP:
            snprintf(out, size, "%-10s -> %u", name, lhat_bx(i));
            break;
        case LHAT_BC_LOADBOOL:
        case LHAT_BC_MOVE:
        case LHAT_BC_NEG:
        case LHAT_BC_NOT:
        case LHAT_BC_NEWERROR:
        case LHAT_BC_ISERROR:
        case LHAT_BC_ISNIL:
        case LHAT_BC_NEWINSTANCE:
            snprintf(out, size, "%-10s r%u r%u", name, lhat_a(i), lhat_b(i));
            break;
        default:
            snprintf(out, size, "%-10s r%u r%u r%u", name, lhat_a(i), lhat_b(i),
                     lhat_c(i));
            break;
    }
}
