// L^ (lhat) -- LSP server: sending a JSON-RPC message.

#include "rpc.h"

#include <string.h>

#include "transport.h"

void lsp_rpc_out_init(LspRpcOut *out, FILE *stream)
{
    out->out = lhat_stream_of_file(stream);
    lhat_mutex_init(&out->lock);
}

void lsp_rpc_out_dispose(LspRpcOut *out)
{
    lhat_mutex_destroy(&out->lock);
}

static void send_envelope(LspRpcOut *out, cJSON *envelope)
{
    char *text = cJSON_PrintUnformatted(envelope);
    if (text != NULL) {
        lhat_mutex_lock(&out->lock);
        lhat_transport_write_message(&out->out, text, strlen(text));
        lhat_mutex_unlock(&out->lock);
        cJSON_free(text);
    }
    cJSON_Delete(envelope);
}

void lsp_rpc_send_response(LspRpcOut *out, const cJSON *id, cJSON *result)
{
    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "jsonrpc", "2.0");
    cJSON_AddItemToObject(envelope, "id",
        id != NULL ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON_AddItemToObject(envelope, "result",
        result != NULL ? result : cJSON_CreateNull());
    send_envelope(out, envelope);
}

void lsp_rpc_send_error(LspRpcOut *out, const cJSON *id, int code,
                        const char *message)
{
    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "jsonrpc", "2.0");
    cJSON_AddItemToObject(envelope, "id",
        id != NULL ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(envelope, "error", error);
    send_envelope(out, envelope);
}

void lsp_rpc_send_notification(LspRpcOut *out, const char *method,
                               cJSON *params)
{
    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "jsonrpc", "2.0");
    cJSON_AddStringToObject(envelope, "method", method);
    cJSON_AddItemToObject(envelope, "params",
        params != NULL ? params : cJSON_CreateNull());
    send_envelope(out, envelope);
}
