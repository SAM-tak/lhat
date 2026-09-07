// L^ (lhat) -- LSP server: textDocument/completion.

#ifndef LSP_HANDLERS_COMPLETION_H
#define LSP_HANDLERS_COMPLETION_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_completion(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_COMPLETION_H
