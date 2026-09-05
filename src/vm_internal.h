// L^ (lhat) -- private interfaces between the VM implementation files.
// No front-end dependency: these files also build with LHAT_WITH_FRONTEND=OFF.

#ifndef LHAT_VM_INTERNAL_H
#define LHAT_VM_INTERNAL_H

#include "lhat/vm.h"
#include "machine.h"
#include "gc.h"


// 11.3改: how one side answered the operator it was asked for.
typedef enum {
    OPERATOR_PICKED,       // a candidate was found and takes the other operand
    OPERATOR_ABSENT,       // this side carries no such member
    OPERATOR_NO_CANDIDATE, // it carries a group, and none of it takes this
    OPERATOR_NOT_CALLABLE, // the member is there and is not a subroutine
    OPERATOR_NO_MEMORY
} OperatorLookup;


// 02 の 16.3: a table's walk has no body to enter, so one step is the whole
// of resuming it. Both the opcode the loops emit and the start()/resume() a
// program writes by hand take this step -- 16.3 puts a table's iterate() on
// the same footing as any other coroutine, so what the two do has to agree.
typedef enum {
    WALK_TOOK,      // a pair came out; the walk is suspended again
    WALK_ENDED      // nothing left, so the walk is finished
} WalkStep;

// 16.3 with 13.8改: how the walk was asked, which decides where one step's
// tuple goes. The loop says which by RESUME's C -- the count of names is
// syntax, so an unchecked compile says the same thing a checked one does
// (03 の 4.2). No mode allocates.
typedef enum {
    // 'for^ v in^ t': the values of the sequence half, in order, the keyed
    // half not visited -- 'for^ i from^ 1 to^ the length { t[i] }' written
    // as a walk.
    WALK_AS_VALUE,
    // 'for^ k, v in^ t', and a hand-driven call that reserved the width:
    // the pair as a run -- head slot plus two positions in the slots the
    // caller reserved.
    WALK_AS_RUN,
    // 15.8's delegation loop, which reserved one slot: the head goes into
    // it and the positions into the frame's answer room, which is exactly
    // where the YIELD forwarding them reads a tuple's positions from.
    WALK_AS_ANSWER
} WalkMode;



// What an index reads from. 04 の 2.3 gives every error message and cause
// without declaring them, so an error answers a member the same way a table
// does -- from the table its fields live in.
static inline LhatTable *vm_table_of(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return (LhatTable *)lhat_as_object(value);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
        return ((LhatError *)lhat_as_object(value))->fields;
    }
    // 02 の 19 章: E.AAA reads the members table; it is sealed, so the
    // writes the instructions refuse stay refused.
    if (lhat_is_object_kind(value, LHAT_OBJECT_ENUM)) {
        return ((LhatEnum *)lhat_as_object(value))->members;
    }
    return NULL;
}


// The caller has checked frame/slot capacity and placed the arguments.
// Only active metadata is initialized; the answer/cleanup arrays are empty.
static inline Frame *vm_push_frame(Machine *m, const LhatClosure *closure,
                                    size_t base, uint8_t result, uint8_t prepared)
{
    Frame *frame = &m->frames[m->frame_count++];
    frame->closure = closure;
    frame->pc = 0;
    frame->base = base;
    frame->result = result;
    frame->prepared = prepared;
    frame->cleanup_count = 0;
    frame->returning = false;
    frame->coroutine = NULL;
    frame->disposing = false;
    frame->derive = LHAT_FRAME_NO_DERIVE;
    frame->derive_equal = false;
    frame->drop_answer = false;
    frame->answer = lhat_nil();
    return frame;
}

// vm.c
bool vm_three_way(LhatValue left, LhatValue right, int *out);
LhatRunResult vm_run_frames(Machine *m, size_t base_depth, bool draining);

// vm_member.c
const char *vm_operator_name(LhatOpcode op, size_t *length);
const LhatTable *vm_readable_table(LhatValue value);
LhatRuntimeType *vm_tag_type(LhatHeap *heap, LhatValue value);
bool vm_plain_table(LhatValue on);
bool vm_fits_call(LhatValue candidate, const LhatValue *at, uint8_t given,
                      bool method, size_t *skip);
OperatorLookup vm_operator_candidate(Machine *m, LhatValue side,
                                         const char *name, size_t length,
                                         LhatValue receiver, LhatValue argument,
                                         uint8_t given, bool self_last,
                                         LhatValue *picked);
LhatRunStatus vm_get_member(Machine *m, size_t into, size_t receiver,
                            size_t key_slot, LhatValue member_key,
                            LhatMemberCache *filling);

// vm_host.c
LhatTable *vm_hostvalue_members_of(Machine *m, const LhatHostValueTag *tag);
bool vm_hostvalue_equal(LhatSlots slots, size_t left, size_t right);
bool vm_place_hostvalue_answer(Machine *m, size_t at, LhatValue answered);
LhatValue vm_hand_hostvalue_out(Machine *m, LhatValue value);
bool vm_place_run_answer(Machine *m, size_t at, size_t reserved,
                             LhatValue answered);
LhatValue vm_stash_sent_hostvalue(Machine *m, size_t slot);
const LhatHostValueField *vm_hostvalue_field_named(
    const LhatHostValueTag *tag, LhatValue key);
bool vm_hostvalue_field_set(LhatValueUnion *data,
                                const LhatHostValueField *field,
                                LhatValue value);

// vm_table.c
bool vm_set_key(Machine *m, LhatTable *table, LhatValue key,
                    LhatValue value, bool *refused);
bool vm_bake_default(Machine *m, LhatValue held, LhatValue *out,
                         size_t depth, bool *refused_value);
LhatTable *vm_clone_table(Machine *m, const LhatTable *source,
                              size_t depth, bool *too_deep);
LhatTable *vm_concat_tables(Machine *m, const LhatTable *left,
                                const LhatTable *right, bool *collided);
LhatRunStatus vm_table_native(Machine *m, const LhatNative *native,
                                  const LhatValue *args, size_t count,
                                  LhatValue *answer);

// vm_native.c
bool vm_ordinal_of(LhatValue value, int64_t *out);
int64_t vm_resolve_ordinal(int64_t written, size_t count);
LhatRunStatus vm_call_native(Machine *m, const LhatNative *native,
                             size_t into, size_t first, uint8_t b,
                             unsigned prepared);

// vm_call.c
void vm_clear_scratch(Machine *m, size_t base, const LhatProto *proto);
void vm_enter_disposal_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result,
                                 Frame **frame, size_t *rbase,
                                 const LhatChunk **chunk, size_t *pc);
LhatUpvalue *vm_capture(Machine *m, size_t slot);
void vm_close_upvalues(Machine *m, size_t above);
void vm_close_one_upvalue(Machine *m, size_t slot);
bool vm_host_faulted(Machine *m, size_t frames_before,
                         LhatRunStatus *status, LhatValue *value);
LhatRunResult vm_finish(Machine *m, const LhatChunk *chunk,
                            LhatRunStatus status, LhatValue value, size_t at);
WalkStep vm_step_table_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                                size_t at, Frame *frame);
WalkStep vm_step_host_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                               size_t at, size_t expected, Frame *frame,
                               const LhatValue *sent, size_t sent_count,
                               LhatRunStatus *fault);
Frame *vm_enter_resume_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result_slot,
                                 uint8_t prepared, const LhatValue *sent,
                                 size_t sent_count);
bool vm_call_host_fn(Machine *m, LhatHostFn call, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answered);

// vm_machine.c
bool vm_set_member(Machine *m, LhatTable *table, const char *name,
                       LhatValue value);

#endif  // LHAT_VM_INTERNAL_H
