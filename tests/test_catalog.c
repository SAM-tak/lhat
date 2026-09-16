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
#include <string.h>

#include "message.h"
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

int main(void)
{
    test_entries();
    test_left_out();
    test_lines();
    test_own_table();
    return lhat_test_report("test_catalog");
}
