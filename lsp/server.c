// L^ (lhat) -- LSP server: the read loop and the state one server holds.

#include "server.h"

#include <stdlib.h>

#include "dispatch.h"
#include "transport.h"

void lsp_server_init(LspServer *server, FILE *out_stream)
{
    // No root yet -- initialize.c's handler replaces this once it knows
    // rootUri/workspaceFolders. Anything arriving before "initialize" (which
    // the base protocol forbids except "exit") would see an empty workspace.
    lsp_workspace_init(&server->workspace, NULL);
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

void lsp_server_mark_dirty(LspServer *server, const char *path)
{
    lsp_queue_mark_dirty(&server->queue, path);
}

int lsp_server_run(LspServer *server, FILE *in_stream)
{
    lsp_transport_use_binary_stdio();

    char *body = NULL;
    size_t length = 0;
    while (!server->should_exit &&
           lsp_transport_read_message(in_stream, &body, &length)) {
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
