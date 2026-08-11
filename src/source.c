// L^ (lhat) -- source text loading and normalisation.

#include "lhat/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"

static char *duplicate(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = (char *)lhat_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static char *format_error(const char *prefix, const char *detail)
{
    size_t n = strlen(prefix) + strlen(detail) + 3;
    char *msg = (char *)lhat_alloc(n);
    if (msg != NULL) {
        snprintf(msg, n, "%s: %s", prefix, detail);
    }
    return msg;
}

// Copies text into dst, turning CRLF and lone CR into LF and dropping a
// leading UTF-8 BOM. Returns the new length.
static size_t normalise(char *dst, const char *text, size_t length)
{
    size_t i = 0;
    size_t out = 0;

    if (length >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        i = 3;
    }

    for (; i < length; i++) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < length && text[i + 1] == '\n') {
                i++;
            }
            dst[out++] = '\n';
        } else {
            dst[out++] = c;
        }
    }

    dst[out] = '\0';
    return out;
}

bool lhat_source_init_from_string(LhatSource *src, const char *name,
                                  const char *text, size_t length)
{
    src->name = NULL;
    src->text = NULL;
    src->length = 0;

    src->name = duplicate(name != NULL ? name : "<memory>");
    if (src->name == NULL) {
        return false;
    }

    src->text = (char *)lhat_alloc(length + 1);
    if (src->text == NULL) {
        lhat_free(src->name);
        src->name = NULL;
        return false;
    }

    src->length = normalise(src->text, text, length);
    return true;
}

bool lhat_source_init_from_file(LhatSource *src, const char *path, char **error)
{
    src->name = NULL;
    src->text = NULL;
    src->length = 0;

    if (error != NULL) {
        *error = NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        if (error != NULL) {
            *error = format_error("cannot open", path);
        }
        return false;
    }

    // Reading in chunks avoids relying on ftell for a correct byte count,
    // which is not portable for every stream.
    size_t capacity = 8192;
    size_t length = 0;
    char *buffer = (char *)lhat_alloc(capacity);
    if (buffer == NULL) {
        fclose(fp);
        if (error != NULL) {
            *error = duplicate("out of memory");
        }
        return false;
    }

    for (;;) {
        if (length == capacity) {
            size_t grown = capacity * 2;
            char *bigger = (char *)lhat_realloc(buffer, grown);
            if (bigger == NULL) {
                lhat_free(buffer);
                fclose(fp);
                if (error != NULL) {
                    *error = duplicate("out of memory");
                }
                return false;
            }
            buffer = bigger;
            capacity = grown;
        }

        size_t got = fread(buffer + length, 1, capacity - length, fp);
        length += got;
        if (got == 0) {
            break;
        }
    }

    bool failed = ferror(fp) != 0;
    fclose(fp);

    if (failed) {
        lhat_free(buffer);
        if (error != NULL) {
            *error = format_error("cannot read", path);
        }
        return false;
    }

    bool ok = lhat_source_init_from_string(src, path, buffer, length);
    lhat_free(buffer);

    if (!ok && error != NULL) {
        *error = duplicate("out of memory");
    }
    return ok;
}

void lhat_source_dispose(LhatSource *src)
{
    lhat_free(src->name);
    lhat_free(src->text);
    src->name = NULL;
    src->text = NULL;
    src->length = 0;
}
