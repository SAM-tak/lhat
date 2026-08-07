// L^ (lhat) -- LSP server: didOpen / didChange / didClose.

#ifndef LSP_HANDLERS_TEXT_DOCUMENT_SYNC_H
#define LSP_HANDLERS_TEXT_DOCUMENT_SYNC_H

#include "cJSON.h"

typedef struct LspServer LspServer;

void lsp_handle_did_open(LspServer *server, const cJSON *params);
void lsp_handle_did_change(LspServer *server, const cJSON *params);
void lsp_handle_did_close(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_TEXT_DOCUMENT_SYNC_H
