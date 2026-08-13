// L^ (lhat) -- what a running machine is made of.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// vm.h keeps LhatMachine opaque, which is the right shape for a host: the
// thing is a whole stack and a frame array, so a caller keeps the handle and
// never the object. This is the inside of it, for the two files that are the
// machine -- vm.c, which runs it, and gc.c, which has to see the roots.
//
// Not on a host's include path and not installed. Nothing include/lhat.h
// reaches names this file.

#ifndef LHAT_MACHINE_H
#define LHAT_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "code.h"
#include "gc.h"
#include "lhat/config.h"
#include "lhat/object.h"
#include "lhat/value.h"

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
    size_t cleanups[LHAT_MAX_CLEANUPS];
    size_t cleanup_count;

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
} Frame;

// 02 の 13.8改: the room above holds one head slot plus the positions, and a
// tuple shares it with 05 の 8.9's host values. Keeping the two limits tied
// here means widening one cannot silently outgrow the other.
_Static_assert(LHAT_MAX_TUPLE <= LHAT_HOSTVALUE_MAX_BYTES / 8,
               "LHAT_MAX_TUPLE must fit a frame's answer room");

// What `derive` holds when a frame's answer is its own. LOADK is never a
// comparison, so it stands for "nothing to read off this one".
#define LHAT_FRAME_NO_DERIVE LHAT_BC_LOADK

struct LhatMachine {
    // 2.2: the one shared stack, as two parallel runs -- payloads dense,
    // tags one byte each -- read and written through `slots` below. 16
    // bytes a slot becomes 9, and a frame's worth of tags sits in one or
    // two cache lines.
    LhatValueUnion stack_values[LHAT_STACK_SLOTS];
    uint8_t stack_tags[LHAT_STACK_SLOTS];
    LhatSlots slots;  // the view over the two, fixed at lhat_machine_new
    Frame frames[LHAT_MAX_FRAMES];
    size_t frame_count;

    // Everything allocated while running. What the answer cannot reach is
    // freed as the program runs; the rest passes to the caller at the end.
    LhatHeap objects;
    LhatUpvalue *open;  // 5.4, innermost first

    // 02 の 10.7: coroutines the collector found unreachable with cleanups
    // still pending. They are kept alive a cycle longer and run one at a
    // time at an instruction boundary, since a finally^ is L^ code and the
    // middle of a collection is no place to run any.
    LhatCoroutine *pending_dispose;

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

    // 05 の 5.3: the units a require^ can reach, in the order the program
    // compiled them. Borrowed -- the program owns them and outlives the run.
    const LhatModule *modules;
    size_t module_count;

    // 05 の 8.9: one members table per registered host value type, indexed
    // by tag->index -- what a member call on a host value answers through,
    // since the value itself has no heap half to carry one. The tables live
    // under L^.modules (bound by lhat_machine_bind_hostvalues), so the
    // environment root already keeps them; only the array is the machine's
    // to free.
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
};

typedef struct LhatMachine Machine;

#endif  // LHAT_MACHINE_H
