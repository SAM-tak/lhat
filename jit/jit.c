// L^ (lhat) -- the copy-and-patch JIT's runtime: choosing a stencil for each
// instruction of a chunk, laying them out, writing their holes, and running
// the result. See jit.h for the shape and stencils.c for the stencils.
//
// A chunk is laid out whole the first time the interpreter would enter it
// -- at a loop's turn, a body's entry or a call's return (vm.c's
// VM_ENTER_JIT). Every instruction gets code: what no stencil was written
// for gets the one that answers its pc, so the interpreter runs it.

#include "jit.h"

#include <stdlib.h>
#include <string.h>

#include "code.h"
#include "vm_internal.h"

#include "stencils_x86_64-windows.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error "the JIT prototype lays out x86-64 Windows code only (jit.h)"
#endif

// What a chunk's code is. `entries[pc]` is where the code of instruction pc
// begins, or NO_ENTRY where nothing was laid (a JUMP_FALSE a comparison
// took in and no jump lands on); entries[count] is the code that answers
// the end of the chunk.
//
// `enter[pc]` says the interpreter may come in at pc. First, so that the
// interpreter reads it through LhatJitCodeHead (jit.h).
//
// For a call at pc, `resumes[pc]` is the code its return goes on in (NULL
// when there is none to enter), and `callees[pc]` the body this site last
// called the fast way -- one pointer, so machines sharing the chunk on other
// threads overwrite each other's guess but never tear it.
#define NO_ENTRY UINT32_MAX
typedef struct {
    bool *enter;
    uint8_t *memory;
    size_t size;
    uint32_t *entries;
    LhatJitOp **resumes;
    const LhatProto **callees;
} JitCode;

// Process-wide, read once: LHAT_JIT=0 in the environment turns the JIT off
// in a build that has it -- one binary measured both ways.
static bool jit_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("LHAT_JIT");
        enabled = setting == NULL || strcmp(setting, "0") != 0;
    }
    return enabled != 0;
}

// ---------------------------------------------------------------------------
// The steps: one instruction's work done in C, the frame staying where it is.

// The registers of the frame the code is running in.
static LhatValue step_read(const LhatJitContext *context, uintptr_t r)
{
    LhatValue v;
    v.as = context->values[r];
    v.tag = (LhatValueTag)context->tags[r];
    return v;
}

// 03 の 5.1改: a member the site's cache answers. A miss is the
// interpreter's, whose lookup fills the cache for the next time.
static bool jit_get_member(LhatJitContext *context, uintptr_t a, uintptr_t b,
                           uintptr_t c)
{
    Machine *m = context->machine;
    const Frame *frame = &m->frames[m->frame_count - 1];
    LhatMemberCache *cache;
    const LhatValue *hit = vm_cached_member(
        m, &frame->closure->proto->chunk, c, step_read(context, b), &cache);
    if (hit == NULL) {
        return false;
    }
    context->values[a] = hit->as;
    context->tags[a] = (uint8_t)hit->tag;
    return true;
}

// The interpreter's SETINDEX on a table, through the same vm_set_key -- its
// barrier, its growth, its refusal of a key that cannot be one. Refused or
// out of memory, nothing was written, and the interpreter runs the write
// again to say why.
static bool jit_set_index(LhatJitContext *context, uintptr_t a, uintptr_t b,
                          uintptr_t c)
{
    LhatValue owner = step_read(context, a);
    if (lhat_is_hostvalue(owner)) {
        return false;
    }
    LhatTable *table = vm_table_of(owner);
    if (table == NULL || table->sealed) {
        return false;
    }
    bool refused = false;
    return vm_set_key(context->machine, table, step_read(context, b),
                      step_read(context, c), &refused) &&
           !refused;
}

// The frame's slots start this far into the machine's.
static size_t step_base(const LhatJitContext *context)
{
    return (size_t)(context->values - context->machine->slots.values);
}

// 5.4: what still points into these registers takes its value with it.
static bool jit_close(LhatJitContext *context, uintptr_t a, uintptr_t b,
                      uintptr_t c)
{
    (void)b;
    (void)c;
    vm_close_upvalues(context->machine, step_base(context) + a);
    return true;
}

static bool jit_close_one(LhatJitContext *context, uintptr_t a, uintptr_t b,
                          uintptr_t c)
{
    (void)b;
    (void)c;
    vm_close_one_upvalue(context->machine, step_base(context) + a);
    return true;
}

// ---------------------------------------------------------------------------
// Choosing.

typedef struct {
    const LhatJitStencil *stencil;
    size_t next;    // the pc CONTINUE goes to
    size_t target;  // the pc TARGET goes to, when there is one
    LhatValue k;    // the constant K and KTAG carry
    LhatJitStep *step;  // what STEP calls
} Choice;

static bool is_jump(LhatOpcode op)
{
    return op == LHAT_BC_JUMP || op == LHAT_BC_JUMP_FALSE ||
           op == LHAT_BC_FORPREP || op == LHAT_BC_FORPREPD ||
           op == LHAT_BC_FORLOOP || op == LHAT_BC_FORLOOPD;
}

static size_t jump_target(size_t pc, LhatInstruction instruction)
{
    return (size_t)((int64_t)pc + 1 + lhat_jump_offset(instruction));
}

// The stencil the four arithmetic opcodes take with the right operand a
// register (`rr`) or, through the constant's kind, a constant.
static const LhatJitStencil *arith_stencil(LhatOpcode op, LhatValue k)
{
    switch (op) {
        case LHAT_BC_ADD: return &lhat_jit_stencil_add;
        case LHAT_BC_SUB: return &lhat_jit_stencil_sub;
        case LHAT_BC_MUL: return &lhat_jit_stencil_mul;
        case LHAT_BC_DIV: return &lhat_jit_stencil_div;
        case LHAT_BC_ADDK:
            return lhat_is_integer(k) ? &lhat_jit_stencil_addk_int
                                      : &lhat_jit_stencil_addk_real;
        case LHAT_BC_SUBK:
            return lhat_is_integer(k) ? &lhat_jit_stencil_subk_int
                                      : &lhat_jit_stencil_subk_real;
        case LHAT_BC_MULK:
            return lhat_is_integer(k) ? &lhat_jit_stencil_mulk_int
                                      : &lhat_jit_stencil_mulk_real;
        case LHAT_BC_DIVK:
            return lhat_is_real(k) ? &lhat_jit_stencil_divk_real : NULL;
        default: return NULL;
    }
}

static const LhatJitStencil *order_stencil(LhatOpcode op, bool fused)
{
    static const LhatJitStencil *const plain[] = {
        &lhat_jit_stencil_lt,  &lhat_jit_stencil_le,
        &lhat_jit_stencil_gt,  &lhat_jit_stencil_ge,
        &lhat_jit_stencil_ltk, &lhat_jit_stencil_lek,
        &lhat_jit_stencil_gtk, &lhat_jit_stencil_gek,
    };
    static const LhatJitStencil *const taking[] = {
        &lhat_jit_stencil_lt_fused,  &lhat_jit_stencil_le_fused,
        &lhat_jit_stencil_gt_fused,  &lhat_jit_stencil_ge_fused,
        &lhat_jit_stencil_ltk_fused, &lhat_jit_stencil_lek_fused,
        &lhat_jit_stencil_gtk_fused, &lhat_jit_stencil_gek_fused,
    };
    size_t at = op >= LHAT_BC_LTK ? 4 + (size_t)(op - LHAT_BC_LTK)
                                  : (size_t)(op - LHAT_BC_LT);
    return fused ? taking[at] : plain[at];
}

// 03 の 5.1改5: the JUMP_FALSE reading a comparison's answer is taken in,
// as the interpreter takes it -- forward only, so the jump back a loop turns
// on keeps its poll. Says whether it was, having pointed `choice` past it.
static bool take_jump_false(const LhatChunk *chunk, size_t pc,
                            Choice *choice)
{
    if (pc + 1 >= chunk->count) {
        return false;
    }
    LhatInstruction paired = chunk->code[pc + 1];
    if (lhat_op(paired) != LHAT_BC_JUMP_FALSE ||
        lhat_a(paired) != lhat_a(chunk->code[pc]) ||
        lhat_jump_offset(paired) < 0) {
        return false;
    }
    choice->next = pc + 2;
    choice->target = jump_target(pc + 1, paired);
    return true;
}

static Choice choose(const LhatChunk *chunk, size_t pc)
{
    LhatInstruction instruction = chunk->code[pc];
    LhatOpcode op = lhat_op(instruction);
    Choice choice = {&lhat_jit_stencil_exit, pc + 1, 0, lhat_nil(), NULL};
    switch (op) {
        case LHAT_BC_LOADK:
            choice.stencil = &lhat_jit_stencil_loadk;
            choice.k = chunk->constants[lhat_bx(instruction)];
            break;
        case LHAT_BC_LOADNIL:
            choice.stencil = &lhat_jit_stencil_loadk;
            break;
        case LHAT_BC_LOADBOOL:
            choice.stencil = &lhat_jit_stencil_loadk;
            choice.k = lhat_bool(lhat_b(instruction) != 0);
            break;
        case LHAT_BC_MOVE:
            choice.stencil = &lhat_jit_stencil_move;
            break;
        case LHAT_BC_ADD: case LHAT_BC_SUB: case LHAT_BC_MUL:
        case LHAT_BC_DIV:
        case LHAT_BC_ADDK: case LHAT_BC_SUBK: case LHAT_BC_MULK:
        case LHAT_BC_DIVK: {
            bool constant = op >= LHAT_BC_ADDK && op <= LHAT_BC_DIVK;
            if (constant) {
                choice.k = chunk->constants[lhat_c(instruction)];
            }
            const LhatJitStencil *stencil = arith_stencil(op, choice.k);
            if (stencil != NULL) {
                choice.stencil = stencil;
            }
            break;
        }
        case LHAT_BC_LTK: case LHAT_BC_LEK: case LHAT_BC_GTK:
        case LHAT_BC_GEK:
            choice.k = chunk->constants[lhat_c(instruction)];
            if (!lhat_is_integer(choice.k)) {
                break;  // a real constant orders with tolerance
            }
            // fallthrough
        case LHAT_BC_LT: case LHAT_BC_LE: case LHAT_BC_GT: case LHAT_BC_GE:
            choice.stencil = order_stencil(op, take_jump_false(chunk, pc,
                                                               &choice));
            break;
        case LHAT_BC_EQ:
        case LHAT_BC_NE: {
            bool fused = take_jump_false(chunk, pc, &choice);
            choice.stencil =
                op == LHAT_BC_EQ
                    ? (fused ? &lhat_jit_stencil_eq_fused : &lhat_jit_stencil_eq)
                    : (fused ? &lhat_jit_stencil_ne_fused : &lhat_jit_stencil_ne);
            break;
        }
        case LHAT_BC_ENV:
            choice.stencil = &lhat_jit_stencil_env;
            break;
        case LHAT_BC_ISNIL:
            choice.stencil = &lhat_jit_stencil_isnil;
            break;
        case LHAT_BC_NOT:
            choice.stencil = &lhat_jit_stencil_not;
            break;
        case LHAT_BC_CLOSE:
            choice.stencil = &lhat_jit_stencil_step;
            choice.step = &jit_close;
            break;
        case LHAT_BC_CLOSEONE:
            choice.stencil = &lhat_jit_stencil_step;
            choice.step = &jit_close_one;
            break;
        case LHAT_BC_JUMP:
            choice.target = jump_target(pc, instruction);
            choice.stencil = lhat_jump_offset(instruction) < 0
                                 ? &lhat_jit_stencil_jump_back
                                 : &lhat_jit_stencil_jump;
            break;
        case LHAT_BC_JUMP_FALSE:
            choice.target = jump_target(pc, instruction);
            choice.stencil = lhat_jump_offset(instruction) < 0
                                 ? &lhat_jit_stencil_jump_false_back
                                 : &lhat_jit_stencil_jump_false;
            break;
        case LHAT_BC_FORLOOP:
        case LHAT_BC_FORLOOPD:
            choice.target = jump_target(pc, instruction);
            choice.stencil = op == LHAT_BC_FORLOOP ? &lhat_jit_stencil_forloop
                                                   : &lhat_jit_stencil_forloopd;
            break;
        case LHAT_BC_THIS:
            choice.stencil = &lhat_jit_stencil_this;
            break;
        case LHAT_BC_GETUPVAL:
            choice.stencil = &lhat_jit_stencil_getupval;
            break;
        case LHAT_BC_GETINDEX:
            choice.stencil = &lhat_jit_stencil_getindex;
            break;
        case LHAT_BC_GETMEMBER:
        case LHAT_BC_GETMETHOD:
            choice.stencil = &lhat_jit_stencil_step;
            choice.step = &jit_get_member;
            break;
        case LHAT_BC_SETINDEX:
            choice.stencil = &lhat_jit_stencil_step;
            choice.step = &jit_set_index;
            break;
        // 5.3: the plain call and the return of one value; what else either
        // can be -- a method, a tail call, a tuple -- is the interpreter's.
        case LHAT_BC_CALL:
        case LHAT_BC_CALLMETHOD:
            choice.stencil = &lhat_jit_stencil_call;
            break;
        // 03 の 5.1改4: taken with the CALLMETHOD it reads for, the way the
        // interpreter takes the two.
        case LHAT_BC_CALLMEMBER:
            if (pc + 1 < chunk->count &&
                lhat_op(chunk->code[pc + 1]) == LHAT_BC_CALLMETHOD) {
                choice.stencil = &lhat_jit_stencil_call;
                choice.next = pc + 2;
            }
            break;
        case LHAT_BC_RETURN:
            if (lhat_b(instruction) == 0) {
                choice.stencil = &lhat_jit_stencil_return;
            }
            break;
        case LHAT_BC_RETURN_NIL:
            choice.stencil = &lhat_jit_stencil_return_nil;
            break;
        default:
            break;
    }
    return choice;
}

// ---------------------------------------------------------------------------
// Laying out.

static LhatJitHelper jit_call;
static LhatJitHelper jit_return;

static uint64_t hole_value(const Choice *choice, const LhatJitHole *hole,
                           LhatInstruction instruction, size_t pc,
                           const uint8_t *memory, const uint32_t *entries)
{
    switch ((LhatJitHoleKind)hole->kind) {
        case LHAT_JIT_HOLE_A: return lhat_a(instruction);
        case LHAT_JIT_HOLE_B: return lhat_b(instruction);
        case LHAT_JIT_HOLE_C: return lhat_c(instruction);
        case LHAT_JIT_HOLE_K: return (uint64_t)choice->k.as.integer;
        case LHAT_JIT_HOLE_KTAG: return (uint64_t)choice->k.tag;
        case LHAT_JIT_HOLE_PC: return pc;
        case LHAT_JIT_HOLE_CONTINUE:
            return (uint64_t)(uintptr_t)(memory + entries[choice->next]);
        case LHAT_JIT_HOLE_TARGET:
            return (uint64_t)(uintptr_t)(memory + entries[choice->target]);
        case LHAT_JIT_HOLE_CALL: return (uint64_t)(uintptr_t)&jit_call;
        case LHAT_JIT_HOLE_RETURN: return (uint64_t)(uintptr_t)&jit_return;
        case LHAT_JIT_HOLE_STEP: return (uint64_t)(uintptr_t)choice->step;
        case LHAT_JIT_HOLE_COUNT: break;
    }
    return 0;
}

static JitCode *lay_out(const LhatChunk *chunk)
{
    size_t count = chunk->count;
    Choice *choices = (Choice *)calloc(count + 1, sizeof *choices);
    uint32_t *entries = (uint32_t *)malloc((count + 1) * sizeof *entries);
    bool *landed = (bool *)calloc(count + 1, sizeof *landed);
    JitCode *code = (JitCode *)calloc(1, sizeof *code);
    if (choices == NULL || entries == NULL || landed == NULL || code == NULL) {
        goto failed;
    }

    // Where jumps land, so a JUMP_FALSE a comparison took in is laid on its
    // own only when something else can reach it.
    for (size_t pc = 0; pc < count; pc++) {
        LhatInstruction instruction = chunk->code[pc];
        if (is_jump(lhat_op(instruction))) {
            size_t to = jump_target(pc, instruction);
            if (to <= count) {
                landed[to] = true;
            }
        }
    }
    for (size_t pc = 0; pc < count; pc++) {
        choices[pc] = choose(chunk, pc);
        // A jump outside the chunk is not one this compiler writes; leave
        // the instruction to the interpreter rather than follow it.
        if (choices[pc].next > count || choices[pc].target > count) {
            choices[pc] = (Choice){&lhat_jit_stencil_exit, pc + 1, 0,
                                   lhat_nil(), NULL};
        }
    }
    choices[count] = (Choice){&lhat_jit_stencil_exit, count, 0, lhat_nil(),
                              NULL};

    // An instruction a comparison took in is laid on its own only when a
    // jump lands on it. `after[pc]` is the instruction laid next, and a
    // stencil whose next code is that one loses its last jump: it falls in.
    size_t *after = (size_t *)malloc((count + 1) * sizeof *after);
    if (after == NULL) {
        goto failed;
    }
    for (size_t pc = 0; pc <= count; pc++) {
        bool taken_in = pc > 0 && pc < count &&
                        choices[pc - 1].next == pc + 1 && !landed[pc];
        entries[pc] = taken_in ? NO_ENTRY : 0;
    }
    // Where the interpreter may come in: anywhere there is code of an
    // instruction's own, not the code that only sends it back.
    bool *enter = landed;  // what landed[] was for is done
    for (size_t pc = 0; pc <= count; pc++) {
        enter[pc] = pc < count && entries[pc] != NO_ENTRY &&
                    choices[pc].stencil != &lhat_jit_stencil_exit;
    }
    size_t following = count;
    for (size_t pc = count + 1; pc-- > 0;) {
        after[pc] = following;
        if (entries[pc] != NO_ENTRY) {
            following = pc;
        }
    }
    size_t size = 0;
    for (size_t pc = 0; pc <= count; pc++) {
        if (entries[pc] == NO_ENTRY) {
            continue;
        }
        const LhatJitStencil *stencil = choices[pc].stencil;
        entries[pc] = (uint32_t)size;
        size += pc < count && choices[pc].next == after[pc] ? stencil->tail
                                                            : stencil->size;
    }

    uint8_t *memory = (uint8_t *)VirtualAlloc(NULL, size,
                                              MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
    if (memory == NULL) {
        free(after);
        goto failed;
    }
    for (size_t pc = 0; pc <= count; pc++) {
        if (entries[pc] == NO_ENTRY) {
            continue;
        }
        const Choice *choice = &choices[pc];
        const LhatJitStencil *stencil = choice->stencil;
        size_t length = pc < count && choice->next == after[pc]
                            ? stencil->tail
                            : stencil->size;
        uint8_t *at = memory + entries[pc];
        memcpy(at, stencil->code, length);
        LhatInstruction instruction = pc < count ? chunk->code[pc] : 0;
        for (size_t h = 0; h < stencil->hole_count; h++) {
            const LhatJitHole *hole = &stencil->holes[h];
            if (hole->offset >= length) {
                continue;  // the jump that was cut
            }
            uint64_t value = hole_value(choice, hole, instruction, pc, memory,
                                        entries) +
                             (uint64_t)(int64_t)hole->addend;
            memcpy(at + hole->offset, &value, sizeof value);
        }
    }
    free(after);

    // Where each call's return goes on, settled once here rather than looked
    // up on every return.
    LhatJitOp **resumes = (LhatJitOp **)calloc(count + 1, sizeof *resumes);
    const LhatProto **callees =
        (const LhatProto **)calloc(count + 1, sizeof *callees);
    DWORD old = 0;
    if (resumes == NULL || callees == NULL ||
        !VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old)) {
        free(resumes);
        free(callees);
        VirtualFree(memory, 0, MEM_RELEASE);
        goto failed;
    }
    for (size_t pc = 0; pc < count; pc++) {
        size_t next = choices[pc].next;
        if (choices[pc].stencil == &lhat_jit_stencil_call && enter[next]) {
            resumes[pc] = (LhatJitOp *)(void *)(memory + entries[next]);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), memory, size);
    code->memory = memory;
    code->size = size;
    code->entries = entries;
    code->enter = enter;
    code->resumes = resumes;
    code->callees = callees;
    free(choices);
    return code;

failed:
    free(choices);
    free(entries);
    free(landed);
    free(code);
    return NULL;
}

// What a chunk that could not be laid out carries, so it is not tried again.
static JitCode refused;

void lhat_jit_free(void *code)
{
    JitCode *laid = (JitCode *)code;
    if (laid == NULL || laid == &refused) {
        return;
    }
    VirtualFree(laid->memory, 0, MEM_RELEASE);
    free(laid->entries);
    free(laid->enter);
    free(laid->resumes);
    free((void *)laid->callees);
    free(laid);
}

// The chunk's code, laid out by whichever machine asks first. Two that ask
// at once both lay it out and one keeps its own; the chunk's pointer is the
// only thing written, once.
static JitCode *code_of(const LhatChunk *chunk)
{
    LhatChunk *mutable_chunk = (LhatChunk *)chunk;
    JitCode *code = __atomic_load_n((JitCode **)&mutable_chunk->jit,
                                    __ATOMIC_ACQUIRE);
    if (code != NULL) {
        return code;
    }
    JitCode *laid = jit_enabled() ? lay_out(chunk) : NULL;
    JitCode *want = laid != NULL ? laid : &refused;
    JitCode *expected = NULL;
    if (!__atomic_compare_exchange_n((JitCode **)&mutable_chunk->jit,
                                     &expected, want, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        lhat_jit_free(laid);
        return expected;
    }
    return want;
}

// The code of the chunk at pc, when there is code of its own to enter there
// and nothing the traps word stands for is waiting on the interpreter.
static LhatJitOp *enter_at(const Machine *m, const LhatChunk *chunk,
                           size_t pc)
{
    if (m->traps) {
        return NULL;
    }
    JitCode *code = code_of(chunk);
    if (code == &refused || pc > chunk->count || !code->enter[pc]) {
        return NULL;
    }
    return (LhatJitOp *)(void *)(code->memory + code->entries[pc]);
}

// The frame on top is the one the code goes on in.
static void moved_to(LhatJitContext *context, const Frame *frame)
{
    Machine *m = context->machine;
    context->values = m->slots.values + frame->base;
    context->tags = m->slots.tags + frame->base;
    context->closure = frame->closure;
    context->moved = true;
}

// 5.3, what the interpreter's CALL and CALLMETHOD do for the case they are
// most often asked: an L^ body taking exactly the arguments laid out for
// it, no spread, no collected tail, not a coroutine to make -- and, for a
// CALLMEMBER, a member its site remembers (03 の 5.1改4), read on the way
// into the CALLMETHOD it is paired with. Nothing is allocated, which is why
// the collector's poll is not asked here. The body is entered the way the
// interpreter enters one, slice and all. The instruction is read here
// rather than handed in: what the call needs of it depends on which it is.
static LhatJitOp *jit_call(LhatJitContext *context, uintptr_t unused_a,
                           uintptr_t unused_b, uintptr_t unused_c,
                           uintptr_t pc)
{
    (void)unused_a;
    (void)unused_b;
    (void)unused_c;
    Machine *m = context->machine;
    Frame *frame = &m->frames[m->frame_count - 1];
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t rbase = frame->base;
    LhatInstruction instruction = chunk->code[pc];
    size_t resume = pc + 1;
    context->leave_pc = pc;
    if (lhat_op(instruction) == LHAT_BC_CALLMEMBER) {
        LhatInstruction paired = chunk->code[pc + 1];
        LhatMemberCache *cache;
        const LhatValue *hit = vm_cached_member(
            m, chunk, lhat_c(instruction),
            lhat_slots_get(m->slots, rbase + lhat_b(instruction)), &cache);
        if (hit == NULL || lhat_op(paired) != LHAT_BC_CALLMETHOD) {
            return NULL;
        }
        lhat_slots_set(m->slots, rbase + lhat_a(instruction), *hit);
        instruction = paired;
        resume = pc + 2;
    }
    size_t a = lhat_a(instruction);
    size_t given = lhat_b(instruction);
    uint8_t c = lhat_c(instruction);
    LhatValue called = lhat_slots_get(m->slots, rbase + a);
    if (!lhat_is_object_kind(called, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    const LhatClosure *callee = (const LhatClosure *)lhat_as_object(called);
    const LhatProto *proto = callee->proto;
    if (proto == NULL) {
        return NULL;
    }
    // 14.4: the receiver sits between the callee and the arguments, and
    // is the first of them only for a callee that takes it.
    size_t skip = 1;
    if (lhat_op(instruction) == LHAT_BC_CALLMETHOD) {
        if (proto->takes_self) {
            given++;
        } else {
            skip = 2;
        }
    }
    // What a body is asked here does not change between two calls of it
    // from this site, so a body this site called the fast way before is
    // not asked again -- only whether the machine has room.
    JitCode *code = (JitCode *)chunk->jit;
    bool known = __atomic_load_n(&code->callees[pc], __ATOMIC_RELAXED) == proto;
    size_t next_base = rbase + a + skip;
    if (!known && ((c & LHAT_CALL_SPREAD) != 0 || proto->has_variadic ||
                   proto->yields || given != proto->parameters)) {
        return NULL;
    }
    if (m->frame_count >= m->frame_capacity ||
        next_base + proto->chunk.registers >= m->slot_capacity) {
        return NULL;
    }
    vm_clear_scratch(m, next_base, proto);
    frame->pc = resume;
    Frame *entered = vm_push_frame(m, callee, next_base, (uint8_t)a,
                                   (uint8_t)lhat_call_prepared(c));
    entered->jit_return = code->resumes[pc];
    moved_to(context, entered);
    // 02 § 15.15 at a body's entry, as the interpreter polls it. A slice
    // about to run out is left for the interpreter to end, at the body's
    // first instruction.
    context->leave_pc = 0;
    if (m->steps_left == 1 || m->traps) {
        return NULL;
    }
    if (m->steps_left != 0) {
        m->steps_left--;
    }
    if (known) {
        const JitCode *body = (const JitCode *)proto->chunk.jit;
        return (LhatJitOp *)(void *)(body->memory + body->entries[0]);
    }
    LhatJitOp *entry = enter_at(m, &proto->chunk, 0);
    if (entry != NULL) {
        __atomic_store_n(&code->callees[pc], proto, __ATOMIC_RELAXED);
    }
    return entry;
}

// The interpreter's RETURN and the drain after it, for a frame with nothing
// to drain: no cleanup waiting, no coroutine whose body it is, no operator
// whose answer is read again (11.9), one plain value. Not the run's own
// frame either -- that one ends the run, which is vm_finish's.
static LhatJitOp *jit_return(LhatJitContext *context, uintptr_t a,
                             uintptr_t nil, uintptr_t c, uintptr_t pc)
{
    (void)c;
    Machine *m = context->machine;
    Frame *frame = &m->frames[m->frame_count - 1];
    context->leave_pc = pc;
    if (m->frame_count <= m->run_base + 1 || frame->cleanup_count != 0 ||
        frame->coroutine != NULL || frame->derive != LHAT_FRAME_NO_DERIVE ||
        m->cleanup_carriers != 0) {
        return NULL;
    }
    LhatValue value =
        nil ? lhat_nil() : lhat_slots_get(m->slots, frame->base + a);
    if (lhat_is_hostvalue(value)) {
        return NULL;
    }
    if (frame->drop_answer) {
        value = lhat_nil();
    }
    vm_close_upvalues(m, frame->base + frame->closure->proto->kept);
    uint8_t into = frame->result;
    LhatJitOp *resume = (LhatJitOp *)frame->jit_return;
    m->frame_count--;
    Frame *caller = &m->frames[m->frame_count - 1];
    lhat_slots_set(m->slots, caller->base + into, value);
    moved_to(context, caller);
    context->leave_pc = caller->pc;
    // A frame the code pushed knows where its caller's code goes on; one the
    // interpreter pushed asks.
    if (resume != NULL) {
        return m->traps ? NULL : resume;
    }
    return enter_at(m, &caller->closure->proto->chunk, caller->pc);
}

size_t lhat_jit_run(Machine *m, const LhatChunk *chunk, size_t rbase,
                    size_t pc)
{
    LhatJitOp *entry = enter_at(m, chunk, pc);
    if (entry == NULL) {
        return pc;
    }
    LhatJitContext context = {
        &m->steps_left, &m->traps, m,
        m->slots.values + rbase, m->slots.tags + rbase,
        m->frames[m->frame_count - 1].closure, pc, false, m->environment,
    };
    size_t left = (size_t)entry(context.values, context.tags, &context);
    return context.moved ? left | LHAT_JIT_MOVED : left;
}
