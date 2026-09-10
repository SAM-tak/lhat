// L^ (lhat) -- LSP server: the read loop and the state one server holds.

#include "server.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "host_config.h"
#include "transport.h"

void lsp_server_init(LspServer *server, FILE *out_stream)
{
    // No folders yet -- initialize.c replaces this once it has the client's
    // workspaceFolders. Anything arriving before "initialize" (which the
    // base protocol forbids except "exit") sees no project.
    lsp_workspace_init(&server->workspace, NULL, 0);
    lsp_rpc_out_init(&server->out, out_stream);
    lsp_queue_init(&server->queue);
    server->worker_started = false;
    server->shutdown_requested = false;
    server->should_exit = false;
    server->published_paths = NULL;
    server->published_count = 0;
    server->published_capacity = 0;
}

void lsp_server_dispose(LspServer *server)
{
    lsp_queue_shutdown(&server->queue);
    if (server->worker_started) {
        lhat_thread_join(&server->worker_thread);
    }
    lsp_queue_dispose(&server->queue);
    lsp_rpc_out_dispose(&server->out);
    lsp_workspace_dispose(&server->workspace);

    // The join above already ensured the worker is done touching these.
    for (size_t i = 0; i < server->published_count; i++) {
        free(server->published_paths[i]);
    }
    free(server->published_paths);
}

void lsp_server_log(LspServer *server, LspLogLevel level, const char *text)
{
    cJSON *params = cJSON_CreateObject();
    if (params == NULL) {
        return;
    }
    cJSON_AddNumberToObject(params, "type", (int)level);
    cJSON_AddStringToObject(params, "message", text);
    lsp_rpc_send_notification(&server->out, "window/logMessage", params);
}

void lsp_server_mark_dirty(LspServer *server, const char *path)
{
    lsp_queue_mark_dirty(&server->queue, path);
}

int lsp_server_run(LspServer *server, FILE *in_stream)
{
    lhat_transport_use_binary_stdio();
    LhatStream in = lhat_stream_of_file(in_stream);

    char *body = NULL;
    size_t length = 0;
    while (!server->should_exit &&
           lhat_transport_read_message(&in, &body, &length)) {
        lsp_dispatch_message(server, body, length);
        free(body);
        body = NULL;
    }
    free(body);

    // LSP 3.17, Base Protocol: exit with 0 if shutdown was requested first,
    // 1 otherwise (an "exit" without a prior "shutdown", or the stream
    // simply closing on us).
    return server->shutdown_requested ? 0 : 1;
}
