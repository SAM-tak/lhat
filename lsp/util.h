// L^ (lhat) -- LSP server: small string helpers shared across the server.
//
// The server lives outside lhat.lib (like cli/main.c), so it allocates with
// the ordinary C library rather than lhat_alloc -- there is no port seam to
// keep here.

#ifndef LSP_UTIL_H
#define LSP_UTIL_H

#include <stddef.h>

// A malloc'd copy of `text`. NULL in, NULL out; out of memory, NULL too.
char *lsp_strdup(const char *text);

// The same, for a span that may not be NUL-terminated.
char *lsp_strndup(const char *text, size_t length);

#endif  // LSP_UTIL_H
