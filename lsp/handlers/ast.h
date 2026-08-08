// L^ (lhat) -- LSP server: lhat/ast.

#ifndef LSP_HANDLERS_AST_H
#define LSP_HANDLERS_AST_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_ast(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_AST_H
