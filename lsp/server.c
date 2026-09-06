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

void lsp_server_load_host_config(LspServer *server)
{
    char *looked_at = NULL;
    LspConfigOutcome outcome =
        lsp_workspace_load_host_config(&server->workspace, &looked_at);

    char message[1024];
    switch (outcome) {
        case LSP_CONFIG_READ: {
            size_t types = 0;
            size_t functions = 0;
            size_t annotations = 0;
            lsp_host_config_counts(server->workspace.host_config, &types,
                                   &functions, &annotations);
            snprintf(message, sizeof message,
                     "host API read from %s: %zu types, %zu functions, "
                     "%zu annotations",
                     looked_at != NULL ? looked_at : "(nowhere)", types,
                     functions, annotations);
            lsp_server_log(server, LSP_LOG_INFO, message);
            break;
        }
        case LSP_CONFIG_ABSENT:
            // The one that had to be said out loud: without it, 05 の 8.7
            // leaves import^ nothing to reach, and every unit that imports
            // reports against its own first line rather than against this.
            snprintf(message, sizeof message,
                     "no host API at %s -- import^ will reach nothing the "
                     "host registered. `lhat --dump-host-api > lhat-host.json` "
                     "writes what the CLI registers.",
                     looked_at != NULL ? looked_at : "(nowhere)");
            lsp_server_log(server, LSP_LOG_WARNING, message);
            break;
        case LSP_CONFIG_UNREADABLE:
            snprintf(message, sizeof message,
                     "host API at %s could not be read as JSON -- carrying on "
                     "with nothing registered.",
                     looked_at != NULL ? looked_at : "(nowhere)");
            lsp_server_log(server, LSP_LOG_ERROR, message);
            break;
        case LSP_CONFIG_NO_ROOT:
            // 05 の 8.7 again, but nothing is wrong: a single file opened
            // with no folder around it has nowhere a config could sit.
            lsp_server_log(server, LSP_LOG_INFO,
                           "no workspace folder, so no lhat-host.json was "
                           "looked for -- import^ will reach nothing the host "
                           "registered.");
            break;
    }
    free(looked_at);
}

// 8.1: force_include_files names files rather than matching them, so a
// mistyped name matches nothing and would otherwise be silent -- the whole
// point of naming one is that it should be checked, and nothing else says
// it is not. Said once per load, naming the file.
static void say_what_was_named_but_absent(LspServer *server)
{
    const LspSettings *settings = server->workspace.settings;
    for (size_t i = 0; i < lsp_settings_force_count(settings); i++) {
        char *path = lsp_workspace_path_under_root(
            &server->workspace, lsp_settings_force_at(settings, i));
        if (path == NULL) {
            continue;
        }
        FILE *file = fopen(path, "rb");
        if (file != NULL) {
            fclose(file);
        } else {
            char message[1024];
            snprintf(message, sizeof message,
                     "force_include_files names %s, which is not there -- "
                     "nothing is checked for it.",
                     path);
            lsp_server_log(server, LSP_LOG_WARNING, message);
        }
        free(path);
    }
}

void lsp_server_load_settings(LspServer *server)
{
    char *looked_at = NULL;
    LspConfigOutcome outcome =
        lsp_workspace_load_settings(&server->workspace, &looked_at);

    char message[1024];
    switch (outcome) {
        case LSP_CONFIG_READ:
            snprintf(message, sizeof message,
                     "project settings read from %s: %zu exclude patterns, "
                     "%zu files named to keep",
                     looked_at != NULL ? looked_at : "(nowhere)",
                     lsp_settings_exclude_count(server->workspace.settings),
                     lsp_settings_force_count(server->workspace.settings));
            lsp_server_log(server, LSP_LOG_INFO, message);
            say_what_was_named_but_absent(server);
            break;
        case LSP_CONFIG_ABSENT:
        case LSP_CONFIG_NO_ROOT:
            // Nothing is wrong, and nothing is missed: every file under the
            // root is checked and lhat-host.json decides how strictly. The
            // host config's absence is worth a warning because it breaks
            // every import^; this one's is the ordinary case, and saying so
            // in every workspace that does not need the file is noise.
            break;
        case LSP_CONFIG_UNREADABLE:
            // Named with what it falls back to, so a mistyped file and the
            // errors that did not go away are visibly the same event.
            snprintf(message, sizeof message,
                     "project settings at %s could not be read as JSON -- "
                     "nothing is excluded and \"strict\" is the host "
                     "config's.",
                     looked_at != NULL ? looked_at : "(nowhere)");
            lsp_server_log(server, LSP_LOG_ERROR, message);
            break;
    }
    free(looked_at);
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
