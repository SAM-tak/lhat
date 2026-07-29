// L^ (lhat) -- tests for source loading and normalisation (section 1).

#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "testutil.h"

static void check_normalised(const char *name, const char *input, size_t input_length,
                             const char *expected)
{
    LhatSource source;
    LHAT_CHECK(lhat_source_init_from_string(&source, name, input, input_length),
               "%s: initialisation failed", name);
    LHAT_CHECK_EQ_STR(source.text, source.length, expected);
    lhat_source_dispose(&source);
}

int main(void)
{
    LHAT_TEST("crlf is normalised to lf");
    check_normalised("crlf", "a\r\nb\r\n", 6, "a\nb\n");

    LHAT_TEST("lone cr is normalised to lf");
    check_normalised("cr", "a\rb\r", 4, "a\nb\n");

    LHAT_TEST("mixed line endings");
    check_normalised("mixed", "a\r\nb\nc\rd", 8, "a\nb\nc\nd");

    LHAT_TEST("leading bom is stripped");
    check_normalised("bom", "\xEF\xBB\xBF" "abc", 6, "abc");

    LHAT_TEST("bom elsewhere is kept for the lexer to reject");
    check_normalised("inner bom", "a\xEF\xBB\xBF" "b", 5, "a\xEF\xBB\xBF" "b");

    LHAT_TEST("empty input");
    check_normalised("empty", "", 0, "");

    LHAT_TEST("missing file reports an error");
    {
        LhatSource source;
        char *error = NULL;
        LHAT_CHECK(!lhat_source_init_from_file(&source, "no-such-file.lhat", &error),
                   "loading a missing file should fail");
        LHAT_CHECK(error != NULL, "an error message should be produced");
        free(error);
    }

    return lhat_test_report("test_source");
}
