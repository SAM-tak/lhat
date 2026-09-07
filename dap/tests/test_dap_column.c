// L^ (lhat) -- the column a stack frame reports (09 の 5.3).
//
// The debugger keeps no column per instruction: 04 の 11.6 decided against
// that table, and a line event is line-granular anyway, so a column can only
// ever be a label on where a stop already is. The label the adapter puts
// there is where the line's own text begins -- which is what this pins.

#include <string.h>

#include "adapter.h"

#include "testutil.h"

// The whole of what the function is asked: text, a line, a column back.
static void expect_column(const char *text, uint32_t line, uint32_t expected)
{
    uint32_t got =
        dap_column_of_line(text, text != NULL ? strlen(text) : 0, line);
    LHAT_CHECK(got == expected, "line %u of %s: expected column %u, got %u",
               line, text != NULL ? "the text" : "nothing", expected, got);
}

static void test_where_the_line_begins(void)
{
    static const char *const source =
        "let^ twice = f^ n:number^ -> number^ {\n"   // 1: no indent
        "    let^ doubled = n * 2\n"                 // 2: four spaces
        "\treturn^ doubled\n"                        // 3: one tab
        "}\n";                                       // 4: no indent

    LHAT_TEST("09 の 5.3: the column is where the line's text begins");
    expect_column(source, 1, 1);
    expect_column(source, 2, 5);
    // A tab is one character wherever it lands a reader's eye: the editor
    // decides how wide it draws, and DAP counts characters.
    expect_column(source, 3, 2);
    expect_column(source, 4, 1);
}

static void test_nothing_to_point_at(void)
{
    static const char *const source =
        "let^ a = 1\n"   // 1
        "\n"             // 2: empty
        "   \n"          // 3: blanks and nothing else
        "let^ b = 2\n";  // 4

    // Pointing past the end of what is written would put the mark where a
    // reader sees nothing, so a line with nothing on it answers 1.
    LHAT_TEST("and a line with nothing on it answers 1");
    expect_column(source, 2, 1);
    expect_column(source, 3, 1);
    expect_column(source, 4, 1);

    LHAT_TEST("as does a line that is not there");
    expect_column(source, 5, 1);
    expect_column(source, 99, 1);
    // Lines are 1-based, so 0 names no line at all.
    expect_column(source, 0, 1);

    LHAT_TEST("and so does having no text to read");
    expect_column(NULL, 1, 1);
    expect_column("", 1, 1);
}

static void test_the_last_line(void)
{
    // A file whose last line has no newline after it is still a line.
    LHAT_TEST("a last line with no newline after it is read like any other");
    expect_column("let^ a = 1\n  let^ b = 2", 2, 3);
    expect_column("  let^ a = 1", 1, 3);

    // And one that ends in a newline has no line after it.
    LHAT_TEST("but the newline that ends a file opens no further line");
    expect_column("let^ a = 1\n", 2, 1);
}

static void test_the_length_is_the_end(void)
{
    // The text is a unit's, whose length is what LhatSource holds -- so what
    // stands past it in memory is nothing this may read.
    static const char *const source = "  a\n    b\n";

    LHAT_TEST("nothing past `length` is read");
    LHAT_CHECK_EQ_INT((int)dap_column_of_line(source, 4, 1), 3);
    // Cut before the second line's text: there is no line 2 within reach.
    LHAT_CHECK_EQ_INT((int)dap_column_of_line(source, 4, 2), 1);
    // Cut inside the second line's indent, so it is all blanks as far as
    // this may see.
    LHAT_CHECK_EQ_INT((int)dap_column_of_line(source, 7, 2), 1);
    LHAT_CHECK_EQ_INT((int)dap_column_of_line(source, 9, 2), 5);
}

int main(void)
{
    test_where_the_line_begins();
    test_nothing_to_point_at();
    test_the_last_line();
    test_the_length_is_the_end();
    return lhat_test_report("test_dap_column");
}
