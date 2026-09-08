// L^ (lhat) -- LSP server: textDocument/references and textDocument/rename.

#ifndef LSP_HANDLERS_REFERENCES_H
#define LSP_HANDLERS_REFERENCES_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_references(LspServer *server, const cJSON *params);
cJSON *lsp_handle_prepare_rename(LspServer *server, const cJSON *params);
cJSON *lsp_handle_rename(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_REFERENCES_H
