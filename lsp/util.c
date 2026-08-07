// L^ (lhat) -- LSP server: small string helpers shared across the server.

#include "util.h"

#include <stdlib.h>
#include <string.h>

char *lsp_strdup(const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    return lsp_strndup(text, strlen(text));
}

char *lsp_strndup(const char *text, size_t length)
{
    char *out = (char *)malloc(length + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}
