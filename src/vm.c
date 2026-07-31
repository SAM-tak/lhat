// L^ (lhat) -- compiling a tree to bytecode, and running it.

#include "vm.h"

#include <string.h>

#define LHAT_MAX_REGISTERS 250
#define LHAT_MAX_LOCALS 200

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    size_t length;
    uint8_t reg;  // 5.2: a name is a slot in the frame like anything else
} Local;

typedef struct {
    const LhatLexer *lexer;
    LhatChunk *chunk;
    LhatCompileStatus status;

    Local locals[LHAT_MAX_LOCALS];
    size_t local_count;

    // Slots below this hold live names; everything above is scratch for the
    // expression being compiled, released as soon as it is consumed.
    uint8_t next_register;
} Compiler;

static void fail(Compiler *c, LhatCompileStatus status)
{
    if (c->status == LHAT_COMPILE_OK) {
        c->status = status;
    }
}

// The trailing hats are not part of a name (01 の 2.3).
static bool node_name(const Compiler *c, const LhatNode *node,
                      const char **text, size_t *length)
{
    if (node == NULL) {
        return false;
    }
    if (node->kind == LHAT_NODE_IDENT || node->kind == LHAT_NODE_HAT_IDENT) {
        *text = c->lexer->source->text + node->v.name.offset;
        *length = node->v.name.length >= node->v.name.hats
                      ? node->v.name.length - node->v.name.hats
                      : node->v.name.length;
        return true;
    }
    if (node->kind == LHAT_NODE_FOCUS) {
        *text = "it";
        *length = 2;
        return true;
    }
    return false;
}

static bool name_is(const char *text, size_t length, const char *literal)
{
    size_t n = strlen(literal);
    return length == n && memcmp(text, literal, n) == 0;
}

static uint8_t reserve(Compiler *c)
{
    if (c->next_register >= LHAT_MAX_REGISTERS) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return 0;
    }
    uint8_t r = c->next_register++;
    if (c->next_register > c->chunk->registers) {
        c->chunk->registers = c->next_register;
    }
    return r;
}

static void emit(Compiler *c, LhatInstruction instruction)
{
    if (lhat_chunk_emit(c->chunk, instruction) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
}

static size_t emit_jump(Compiler *c, LhatOpcode op, uint8_t a)
{
    size_t at = lhat_chunk_emit(c->chunk, lhat_encode_jump(op, a, 0));
    if (at == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
    return at;
}

static void load_constant(Compiler *c, uint8_t into, LhatValue value)
{
    size_t k = lhat_chunk_constant(c->chunk, value);
    if (k == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

static const Local *find_local(const Compiler *c, const char *name,
                               size_t length)
{
    // Backwards, so an inner name shadows an outer one of the same spelling
    // as 02 の 8.6 intends.
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->length == length && memcmp(local->name, name, length) == 0) {
            return local;
        }
    }
    return NULL;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into);
static void compile_statement(Compiler *c, const LhatNode *node);

static bool binary_opcode(LhatOpKind op, LhatOpcode *out)
{
    // The label is a token operator (01 の 7.1); the value is an opcode.
    switch (op) {
        case LHAT_OP_ADD:      *out = LHAT_BC_ADD;  return true;
        case LHAT_OP_SUB:      *out = LHAT_BC_SUB;  return true;
        case LHAT_OP_MUL:      *out = LHAT_BC_MUL;  return true;
        case LHAT_OP_DIV:      *out = LHAT_BC_DIV;  return true;
        case LHAT_OP_FLOORDIV: *out = LHAT_BC_IDIV; return true;
        case LHAT_OP_MOD:      *out = LHAT_BC_MOD;  return true;
        case LHAT_OP_POW:      *out = LHAT_BC_POW;  return true;
        case LHAT_OP_EQ:       *out = LHAT_BC_EQ;   return true;
        case LHAT_OP_NE:       *out = LHAT_BC_NE;   return true;
        case LHAT_OP_LT:       *out = LHAT_BC_LT;   return true;
        case LHAT_OP_GT:       *out = LHAT_BC_GT;   return true;
        case LHAT_OP_LE:       *out = LHAT_BC_LE;   return true;
        case LHAT_OP_GE:       *out = LHAT_BC_GE;   return true;
        default: return false;
    }
}

static void compile_binary(Compiler *c, const LhatNode *node, uint8_t into)
{
    LhatOpKind op = node->v.binary.op;

    // 11.6: and^ and or^ decide without evaluating the right side when the
    // left has settled it, so they are branches rather than instructions.
    if (op == LHAT_OP_AND || op == LHAT_OP_OR) {
        compile_expression(c, node->v.binary.left, into);
        if (op == LHAT_OP_OR) {
            // Skip the right side when the left is already true: jump over a
            // jump, since the only test instruction asks about false.
            size_t to_right = emit_jump(c, LHAT_BC_JUMP_FALSE, into);
            size_t done = emit_jump(c, LHAT_BC_JUMP, 0);
            lhat_chunk_patch_here(c->chunk, to_right);
            compile_expression(c, node->v.binary.right, into);
            lhat_chunk_patch_here(c->chunk, done);
            return;
        }
        size_t done = emit_jump(c, LHAT_BC_JUMP_FALSE, into);
        compile_expression(c, node->v.binary.right, into);
        lhat_chunk_patch_here(c->chunk, done);
        return;
    }

    LhatOpcode opcode;
    if (!binary_opcode(op, &opcode)) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // Operands go above the names, and the scratch is given back as soon as
    // the instruction has consumed it.
    uint8_t mark = c->next_register;
    uint8_t left = reserve(c);
    uint8_t right = reserve(c);
    compile_expression(c, node->v.binary.left, left);
    compile_expression(c, node->v.binary.right, right);
    emit(c, lhat_encode_abc(opcode, into, left, right));
    c->next_register = mark;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node == NULL || c->status != LHAT_COMPILE_OK) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_INT:
            load_constant(c, into, lhat_integer((int64_t)node->v.integer.value));
            return;

        case LHAT_NODE_FLOAT:
            load_constant(c, into, lhat_real(node->v.real));
            return;

        case LHAT_NODE_IDENT:
        case LHAT_NODE_FOCUS: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            const Local *local = find_local(c, name, length);
            if (local == NULL) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, local->reg, 0));
            return;
        }

        case LHAT_NODE_HAT_IDENT: {
            // 01 の 2.2: a few hat identifiers are values.
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            if (name_is(name, length, "true") || name_is(name, length, "false")) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, into,
                                        name_is(name, length, "true") ? 1 : 0, 0));
                return;
            }
            if (name_is(name, length, "nil")) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                return;
            }
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }

        case LHAT_NODE_UNARY: {
            uint8_t mark = c->next_register;
            uint8_t operand = reserve(c);
            compile_expression(c, node->v.unary.operand, operand);
            emit(c, lhat_encode_abc(node->v.unary.op == LHAT_OP_NOT
                                        ? LHAT_BC_NOT
                                        : LHAT_BC_NEG,
                                    into, operand, 0));
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_BINARY:
            compile_binary(c, node, into);
            return;

        case LHAT_NODE_IF_EXPR: {
            // 5.1 の (5.2): every clause writes into the same register, so
            // the value of the whole expression is wherever control lands.
            size_t leaving[LHAT_MAX_LOCALS];
            size_t leaving_count = 0;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    compile_expression(c, clause->v.clause.body, into);
                    break;
                }

                uint8_t mark = c->next_register;
                uint8_t test = reserve(c);
                compile_expression(c, condition, test);
                size_t next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = mark;

                compile_expression(c, clause->v.clause.body, into);
                if (leaving_count < LHAT_MAX_LOCALS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(c->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(c->chunk, leaving[i]);
            }
            return;
        }

        default:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
    }
}

// 02 の 8.6: let^ makes a name, and the name is a slot that stays.
static void compile_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const LhatNode *named =
            target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;

        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, named, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        if (c->local_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }

        uint8_t slot = reserve(c);
        if (value != NULL) {
            compile_expression(c, value, slot);
            value = value->next;
        } else {
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
        }

        // Added after the value is compiled, so 'let^ x = x' reads the outer
        // x rather than the one being made -- 8.7 makes the name visible but
        // not readable until its let^ has run.
        Local *local = &c->locals[c->local_count++];
        local->name = name;
        local->length = length;
        local->reg = slot;
    }
}

static void compile_reassign(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, target, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        const Local *local = find_local(c, name, length);
        if (local == NULL) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        if (value != NULL) {
            compile_expression(c, value, local->reg);
            value = value->next;
        }
    }
}

static void compile_statements(Compiler *c, const LhatNode *statements)
{
    // A block's names end with it, and so do their slots.
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        compile_statement(c, s);
    }

    c->local_count = local_mark;
    c->next_register = register_mark;
}

static void compile_statement(Compiler *c, const LhatNode *node)
{
    if (node == NULL || c->status != LHAT_COMPILE_OK) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
            compile_define(c, node);
            return;

        case LHAT_NODE_REASSIGN:
            compile_reassign(c, node);
            return;

        case LHAT_NODE_BLOCK:
            compile_statements(c, node->v.list.items);
            return;

        case LHAT_NODE_RETURN: {
            if (node->v.jump.value == NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->v.jump.value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_RETURN, slot, 0, 0));
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_IF_STMT: {
            size_t leaving[LHAT_MAX_LOCALS];
            size_t leaving_count = 0;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    compile_statement(c, clause->v.clause.body);
                    break;
                }

                uint8_t mark = c->next_register;
                uint8_t test = reserve(c);
                compile_expression(c, condition, test);
                size_t next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = mark;

                compile_statement(c, clause->v.clause.body);
                if (clause->next != NULL && leaving_count < LHAT_MAX_LOCALS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(c->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(c->chunk, leaving[i]);
            }
            return;
        }

        case LHAT_NODE_MODULE:
            return;  // 05 の 3 章: a name for the unit, nothing to run

        default:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
    }
}

LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatChunk *chunk)
{
    Compiler c;
    memset(&c, 0, sizeof c);
    c.lexer = lexer;
    c.chunk = chunk;
    c.status = LHAT_COMPILE_OK;

    if (unit == NULL || lexer == NULL) {
        return LHAT_COMPILE_UNSUPPORTED;
    }

    for (const LhatNode *s = unit->v.list.items; s != NULL; s = s->next) {
        compile_statement(&c, s);
    }
    emit(&c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));
    return c.status;
}

const char *lhat_compile_status_message(LhatCompileStatus status)
{
    switch (status) {
        case LHAT_COMPILE_OK:          return "compiled";
        case LHAT_COMPILE_UNSUPPORTED: return "this form does not compile yet";
        case LHAT_COMPILE_TOO_COMPLEX: return "too many registers or constants";
        case LHAT_COMPILE_UNDEFINED:   return "no such name";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

static LhatRunResult fault(LhatRunStatus status, size_t at)
{
    LhatRunResult result;
    result.status = status;
    result.value = lhat_nil();
    result.at = at;
    return result;
}

// 5.1: the generic form checks. 02 の 14.8 makes number^ one type with two
// representations, so an operation stays in integers when both sides are and
// widens only when one of them is real.
static bool arithmetic(LhatOpcode op, LhatValue left, LhatValue right,
                       LhatValue *out, LhatRunStatus *status)
{
    if (!lhat_is_number(left) || !lhat_is_number(right)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }

    bool exact = lhat_is_integer(left) && lhat_is_integer(right);
    double a = lhat_number_as_real(left);
    double b = lhat_number_as_real(right);

    switch (op) {
        case LHAT_BC_ADD:
            *out = exact ? lhat_integer(lhat_as_integer(left) +
                                        lhat_as_integer(right))
                         : lhat_real(a + b);
            return true;
        case LHAT_BC_SUB:
            *out = exact ? lhat_integer(lhat_as_integer(left) -
                                        lhat_as_integer(right))
                         : lhat_real(a - b);
            return true;
        case LHAT_BC_MUL:
            *out = exact ? lhat_integer(lhat_as_integer(left) *
                                        lhat_as_integer(right))
                         : lhat_real(a * b);
            return true;
        case LHAT_BC_DIV:
            // 04 の 11.2: real division, so a zero divisor gives inf rather
            // than failing. That is what keeps ordinary arithmetic out of the
            // unions.
            *out = lhat_real(a / b);
            return true;
        case LHAT_BC_IDIV:
        case LHAT_BC_MOD: {
            if (b == 0) {
                *status = LHAT_RUN_DIVIDE_BY_ZERO;
                return false;
            }
            if (exact) {
                int64_t x = lhat_as_integer(left);
                int64_t y = lhat_as_integer(right);
                int64_t quotient = x / y;
                int64_t remainder = x % y;
                // Floor rather than truncate, so that '%' agrees in sign with
                // the divisor as it does in Lua.
                if (remainder != 0 && ((remainder < 0) != (y < 0))) {
                    quotient--;
                    remainder += y;
                }
                *out = op == LHAT_BC_IDIV ? lhat_integer(quotient)
                                          : lhat_integer(remainder);
                return true;
            }
            double quotient = a / b;
            double floored = quotient - (quotient < 0 ? 1.0 : 0.0);
            double truncated = (double)(int64_t)quotient;
            floored = quotient < 0 && truncated != quotient ? truncated - 1
                                                            : truncated;
            *out = op == LHAT_BC_IDIV ? lhat_real(floored)
                                      : lhat_real(a - floored * b);
            return true;
        }
        case LHAT_BC_POW: {
            double r = 1.0;
            *out = lhat_real(r);
            double base = a;
            double exponent = b;
            // Kept out of libm so the core has no maths dependency yet; only
            // whole exponents are wanted before the standard library lands.
            int64_t times = (int64_t)exponent;
            if ((double)times != exponent) {
                *status = LHAT_RUN_TYPE_ERROR;
                return false;
            }
            bool invert = times < 0;
            if (invert) {
                times = -times;
            }
            for (int64_t i = 0; i < times; i++) {
                r *= base;
            }
            *out = lhat_real(invert ? 1.0 / r : r);
            return true;
        }
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

static bool ordering(LhatOpcode op, LhatValue left, LhatValue right,
                     bool *out, LhatRunStatus *status)
{
    if (!lhat_is_number(left) || !lhat_is_number(right)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }
    double a = lhat_number_as_real(left);
    double b = lhat_number_as_real(right);
    switch (op) {
        case LHAT_BC_LT: *out = a < b;  return true;
        case LHAT_BC_LE: *out = a <= b; return true;
        case LHAT_BC_GT: *out = a > b;  return true;
        case LHAT_BC_GE: *out = a >= b; return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

LhatRunResult lhat_run(const LhatChunk *chunk)
{
    LhatValue registers[LHAT_MAX_REGISTERS + 8];
    for (size_t i = 0; i < sizeof registers / sizeof registers[0]; i++) {
        registers[i] = lhat_nil();
    }

    size_t pc = 0;
    while (pc < chunk->count) {
        LhatInstruction instruction = chunk->code[pc];
        size_t at = pc++;

        uint8_t a = lhat_a(instruction);
        uint8_t b = lhat_b(instruction);
        uint8_t cc = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);

        switch (op) {
            case LHAT_BC_LOADK:
                registers[a] = chunk->constants[lhat_bx(instruction)];
                break;
            case LHAT_BC_LOADNIL:
                registers[a] = lhat_nil();
                break;
            case LHAT_BC_LOADBOOL:
                registers[a] = lhat_bool(b != 0);
                break;
            case LHAT_BC_MOVE:
                registers[a] = registers[b];
                break;

            case LHAT_BC_ADD:
            case LHAT_BC_SUB:
            case LHAT_BC_MUL:
            case LHAT_BC_DIV:
            case LHAT_BC_IDIV:
            case LHAT_BC_MOD:
            case LHAT_BC_POW: {
                LhatValue out;
                LhatRunStatus status = LHAT_RUN_OK;
                if (!arithmetic(op, registers[b], registers[cc], &out,
                                &status)) {
                    return fault(status, at);
                }
                registers[a] = out;
                break;
            }

            case LHAT_BC_NEG: {
                if (!lhat_is_number(registers[b])) {
                    return fault(LHAT_RUN_TYPE_ERROR, at);
                }
                registers[a] = lhat_is_integer(registers[b])
                                   ? lhat_integer(-lhat_as_integer(registers[b]))
                                   : lhat_real(-lhat_as_real(registers[b]));
                break;
            }

            case LHAT_BC_NOT: {
                // 02 の 8.6's condition rule: only a bool is a truth value,
                // so this refuses anything else rather than inventing one.
                if (!lhat_is_bool(registers[b])) {
                    return fault(LHAT_RUN_TYPE_ERROR, at);
                }
                registers[a] = lhat_bool(!lhat_as_bool(registers[b]));
                break;
            }

            case LHAT_BC_EQ:
                registers[a] = lhat_bool(
                    lhat_value_equal(registers[b], registers[cc]));
                break;
            case LHAT_BC_NE:
                registers[a] = lhat_bool(
                    !lhat_value_equal(registers[b], registers[cc]));
                break;

            case LHAT_BC_LT:
            case LHAT_BC_LE:
            case LHAT_BC_GT:
            case LHAT_BC_GE: {
                bool out = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (!ordering(op, registers[b], registers[cc], &out, &status)) {
                    return fault(status, at);
                }
                registers[a] = lhat_bool(out);
                break;
            }

            case LHAT_BC_JUMP:
                pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                break;

            case LHAT_BC_JUMP_FALSE:
                if (!lhat_is_bool(registers[a])) {
                    return fault(LHAT_RUN_TYPE_ERROR, at);
                }
                if (!lhat_as_bool(registers[a])) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;

            case LHAT_BC_RETURN: {
                LhatRunResult result;
                result.status = LHAT_RUN_OK;
                result.value = registers[a];
                result.at = at;
                return result;
            }

            case LHAT_BC_RETURN_NIL: {
                LhatRunResult result;
                result.status = LHAT_RUN_OK;
                result.value = lhat_nil();
                result.at = at;
                return result;
            }

            case LHAT_BC_COUNT:
                return fault(LHAT_RUN_TYPE_ERROR, at);
        }
    }

    LhatRunResult result;
    result.status = LHAT_RUN_OK;
    result.value = lhat_nil();
    result.at = chunk->count;
    return result;
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_DIVIDE_BY_ZERO:  return "// and % cannot divide by zero";
    }
    return "unknown";
}
