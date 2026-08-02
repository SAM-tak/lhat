// L^ (lhat) -- where memory comes from.
//
// The C library, which is what a host without an opinion wants. Copy this
// file, change the four functions, leave lhatport out of the link, and the
// core uses yours instead -- see src/port.h for why the seam is here
// rather than in a registration call.
//
// Nothing else in the core calls malloc, so these four are the whole of it.

#include "port.h"

#include <stdlib.h>

void *lhat_alloc(size_t size)
{
    return malloc(size);
}

void *lhat_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void *lhat_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}

void lhat_free(void *pointer)
{
    free(pointer);
}
