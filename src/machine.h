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
#include "lhatconfig.h"
#include "object.h"
#include "value.h"

typedef struct {
    const LhatClosure *closure;
    size_t pc;
    size_t base;       // 5.2: the frame's registers start at this stack slot
    uint8_t result;    // where in the caller's frame the answer goes

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

    // 5.11: the coroutine this frame belongs to, when it is one. NULL for an
    // ordinary call.
    LhatCoroutine *coroutine;
    bool disposing;  // 02 の 10.7: no yield^ while the cleanups are running
} Frame;

struct LhatMachine {
    // 2.2改: the one shared stack, as two parallel runs -- payloads dense,
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
};

typedef struct LhatMachine Machine;

#endif  // LHAT_MACHINE_H
