// L^ (lhat) -- bytecode: instructions and the chunk they live in.

#include "code.h"

#include "lhat/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "grow.h"
#include "lhat/port.h"

LhatProto *lhat_proto_new(void)
{
    LhatProto *proto = (LhatProto *)lhat_calloc(1, sizeof *proto);
    if (proto != NULL) {
        // See lhat_chunk_init's comment: born black so that a machine
        // reading this proto's constants never writes into it.
        proto->chunk.heap.white = LHAT_GC_BLACK;
    }
    return proto;
}

bool lhat_proto_yields(const LhatProto *proto)
{
    return proto->yields;
}

size_t lhat_proto_upvalue_count(const LhatProto *proto)
{
    return proto->upvalue_count;
}

size_t lhat_proto_parameters(const LhatProto *proto)
{
    return proto->parameters;
}

bool lhat_proto_has_variadic(const LhatProto *proto)
{
    return proto->has_variadic;
}

void lhat_proto_free(LhatProto *proto)
{
    if (proto == NULL) {
        return;
    }
    for (size_t i = 0; i < proto->proto_count; i++) {
        lhat_proto_free(proto->protos[i]);
    }
    lhat_free(proto->debug_name);
    lhat_free(proto->source_name);
    lhat_free(proto->protos);
    lhat_free(proto->upvalues);
    lhat_free(proto->parameter_types);
    if (proto->is_unit && proto->units != NULL) {
        lhat_free((void *)proto->units->protos);
        lhat_free((void *)proto->units);
    }
    lhat_chunk_dispose(&proto->chunk);
    lhat_free(proto);
}

static void point_at_units(LhatProto *proto, const LhatUnitTable *table)
{
    proto->units = table;
    for (size_t i = 0; i < proto->proto_count; i++) {
        point_at_units(proto->protos[i], table);
    }
}

bool lhat_proto_give_units(LhatProto *unit, const LhatProto **protos,
                           size_t count)
{
    LhatUnitTable *table = (LhatUnitTable *)lhat_alloc(sizeof *table);
    if (table == NULL) {
        return false;
    }
    table->protos = protos;
    table->count = count;
    point_at_units(unit, table);
    return true;
}

size_t lhat_proto_add(LhatProto *parent, LhatProto *child)
{
    LHAT_GROW(parent->protos, parent->proto_count, parent->proto_capacity, 4,
              return SIZE_MAX);
    if (parent->proto_count > 0xFFFF) {
        return SIZE_MAX;
    }
    parent->protos[parent->proto_count] = child;
    return parent->proto_count++;
}

size_t lhat_proto_add_upvalue(LhatProto *proto, LhatUpvalueSource source,
                              uint8_t index)
{
    // Reused rather than appended: a name read twice is one shared place, not
    // two, which is what 5.4 means by sharing the location.
    for (size_t i = 0; i < proto->upvalue_count; i++) {
        if (proto->upvalues[i].source == source &&
            proto->upvalues[i].index == index) {
            return i;
        }
    }

    LHAT_GROW(proto->upvalues, proto->upvalue_count, proto->upvalue_capacity,
              4, return SIZE_MAX);
    if (proto->upvalue_count > 0xFF) {
        return SIZE_MAX;
    }
    proto->upvalues[proto->upvalue_count].source = source;
    proto->upvalues[proto->upvalue_count].index = index;
    return proto->upvalue_count++;
}

void lhat_chunk_init(LhatChunk *chunk)
{
    memset(chunk, 0, sizeof *chunk);
    // 5.12 の白は、掃かれる heap がどちらを配っているかを言うためのもの
    // (object.h の LhatHeap.white)。chunk の heap は掃かれることが無い
    // -- 一度作られたら書き換わらない定数だけを持つので、machine の
    // GC サイクルをまたいで生き続ける。白のままだと、実行中の machine が
    // このヒープ上のオブジェクト(誤り種別・文字列定数)へ触れるたびに
    // reach() がその object->color/gclist を書き換えてしまい、複数の
    // machine (= 複数の OS スレッド, std.thread) が同じ chunk を共有する
    // と data race になる。生まれた時点で黒にしておけば reach() は即座に
    // 戻り、何も書き換えない。
    chunk->heap.white = LHAT_GC_BLACK;
}

void lhat_chunk_dispose(LhatChunk *chunk)
{
    lhat_free(chunk->code);
    lhat_free(chunk->lines);
    lhat_free(chunk->constants);
    lhat_object_free_all(&chunk->heap);
    memset(chunk, 0, sizeof *chunk);
}

size_t lhat_chunk_emit(LhatChunk *chunk, LhatInstruction instruction,
                       uint32_t line)
{
    if (chunk->count == chunk->capacity) {
        size_t grown = chunk->capacity ? chunk->capacity * 2 : 16;
        // realloc invalidates the old pointer as soon as it succeeds, so
        // `chunk->code` is updated right away rather than held back until
        // both arrays are known to have grown -- a lines-only failure below
        // then just leaves `capacity` behind, which the next call retries.
        LhatInstruction *bigger =
            (LhatInstruction *)lhat_realloc(chunk->code, grown * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        chunk->code = bigger;
        uint32_t *bigger_lines =
            (uint32_t *)lhat_realloc(chunk->lines, grown * sizeof *bigger_lines);
        if (bigger_lines == NULL) {
            return SIZE_MAX;
        }
        chunk->lines = bigger_lines;
        chunk->capacity = grown;
    }
    chunk->code[chunk->count] = instruction;
    chunk->lines[chunk->count] = line;
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

    LHAT_GROW(chunk->constants, chunk->constant_count,
              chunk->constant_capacity, 8, return SIZE_MAX);
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

    LhatString *string = lhat_string_new(&chunk->heap, text, length);
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
        case LHAT_BC_CONCAT:      return "concat";
        case LHAT_BC_NEG:         return "neg";
        case LHAT_BC_NOT:         return "not";
        case LHAT_BC_EQ:          return "eq";
        case LHAT_BC_SAME:        return "same";
        case LHAT_BC_NE:          return "ne";
        case LHAT_BC_LT:          return "lt";
        case LHAT_BC_LE:          return "le";
        case LHAT_BC_GT:          return "gt";
        case LHAT_BC_GE:          return "ge";
        case LHAT_BC_CLOSURE:     return "closure";
        case LHAT_BC_CALL:        return "call";
        case LHAT_BC_PICKARM:     return "pickarm";
        case LHAT_BC_GETUPVAL:    return "getupval";
        case LHAT_BC_SETUPVAL:    return "setupval";
        case LHAT_BC_CLOSE:       return "close";
        case LHAT_BC_CLOSEONE:    return "closeone";
        case LHAT_BC_THIS:        return "this";
        case LHAT_BC_ENV:         return "env";
        case LHAT_BC_UNIT:        return "unit";
        case LHAT_BC_NEWTABLE:    return "newtable";
        case LHAT_BC_SEAL:        return "seal";
        case LHAT_BC_GETINDEX:    return "getindex";
        case LHAT_BC_SETINDEX:    return "setindex";
        case LHAT_BC_CHECKRUN:    return "checkrun";
        case LHAT_BC_PACK:        return "pack";
        case LHAT_BC_MAKERUN:     return "makerun";
        case LHAT_BC_ADDOVERLOAD: return "addoverload";
        case LHAT_BC_OVERRIDEINDEX: return "overrideindex";
        case LHAT_BC_OVERRIDEARM: return "overridearm";
        case LHAT_BC_NEWERROR:    return "newerror";
        case LHAT_BC_ISERROR:     return "iserror";
        case LHAT_BC_ISA:         return "isa";
        case LHAT_BC_ISNIL:       return "isnil";
        case LHAT_BC_NEWINSTANCE: return "newinstance";
        case LHAT_BC_SETPROTO:    return "setproto";
        case LHAT_BC_BOX:         return "box";
        case LHAT_BC_CALLMETHOD:  return "callmethod";
        case LHAT_BC_TAILCALL:    return "tailcall";
        case LHAT_BC_TAILCALLMETHOD: return "tailcallmethod";
        case LHAT_BC_PUSHCLEANUP: return "pushcleanup";
        case LHAT_BC_POPCLEANUP:  return "popcleanup";
        case LHAT_BC_ENDCLEANUP:  return "endcleanup";
        case LHAT_BC_JUMP:        return "jump";
        case LHAT_BC_JUMP_FALSE:  return "jumpfalse";
        case LHAT_BC_YIELD:       return "yield";
        case LHAT_BC_RESUME:      return "resume";
        case LHAT_BC_ISDONE:      return "isdone";
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
        case LHAT_BC_UNIT:
            snprintf(out, size, "%-10s r%u u%u", name, lhat_a(i), lhat_bx(i));
            break;
        case LHAT_BC_CLOSURE:
            snprintf(out, size, "%-10s r%u p%u", name, lhat_a(i), lhat_bx(i));
            break;
        case LHAT_BC_CALL:
        case LHAT_BC_CALLMETHOD:
        case LHAT_BC_TAILCALL:
        case LHAT_BC_TAILCALLMETHOD: {
            // 13.8改: C's high bits say how many slots the answer was given
            // room in. Shown only when it is a run, so every call written
            // before tuples reads exactly as it did. 5.3: a tail call that
            // throws the answer away says so beside the count.
            unsigned prepared = lhat_call_prepared(lhat_c(i));
            const char *dropped =
                (lhat_c(i) & LHAT_CALL_DROP) != 0 ? ", dropped" : "";
            if (prepared > 1) {
                snprintf(out, size, "%-10s r%u (%u args, %u slots%s)", name,
                         lhat_a(i), lhat_b(i), prepared, dropped);
            } else {
                snprintf(out, size, "%-10s r%u (%u args%s)", name, lhat_a(i),
                         lhat_b(i), dropped);
            }
            break;
        }
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
        case LHAT_BC_CLOSEONE:
        case LHAT_BC_THIS:
        case LHAT_BC_ENV:
        case LHAT_BC_NEWTABLE:
        case LHAT_BC_SEAL:
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
        case LHAT_BC_SETPROTO:
        case LHAT_BC_BOX:
        case LHAT_BC_RESUME:
        case LHAT_BC_ISDONE:
            snprintf(out, size, "%-10s r%u r%u", name, lhat_a(i), lhat_b(i));
            break;
        default:
            snprintf(out, size, "%-10s r%u r%u r%u", name, lhat_a(i), lhat_b(i),
                     lhat_c(i));
            break;
    }
}
