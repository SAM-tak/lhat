// L^ (lhat) -- LSP server: the dirty-path queue the worker drains.

#include "queue.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

void lsp_queue_init(LspRecheckQueue *queue)
{
    queue->paths = NULL;
    queue->count = 0;
    queue->capacity = 0;
    queue->shutdown = false;
    mtx_init(&queue->lock, mtx_plain);
    cnd_init(&queue->signal);
}

void lsp_queue_dispose(LspRecheckQueue *queue)
{
    for (size_t i = 0; i < queue->count; i++) {
        free(queue->paths[i]);
    }
    free(queue->paths);
    mtx_destroy(&queue->lock);
    cnd_destroy(&queue->signal);
}

void lsp_queue_mark_dirty(LspRecheckQueue *queue, const char *path)
{
    mtx_lock(&queue->lock);
    for (size_t i = 0; i < queue->count; i++) {
        if (strcmp(queue->paths[i], path) == 0) {
            mtx_unlock(&queue->lock);
            return;  // already pending
        }
    }
    if (queue->count == queue->capacity) {
        size_t grown = queue->capacity ? queue->capacity * 2 : 8;
        char **bigger = (char **)realloc(queue->paths, grown * sizeof *bigger);
        if (bigger == NULL) {
            mtx_unlock(&queue->lock);
            return;
        }
        queue->paths = bigger;
        queue->capacity = grown;
    }
    queue->paths[queue->count++] = lsp_strdup(path);
    cnd_signal(&queue->signal);
    mtx_unlock(&queue->lock);
}

static void add_millis(struct timespec *ts, int millis)
{
    ts->tv_nsec += (long)millis * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

size_t lsp_queue_wait_and_drain(LspRecheckQueue *queue, char ***out_paths,
                                int debounce_ms)
{
    mtx_lock(&queue->lock);
    while (queue->count == 0 && !queue->shutdown) {
        cnd_wait(&queue->signal, &queue->lock);
    }
    if (queue->count == 0) {  // shutdown, and nothing left to drain
        mtx_unlock(&queue->lock);
        *out_paths = NULL;
        return 0;
    }
    mtx_unlock(&queue->lock);

    // Debounce: keep waiting in debounce_ms slices until one slice passes
    // with the queue unchanged -- typing that is still going on postpones
    // the drain rather than triggering one per keystroke.
    for (;;) {
        mtx_lock(&queue->lock);
        size_t before = queue->count;
        struct timespec deadline;
        timespec_get(&deadline, TIME_UTC);
        add_millis(&deadline, debounce_ms);
        cnd_timedwait(&queue->signal, &queue->lock, &deadline);
        size_t after = queue->count;
        mtx_unlock(&queue->lock);

        if (after == before) {
            break;
        }
    }

    mtx_lock(&queue->lock);
    size_t count = queue->count;
    char **drained = queue->paths;
    queue->paths = NULL;
    queue->count = 0;
    queue->capacity = 0;
    mtx_unlock(&queue->lock);

    *out_paths = drained;
    return count;
}

void lsp_queue_shutdown(LspRecheckQueue *queue)
{
    mtx_lock(&queue->lock);
    queue->shutdown = true;
    cnd_broadcast(&queue->signal);
    mtx_unlock(&queue->lock);
}
