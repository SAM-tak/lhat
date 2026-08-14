// L^ (lhat) -- LSP server: lhat/signature.

#ifndef LSP_HANDLERS_SIGNATURE_H
#define LSP_HANDLERS_SIGNATURE_H

#include "cJSON.h"

typedef struct LspServer LspServer;

cJSON *lsp_handle_signature(LspServer *server, const cJSON *params);

#endif  // LSP_HANDLERS_SIGNATURE_H
