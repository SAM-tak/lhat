// L^ (lhat) -- the DAP request/response/event envelope.

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

cJSON *dap_read(DapPeer *peer)
{
    char *body = NULL;
    size_t length = 0;
    if (!lhat_transport_read_message(&peer->stream, &body, &length)) {
        return NULL;
    }
    cJSON *message = cJSON_Parse(body);
    free(body);
    return message;
}

static bool send_message(DapPeer *peer, cJSON *message)
{
    cJSON_AddNumberToObject(message, "seq", peer->seq++);
    char *text = cJSON_PrintUnformatted(message);
    bool ok = false;
    if (text != NULL) {
        ok = lhat_transport_write_message(&peer->stream, text, strlen(text));
        cJSON_free(text);
    }
    cJSON_Delete(message);
    return ok;
}

static bool send_response(DapPeer *peer, const cJSON *request, bool success,
                          cJSON *body, const char *why)
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "response");
    const cJSON *request_seq = cJSON_GetObjectItem(request, "seq");
    cJSON_AddNumberToObject(message, "request_seq",
                            request_seq != NULL ? request_seq->valuedouble : 0);
    cJSON_AddBoolToObject(message, "success", success);
    cJSON_AddStringToObject(message, "command", dap_command(request));
    if (why != NULL) {
        cJSON_AddStringToObject(message, "message", why);
    }
    if (body != NULL) {
        cJSON_AddItemToObject(message, "body", body);
    }
    return send_message(peer, message);
}

bool dap_respond(DapPeer *peer, const cJSON *request, bool success,
                 cJSON *body)
{
    return send_response(peer, request, success, body, NULL);
}

bool dap_fail(DapPeer *peer, const cJSON *request, const char *message)
{
    return send_response(peer, request, false, NULL, message);
}

bool dap_event(DapPeer *peer, const char *event, cJSON *body)
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "event");
    cJSON_AddStringToObject(message, "event", event);
    if (body != NULL) {
        cJSON_AddItemToObject(message, "body", body);
    }
    return send_message(peer, message);
}

const char *dap_command(const cJSON *message)
{
    const cJSON *command = cJSON_GetObjectItem(message, "command");
    return cJSON_IsString(command) ? command->valuestring : "";
}

const cJSON *dap_arguments(const cJSON *message)
{
    return cJSON_GetObjectItem(message, "arguments");
}
