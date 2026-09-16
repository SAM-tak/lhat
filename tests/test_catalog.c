// 10 §6.4: reading a catalog. What a host hands over is bytes in 10 §6.1's
// format, for one language and one source; what comes out is the entries
// this build knows the names and the holes of. Nothing here fails -- a line
// the reader cannot use is left out and the rest stands -- so what each case
// pins is which lines were taken and what they came to mean.
//
// The texts stand in for translations without being any language's: what is
// checked is the reading, and an ASCII stand-in reads the same as a Japanese
// one would.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "message.h"
#include "program_internal.h"
#include "testutil.h"

// The catalog `text` makes for `source`, against the English this build
// holds. Disposed by the caller.
static size_t load(LhatCatalog *catalog, const char *source, const char *text)
{
    return lhat_catalog_load(catalog, "xx", source, NULL, 0, text,
                             strlen(text));
}

#define SAME(actual, expected)                                               \
    LHAT_CHECK((actual) != NULL && strcmp((actual), (expected)) == 0,        \
               "got '%s', want '%s'", (actual) != NULL ? (actual) : "(none)",\
               (expected))

static void test_entries(void)
{
    LhatCatalog catalog;
    memset(&catalog, 0, sizeof catalog);

    LHAT_TEST("an entry is taken under the ID its source and name make");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "# trace -- some version\n"
                           "header = a traceback of sorts\n"
                           "# in = in {function}\n"
                           "in = inside {function}\n"),
                      2);
    SAME(lhat_catalog_text(&catalog, "trace.header"), "a traceback of sorts");
    SAME(lhat_catalog_text(&catalog, "trace.in"), "inside {function}");
    LHAT_CHECK(lhat_catalog_text(&catalog, "trace.top-level") == NULL,
               "what was left commented out is not in it");

    LHAT_TEST("loading again replaces what the catalog held");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "coroutine = (a walk)\n"), 1);
    LHAT_CHECK(lhat_catalog_text(&catalog, "trace.header") == NULL,
               "the entry from before is gone");
    SAME(lhat_catalog_text(&catalog, "trace.coroutine"), "(a walk)");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("the later of two entries of one name is the one that counts");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "header = first\n"
                           "header = second\n"),
                      1);
    SAME(lhat_catalog_text(&catalog, "trace.header"), "second");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("an empty catalog has nothing, and neither has a disposed one");
    LHAT_CHECK(lhat_catalog_text(&catalog, "trace.header") == NULL,
               "nothing in it");
    LHAT_CHECK_EQ_INT(catalog.count, 0);
}

static void test_left_out(void)
{
    LhatCatalog catalog;
    memset(&catalog, 0, sizeof catalog);

    LHAT_TEST("a name this source does not hold is left out");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "nowhere = something\n"), 0);

    LHAT_TEST("a text whose holes are not the English's is left out");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "in = inside it\n"), 0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "in = inside {function} of {where}\n"),
                      0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "in = inside {function}\n"), 1);
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("an entry with no text is left out");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header =\n"), 0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header =    \n"), 0);

    LHAT_TEST("a name that is not spelled the way an ID is, is left out");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "-header = x\n"), 0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "hea--der = x\n"), 0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "Header = x\n"), 0);
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header. = x\n"), 0);

    LHAT_TEST("a line with no '=' is left out");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header\n"), 0);

    LHAT_TEST("a source this build does not hold takes nothing");
    LHAT_CHECK_EQ_INT(load(&catalog, "nowhere", "header = x\n"), 0);
    lhat_catalog_dispose(&catalog);
}

static void test_lines(void)
{
    LhatCatalog catalog;
    memset(&catalog, 0, sizeof catalog);

    LHAT_TEST("a continuation line loses one space and keeps the rest");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "top-level = the first line\n"
                           "  the second, indented by one\n"
                           "\tand a third, after a tab\n"),
                      1);
    SAME(lhat_catalog_text(&catalog, "trace.top-level"),
         "the first line\n the second, indented by one\nand a third, after a "
         "tab");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("a blank line is the text's only when the entry goes on");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "top-level = one\n"
                           "\n"
                           " three\n"
                           "\n"),
                      1);
    SAME(lhat_catalog_text(&catalog, "trace.top-level"), "one\n\nthree");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("a comment ends the entry before it");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "top-level = one\n"
                           "# a note between them\n"
                           " not a continuation of anything\n"),
                      1);
    SAME(lhat_catalog_text(&catalog, "trace.top-level"), "one");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("what is at the end of a line is not in the text");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header =   spaced out   \n"), 1);
    SAME(lhat_catalog_text(&catalog, "trace.header"), "spaced out");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("an editor's BOM and its line endings read the same");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "\xEF\xBB\xBF"
                           "header = one\r\n"
                           "  two\r"
                           "top-level = three\r\n"),
                      2);
    SAME(lhat_catalog_text(&catalog, "trace.header"), "one\n two");
    SAME(lhat_catalog_text(&catalog, "trace.top-level"), "three");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("the last line needs no line ending");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace", "header = at the end"), 1);
    SAME(lhat_catalog_text(&catalog, "trace.header"), "at the end");
    lhat_catalog_dispose(&catalog);

    LHAT_TEST("an escaped brace is not a hole, here as in the renderer");
    LHAT_CHECK_EQ_INT(load(&catalog, "trace",
                           "header = a \\{not a hole\\} of sorts\n"),
                      1);
    SAME(lhat_catalog_text(&catalog, "trace.header"),
         "a \\{not a hole\\} of sorts");
    lhat_catalog_dispose(&catalog);
}

// 10 §6.3: a tool holds its own table, so it hands it over to be checked
// against instead of a source this build holds.
static void test_own_table(void)
{
    static const LhatMessageEntry OWN[] = {
        {"tool.one", "first"},
        {"tool.two", "second, about {member}"},
    };
    LhatCatalog catalog;
    memset(&catalog, 0, sizeof catalog);

    LHAT_TEST("a tool's own names are the ones its catalog may use");
    const char *text = "one = the first\n"
                       "two = the second, about {member}\n"
                       "three = no such entry\n";
    LHAT_CHECK_EQ_INT(lhat_catalog_load(&catalog, "xx", "tool", OWN, 2, text,
                                        strlen(text)),
                      2);
    SAME(lhat_catalog_text(&catalog, "tool.one"), "the first");
    SAME(lhat_catalog_text(&catalog, "tool.two"), "the second, about {member}");
    LHAT_CHECK(lhat_catalog_text(&catalog, "tool.three") == NULL,
               "a name the table does not hold is left out");
    lhat_catalog_dispose(&catalog);
}

// 10 §7.1: a program holds the catalogs it was handed and the language its
// messages come out in -- one program's language is not another's.
static void test_program(void)
{
    LhatProgram program;
    lhat_program_init(&program, true, NULL, NULL);
    const char *said = "top-level = the top, in xx\n";

    LHAT_TEST("a catalog is held, and the English stands until xx is chosen");
    LHAT_CHECK_EQ_INT(
        lhat_program_load_language(&program, "xx", "trace", said,
                                   strlen(said)),
        1);
    SAME(lhat_program_language(&program), "en");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "at the top level");

    LHAT_TEST("the language chosen is what a message is drawn in");
    LHAT_CHECK(lhat_program_set_language(&program, "xx"), "xx is chosen");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "the top, in xx");
    SAME(lhat_program_text(&program, "trace.header", "traceback:"),
         "traceback:");

    LHAT_TEST("a tag falls back to its language, and case is not read");
    LHAT_CHECK(lhat_program_set_language(&program, "XX-YY"), "XX-YY is chosen");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "the top, in xx");
    LHAT_CHECK(lhat_program_set_language(&program, "zz"), "zz is chosen");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "at the top level");

    LHAT_TEST("the closest catalog answers, and the language's own fills in");
    const char *closer = "top-level = the top, in xx-yy\n";
    LHAT_CHECK_EQ_INT(
        lhat_program_load_language(&program, "xx-YY", "trace", closer,
                                   strlen(closer)),
        1);
    LHAT_CHECK(lhat_program_set_language(&program, "xx-yy"), "xx-yy is chosen");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "the top, in xx-yy");
    const char *again = "header = a traceback, in xx-yy\n";
    LHAT_CHECK_EQ_INT(
        lhat_program_load_language(&program, "xx-YY", "trace", again,
                                   strlen(again)),
        1);
    SAME(lhat_program_text(&program, "trace.header", "traceback:"),
         "a traceback, in xx-yy");
    SAME(lhat_program_text(&program, "trace.top-level", "at the top level"),
         "the top, in xx");

    LHAT_TEST("a source the caller holds the English of is held beside them");
    static const LhatMessageEntry OWN[] = {{"tool.one", "first"}};
    const char *tool = "one = the first, in xx\n";
    LHAT_CHECK_EQ_INT(lhat_program_load_catalog(&program, "xx", "tool", OWN, 1,
                                                tool, strlen(tool)),
                      1);
    SAME(lhat_program_text(&program, "tool.one", "first"), "the first, in xx");

    lhat_program_dispose(&program);
}

// 10 §6.5: the catalogs this repository ships are held to the English -- a
// name no source holds, or a text whose holes are not the English's, is left
// out by the reader, and that is what this would catch.
static void test_shipped(void)
{
#ifdef LHAT_SOURCE_DIR
    for (size_t i = 0;; i++) {
        const char *source = lhat_messages_source(i);
        if (source == NULL) {
            break;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/messages/ja/%s.txt", LHAT_SOURCE_DIR,
                 source);
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            continue;  // a source with no translation yet is no failure
        }
        LHAT_TEST(path);
        static char text[65536];
        size_t length = fread(text, 1, sizeof text - 1, file);
        fclose(file);
        text[length] = '\0';

        // What the file writes as an entry: a line of its own holding an '='.
        size_t written = 0;
        for (size_t at = 0; at < length; at++) {
            bool first = at == 0 || text[at - 1] == '\n';
            char c = text[at];
            if (!first || c == '#' || c == '\n' || c == '\r' || c == ' ' ||
                c == '\t') {
                continue;
            }
            const char *line = text + at;
            const char *end = strchr(line, '\n');
            const char *equals = strchr(line, '=');
            written += equals != NULL && (end == NULL || equals < end) ? 1 : 0;
        }

        LhatCatalog catalog;
        memset(&catalog, 0, sizeof catalog);
        size_t taken = lhat_catalog_load(&catalog, "ja", source, NULL, 0, text,
                                         length);
        LHAT_CHECK(taken == written, "%zu of the %zu entries written were taken",
                   taken, written);
        lhat_catalog_dispose(&catalog);
    }
#endif
}

int main(void)
{
    test_entries();
    test_left_out();
    test_lines();
    test_own_table();
    test_program();
    test_shipped();
    return lhat_test_report("test_catalog");
}
