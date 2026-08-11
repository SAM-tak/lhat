// L^ (lhat) -- source text loading and normalisation.
//
// Section 1 of DesignDocuments/01-lexical-structure.md.

#ifndef LHAT_SOURCE_H
#define LHAT_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *name;    // for diagnostics; owned
    char *text;    // NUL-terminated, owned; CRLF and CR normalised to LF
    size_t length; // excluding the terminating NUL
} LhatSource;

// Both initialisers normalise line endings and strip a leading UTF-8 BOM, so
// offsets recorded by the lexer refer to the normalised text rather than to
// the bytes on disk.
bool lhat_source_init_from_string(LhatSource *src, const char *name,
                                  const char *text, size_t length);

// On failure returns false and, if error is non-NULL, stores a malloc'd
// message that the caller must free.
bool lhat_source_init_from_file(LhatSource *src, const char *path, char **error);

void lhat_source_dispose(LhatSource *src);

#endif  // LHAT_SOURCE_H
