// L^ (lhat) -- what the core asks of its surroundings.
//
// Two things the language needs but does not want to decide: where memory
// comes from, and how a unit's text is read. Both are declared here and
// defined in port/, which is a library of its own.
//
// **The seam is the linker, not a registration.** A host that wants its own
// allocator copies port/alloc.c into its build and leaves ours out; the
// core is linked against whichever is there. Nothing has to be set up before
// the first allocation, and there is no hook to forget to install.
//
// The trade is one implementation per binary. Lua's lua_newstate(alloc, ud)
// buys a per-state allocator with a pointer threaded through every call; that
// is worth it for a language whose states are many and different, and is not
// what this is. FreeRTOS makes the same choice this does, and for the same
// reason: an embedded host has one idea about memory.
//
// A shared build cannot use the seam -- a DLL is linked before the host sees
// it, so it carries the default. Replacing the allocator means building the
// core statically, or building the DLL with the replacement in it.

#ifndef LHAT_PORT_H
#define LHAT_PORT_H

#include <stddef.h>

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
//
// These stand where the C library's four would. `lhat_calloc` is separate
// rather than folded into `lhat_alloc` because 03 の 2.2 numbers
// LHAT_VALUE_NIL first with a zero payload, which makes zeroed memory a stack
// already full of nil^ -- the machine leans on that rather than filling one.

void *lhat_alloc(size_t size);
void *lhat_calloc(size_t count, size_t size);
void *lhat_realloc(void *pointer, size_t size);
void lhat_free(void *pointer);

// ---------------------------------------------------------------------------
// Reading a unit
// ---------------------------------------------------------------------------

// 05 の 5.1: the bytes at `path`, or NULL when there are none there. The
// caller frees the result with lhat_free. Normalising newlines and the BOM
// belongs to the source (01 の 1 章) and is not done here.
//
// A program may also be given a loader of its own with
// lhat_program_set_loader, which is the per-program seam; this is the one the
// default loader goes through, and the one a host replaces to change every
// program at once.
char *lhat_load_file(const char *path, size_t *length);

#endif  // LHAT_PORT_H
