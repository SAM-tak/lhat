// L^ (lhat) -- the bridge from the checker's types to the machine's.
//
// 03 の 1.3 keeps the two representations apart on purpose: check results
// are thrown away when checking ends, and compiling reads the tree
// independently. What crosses anyway crosses through 5.11a's checked_type
// stamp, and these two readings are the whole of what the compiler does
// with one. Internal to the library -- a host sees neither type.

#ifndef LHAT_RTTYPE_H
#define LHAT_RTTYPE_H

#include "lhat/object.h"
#include "type.h"

// What one compile has already converted onto one heap. A definition's
// descriptor names every signature it carries, and each of those names the
// definition again, so converting every parameter and signature afresh
// rebuilds the same trees over and over, until they outweigh everything else
// the compile made. rttype.c says when an earlier answer stands for a later
// one.
typedef struct LhatRtCache {
    LhatHeap *heap;
    struct LhatRtNode *nodes;  // by type address, open addressing
    size_t node_count;
    size_t node_capacity;
    // Tarjan's components of the checker's type graph, found as needed.
    const LhatType **stack;
    size_t stack_count;
    size_t stack_capacity;
    size_t next_index;
    const LhatType **components;  // one type of each; the rest are linked
    size_t component_count;
    size_t component_capacity;
    struct LhatRtReach *reach;  // (component, component) -> one reaches the other
    size_t reach_count;
    size_t reach_capacity;
    bool broken;  // out of memory once: convert without remembering
} LhatRtCache;

void lhat_rt_cache_init(LhatRtCache *cache, LhatHeap *heap);
void lhat_rt_cache_dispose(LhatRtCache *cache);

// Converts one of the checker's own LhatType objects into the shape
// lower_type builds from a written annotation. Used only where nothing was
// written at all -- a fallback onto what infer_func (check.c) settled, not
// a second opinion on what was written. Allocates on `heap`, which is the
// owning chunk's. `cache` may be NULL, and is not used for any other heap
// than its own.
LhatRuntimeType *lhat_rt_from_checked(LhatHeap *heap, const LhatType *type,
                                      LhatRtCache *cache);

// 13.13-aware: does an error hide anywhere in this checker type?
bool lhat_rt_mentions_error(const LhatType *type);

#endif  // LHAT_RTTYPE_H
