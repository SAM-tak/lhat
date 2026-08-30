// L^ (lhat) -- the Content-Length framing an LSP or DAP message travels in,
// over whatever carries the bytes. The framing is the Base Protocol both
// share (LSP 3.17, DAP 1.x); what a message says (JSON-RPC, or DAP's own
// envelope) and means is the caller's.
//
// A stream is a pair of byte functions and a context, so the same framing
// serves stdio (the language server) and a socket (the debug adapter) --
// and a memory buffer, which is how a test drives either without a pipe.

#ifndef LHAT_TRANSPORT_H
#define LHAT_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// `read` fills up to `size` bytes and answers how many, or 0 at end of
// input or on error -- it blocks for at least one byte otherwise, the way a
// socket recv or fread does. `write` sends all `size` bytes and answers
// whether it could. Neither is asked to be thread-safe; a caller sharing a
// stream between threads locks it itself (lsp/rpc.h).
typedef struct {
    void *context;
    size_t (*read)(void *context, char *buffer, size_t size);
    bool (*write)(void *context, const char *bytes, size_t size);
} LhatStream;

// A stream over a stdio file. The file is the caller's to close.
LhatStream lhat_stream_of_file(FILE *file);

// Puts stdin/stdout into binary mode on Windows, where text mode would
// otherwise turn a '\n' inside a body into "\r\n" and make the
// Content-Length lie. A no-op elsewhere.
void lhat_transport_use_binary_stdio(void);

// Reads one message's headers and body. `*out_body` is malloc'd,
// NUL-terminated, and `*out_length` its length without the NUL. false on
// end of input or a header the framing does not recognise -- either way the
// stream has lost sync and reading on is not meaningful.
bool lhat_transport_read_message(LhatStream *in, char **out_body,
                                 size_t *out_length);

// Writes the framing and `body`. The caller still owns `body`. false when
// the stream's write failed.
bool lhat_transport_write_message(LhatStream *out, const char *body,
                                  size_t length);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_TRANSPORT_H
