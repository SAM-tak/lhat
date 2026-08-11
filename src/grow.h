// L^ (lhat) -- the one way a growable array grows.
//
// The same five lines were written at every append site: full? double the
// capacity (or start it), realloc, bail on refusal, store both back. The
// macro keeps the site's own failure statement -- what to return differs
// per caller -- and touches nothing when the memory is refused, so the
// array stays usable at its old size.
//
// The two-array grow in code.c's emit and the hash migration in object.c
// keep their own shapes on purpose: their policies (partial-growth retry,
// rehash-on-grow) are not this one.

#ifndef LHAT_GROW_H
#define LHAT_GROW_H

#include "lhat/port.h"

#define LHAT_GROW(array, count, capacity, first, fail_stmt)              \
    do {                                                                 \
        if ((count) == (capacity)) {                                     \
            size_t lhat_grown_ = (capacity) ? (capacity) * 2 : (first);  \
            void *lhat_bigger_ =                                         \
                lhat_realloc((array), lhat_grown_ * sizeof *(array));    \
            if (lhat_bigger_ == NULL) {                                  \
                fail_stmt;                                               \
            }                                                            \
            (array) = lhat_bigger_;                                      \
            (capacity) = lhat_grown_;                                    \
        }                                                                \
    } while (0)

#endif  // LHAT_GROW_H
