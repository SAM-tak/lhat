// L^ (lhat) -- LSP server: the dirty-path queue the worker drains.
//
// didOpen/didChange/didClose mark a path dirty and return immediately --
// the main thread never runs lhat_program_check. The worker waits for the
// queue to go non-empty, then waits a little longer for typing to quiet
// down (debounce) before draining it in one batch, so a burst of keystrokes
// re-checks once rather than once per keystroke.

#ifndef LSP_QUEUE_H
#define LSP_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <threads.h>

typedef struct {
    char **paths;  // pending, deduplicated
    size_t count;
    size_t capacity;
    bool shutdown;
    mtx_t lock;
    cnd_t signal;
} LspRecheckQueue;

void lsp_queue_init(LspRecheckQueue *queue);
void lsp_queue_dispose(LspRecheckQueue *queue);

// Adds `path` if it is not already pending. Never blocks on the worker.
void lsp_queue_mark_dirty(LspRecheckQueue *queue, const char *path);

// Blocks until the queue is non-empty, then until `debounce_ms` has passed
// without the queue growing further, then hands over everything pending as
// a malloc'd array of malloc'd strings (the caller frees both). Returns 0
// with *out_paths set to NULL only when lsp_queue_shutdown was called and
// nothing was pending.
size_t lsp_queue_wait_and_drain(LspRecheckQueue *queue, char ***out_paths,
                                int debounce_ms);

// Wakes a blocked lsp_queue_wait_and_drain so the worker can exit.
void lsp_queue_shutdown(LspRecheckQueue *queue);

#endif  // LSP_QUEUE_H
