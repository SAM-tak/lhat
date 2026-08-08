// L^ (lhat) -- LSP server: the dirty-path queue the worker drains.

#include "queue.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void lsp_queue_init(LspRecheckQueue *queue)
{
    queue->paths = NULL;
    queue->count = 0;
    queue->capacity = 0;
    queue->shutdown = false;
    lhat_mutex_init(&queue->lock);
    lhat_condition_init(&queue->signal);
}

void lsp_queue_dispose(LspRecheckQueue *queue)
{
    for (size_t i = 0; i < queue->count; i++) {
        free(queue->paths[i]);
    }
    free(queue->paths);
    lhat_mutex_destroy(&queue->lock);
    lhat_condition_destroy(&queue->signal);
}

void lsp_queue_mark_dirty(LspRecheckQueue *queue, const char *path)
{
    lhat_mutex_lock(&queue->lock);
    for (size_t i = 0; i < queue->count; i++) {
        if (strcmp(queue->paths[i], path) == 0) {
            lhat_mutex_unlock(&queue->lock);
            return;  // already pending
        }
    }
    if (queue->count == queue->capacity) {
        size_t grown = queue->capacity ? queue->capacity * 2 : 8;
        char **bigger = (char **)realloc(queue->paths, grown * sizeof *bigger);
        if (bigger == NULL) {
            lhat_mutex_unlock(&queue->lock);
            return;
        }
        queue->paths = bigger;
        queue->capacity = grown;
    }
    queue->paths[queue->count++] = lsp_strdup(path);
    lhat_condition_signal(&queue->signal);
    lhat_mutex_unlock(&queue->lock);
}

size_t lsp_queue_wait_and_drain(LspRecheckQueue *queue, char ***out_paths,
                                int debounce_ms)
{
    lhat_mutex_lock(&queue->lock);
    while (queue->count == 0 && !queue->shutdown) {
        lhat_condition_wait(&queue->signal, &queue->lock);
    }
    if (queue->count == 0) {  // shutdown, and nothing left to drain
        lhat_mutex_unlock(&queue->lock);
        *out_paths = NULL;
        return 0;
    }
    lhat_mutex_unlock(&queue->lock);

    // Debounce: keep waiting in debounce_ms slices until one slice passes
    // with the queue unchanged -- typing that is still going on postpones
    // the drain rather than triggering one per keystroke.
    for (;;) {
        lhat_mutex_lock(&queue->lock);
        size_t before = queue->count;
        lhat_condition_wait_for(&queue->signal, &queue->lock, debounce_ms);
        size_t after = queue->count;
        lhat_mutex_unlock(&queue->lock);

        if (after == before) {
            break;
        }
    }

    lhat_mutex_lock(&queue->lock);
    size_t count = queue->count;
    char **drained = queue->paths;
    queue->paths = NULL;
    queue->count = 0;
    queue->capacity = 0;
    lhat_mutex_unlock(&queue->lock);

    *out_paths = drained;
    return count;
}

void lsp_queue_shutdown(LspRecheckQueue *queue)
{
    lhat_mutex_lock(&queue->lock);
    queue->shutdown = true;
    lhat_condition_broadcast(&queue->signal);
    lhat_mutex_unlock(&queue->lock);
}
