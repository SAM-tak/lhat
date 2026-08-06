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

// Marks the value if it is an unmarked object, and puts it on `*gray` so that
// what it refers to is reached too.
void lhat_gc_reach(LhatObject **gray, LhatValue value);

// Marks everything the object refers to. Objects a chunk owns are reached
// like any other; the sweep simply never visits that list.
void lhat_gc_children(LhatObject **gray, LhatObject *object);

// Frees what is not marked, and unmarks what is. Returns how many objects
// were freed. `machine` is what a host value's dispose^ is handed; NULL where
// the heap cannot hold one, as a chunk's cannot.
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
