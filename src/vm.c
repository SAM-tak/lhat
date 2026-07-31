// L^ (lhat) -- compiling a tree to bytecode, and running it.

#include "vm.h"

#include <stdlib.h>
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

// One subroutine being compiled. The chain of parents is what a name search
// walks when it is not found here, which is how 5.4 decides what to capture.
typedef struct Compiler {
    struct Compiler *parent;
    const LhatLexer *lexer;
    LhatProto *proto;
    LhatCompileStatus *status;  // shared, so the first failure sticks

    Local locals[LHAT_MAX_LOCALS];
    size_t local_count;

    struct {
        const char *name;
        size_t length;
    } upvalue_names[256];

    // Slots below this hold live names; everything above is scratch for the
    // expression being compiled, released as soon as it is consumed.
    uint8_t next_register;
} Compiler;

static void fail(Compiler *c, LhatCompileStatus status)
{
    if (*c->status == LHAT_COMPILE_OK) {
        *c->status = status;
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
    if (c->next_register > c->proto->chunk.registers) {
        c->proto->chunk.registers = c->next_register;
    }
    return r;
}

static void emit(Compiler *c, LhatInstruction instruction)
{
    if (lhat_chunk_emit(&c->proto->chunk, instruction) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
}

static size_t emit_jump(Compiler *c, LhatOpcode op, uint8_t a)
{
    size_t at = lhat_chunk_emit(&c->proto->chunk, lhat_encode_jump(op, a, 0));
    if (at == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
    return at;
}

static void load_constant(Compiler *c, uint8_t into, LhatValue value)
{
    size_t k = lhat_chunk_constant(&c->proto->chunk, value);
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

// 5.4: a name not held here is looked for in the enclosing subroutines, and
// found means a place to share rather than a value to copy. Each level on the
// way down records how it reaches the level above -- a register when the
// parent holds it, or one of the parent's own upvalues when it does not.
// Returns SIZE_MAX when there is no such name anywhere.
static size_t find_upvalue(Compiler *c, const char *name, size_t length)
{
    if (c->parent == NULL) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_names[i].length == length &&
            memcmp(c->upvalue_names[i].name, name, length) == 0) {
            return i;
        }
    }

    bool from_register = false;
    uint8_t index = 0;

    const Local *local = find_local(c->parent, name, length);
    if (local != NULL) {
        from_register = true;
        index = local->reg;
    } else {
        size_t outer = find_upvalue(c->parent, name, length);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        index = (uint8_t)outer;
    }

    size_t added = lhat_proto_add_upvalue(c->proto, from_register, index);
    if (added == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return SIZE_MAX;
    }
    c->upvalue_names[added].name = name;
    c->upvalue_names[added].length = length;
    return added;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into);
static void compile_statement(Compiler *c, const LhatNode *node);
static void compile_statements(Compiler *c, const LhatNode *statements);

// 5.3: the callee sits in a register and its arguments follow it, so the
// machine can hand the callee's frame a contiguous run.
static void compile_call(Compiler *c, const LhatNode *node, uint8_t into)
{
    uint8_t mark = c->next_register;
    uint8_t callee = reserve(c);
    compile_expression(c, node->v.access.target, callee);

    size_t count = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        uint8_t slot = reserve(c);
        compile_expression(c, arg, slot);
        count++;
    }
    if (count > 0xFF) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    emit(c, lhat_encode_abc(LHAT_BC_CALL, callee, (uint8_t)count, 0));
    if (into != callee) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, callee, 0));
    }
    c->next_register = mark;
}

// 02 の 15 章: f^ and p^ are both compiled the same way here; the difference
// they carry is for the checker, and 5.1 keeps the machine out of it.
static void compile_subroutine(Compiler *c, const LhatNode *node, uint8_t into)
{
    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    proto->is_function = node->v.func.is_function;

    size_t index = lhat_proto_add(c->proto, proto);
    if (index == SIZE_MAX) {
        lhat_proto_free(proto);
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    Compiler inner;
    memset(&inner, 0, sizeof inner);
    inner.parent = c;
    inner.lexer = c->lexer;
    inner.proto = proto;
    inner.status = c->status;

    // 5.3: the parameters are the frame's first registers, in order.
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        const char *name = NULL;
        size_t length = 0;
        if (param->v.param.variadic ||
            !node_name(&inner, param->v.param.name, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        if (inner.local_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        uint8_t slot = reserve(&inner);
        Local *local = &inner.locals[inner.local_count++];
        local->name = name;
        local->length = length;
        local->reg = slot;
        proto->parameters++;
    }

    const LhatNode *body = node->v.func.body;
    compile_statements(&inner, body != NULL ? body->v.list.items : NULL);
    if (lhat_chunk_emit(&proto->chunk,
                        lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0)) ==
        SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }

    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, into, (uint16_t)index));
}

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
            lhat_chunk_patch_here(&c->proto->chunk, to_right);
            compile_expression(c, node->v.binary.right, into);
            lhat_chunk_patch_here(&c->proto->chunk, done);
            return;
        }
        size_t done = emit_jump(c, LHAT_BC_JUMP_FALSE, into);
        compile_expression(c, node->v.binary.right, into);
        lhat_chunk_patch_here(&c->proto->chunk, done);
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
    if (node == NULL || *c->status != LHAT_COMPILE_OK) {
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
            if (local != NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, local->reg, 0));
                return;
            }
            size_t upvalue = find_upvalue(c, name, length);
            if (upvalue == SIZE_MAX) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
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

        case LHAT_NODE_CALL:
            compile_call(c, node, into);
            return;

        case LHAT_NODE_FUNC:
            compile_subroutine(c, node, into);
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
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            return;
        }

        default:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
    }
}

// The name a let^ target carries, whether or not it was annotated.
static const LhatNode *define_target_name(const LhatNode *target)
{
    return target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
}

// 8.7: a let^ name is visible across the whole scope, written before its own
// definition or after it. So every slot in a block is made before anything is
// compiled into it -- which is what lets a body call itself, and two bodies
// call each other, with nothing declared ahead of them.
static void declare_names(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        for (const LhatNode *target = s->v.binding.targets; target != NULL;
             target = target->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, define_target_name(target), &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            if (c->local_count >= LHAT_MAX_LOCALS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }

            uint8_t slot = reserve(c);
            // The slot may still hold what an earlier block left in it, and
            // 8.7 lets the name be read from a body before its let^ has run.
            // Emptying it makes that nil^ rather than rubbish.
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));

            Local *local = &c->locals[c->local_count++];
            local->name = name;
            local->length = length;
            local->reg = slot;
        }
    }
}

// 02 の 8.6: let^ makes a name, and the name is a slot that stays. The slot
// itself was made by declare_names; this fills it.
static void compile_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, define_target_name(target), &name, &length)) {
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
        if (local != NULL) {
            if (value != NULL) {
                compile_expression(c, value, local->reg);
                value = value->next;
            }
            continue;
        }

        // 8.6 is the reason 5.4 shares a place rather than a value: a ':='
        // written inside a nested subroutine has to reach the outer binding.
        size_t upvalue = find_upvalue(c, name, length);
        if (upvalue == SIZE_MAX) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        if (value != NULL) {
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, slot, (uint8_t)upvalue, 0));
            c->next_register = mark;
            value = value->next;
        }
    }
}

static void compile_statements(Compiler *c, const LhatNode *statements)
{
    // A block's names end with it, and so do their slots.
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    declare_names(c, statements);
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        compile_statement(c, s);
    }

    // The slots go back to the pool, so anything sharing one has to stop
    // sharing it first -- otherwise a closure made inside the block would read
    // whatever the next block puts there.
    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }

    c->local_count = local_mark;
    c->next_register = register_mark;
}

static void compile_statement(Compiler *c, const LhatNode *node)
{
    if (node == NULL || *c->status != LHAT_COMPILE_OK) {
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
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            return;
        }

        case LHAT_NODE_CALL_STMT: {
            // 02 の 8.2: a call may stand alone, and its value is discarded.
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->v.jump.value, slot);
            c->next_register = mark;
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
                               LhatProto **out)
{
    *out = NULL;
    if (unit == NULL || lexer == NULL) {
        return LHAT_COMPILE_UNSUPPORTED;
    }

    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        return LHAT_COMPILE_TOO_COMPLEX;
    }

    LhatCompileStatus status = LHAT_COMPILE_OK;

    Compiler c;
    memset(&c, 0, sizeof c);
    c.lexer = lexer;
    c.proto = proto;
    c.status = &status;

    // The unit is a scope like any other, so 8.7 applies to it too.
    compile_statements(&c, unit->v.list.items);
    emit(&c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));

    if (status != LHAT_COMPILE_OK) {
        lhat_proto_free(proto);
        return status;
    }
    *out = proto;
    return status;
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

#define LHAT_STACK_SLOTS 8192
#define LHAT_MAX_FRAMES 200

typedef struct {
    const LhatClosure *closure;
    size_t pc;
    LhatValue *base;   // 5.2: the frame's registers start here
    uint8_t result;    // where in the caller's frame the answer goes
} Frame;

typedef struct {
    LhatValue stack[LHAT_STACK_SLOTS];
    Frame frames[LHAT_MAX_FRAMES];
    size_t frame_count;

    // Everything allocated while running, so it can all be released at the
    // end. A collector replaces this; until then a program frees on exit.
    LhatObject *objects;
    LhatUpvalue *open;  // 5.4, innermost first
} Machine;

static void *allocate(Machine *m, size_t size, LhatObjectKind kind)
{
    LhatObject *object = (LhatObject *)calloc(1, size);
    if (object == NULL) {
        return NULL;
    }
    object->kind = kind;
    object->next = m->objects;
    m->objects = object;
    return object;
}

static void release(Machine *m)
{
    LhatObject *object = m->objects;
    while (object != NULL) {
        LhatObject *next = object->next;
        if (object->kind == LHAT_OBJECT_SUBROUTINE) {
            free(((LhatClosure *)object)->upvalues);
        }
        free(object);
        object = next;
    }
    m->objects = NULL;
    m->open = NULL;
}

// 5.4: one place per slot, so two closures capturing the same name share it.
static LhatUpvalue *capture(Machine *m, LhatValue *slot)
{
    LhatUpvalue **link = &m->open;
    while (*link != NULL && (*link)->location > slot) {
        link = &(*link)->next_open;
    }
    if (*link != NULL && (*link)->location == slot) {
        return *link;
    }

    LhatUpvalue *upvalue =
        (LhatUpvalue *)allocate(m, sizeof *upvalue, LHAT_OBJECT_UPVALUE);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = slot;
    upvalue->next_open = *link;
    *link = upvalue;
    return upvalue;
}

// The frame is going, so anything still pointing into it carries its value
// away. Without this a closure returned from a subroutine would read a slot
// that has been reused.
static void close_upvalues(Machine *m, const LhatValue *above)
{
    while (m->open != NULL && m->open->location >= above) {
        LhatUpvalue *upvalue = m->open;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        m->open = upvalue->next_open;
        upvalue->next_open = NULL;
    }
}

static LhatRunResult finish(Machine *m, LhatRunStatus status, LhatValue value,
                            size_t at)
{
    release(m);
    LhatRunResult result;
    result.status = status;
    result.value = value;
    result.at = at;
    return result;
}

LhatRunResult lhat_run(const LhatProto *proto)
{
    static Machine machine;  // large enough that the stack is the wrong place
    memset(&machine, 0, sizeof machine);
    Machine *m = &machine;

    for (size_t i = 0; i < LHAT_STACK_SLOTS; i++) {
        m->stack[i] = lhat_nil();
    }

    LhatClosure *entry =
        (LhatClosure *)allocate(m, sizeof *entry, LHAT_OBJECT_SUBROUTINE);
    if (entry == NULL) {
        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
    }
    entry->proto = proto;

    Frame *frame = &m->frames[m->frame_count++];
    frame->closure = entry;
    frame->pc = 0;
    frame->base = m->stack;
    frame->result = 0;

    LhatValue *registers = frame->base;
    const LhatChunk *chunk = &proto->chunk;
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
                    return finish(m, status, lhat_nil(), at);
                }
                registers[a] = out;
                break;
            }

            case LHAT_BC_NEG: {
                if (!lhat_is_number(registers[b])) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
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
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
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
                    return finish(m, status, lhat_nil(), at);
                }
                registers[a] = lhat_bool(out);
                break;
            }

            case LHAT_BC_JUMP:
                pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                break;

            case LHAT_BC_JUMP_FALSE:
                if (!lhat_is_bool(registers[a])) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (!lhat_as_bool(registers[a])) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;

            case LHAT_BC_CLOSURE: {
                const LhatProto *nested =
                    frame->closure->proto->protos[lhat_bx(instruction)];
                LhatClosure *closure = (LhatClosure *)allocate(
                    m, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = nested;
                closure->upvalue_count = nested->upvalue_count;
                if (nested->upvalue_count > 0) {
                    closure->upvalues = (LhatUpvalue **)calloc(
                        nested->upvalue_count, sizeof *closure->upvalues);
                    if (closure->upvalues == NULL) {
                        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                // 5.4: a register of this frame, or one of its own upvalues
                // when the name came from further out.
                for (size_t i = 0; i < nested->upvalue_count; i++) {
                    const LhatUpvalueDesc *desc = &nested->upvalues[i];
                    closure->upvalues[i] =
                        desc->from_parent_register
                            ? capture(m, &registers[desc->index])
                            : frame->closure->upvalues[desc->index];
                    if (closure->upvalues[i] == NULL) {
                        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                registers[a] = lhat_object((LhatObject *)closure);
                break;
            }

            case LHAT_BC_GETUPVAL:
                registers[a] = *frame->closure->upvalues[b]->location;
                break;

            case LHAT_BC_SETUPVAL:
                *frame->closure->upvalues[b]->location = registers[a];
                break;

            case LHAT_BC_CLOSE:
                close_upvalues(m, &registers[a]);
                break;

            case LHAT_BC_CALL: {
                if (!lhat_is_object_kind(registers[a], LHAT_OBJECT_SUBROUTINE)) {
                    return finish(m, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                const LhatClosure *callee =
                    (const LhatClosure *)lhat_as_object(registers[a]);
                if (callee->proto == NULL) {
                    return finish(m, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                if (b != callee->proto->parameters) {
                    return finish(m, LHAT_RUN_ARITY, lhat_nil(), at);
                }
                if (m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 5.3: the arguments already sit just above the callee, so
                // the new frame starts there and needs no shuffling.
                LhatValue *next_base = &registers[a] + 1;
                if (next_base + LHAT_MAX_REGISTERS >=
                    m->stack + LHAT_STACK_SLOTS) {
                    return finish(m, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                frame->pc = pc;
                Frame *called = &m->frames[m->frame_count++];
                called->closure = callee;
                called->pc = 0;
                called->base = next_base;
                called->result = a;

                frame = called;
                registers = frame->base;
                chunk = &callee->proto->chunk;
                pc = 0;
                break;
            }

            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL: {
                LhatValue value = op == LHAT_BC_RETURN ? registers[a]
                                                       : lhat_nil();
                // 5.4: whatever still points into this frame takes its value
                // with it, since the slots are about to be reused.
                close_upvalues(m, frame->base);
                m->frame_count--;

                if (m->frame_count == 0) {
                    return finish(m, LHAT_RUN_OK, value, at);
                }

                uint8_t into = frame->result;
                frame = &m->frames[m->frame_count - 1];
                registers = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                registers[into] = value;
                break;
            }

            case LHAT_BC_COUNT:
                return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
    }

    return finish(m, LHAT_RUN_OK, lhat_nil(), chunk->count);
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_DIVIDE_BY_ZERO:  return "// and % cannot divide by zero";
        case LHAT_RUN_NOT_CALLABLE:    return "this is not a subroutine";
        case LHAT_RUN_ARITY:           return "the wrong number of arguments";
        case LHAT_RUN_STACK_OVERFLOW:  return "the calls went too deep";
        case LHAT_RUN_OUT_OF_MEMORY:   return "out of memory";
    }
    return "unknown";
}
