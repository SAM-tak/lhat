// L^ (lhat) -- LSP server: the editor's own copy of an open file's text.

#include "document_store.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"  // lhat_alloc: lsp_document_store_copy feeds LhatProgramLoader,
                   // whose result program.c frees with lhat_free (program.c's
                   // check_path calls lhat_free(text) right after handing it
                   // to lhat_source_init_from_string)
#include "util.h"

void lsp_document_store_init(LspDocumentStore *store)
{
    store->open = NULL;
    lhat_mutex_init(&store->lock);
}

void lsp_document_store_dispose(LspDocumentStore *store)
{
    LspDocument *doc = store->open;
    while (doc != NULL) {
        LspDocument *next = doc->next;
        free(doc->path);
        free(doc->text);
        free(doc);
        doc = next;
    }
    store->open = NULL;
    lhat_mutex_destroy(&store->lock);
}

static LspDocument *find_locked(LspDocumentStore *store, const char *path)
{
    for (LspDocument *doc = store->open; doc != NULL; doc = doc->next) {
        if (strcmp(doc->path, path) == 0) {
            return doc;
        }
    }
    return NULL;
}

void lsp_document_store_put(LspDocumentStore *store, const char *path,
                            char *text, size_t length, int version)
{
    lhat_mutex_lock(&store->lock);
    LspDocument *doc = find_locked(store, path);
    if (doc != NULL) {
        free(doc->text);
        doc->text = text;
        doc->length = length;
        doc->version = version;
    } else {
        doc = (LspDocument *)malloc(sizeof *doc);
        if (doc != NULL) {
            doc->path = lsp_strdup(path);
            doc->text = text;
            doc->length = length;
            doc->version = version;
            doc->next = store->open;
            store->open = doc;
        }
    }
    lhat_mutex_unlock(&store->lock);
}

void lsp_document_store_remove(LspDocumentStore *store, const char *path)
{
    lhat_mutex_lock(&store->lock);
    LspDocument **link = &store->open;
    while (*link != NULL) {
        if (strcmp((*link)->path, path) == 0) {
            LspDocument *dead = *link;
            *link = dead->next;
            free(dead->path);
            free(dead->text);
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    lhat_mutex_unlock(&store->lock);
}

char *lsp_document_store_copy(LspDocumentStore *store, const char *path,
                              size_t *out_length)
{
    lhat_mutex_lock(&store->lock);
    LspDocument *doc = find_locked(store, path);
    char *copy = NULL;
    if (doc != NULL) {
        // lhat_alloc, not malloc: this feeds a LhatProgramLoader result, and
        // program.c frees what the loader returns with lhat_free.
        copy = (char *)lhat_alloc(doc->length + 1);
        if (copy != NULL) {
            memcpy(copy, doc->text, doc->length + 1);
            *out_length = doc->length;
        }
    }
    lhat_mutex_unlock(&store->lock);
    return copy;
}

bool lsp_document_store_has(LspDocumentStore *store, const char *path)
{
    lhat_mutex_lock(&store->lock);
    bool found = find_locked(store, path) != NULL;
    lhat_mutex_unlock(&store->lock);
    return found;
}
