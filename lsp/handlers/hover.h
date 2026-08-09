// L^ (lhat) -- LSP server: textDocument/hover.

#ifndef LSP_HANDLERS_HOVER_H
#define LSP_HANDLERS_HOVER_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_hover(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_HOVER_H
