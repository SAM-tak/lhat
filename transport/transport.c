// L^ (lhat) -- Content-Length framing over an LhatStream.

#include "transport.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static size_t file_read(void *context, char *buffer, size_t size)
{
    return fread(buffer, 1, size, (FILE *)context);
}

static bool file_write(void *context, const char *bytes, size_t size)
{
    FILE *file = (FILE *)context;
    bool ok = fwrite(bytes, 1, size, file) == size;
    fflush(file);
    return ok;
}

LhatStream lhat_stream_of_file(FILE *file)
{
    LhatStream stream;
    stream.context = file;
    stream.read = file_read;
    stream.write = file_write;
    return stream;
}

void lhat_transport_use_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// One byte, or EOF (-1). A stream's read may hand back fewer than asked --
// a socket delivers what has arrived -- so the whole of the framing reads
// through this and the loop below rather than trusting one call.
static int read_byte(LhatStream *in)
{
    char c;
    return in->read(in->context, &c, 1) == 1 ? (unsigned char)c : -1;
}

// One header line, its line ending dropped. NULL at end of input with
// nothing read.
static char *read_line(LhatStream *in)
{
    size_t capacity = 128;
    size_t length = 0;
    char *line = (char *)malloc(capacity);
    if (line == NULL) {
        return NULL;
    }
    for (;;) {
        int c = read_byte(in);
        if (c == -1) {
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

bool lhat_transport_read_message(LhatStream *in, char **out_body,
                                 size_t *out_length)
{
    long content_length = -1;
    for (;;) {
        char *line = read_line(in);
        if (line == NULL) {
            return false;  // end of input before a body arrived
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
        // Content-Type and anything else is read past and dropped.
        free(line);
    }
    if (content_length < 0) {
        return false;  // no Content-Length: not a framing we know
    }

    char *body = (char *)malloc((size_t)content_length + 1);
    if (body == NULL) {
        return false;
    }
    size_t got = 0;
    while (got < (size_t)content_length) {
        size_t n = in->read(in->context, body + got,
                            (size_t)content_length - got);
        if (n == 0) {
            free(body);
            return false;  // the stream ended mid-body
        }
        got += n;
    }
    body[content_length] = '\0';
    *out_body = body;
    *out_length = (size_t)content_length;
    return true;
}

bool lhat_transport_write_message(LhatStream *out, const char *body,
                                  size_t length)
{
    char header[64];
    int header_length =
        snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", length);
    if (header_length < 0) {
        return false;
    }
    return out->write(out->context, header, (size_t)header_length) &&
           out->write(out->context, body, length);
}
