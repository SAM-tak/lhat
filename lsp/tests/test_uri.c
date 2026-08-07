// L^ (lhat) -- LSP server tests: file:// URI <-> absolute path.

#include <stdlib.h>
#include <string.h>

#include "uri.h"

#include "testutil.h"

static void test_windows_drive_letter(void)
{
    LHAT_TEST("file URI with a Windows drive letter");
    char *path = lsp_uri_to_absolute_path("file:///C:/Users/foo/main.lh");
    LHAT_CHECK(path != NULL, "expected a path");
    if (path != NULL) {
        LHAT_CHECK_EQ_STR(path, strlen(path), "c:/Users/foo/main.lh");
    }
    free(path);
}

static void test_round_trip(void)
{
    LHAT_TEST("absolute path -> uri -> absolute path round-trips");
    const char *original = "c:/Users/foo/bar baz.lh";  // a space needs escaping
    char *uri = lsp_absolute_path_to_uri(original);
    LHAT_CHECK(uri != NULL, "expected a uri");
    char *back = uri != NULL ? lsp_uri_to_absolute_path(uri) : NULL;
    LHAT_CHECK(back != NULL, "expected a path back");
    if (back != NULL) {
        LHAT_CHECK_EQ_STR(back, strlen(back), original);
    }
    free(uri);
    free(back);
}

static void test_non_file_scheme(void)
{
    LHAT_TEST("a non-file scheme is rejected");
    char *path = lsp_uri_to_absolute_path("untitled:Untitled-1");
    LHAT_CHECK(path == NULL, "expected NULL for a non-file scheme");
    free(path);
}

static void test_percent_decoding(void)
{
    LHAT_TEST("percent-escapes decode");
    char *path = lsp_uri_to_absolute_path("file:///C:/a%20b.lh");
    LHAT_CHECK(path != NULL, "expected a path");
    if (path != NULL) {
        LHAT_CHECK_EQ_STR(path, strlen(path), "c:/a b.lh");
    }
    free(path);
}

static void test_percent_encoding_on_the_way_out(void)
{
    LHAT_TEST("a space is percent-encoded in the uri");
    char *uri = lsp_absolute_path_to_uri("c:/a b.lh");
    LHAT_CHECK(uri != NULL, "expected a uri");
    if (uri != NULL) {
        LHAT_CHECK_EQ_STR(uri, strlen(uri), "file:///c:/a%20b.lh");
    }
    free(uri);
}

int main(void)
{
    test_windows_drive_letter();
    test_round_trip();
    test_non_file_scheme();
    test_percent_decoding();
    test_percent_encoding_on_the_way_out();
    return lhat_test_report("test_lsp_uri");
}
