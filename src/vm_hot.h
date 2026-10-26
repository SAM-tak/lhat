// Instantiated for value-only loops and loops with ordinary script calls.
// Unsupported operations leave their instruction untouched for the complete
// interpreter. Allocations, host calls and GC steps stay on that path; all
// live values remain in the ordinary machine registers.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static size_t LHAT_HOT_FUNCTION(Machine *m, const LhatChunk *chunk,
                          size_t base, size_t pc)
{
#if LHAT_HOT_CALLS
    LhatSlots slots = m->slots;
    Frame *frame = &m->frames[m->frame_count - 1];
    size_t initialized_depth = m->frame_count;
#define HOT_FRAME frame
#define HOT_EXIT(next_pc) return vm_hot_leave(m, initialized_depth, (next_pc))
#define HOT_R(i) lhat_slots_get(slots, base + (size_t)(i))
#define HOT_SET(i, v) lhat_slots_set(slots, base + (size_t)(i), (v))
#define HOT_INTEGER_FINISH break
#else
    LhatSlots slots = { m->slots.values + base, m->slots.tags + base };
    size_t tail_pc = SIZE_MAX, tail_test_pc = 0;
    LhatInstruction tail_test = 0;
#define HOT_FRAME (&m->frames[m->frame_count - 1])
#define HOT_EXIT(next_pc) return (next_pc)
#define HOT_R(i) lhat_slots_get(slots, (size_t)(i))
#define HOT_SET(i, v) lhat_slots_set(slots, (size_t)(i), (v))
#define HOT_INTEGER_FINISH goto hot_integer_continuation
#endif
    if (m->traps != 0) return pc;
#if LHAT_HOT_FIELDS
    LhatHotFieldCache field_cache[2];
    vm_hot_fields_init(chunk, pc, field_cache);
#endif
#if defined(__clang__) || defined(__GNUC__)
#define HOT_ADD_EXACT(x, y, out) (!__builtin_add_overflow((x), (y), (out)))
#define HOT_SUB_EXACT(x, y, out) (!__builtin_sub_overflow((x), (y), (out)))
#else
#define HOT_ADD_EXACT add_exact
#define HOT_SUB_EXACT subtract_exact
#endif
#define HOT_INTEGER_ARITH(exact, right_value, finish)                       \
    {                                                                     \
        LhatValue left = HOT_R(b), right = (right_value);                   \
        if (!lhat_is_integer(left) || !lhat_is_integer(right)) HOT_EXIT(at);    \
        LhatInteger answer;                                               \
        if (!exact(lhat_as_integer(left), lhat_as_integer(right), &answer)) \
            HOT_EXIT(at);                                                    \
        HOT_SET(a, lhat_integer(answer));                                  \
        finish;                                                           \
    }
#define HOT_INTEGER_ORDER(comparison, right_value)                          \
    {                                                                     \
        LhatValue left = HOT_R(b), right = (right_value);                   \
        if (!lhat_is_integer(left) || !lhat_is_integer(right)) HOT_EXIT(at);    \
        ordered = lhat_as_integer(left) comparison lhat_as_integer(right);  \
        goto hot_ordering;                                                 \
    }
    // Only allocation-free operations run here. Neither hooks nor disposal
    // traps can become live until control returns to the complete VM.
    while (pc < chunk->count) {
        size_t at = pc;
        LhatInstruction instruction = chunk->code[pc];
        unsigned a = lhat_a(instruction), b = lhat_b(instruction);
        unsigned c = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);
        bool ordered;
#if LHAT_HOT_CALLS
        const LhatClosure *direct_callee;
#endif
        switch (op) {
            case LHAT_BC_LOADK:
                HOT_SET(a, chunk->constants[lhat_bx(instruction)]);
                break;
            case LHAT_BC_LOADNIL:
                HOT_SET(a, lhat_nil());
                break;
            case LHAT_BC_LOADBOOL:
                HOT_SET(a, lhat_bool(b != 0));
                break;
            case LHAT_BC_MOVE:
#if LHAT_HOT_FIELDS
                {
                    LhatHotFieldCache *hit = field_cache[0].pc == pc ? &field_cache[0]
                                             : field_cache[1].pc == pc ? &field_cache[1] : NULL;
                    if (hit != NULL && vm_hot_field_step(m, &slots, hit)) {
                        pc += hit->fold.span;
                        continue;
                    }
                }
#endif
                HOT_SET(a, HOT_R(b));
                break;
            case LHAT_BC_THIS:
                HOT_SET(a, lhat_object((LhatObject *)(void *)HOT_FRAME->closure));
#if LHAT_HOT_CALLS
                // Common recursive-call staging: this^, an exact integer
                // subtraction for one argument, then CALL. Preserve both
                // writes and leave at CALL if a GC/budget/general path is
                // needed; an aliased argument must not be subtracted twice.
                if (pc + 2 < chunk->count) {
                    LhatInstruction argument = chunk->code[pc + 1];
                    LhatInstruction call = chunk->code[pc + 2];
                    if (lhat_op(argument) == LHAT_BC_SUBK && lhat_a(argument) == a + 1 &&
                        lhat_op(call) == LHAT_BC_CALL && lhat_a(call) == a && lhat_b(call) == 1 &&
                        lhat_c(argument) < chunk->constant_count) {
                        LhatValue left = HOT_R(lhat_b(argument));
                        LhatValue right = chunk->constants[lhat_c(argument)];
                        LhatInteger value;
                        if (lhat_is_integer(left) && lhat_is_integer(right) &&
                            HOT_SUB_EXACT(lhat_as_integer(left), lhat_as_integer(right), &value)) {
                            direct_callee = frame->closure;
                            HOT_SET(a + 1, lhat_integer(value));
                            pc += 2;
                            at = pc;
                            instruction = call;
                            op = LHAT_BC_CALL;
                            b = 1;
                            c = lhat_c(call);
                            goto hot_script_call;
                        }
                    }
                }
#endif
                break;
            case LHAT_BC_GETUPVAL:
                HOT_SET(a, lhat_ref_get(HOT_FRAME->closure->upvalues[b]->location));
                break;
#if LHAT_HOT_CALLS
            case LHAT_BC_CALL: {
                LhatValue called = HOT_R(a);
                if (!lhat_is_object_kind(called, LHAT_OBJECT_SUBROUTINE)) HOT_EXIT(at);
                direct_callee = (const LhatClosure *)lhat_as_object(called);
            hot_script_call:;
                // The full interpreter owns GC polls, wide/variadic/method
                // calls, coroutine construction and error reporting.
                if (m->objects.count >= m->threshold || m->steps_left == 1 ||
                    (c & LHAT_CALL_SPREAD) != 0 || lhat_call_prepared((uint8_t)c) > 1)
                    HOT_EXIT(at);
                const LhatProto *proto = direct_callee->proto;
                if (proto == NULL) HOT_EXIT(at);
                uint32_t shape = proto->fast_call_shape;
                if ((shape & 0x1ffu) != b + 1) HOT_EXIT(at);
                size_t registers = shape >> 9;
                size_t next_base = base + a + 1;
                if (m->frame_count >= m->frame_capacity ||
                    next_base + registers >= m->slot_capacity)
                    HOT_EXIT(at);
                vm_clear_tags_inline(m->slots.tags + next_base + b, registers - b);
                frame->pc = pc + 1;
                frame = &m->frames[m->frame_count++];
                frame->closure = direct_callee;
                frame->pc = 0;
                frame->base = next_base;
                frame->result = (uint8_t)a;
                frame->prepared = (uint8_t)lhat_call_prepared((uint8_t)c);
                base = next_base;
                chunk = &proto->chunk;
                pc = 0;
                if (m->steps_left != 0) --m->steps_left;
                continue;
            }
            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL: {
                bool pending = m->frame_count > initialized_depth;
                if (m->frame_count <= m->run_base + 1 || m->open != NULL ||
                    m->cleanup_carriers != 0 ||
                    (!pending && (frame->cleanup_count != 0 || frame->coroutine != NULL ||
                                  frame->derive != LHAT_FRAME_NO_DERIVE)) ||
                    frame->prepared > 1 || (op == LHAT_BC_RETURN && b != 0))
                    HOT_EXIT(at);
                LhatValue answer = (!pending && frame->drop_answer) || op == LHAT_BC_RETURN_NIL
                                       ? lhat_nil() : HOT_R(a);
                if (lhat_is_hostvalue(answer) || lhat_is_run(answer)) HOT_EXIT(at);
                unsigned into = frame->result;
                --m->frame_count;
                if (initialized_depth > m->frame_count) initialized_depth = m->frame_count;
                frame = &m->frames[m->frame_count - 1];
                base = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                HOT_SET(into, answer);
                continue;
            }
#endif
            case LHAT_BC_ISNIL:
                ordered = lhat_is_nil(HOT_R(b));
                goto hot_ordering;
            case LHAT_BC_NOT:
                if (!lhat_is_bool(HOT_R(b))) HOT_EXIT(at);
                ordered = !lhat_as_bool(HOT_R(b));
                goto hot_ordering;
            // Only constant-step arithmetic probes the loop-tail shortcut.
            // Ordinary accumulator updates keep the short dispatch path.
            case LHAT_BC_ADD:  HOT_INTEGER_ARITH(HOT_ADD_EXACT, HOT_R(c), break);
            case LHAT_BC_SUB:  HOT_INTEGER_ARITH(HOT_SUB_EXACT, HOT_R(c), break);
            case LHAT_BC_ADDK: HOT_INTEGER_ARITH(HOT_ADD_EXACT, chunk->constants[c], HOT_INTEGER_FINISH);
            case LHAT_BC_SUBK: HOT_INTEGER_ARITH(HOT_SUB_EXACT, chunk->constants[c], HOT_INTEGER_FINISH);
            case LHAT_BC_LT:   HOT_INTEGER_ORDER(<, HOT_R(c));
            case LHAT_BC_LE:   HOT_INTEGER_ORDER(<=, HOT_R(c));
            case LHAT_BC_GT:   HOT_INTEGER_ORDER(>, HOT_R(c));
            case LHAT_BC_GE:   HOT_INTEGER_ORDER(>=, HOT_R(c));
            case LHAT_BC_LTK:  HOT_INTEGER_ORDER(<, chunk->constants[c]);
            case LHAT_BC_LEK:  HOT_INTEGER_ORDER(<=, chunk->constants[c]);
            case LHAT_BC_GTK:  HOT_INTEGER_ORDER(>, chunk->constants[c]);
            case LHAT_BC_GEK:  HOT_INTEGER_ORDER(>=, chunk->constants[c]);
            case LHAT_BC_JUMP:
            case LHAT_BC_JUMP_FALSE: {
                if (op == LHAT_BC_JUMP_FALSE) {
                    if (!lhat_is_bool(HOT_R(a))) HOT_EXIT(at);
                    if (lhat_as_bool(HOT_R(a))) break;
                }
                int32_t offset = lhat_jump_offset(instruction);
                if (offset < 0) {
                    // Leave the suspending poll to the complete interpreter,
                    // before changing either the PC or the remaining budget.
                    if (m->steps_left == 1) HOT_EXIT(at);
                    if (m->steps_left != 0) --m->steps_left;
                }
                pc = (size_t)((int64_t)pc + 1 + offset);
                continue;
            }
            case LHAT_BC_FORLOOP:
            case LHAT_BC_FORLOOPD: {
                LhatValue focus = HOT_R(a), end = HOT_R(a + 1), step = HOT_R(a + 2);
                if (!lhat_is_integer(focus) || !lhat_is_integer(end) || !lhat_is_integer(step)) HOT_EXIT(at);
                LhatInteger next;
                bool exact = op == LHAT_BC_FORLOOP
                                 ? add_exact(lhat_as_integer(focus), lhat_as_integer(step), &next)
                                 : subtract_exact(lhat_as_integer(focus), lhat_as_integer(step), &next);
                if (!exact) break;
                bool again = op == LHAT_BC_FORLOOP ? next <= lhat_as_integer(end) : next >= lhat_as_integer(end);
                if (again && m->steps_left == 1) HOT_EXIT(at);
                HOT_SET(a, lhat_integer(next));
                if (again) {
                    if (m->steps_left != 0) --m->steps_left;
                    pc = (size_t)((int64_t)pc + 1 + lhat_jump_offset(instruction));
                    continue;
                }
                break;
            }
            case LHAT_BC_GETINDEX:
            case LHAT_BC_GETMEMBER: {
                LhatValue key = op == LHAT_BC_GETMEMBER
                                    ? chunk->constants[chunk->member_keys[c]] : HOT_R(c);
                LhatValue answer;
                if (!vm_read_own_table(HOT_R(b), key, &answer)) HOT_EXIT(at);
                HOT_SET(a, answer);
                break;
            }
            case LHAT_BC_SETINDEX: {
                LhatValue owner = HOT_R(a), key = HOT_R(b), value = HOT_R(c);
                if (!lhat_is_object_kind(owner, LHAT_OBJECT_TABLE)) HOT_EXIT(at);
                LhatTable *table = (LhatTable *)lhat_as_object(owner);
                if (table->sealed || table->shared) HOT_EXIT(at);
                if (lhat_is_integer(key) &&
                    (uint64_t)lhat_as_integer(key) < (uint64_t)table->array_count) {
                    lhat_slots_set(table->array, (size_t)lhat_as_integer(key), value);
#if !LHAT_HOT_CALLS && !LHAT_HOT_FIELDS
                } else if (lhat_is_integer(key) &&
                           (uint64_t)lhat_as_integer(key) == (uint64_t)table->array_count &&
                           table->array_count < table->array_capacity && table->entry_count == 0) {
                    lhat_slots_set(table->array, table->array_count++, value);
#endif
                } else {
                    if (lhat_is_nil(value)) HOT_EXIT(at);
#if !LHAT_HOT_CALLS && !LHAT_HOT_FIELDS
                    LhatTableEntry *entry = vm_own_string_store_entry(table, key);
#else
                    LhatTableEntry *entry = vm_own_string_entry(table, key);
#endif
                    if (entry == NULL) HOT_EXIT(at);
                    entry->key = key;
                    entry->value = value;
                }
                vm_table_write_barrier(m, table, key, value);
                break;
            }
            default:
                HOT_EXIT(at);
        }
        pc++;
        continue;

#if !LHAT_HOT_CALLS
    hot_integer_continuation:
        // The arithmetic already committed. For the common i += step;
        // jump; i < bound; conditional-jump sequence, pay one budget poll
        // and continue straight into the body or exit. A suspending poll is
        // left at the jump, so the full VM resumes at exactly the same PC.
        if (pc == tail_pc && m->steps_left != 1) {
            LhatValue bound = lhat_op(tail_test) == LHAT_BC_LTK
                                  ? chunk->constants[lhat_c(tail_test)]
                                  : HOT_R(lhat_c(tail_test));
            if (lhat_is_integer(bound)) {
                ordered = lhat_as_integer(HOT_R(a)) < lhat_as_integer(bound);
                if (m->steps_left != 0) --m->steps_left;
                pc = tail_test_pc;
                a = lhat_a(tail_test);
                goto hot_ordering;
            }
        }
        if (pc != tail_pc && a == b && pc + 1 < chunk->count) {
            LhatInstruction back = chunk->code[pc + 1];
            if (lhat_op(back) == LHAT_BC_JUMP && lhat_jump_offset(back) < 0) {
                int64_t target = (int64_t)pc + 2 + lhat_jump_offset(back);
                if (target >= 0 && (uint64_t)target + 1 < chunk->count) {
                    LhatInstruction test = chunk->code[(size_t)target];
                    LhatInstruction branch = chunk->code[(size_t)target + 1];
                    LhatOpcode test_op = lhat_op(test);
                    if ((test_op == LHAT_BC_LT || test_op == LHAT_BC_LTK) &&
                        lhat_b(test) == a &&
                        lhat_op(branch) == LHAT_BC_JUMP_FALSE &&
                        lhat_a(branch) == lhat_a(test) && lhat_jump_offset(branch) >= 0) {
                        tail_pc = pc;
                        tail_test_pc = (size_t)target;
                        tail_test = test;
                        goto hot_integer_continuation;
                    }
                }
            }
        }
        pc++;
        continue;
#endif

    hot_ordering:
        // Consume the conditional branch like the full interpreter.
        if (pc + 1 < chunk->count) {
            LhatInstruction paired = chunk->code[pc + 1];
            if (lhat_op(paired) == LHAT_BC_JUMP_FALSE && lhat_a(paired) == a) {
                int32_t offset = lhat_jump_offset(paired);
                if (!ordered && offset < 0) {
                    if (m->steps_left == 1) HOT_EXIT(at);
                    if (m->steps_left != 0) --m->steps_left;
                }
                HOT_SET(a, lhat_bool(ordered));
                pc += 2;
                if (!ordered) pc = (size_t)((int64_t)pc + offset);
                continue;
            }
        }
        HOT_SET(a, lhat_bool(ordered));
        pc++;
    }
#undef HOT_R
#undef HOT_SET
#undef HOT_FRAME
#undef HOT_INTEGER_ARITH
#undef HOT_INTEGER_ORDER
#undef HOT_ADD_EXACT
#undef HOT_SUB_EXACT
#undef HOT_INTEGER_FINISH
#undef HOT_EXIT
#if LHAT_HOT_CALLS
    return vm_hot_leave(m, initialized_depth, pc);
#else
    return pc;
#endif
}
