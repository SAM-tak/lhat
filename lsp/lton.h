// L^ (lhat) -- LSP server: a .lton file, checked as what it is.
//
// DesignDocuments/08-lton.md: an LTON file is the inside of a table literal
// and nothing else. It is not a unit and nothing require^s it, so the front
// end has nothing to say about it until it is wrapped -- which is what
// stdlib/lton.c does to read one, and what this does to check one.
//
// The wrapper is stdlib/lton.h's own, not a second spelling of it: two
// wrappings of one format would be two formats, and the editor would answer
// about a file the runtime never reads.
//
// WHERE THE POSITIONS GO. The prologue is one line long and the file's own
// bytes follow it on that same line, so line N of the file is line N of the
// wrapped text and only the first line's columns move -- by exactly the
// prologue's length. Everything the server hands an editor goes through
// here, and everything an editor hands the server comes back through it.

#ifndef LSP_LTON_H
#define LSP_LTON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "program_internal.h"

// Whether `path` names an LTON file -- the one thing that decides all of
// this, since a .lton is data and carries no marker of its own.
bool lsp_lton_is_path(const char *path);

// `text` inside the wrapper (stdlib/lton.h). Malloc'd; the caller frees it,
// and `out_length` is the whole of it. NULL if there is no room.
char *lsp_lton_wrap(const char *text, size_t length, size_t *out_length);

// How far the first line's columns moved: the prologue's length in bytes.
uint32_t lsp_lton_prologue_length(void);

#endif  // LSP_LTON_H
