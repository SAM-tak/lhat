// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.

#ifndef LSP_HANDLERS_SEMANTIC_TOKENS_H
#define LSP_HANDLERS_SEMANTIC_TOKENS_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_semantic_tokens_full(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_SEMANTIC_TOKENS_H
