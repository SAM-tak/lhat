// L^ (lhat) -- LSP server: lhat/toggleDisabledCode.

#ifndef LSP_HANDLERS_DISABLED_CODE_H
#define LSP_HANDLERS_DISABLED_CODE_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_toggle_disabled_code(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_DISABLED_CODE_H
