// L^ (lhat) -- LSP server: a .lton file, checked as what it is.

#include "lton.h"

#include <stdlib.h>
#include <string.h>

// The wrapper itself, and only that: including this costs the server no
// link-time dependency on the sample standard library, which it does not
// build against (CMakeLists.txt says why).
#include "stdlib/lton.h"

bool lsp_lton_is_path(const char *path)
{
    if (path == NULL) {
        return false;
    }
    size_t length = strlen(path);
    size_t extension = strlen(".lton");
    return length > extension &&
           strcmp(path + length - extension, ".lton") == 0;
}

uint32_t lsp_lton_prologue_length(void)
{
    return (uint32_t)strlen(LHATSTDLIB_LTON_PROLOGUE);
}

char *lsp_lton_wrap(const char *text, size_t length, size_t *out_length)
{
    size_t prologue = strlen(LHATSTDLIB_LTON_PROLOGUE);
    size_t epilogue = strlen(LHATSTDLIB_LTON_EPILOGUE);
    char *wrapped = (char *)malloc(prologue + length + epilogue + 1);
    if (wrapped == NULL) {
        return NULL;
    }
    memcpy(wrapped, LHATSTDLIB_LTON_PROLOGUE, prologue);
    if (length > 0 && text != NULL) {
        memcpy(wrapped + prologue, text, length);
    }
    memcpy(wrapped + prologue + length, LHATSTDLIB_LTON_EPILOGUE, epilogue + 1);
    if (out_length != NULL) {
        *out_length = prologue + length + epilogue;
    }
    return wrapped;
}
