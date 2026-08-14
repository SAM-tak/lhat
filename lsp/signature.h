// L^ (lhat) -- LSP server: the type at a position, as text to keep.
//
// A hover would seem to answer this already, but it does not. What it
// returns is Markdown -- the line as written, the type under it, the
// comments beside it -- and the copy an editor offers on a hover takes all
// of it; neither that button nor what it copies is the server's to change.
// What a reader wants to paste into an annotation, or into a host
// registration (05 の 8.7 reads the same grammar back), is the type alone.
//
// The two also differ in how much they say. A hover's type is elided
// (config.h's depth and item limits) because a shorter answer reads better
// in a popup; one being kept has to be whole. So this is its own answer
// rather than something teased back out of the hover's Markdown.

#ifndef LSP_SIGNATURE_H
#define LSP_SIGNATURE_H

#include <stdint.h>

#include "program_internal.h"

// The type at `offset`, written out whole (lhat_type_write_full). Malloc'd;
// the caller frees it. NULL when nothing there names a type.
//
// Answers at a declaration as well as at a use, which is where someone
// wanting to copy a signature is most likely to be standing.
char *lsp_signature_for_unit(const LhatUnit *unit, uint32_t offset);

#endif  // LSP_SIGNATURE_H
