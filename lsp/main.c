// L^ (lhat) -- LSP server: entry point.
//
// A client (VSCode's language client, or any other) spawns this process and
// speaks LSP over its stdin/stdout -- clangd and rust-analyzer's shape.

#include <stdio.h>

#include "server.h"

int main(void)
{
    LspServer server;
    lsp_server_init(&server, stdout);
    int status = lsp_server_run(&server, stdin);
    lsp_server_dispose(&server);
    return status;
}
