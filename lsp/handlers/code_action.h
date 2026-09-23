// L^ (lhat) -- LSP server: textDocument/codeAction.

#ifndef LSP_HANDLERS_CODE_ACTION_H
#define LSP_HANDLERS_CODE_ACTION_H

#include "cJSON.h"
#include "server.h"

cJSON *lsp_handle_code_action(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_CODE_ACTION_H
