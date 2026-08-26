// L^ (lhat) -- LSP server: everything one running server holds.
//
// The main thread only ever does cheap things: read a frame, update
// document_store/workspace bookkeeping, mark a path dirty. lhat_program_check
// -- the only slow call in this server -- runs on the one worker thread
// (worker.c), so a big workspace never makes the server miss a keystroke's
// worth of stdin.

#ifndef LSP_SERVER_H
#define LSP_SERVER_H

#include <stdbool.h>
#include <stdio.h>

#include "port/thread.h"

#include "queue.h"
#include "rpc.h"
#include "workspace.h"

typedef struct LspServer {
    LspWorkspace workspace;
    LspRpcOut out;
    LspRecheckQueue queue;

    LhatThread worker_thread;
    bool worker_started;

    bool shutdown_requested;  // "shutdown" request handled; only "exit" may follow
    bool should_exit;         // "exit" notification seen; lsp_server_run returns

    // Paths this server last sent a non-empty Diagnostic[] for, so a path
    // that has gone clean since is sent an empty array to clear it rather
    // than being silently left with a stale red squiggle. Touched only by
    // the worker thread (lsp_server_publish_diagnostics, worker.c), so
    // unlike workspace/document_store this needs no lock of its own.
    char **published_paths;
    size_t published_count;
    size_t published_capacity;
} LspServer;

void lsp_server_init(LspServer *server, FILE *out_stream);
void lsp_server_dispose(LspServer *server);

// window/logMessage's levels, as the spec numbers them. What this server
// says goes to the editor's own output view for the server, which is where
// a reader looks to find out what it did rather than what it found -- a
// diagnostic is about the code, and this is about the server.
typedef enum {
    LSP_LOG_ERROR = 1,
    LSP_LOG_WARNING = 2,
    LSP_LOG_INFO = 3,
} LspLogLevel;

// Sends one window/logMessage. Safe from either thread: it goes through the
// same lock every other write to stdout does (rpc.h).
void lsp_server_log(LspServer *server, LspLogLevel level, const char *text);

// Reads the workspace's lhat-host.json and says on the log which file it
// read, or that there was nothing at the path it tried -- 05 の 8.7's
// registrations are the whole of what import^ can reach, so a workspace
// missing them has every import^ fail, and the reason is not in the file
// being edited. Both call sites (initialize.c at startup, worker.c when the
// file itself changes) want the same sentence.
void lsp_server_load_host_config(LspServer *server);

// worker.c: starts the one recheck worker thread. Called once, from the
// "initialized" notification.
void lsp_server_start_worker(LspServer *server);

// Marks `path` (absolute, forward-slashed -- uri.h) dirty; the worker picks
// it up (queue.h's debounce). Never blocks on a recheck.
void lsp_server_mark_dirty(LspServer *server, const char *path);

// worker.c: re-collects diagnostics for the whole workspace and publishes
// them, clearing anything previously published that has gone clean.
void lsp_server_publish_diagnostics(LspServer *server);

// The read loop: reads and dispatches messages from `in_stream` until EOF or
// an "exit" notification. Returns the process exit code (LSP 3.17: 0 if
// shutdown was requested first, 1 otherwise).
int lsp_server_run(LspServer *server, FILE *in_stream);

#endif  // LSP_SERVER_H
