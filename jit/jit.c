// L^ (lhat) -- the copy-and-patch JIT's runtime: choosing a stencil for each
// instruction of a chunk, laying them out, writing their holes, and running
// the result. See jit.h for the shape and stencils.c for the stencils.
//
// A chunk is laid out whole the first time the interpreter turns a loop in
// it (vm.c's VM_LOOP_POLL). Every instruction gets code: what no stencil was
// written for gets the one that answers its pc, so the interpreter runs it.
// Entering is possible at any instruction a jump can land on, which is every
// instruction the interpreter could be standing at after a jump back.

#include "jit.h"

#include <stdlib.h>
#include <string.h>

#include "code.h"
#include "machine.h"

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
// `loops[pc]` says the interpreter may come in at pc: the start of a loop
// whose every instruction has code of its own. Coming in costs the
// trampoline's saves, which a loop paying them on every turn -- leaving
// again at a call a few instructions on -- does not win back.
#define NO_ENTRY UINT32_MAX
typedef struct {
    uint8_t *memory;
    size_t size;
    uint32_t *entries;
    bool *loops;
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
// Choosing.

typedef struct {
    const LhatJitStencil *stencil;
    size_t next;    // the pc CONTINUE goes to
    size_t target;  // the pc TARGET goes to, when there is one
    LhatValue k;    // the constant K and KTAG carry
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

static Choice choose(const LhatChunk *chunk, size_t pc)
{
    LhatInstruction instruction = chunk->code[pc];
    LhatOpcode op = lhat_op(instruction);
    Choice choice = {&lhat_jit_stencil_exit, pc + 1, 0, lhat_nil()};
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
        case LHAT_BC_LT: case LHAT_BC_LE: case LHAT_BC_GT: case LHAT_BC_GE: {
            // 03 の 5.1改5: the JUMP_FALSE reading this answer is taken in,
            // as the interpreter takes it -- forward only, so the jump back
            // a loop turns on keeps its poll.
            bool fused = false;
            if (pc + 1 < chunk->count) {
                LhatInstruction paired = chunk->code[pc + 1];
                fused = lhat_op(paired) == LHAT_BC_JUMP_FALSE &&
                        lhat_a(paired) == lhat_a(instruction) &&
                        lhat_jump_offset(paired) >= 0;
                if (fused) {
                    choice.next = pc + 2;
                    choice.target = jump_target(pc + 1, paired);
                }
            }
            choice.stencil = order_stencil(op, fused);
            break;
        }
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
        default:
            break;
    }
    return choice;
}

// ---------------------------------------------------------------------------
// Laying out.

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
                                   lhat_nil()};
        }
    }
    choices[count] = (Choice){&lhat_jit_stencil_exit, count, 0, lhat_nil()};

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
    bool *loops = landed;  // what landed[] was for is done
    memset(loops, 0, (count + 1) * sizeof *loops);
    for (size_t pc = 0; pc < count; pc++) {
        size_t start = choices[pc].target;
        if (choices[pc].stencil == &lhat_jit_stencil_exit || start > pc ||
            !is_jump(lhat_op(chunk->code[pc]))) {
            continue;
        }
        bool whole = true;
        for (size_t q = start; q <= pc && whole; q++) {
            whole = entries[q] == NO_ENTRY ||
                    choices[q].stencil != &lhat_jit_stencil_exit;
        }
        loops[start] = loops[start] || whole;
    }
    size_t following = count;
    for (size_t pc = count + 1; pc-- > 0;) {
        after[pc] = following;
        if (entries[pc] != NO_ENTRY) {
            following = pc;
        }
    }
    size_t size = lhat_jit_stencil_trampoline.size;
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
    memcpy(memory, lhat_jit_stencil_trampoline.code,
           lhat_jit_stencil_trampoline.size);
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

    DWORD old = 0;
    if (!VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        goto failed;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, size);
    code->memory = memory;
    code->size = size;
    code->entries = entries;
    code->loops = loops;
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
    free(laid->loops);
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
    JitCode *laid = lay_out(chunk);
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

size_t lhat_jit_run(Machine *m, const LhatChunk *chunk, size_t rbase,
                    size_t pc)
{
    if (!jit_enabled() || m->traps) {
        return pc;
    }
    JitCode *code = code_of(chunk);
    if (code == &refused || pc > chunk->count ||
        !code->loops[pc]) {
        return pc;
    }
    LhatJitPoll poll = {&m->steps_left, &m->traps};
    LhatJitTrampoline *enter = (LhatJitTrampoline *)(void *)code->memory;
    LhatJitOp *entry = (LhatJitOp *)(void *)(code->memory + code->entries[pc]);
    return (size_t)enter(m->slots.values + rbase, m->slots.tags + rbase, &poll,
                         entry);
}
