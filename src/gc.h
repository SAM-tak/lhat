// L^ (lhat) -- the collector.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// 5.12: a mark and sweep that does not move anything, so nothing outside has
// to be told where a value went -- 5.4's upvalues point into the stack and
// 5.7's coroutines keep a copy of their registers, and neither would follow.
// 03 の 1.2 keeps Lua's incremental collector as something to borrow later;
// this is the working form it would replace.
//
// Marking uses an explicit list rather than recursion, so a deep structure
// cannot run the C stack out. The list is threaded through the objects
// themselves (LhatObject.gclist), so putting one on it cannot fail -- there
// is nothing to allocate. Nothing here answers "out of memory": a collection
// that could give up halfway would be leaving the heap in a state it has no
// way to describe.
//
// The machine owns the heap and the roots, and this reads both. 02 の 10.7
// also has it hold back a coroutine it finds unreachable with cleanups still
// pending, which is the one place a collection reaches back into the run.

#ifndef LHAT_GC_H
#define LHAT_GC_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"
#include "value.h"

struct LhatMachine;

// 5.12: what a collection knows about an object. White is "not reached";
// gray is "reached, but what it refers to has not been"; black is "reached,
// and so is everything it refers to". The invariant a collector has to keep
// while it marks is that **no black object refers to a white one** -- a black
// one is finished with, so a white one only it refers to would be swept while
// still live.
//
// There are two whites because the mutator runs between steps. The heap
// hands out one of them (LhatHeap.white) and the collector sweeps the other,
// so an object made while a sweep is under way is born the colour of the
// living. They swap each cycle. Lua does the same, with bits rather than a
// number; here nothing is packed alongside, and zero being a white is what
// lets a heap need no initialiser.
#define LHAT_GC_WHITE0 0
#define LHAT_GC_WHITE1 1
#define LHAT_GC_GRAY   2
#define LHAT_GC_BLACK  3

#define lhat_gc_is_white(o) ((o)->color <= LHAT_GC_WHITE1)
#define lhat_gc_is_black(o) ((o)->color == LHAT_GC_BLACK)

// The white a heap is not handing out. After the swap, an object still
// wearing it was not reached by the collection that just finished.
#define lhat_gc_other_white(w) ((uint8_t)(LHAT_GC_WHITE1 - (w)))

// Marks the value if it is a white object, and puts it on `*gray` so that
// what it refers to is reached too.
void lhat_gc_reach(LhatObject **gray, LhatValue value);

// Marks everything the object refers to. Objects a chunk owns are reached
// like any other; the sweep simply never visits that list, so one of those
// goes black and stays black, and a later collection walks past it without
// looking in. That is only sound because what a chunk holds -- 5.9's error
// kinds, 5.11's types, the strings its constants name -- refers to nothing
// but other objects of the same chunk, and none of them is ever written to
// after the chunk is built. A chunk object able to hold a running program's
// value would need the barriers 5.12 puts on the machine's own.
void lhat_gc_children(LhatObject **gray, LhatObject *object);

// ---------------------------------------------------------------------------
// Running a cycle a piece at a time
// ---------------------------------------------------------------------------
//
// Where a cycle has got to. A step picks up from here and leaves it here.
//
//   PAUSE      nothing under way. Everything is white; the next step starts
//              a cycle by reading the roots.
//   PROPAGATE  following what the roots reached. The program runs between
//              steps, so this is where the barriers below have to hold.
//   SWEEP      walking the heap, freeing what still wears the dead white.
//              The marking is over, so everything the program can reach is
//              black or was born since the swap: the barriers are not needed
//              here, and nothing new can be lost.
#define LHAT_GC_PAUSE     0
#define LHAT_GC_PROPAGATE 1
#define LHAT_GC_SWEEP     2

// LHAT_GC_STEP_WORK objects' worth of whatever phase the collector is in,
// and then back to the program. Also sets when the next step is due.
void lhat_gc_step(struct LhatMachine *machine);

// A whole cycle, now, and nothing left half done: what the program cannot
// reach when this is called has been freed when it answers. What 05 の 8.6's
// collectgarbage() is, and the only thing that gives a pause the size of the
// heap -- which is why the program has to ask for it by name.
void lhat_gc_collect(struct LhatMachine *machine);

// ---------------------------------------------------------------------------
// The barriers
// ---------------------------------------------------------------------------
//
// 5.12: a black object is one the collector is finished with, so a white
// object that only it refers to would be swept while still live. These are
// what a write that could do that has to go through. Both are nothing at all
// unless a cycle is in PROPAGATE and `parent` is actually black.
//
// The collector only ever runs at an instruction boundary (5.12's rule about
// half-built objects), so a write in the same instruction that made `parent`
// needs neither: nothing has looked at it yet. What needs one is a write to
// an object that was already there when a step ran -- a table, an upvalue,
// a suspended coroutine's saved registers, an overload^ group.

// Forward: reaches `value` now, so the black parent is telling the truth
// again. For a place written once or twice.
void lhat_gc_barrier(struct LhatMachine *machine, LhatObject *parent,
                     LhatValue value);

// Backward: puts `parent` back on the gray list to be looked at again. For a
// place written over and over -- a table -- where marking each value as it
// arrives would cost more than one more visit to the table at the end.
void lhat_gc_barrier_back(struct LhatMachine *machine, LhatObject *parent,
                          LhatValue value);

// 02 の 10.7: the next coroutine the last collection held back, or NULL.
// Taking it off the list is the caller's signal that it is about to run the
// cleanups; nothing else marks one as handled.
LhatCoroutine *lhat_gc_take_pending(struct LhatMachine *machine);

#endif  // LHAT_GC_H
