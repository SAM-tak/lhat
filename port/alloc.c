// L^ (lhat) -- where memory comes from.
//
// Two ways to change it, because one of them cannot always be used.
//
// **Replace this file.** Copy it, write the four functions however you like,
// and leave lhatport out of your link. Nothing is registered, nothing can be
// forgotten, and there is no indirection at all. This is the way for a static
// build, and it is why src/port.h declares plain functions.
//
// **Call lhat_set_allocator.** A shared build is linked before the host sees
// it and carries this file inside, so the linker seam is out of reach there.
// The table below is what makes that case work; the cost is one indirect call
// per allocation, which a static host does not pay because it took the other
// way.
//
// The order that matters is the one a linker seam does not have: the table
// has to be set before anything is taken. That is checkable, so it is
// checked -- see lhat_set_allocator.

#include "port.h"

#include <stdlib.h>

static LhatAllocator registered;
static bool have_registered;
static bool have_allocated;

bool lhat_set_allocator(const LhatAllocator *allocator)
{
    // 05 の 8.9: what is already held was taken from somewhere else, and
    // handing it to a different free is worse than refusing.
    if (have_allocated) {
        return false;
    }
    if (allocator == NULL) {
        have_registered = false;
        return true;
    }
    if (allocator->alloc == NULL || allocator->calloc == NULL ||
        allocator->realloc == NULL || allocator->free == NULL) {
        return false;  // all four or none: the core uses all four
    }
    registered = *allocator;
    have_registered = true;
    return true;
}

void *lhat_alloc(size_t size)
{
    have_allocated = true;
    if (have_registered) {
        return registered.alloc(registered.context, size);
    }
    return malloc(size);
}

void *lhat_calloc(size_t count, size_t size)
{
    have_allocated = true;
    if (have_registered) {
        return registered.calloc(registered.context, count, size);
    }
    return calloc(count, size);
}

void *lhat_realloc(void *pointer, size_t size)
{
    have_allocated = true;
    if (have_registered) {
        return registered.realloc(registered.context, pointer, size);
    }
    return realloc(pointer, size);
}

void lhat_free(void *pointer)
{
    if (have_registered) {
        registered.free(registered.context, pointer);
        return;
    }
    free(pointer);
}
