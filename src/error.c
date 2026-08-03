// L^ (lhat) -- writing a diagnostic down.

#include "error.h"

#include <stdio.h>
#include <string.h>

// The same moving cursor value.c writes with: `used` keeps growing past the
// buffer so the caller can measure with a NULL one and ask again.
typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} Writer;

static void put(Writer *w, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (w->out != NULL && w->used + 1 < w->capacity) {
            w->out[w->used] = text[i];
        }
        w->used++;
    }
}

static void put_text(Writer *w, const char *text)
{
    if (text != NULL) {
        put(w, text, strlen(text));
    }
}

static void put_number(Writer *w, uint32_t value)
{
    char digits[16];
    int written = snprintf(digits, sizeof digits, "%u", value);
    if (written > 0) {
        put(w, digits, (size_t)written);
    }
}

// 01 の 1 章 normalises every line ending to LF, so one character ends a line
// and nothing has to look for a pair.
static bool line_bounds(const LhatSource *source, uint32_t offset,
                        size_t *begin, size_t *end)
{
    if (source == NULL || source->text == NULL || offset > source->length) {
        return false;
    }
    size_t at = offset;
    size_t start = at;
    while (start > 0 && source->text[start - 1] != '\n') {
        start--;
    }
    size_t stop = at;
    while (stop < source->length && source->text[stop] != '\n') {
        stop++;
    }
    *begin = start;
    *end = stop;
    return true;
}

// A UTF-8 continuation byte is part of the character before it, so it takes
// no column of its own. That is what makes a mark land under a character
// rather than under a byte.
static bool starts_character(unsigned char byte)
{
    return (byte & 0xC0u) != 0x80u;
}

static size_t columns_in(const char *line, size_t length)
{
    size_t columns = 0;
    for (size_t i = 0; i < length; i++) {
        if (starts_character((unsigned char)line[i])) {
            columns++;
        }
    }
    return columns;
}

static size_t column_at(const char *line, size_t length, size_t byte)
{
    return columns_in(line, byte < length ? byte : length);
}

static size_t byte_at(const char *line, size_t length, size_t column)
{
    size_t columns = 0;
    for (size_t i = 0; i < length; i++) {
        if (!starts_character((unsigned char)line[i])) {
            continue;
        }
        if (columns == column) {
            return i;
        }
        columns++;
    }
    return length;
}

#define LHAT_REPORT_ELISION "..."
#define LHAT_REPORT_ELISION_COLUMNS 3

// A run of blanks as wide as the text before the mark. Tabs are kept, so a
// line indented with them lines up whatever the terminal makes a tab -- both
// this and the line above expand the same way. A windowed line has none: it
// was cut mid-way and the tab is written as one blank there.
static void put_lead(Writer *w, const char *line, size_t length, bool keep_tabs)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)line[i];
        if (byte == '\t' && keep_tabs) {
            put(w, "\t", 1);
        } else if (starts_character(byte)) {
            put(w, " ", 1);
        }
    }
}

// The line as shown, with tabs flattened so that every character is one
// column -- which is what the window's arithmetic assumes.
static void put_flattened(Writer *w, const char *line, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (line[i] == '\t') {
            put(w, " ", 1);
        } else {
            put(w, line + i, 1);
        }
    }
}

size_t lhat_report_write(const LhatReport *report, const LhatSource *source,
                         const char *name, bool rich, char *out,
                         size_t capacity)
{
    Writer w;
    w.out = out;
    w.capacity = capacity;
    w.used = 0;

    if (report == NULL) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    const char *where = name != NULL ? name
                        : (source != NULL ? source->name : NULL);
    const char *label =
        report->kind == LHAT_REPORT_NOTE ? "note: " : "error: ";

    size_t begin = 0;
    size_t end = 0;
    bool quoted = rich && line_bounds(source, report->offset, &begin, &end);

    if (quoted) {
        const char *line = source->text + begin;
        size_t length = end - begin;
        size_t at = report->offset < end ? report->offset : end;
        size_t mark = column_at(line, length, at - begin);
        size_t width = columns_in(line, length);

        // A window around the mark when the line is too wide to fit. Kept a
        // third of the way in, so there is something before it to read.
        size_t first = 0;
        size_t last = width;
        if (width > LHAT_REPORT_MAX_COLUMNS) {
            size_t lead = LHAT_REPORT_MAX_COLUMNS / 3;
            first = mark > lead ? mark - lead : 0;
            if (first + LHAT_REPORT_MAX_COLUMNS > width) {
                first = width - LHAT_REPORT_MAX_COLUMNS;
            }
            last = first + LHAT_REPORT_MAX_COLUMNS;
        }
        bool cut_left = first > 0;
        bool cut_right = last < width;

        size_t from = byte_at(line, length, first);
        size_t to = byte_at(line, length, last);

        if (cut_left) {
            put_text(&w, LHAT_REPORT_ELISION);
        }
        if (cut_left || cut_right) {
            put_flattened(&w, line + from, to - from);
        } else {
            put(&w, line, length);
        }
        if (cut_right) {
            put_text(&w, LHAT_REPORT_ELISION);
        }
        put(&w, "\n", 1);

        // '~' rather than '^': a hat is a letter here (01 の 2.2), and a mark
        // made of one would read as part of the line above it. A span is
        // marked for its whole width, so a reader sees how much is meant.
        if (cut_left) {
            for (int i = 0; i < LHAT_REPORT_ELISION_COLUMNS; i++) {
                put(&w, " ", 1);
            }
        }
        put_lead(&w, line + from, (at - begin) - from, !cut_left && !cut_right);
        put(&w, "~", 1);
        for (uint32_t i = 1; i < report->length && at + i < end; i++) {
            if (column_at(line, length, (at - begin) + i) >= last) {
                break;
            }
            put(&w, "~", 1);
        }
        put(&w, "\n", 1);

        // The place last, under what it points at, so the eye goes from the
        // line to the mark to the reason.
        if (where != NULL) {
            put(&w, "(", 1);
            put_text(&w, where);
            put(&w, ")", 1);
        }
    } else if (where != NULL) {
        put_text(&w, where);
        put(&w, ":", 1);
    }

    put_number(&w, report->line);
    put(&w, ":", 1);
    put_number(&w, report->column);
    put(&w, ": ", 2);
    put_text(&w, label);
    put_text(&w, report->message);

    if (w.out != NULL && w.capacity > 0) {
        w.out[w.used < w.capacity ? w.used : w.capacity - 1] = '\0';
    }
    return w.used;
}
