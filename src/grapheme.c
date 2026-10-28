// L^ (lhat) -- Unicode 17 extended grapheme clusters (UAX #29).
#include "grapheme.h"

#include "utf8proc.h"

// The common ASCII path needs no property lookup. CR LF is the only pair
// of ASCII bytes which belongs to one extended grapheme cluster.
static ptrdiff_t decode(const char *text, size_t length, size_t at,
                        utf8proc_int32_t *codepoint)
{
    if ((unsigned char)text[at] < 0x80) {
        *codepoint = (unsigned char)text[at];
        return 1;
    }
    size_t available = length - at;
    return utf8proc_iterate((const utf8proc_uint8_t *)text + at,
                            available < 4 ? (utf8proc_ssize_t)available : 4,
                            codepoint);
}

size_t lhat_grapheme_next(const char *text, size_t length, size_t from)
{
    if (from >= length) return length;
    utf8proc_int32_t previous;
    ptrdiff_t width = decode(text, length, from, &previous);
    if (width < 0) return from + 1;
    size_t at = from + (size_t)width;
    utf8proc_int32_t state = 0;
    while (at < length) {
        utf8proc_int32_t current;
        width = decode(text, length, at, &current);
        if (width < 0) break;
        if (previous < 0x80 && current < 0x80) {
            if (previous != '\r' || current != '\n') break;
        } else if (utf8proc_grapheme_break_stateful(previous, current, &state)) {
            break;
        }
        previous = current;
        at += (size_t)width;
    }
    return at;
}

size_t lhat_grapheme_count(const char *text, size_t length)
{
    size_t count = 0;
    for (size_t at = 0; at < length; count++) {
        if ((unsigned char)text[at] < 0x80 &&
            (at + 1 == length || (unsigned char)text[at + 1] < 0x80)) {
            at += text[at] == '\r' && at + 1 < length && text[at + 1] == '\n' ? 2 : 1;
        } else {
            at = lhat_grapheme_next(text, length, at);
        }
    }
    return count;
}
