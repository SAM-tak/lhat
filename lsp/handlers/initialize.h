// L^ (lhat) -- LSP server: initialize / initialized / shutdown / exit.

#ifndef LSP_HANDLERS_INITIALIZE_H
#define LSP_HANDLERS_INITIALIZE_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_initialize(LspServer *server, const cJSON *params);
void lsp_handle_initialized(LspServer *server, const cJSON *params);
cJSON *lsp_handle_shutdown(LspServer *server, const cJSON *params);
void lsp_handle_exit(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_INITIALIZE_H
