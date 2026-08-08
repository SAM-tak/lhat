// L^ (lhat) -- LSP server: the editor's own copy of an open file's text.
//
// Keyed the same way as everything else program.h touches in this server:
// an absolute, forward-slashed path (uri.h). workspace.c's loader
// (lsp_program_load) asks this store before falling back to disk, so an
// unsaved edit is what gets checked rather than what is on disk.

#ifndef LSP_DOCUMENT_STORE_H
#define LSP_DOCUMENT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "port/thread.h"

typedef struct LspDocument {
    char *path;   // absolute, '/'-separated; owned
    char *text;   // UTF-8, NUL-terminated; owned
    size_t length;
    int version;
    struct LspDocument *next;
} LspDocument;

typedef struct {
    LspDocument *open;
    LhatMutex lock;
} LspDocumentStore;

void lsp_document_store_init(LspDocumentStore *store);
void lsp_document_store_dispose(LspDocumentStore *store);

// didOpen/didChange. Takes ownership of `text` (must be malloc'd) -- the
// caller must not free or otherwise reuse it afterwards.
void lsp_document_store_put(LspDocumentStore *store, const char *path,
                            char *text, size_t length, int version);

// didClose.
void lsp_document_store_remove(LspDocumentStore *store, const char *path);

// A malloc'd copy of the open text at `path`, or NULL when nothing is open
// there -- the loader's cue to fall back to disk. *out_length is set only
// when the copy is non-NULL.
char *lsp_document_store_copy(LspDocumentStore *store, const char *path,
                              size_t *out_length);

bool lsp_document_store_has(LspDocumentStore *store, const char *path);

#endif  // LSP_DOCUMENT_STORE_H
