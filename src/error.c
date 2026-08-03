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

// A UTF-8 continuation byte is part of the character before it.
static bool starts_character(unsigned char byte)
{
    return (byte & 0xC0u) != 0x80u;
}

// The character at `*at`, with `*at` moved past it. Anything malformed is one
// byte standing for itself, so a mark still lands somewhere sensible in text
// this was never meant to read.
static uint32_t next_character(const char *line, size_t length, size_t *at)
{
    size_t i = *at;
    unsigned char lead = (unsigned char)line[i];
    size_t follow = 0;
    uint32_t code = lead;

    if (lead >= 0xF0u) {
        follow = 3;
        code = lead & 0x07u;
    } else if (lead >= 0xE0u) {
        follow = 2;
        code = lead & 0x0Fu;
    } else if (lead >= 0xC0u) {
        follow = 1;
        code = lead & 0x1Fu;
    }
    if (i + follow >= length) {
        *at = i + 1;
        return lead;
    }
    for (size_t k = 1; k <= follow; k++) {
        unsigned char byte = (unsigned char)line[i + k];
        if (starts_character(byte)) {
            *at = i + 1;
            return lead;
        }
        code = (code << 6) | (byte & 0x3Fu);
    }
    *at = i + follow + 1;
    return code;
}

// How many cells of a terminal a character takes, by UAX #11: a wide or
// fullwidth one takes two, a combining mark or a zero-width joiner none.
//
// **The column a diagnostic names is still a count of characters** -- that is
// what an editor is asked to go to. This is only for putting the mark under
// the right place, where what matters is what a terminal draws.
static size_t cells_of(uint32_t code)
{
    if ((code >= 0x0300u && code <= 0x036Fu) ||   // combining diacriticals
        (code >= 0x200Bu && code <= 0x200Fu) ||   // zero width, bidi marks
        (code >= 0xFE00u && code <= 0xFE0Fu)) {   // variation selectors
        return 0;
    }
    if ((code >= 0x1100u && code <= 0x115Fu) ||   // Hangul Jamo
        (code >= 0x2E80u && code <= 0x303Eu) ||   // radicals to CJK symbols
        (code >= 0x3041u && code <= 0x33FFu) ||   // kana to CJK compatibility
        (code >= 0x3400u && code <= 0x4DBFu) ||   // CJK extension A
        (code >= 0x4E00u && code <= 0x9FFFu) ||   // CJK unified
        (code >= 0xA000u && code <= 0xA4CFu) ||   // Yi
        (code >= 0xA960u && code <= 0xA97Fu) ||   // Hangul Jamo extended A
        (code >= 0xAC00u && code <= 0xD7A3u) ||   // Hangul syllables
        (code >= 0xF900u && code <= 0xFAFFu) ||   // CJK compatibility
        (code >= 0xFE10u && code <= 0xFE19u) ||   // vertical forms
        (code >= 0xFE30u && code <= 0xFE6Fu) ||   // CJK compatibility forms
        (code >= 0xFF00u && code <= 0xFF60u) ||   // fullwidth forms
        (code >= 0xFFE0u && code <= 0xFFE6u) ||   // fullwidth signs
        (code >= 0x1F300u && code <= 0x1F64Fu) || // emoji
        (code >= 0x1F900u && code <= 0x1F9FFu) ||
        (code >= 0x20000u && code <= 0x3FFFDu)) { // CJK extension B and up
        return 2;
    }
    return 1;
}

// The cells the text before `byte` takes. A tab counts as one, which is what
// put_flattened writes it as.
static size_t cells_before(const char *line, size_t length, size_t byte)
{
    size_t stop = byte < length ? byte : length;
    size_t cells = 0;
    size_t at = 0;
    while (at < stop) {
        uint32_t code = next_character(line, length, &at);
        cells += code == '\t' ? 1 : cells_of(code);
    }
    return cells;
}

// The first byte at or after which the text takes `cells` or more.
static size_t byte_after_cells(const char *line, size_t length, size_t cells)
{
    size_t seen = 0;
    size_t at = 0;
    while (at < length && seen < cells) {
        size_t was = at;
        uint32_t code = next_character(line, length, &at);
        seen += code == '\t' ? 1 : cells_of(code);
        if (seen >= cells) {
            return seen == cells ? at : was;
        }
    }
    return at;
}

#define LHAT_REPORT_ELISION "..."
#define LHAT_REPORT_ELISION_COLUMNS 3

// A run of blanks as wide as the text before the mark. Tabs are kept when the
// line is shown whole, so a line indented with them lines up whatever the
// terminal makes a tab -- both this and the line above expand the same way.
// A windowed line was cut mid-way and writes a tab as one blank, so this does
// too.
static void put_lead(Writer *w, const char *line, size_t length, bool keep_tabs)
{
    size_t at = 0;
    while (at < length) {
        uint32_t code = next_character(line, length, &at);
        if (code == '\t' && keep_tabs) {
            put(w, "\t", 1);
            continue;
        }
        size_t cells = code == '\t' ? 1 : cells_of(code);
        for (size_t i = 0; i < cells; i++) {
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
        size_t mark = cells_before(line, length, at - begin);
        size_t width = cells_before(line, length, length);

        // A window around the mark when the line is too wide to fit. Kept a
        // third of the way in, so there is something before it to read.
        // Measured in cells, since what it is avoiding is a terminal wrapping.
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

        size_t from = byte_after_cells(line, length, first);
        size_t to = byte_after_cells(line, length, last);

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

        // As wide as what it marks, so a span of full-width text is covered
        // rather than half covered.
        size_t marked = report->length > 0 ? report->length : 1;
        size_t stop = (at - begin) + marked;
        if (stop > length) {
            stop = length;
        }
        // Always one, even where the place is the end of the line and there
        // is no character under it: a construct that ran out of input is
        // reported there, and a mark is what says where.
        size_t wide = cells_before(line, length, stop) - mark;
        put(&w, "~", 1);
        for (size_t i = 1; i < wide && mark + i < last; i++) {
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
