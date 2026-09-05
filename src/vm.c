// L^ (lhat) -- bytecode dispatch and instruction control flow.

#include "vm_internal.h"
#include <math.h>
#include <string.h>
#include "lhat/config.h"
#include "lhat/port.h"
#include "registry.h"
#include "type.h"

// 02 の 14.8改: the three that can leave the integers. Answers false when the
// result does not fit, and then the caller redoes it in reals.
//
// Computed through the unsigned type: signed overflow is undefined in C11, so
// the wrapped value a check would have to look at cannot be obtained by
// letting it wrap. Unsigned wraps by definition, and converting back is
// implementation-defined rather than undefined -- every target this runs on
// is two's complement, which C23 now requires outright.
static bool add_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x + (uint64_t)y;
    int64_t sum = (int64_t)wrapped;
    // Overflow is exactly the case where both operands share a sign and the
    // result does not.
    if (((x ^ sum) & (y ^ sum)) < 0) {
        return false;
    }
    *out = sum;
    return true;
}

static bool subtract_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x - (uint64_t)y;
    int64_t difference = (int64_t)wrapped;
    if (((x ^ y) & (x ^ difference)) < 0) {
        return false;
    }
    *out = difference;
    return true;
}

static bool multiply_exact(int64_t x, int64_t y, int64_t *out)
{
    if (x == 0 || y == 0) {
        *out = 0;
        return true;
    }
    uint64_t wrapped = (uint64_t)x * (uint64_t)y;
    int64_t product = (int64_t)wrapped;
    // Dividing back is the check that needs no wider type. The extra guard is
    // INT64_MIN / -1, which is the one division that overflows.
    if (x == -1 && y == INT64_MIN) {
        return false;
    }
    if (product / x != y) {
        return false;
    }
    *out = product;
    return true;
}

// 5.1: the generic form checks. 02 の 14.8 makes number^ one type with two
// representations, so an operation stays in integers when both sides are and
// widens when one of them is real -- or when the integers no longer hold the
// answer (14.8改).
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
        case LHAT_BC_ADD: {
            int64_t whole = 0;
            *out = exact && add_exact(lhat_as_integer(left),
                                      lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a + b);
            return true;
        }
        case LHAT_BC_SUB: {
            int64_t whole = 0;
            *out = exact && subtract_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a - b);
            return true;
        }
        case LHAT_BC_MUL: {
            int64_t whole = 0;
            *out = exact && multiply_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a * b);
            return true;
        }
        case LHAT_BC_DIV:
            // 04 の 11.2: real division, so a zero divisor gives inf rather
            // than failing. That is what keeps ordinary arithmetic out of the
            // unions.
            *out = lhat_real(a / b);
            return true;
        case LHAT_BC_IDIV:
        case LHAT_BC_MOD: {
            // 04 の 11.2: a zero divisor does not fail -- like an
            // overflow (14.8改), it widens to real arithmetic instead,
            // which already answers inf/nan for this the same way '/'
            // does, so ordinary arithmetic stays out of a union either way.
            if (exact && b != 0) {
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
            // floor() rather than a hand-rolled truncate-and-adjust: the
            // quotient can be inf or nan here (b == 0), and casting either
            // to int64_t is undefined.
            double floored = floor(a / b);
            *out = op == LHAT_BC_IDIV ? lhat_real(floored)
                                      : lhat_real(a - floored * b);
            return true;
        }
        case LHAT_BC_POW:
            if (lhat_is_integer(right)) {
                // small interger exponent specified hand loop
                int64_t times = lhat_as_integer(right);
                if (times == 0) {
                    *out = lhat_integer(1);
                    return true;
                }
                // small positive interger exponent specified hand loop
                if (lhat_is_integer(left) && times > 0 && times <= 100) {
                    int64_t r = 1;
                    int64_t base = lhat_as_integer(left);
                    if(base == 0) {
                        *out = lhat_integer(0);
                        return true;
                    }
                    for (int64_t i = 0; i < times; i++) {
                        r *= base;
                    }
                    *out = lhat_integer(r);
                    return true;
                }
                // generic small interger exponent hand loop
                if(fabs(a) > 0.00000001 && times <= 100 && times >= -100) {
                    double r = 1.0;
                    double base = a;
                    // Kept out of libm so the core has no maths dependency yet; only
                    // whole exponents are wanted before the standard library lands.
                    bool invert = times < 0;
                    if (invert)
                    {
                        times = -times;
                    }
                    for (int64_t i = 0; i < times; i++)
                    {
                        r *= base;
                    }
                    *out = lhat_real(invert ? 1.0 / r : r);
                    return true;
                }
            }
            // A negative base under a fractional exponent is NaN -- not a
            // fault, the same line 04 の 11.2 draws for a zero divisor.
            *out = lhat_real(pow(a, b));
            return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

// 02 の 11.9: which of the two comes first, for the types that order
// their own. Negative, zero or positive -- the shape every '<=>' answers in,
// and what the four orderings read against zero. False when neither number^
// nor string^ is what is being asked about, which sends the question to the
// member 11.8 names.
bool vm_three_way(LhatValue left, LhatValue right, int *out)
{
    if (lhat_is_number(left) && lhat_is_number(right)) {
        // 14.8: one type over integers and reals, and the same question '='
        // asks -- so two numbers within the error a real carries order as
        // one. Reading '=' off '<=>' the way 11.9 does then answers what '='
        // answers, rather than 'equal' and 'less' both holding.
        if (lhat_value_close(left, right, LHAT_NUMBER_TOLERANCE)) {
            *out = 0;
            return true;
        }
        *out = lhat_number_as_real(left) < lhat_number_as_real(right) ? -1 : 1;
        return true;
    }
    if (lhat_is_object_kind(left, LHAT_OBJECT_STRING) &&
        lhat_is_object_kind(right, LHAT_OBJECT_STRING)) {
        // The bytes, in order. 01 の 1 章 fixes the encoding at UTF-8, which
        // orders code points the same way their bytes order.
        const LhatString *a = (const LhatString *)lhat_as_object(left);
        const LhatString *b = (const LhatString *)lhat_as_object(right);
        size_t shorter = a->length < b->length ? a->length : b->length;
        int by_bytes = shorter > 0 ? memcmp(a->text, b->text, shorter) : 0;
        if (by_bytes != 0) {
            *out = by_bytes < 0 ? -1 : 1;
        } else if (a->length == b->length) {
            *out = 0;
        } else {
            *out = a->length < b->length ? -1 : 1;
        }
        return true;
    }
    return false;
}

// 11.9: every ordering is read off a three-way answer, whether the
// answer came from a type that orders its own or from a written op^<=>. False
// when neither side is one of those, which is what sends the question on.
static bool ordering(LhatOpcode op, LhatValue left, LhatValue right,
                     bool *out, LhatRunStatus *status)
{
    int outcome = 0;
    if (!vm_three_way(left, right, &outcome)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }
    switch (op) {
        case LHAT_BC_LT: *out = outcome <  0; return true;
        case LHAT_BC_LE: *out = outcome <= 0; return true;
        case LHAT_BC_GT: *out = outcome >  0; return true;
        case LHAT_BC_GE: *out = outcome >= 0; return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}
// 13.7: the i-th effective argument of a call, whether it sits in an
// ordinary register or comes from what a spread ('expr...') unpacked -- the
// two are not contiguous in memory, so nothing beyond this reads registers
// directly once a spread is in play.
//
// 13.8改: the spread is either a table's positions or a tuple's. A tuple's
// are contiguous with the registers -- they are the run's own slots, one
// past the head -- so `run_at` is where that head sits and the positions
// follow it. Only one of the two is ever set.
static LhatValue call_arg(LhatSlots regs, size_t rbase, uint8_t a, size_t skip,
                          const LhatTable *spread, size_t run_at,
                          size_t before_spread, size_t i)
{
    if (i >= before_spread) {
        if (spread != NULL) {
            return lhat_slots_get(spread->array, i - before_spread);
        }
        if (run_at != SIZE_MAX) {
            return lhat_slots_get(regs, run_at + 1 + (i - before_spread));
        }
    }
    return lhat_slots_get(regs, rbase + a + skip + i);
}
// 03 の 5.1改5: what a cached place has to still hold for a hit to be this
// member.
//
// `member_caches` is the one thing on a chunk that is written while the
// program runs, and a chunk is shared by every machine of a program that
// runs the same body (05 の 8.8改: a carried closure keeps its proto, and a
// pool hands one job to N workers). Two of them filling one site write the
// place in three fields with nothing between them, so a third may read a
// mix of the two.
//
// Reading the key back is what makes a mix harmless. An index past the end
// is a miss; an index in range whose key is this site's key IS this
// member, whichever fill it came from -- so the answer is either right or
// a miss, never another member and never a wild read. The cost is one
// comparison of a short name, against the hash probe a hit is avoiding.
//
// The mutable half is kept on the chunk rather than moved to the machine
// because a per-machine table would have to be found by the chunk first,
// and that lookup is the very thing the cache exists to avoid.
static bool cached_here(const LhatTable *table, uint32_t index, LhatValue key)
{
    return (size_t)index < table->entry_capacity &&
           lhat_value_equal(table->entries[index].key, key);
}
// 03 の 5.12改2: the collector's poll, beside the instructions that can have
// allocated -- Lua places its checkGC against the allocating opcodes the same
// way. Instructions that never allocate never ask; the paranoid build (debug)
// keeps the every-boundary poll in the run loop instead, since its purpose is
// exercising the barriers, not pacing.
#define LHAT_GC_POLL() do { if (m->objects.count >= m->threshold) { frame->pc = pc; lhat_gc_step(m); } } while (0)
// 09 の 2.1: whether the instruction at `at` begins a line the hook should
// be told about, and, when it is, the hook is called here. `frame->pc` is
// already `at + 1`, so lhat_machine_fault_frame reads this instruction.
//
// The rule is lua's: a new line, a jump back onto the same line (a loop), or
// the first instruction of a body just entered. A body is entered or
// returned to whenever the frame count moves, and the line to measure
// against is then the instruction that left -- the CALL, RESUME or YIELD one
// before this frame's, which is `at - 1` in the caller and 0 at a body's
// own top. Returns true when the hook, or a call it made, faulted -- the run
// ends then, the way a host function's fault ends it.
#if LHAT_WITH_DEBUGGER
static bool hook_line(Machine *m, Frame *frame, size_t at,
                      LhatRunStatus *status, LhatValue *value)
{
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t old = m->hook_pc;
    if (m->frame_count != m->hook_depth) {
        old = at > 0 ? at - 1 : 0;
        m->hook_depth = m->frame_count;
    }
    m->hook_pc = at;
    // Moving forward within one line is the only case that does not sound:
    // a jump back (at <= old) is a loop, and a changed line is a new line.
    if (at > old && chunk->lines[at] == chunk->lines[old]) {
        return false;
    }
    LhatFrameInfo where;
    lhat_machine_fault_frame((LhatMachine *)m, 0, &where);
    size_t frames_before = m->frame_count;
    m->hook_live = NULL;  // a call the hook makes is not itself hooked
    m->hook((LhatMachine *)m, m->hook_context, LHAT_DEBUG_LINE, &where);
    m->hook_live = m->hook;  // which the hook may have cleared
    return vm_host_faulted(m, frames_before, status, value);
}
#endif  // LHAT_WITH_DEBUGGER
// The run loop itself, shared by lhat_run (base_depth == 0, a fresh unit
// entered through its own wrapper closure) and lhat_machine_call
// (base_depth == m->frame_count at the time of the call, a value already
// callable pushed as one more frame). "the run is over" means the frame
// count has drained back down to base_depth, not to zero -- 0 stays right
// for lhat_run because nothing was on the machine before it pushed frame 0.
// 2.2: the frame's registers, read and written through the machine's two
// parallel runs. `m` and `rbase` are vm_run_frames' own locals; nothing outside
// it may use these.
#define R(i) lhat_slots_get(m->slots, rbase + (size_t)(i))
#define SET_R(i, v) lhat_slots_set(m->slots, rbase + (size_t)(i), (v))

// `draining` enters at the drain rather than at the frame's pc: the frame on
// top is a disposal one (vm_enter_disposal_frame), which has no instructions of
// its own to run -- only cleanups to walk. It is what the loop's own
// `goto drain` does for a disposal it entered itself, said from outside so
// that a host can start one (lhat_machine_collectgarbage).
static LhatRunResult run_frames_loop(Machine *m, size_t base_depth,
                                     bool draining);

// 04 の 11.6改: a nested run -- a host calling back in -- borrows run_base
// for as long as it runs and hands it back, so a fault in the run it came
// out of still bounds that run's own frames rather than the nested one's.
LhatRunResult vm_run_frames(Machine *m, size_t base_depth, bool draining)
{
    size_t outer = m->run_base;
    // 02 の 15.15: the slice belongs to the outermost run. A nested one --
    // a host calling back, sort^'s comparator, clone^'s policy -- has its
    // caller's C stack under it and roots on it (machine.h's native_hold),
    // so it cannot be left standing; it runs with no budget and gives the
    // outer run's remainder back on the way out. The drain is not a run of
    // the program's either.
    int64_t kept = m->steps_left;
    bool outermost = m->running_depth == 0 && !draining;
    m->steps_left = outermost ? m->budget : 0;
    m->running_depth++;
    LhatRunResult result = run_frames_loop(m, base_depth, draining);
    m->running_depth--;
    if (!outermost) {
        m->steps_left = kept;
    }
    m->run_base = outer;
    return result;
}

// 02 の 15.15: the slice, counted where a run turns back on itself. Reads
// the loop's own `frame`, `pc`, `chunk`, `at` and `base_depth`, so it can
// only stand inside run_frames_loop.
#define LHAT_TURN_BACK()                                                    \
    do {                                                                    \
        if (m->steps_left != 0 && --m->steps_left == 0) {                   \
            frame->pc = pc;                                                 \
            m->suspended = true;                                            \
            m->suspended_base = base_depth;                                 \
            return vm_finish(m, chunk, LHAT_RUN_SUSPENDED, lhat_nil(), at);    \
        }                                                                   \
    } while (0)

static LhatRunResult run_frames_loop(Machine *m, size_t base_depth,
                                     bool draining)
{
    m->run_base = base_depth;  // 04 の 11.6改: so vm_finish can bound a fault
    Frame *frame = &m->frames[m->frame_count - 1];
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t rbase = frame->base;
    size_t pc = frame->pc;

    // 02 の 10.7: the last collection of the run, and what it found. See the
    // drain, which is where both are decided. `at` is hoisted out of the
    // loop only so that `ending` can jump to the drain without stepping over
    // its initialiser -- which is what lets `draining` jump there too.
    bool end_swept = false;
    bool ending = false;
    size_t at = 0;

    if (draining) {
        goto drain;
    }

    while (pc < chunk->count) {
        // Between instructions, where every live value is in a register, a
        // frame or the open list. Inside one there is a half-built object the
        // roots do not name yet -- which is also why a write made in the same
        // instruction that made the object it goes into needs no barrier.
        //
        // 5.12: a step, not a collection. What this costs is bounded by
        // LHAT_GC_STEP_WORK whatever the heap has grown to.
#ifdef LHAT_GC_PARANOID
        // The paranoid build polls at every boundary on purpose: its point
        // is that a cycle is nearly always half done so every barrier gets
        // walked over (gc.c). The ordinary build polls where allocations
        // happen -- LHAT_GC_POLL below, placed the way Lua places checkGC.
        if (m->objects.count >= m->threshold) {
            frame->pc = pc;
            lhat_gc_step(m);
        }
#endif

        // 02 の 10.7: and here is where what the collector held back gets to
        // run. The heap is whole again and this is an ordinary place to call
        // L^ code, which is exactly what a finally^ body is. One coroutine
        // per boundary -- the rest keep, so a long list never becomes one
        // long pause.
        //
        // Never on top of a disposal already under way (`disposing`): a
        // cleanup is 10.7's own unwinding and another coroutine's cleanups
        // cutting into it would interleave two unwindings that have nothing
        // to do with each other. The queue waits; it is in no hurry.
        if (m->pending_dispose != NULL && !frame->disposing) {
            if (m->frame_count >= m->frame_capacity) {
                return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            // Just past what this frame uses, so nothing live is written
            // over. Disposal answers nil^ and no one is asking, but the
            // return still lands somewhere and this is somewhere harmless.
            uint8_t into = chunk->registers;
            size_t next_base = rbase + (into) + 1;
            // 4.3改: the queued one's own width. gc.c holds back only a
            // suspended BODY coroutine, so the closure is there to ask.
            if (next_base +
                    m->pending_dispose->closure->proto->chunk.registers >=
                m->slot_capacity) {
                return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            LhatCoroutine *co = lhat_gc_take_pending(m);
            frame->pc = pc;
            vm_enter_disposal_frame(m, co, next_base, into, &frame, &rbase,
                                 &chunk, &pc);
            // 15.4: same as an explicit dispose -- the slot a yield^ answers
            // into gets nil^, since nothing sent anything in.
            SET_R(co->sent_into, lhat_nil());
            goto drain;
        }

        // 02 の 10.7: the run already ended and only the queue was holding
        // it open. It is empty now, so back to the frame that was leaving.
        // Only once that frame is on top again -- while a cleanup of its
        // own is running there is a frame above it with instructions left.
        if (ending && m->frame_count == base_depth + 1) {
            goto drain;
        }

        LhatInstruction instruction = chunk->code[pc];
        at = pc++;

        // 09 の 2 章: the debugger's line hook, at the same boundary as the
        // GC step above -- every live value is in a register, a frame or the
        // open list. `frame->pc` is written first (to `at + 1`), so a
        // traceback the hook reads names this instruction, not the last.
#if LHAT_WITH_DEBUGGER
        if (m->hook_live != NULL) {
            frame->pc = pc;
            LhatRunStatus left;
            LhatValue left_with;
            if (hook_line(m, frame, at, &left, &left_with)) {
                return vm_finish(m, chunk, left, left_with, at);
            }
        }
#endif

        uint8_t a = lhat_a(instruction);
        uint8_t b = lhat_b(instruction);
        uint8_t cc = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);
        // 02 の 11.9: the comparison that sent control to call_operator,
        // when one did. `op` becomes SPACESHIP there -- that is the member to
        // look for -- and this is what the answer gets read against zero with.
        LhatOpcode derive_from = LHAT_FRAME_NO_DERIVE;
        // Whether call_operator's right operand is K[cc] rather than R(cc)
        // -- set only by the ADDK family's fallback below.
        bool k_right = false;
        // 03 の 5.1改: what GETINDEX and GETMEMBER share. Declared out here
        // because the second jumps into the first's body having settled them
        // -- the key it asks by, and the cache to fill on the way out (NULL
        // for a GETINDEX, which remembers nothing).
        //
        // Left unwritten on purpose: both cases set both before jumping in,
        // and every other instruction would pay for the stores.
        LhatValue member_key;
        LhatMemberCache *filling;

        switch (op) {
            case LHAT_BC_LOADK:
                SET_R(a, chunk->constants[lhat_bx(instruction)]);
                break;
            case LHAT_BC_LOADNIL:
                SET_R(a, lhat_nil());
                break;
            case LHAT_BC_LOADBOOL:
                SET_R(a, lhat_bool(b != 0));
                break;
            case LHAT_BC_MOVE:
                SET_R(a, R(b));
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
                if (arithmetic(op, R(b), R(cc), &out,
                               &status)) {
                    SET_R(a, out);
                    break;
                }
                // 02 の 11.3: numbers answer built in; anything else answers
                // with the member 11.8 names, or not at all.
                //
                // 11.3改: the right operand is reason enough to go on.
                // '1 + v' is the case the rule exists for -- number^ carries
                // the arithmetic and takes only its own kind, so the answer
                // can only be on the other side.
                // 05 の 8.9: a host value carries its registered operators
                // the same way a table carries 11.8's members.
                if (status != LHAT_RUN_TYPE_ERROR ||
                    (vm_table_of(R(b)) == NULL && vm_table_of(R(cc)) == NULL &&
                     !lhat_is_hostvalue(R(b)) && !lhat_is_hostvalue(R(cc)))) {
                    return vm_finish(m, chunk, status, lhat_nil(), at);
                }
                goto call_operator;
            }

            // The same four with the right operand a constant. A constant is
            // a number by construction (compile.c emits these for numeric
            // literals alone), so only the left can carry an operator.
            case LHAT_BC_ADDK:
            case LHAT_BC_SUBK:
            case LHAT_BC_MULK:
            case LHAT_BC_DIVK: {
                LhatValue out;
                LhatRunStatus status = LHAT_RUN_OK;
                op = (LhatOpcode)(op - LHAT_BC_ADDK + LHAT_BC_ADD);
                if (arithmetic(op, R(b), chunk->constants[cc], &out,
                               &status)) {
                    SET_R(a, out);
                    break;
                }
                if (status != LHAT_RUN_TYPE_ERROR ||
                    (vm_table_of(R(b)) == NULL && !lhat_is_hostvalue(R(b)))) {
                    return vm_finish(m, chunk, status, lhat_nil(), at);
                }
                k_right = true;
                goto call_operator;
            }

            case LHAT_BC_NEG: {
                // 02 の 11.8改: number^ carries its own negation and pays
                // nothing for the search, the same posture the binary
                // instructions take. Anything else asks for the member.
                if (!lhat_is_number(R(b))) {
                    goto call_operator;
                }
                // 14.8改: INT64_MIN is the one integer whose negation is not
                // an integer, so it widens like any other overflow.
                int64_t negated = 0;
                SET_R(a,
                    lhat_is_integer(R(b)) &&
                            subtract_exact(0, lhat_as_integer(R(b)),
                                           &negated)
                        ? lhat_integer(negated)
                        : lhat_real(-lhat_number_as_real(R(b))));
                break;
            }

            case LHAT_BC_NOT: {
                // 02 の 5.4's condition rule: only a bool is a truth value,
                // so this refuses anything else rather than inventing one.
                if (!lhat_is_bool(R(b))) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(!lhat_as_bool(R(b))));
                break;
            }

            case LHAT_BC_TYPEOF: {
                LhatRuntimeType *type = vm_tag_type(&m->objects, R(b));
                if (type == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)type));
                break;
            }

            // 11.9 with 11.9改: a type saying what equals what is asked
            // before the default answer is taken. The member is looked for
            // as '=' first and as '<=>' second, and whatever neither answers
            // falls back on what the value was already the same as -- 14.2's
            // identity for a table, 05 の 8.9's bytes for a host value. That
            // is what leaves every other value exactly as it was.
            case LHAT_BC_EQ:
            case LHAT_BC_NE:
                if (vm_table_of(R(b)) != NULL || vm_table_of(R(cc)) != NULL ||
                    lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    derive_from = op;
                    op = LHAT_BC_EQ;  // the name to look for; '≠' has none
                    goto call_operator;
                }
                // 14.8: two numbers within the error a real carries are one
                // number here. A key, a constant and 'is^' go on asking the
                // exact question -- lhat_value_close is only what '=' and
                // 11.9's orderings read.
                {
                    bool held =
                        lhat_value_close(R(b), R(cc),
                                         LHAT_NUMBER_TOLERANCE) ==
                        (op == LHAT_BC_EQ);
                    SET_R(a, lhat_bool(held));
                    // 5.1改5, as the orderings below
                    LhatInstruction paired = chunk->code[pc];
                    if (lhat_op(paired) == LHAT_BC_JUMP_FALSE &&
                        lhat_a(paired) == a) {
                        pc++;
                        if (!held) {
                            int32_t offset = lhat_jump_offset(paired);
                            pc = (size_t)((int64_t)pc + offset);
                            if (offset < 0) {
                                LHAT_TURN_BACK();
                            }
                        }
                    }
                }
                break;
            case LHAT_BC_SAME:
                // 05 の 8.9: a value type has no identity apart from its
                // bytes, so "the same" is the equality above.
                if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    SET_R(a, lhat_bool(lhat_is_hostvalue(R(b)) &&
                                       vm_hostvalue_equal(m->slots, rbase + b,
                                                       rbase + cc)));
                    break;
                }
                SET_R(a, lhat_bool(
                    lhat_value_same(R(b), R(cc))));
                break;

            case LHAT_BC_LT:
            case LHAT_BC_LE:
            case LHAT_BC_GT:
            case LHAT_BC_GE: {
                bool out = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (ordering(op, R(b), R(cc), &out, &status)) {
                    SET_R(a, lhat_bool(out));
                    // 03 の 5.1改5: the JUMP_FALSE that reads this answer,
                    // consumed on the spot when it stands right here --
                    // one turn for the pair. Any jump that lands on it
                    // still runs it as itself.
                    LhatInstruction paired = chunk->code[pc];
                    if (lhat_op(paired) == LHAT_BC_JUMP_FALSE &&
                        lhat_a(paired) == a) {
                        pc++;
                        if (!out) {
                            int32_t offset = lhat_jump_offset(paired);
                            pc = (size_t)((int64_t)pc + offset);
                            if (offset < 0) {
                                LHAT_TURN_BACK();
                            }
                        }
                    }
                    break;
                }
                // 11.9: numbers order themselves; anything else says
                // how it orders with a '<=>', and this reads the answer.
                derive_from = op;
                op = LHAT_BC_SPACESHIP;
                goto call_operator;
            }

            // 11.9: written out. number^ and string^ each order their
            // own; anything else answers with the member 11.8 names.
            case LHAT_BC_SPACESHIP: {
                int outcome = 0;
                if (!vm_three_way(R(b), R(cc), &outcome)) {
                    goto call_operator;
                }
                SET_R(a, lhat_integer(outcome));
                break;
            }

            // 02 の 15.15: a jump back is where every loop turns, and a
            // run that does not end has to turn -- straight-line code is as
            // long as the body and recursion runs out of frames. So this is
            // the one place a slice has to be counted, and an instruction
            // between two of them costs nothing.
            case LHAT_BC_JUMP: {
                int32_t offset = lhat_jump_offset(instruction);
                pc = (size_t)((int64_t)pc + offset);
                if (offset < 0) {
                    LHAT_TURN_BACK();
                }
                break;
            }

            case LHAT_BC_JUMP_FALSE: {
                if (!lhat_is_bool(R(a))) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (!lhat_as_bool(R(a))) {
                    int32_t offset = lhat_jump_offset(instruction);
                    pc = (size_t)((int64_t)pc + offset);
                    if (offset < 0) {
                        LHAT_TURN_BACK();
                    }
                }
                break;
            }

            case LHAT_BC_CLOSURE: {
                LHAT_GC_POLL();  // this case allocates
                const LhatProto *nested =
                    frame->closure->proto->protos[lhat_bx(instruction)];
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = nested;
                closure->upvalue_count = nested->upvalue_count;
                if (nested->upvalue_count > 0) {
                    closure->upvalues = (LhatUpvalue **)lhat_calloc(
                        nested->upvalue_count, sizeof *closure->upvalues);
                    if (closure->upvalues == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                // 5.4: a register of this frame, one of its own upvalues
                // when the name came from further out, or -- 15.10's
                // this^^ -- this frame's own closure, boxed closed on the
                // spot: nothing on the stack holds it, so there is nothing
                // to keep open.
                for (size_t i = 0; i < nested->upvalue_count; i++) {
                    const LhatUpvalueDesc *desc = &nested->upvalues[i];
                    LhatUpvalue *made = NULL;
                    switch (desc->source) {
                        case LHAT_UPVALUE_REGISTER:
                            made = vm_capture(m, rbase + desc->index);
                            break;
                        case LHAT_UPVALUE_OUTER:
                            made = frame->closure->upvalues[desc->index];
                            break;
                        case LHAT_UPVALUE_THIS:
                            made = (LhatUpvalue *)lhat_object_alloc(
                                &m->objects, sizeof *made,
                                LHAT_OBJECT_UPVALUE);
                            if (made != NULL) {
                                lhat_ref_set(
                                    lhat_upvalue_closed_ref(made),
                                    lhat_object(
                                        (LhatObject *)frame->closure));
                                made->location =
                                    lhat_upvalue_closed_ref(made);
                                made->next_open = NULL;
                            }
                            break;
                    }
                    closure->upvalues[i] = made;
                    if (made == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            // 03 の 5.11c: strict already found the one arm that fits (14.12
            // leaves at most one), so the call ahead takes it instead of
            // asking every candidate again. Nothing else is touched: a value
            // that is not a group reached here some way the checker did not
            // read, and the ordinary call is still right for it.
            case LHAT_BC_PICKARM: {
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t which = lhat_bx(instruction);
                    if (which < group->count) {
                        SET_R(a, group->candidates[which]);
                    }
                }
                break;
            }

            case LHAT_BC_GETUPVAL:
                SET_R(a, lhat_ref_get(frame->closure->upvalues[b]->location));
                break;

            case LHAT_BC_SETUPVAL: {
                // 5.12: an open upvalue's place is a stack slot and the
                // stack is a root, so only a closed one gains anything the
                // collector has not seen. The barrier asks which it is by
                // asking whether the upvalue is black.
                LhatUpvalue *upvalue = frame->closure->upvalues[b];
                lhat_ref_set(upvalue->location, R(a));
                lhat_gc_barrier(m, (LhatObject *)upvalue, R(a));
                break;
            }

            // 02 の 11.2: '..' is concatenation in general, and strings are
            // the case that is settled. 11.3 leaves the rest to the
            // operator's own definition, which needs op^.
            case LHAT_BC_CONCAT: {
                LHAT_GC_POLL();  // this case allocates
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_STRING) &&
                    lhat_is_object_kind(R(cc), LHAT_OBJECT_STRING)) {
                    const LhatString *left =
                        (const LhatString *)lhat_as_object(R(b));
                    const LhatString *right =
                        (const LhatString *)lhat_as_object(R(cc));
                    LhatString *joined =
                        lhat_string_concat(&m->objects, left, right);
                    if (joined == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)joined));
                    break;
                }

                // 02 の 11.2改: two plain tables concatenate, built in the
                // way two strings do. A definition never comes through here
                // (14.2 settles composition at compile time), and a table
                // that carries an op^.. of its own is not plain -- it is an
                // instance, and falls through to the search below.
                if (vm_plain_table(R(b)) && vm_plain_table(R(cc))) {
                    bool collided = false;
                    LhatTable *joined = vm_concat_tables(
                        m, (const LhatTable *)lhat_as_object(R(b)),
                        (const LhatTable *)lhat_as_object(R(cc)), &collided);
                    if (joined == NULL) {
                        return vm_finish(m, chunk,
                                      collided ? LHAT_RUN_BAD_KEY
                                               : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)joined));
                    break;
                }

                // 02 の 11.3: a string answers above, built in. Anything else
                // answers with the member 11.8 names, or not at all -- and
                // 11.3改 reads the right operand for one too.
                if (vm_table_of(R(b)) == NULL && vm_table_of(R(cc)) == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                goto call_operator;
            }

            case LHAT_BC_CLOSE:
                vm_close_upvalues(m, rbase + a);
                break;

            case LHAT_BC_CLOSEONE:
                vm_close_one_upvalue(m, rbase + a);
                break;

            // 02 の 15.10: the frame already holds it, so naming it costs a
            // move rather than a vm_capture.
            case LHAT_BC_THIS:
                SET_R(a,
                    lhat_object((LhatObject *)(void *)frame->closure));
                break;

            // 05 の 8.6: one table per machine, so naming it is a move too.
            case LHAT_BC_ENV:
                SET_R(a, lhat_object((LhatObject *)m->environment));
                break;

            // 05 の 5.3: a unit is a body like any other, so requiring it is
            // making a closure of it and calling that. What makes it load
            // once is the guard the unit itself begins with, not this.
            case LHAT_BC_UNIT: {
                LHAT_GC_POLL();  // this case allocates
                // The number indexes the table of the unit this body was
                // written in (LhatUnitTable), not anything of the machine's
                // -- which is what lets a program grow under it.
                size_t which = lhat_bx(instruction);
                const LhatUnitTable *units = frame->closure->proto->units;
                if (units == NULL || which >= units->count ||
                    units->protos[which] == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_NO_SUCH_UNIT, lhat_nil(), at);
                }
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = units->protos[which];
                closure->upvalues = NULL;
                closure->upvalue_count = 0;
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            case LHAT_BC_NEWTABLE: {
                LHAT_GC_POLL();  // this case allocates
                LhatTable *table = lhat_table_new(&m->objects);
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                table->is_definition = b != 0;  // 14.9
                SET_R(a, lhat_object((LhatObject *)table));
                break;
            }

            // 05 の 8.6: what require^ answers is the machine's record
            // of what a unit published. Sealed once it is built, so that a
            // requiring unit cannot add to it or write over it -- not even by
            // passing it to a p^ whose parameter is written t^{ … }, which is
            // the one path check.c cannot name.
            case LHAT_BC_SEAL: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                table->sealed = true;
                break;
            }

            // 04 の 11.3: a key that is not there answers nil^, so the only
            // way this fails is being asked of something that is not a table.
            // An error answers from its fields: 2.3 gives every kind message
            // and cause, and they are reached the same way as a member.
            // 03 の 5.1改: a written member name, which is the same key every
            // time this site runs. What it remembers is where the answer was
            // last found, and a hit is two comparisons -- against a walk of
            // the 14.7 chain and a probe with a full key equality in it.
            //
            // A miss falls into the body below having settled `key` from the
            // cache and `filling` to the cache, so the ordinary read answers
            // and fills it on the way out. That way there is one place that
            // decides what a member read means, and this is only about how
            // long it takes (4.2).
            case LHAT_BC_GETMEMBER: {
                LhatMemberCache *cache = &chunk->member_caches[cc];
                const LhatTable *start = vm_readable_table(R(b));
                if (cache->answered != NULL && start != NULL &&
                    (cache->from_definition
                         // An instance is a fresh table per value, so what is
                         // compared is not the table but the fact that it has
                         // never been structurally written since it was cloned
                         // -- and 5.10 seals the prototype, so such a clone
                         // carries the prototype's keys and no others and
                         // cannot be shadowing this member.
                         ? (start->version == 0 &&
                            start->definition == cache->answered &&
                            cache->answered->version == cache->version)
                         // 05 の 8.8 shares one members table per host type,
                         // so for those this is the same table every time.
                         : (start == cache->answered &&
                            start->version == cache->version)) &&
                    cached_here(cache->answered, cache->index,
                                chunk->constants[cache->key])) {
                    SET_R(a, cache->answered->entries[cache->index].value);
                    break;
                }
                member_key = chunk->constants[cache->key];
                filling = cache;
                goto member_body;
            }

            // 03 の 5.1改4: GETMEMBER fused with its call. A hit loads
            // the member and walks straight into the paired call
            // instruction; a miss is GETMEMBER to the letter, and the pair
            // runs as itself on the next turn.
            case LHAT_BC_CALLMEMBER: {
                LhatMemberCache *cache = &chunk->member_caches[cc];
                const LhatTable *start = vm_readable_table(R(b));
                if (cache->answered != NULL && start != NULL &&
                    (cache->from_definition
                         ? (start->version == 0 &&
                            start->definition == cache->answered &&
                            cache->answered->version == cache->version)
                         : (start == cache->answered &&
                            start->version == cache->version)) &&
                    cached_here(cache->answered, cache->index,
                                chunk->constants[cache->key])) {
                    SET_R(a, cache->answered->entries[cache->index].value);
                    at = pc;
                    instruction = chunk->code[pc++];
                    a = lhat_a(instruction);
                    b = lhat_b(instruction);
                    cc = lhat_c(instruction);
                    op = lhat_op(instruction);
                    goto call_entry;
                }
                member_key = chunk->constants[cache->key];
                filling = cache;
                goto member_body;
            }

            case LHAT_BC_GETINDEX:
                LHAT_GC_POLL();  // string cuts, written-down members
                member_key = R(cc);
                filling = NULL;
                goto member_body;

            member_body: {
                LhatRunStatus status = vm_get_member(
                    m, rbase + a, rbase + b, rbase + cc, member_key, filling);
                if (status != LHAT_RUN_OK) {
                    return vm_finish(m, chunk, status, lhat_nil(), at);
                }
                break;
            }

            // 02 の 13.10: the position a destructuring bind just read.
            // 04 の 11.3 spells absence nil^, so a position answering nil^
            // is a position the value does not have, and 13.10 makes a count
            // that does not agree an error rather than Lua's list
            // adjustment. Under strict the checker has already said so and
            // the compiler leaves this out; this is where relaxed and an
            // unchecked compile land.
            // 02 の 13.8改: what stands between an error (or a plain value)
            // arriving where a run was expected and the slots after it being
            // read as positions that were never written.
            case LHAT_BC_CHECKRUN: {
                LhatValue head = R(a);
                // 13.8改: the short side of a widened fold arrives narrow --
                // one value, or a run of fewer positions -- and the missing
                // positions are nil^, which is exactly what the folded type
                // said of them ('(A, B)|C' is '(A|C, B|nil^)'). They are
                // padded here, into the slots the binding reserved. An arm a
                // construct discriminates -- nil^, an error -- never folds,
                // so meeting one here is the mismatch it always was; a run
                // wider than the reservation stays one too.
                if (!lhat_is_run(head)) {
                    if (lhat_is_nil(head) ||
                        lhat_is_object_kind(head, LHAT_OBJECT_ERROR) ||
                        lhat_is_hostvalue(head)) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    if (rbase + a + (size_t)b >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                      lhat_nil(), at);
                    }
                    SET_R(a + 1, head);
                    for (size_t i = 2; i <= (size_t)b; i++) {
                        SET_R(a + i, lhat_nil());
                    }
                    SET_R(a, lhat_run_head((size_t)b));
                    break;
                }
                size_t width = lhat_run_width(head);
                if (width == (size_t)b) {
                    break;
                }
                if (width > (size_t)b ||
                    rbase + a + (size_t)b >= m->slot_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                for (size_t i = width + 1; i <= (size_t)b; i++) {
                    SET_R(a + i, lhat_nil());
                }
                SET_R(a, lhat_run_head((size_t)b));
                break;
            }

            // 02 の 13.8改: a tuple written as a value. The positions are in
            // the slots above already -- the head goes down over them, and
            // from here the run is the one every other path produces.
            case LHAT_BC_MAKERUN: {
                if (b < 2 || (size_t)b > LHAT_MAX_TUPLE ||
                    rbase + a + (size_t)b >= m->slot_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                SET_R(a, lhat_run_head((size_t)b));
                break;
            }

            // 02 の 13.8改: pack^ -- the one bridge from a tuple to a table.
            // 14.10 numbers positions from 1, which is what a destructuring
            // and 't[1]' both read.
            case LHAT_BC_PACK: {
                LHAT_GC_POLL();  // this case allocates
                if (!lhat_is_run(R(a)) || lhat_run_width(R(a)) != (size_t)b) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                LhatTable *packed = lhat_table_new(&m->objects);
                if (packed == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                for (size_t i = 0; i < (size_t)b; i++) {
                    // The key is a positive integer every time, so `refused`
                    // (04 の 11.3's nil^, a NaN) cannot come back set.
                    bool refused = false;
                    if (!vm_set_key(m, packed, lhat_integer((int64_t)i + 1),
                                 R(a + 1 + i), &refused)) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                }
                SET_R(a, lhat_object((LhatObject *)packed));
                break;
            }

            // 02 の 14.15: the declaration's seat -- the key, no value.
            case LHAT_BC_RESERVE: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
                if (!lhat_table_reserve(table, R(b))) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                  lhat_nil(), at);
                }
                lhat_gc_barrier_back(m, (LhatObject *)table, R(b));
                break;
            }

            case LHAT_BC_SETINDEX: {
            set_index:;
                // 05 の 8.9: 'v.x := n' writes the field's bytes in place --
                // the owner register IS the value, so the write lands in the
                // very slots the name holds. Only a registered field takes a
                // write; the members are the host's (8.8's rule holds).
                if (lhat_is_hostvalue(R(a))) {
                    const LhatHostValueField *field = vm_hostvalue_field_named(
                        lhat_as_hostvalue_tag(R(a)), R(b));
                    if (field == NULL ||
                        !vm_hostvalue_field_set(
                            m->slots.values + rbase + a + 1, field, R(cc))) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    break;
                }
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 05 の 8.6: the machine's own tables are written by
                // the host, not by an instruction. check.c refuses the ones it
                // can name, but a t^{ … } parameter carries no mark of this,
                // so the same question is asked once more where the write
                // actually happens.
                if (table->sealed) {
                    return vm_finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!vm_set_key(m, table, R(b), R(cc), &refused)) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // nil^ is how 11.3 spells "not there", so it cannot also be
                // a key. Neither can a NaN, which is not equal to itself.
                if (refused) {
                    return vm_finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            // 14.12: the name keeps what it had and gains another way to be
            // called. What was there may already be a group, or the first of
            // two, or nothing when the base did not define it.
            case LHAT_BC_ADDOVERLOAD: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                LhatOverload *group = NULL;
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    group = (LhatOverload *)lhat_as_object(held);
                } else {
                    group = lhat_overload_new(&m->objects);
                    if (group == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    if (!lhat_is_nil(held) && !lhat_overload_add(group, held)) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    bool refused = false;
                    if (!vm_set_key(m, table, R(b),
                                 lhat_object((LhatObject *)group), &refused)) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                if (!lhat_overload_add(group, R(cc))) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // 5.12: `group` may be one already in the table, black since
                // an earlier step. A group gains arms one at a time, so the
                // forward barrier is the cheaper of the two.
                lhat_gc_barrier(m, (LhatObject *)group, R(cc));
                break;
            }

            // 14.12: an override^ replaces the one arm it overlaps, and the
            // arms an overload^ put there are otherwise untouched. A plain
            // write would take the whole group with them.
            case LHAT_BC_OVERRIDEINDEX: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_with_first(
                        &m->objects, group, R(cc));
                    if (made == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!vm_set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                goto set_index;
            }

            // 03 の 5.11c: the same write with the arm named. What it replaces
            // is dropped rather than shadowed, so the arms left are the arms
            // the checker's type says the name carries -- which is what makes
            // an arm index mean the same thing on both sides. super^ is
            // unaffected: it was bound from the old group before this ran.
            case LHAT_BC_OVERRIDEARM: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_replacing(
                        &m->objects, group, cc, R(b + 1));
                    if (made == NULL) {
                        // No such arm. The group is not the one the checker
                        // read, so the honest answer is 14.12's own: put the
                        // replacement in front and let the search sort it out.
                        made = lhat_overload_with_first(&m->objects, group,
                                                        R(b + 1));
                    }
                    if (made == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!vm_set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                // Nothing overloaded under the name, so this is a plain write.
                // set_index cannot be jumped to for it: that reads the value
                // from C, which here is the arm.
                if (table->sealed) {
                    return vm_finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!vm_set_key(m, table, R(b), R(b + 1), &refused)) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                if (refused) {
                    return vm_finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            case LHAT_BC_NEWERROR: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_ERROR_KIND)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatErrorKind *kind =
                    (const LhatErrorKind *)lhat_as_object(R(b));
                LhatError *error = lhat_error_new(&m->objects, kind);
                if (error == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)error));
                break;
            }

            // 04 の 2.6: an error satisfies no type but an error's, so asking
            // whether a value is one is a question about the value alone.
            case LHAT_BC_ISERROR:
                SET_R(a,
                    lhat_bool(lhat_is_object_kind(R(b),
                                                  LHAT_OBJECT_ERROR)));
                break;

            // 02 の 13.11 with 03 の 5.13: R[C] is always a type the
            // compiler lowered -- a written type, never a runtime value --
            // and lhat_value_satisfies answers, the same relation ASCAST and
            // vm_fits_call already trust. (A fallback that read a shape off a
            // definition table at run time: a value that only arrives while
            // the program runs carries no type to ask about.)
            case LHAT_BC_FITS: {
                LhatValue wanted = R(cc);
                if (!lhat_is_object_kind(wanted, LHAT_OBJECT_TYPE)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatRuntimeType *type =
                    (const LhatRuntimeType *)lhat_as_object(wanted);
                SET_R(a,
                    lhat_bool(lhat_value_satisfies(R(b), type)));
                break;
            }

            case LHAT_BC_ISNIL:
                SET_R(a, lhat_bool(lhat_is_nil(R(b))));
                break;

            // 14.11: construction is the machine's -- a copy of the
            // prototype the definition's self^ holds, which is where every
            // field starts as its default. A table held in a field is
            // copied as its own tree; a definition among the values is
            // shared. 14.3 and 14.7: the copy holds its own fields and
            // reads the shared members through the link; 14.2 fixes it
            // here and gives no way to change it afterwards.
            case LHAT_BC_NEWINSTANCE: {
                LHAT_GC_POLL();  // this case allocates
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_TABLE)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatTable *definition =
                    (const LhatTable *)lhat_as_object(R(b));
                LhatValue held = lhat_table_get(
                    definition, lhat_object((LhatObject *)m->self_key));
                LhatTable *instance;
                bool too_deep = false;
                if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
                    instance = vm_clone_table(
                        m, (const LhatTable *)lhat_as_object(held), 0,
                        &too_deep);
                } else {
                    // A table wearing the definition mark without a
                    // prototype -- a host built it. Empty, with the link.
                    instance = lhat_table_new(&m->objects);
                }
                if (instance == NULL) {
                    return vm_finish(m, chunk,
                                  too_deep ? LHAT_RUN_MUTABLE_DEFAULT
                                           : LHAT_RUN_OUT_OF_MEMORY,
                                  lhat_nil(), at);
                }
                instance->definition = definition;
                SET_R(a, lhat_object((LhatObject *)instance));
                break;
            }

            // 14.11: the prototype goes under the definition's self^, sealed
            // -- what it holds is every instance's starting point, and 8.8
            // closes a definition to writes -- and linked, so its own reads
            // reach the members the way any instance's do. The values are
            // baked here because this is where they actually are: a table
            // becomes the prototype's own sealed tree, whatever expression
            // produced it, and what nothing may share is refused -- the same
            // answer the checker gives where it ran.
            // 14.7改2: what the definition delegates to. The key is a chunk
            // constant, so nothing is written into the machine's heap here
            // and no barrier is owed.
            case LHAT_BC_SETDELEGATE: {
                LhatTable *table = vm_table_of(R(a));
                if (table == NULL ||
                    !lhat_is_object_kind(R(b), LHAT_OBJECT_STRING)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                table->delegate_key = R(b);
                table->delegate_from_self = cc != 0;
                break;
            }

            case LHAT_BC_SETPROTO: {
                LHAT_GC_POLL();  // this case allocates
                LhatTable *table = vm_table_of(R(a));
                LhatTable *proto = vm_table_of(R(b));
                if (table == NULL || proto == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 02 の 14.15 with 03 の 4.2: a seat the definition itself
                // answers as a member is dropped before the seal -- filled,
                // it would shadow that member with the clone's version still
                // 0, the one hole in 5.1改's guard. The checker refuses the
                // clash first; this is the unchecked run's backstop.
                lhat_table_prune_seats(proto, table);
                for (size_t i = 0; i < proto->array_count; i++) {
                    LhatValue baked = lhat_nil();
                    bool bad = false;
                    if (!vm_bake_default(m, lhat_slots_get(proto->array, i),
                                      &baked, 0, &bad)) {
                        return vm_finish(m, chunk,
                                      bad ? LHAT_RUN_MUTABLE_DEFAULT
                                          : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    lhat_slots_set(proto->array, i, baked);
                    lhat_gc_barrier_back(m, (LhatObject *)proto, baked);
                }
                for (size_t i = 0; i < proto->entry_capacity; i++) {
                    LhatTableEntry *entry = &proto->entries[i];
                    if (lhat_is_nil(entry->key)) {
                        continue;
                    }
                    LhatValue baked = lhat_nil();
                    bool bad = false;
                    if (!vm_bake_default(m, entry->value, &baked, 0, &bad)) {
                        return vm_finish(m, chunk,
                                      bad ? LHAT_RUN_MUTABLE_DEFAULT
                                          : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    entry->value = baked;
                    lhat_gc_barrier_back(m, (LhatObject *)proto, baked);
                }
                proto->definition = table;
                proto->sealed = true;
                bool refused = false;
                if (!vm_set_key(m, table,
                             lhat_object((LhatObject *)m->self_key),
                             lhat_object((LhatObject *)proto), &refused)) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                break;
            }

            // 05 の 8.9: the host value at R[B..], boxed. The head slot
            // carries the tag, and the tag the width, so the copy is the
            // whole run.
            case LHAT_BC_BOX: {
                LHAT_GC_POLL();  // this case allocates
                // 8.9改: C bit 0 seals the box (constbox^); bit 1 reads
                // R[B] as a box to copy rather than a value laid out.
                const LhatValueUnion *from;
                const LhatHostValueTag *tag;
                if ((cc & 2) != 0) {
                    if (!lhat_is_object_kind(R(b), LHAT_OBJECT_HOSTVALUE_BOX)) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    const LhatHostValueBox *source =
                        (const LhatHostValueBox *)lhat_as_object(R(b));
                    tag = lhat_hostvalue_box_tag(source);
                    from = source->run;
                } else {
                    if (!lhat_is_hostvalue(R(b))) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    tag = lhat_as_hostvalue_tag(R(b));
                    from = m->slots.values + rbase + b;
                }
                LhatHostValueBox *box =
                    lhat_hostvalue_box_new(&m->objects, tag);
                if (box == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                for (size_t i = 0; i < tag->width; i++) {
                    box->run[i] = from[i];
                }
                box->sealed = (cc & 1) != 0;
                SET_R(a, lhat_object((LhatObject *)box));
                break;
            }

            case LHAT_BC_CALL:
            case LHAT_BC_CALLMETHOD:
            case LHAT_BC_TAILCALL:
            case LHAT_BC_TAILCALLMETHOD: {
            call_entry:;
                // 14.4: whether the receiver was laid out below the arguments.
                // 5.3: and whether the call may take this frame over rather
                // than push one -- which only the closure path below can do,
                // so everything up to it reads the same either way.
                bool as_method = op == LHAT_BC_CALLMETHOD ||
                                 op == LHAT_BC_TAILCALLMETHOD;
                bool tail = op == LHAT_BC_TAILCALL ||
                            op == LHAT_BC_TAILCALLMETHOD;
                LHAT_GC_POLL();  // hosts, variadic tables, coroutines

                // 14.12: at most one candidate fits, so this is a search and
                // not a choice -- no ranking, no ambiguity to report. It ends
                // at the first that takes what it was given.
                //
                // Settled before anything else looks at the callee: 05 の 8.7
                // lets a registration be one of the arms, so what is chosen
                // may be a host function, and every path below has to see
                // what was chosen rather than the group it came out of.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t picked_skip = 1;
                    // 2.2: vm_fits_call reads the callee and arguments as one
                    // LhatValue run, which the stack is not -- so the
                    // slots are gathered once and every candidate reads the
                    // same copy.
                    LhatValue lineup[LHAT_MAX_REGISTERS + 2];
                    for (size_t i = 0; i <= (size_t)b + 1; i++) {
                        lineup[i] = R(a + i);
                    }
                    LhatValue chosen = lhat_nil();
                    for (size_t i = 0; i < group->count; i++) {
                        if (vm_fits_call(group->candidates[i], lineup, b,
                                      as_method, &picked_skip)) {
                            chosen = group->candidates[i];
                            break;
                        }
                    }
                    if (lhat_is_nil(chosen)) {
                        return vm_finish(m, chunk, LHAT_RUN_NO_CANDIDATE,
                                      lhat_nil(), at);
                    }
                    SET_R(a, chosen);
                }

                // 05 の 8.7: the host wrote this one in C. 13.1 settled how
                // many arguments there are before anything ran, so they are
                // handed over as they lie rather than pushed one by one.
                // 04 の 12.8 makes an error a value, so what comes back is
                // one -- there is no unwinding to arrange.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_HOST)) {
                    LhatHost *host = (LhatHost *)lhat_as_object(R(a));
                    size_t skip = as_method ? 2 : 1;

                    // 13.7: 'expr...' wrote a table in the last slot instead
                    // of an ordinary argument. The closure path below unpacks
                    // one into the frame it is about to push; there is no
                    // frame to push here, so it is unpacked into an array of
                    // its own further down.
                    const LhatTable *spread_table = NULL;
                    size_t spread_run = SIZE_MAX;
                    size_t written = b;
                    // 13.8改: C carries the reserved-slot count too, so the
                    // spread is the low bit rather than the whole byte.
                    if ((cc & LHAT_CALL_SPREAD) != 0) {
                        if (b == 0) {
                            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        LhatValue spread_from = R(a + skip + b - 1);
                        // 13.8改: a tuple spreads as the run it already is.
                        // Its positions are the slots after the head, so
                        // nothing is unpacked -- only counted.
                        if (lhat_is_run(spread_from)) {
                            spread_run = rbase + a + skip + (size_t)b - 1;
                            written =
                                (size_t)b - 1 + lhat_run_width(spread_from);
                        } else {
                            spread_table = vm_table_of(spread_from);
                            if (spread_table == NULL) {
                                return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            written =
                                (size_t)b - 1 + spread_table->array_count;
                        }
                    }

                    // 13.4 keeps self^ out of the parameter list, so what the
                    // call wrote is compared against the list and the
                    // receiver is handed over besides it. 13.7 makes that
                    // comparison a floor once the signature ended in '...'.
                    if (host->has_variadic ? written < host->parameters
                                           : written != host->parameters) {
                        return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }

                    // 14.4: the receiver comes first, and sits just below the
                    // arguments the call wrote.
                    bool receiver_first =
                        host->takes_self && as_method;
                    size_t given = written + (receiver_first ? 1 : 0);
                    // 05 の 8.9: a host value receiver holds its width of
                    // slots, so a skipped receiver is skipped whole.
                    size_t receiver_width =
                        as_method && lhat_is_hostvalue(R(a + 1))
                            ? lhat_as_hostvalue_tag(R(a + 1))->width
                            : 1;
                    // 2.2: the stack holds no LhatValue run any more, so
                    // the arguments a host receives are gathered into one.
                    // 05 の 8.9: walked by slot rather than by value -- a
                    // host value argument is one argument, its head handed
                    // over aiming into the stack, and its width of slots
                    // stepped past.
                    LhatValue gathered[LHAT_MAX_REGISTERS + 2];
                    size_t slot = receiver_first
                                      ? (size_t)a + 1
                                      : (size_t)a +
                                            (as_method
                                                 ? 1 + receiver_width
                                                 : 1);
                    for (size_t i = 0; i < given; i++) {
                        LhatValue held = R(slot);
                        if (lhat_is_hostvalue(held)) {
                            gathered[i] =
                                hostvalue_argument(m->slots, rbase + slot);
                            slot += lhat_as_hostvalue_tag(held)->width;
                        } else {
                            gathered[i] = held;
                            slot += 1;
                        }
                    }
                    const LhatValue *arguments = gathered;

                    // Packed only where a spread broke the contiguity the
                    // registers otherwise have. An allocation rather than the
                    // stack above this frame, because a host function may call
                    // back in and lhat_machine_call starts its frame exactly
                    // there. Nothing in it needs rooting: every value is also
                    // a position of `spread_table` or a register of this
                    // frame, and both stay reachable for as long as the call
                    // does.
                    LhatValue *packed = NULL;
                    if ((spread_table != NULL || spread_run != SIZE_MAX) &&
                        given > 0) {
                        packed = (LhatValue *)lhat_alloc(given * sizeof *packed);
                        if (packed == NULL) {
                            return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        size_t into = 0;
                        if (receiver_first) {
                            packed[into++] = R(a + 1);
                        }
                        for (size_t i = 0; i < written; i++) {
                            packed[into++] = call_arg(m->slots, rbase, a, skip,
                                                      spread_table, spread_run,
                                                      (size_t)b - 1, i);
                        }
                        arguments = packed;
                    }

                    // 05 の 8.8: a dispose^ written by hand is the same
                    // giving-back the collection would do, so it is marked
                    // here and 10.7 keeps the sweep from doing it again.
                    if (host->takes_self && given > 0 &&
                        lhat_is_object_kind(arguments[0],
                                            LHAT_OBJECT_HOSTDATA)) {
                        LhatHostData *data =
                            (LhatHostData *)lhat_as_object(arguments[0]);
                        // 8.8改: the nearest release on the chain, since an
                        // inherited dispose^ is the type's own way back.
                        const LhatHostDataTag *by =
                            lhat_hostdata_releaser(data->tag);
                        if (by != NULL && by->release == host->call) {
                            data->released = true;
                        }
                    }
                    // 04 の 11.6改: the host may read the frames while it
                    // runs, and this frame's line is read off its saved pc.
                    frame->pc = pc;
                    size_t frames_before = m->frame_count;
                    LhatValue answered = lhat_nil();
                    bool said = vm_call_host_fn(m, host->call, host->context,
                                             arguments, given, &answered);
                    lhat_free(packed);
                    if (!said) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    // 05 の 8.7改 and 8.7改2: a nested run that faulted, or
                    // a panic the host asked for, ends this run here --
                    // running on would stack this frame's own calls over
                    // the standing ones, and the traceback keeps the chain.
                    LhatRunStatus left = LHAT_RUN_OK;
                    LhatValue left_with = lhat_nil();
                    if (vm_host_faulted(m, frames_before, &left, &left_with)) {
                        return vm_finish(m, chunk, left, left_with, at);
                    }
                    // 05 の 8.9: a host value answer arrives as a
                    // head-shaped run and is written out whole on the spot,
                    // before anything else can touch the scratch it may live
                    // in.
                    if (lhat_is_hostvalue(answered)) {
                        // 05 の 8.9: the call site said the width it made
                        // room for (compile's prepared); a site that could
                        // not know the type reserved one slot, and writing
                        // past it would eat the neighbours -- refused, as
                        // every width the types never settled is.
                        const LhatValueUnion *arun = answered.as.hostvalue_run;
                        if (arun == NULL || arun[0].hostvalue == NULL ||
                            lhat_call_prepared(cc) <
                                arun[0].hostvalue->width ||
                            !vm_place_hostvalue_answer(m, rbase + a, answered)) {
                            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        break;
                    }
                    // 02 の 13.8改: and the several values a host answers
                    // with go down the same way. Until this, the host path
                    // was the one caller that never read C's reserved count
                    // -- a registration saying '-> (A, B)' compiled to a
                    // reservation nothing filled, and the slots behind the
                    // head kept whatever the frame last had there.
                    size_t room = lhat_call_prepared(cc);
                    if (lhat_is_run(answered)) {
                        if (!vm_place_run_answer(m, rbase + a, room, answered)) {
                            return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        break;
                    }
                    // Whatever the host may have staged, it did not answer
                    // with it, so the room is free again.
                    m->tuple_scratch_count = 0;
                    // 13.8改 with 04 の 3.1, and 05 の 8.9改: room past one
                    // slot says the site reserved for something wide -- a
                    // run ('-> (A, B)'), or a host value's own width
                    // ('-> Vector3|nil^'). One value coming back fills it
                    // only when that value is the arm the head slot tells
                    // apart: an error for a try^ to find, or a nil^ for a
                    // '??'. Anything else is a registration promising a
                    // shape its C never answers with, and the promise is
                    // the host's own -- so it faults here rather than
                    // leaving the slots behind the head as they were.
                    if (room > 1 && !lhat_is_nil(answered) &&
                        !lhat_is_object_kind(answered, LHAT_OBJECT_ERROR)) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, answered);
                    break;
                }

                // 02 の 12.6 and 15.6: resume and dispose are the runtime's,
                // not the program's, so they are performed rather than called.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_NATIVE)) {
                    const LhatNative *native =
                        (const LhatNative *)lhat_as_object(R(a));
                    size_t first = a + (as_method ? 2 : 1);
                    LhatValue sent = b > 0 ? R(first) : lhat_nil();
                    if (native->kind != LHAT_NATIVE_START &&
                        native->kind != LHAT_NATIVE_RESUME &&
                        native->kind != LHAT_NATIVE_DISPOSE &&
                        native->kind != LHAT_NATIVE_DONE &&
                        native->kind != LHAT_NATIVE_STARTED) {
                        LhatRunStatus status = vm_call_native(
                            m, native, rbase + a, rbase + first, b,
                            lhat_call_prepared(cc));
                        if (status != LHAT_RUN_OK) {
                            return vm_finish(m, chunk, status, lhat_nil(), at);
                        }
                        break;
                    }

                    if (!lhat_is_object_kind(native->bound,
                                             LHAT_OBJECT_COROUTINE)) {
                        return vm_finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                    }
                    LhatCoroutine *co =
                        (LhatCoroutine *)lhat_as_object(native->bound);

                    // 15.6改: the two questions, answered before any of the
                    // guards below. Neither runs the body, so both hold on a
                    // finished coroutine, where everything else faults, and
                    // on a fresh one, where resume does.
                    if (native->kind == LHAT_NATIVE_DONE ||
                        native->kind == LHAT_NATIVE_STARTED) {
                        if (b != 0) {
                            return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                        }
                        SET_R(a, lhat_bool(
                            native->kind == LHAT_NATIVE_DONE
                                ? co->state == LHAT_COROUTINE_DONE
                                : co->state != LHAT_COROUTINE_FRESH));
                        break;
                    }

                    // 15.2: the machine holds this itself rather than
                    // trusting the checker to have (vm.h's opening comment).
                    // start takes none, since nothing has been yield^ed yet to
                    // send a value to. 13.9: a resume takes one where R is
                    // there and none where it is empty -- a body no var^ of
                    // which receives a yield^ has nothing being sent in.
                    // 16.3's built-in walk is one of those; nothing sends it
                    // anything either.
                    if (native->kind == LHAT_NATIVE_START && b != 0) {
                        return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME) {
                        const LhatProto *from =
                            co->source == LHAT_COROUTINE_BODY &&
                                    co->closure != NULL
                                ? co->closure->proto
                                : NULL;
                        // 16.3's built-in walk receives nothing either, and
                        // has no proto to say so. 13.8改: a tuple R takes
                        // that many arguments -- but only a checked body has
                        // reserved the run's slots for the yield^'s binding,
                        // so an unknown proto keeps the old ceiling of one:
                        // a run laid where nothing reserved it would write
                        // over the frame's own locals.
                        bool known = from == NULL || from->yield_receives_known;
                        uint8_t wants =
                            from != NULL ? from->yield_receive_count : 0;
                        if (known ? b != wants : b > 1) {
                            return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        // And the run still has to fit the suspended frame
                        // (5.1's stop, never a scribble past it).
                        if (b > 1 && co->state == LHAT_COROUTINE_SUSPENDED &&
                            (size_t)co->sent_into + b >= co->register_count) {
                            return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                    }

                    // 15.2: start and resume split the two jobs, so each has
                    // to be called on the state that makes it meaningful.
                    if (native->kind == LHAT_NATIVE_START &&
                        co->state != LHAT_COROUTINE_FRESH) {
                        return vm_finish(m, chunk, LHAT_RUN_COROUTINE_ALREADY_STARTED,
                                      lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME &&
                        co->state == LHAT_COROUTINE_FRESH) {
                        return vm_finish(m, chunk, LHAT_RUN_COROUTINE_NOT_STARTED,
                                      lhat_nil(), at);
                    }

                    // 02 の 10.7: disposal runs what is still pending and
                    // never runs the same cleanup twice, so a coroutine that
                    // has finished simply has nothing left to do.
                    bool dispose = native->kind == LHAT_NATIVE_DISPOSE;
                    if (co->state == LHAT_COROUTINE_DONE ||
                        (dispose && co->state == LHAT_COROUTINE_FRESH)) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            // 05 の 8.8: a host walk's state goes back with
                            // the disposal. Nothing on any other source.
                            lhat_coroutine_release((LhatObject *)co, m);
                            SET_R(a, lhat_nil());
                            break;
                        }
                        return vm_finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }
                    if (co->state == LHAT_COROUTINE_RUNNING) {
                        return vm_finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }

                    // 16.3: a walk of a table has no body, so nothing below
                    // applies to it -- no frame to enter and nothing pending
                    // to drain. The guards above leave start() on a fresh
                    // walk and resume() on a suspended one, and one step is
                    // the whole of either.
                    if (co->source == LHAT_COROUTINE_TABLE) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        unsigned room = lhat_call_prepared(cc);
                        // 16.3改2: a projection yields one value, so one slot
                        // is what it wants and a run has nothing to put in
                        // the second position.
                        if (co->part != LHAT_WALK_PAIR) {
                            if (room > 1) {
                                return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                              lhat_nil(), at);
                            }
                            vm_step_table_walk(m, co, WALK_AS_VALUE, rbase + a,
                                            frame);
                            break;
                        }
                        // 16.3 with 13.8改: hand-driven start()/resume()
                        // answer the pair as the tuple every walk yields.
                        // The call's reservation says whether the run has
                        // anywhere to land; a checked call reserved the
                        // width (the stamp on a discarded one included),
                        // and a mismatch is refused the way 03 の 5.3
                        // refuses any tuple answer.
                        if (room > 1 && room != 3) {
                            return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        if (room == 3) {
                            if (rbase + a + 2 >= m->slot_capacity) {
                                return vm_finish(m, chunk,
                                              LHAT_RUN_STACK_OVERFLOW,
                                              lhat_nil(), at);
                            }
                            vm_step_table_walk(m, co, WALK_AS_RUN, rbase + a,
                                            frame);
                            break;
                        }
                        // One slot reserved: the step still advances, and
                        // the walk ending still answers nil^; a pair coming
                        // back has nowhere to land.
                        LhatValue key, value;
                        if (!lhat_table_walk(co, &key, &value)) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        co->state = LHAT_COROUTINE_SUSPENDED;
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_UNEXPECTED,
                                      lhat_nil(), at);
                    }

                    // 05 の 8.8: a host's walk is the same shape -- no body,
                    // no frame, one step per call. The step itself says what
                    // shape it yields, so the reservation is judged inside
                    // rather than up front the way a table's part allows.
                    if (co->source == LHAT_COROUTINE_HOST) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            lhat_coroutine_release((LhatObject *)co, m);
                            SET_R(a, lhat_nil());
                            break;
                        }
                        unsigned room = lhat_call_prepared(cc);
                        if (room > 1 &&
                            rbase + a + room - 1 >= m->slot_capacity) {
                            return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                          lhat_nil(), at);
                        }
                        LhatRunStatus fault = LHAT_RUN_OK;
                        // 13.8改: everything the resume sent, not the
                        // first of it. The body path below gathers the
                        // same way; a walk is no less entitled to it.
                        LhatValue walk_sent[LHAT_MAX_TUPLE];
                        size_t walk_sent_count = 0;
                        for (size_t i = 0;
                             i < (size_t)b && i < LHAT_MAX_TUPLE; i++) {
                            walk_sent[walk_sent_count++] = R(first + i);
                        }
                        vm_step_host_walk(m, co,
                                       room > 1 ? WALK_AS_RUN : WALK_AS_VALUE,
                                       rbase + a,
                                       room > 1 ? (size_t)room - 1
                                                : (size_t)room,
                                       frame, walk_sent, walk_sent_count,
                                       &fault);
                        if (fault != LHAT_RUN_OK) {
                            return vm_finish(m, chunk, fault, lhat_nil(), at);
                        }
                        break;
                    }

                    if (m->frame_count >= m->frame_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }

                    size_t next_base = rbase + (a) + 1;
                    // 4.3改: a suspension's saved registers are its body's
                    // width, which is the window the restore lays down.
                    if (next_base + co->register_count >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }
                    frame->pc = pc;

                    if (dispose) {
                        // 10.7: what is pending runs, innermost first, and
                        // then the coroutine is finished.
                        vm_enter_disposal_frame(m, co, next_base, a, &frame,
                                                                  &rbase, &chunk, &pc);
                        // 15.4: the guards above leave only a suspended
                        // coroutine here, so the slot a yield^ answers into
                        // is written the way a resume writes it -- with
                        // nil^, since dispose sends nothing in.
                        SET_R(co->sent_into, sent);
                        goto drain;
                    }

                    // 5.11: one frame, put back where it left off. 13.8改:
                    // `prepared` carries what the call site reserved, so the
                    // callee can tell whether a run is wanted here -- and
                    // the resume's arguments go over as they were written,
                    // however many the R takes.
                    LhatValue sent_run[LHAT_MAX_TUPLE];
                    size_t sent_count = 0;
                    for (size_t i = 0; i < (size_t)b && i < LHAT_MAX_TUPLE;
                         i++) {
                        sent_run[sent_count++] = R(first + i);
                    }
                    // 05 の 8.9: a host value argument goes over whole, as
                    // the pointer form, and rides alone -- a run's positions
                    // are single slots, so it mixes with nothing.
                    if (sent_count == 1 && lhat_is_hostvalue(sent_run[0])) {
                        sent_run[0] =
                            vm_stash_sent_hostvalue(m, rbase + first);
                        const LhatHostValueTag *sent_tag =
                            sent_run[0].as.hostvalue_run[0].hostvalue;
                        if ((size_t)co->sent_into + sent_tag->width >
                            co->register_count) {
                            return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                    } else {
                        for (size_t i = 0; i < sent_count; i++) {
                            if (lhat_is_hostvalue(sent_run[i])) {
                                return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                        }
                    }
                    frame = vm_enter_resume_frame(
                        m, co, next_base, a, (uint8_t)lhat_call_prepared(cc),
                        sent_run, sent_count);
                    rbase = frame->base;
                    chunk = &co->closure->proto->chunk;
                    pc = frame->pc;
                    break;
                }

                if (!lhat_is_object_kind(R(a), LHAT_OBJECT_SUBROUTINE)) {
                    return vm_finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                const LhatClosure *callee =
                    (const LhatClosure *)lhat_as_object(R(a));
                if (callee->proto == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }

                // 14.4: the receiver sits between the callee and the
                // arguments, and whether it is passed depends on the callee.
                // A member that takes no self^ is a static one (14.4), so the
                // frame simply starts after the receiver.
                size_t given = b;
                size_t skip = 1;
                if (as_method) {
                    if (callee->proto->takes_self) {
                        given = (size_t)b + 1;
                    } else {
                        skip = 2;
                    }
                }
                // 13.7: 'expr...' put a table in the last slot instead of an
                // ordinary argument; its positions are unpacked in place of
                // it, which is why what a call owes is read through
                // call_arg() from here on rather than off registers directly.
                const LhatTable *spread_table = NULL;
                size_t spread_run = SIZE_MAX;
                size_t before_spread = given;
                if ((cc & LHAT_CALL_SPREAD) != 0) {  // 13.8改, as above
                    if (given == 0) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    LhatValue spread_from = R(a + skip + given - 1);
                    before_spread = given - 1;
                    // 13.8改: a tuple spreads as the run it already is -- the
                    // positions are the slots past the head, so there is
                    // nothing to unpack and nothing to allocate.
                    if (lhat_is_run(spread_from)) {
                        spread_run = rbase + a + skip + given - 1;
                        given = before_spread + lhat_run_width(spread_from);
                    } else {
                        spread_table = vm_table_of(spread_from);
                        if (spread_table == NULL) {
                            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        given = before_spread + spread_table->array_count;
                    }
                }
                // 13.7: the last slot of a variadic callee collects the rest
                // into a table rather than taking one argument for itself, so
                // a call owes at least the fixed count and not exactly the
                // slot count.
                size_t declared_slots = callee->proto->parameters;
                size_t required = callee->proto->has_variadic
                                      ? declared_slots - 1
                                      : declared_slots;
                // 13.4: nothing fills a missing argument in. A parameter's
                // default belongs to the editor that writes the call, so the
                // count owed here is the declared one either way.
                if (callee->proto->has_variadic ? given < required
                                                : given != declared_slots) {
                    return vm_finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                }
                // Built before either path below places it, since both read
                // the same arguments the same way.
                LhatValue collected_variadic = lhat_nil();
                if (callee->proto->has_variadic) {
                    LhatTable *collected = lhat_table_new(&m->objects);
                    if (collected == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    for (size_t i = required; i < given; i++) {
                        bool refused = false;
                        LhatValue value = call_arg(m->slots, rbase, a, skip,
                                                   spread_table, spread_run,
                                                   before_spread,
                                                   i);
                        // 05 の 8.9: the collection is a table, and a table
                        // member is never a host value -- the checker said
                        // so at the call; this is the unchecked backstop.
                        if (lhat_is_hostvalue(value)) {
                            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        if (!vm_set_key(m, collected,
                                     lhat_integer((int64_t)(i - required + 1)),
                                     value, &refused)) {
                            return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                    }
                    collected_variadic = lhat_object((LhatObject *)collected);
                }

                // 02 の 15.5: calling a yieldable procedure does not suspend
                // the caller. It answers a coroutine, and the body has not
                // started -- which is why the colouring of async/await never
                // arises here.
                if (callee->proto->yields) {
                    LhatCoroutine *co =
                        lhat_coroutine_new(&m->objects, callee,
                                           callee->proto->chunk.registers);
                    if (co == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    // 05 の 8.9: without a spread the arguments sit in their
                    // laid-out slots, widths included, so the copy is
                    // slot-blind -- what lets a wide parameter cross. A
                    // spread re-indexes by value, and never mixes with a
                    // wide argument (the compiler refused that pair, and
                    // wide with variadic likewise).
                    if (spread_table == NULL && spread_run == SIZE_MAX &&
                        !callee->proto->has_variadic) {
                        for (size_t i = 0; i < callee->proto->parameter_slots;
                             i++) {
                            lhat_slots_set(
                                co->registers, i,
                                lhat_slots_get(m->slots,
                                               rbase + a + skip + i));
                        }
                    } else {
                        size_t fixed =
                            callee->proto->has_variadic ? required : given;
                        for (size_t i = 0; i < fixed; i++) {
                            lhat_slots_set(co->registers, i,
                                           call_arg(m->slots, rbase, a, skip,
                                                    spread_table, spread_run,
                                                    before_spread, i));
                        }
                    }
                    if (callee->proto->has_variadic) {
                        lhat_slots_set(co->registers, required, collected_variadic);
                    }
                    SET_R(a, lhat_object((LhatObject *)co));
                    break;
                }

                // 5.3: a tail call runs in this frame rather than one above
                // it, where the frame is free to go. It is not free while a
                // cleanup is pending (5.5 runs those after the call, and a
                // frame that has left cannot run them), nor when it is a
                // coroutine's (5.11 -- the frame is that coroutine's body),
                // nor at the top level of a session (03 の 4.3 keeps those
                // slots for the next input).
                bool reuse = tail && frame->cleanup_count == 0 &&
                             frame->coroutine == NULL &&
                             frame->closure->proto->kept == 0;
                if (!reuse && m->frame_count >= m->frame_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 5.3: the arguments already sit just above the callee, so
                // the new frame starts there and needs no shuffling -- unless
                // a spread broke the contiguity, or 13.7's collector has to
                // overwrite the slot after the fixed ones with the table just
                // built.
                size_t next_base = rbase + a + skip;
                // 03 の 4.3改: room for THIS callee's window, not for the
                // widest a compile could make. The two were the same
                // question while every machine had 8192 slots to spend; a
                // machine measured by its caller may have a few hundred,
                // and reserving 250 of them per call would leave it one
                // frame deep. What the callee's own calls need is asked
                // again here when it makes them.
                if (next_base + callee->proto->chunk.registers >=
                    m->slot_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                if (spread_table != NULL) {
                    size_t fixed = callee->proto->has_variadic ? required
                                                               : given;
                    for (size_t i = 0; i < fixed; i++) {
                        lhat_slots_set(m->slots, next_base + i,
                                       call_arg(m->slots, rbase, a, skip,
                                                spread_table, spread_run,
                                                before_spread, i));
                    }
                }
                if (callee->proto->has_variadic) {
                    lhat_slots_set(m->slots, next_base + (required), collected_variadic);
                }
                vm_clear_scratch(m, next_base, callee->proto);

                // 5.3: the window the callee is about to run in has been laid
                // out above this frame; taking the frame over is moving it
                // down onto this frame's own registers. Nothing above is read
                // again, and next_base is always higher than base (a callee
                // sits at least one slot above the frame it was called from),
                // so the move is forward and needs no care about overlap.
                if (reuse) {
                    // 5.4: what still points into these registers takes its
                    // value with it first. Closing leaves the slots as they
                    // are, so the arguments being moved down are still there
                    // to read.
                    vm_close_upvalues(m, frame->base);
                    size_t window = callee->proto->chunk.registers;
                    for (size_t i = 0; i < window; i++) {
                        // 05 の 8.9: a host value's continuation slots are raw
                        // bytes, so the payload and the tag travel exactly as
                        // they lie rather than through a value read.
                        m->slots.values[frame->base + i] =
                            m->slots.values[next_base + i];
                        m->slots.tags[frame->base + i] =
                            m->slots.tags[next_base + i];
                    }
                    frame->closure = callee;
                    frame->pc = 0;
                    frame->returning = false;
                    // 5.3: `base`, `result` and `prepared` are the original
                    // caller's and stay its own -- the answer still goes where
                    // that call site reserved room for it. 11.9's `derive`
                    // stays for the same reason: what this frame answers is
                    // still read the way the expression that made it asks.
                    // The drop is sticky: a body whose answer was already
                    // being thrown away throws away whatever it goes on to
                    // tail-call for.
                    if ((cc & LHAT_CALL_DROP) != 0) {
                        frame->drop_answer = true;
                    }
                    chunk = &callee->proto->chunk;
                    pc = 0;
                    break;
                }

                frame->pc = pc;

                Frame *called = vm_push_frame(m, callee, next_base, a, (uint8_t)lhat_call_prepared(cc));

                frame = called;
                rbase = frame->base;
                chunk = &callee->proto->chunk;
                pc = 0;
                break;
            }

            case LHAT_BC_PUSHCLEANUP:
                if (frame->cleanup_count >= LHAT_MAX_CLEANUPS) {
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                frame->cleanups[frame->cleanup_count++] = lhat_bx(instruction);
                break;

            // 10.2 and 12.3: leaving the block runs what it entered. The two
            // cases differ only in where control goes afterwards.
            case LHAT_BC_POPCLEANUP:
                frame->drain_target = a;
                frame->resume = pc;
                frame->returning = false;
                goto drain;

            case LHAT_BC_ENDCLEANUP:
                goto drain;

            // 10.4: leaving the procedure leaves every block inside it, so
            // the drain runs everything still pending before the frame goes.
            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL:
                frame->drain_target = 0;
                frame->returning = true;
                frame->answer = op == LHAT_BC_RETURN ? R(a) : lhat_nil();
                // 05 の 8.9: a host value answer moves into the frame's own
                // room before the drain -- the callee's window overlaps the
                // caller's scratch, so no register survives the cleanups.
                // The pop places it whole from there. A coroutine's own
                // return crossing a suspension was refused by the checker;
                // this is the backstop.
                if (op == LHAT_BC_RETURN && lhat_is_hostvalue(R(a))) {
                    // 8.9改2: the base is no exception any more -- the drain
                    // hands the value to the host whole, off this same room.
                    const LhatHostValueTag *tag = lhat_as_hostvalue_tag(R(a));
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = m->slots.values[rbase + a + i];
                    }
                    frame->answer.as.hostvalue_run = frame->answer_run;
                }
                // 02 の 13.8改: a tuple answer moves into the same room, for
                // the same reason -- no register survives the drain. The
                // positions are values rather than raw bytes, so their tags
                // travel beside them, and the collector reads them there
                // (gc.c's mark_roots) while the cleanups run.
                if (op == LHAT_BC_RETURN && b != 0) {
                    if ((size_t)b > LHAT_MAX_TUPLE) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < (size_t)b; i++) {
                        frame->answer_run[i + 1] =
                            m->slots.values[rbase + a + i];
                        frame->answer_tags[i + 1] =
                            m->slots.tags[rbase + a + i];
                    }
                    frame->answer = lhat_run_head((size_t)b);
                }
                goto drain;

            // 04 の 11.6: unlike a fault the machine itself raises, the
            // value is the program's own -- carried through to the host
            // exactly as lhat_run always has, rather than discarded like
            // every other vm_finish() here discards its nil^.
            case LHAT_BC_PANIC:
                return vm_finish(m, chunk, LHAT_RUN_PANIC, R(a), at);

            // 11.6: 14.12's own runtime check (vm_fits_call already
            // trusts it for overload^ resolution), just asked once instead
            // of per candidate. Compile-time disjointness (check.c) already
            // ruled out what could never hold; this is what it could not
            // rule out, checked against the actual value.
            // 02 の 11.6改3: where the value fits, it stays and only the
            // type the checker tracks narrowed. Where it does not, the
            // answer is the other arm of what as^ was said to be -- a
            // localerror^.CastFailure, which 04 の 2.7改 will not let the
            // frame return, so a writer meets it here or nowhere.
            //
            // 03 の 4.2: this runs whether or not anything was checked. A
            // relaxed build makes the same value the strict one promised.
            case LHAT_BC_ASCAST: {
                const LhatRuntimeType *wanted =
                    (const LhatRuntimeType *)lhat_as_object(R(b));
                if (!lhat_value_satisfies(R(a), wanted)) {
                    const LhatErrorKind *kind = lhat_registry_cast_failure();
                    LhatError *failure =
                        kind != NULL ? lhat_error_new(&m->objects, kind) : NULL;
                    if (failure == NULL) {
                        return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)failure));
                }
                break;
            }

            // 02 の 15.4: the frame stops here and the value goes out. 5.11
            // keeps the one frame rather than a stack, which 15.5 is what
            // makes possible -- a yield^ is always in the body it suspends.
            case LHAT_BC_YIELD: {
                LhatCoroutine *co = frame->coroutine;
                // 10.7: nothing is waiting for a yield^ during disposal.
                if (co == NULL || frame->disposing) {
                    return vm_finish(m, chunk, LHAT_RUN_YIELD_OUTSIDE, lhat_nil(), at);
                }
                LhatValue value = R(a);
                // 02 の 13.8改: a tuple goes out whole. The positions ride
                // the frame's own room the way a return^'s do -- the window
                // is about to be copied into the coroutine and then left
                // behind, so no register survives to be read from the
                // resumer's side. What the resume sends comes back into the
                // first position's slot (co->sent_into below) whatever its
                // own width is -- one value, a host value laid out whole, or
                // 13.8改's several as a run head with the positions after.
                //
                // b == 0 with a run head in the slot is 15.8's delegation
                // loop forwarding what it was handed: the positions are
                // already in this frame's answer room, put there by the
                // RESUME just above -- by WALK_AS_ANSWER for a table's walk,
                // or by the inner body's own yield collating into it -- so
                // there is nothing to fill and the head goes out as it
                // stands.
                // 05 の 8.9: b == LHAT_YIELD_HOSTVALUE says R[A..] is a host
                // value laid out whole. It rides the frame's own room the
                // way a return's does -- the window is about to be left
                // behind -- and goes out as the pointer form the placement
                // reads.
                if (b == LHAT_YIELD_HOSTVALUE) {
                    if (!lhat_is_hostvalue(value)) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    const LhatHostValueTag *tag = lhat_as_hostvalue_tag(value);
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = m->slots.values[rbase + a + i];
                    }
                    value.as.hostvalue_run = frame->answer_run;
                } else if (b != 0) {
                    if ((size_t)b > LHAT_MAX_TUPLE) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < (size_t)b; i++) {
                        frame->answer_run[i + 1] =
                            m->slots.values[rbase + a + i];
                        frame->answer_tags[i + 1] =
                            m->slots.tags[rbase + a + i];
                    }
                    value = lhat_run_head((size_t)b);
                } else if (lhat_is_hostvalue(value)) {
                    // b == 0 forwards what a delegation was handed. A host
                    // value arrives re-aimed at this frame's own room (see
                    // the hand-back below), so anything aimed anywhere else
                    // was never one of ours -- refused before the tag is
                    // ever read through it.
                    if (value.as.hostvalue_run != frame->answer_run) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                }

                for (size_t i = 0; i < co->register_count; i++) {
                    lhat_slots_set(co->registers, i, R(i));
                    // 5.12: the coroutine has just taken a whole frame's
                    // worth of the stack into itself. The backward barrier,
                    // as for a table: one more visit to the coroutine costs
                    // less than marking every register as it is copied.
                    lhat_gc_barrier_back(m, (LhatObject *)co, R(i));
                }
                // 15.4 with 5.4: the captures of this frame's slots travel
                // with the registers, or a resume at another depth would
                // leave them reading whatever frame took these addresses
                // over. This frame is the top one, so its captures are the
                // head of the machine's descending open list; popped highest
                // first and prepended, the coroutine's own list comes out
                // ascending, which is what reattach_upvalues expects.
                const LhatValueUnion *suspending = m->slots.values + rbase;
                while (m->open != NULL &&
                       m->open->location.value >= suspending) {
                    LhatUpvalue *up = m->open;
                    m->open = up->next_open;
                    size_t offset = (size_t)(up->location.value - suspending);
                    up->location = lhat_slots_ref(co->registers, offset);
                    up->suspended_in = co;
                    up->next_open = co->open;
                    co->open = up;
                    // 5.12: the vm_capture now keeps the coroutine alive
                    // (gc.c), which a black upvalue has to declare.
                    lhat_gc_barrier(m, (LhatObject *)up,
                                    lhat_object((LhatObject *)co));
                }
                co->pc = pc;
                co->sent_into = a;
                co->state = LHAT_COROUTINE_SUSPENDED;
                if (frame->cleanup_count > LHAT_COROUTINE_CLEANUPS) {
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                co->cleanup_count = frame->cleanup_count;
                for (size_t i = 0; i < frame->cleanup_count; i++) {
                    co->cleanups[i] = frame->cleanups[i];
                }
                if (co->cleanup_count > 0) {
                    m->cleanup_carriers++;  // 10.7: a suspended carrier
                }

                uint8_t into = frame->result;
                // 13.8改: read before `frame` becomes the resumer below. The
                // room outlives the pop -- the entry is still there, only
                // uncounted.
                uint8_t reserved = frame->prepared;
                const LhatValueUnion *out_run = frame->answer_run;
                const uint8_t *out_tags = frame->answer_tags;
                // 05 の 8.8: no resumer below -- the host resumed this
                // coroutine at the base of this run (lhat_machine_resume),
                // so the yield leaves vm_run_frames the way a return does, the
                // coroutine staying suspended for the next resume. The
                // answer crosses the boundary the way a return's does: one
                // value, or the positions copied into the machine's room.
                if (m->frame_count == base_depth + 1) {
                    m->frame_count--;
                    // 05 の 8.9改2: the host receives the value whole, as
                    // the pointer form aimed at the machine's scratch.
                    if (lhat_is_hostvalue(value)) {
                        return vm_finish(m, chunk, LHAT_RUN_OK,
                                      vm_hand_hostvalue_out(m, value), at);
                    }
                    if (lhat_is_run(value)) {
                        size_t positions = lhat_run_width(value);
                        if (positions > LHAT_MAX_TUPLE) {
                            return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        for (size_t i = 0; i < positions; i++) {
                            LhatValue held;
                            held.as = out_run[i + 1];
                            held.tag = (LhatValueTag)out_tags[i + 1];
                            m->tuple_scratch[i] = held;
                        }
                        m->tuple_scratch_count = positions;
                        value =
                            positions > 0 ? m->tuple_scratch[0] : lhat_nil();
                    }
                    return vm_finish(m, chunk, LHAT_RUN_OK, value, at);
                }
                m->frame_count--;
                frame = &m->frames[m->frame_count - 1];
                rbase = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                if (lhat_is_hostvalue(value) && reserved == 0) {
                    // 15.8: the delegation loop forwards the value. The
                    // bytes move into the resumer's own room -- the popped
                    // frame's entry would be taken over by the next push --
                    // and the head is re-aimed there, keeping the invariant
                    // the YIELD above checks.
                    const LhatValueUnion *from = value.as.hostvalue_run;
                    const LhatHostValueTag *tag = from[0].hostvalue;
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = from[i];
                    }
                    value.as.hostvalue_run = frame->answer_run;
                    lhat_slots_set(m->slots, rbase + into, value);
                } else if (lhat_is_hostvalue(value)) {
                    // 05 の 8.9: written out whole at the resume's slot, as
                    // a call's answer is at its own -- the call said the
                    // width it made room for (compile's prepared).
                    if ((size_t)reserved <
                            value.as.hostvalue_run[0].hostvalue->width ||
                        !vm_place_hostvalue_answer(m, rbase + into, value)) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                } else if (lhat_is_run(value) && reserved == 0) {
                    // 15.8: the resumer is a delegation loop, which reserved
                    // one slot because it could not know this width (see
                    // RESUME). The head goes in that slot and the positions
                    // into the resumer's own answer room -- which is exactly
                    // where the YIELD it runs next reads a run's positions
                    // from, so the run is forwarded outwards untouched, and
                    // a chain of delegations hands it along one room at a
                    // time.
                    //
                    // The positions are safe across the collections the
                    // instructions between here and that YIELD may take: the
                    // loop holds the coroutine in a register for as long as
                    // it drives it, the yield above has just copied its
                    // registers into it, and everything yielded came from
                    // there. Reachability is through the coroutine, not
                    // through this room -- gc.c only walks the room for a
                    // frame whose answer is a run, which is a return in
                    // progress and not this.
                    size_t positions = lhat_run_width(value);
                    for (size_t i = 0; i < positions; i++) {
                        frame->answer_run[i + 1] = out_run[i + 1];
                        frame->answer_tags[i + 1] = out_tags[i + 1];
                    }
                    lhat_slots_set(m->slots, rbase + into, value);
                } else if (lhat_is_run(value)) {
                    // 13.8改: laid out as a head slot naming the width plus
                    // the positions, exactly as a returned tuple is. 13.9's
                    // 'union(Y, T)' is what the resumer holds, and the head's
                    // tag is what tells the two apart there. A narrower run
                    // (the short side of a widened fold) lands as it is --
                    // CHECKRUN pads it where the binding takes it apart.
                    size_t positions = lhat_run_width(value);
                    if ((size_t)reserved < positions + 1 ||
                        rbase + into + positions >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    lhat_slots_set(m->slots, rbase + into, value);
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held;
                        held.as = out_run[i + 1];
                        held.tag = (LhatValueTag)out_tags[i + 1];
                        lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                    }
                } else if (reserved > 1 &&
                           lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
                    // 16.3 with 13.8改: a loop reserved a run and the body
                    // yielded a table -- the iterator that answers pairs as
                    // tables rather than tuples. Its positions go into the
                    // reserved slots -- indexed here rather than by the loop --
                    // so the two iterator shapes meet the same binds. A missing position is 04 の 11.3's
                    // absence, which a destructuring faults on.
                    const LhatTable *yielded =
                        (const LhatTable *)lhat_as_object(value);
                    size_t positions = (size_t)reserved - 1;
                    if (rbase + into + positions >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held = lhat_table_get(
                            yielded, lhat_integer((int64_t)i + 1));
                        if (lhat_is_nil(held)) {
                            return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                    }
                    lhat_slots_set(m->slots, rbase + into,
                                   lhat_run_head(positions));
                } else {
                    SET_R(into, value);
                }
                break;
            }

            // 02 の 15.8: the delegation loop asks whether the inner one is
            // finished, since 13.9 makes what a resume answers the union of
            // its yield type and its return type.
            case LHAT_BC_ISDONE: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatCoroutine *co =
                    (const LhatCoroutine *)lhat_as_object(R(b));
                SET_R(a, lhat_bool(co->state == LHAT_COROUTINE_DONE));
                break;
            }

            case LHAT_BC_RESUME: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatCoroutine *co =
                    (LhatCoroutine *)lhat_as_object(R(b));
                if (co->state == LHAT_COROUTINE_DONE ||
                    co->state == LHAT_COROUTINE_RUNNING) {
                    return vm_finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                }

                // 16.3 with 13.8改: which shape one step puts down. C is the
                // loop's word -- the count of names is syntax, so unchecked
                // and checked compiles say the same thing (03 の 4.2).
                // 0 (and 1) is 15.8's delegation loop, the one emitter of a
                // RESUME with no width: the pair rides the answer room and
                // the YIELD after it forwards the run as it stands.
                WalkMode mode = (cc & LHAT_RESUME_WIDE) != 0 ? WALK_AS_VALUE
                                : cc >= 3                    ? WALK_AS_RUN
                                : cc == 2                    ? WALK_AS_VALUE
                                                             : WALK_AS_ANSWER;

                // 16.3: a table's walk has no body to enter, so resuming it
                // is one step and nothing more.
                if (co->source == LHAT_COROUTINE_TABLE) {
                    // A run is two positions; a loop that reserved another
                    // width asked for something the walk does not yield.
                    // 16.3改2: and a projection yields no pair at all, so a
                    // loop written with two names is asking the same way.
                    if (mode == WALK_AS_RUN &&
                        (co->part != LHAT_WALK_PAIR || (size_t)cc != 3 ||
                         rbase + a + 2 >= m->slot_capacity)) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    vm_step_table_walk(m, co, mode, rbase + a, frame);
                    break;
                }
                // 05 の 8.8: and so is a host's -- one C call. The loops
                // send nothing in; the delegation loop forwards R(a), which
                // is what a BODY's resume writes into sent_into below.
                if (co->source == LHAT_COROUTINE_HOST) {
                    if (mode == WALK_AS_RUN &&
                        rbase + a + (size_t)cc - 1 >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                      lhat_nil(), at);
                    }
                    LhatRunStatus fault = LHAT_RUN_OK;
                    // 15.8 with 13.8改: what the outer resume sent may
                    // itself be a run, laid at R(a) head-first by this
                    // frame's own suspension -- the body path below reads
                    // it the same way. A for^ loop sends nothing.
                    LhatValue walk_sent[LHAT_MAX_TUPLE];
                    size_t walk_sent_count = 0;
                    if (mode == WALK_AS_ANSWER) {
                        if (lhat_is_run(R(a))) {
                            size_t width = lhat_run_width(R(a));
                            if (width > LHAT_MAX_TUPLE ||
                                rbase + a + width >= m->slot_capacity) {
                                return vm_finish(m, chunk,
                                              LHAT_RUN_TUPLE_ARITY,
                                              lhat_nil(), at);
                            }
                            for (size_t i = 0; i < width; i++) {
                                walk_sent[walk_sent_count++] = R(a + 1 + i);
                            }
                        } else {
                            walk_sent[walk_sent_count++] = R(a);
                        }
                    }
                    vm_step_host_walk(m, co, mode, rbase + a,
                                   mode == WALK_AS_RUN
                                       ? (size_t)cc - 1
                                       : (cc & LHAT_RESUME_WIDE) != 0
                                             ? (size_t)(cc & ~LHAT_RESUME_WIDE)
                                             : 0,
                                   frame, walk_sent, walk_sent_count,
                                   &fault);
                    if (fault != LHAT_RUN_OK) {
                        return vm_finish(m, chunk, fault, lhat_nil(), at);
                    }
                    break;
                }
                if (m->frame_count >= m->frame_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                // 13.8改: the frame goes above the slots the loop reserved
                // for the answer, or the callee's window would overlap the
                // run about to be written back into this one.
                size_t next_base =
                    rbase + (a) +
                    ((cc & LHAT_RESUME_WIDE) != 0
                         ? (size_t)(cc & ~LHAT_RESUME_WIDE)
                         : cc >= 3 ? (size_t)cc : 1);
                if (next_base + co->register_count >= m->slot_capacity) {  // 4.3改
                    return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 15.8 with 13.8改: the delegation loop forwards what the
                // outer resume sent, which may itself be a run -- laid at
                // R(a) by this frame's own suspension, head plus positions.
                // A for^ loop sends nil^, which is never one.
                LhatValue sent_run[LHAT_MAX_TUPLE];
                size_t sent_count = 1;
                sent_run[0] = R(a);
                if (lhat_is_hostvalue(R(a))) {
                    // 05 の 8.9: the outer resume laid the value out whole
                    // at this slot; it goes on as the pointer form, its
                    // bytes stashed clear of the restore.
                    sent_run[0] = vm_stash_sent_hostvalue(m, rbase + a);
                    const LhatHostValueTag *sent_tag =
                        sent_run[0].as.hostvalue_run[0].hostvalue;
                    if (co->state == LHAT_COROUTINE_SUSPENDED &&
                        (size_t)co->sent_into + sent_tag->width >
                            co->register_count) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                } else if (lhat_is_run(R(a))) {
                    size_t width = lhat_run_width(R(a));
                    if (width > LHAT_MAX_TUPLE ||
                        rbase + a + width >= m->slot_capacity) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < width; i++) {
                        sent_run[i] = R(a + 1 + i);
                    }
                    sent_count = width;
                }
                // As at the natives' resume: the run has to fit the
                // suspended frame (5.1's stop for an unchecked proto).
                if (sent_count > 1 &&
                    co->state == LHAT_COROUTINE_SUSPENDED &&
                    (size_t)co->sent_into + sent_count >= co->register_count) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                frame->pc = pc;
                // 13.8改: what the resume reserved for the answer -- a loop
                // driving a tuple-yielding body says its width here, and the
                // yield's placement reads it (15.4).
                //
                // 15.8: zero is the delegation loop, and means "whatever
                // width comes, forward it". It cannot say a width: unlike a
                // for^, whose count of names is syntax (03 の 4.2), a
                // await^ would have to read the inner body's type to know
                // one, and an unchecked compile has no type to read. So the
                // width is settled where it is known -- at the yield -- and
                // the run travels through this frame's answer room rather
                // than through slots it never reserved.
                frame = vm_enter_resume_frame(
                    m, co, next_base, a,
                    (cc & LHAT_RESUME_WIDE) != 0
                        ? (uint8_t)(cc & ~LHAT_RESUME_WIDE)
                        : cc >= 3 ? cc : (cc == 0 ? 0 : 1),
                    sent_run, sent_count);
                rbase = frame->base;
                chunk = &co->closure->proto->chunk;
                pc = frame->pc;
                break;
            }


            // 02 の 19 章: the declaration builds its objects where it
            // stands. NEWENUM reads the descriptor constant the compiler
            // made; NEWENUMERATOR evaluates nothing itself -- the value
            // arrived in R[C], put there by the member's own expression.
            case LHAT_BC_NEWENUM: {
                LHAT_GC_POLL();  // this case allocates
                LhatValue held = chunk->constants[lhat_bx(instruction)];
                if (!lhat_is_object_kind(held, LHAT_OBJECT_TYPE)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatRuntimeType *decl =
                    (const LhatRuntimeType *)lhat_as_object(held);
                LhatEnum *made =
                    lhat_enum_new(&m->objects, decl->enum_name, decl);
                if (made == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                SET_R(a, lhat_object((LhatObject *)made));
                break;
            }

            case LHAT_BC_NEWENUMERATOR: {
                LHAT_GC_POLL();  // this case allocates
                if (!lhat_is_object_kind(R(a), LHAT_OBJECT_ENUM) ||
                    !lhat_is_object_kind(R(b), LHAT_OBJECT_STRING) ||
                    // 05 の 8.9: a host value cannot live in a heap object.
                    lhat_is_hostvalue(R(cc))) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatEnum *owner = (LhatEnum *)lhat_as_object(R(a));
                size_t index = lhat_table_count(owner->members) + 1;
                LhatEnumerator *e = lhat_enumerator_new(
                    &m->objects, owner,
                    (const LhatString *)lhat_as_object(R(b)), R(cc), index);
                if (e == NULL) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                bool refused = false;
                if (!vm_set_key(m, owner->members, R(b),
                             lhat_object((LhatObject *)e), &refused)) {
                    return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                break;
            }

            // 03 の 5.1改3: the counted loop fused. The three registers are
            // asked to be numbers here, once -- 16.4 refuses reassigning the
            // focus and reads the bound and step^ before the loop, so nothing
            // can change their kind while it runs (an addition may widen the
            // focus to real, which both helpers speak).
            case LHAT_BC_FORPREP:
            case LHAT_BC_FORPREPD: {
                if (!lhat_is_number(R(a)) || !lhat_is_number(R(a + 1)) ||
                    !lhat_is_number(R(a + 2))) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                bool enter = false;
                LhatRunStatus status = LHAT_RUN_OK;
                ordering(op == LHAT_BC_FORPREP ? LHAT_BC_LE : LHAT_BC_GE,
                         R(a), R(a + 1), &enter, &status);
                if (!enter) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;
            }

            case LHAT_BC_FORLOOP:
            case LHAT_BC_FORLOOPD: {
                bool down = op == LHAT_BC_FORLOOPD;
                LhatValue moved = lhat_nil();
                LhatRunStatus status = LHAT_RUN_OK;
                if (!arithmetic(down ? LHAT_BC_SUB : LHAT_BC_ADD, R(a),
                                R(a + 2), &moved, &status)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                SET_R(a, moved);
                bool more = false;
                ordering(down ? LHAT_BC_GE : LHAT_BC_LE, moved, R(a + 1),
                         &more, &status);
                if (more) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                    LHAT_TURN_BACK();
                }
                break;
            }

            case LHAT_BC_COUNT:
                return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        continue;

    // 02 の 11.1: an operator is a function the left operand carries, named
    // by 11.8 after the operator itself. The instructions above take their
    // own types directly and come here for everything else, which is why the
    // built-in cases pay nothing for this.
    call_operator: {
        // 11.8改: NEG is the one instruction arriving here with a single
        // operand. Everything below reads 'cc' only where a right operand
        // exists, so the unary path leaves that register untouched -- NEG
        // never wrote one.
        bool unary = op == LHAT_BC_NEG;
        uint8_t given = unary ? 0 : 1;
        LHAT_GC_POLL();  // candidate lookups intern the operator's name
        // The right operand as a value: K[cc] when the ADDK family fell
        // through to here, R(cc) otherwise. A constant is never a host
        // value, so every branch below that wants a pointer aimed into the
        // stack (hostvalue_argument) already excludes it by asking the tag.
        LhatValue rhs = unary            ? lhat_nil()
                        : k_right        ? chunk->constants[cc]
                                         : R(cc);
        LhatValue found = lhat_nil();
        OperatorLookup answer = OPERATOR_ABSENT;
        for (;;) {
            size_t length = 0;
            const char *name = vm_operator_name(op, &length);
            // 14.4 makes an operator a method: the left operand is the
            // receiver and the right one the single argument.
            answer = vm_operator_candidate(m, R(b), name, length, R(b), rhs,
                                        given, false, &found);
            // 11.3改: the left carries nothing that takes this right
            // operand, so the right one is asked whether it was written as
            // the receiver instead. This is what lets a value join an
            // operation whose left operand is a built-in, which can carry no
            // answer for it.
            //
            // 11.8改: a unary operator has no other side to ask. Its one
            // operand is the receiver by the only reading there is.
            if (!unary &&
                (answer == OPERATOR_ABSENT || answer == OPERATOR_NO_CANDIDATE)) {
                LhatValue other = lhat_nil();
                OperatorLookup right = vm_operator_candidate(
                    m, rhs, name, length, rhs, R(b), given, true, &other);
                if (right == OPERATOR_PICKED || right == OPERATOR_NO_MEMORY) {
                    found = other;
                    answer = right;
                }
            }
            // 11.9改: an equality was looked for as '=' first. A type that
            // wrote none may still have said how it orders, and 11.9 has
            // that refine equality too -- so the other name is asked before
            // the default is taken.
            if (answer != OPERATOR_PICKED && op == LHAT_BC_EQ &&
                derive_from != LHAT_FRAME_NO_DERIVE) {
                op = LHAT_BC_SPACESHIP;
                continue;
            }
            break;
        }
        // 11.9改: which of the two answered, and so how what comes back is
        // read -- an op^= answers the bool^ itself, a '<=>' a number^ to put
        // beside zero.
        bool answered_bool = op == LHAT_BC_EQ;
        if (answer == OPERATOR_NO_MEMORY) {
            return vm_finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
        }
        // 11.9: equality is answered whether or not either was written --
        // 14.2 says what a table is the same as and 05 の 8.9 what a host
        // value is, and a type that says more only refines that. An ordering
        // has no such answer to fall back on and faults the way it always did.
        if (answer != OPERATOR_PICKED &&
            (derive_from == LHAT_BC_EQ || derive_from == LHAT_BC_NE)) {
            bool equal;
            if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(rhs)) {
                // 05 の 8.9: the bytes under the same tag. Reading the heads
                // alone would call two same-typed values equal whatever they
                // hold.
                equal = lhat_is_hostvalue(R(b)) &&
                        vm_hostvalue_equal(m->slots, rbase + b, rbase + cc);
            } else {
                equal = lhat_value_equal(R(b), rhs);
            }
            SET_R(a, lhat_bool(equal == (derive_from == LHAT_BC_EQ)));
            continue;  // the label sits in the loop, not in the switch
        }
        if (answer == OPERATOR_NO_CANDIDATE) {
            return vm_finish(m, chunk, LHAT_RUN_NO_CANDIDATE, lhat_nil(), at);
        }
        if (answer != OPERATOR_PICKED) {
            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        // 05 の 8.9: a host value's operator is C and answers on the spot --
        // no frame, no derive to carry: the receiver convention is 14.4's,
        // with each operand handed over as its head aimed into the stack.
        if (lhat_is_object_kind(found, LHAT_OBJECT_HOST)) {
            LhatHost *carried_host = (LhatHost *)lhat_as_object(found);
            LhatValue operands[2];
            operands[0] = lhat_is_hostvalue(R(b))
                              ? hostvalue_argument(m->slots, rbase + b)
                              : R(b);
            if (!unary) {
                operands[1] = lhat_is_hostvalue(rhs)
                                  ? hostvalue_argument(m->slots, rbase + cc)
                                  : rhs;
            }
            frame->pc = pc;  // 11.6改, as at a CALL
            size_t frames_before = m->frame_count;
            LhatValue answered = lhat_nil();
            if (!vm_call_host_fn(m, carried_host->call,
                              carried_host->context, operands,
                              unary ? 1 : 2, &answered)) {
                return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                              at);
            }
            LhatRunStatus left = LHAT_RUN_OK;
            LhatValue left_with = lhat_nil();
            if (vm_host_faulted(m, frames_before, &left, &left_with)) {
                return vm_finish(m, chunk, left, left_with, at);
            }
            if (derive_from != LHAT_FRAME_NO_DERIVE) {
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (answered_bool) {
                    // 11.9改: an op^= answers the judgement itself. The shape
                    // rule asks it for a bool^, so anything else is a body
                    // that did not keep to its signature.
                    if (!lhat_is_bool(answered)) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    held = lhat_as_bool(answered) ==
                           (derive_from == LHAT_BC_EQ);
                } else if (derive_from == LHAT_BC_EQ ||
                           derive_from == LHAT_BC_NE) {
                    // 11.9: what came back is read against zero.
                    held = lhat_value_equal(answered, lhat_integer(0)) ==
                           (derive_from == LHAT_BC_EQ);
                } else if (!ordering(derive_from, answered, lhat_integer(0),
                                     &held, &status)) {
                    return vm_finish(m, chunk, status, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(held));
                continue;
            }
            // 02 の 13.8改: an operator answers one value, whoever wrote it
            // -- 11.8's shape gives it no room for several, and the frame
            // pushed for an L^ one says so too (prepared = 1 below). A host
            // that answered a run has nowhere to put it here.
            if (lhat_is_run(answered)) {
                m->tuple_scratch_count = 0;
                return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
            }
            if (lhat_is_hostvalue(answered)) {
                if (!vm_place_hostvalue_answer(m, rbase + a, answered)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
                continue;
            }
            SET_R(a, answered);
            continue;
        }
        const LhatClosure *carried =
            (const LhatClosure *)lhat_as_object(found);
        // 15.7改: an operator may not be yieldable. Not because it is an f^ --
        // 15.3改 lets one of those suspend -- but because 11.8's signature
        // answers T, and a yieldable subroutine answers a coroutine (15.5).
        if (carried->proto == NULL || carried->proto->yields) {
            return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        if (m->frame_count >= m->frame_capacity) {
            return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }

        // 5.3 wants the arguments in a contiguous run, and b and cc need not
        // be one. 14.4 puts the left operand in self^, so it leads: the frame
        // is laid out just past where the answer goes, the way a native call
        // lays one out.
        //
        // 11.3改: and a self^-last one needs nothing else here. Its
        // parameter list is written in operand order too -- the left operand
        // first, the self^ after it -- so the same two slots hold the same two
        // values whichever side the receiver is. Only which slot the body
        // calls self^ differs, and that is the body's own business.
        size_t next_base = rbase + (a) + 1;
        if (next_base + carried->proto->chunk.registers >=
            m->slot_capacity) {  // 4.3改
            return vm_finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }
        // Both operands are read before either slot of the new window is
        // written: 03 の 5.1's forwarding reads an operand where it lies,
        // and a destination that is itself a local puts the window right on
        // top of one -- 's := s .. t' has t sitting at next_base.
        LhatValue left_operand = R(b);
        LhatValue right_operand = rhs;
        lhat_slots_set(m->slots, next_base + (0), left_operand);
        // 11.8改: a unary one declares self^ and nothing else, so the one
        // slot is the whole frame.
        if (!unary) {
            lhat_slots_set(m->slots, next_base + (1), right_operand);
        }
        vm_clear_scratch(m, next_base, carried->proto);

        frame->pc = pc;
        Frame *entered = &m->frames[m->frame_count++];
        entered->closure = carried;
        entered->pc = 0;
        entered->base = next_base;
        entered->result = a;
        entered->prepared = 1;  // 13.8改: an operator answers one value
        entered->cleanup_count = 0;
        entered->returning = false;
        entered->coroutine = NULL;
        entered->disposing = false;
        entered->drop_answer = false;  // 5.3
        // The room is a root while the frame lives (mark_roots), so it
        // starts empty rather than as whatever the slot held before.
        entered->answer = lhat_nil();
        // 11.9: an ordering that reached for '<=>' wants the answer
        // read against zero, not handed over as it is.
        entered->derive = derive_from;
        // 11.9改: unless what answered was an op^=, whose bool^ is the
        // judgement itself -- `derive` then says only whether to negate it.
        entered->derive_equal = answered_bool;

        frame = entered;
        rbase = frame->base;
        chunk = &carried->proto->chunk;
        pc = 0;
        continue;
    }

    drain:
        // Innermost first, one at a time: each body ends with ENDCLEANUP,
        // which comes back here for the next.
        if (frame->cleanup_count > frame->drain_target) {
            pc = frame->cleanups[--frame->cleanup_count];
            continue;
        }
        if (!frame->returning) {
            pc = frame->resume;
            continue;
        }

        // 5.4: whatever still points into this frame takes its value with it,
        // since the slots are about to be reused.
        {
            // 5.3: a tail call from a body that discards what it calls answers
            // for that body, which answers the nil^ of falling off its end.
            LhatValue value = frame->drop_answer ? lhat_nil() : frame->answer;
            // 5.11: the body is over, so the coroutine has nothing left to
            // resume and its cleanups have all run.
            if (frame->coroutine != NULL) {
                frame->coroutine->state = LHAT_COROUTINE_DONE;
                frame->coroutine->cleanup_count = 0;
            }

            // 03 の 4.3: a session's top-level slots outlive the input, so
            // what points into them goes on sharing them. Closing those here
            // would undo the CLOSE compile_session_statements deliberately
            // does not write, and a later input's ':=' through a closure
            // would land in a private copy instead of the slot the next
            // input reads. `kept` is zero everywhere else, which leaves an
            // ordinary frame closed whole the way it always was.
            const LhatProto *ran = frame->closure->proto;
            vm_close_upvalues(m, frame->base + (ran != NULL ? ran->kept : 0));

            // 02 の 10.7: a coroutine dropped in the last few instructions
            // never met a collection, and without one here whether its
            // finally^ runs comes down to whether the heap happened to fill
            // up in time. The frame is still counted, which is what makes
            // this safe -- its registers and its answer are still roots, so
            // this asks the same question every other collection asks and
            // not a wider one. What the program is still holding when the
            // run ends is still held; the run ending is not the machine
            // ending, and lhat_run's own start is what lets the next one go.
            //
            // `ending` says the queue is the only thing keeping the run
            // open. The loop takes one coroutine per turn and comes straight
            // back here, and once nothing is left the frame goes for real.
            // Once only (`end_swept`): a cleanup that drops something has
            // already had its own chance, and the end of a run must not be
            // something a program can go on extending.
            // ... and only when a carrier exists at all. When no suspended
            // coroutine holds pending cleanups the sweep's answer is
            // provably empty, so the run ends without it -- a host calling
            // in a loop pays nothing for a promise nothing is owed under.
            if (m->frame_count == base_depth + 1 && !end_swept &&
                m->cleanup_carriers != 0) {
                end_swept = true;
                lhat_gc_collect(m);
                if (m->pending_dispose != NULL) {
                    ending = true;
                    pc = at;
                    continue;
                }
            }
            m->frame_count--;

            if (m->frame_count == base_depth) {
                // 05 の 8.9改2: a coroutine's return^ hands its T to the
                // host whole, the same pointer form a yield^'s produce
                // takes. (The checker still refuses one as the program's
                // own answer; a relaxed run that wrote one gets it too --
                // the bytes are real either way.)
                if (lhat_is_hostvalue(value)) {
                    return vm_finish(m, chunk, LHAT_RUN_OK,
                                  vm_hand_hostvalue_out(m, value), at);
                }
                // 02 の 13.8改: several values do cross. The positions rode
                // the frame's own room through the drain; they are copied
                // into the machine's here so the result may point at them
                // once the frame is gone. `value` becomes position 1, which
                // is what lets a host written before tuples read the answer
                // and get something it can use.
                if (lhat_is_run(value)) {
                    size_t positions = lhat_run_width(value);
                    if (positions > LHAT_MAX_TUPLE) {
                        return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held;
                        held.as = frame->answer_run[i + 1];
                        held.tag = (LhatValueTag)frame->answer_tags[i + 1];
                        m->tuple_scratch[i] = held;
                    }
                    m->tuple_scratch_count = positions;
                    value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
                }
                return vm_finish(m, chunk, LHAT_RUN_OK, value, at);
            }

            uint8_t into = frame->result;
            // 02 の 13.8改: read off the frame that is going, before `frame`
            // becomes the caller below. The room outlives the pop -- the
            // array entry is still there, only uncounted.
            uint8_t reserved = frame->prepared;
            const LhatValueUnion *answered_run = frame->answer_run;
            const uint8_t *answered_tags = frame->answer_tags;
            // 02 の 11.9: the one frame whose answer is not the value of
            // the expression that made it. An ordering that reached for '<=>'
            // asked a number^ of it, and what was written asks which side of
            // zero that falls on.
            LhatOpcode derived = frame->derive;
            bool derived_equal = frame->derive_equal;
            frame = &m->frames[m->frame_count - 1];
            rbase = frame->base;
            chunk = &frame->closure->proto->chunk;
            pc = frame->pc;
            if (derived != LHAT_FRAME_NO_DERIVE) {
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (derived_equal) {
                    // 11.9改: an op^= answered, and its bool^ is the
                    // judgement. The shape rule asks it for one, so anything
                    // else is a body that did not keep to its signature.
                    if (!lhat_is_bool(value)) {
                        return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    held = lhat_as_bool(value) == (derived == LHAT_BC_EQ);
                } else if (derived == LHAT_BC_EQ || derived == LHAT_BC_NE) {
                    held = lhat_value_equal(value, lhat_integer(0)) ==
                           (derived == LHAT_BC_EQ);
                } else if (!ordering(derived, value, lhat_integer(0), &held,
                                     &status)) {
                    // 11.9: the shape rule asks an op^<=> for a number^, so
                    // this is a body that answered with something else.
                    return vm_finish(m, chunk, status, lhat_nil(), at);
                }
                value = lhat_bool(held);
            }
            // 05 の 8.9: a host value answer rides the frame's own room
            // through the drain (see the RETURN case), and is written out
            // whole here, into the caller's slots -- which are live again
            // now that the callee's window is gone.
            // 02 の 13.8改: a tuple rides the same room, and is laid out here
            // as a head slot naming the width plus the positions after it.
            // The two sides have to agree on that width; a disagreement is
            // reported rather than reconciled, because a tuple and a t^{...}
            // are different types and only pack^ turns one into the other.
            // Wherever the checker ran, the type settled this already -- an
            // unchecked compile, 03 の 4.3's session and 05 の 5.3's
            // separately compiled units are what land here.
            if (lhat_is_run(value)) {
                size_t positions = lhat_run_width(value);
                if (reserved <= 1) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_UNEXPECTED,
                                  lhat_nil(), at);
                }
                // 13.8改: a narrower run is the short side of a widened
                // fold and lands as it is -- CHECKRUN pads the missing
                // positions with nil^ where the binding takes it apart.
                // Only a wider run than the reservation is the mismatch.
                if ((size_t)reserved < positions + 1 ||
                    rbase + into + positions >= m->slot_capacity) {
                    return vm_finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                lhat_slots_set(m->slots, rbase + into, value);
                for (size_t i = 0; i < positions; i++) {
                    LhatValue held;
                    held.as = answered_run[i + 1];
                    held.tag = (LhatValueTag)answered_tags[i + 1];
                    lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                }
            } else if (lhat_is_hostvalue(value)) {
                // 05 の 8.9: before the run-reservation reading -- a call
                // that answers a host value declared its width as prepared,
                // and the whole value is what belongs at the slot. A site
                // that could not know the type reserved one slot, and is
                // refused rather than overwritten past.
                if ((size_t)reserved < value.as.hostvalue_run[0].hostvalue->width ||
                    !vm_place_hostvalue_answer(m, rbase + into, value)) {
                    return vm_finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
            } else {
                SET_R(into, value);
            }
        }
    }

    return vm_finish(m, chunk, LHAT_RUN_OK, lhat_nil(), chunk->count);
}
