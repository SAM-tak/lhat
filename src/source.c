// L^ (lhat) -- source text loading and normalisation.

#include "lhat/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"
#include "message.h"

static char *duplicate(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = (char *)lhat_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

// 10 §5.1: why a file did not become a source.
enum { SOURCE_CANNOT_OPEN, SOURCE_CANNOT_READ, SOURCE_OUT_OF_MEMORY };

static const LhatMessageEntry SOURCE_PARTS[] = {
    [SOURCE_CANNOT_OPEN] = {"source.cannot-open", "cannot open: {path}"},
    [SOURCE_CANNOT_READ] = {"source.cannot-read", "cannot read: {path}"},
    [SOURCE_OUT_OF_MEMORY] = {"source.out-of-memory", "out of memory"},
};

const char *lhat_source_part_id(size_t index)
{
    const LhatMessageEntry *entry = LHAT_MESSAGE_AT(SOURCE_PARTS, index);
    return entry != NULL ? entry->id : NULL;
}

// The error lhat_source_init_from_file hands its caller to free.
static char *error_text(size_t part, const char *path)
{
    const LhatMessageArg arg = {"path", path, strlen(path)};
    const char *text = SOURCE_PARTS[part].text;
    size_t n = lhat_message_render(text, &arg, 1, NULL, 0) + 1;
    char *msg = (char *)lhat_alloc(n);
    if (msg != NULL) {
        lhat_message_render(text, &arg, 1, msg, n);
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
            *error = error_text(SOURCE_CANNOT_OPEN, path);
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
            *error = error_text(SOURCE_OUT_OF_MEMORY, path);
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
                    *error = error_text(SOURCE_OUT_OF_MEMORY, path);
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
            *error = error_text(SOURCE_CANNOT_READ, path);
        }
        return false;
    }

    bool ok = lhat_source_init_from_string(src, path, buffer, length);
    lhat_free(buffer);

    if (!ok && error != NULL) {
        *error = error_text(SOURCE_OUT_OF_MEMORY, path);
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
