// L^ (lhat) -- LSP server: method name -> handler, table-driven.

#include "dispatch.h"

#include <stdbool.h>
#include <string.h>

#include "rpc.h"
#include "server.h"

void lsp_dispatch_message(LspServer *server, const char *body, size_t length)
{
    (void)length;
    cJSON *message = cJSON_Parse(body);
    if (message == NULL) {
        return;  // not JSON at all; nothing to answer and no id to answer with
    }

    const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(message, "method");
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(message, "id");
    const cJSON *params_item = cJSON_GetObjectItemCaseSensitive(message, "params");

    if (!cJSON_IsString(method_item)) {
        cJSON_Delete(message);
        return;
    }
    const char *method = method_item->valuestring;
    bool is_request = id_item != NULL && !cJSON_IsNull(id_item);

    if (is_request) {
        for (const LspRequestEntry *e = LSP_REQUEST_TABLE; e->method != NULL; e++) {
            if (strcmp(e->method, method) == 0) {
                cJSON *result = e->handler(server, params_item);
                lsp_rpc_send_response(&server->out, id_item, result);
                cJSON_Delete(message);
                return;
            }
        }
        lsp_rpc_send_error(&server->out, id_item, -32601, "method not found");
    } else {
        for (const LspNotificationEntry *e = LSP_NOTIFICATION_TABLE;
             e->method != NULL; e++) {
            if (strcmp(e->method, method) == 0) {
                e->handler(server, params_item);
                break;
            }
        }
        // An unrecognised notification is dropped, per the base protocol.
    }
    cJSON_Delete(message);
}
