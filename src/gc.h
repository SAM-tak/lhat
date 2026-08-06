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
#define lhat_gc_is_gray(o)  ((o)->color == LHAT_GC_GRAY)
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

// Frees what still wears the heap's other white, and turns what survives to
// the heap's current one. Returns how many objects were freed. **The caller
// swaps LhatHeap.white before calling this**: what was being handed out
// during the marking is what the unreached are wearing.
//
// `machine` is what a host value's dispose^ is handed; NULL where the heap
// cannot hold one, as a chunk's cannot.
size_t lhat_gc_sweep(LhatHeap *heap, struct LhatMachine *machine);

// One whole cycle: the roots, what they reach, 10.7's holding back, and the
// sweep. The machine's threshold is set from what survived, so this is also
// what decides when the next one happens.
void lhat_gc_collect(struct LhatMachine *machine);

// 02 の 10.7: the next coroutine the last collection held back, or NULL.
// Taking it off the list is the caller's signal that it is about to run the
// cleanups; nothing else marks one as handled.
LhatCoroutine *lhat_gc_take_pending(struct LhatMachine *machine);

#endif  // LHAT_GC_H
