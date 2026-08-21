// L^ (lhat) -- LSP server: textDocument/definition.

#ifndef LSP_HANDLERS_DEFINITION_H
#define LSP_HANDLERS_DEFINITION_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_definition(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_DEFINITION_H
