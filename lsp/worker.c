// L^ (lhat) -- LSP server: the one thread that ever calls lhat_program_check.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "port/thread.h"

#include "diagnostics.h"
#include "queue.h"
#include "rpc.h"
#include "server.h"
#include "uri.h"
#include "util.h"
#include "workspace.h"

#define LSP_DEBOUNCE_MS 250

typedef struct {
    LspServer *server;
    char **current;  // paths a diagnostic was published for this round
    size_t current_count;
    size_t current_capacity;
} PublishState;

static void remember_current(PublishState *state, const char *path)
{
    if (state->current_count == state->current_capacity) {
        size_t grown = state->current_capacity ? state->current_capacity * 2 : 16;
        char **bigger =
            (char **)realloc(state->current, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        state->current = bigger;
        state->current_capacity = grown;
    }
    state->current[state->current_count++] = lsp_strdup(path);
}

static void send_diagnostics(LspRpcOut *out, const char *path, cJSON *diagnostics)
{
    char *uri = lsp_absolute_path_to_uri(path);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    cJSON_AddItemToObject(params, "diagnostics", diagnostics);
    free(uri);
    lsp_rpc_send_notification(out, "textDocument/publishDiagnostics", params);
}

static void publish_sink(void *context, const char *path, cJSON *diagnostics)
{
    PublishState *state = (PublishState *)context;
    remember_current(state, path);
    send_diagnostics(&state->server->out, path, diagnostics);
}

// Re-collects diagnostics for the whole workspace and publishes one
// notification per path that has any, then clears (sends an empty array
// for) anything published last round that did not show up this round --
// otherwise a fixed error would leave a stale red squiggle behind.
void lsp_server_publish_diagnostics(LspServer *server)
{
    PublishState state;
    state.server = server;
    state.current = NULL;
    state.current_count = 0;
    state.current_capacity = 0;

    lsp_workspace_collect_diagnostics(&server->workspace, publish_sink, &state);

    // published_paths is worker-thread-owned (server.h), so no lock here.
    for (size_t i = 0; i < server->published_count; i++) {
        bool still_current = false;
        for (size_t j = 0; j < state.current_count; j++) {
            if (strcmp(server->published_paths[i], state.current[j]) == 0) {
                still_current = true;
                break;
            }
        }
        if (!still_current) {
            send_diagnostics(&server->out, server->published_paths[i],
                             cJSON_CreateArray());
        }
        free(server->published_paths[i]);
    }
    free(server->published_paths);
    server->published_paths = state.current;
    server->published_count = state.current_count;
    server->published_capacity = state.current_capacity;
}

static int worker_main(void *arg)
{
    LspServer *server = (LspServer *)arg;

    // The initial check is every discovered root at once (recheck_all)
    // rather than one queue entry at a time: recheck_affected's own reach
    // is transitive (05 の 6.2's require^ walk), so draining a freshly
    // discovered workspace one root at a time would re-check an already
    // up-to-date root again each time a root that depends on it came up
    // next in the same batch.
    lsp_workspace_recheck_all(&server->workspace);
    lsp_server_publish_diagnostics(server);

    for (;;) {
        char **paths = NULL;
        size_t count =
            lsp_queue_wait_and_drain(&server->queue, &paths, LSP_DEBOUNCE_MS);
        if (count == 0 && paths == NULL) {
            break;  // lsp_queue_shutdown, nothing left pending
        }

        // lhat-host.json in the batch means the registrations changed under
        // every root at once, so the batch's per-path rechecks would be
        // stale before they finished -- reload and re-check everything
        // instead, which subsumes them.
        bool config_changed = false;
        for (size_t i = 0; i < count; i++) {
            if (lsp_workspace_is_host_config_path(&server->workspace,
                                                  paths[i])) {
                config_changed = true;
                break;
            }
        }
        if (config_changed) {
            lsp_server_load_host_config(server);
            lsp_workspace_recheck_all(&server->workspace);
        } else {
            for (size_t i = 0; i < count; i++) {
                lsp_workspace_recheck_affected(&server->workspace, paths[i]);
            }
        }
        for (size_t i = 0; i < count; i++) {
            free(paths[i]);
        }
        free(paths);
        lsp_server_publish_diagnostics(server);
    }
    return 0;
}

void lsp_server_start_worker(LspServer *server)
{
    if (server->worker_started) {
        return;
    }
    if (lhat_thread_start(&server->worker_thread, worker_main, server)) {
        server->worker_started = true;
    }
}
