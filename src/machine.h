// 02 の 14.22: one link of the chain Machine.native_hold heads. Declared
// here, defined by the built-in that pushes it -- on its own C stack frame,
// which outlives exactly the window the root is needed for.
typedef struct LhatNativeHold {
    LhatValue held;
    struct LhatNativeHold *outer;
} LhatNativeHold;

// L^ (lhat) -- what a running machine is made of.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// vm.h keeps LhatMachine opaque, which is the right shape for a host: the
// thing is a whole stack and a frame array, so a caller keeps the handle and
// never the object. This is the inside of it, for the three files that are
// the machine -- vm.c, which runs it, gc.c, which has to see the roots, and
// debug.c, which reads the frames for a debugger (09 章).
//
// Not on a host's include path and not installed. Nothing include/lhat.h
// reaches names this file.

#ifndef LHAT_MACHINE_H
#define LHAT_MACHINE_H

#include "lhat/version.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "code.h"
#include "gc.h"
#include "lhat/config.h"
#include "lhat/debug.h"
#include "lhat/object.h"
#include "lhat/value.h"
#include "lhat/vm.h"

typedef struct {
    const LhatClosure *closure;
    size_t pc;
    size_t base;       // 5.2: the frame's registers start at this stack slot
    uint8_t result;    // where in the caller's frame the answer goes

    // 02 の 13.8改: how many consecutive slots the call site reserved at
    // `result` for the answer. 0 and 1 both mean one, which is every call
    // written before tuples existed. The callee reads it to know whether the
    // caller is expecting a run of slots -- the agreement cannot be settled
    // statically (an unchecked compile, 03 の 4.3's session, 05 の 5.3's
    // units, and a callee that is only ever a value), so it is carried here
    // and a mismatch is caught rather than papered over.
    uint8_t prepared;

    // 5.5: the cleanups this frame has entered and not yet run, innermost
    // last. A finally^ and a with^ are both just a stretch of code to run.
    // An instruction index is a Bx (code.h's lhat_bx), so 16 bits hold every
    // one a jump could reach -- and a frame array this wide is most of what
    // a machine weighs.
    uint16_t cleanups[LHAT_MAX_CLEANUPS];
    uint8_t cleanup_count;

    // Draining state. `target` is the depth to stop at; `resume` is where to
    // carry on afterwards, unless the drain is a return^ carrying `answer`.
    size_t drain_target;
    size_t resume;
    LhatValue answer;
    bool returning;

    // 05 の 8.9: a host value answer, carried whole. `answer` is one slot,
    // and no register survives the drain -- the callee's window overlaps
    // the caller's scratch, so there is nowhere on the stack a wide answer
    // could sit while the cleanups run. The frame carries its own room
    // instead (a head-shaped run, as everywhere at the host boundary), and
    // nesting is free: a cleanup's own calls return through their own
    // frames' rooms.
    LhatValueUnion answer_run[1 + LHAT_HOSTVALUE_MAX_BYTES / 8];

    // 02 の 13.8改: the tags of the run above, when it is carrying a tuple
    // rather than a host value. A host value's continuation slots are raw
    // bytes and need none; a tuple's positions are ordinary values, and the
    // collector has to read them as values while the cleanups run -- so this
    // array is the whole of what a tuple adds to the room. Meaningful only
    // when `answer` is LHAT_VALUE_RUN.
    uint8_t answer_tags[1 + LHAT_HOSTVALUE_MAX_BYTES / 8];

    // 5.11: the coroutine this frame belongs to, when it is one. NULL for an
    // ordinary call.
    LhatCoroutine *coroutine;
    bool disposing;  // 02 の 10.7: no yield^ while the cleanups are running

    // 5.3: a tail call took this frame over from a body that was throwing the
    // answer away -- a bare call standing last, where what the frame answers
    // is the nil^ of falling off the end. Whatever runs in the frame now, that
    // is still what its caller is owed.
    bool drop_answer;

    // 02 の 11.9: an ordering that had to reach for '<=>' to answer.
    // The frame carries which comparison was written, and what comes back is
    // read against zero with it rather than handed over as it is -- the one
    // place a frame's answer is not the value of the expression that made it.
    // LHAT_FRAME_NO_DERIVE means an ordinary call, which every other frame is.
    LhatOpcode derive;
    // 02 の 11.9改: what answered was an op^= rather than an op^<=>, so the
    // bool^ it hands back is the judgement itself. `derive` still says which
    // comparison was written -- '≠' negates what comes back.
    bool derive_equal;
} Frame;

// 5.5: a cleanup is remembered as the Bx of the PUSHCLEANUP that entered it,
// so the room it is kept in is tied to how wide a Bx is -- both here and in
// the coroutine a suspended frame hands them to (02 の 10.7).
_Static_assert(sizeof(((Frame *)0)->cleanups[0]) >=
                   sizeof(lhat_bx((LhatInstruction)0)),
               "a cleanup has to hold a Bx");
_Static_assert(sizeof(((LhatCoroutine *)0)->cleanups[0]) >=
                   sizeof(((Frame *)0)->cleanups[0]),
               "a coroutine has to hold what a frame hands it");
_Static_assert(LHAT_MAX_CLEANUPS <= UINT8_MAX,
               "cleanup_count has to count them");

// 02 の 13.8改: the room above holds one head slot plus the positions, and a
// tuple shares it with 05 の 8.9's host values. Keeping the two limits tied
// here means widening one cannot silently outgrow the other.
_Static_assert(LHAT_MAX_TUPLE <= LHAT_HOSTVALUE_MAX_BYTES / 8,
               "LHAT_MAX_TUPLE must fit a frame's answer room");

// What `derive` holds when a frame's answer is its own. LOADK is never a
// comparison, so it stands for "nothing to read off this one".
#define LHAT_FRAME_NO_DERIVE LHAT_BC_LOADK

// 05 の 8.9: the host value whose head sits at `slot`, as a host receives
// one -- a value aiming back into the stack. Stable for as long as the
// machine lives: the stack is the machine's own, made once and never moved
// (03 の 4.3改 lets a caller choose how long it is, not where it sits).
static inline LhatValue hostvalue_argument(LhatSlots slots, size_t slot)
{
    LhatValue v;
    v.tag = LHAT_VALUE_HOSTVALUE;
    v.as.hostvalue_run = slots.values + slot;
    return v;
}

// 05 の 8.12: one thing a host remembered about one of its own objects. A
// free slot's key is NULL and one a removal left is LHAT_WEAK_GONE.
#define LHAT_WEAK_GONE ((const void *)(uintptr_t)1)

typedef struct LhatWeakEntry {
    const void *key;
    LhatValue value;
} LhatWeakEntry;

struct LhatMachine {
    // 2.2: the one shared stack, as two parallel runs -- payloads dense,
    // tags one byte each -- read and written through `slots`. 16 bytes a
    // slot becomes 9, and a frame's worth of tags sits in one or two cache
    // lines.
    //
    // 03 の 4.3改: the two runs and the frame array are laid down after the
    // struct inside the machine's single allocation, so how long they are is
    // the caller's to say (lhat_machine_new_with_size) while a machine stays
    // one calloc and one free. Their lengths are read from the machine, not
    // from the constants -- those are only what lhat_machine_new asks for.
    LhatSlots slots;       // the view over the two runs, fixed at creation
    size_t slot_capacity;  // how many slots the two runs hold
    Frame *frames;
    size_t frame_capacity;
    size_t frame_count;

    // 04 の 11.6改: where the running span of frames begins -- run_frames'
    // base_depth, kept here so a fault can say which frames were this
    // run's. And the fault itself: the span [fault_base, fault_depth) plus
    // the faulting instruction, recorded by finish() and readable through
    // lhat_machine_fault_* until the next run resets the frames.
    size_t run_base;
    // 02 の 15.15: the slice. `budget` is what a run starts with (0 = none,
    // and then nothing is counted); `steps_left` is what is left of it, and
    // is 0 for a nested run -- only the outermost may be taken off the
    // processor. `running_depth` is how many run_frames are on the C stack,
    // which is what tells the two apart. A run that ran out leaves
    // `suspended` set with its frames standing and `suspended_base` saying
    // where the run began, for lhat_machine_continue.
    int64_t budget;
    int64_t steps_left;
    size_t running_depth;
    size_t suspended_base;
    bool suspended;
    size_t fault_base;
    size_t fault_depth;
    size_t fault_at;

    // Everything allocated while running. What the answer cannot reach is
    // freed as the program runs; the rest passes to the caller at the end.
    LhatHeap objects;
    LhatUpvalue *open;  // 5.4, innermost first

    // 02 の 10.7: coroutines the collector found unreachable with cleanups
    // still pending. They are kept alive a cycle longer and run one at a
    // time at an instruction boundary, since a finally^ is L^ code and the
    // middle of a collection is no place to run any.
    LhatCoroutine *pending_dispose;

    // 02 の 10.7: how many suspended coroutines are carrying pending
    // cleanups right now. The end-of-run collection exists only to find
    // such a coroutine dropped too late for any other cycle -- when this
    // is zero its answer is provably empty, so the run's last frame leaves
    // without one. Raised where a yield^ stores a non-empty cleanup list,
    // lowered where a suspended carrier is taken up again (resume or
    // disposal -- the collector's queue goes through disposal too).
    size_t cleanup_carriers;

    // 5.12: what the collector has reached and not yet looked into, threaded
    // through LhatObject.gclist. Empty except while a collection is running.
    LhatObject *gray;

    // Where the cycle has got to (gc.h's LHAT_GC_PAUSE and friends), and
    // where in the heap's list the sweep is. The sweep is a link rather than
    // an object so that freeing the one it is on needs no special case; it
    // is NULL outside LHAT_GC_SWEEP.
    uint8_t gcstate;
    LhatObject **sweep;

    size_t collected;
    size_t threshold;   // how many live objects before the next step

    // 05 の 8.6: what L^ answers. The one table nothing has to import, so it
    // is made with the machine and rooted by it rather than by any frame.
    LhatTable *environment;

    // 02 の 14.11: the key a definition's prototype sits under. Construction
    // reads it on every NEWINSTANCE, so the machine keeps the one string
    // rather than making it each time. A root of its own -- no frame holds
    // it (gc.c's mark_roots).
    LhatString *self_key;

    // 05 の 8.7: L^.modules itself. Every registration and every hostdata
    // value made reaches through it, and finding it by name meant a
    // "modules" string made to ask the question and thrown away. Held here
    // instead; the environment above is what roots it, and this is that
    // same table rather than a second one.
    LhatTable *modules;

    // 05 の 8.9: one members table per registered host value type, indexed
    // by tag->index -- what a member call on a host value answers through,
    // since the value itself has no heap half to carry one. The tables live
    // under L^.modules (bound by lhat_machine_bind_hostvalues), so the
    // environment root already keeps them; only the array is the machine's
    // to free.
    // 05 の 8.12: what a host remembers about its own objects, held WEAKLY.
    // Not a root: the collector drops an entry whose value it decided is
    // dead, at the end of marking and before the sweep (gc.c's atomic).
    // That is the whole of what makes reading one safe -- a host's own map
    // cannot do it, because nothing tells it when the decision was made.
    LhatWeakEntry *weak;
    size_t weak_count;     // live entries
    size_t weak_used;      // live plus removed, which is what the load is
    size_t weak_capacity;  // a power of two, or 0

    LhatTable **hostvalue_members;
    size_t hostvalue_member_count;

    // 05 の 8.9: where lhat_make_hostvalue builds the head-shaped run a host
    // answers with. Consumed the moment the call returns (the machine writes
    // it out into slots before anything else runs), so one is enough even
    // when hosts call back in.
    LhatValueUnion hostvalue_scratch[1 + LHAT_HOSTVALUE_MAX_BYTES / 8];

    // 02 の 13.8改: the same room for the several values a host answers with,
    // and what a run answered to a host is copied into on the way out. One is
    // enough for the same reason as above.
    //
    // The one place this is not like 8.9's: a host value is bytes and holds
    // no reference, so the scratch above is not a root. These are ordinary
    // values, and a host may allocate between filling them and returning
    // (a table it builds, a call back into L^), so gc.c walks these -- the
    // same reason a frame's answer room is walked.
    LhatValue tuple_scratch[LHAT_MAX_TUPLE];
    size_t tuple_scratch_count;  // 0 when nothing is being carried

    // 04 の 11.6改 with 05 の 8.7: what the last fault was. Set beside
    // fault_depth so that a host function whose nested lhat_machine_call
    // faulted -- leaving those frames standing -- can be seen from the
    // instruction that called the host, and the outer run ended with the
    // same status rather than running on over dead frames.
    LhatRunStatus fault_status;
    LhatValue fault_value;  // what a PANIC carried; nil^ for the rest

    // 05 の 8.7改2: lhat_machine_panic was called inside a host function
    // that has not returned yet -- the value is in fault_value, and the
    // instruction that called the host ends the run with it (host_faulted).
    bool host_panicked;

    // 09 の 2 章: the debugger's hook. `hook_live` is what the loop tests:
    // the hook while it is set and not running, NULL while it runs -- so
    // a call it makes back into L^ never sounds it again -- and NULL when
    // there is none. `hook_depth` and `hook_pc` are the frame count and
    // the instruction the hook last looked at, which is how the next
    // instruction is known to begin a line (vm.c's hook_line).
#if LHAT_WITH_DEBUGGER
    LhatDebugHook hook;
    LhatDebugHook hook_live;
    void *hook_context;
    size_t hook_depth;
    size_t hook_pc;
#endif

    // 02 の 14.22: the tables built-ins are holding onto while written L^
    // code runs inside them -- a sort's aux while the comparator runs, a
    // clone under construction while the policy does. Either may allocate
    // and collect, so each is a root for its window. A chain, not one
    // slot: a clone's policy may itself clone ('x.clone^(this^)'), and
    // every level's table has to stay reachable, not just the innermost.
    // The nodes live on the C stack of the built-in that pushed them.
    struct LhatNativeHold *native_hold;
};

typedef struct LhatMachine Machine;

// 09 の 5.1: the one process-wide machine watcher, owned by debug.c and read
// by vm.c at lhat_machine_new / lhat_machine_dispose. Zeroed = none.
#if LHAT_WITH_DEBUGGER
extern LhatMachineWatcher lhat_machine_watcher;
#endif

// 09 の 3.5: runs `closure` -- one that declares no parameters -- on a frame
// of its own above everything standing, with `seed` values laid into its
// first registers before it starts. What a debugger's evaluation runs on:
// the seeded slots are a stopped frame's names, copied in (debug.c).
//
// Unlike everywhere else a fault does NOT leave its frames standing: an
// evaluation is nobody's run to read a traceback off, and the machine has a
// paused program to go back to -- so they are closed over and dropped, and
// no fault stays recorded. Defined in vm.c, which owns the loop.
LhatRunResult lhat_machine_run_seeded(Machine *machine,
                                      const LhatClosure *closure,
                                      const LhatValue *seed, size_t count);

#endif  // LHAT_MACHINE_H
