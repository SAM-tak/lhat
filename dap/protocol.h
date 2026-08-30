// L^ (lhat) -- the Debug Adapter Protocol envelope, over an LhatStream.
//
// DAP shares LSP's Content-Length framing (transport.h) but not its
// message shape: a DAP message is a request, a response or an event, each
// with its own `seq`, and lsp/rpc.c's fixed `jsonrpc: "2.0"` is not it. So
// this is the small layer that reads and writes those three, and cJSON is
// the body inside.

#ifndef LHAT_DAP_PROTOCOL_H
#define LHAT_DAP_PROTOCOL_H

#include <stdbool.h>

#include "transport.h"

#include "cJSON.h"

// A DAP peer: the stream both ways, and the sequence number every message
// this side sends carries (DAP numbers each direction from 1).
typedef struct {
    LhatStream stream;
    int seq;
} DapPeer;

// One message from the peer, parsed. NULL at end of input or on a body that
// is not JSON. The caller owns it (cJSON_Delete).
cJSON *dap_read(DapPeer *peer);

// A response to `request`, carrying `body` (owned, may be NULL).
bool dap_respond(DapPeer *peer, const cJSON *request, bool success,
                 cJSON *body);

// A failed response carrying why -- DAP's `message`, which the client shows.
bool dap_fail(DapPeer *peer, const cJSON *request, const char *message);

// An event with `body` (owned, may be NULL).
bool dap_event(DapPeer *peer, const char *event, cJSON *body);

// The command / event name of a message, or "" -- a borrowed pointer into
// the message.
const char *dap_command(const cJSON *message);
// Its `arguments` object, or NULL.
const cJSON *dap_arguments(const cJSON *message);

#endif  // LHAT_DAP_PROTOCOL_H
