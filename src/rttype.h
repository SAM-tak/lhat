// L^ (lhat) -- the bridge from the checker's types to the machine's.
//
// 03 の 1.3 keeps the two representations apart on purpose: check results
// are thrown away when checking ends, and compiling reads the tree
// independently. What crosses anyway crosses through 5.11a's checked_type
// stamp, and these two readings are the whole of what the compiler does
// with one. Internal to the library -- a host sees neither type.

#ifndef LHAT_RTTYPE_H
#define LHAT_RTTYPE_H

#include "object.h"
#include "type.h"

// Converts one of the checker's own LhatType objects into the shape
// lower_type builds from a written annotation. Used only where nothing was
// written at all -- a fallback onto what infer_func (check.c) settled, not
// a second opinion on what was written. Allocates on `heap`, which is the
// owning chunk's.
LhatRuntimeType *lhat_rt_from_checked(LhatHeap *heap, const LhatType *type);

// 13.13-aware: does an error hide anywhere in this checker type?
bool lhat_rt_mentions_error(const LhatType *type);

#endif  // LHAT_RTTYPE_H
