// L^ (lhat) -- extended grapheme boundaries in immutable byte strings.
#ifndef LHAT_GRAPHEME_H
#define LHAT_GRAPHEME_H

#include <stddef.h>

// `from` is a boundary returned by this function, or zero. Invalid UTF-8
// bytes each form a separate cluster; no input is replaced or normalised.
// Returns length at the end. Embedded NUL bytes are ordinary input.
size_t lhat_grapheme_next(const char *text, size_t length, size_t from);
size_t lhat_grapheme_count(const char *text, size_t length);

#endif
