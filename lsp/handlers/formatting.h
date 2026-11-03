// L^ (lhat) -- LSP server: textDocument/formatting.

#ifndef LSP_HANDLERS_FORMATTING_H
#define LSP_HANDLERS_FORMATTING_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_formatting(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_FORMATTING_H
