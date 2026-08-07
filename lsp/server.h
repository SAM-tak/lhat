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
#include <threads.h>

#include "queue.h"
#include "rpc.h"
#include "workspace.h"

typedef struct LspServer {
    LspWorkspace workspace;
    LspRpcOut out;
    LspRecheckQueue queue;

    thrd_t worker_thread;
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
