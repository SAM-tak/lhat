// L^ (lhat) -- LSP server: the Content-Length framing every LSP message
// travels in over stdio, independent of what the message says (JSON-RPC) or
// means (LSP). Base Protocol, LSP specification 3.17.

#ifndef LSP_TRANSPORT_H
#define LSP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Puts stdin/stdout into binary mode on Windows, where the C runtime's text
// mode would otherwise turn a '\n' inside a JSON body into "\r\n" and make
// the Content-Length lie about how many bytes follow. A no-op elsewhere.
void lsp_transport_use_binary_stdio(void);

// Reads one message's headers and body from `in`. `*out_body` is malloc'd,
// NUL-terminated, and `*out_length` is its length without the NUL. false on
// EOF or a header the framing does not recognise -- either way the stream
// has lost sync and reading further is not meaningful.
bool lsp_transport_read_message(FILE *in, char **out_body, size_t *out_length);

// Writes the framing and `body` to `out` and flushes. The caller still owns
// `body`.
void lsp_transport_write_message(FILE *out, const char *body, size_t length);

#endif  // LSP_TRANSPORT_H
