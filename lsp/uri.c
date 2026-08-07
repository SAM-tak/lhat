// L^ (lhat) -- LSP server: file:// URIs and the paths program.h wants.

#include "uri.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *percent_decode(const char *text, size_t length)
{
    char *out = (char *)malloc(length + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '%' && i + 2 < length) {
            int hi = hex_value(text[i + 1]);
            int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[w++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[w++] = text[i];
    }
    out[w] = '\0';
    return out;
}

// RFC 3986 unreserved, plus '/' (a path separator, kept literal) and ':'
// (only ever a Windows drive letter's, here).
static bool is_unreserved(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
           c == '/' || c == ':';
}

static char *percent_encode(const char *text)
{
    size_t length = strlen(text);
    char *out = (char *)malloc(length * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (is_unreserved(c)) {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0xF];
        }
    }
    out[w] = '\0';
    return out;
}

static bool is_drive_letter_path(const char *text)
{
    return text[0] == '/' && isalpha((unsigned char)text[1]) &&
           text[2] == ':' && (text[3] == '/' || text[3] == '\0');
}

char *lsp_uri_to_absolute_path(const char *uri)
{
    static const char scheme[] = "file://";
    size_t scheme_length = sizeof(scheme) - 1;
    if (strncmp(uri, scheme, scheme_length) != 0) {
        return NULL;
    }
    const char *rest = uri + scheme_length;  // starts with '/'
    char *decoded = percent_decode(rest, strlen(rest));
    if (decoded == NULL) {
        return NULL;
    }

    // "/C:/..." -> "c:/..." -- the one place in this server that knows
    // about a Windows drive letter, the same way 05 の 8.9 keeps the
    // language itself away from its surroundings by confining a seam to
    // one file.
    if (is_drive_letter_path(decoded)) {
        size_t length = strlen(decoded);
        memmove(decoded, decoded + 1, length);  // drops the leading '/', keeps the NUL
        decoded[0] = (char)tolower((unsigned char)decoded[0]);
    }
    return decoded;
}

char *lsp_absolute_path_to_uri(const char *absolute_path)
{
    size_t length = strlen(absolute_path);
    bool drive_letter =
        length >= 2 && isalpha((unsigned char)absolute_path[0]) &&
        absolute_path[1] == ':';

    char *slashed = (char *)malloc(length + 2);  // room for a leading '/'
    if (slashed == NULL) {
        return NULL;
    }
    size_t w = 0;
    if (drive_letter) {
        slashed[w++] = '/';
    }
    for (size_t i = 0; i < length; i++) {
        slashed[w++] = absolute_path[i] == '\\' ? '/' : absolute_path[i];
    }
    slashed[w] = '\0';

    char *encoded = percent_encode(slashed);
    free(slashed);
    if (encoded == NULL) {
        return NULL;
    }

    size_t encoded_length = strlen(encoded);
    char *uri = (char *)malloc(encoded_length + 8);
    if (uri == NULL) {
        free(encoded);
        return NULL;
    }
    memcpy(uri, "file://", 7);
    memcpy(uri + 7, encoded, encoded_length + 1);
    free(encoded);
    return uri;
}
