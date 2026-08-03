// L^ (lhat) -- tests for the one shape every stage reports in.
//
// What is pinned here is the rendering: where the mark lands, what happens
// with no source to quote, and that measuring and filling agree. The codes
// themselves belong to the stages and are tested with them.

#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "testutil.h"

// Writes into a buffer of its own, the way a caller would: measure, then fill.
// Answers the text, which the caller frees.
static char *rendered(const LhatReport *report, const LhatSource *source,
                      const char *name, bool rich)
{
    size_t needed = lhat_report_write(report, source, name, rich, NULL, 0);
    char *out = (char *)malloc(needed + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t again = lhat_report_write(report, source, name, rich, out,
                                     needed + 1);
    // Measuring and filling have to agree, or a caller sizing a buffer from
    // the first call gets a truncated second one.
    if (again != needed) {
        free(out);
        return NULL;
    }
    return out;
}

static LhatReport at(const char *message, uint32_t offset, uint32_t line,
                     uint32_t column, uint32_t length)
{
    LhatReport report;
    report.kind = LHAT_REPORT_ERROR;
    report.message = message;
    report.offset = offset;
    report.line = line;
    report.column = column;
    report.length = length;
    return report;
}

static void test_plain(void)
{
    LhatSource source;
    lhat_source_init_from_string(&source, "main.lh", "let^ x = 1\n", 11);

    LHAT_TEST("the plain form is one line");
    {
        LhatReport report = at("something went wrong", 5, 1, 6, 0);
        char *text = rendered(&report, &source, NULL, false);
        LHAT_CHECK(text != NULL, "measured and filled agree");
        if (text != NULL) {
            LHAT_CHECK(strcmp(text, "main.lh:1:6: error: something went wrong") == 0,
                       "the name, the place and the message");
            free(text);
        }
    }

    LHAT_TEST("and a note says so");
    {
        LhatReport report = at("just so you know", 0, 1, 1, 0);
        report.kind = LHAT_REPORT_NOTE;
        char *text = rendered(&report, &source, NULL, false);
        if (text != NULL) {
            LHAT_CHECK(strstr(text, "note: ") != NULL, "not an error");
            free(text);
        }
    }

    LHAT_TEST("and a name of the caller's wins over the source's");
    {
        LhatReport report = at("m", 0, 1, 1, 0);
        char *text = rendered(&report, &source, "stdin", false);
        if (text != NULL) {
            LHAT_CHECK(strncmp(text, "stdin:", 6) == 0, "the one handed in");
            free(text);
        }
    }

    lhat_source_dispose(&source);
}

static void test_rich(void)
{
    // Three lines, so the middle one has to be found rather than assumed.
    static const char *const text = "let^ a = 1\nlet^ b = wrong\nlet^ c = 3\n";
    LhatSource source;
    lhat_source_init_from_string(&source, "main.lh", text, strlen(text));

    LHAT_TEST("the rich form quotes the line and marks the place");
    {
        // 'wrong' begins at offset 20, which is column 10 of line 2.
        LhatReport report = at("no such name in scope", 20, 2, 10, 0);
        char *out = rendered(&report, &source, NULL, true);
        LHAT_CHECK(out != NULL, "measured and filled agree");
        if (out != NULL) {
            LHAT_CHECK(strcmp(out,
                              "let^ b = wrong\n"
                              "         ~\n"
                              "(main.lh)2:10: error: no such name in scope") == 0,
                       "the line, the mark under it, then the place");
            free(out);
        }
    }

    // '~' and not '^': 01 の 2.2 makes a hat a letter, so a mark of them
    // under a line of them would read as more of the line.
    LHAT_TEST("the mark is a tilde");
    {
        LhatReport report = at("m", 20, 2, 10, 0);
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strchr(out, '^') == NULL || strstr(out, "let^") != NULL,
                       "no hat of its own");
            LHAT_CHECK(strstr(out, "\n         ~\n") != NULL, "a tilde");
            free(out);
        }
    }

    LHAT_TEST("and a span is marked for its width");
    {
        LhatReport report = at("m", 20, 2, 10, 5);  // 'wrong'
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strstr(out, "\n         ~~~~~\n") != NULL, "five wide");
            free(out);
        }
    }

    // Nothing to quote, so there is nothing the rich form can add.
    LHAT_TEST("with no source it falls back to the plain form");
    {
        LhatReport report = at("m", 0, 1, 1, 0);
        char *out = rendered(&report, NULL, "stdin", true);
        if (out != NULL) {
            LHAT_CHECK(strcmp(out, "stdin:1:1: error: m") == 0, "one line");
            free(out);
        }
    }

    lhat_source_dispose(&source);
}

// The column the mark sits in, counting a UTF-8 character as one -- which is
// the whole point of the arithmetic being tested.
static size_t mark_column(const char *rendered_text)
{
    const char *newline = strchr(rendered_text, '\n');
    if (newline == NULL) {
        return (size_t)-1;
    }
    size_t column = 0;
    for (const char *p = newline + 1; *p != '\0' && *p != '\n'; p++) {
        if (*p == '~') {
            return column;
        }
        if (((unsigned char)*p & 0xC0u) != 0x80u) {
            column++;
        }
    }
    return (size_t)-1;
}

// A terminal wraps a line too wide for it and the mark lands under the wrong
// place, so a window is shown instead. What has to hold is that the mark
// still points at the same character.
static void test_window(void)
{
    LHAT_TEST("a line that fits is shown whole");
    {
        LhatSource source;
        lhat_source_init_from_string(&source, "m", "abcdef", 6);
        LhatReport report = at("m", 3, 1, 4, 0);
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strncmp(out, "abcdef\n", 7) == 0, "no elision");
            LHAT_CHECK_EQ_INT(mark_column(out), 3);
            free(out);
        }
        lhat_source_dispose(&source);
    }

    LHAT_TEST("a wide line is windowed and the mark still points at it");
    {
        // The mark is far enough in that both ends are cut.
        char line[400];
        memset(line, 'x', sizeof line);
        line[200] = 'Q';
        LhatSource source;
        lhat_source_init_from_string(&source, "m", line, sizeof line);

        LhatReport report = at("m", 200, 1, 201, 0);
        char *out = rendered(&report, &source, NULL, true);
        LHAT_CHECK(out != NULL, "measured and filled agree");
        if (out != NULL) {
            LHAT_CHECK(strncmp(out, "...", 3) == 0, "cut on the left");
            LHAT_CHECK(strstr(out, "...\n") != NULL, "and on the right");

            // The mark's column has to index the character it names.
            size_t column = mark_column(out);
            LHAT_CHECK(column != (size_t)-1, "there is a mark");
            if (column != (size_t)-1) {
                LHAT_CHECK_EQ_INT(out[column], 'Q');
            }
            free(out);
        }
        lhat_source_dispose(&source);
    }

    LHAT_TEST("and a mark near the start keeps the left of the line");
    {
        char line[400];
        memset(line, 'x', sizeof line);
        line[2] = 'Q';
        LhatSource source;
        lhat_source_init_from_string(&source, "m", line, sizeof line);

        LhatReport report = at("m", 2, 1, 3, 0);
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strncmp(out, "...", 3) != 0, "nothing cut on the left");
            size_t column = mark_column(out);
            if (column != (size_t)-1) {
                LHAT_CHECK_EQ_INT(out[column], 'Q');
            }
            free(out);
        }
        lhat_source_dispose(&source);
    }

    // The mark is placed by what a terminal draws, so a full-width character
    // moves it by two and a multi-byte one is not read as its bytes. UAX #11:
    // 日本 is four cells, two characters and six bytes -- three numbers that
    // all differ, so getting the mark right means using the correct one.
    LHAT_TEST("and a full-width character is two cells wide");
    {
        static const char *const text = "let^ x = \"\xe6\x97\xa5\xe6\x9c\xac\" + 1";
        LhatSource source;
        lhat_source_init_from_string(&source, "m", text, strlen(text));

        const char *plus = strchr(text, '+');
        LHAT_CHECK_EQ_INT((int)(plus - text), 18);  // bytes

        // 'let^ x = "' is ten cells, 日本 four, then the quote and a blank.
        LhatReport report = at("m", (uint32_t)(plus - text), 1, 15, 0);
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK_EQ_INT(mark_column(out), 16);
            free(out);
        }
        lhat_source_dispose(&source);
    }

    // A span of full-width text is covered rather than half covered.
    LHAT_TEST("and a span of them is marked for its width in cells");
    {
        static const char *const text = "\xe6\x97\xa5\xe6\x9c\xac + 1";
        LhatSource source;
        lhat_source_init_from_string(&source, "m", text, strlen(text));

        LhatReport report = at("m", 0, 1, 1, 6);  // the six bytes of 日本
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strstr(out, "\n~~~~\n") != NULL, "four cells, not two");
            free(out);
        }
        lhat_source_dispose(&source);
    }
}

static void test_edges(void)
{
    LHAT_TEST("a report of nothing writes nothing");
    {
        char room[8];
        room[0] = 'x';
        LHAT_CHECK_EQ_INT(lhat_report_write(NULL, NULL, NULL, true, room,
                                            sizeof room),
                          0);
        LHAT_CHECK_EQ_INT(room[0], '\0');
    }

    LHAT_TEST("a buffer too small is filled as far as it goes and terminated");
    {
        LhatSource source;
        lhat_source_init_from_string(&source, "m", "abc\n", 4);
        LhatReport report = at("message", 0, 1, 1, 0);
        char room[6];
        size_t needed =
            lhat_report_write(&report, &source, NULL, false, room, sizeof room);
        LHAT_CHECK(needed >= sizeof room, "it wanted more");
        LHAT_CHECK_EQ_INT(room[sizeof room - 1], '\0');
        LHAT_CHECK(strlen(room) < sizeof room, "terminated within it");
        lhat_source_dispose(&source);
    }

    // The offset points past the line it names when a construct ran to the
    // end of the input. The mark belongs at the end rather than nowhere.
    LHAT_TEST("an offset at the end of the line still marks it");
    {
        LhatSource source;
        lhat_source_init_from_string(&source, "m", "ab", 2);
        LhatReport report = at("message", 2, 1, 3, 0);
        char *out = rendered(&report, &source, NULL, true);
        if (out != NULL) {
            LHAT_CHECK(strncmp(out, "ab\n  ~\n", 7) == 0, "past the last byte");
            free(out);
        }
    }
}

int main(void)
{
    test_plain();
    test_rich();
    test_window();
    test_edges();
    return lhat_test_report("test_error");
}
