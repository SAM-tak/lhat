// L^ (lhat) -- LSP server: sending a JSON-RPC message.
//
// stdout is shared between the main thread (responses to initialize and
// shutdown) and the worker thread (publishDiagnostics notifications), so
// writing it is the one place this server needs a lock outside workspace.h.

#ifndef LSP_RPC_H
#define LSP_RPC_H

#include <stdio.h>

#include "port/thread.h"
#include "transport.h"

#include "cJSON.h"

typedef struct {
    LhatStream out;
    LhatMutex lock;
} LspRpcOut;

void lsp_rpc_out_init(LspRpcOut *out, FILE *stream);
void lsp_rpc_out_dispose(LspRpcOut *out);

// {jsonrpc, id, result}. Takes ownership of `result` (may be NULL for a
// JSON null). `id` is copied, not owned.
void lsp_rpc_send_response(LspRpcOut *out, const cJSON *id, cJSON *result);

// {jsonrpc, id, error: {code, message}}.
void lsp_rpc_send_error(LspRpcOut *out, const cJSON *id, int code,
                        const char *message);

// {jsonrpc, method, params} -- a notification, with no id. Takes ownership
// of `params` (may be NULL for a JSON null).
void lsp_rpc_send_notification(LspRpcOut *out, const char *method,
                               cJSON *params);

#endif  // LSP_RPC_H
