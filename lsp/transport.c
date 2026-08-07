// L^ (lhat) -- LSP server: Content-Length framing.

#include "transport.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

void lsp_transport_use_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// One header line, its line ending dropped. NULL at EOF with nothing read.
static char *read_line(FILE *in)
{
    size_t capacity = 128;
    size_t length = 0;
    char *line = (char *)malloc(capacity);
    if (line == NULL) {
        return NULL;
    }
    for (;;) {
        int c = fgetc(in);
        if (c == EOF) {
            if (length == 0) {
                free(line);
                return NULL;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;  // the '\r' of a "\r\n" pair
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *bigger = (char *)realloc(line, capacity);
            if (bigger == NULL) {
                free(line);
                return NULL;
            }
            line = bigger;
        }
        line[length++] = (char)c;
    }
    line[length] = '\0';
    return line;
}

bool lsp_transport_read_message(FILE *in, char **out_body, size_t *out_length)
{
    long content_length = -1;
    for (;;) {
        char *line = read_line(in);
        if (line == NULL) {
            return false;  // EOF before a body arrived
        }
        if (line[0] == '\0') {
            free(line);
            break;  // the blank line that ends the headers
        }
        static const char prefix[] = "Content-Length:";
        size_t prefix_length = sizeof(prefix) - 1;
        if (strncmp(line, prefix, prefix_length) == 0) {
            const char *value = line + prefix_length;
            while (*value == ' ') {
                value++;
            }
            content_length = strtol(value, NULL, 10);
        }
        // Content-Type and anything else is read past and dropped -- this
        // server writes only Content-Length itself and needs nothing more.
        free(line);
    }
    if (content_length < 0) {
        return false;  // no Content-Length: the framing is not one we know
    }

    char *body = (char *)malloc((size_t)content_length + 1);
    if (body == NULL) {
        return false;
    }
    size_t got = fread(body, 1, (size_t)content_length, in);
    if (got != (size_t)content_length) {
        free(body);
        return false;
    }
    body[content_length] = '\0';
    *out_body = body;
    *out_length = (size_t)content_length;
    return true;
}

void lsp_transport_write_message(FILE *out, const char *body, size_t length)
{
    fprintf(out, "Content-Length: %zu\r\n\r\n", length);
    fwrite(body, 1, length, out);
    fflush(out);
}
