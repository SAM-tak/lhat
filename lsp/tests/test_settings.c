// L^ (lhat) -- LSP server tests: lhat-lsp.json (settings.c).
//
// The pattern language is deliberately not gitignore, so what it does NOT
// do is pinned as carefully as what it does -- a reader who assumes the
// familiar syntax has to meet a failing test rather than a surprise.

#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "testutil.h"

// Settings from one exclude pattern, which is what most of these want.
static LspSettings *with_pattern(const char *pattern)
{
    char text[256];
    snprintf(text, sizeof text, "{\"exclude\": [\"%s\"]}", pattern);
    LspSettings *settings = lsp_settings_parse(text, strlen(text));
    LHAT_CHECK(settings != NULL, "the config parsed: %s", text);
    return settings;
}

static void expect_match(const char *pattern, const char *relative,
                         bool expected)
{
    LspSettings *settings = with_pattern(pattern);
    if (settings == NULL) {
        return;
    }
    bool got = lsp_settings_excludes_relative(settings, relative);
    LHAT_CHECK(got == expected, "\"%s\" against \"%s\": expected %s, got %s",
               pattern, relative, expected ? "excluded" : "kept",
               got ? "excluded" : "kept");
    lsp_settings_free(settings);
}

// A directory names itself and everything under it, and a trailing slash
// changes nothing -- so a scan tests a directory and a file with one call.
static void test_a_directory_and_what_is_under_it(void)
{
    LHAT_TEST("a pattern names a path and everything below it");
    expect_match("build/", "build", true);
    expect_match("build/", "build/x.lh", true);
    expect_match("build/", "build/debug/x.lh", true);
    expect_match("build/", "src/build/x.lh", false);
    expect_match("build/", "builder/x.lh", false);

    LHAT_TEST("and the trailing slash is spelling, not a rule");
    expect_match("build", "build", true);
    expect_match("build", "build/debug/x.lh", true);
    expect_match("build", "builder/x.lh", false);
}

static void test_the_two_stars(void)
{
    LHAT_TEST("'*' never crosses a '/'");
    expect_match("*.lh", "a.lh", true);
    expect_match("*.lh", "sub/a.lh", false);
    expect_match("src/*.lh", "src/a.lh", true);
    expect_match("src/*.lh", "src/sub/a.lh", false);

    LHAT_TEST("'**' crosses, and '**/' also stands for no segments at all");
    expect_match("**/build/", "build/x.lh", true);
    expect_match("**/build/", "src/build/x.lh", true);
    expect_match("**/build/", "src/deep/build/x.lh", true);
    expect_match("**/build/", "x.lh", false);
    expect_match("**/*.lton", "c.lton", true);
    expect_match("**/*.lton", "a/b/c.lton", true);
    expect_match("**/*.lton", "c.lh", false);

    // 'src/**' has a written '/' facing the end of "src", so it does not
    // name the directory itself -- a scan cannot prune on it and excludes
    // the files one at a time instead. Correct, just slower; pinned so that
    // changing it is a decision rather than an accident.
    LHAT_TEST("but 'src/**' does not name 'src' itself");
    expect_match("src/**", "src/a/b.lh", true);
    expect_match("src/**", "src", false);
}

static void test_what_the_language_does_not_have(void)
{
    LHAT_TEST("there is no negation: '!' is a character like any other");
    expect_match("!keep.lh", "!keep.lh", true);
    expect_match("!keep.lh", "keep.lh", false);

    LHAT_TEST("and no '?' and no character class");
    expect_match("a?c.lh", "a?c.lh", true);
    expect_match("a?c.lh", "abc.lh", false);
    expect_match("a[bc].lh", "a[bc].lh", true);
    expect_match("a[bc].lh", "ab.lh", false);
}

// Every path key in this server is absolute (lsp/uri.h), so the workspace
// asks about one of those and the relative form is cut here.
static void test_an_absolute_path(void)
{
    LspSettings *settings = with_pattern("build/");
    if (settings == NULL) {
        return;
    }

    LHAT_TEST("an absolute path is cut against the workspace root");
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/repo", "c:/repo/build/x.lh"),
        true);
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/repo", "c:/repo/src/x.lh"),
        false);

    // The prefix alone would let "c:/repo" swallow this.
    LHAT_TEST("and a root is not a prefix of another root's name");
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_path(settings, "c:/repo",
                                                  "c:/repository/build/x.lh"),
                       false);

    LHAT_TEST("a path outside the root is never excluded");
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/repo", "c:/other/build/x.lh"),
        false);
    // Single-file mode: no project for anything to be outside of.
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, NULL, "c:/repo/build/x.lh"),
        false);
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_path(NULL, "c:/repo",
                                                  "c:/repo/build/x.lh"),
                       false);

    LHAT_TEST("a root that already ends in '/' is not counted twice");
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/", "c:/build/x.lh"), true);

    lsp_settings_free(settings);
}

// 8.1: a few files inside an excluded directory are sources after all.
// Named rather than matched -- so nothing has to be searched for in a
// directory the scan was told to skip, which is the caveat gitignore's `!`
// carries and this does not.
static void test_a_named_file_survives_the_exclusion(void)
{
    static const char *const text =
        "{\"exclude\": [\"build/\"],"
        " \"force_include_files\": [\"build/generated/api.lh\"]}";
    LspSettings *settings = lsp_settings_parse(text, strlen(text));
    LHAT_CHECK(settings != NULL, "the config parsed");
    if (settings == NULL) {
        return;
    }

    LHAT_TEST("the named file is kept and its neighbours are not");
    LHAT_CHECK_EQ_INT((int)lsp_settings_force_count(settings), 1);
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_relative(settings, "build/generated/api.lh"),
        false);
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_relative(settings, "build/generated/other.lh"),
        true);
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build/x.lh"),
                       true);
    // The directory itself stays excluded, which is what lets the scan go on
    // pruning it: the named file is added afterwards, by name.
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_relative(settings, "build/generated"), true);
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build"), true);

    LHAT_TEST("and the same answer through an absolute path");
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/repo",
                                   "c:/repo/build/generated/api.lh"),
        false);
    LHAT_CHECK_EQ_BOOL(
        lsp_settings_excludes_path(settings, "c:/repo", "c:/repo/build/x.lh"),
        true);

    LHAT_TEST("the entry is what workspace.c joins to the root");
    const char *named = lsp_settings_force_at(settings, 0);
    LHAT_CHECK(named != NULL && strcmp(named, "build/generated/api.lh") == 0,
               "expected the written path, got %s",
               named != NULL ? named : "(nothing)");
    LHAT_CHECK(lsp_settings_force_at(settings, 1) == NULL,
               "expected nothing past the end");
    lsp_settings_free(settings);
}

// The name is a path, not a pattern: what it does not do matters as much.
static void test_a_named_file_is_not_a_pattern(void)
{
    static const char *const text =
        "{\"exclude\": [\"build/\"],"
        " \"force_include_files\": [\"build/*.lh\", \"build/**\"]}";
    LspSettings *settings = lsp_settings_parse(text, strlen(text));
    if (settings == NULL) {
        return;
    }

    LHAT_TEST("a wildcard in a named file is a character, not a match");
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build/a.lh"),
                       true);
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build/a/b.lh"),
                       true);
    // Written with those characters, it names a file with those characters.
    LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build/*.lh"),
                       false);
    lsp_settings_free(settings);
}

// The one spelling refused outright. Every other path that names nothing
// under the root simply names nothing and is reported missing -- but a ".."
// can name a real file by a path nothing in this server folds, and one file
// would become two roots.
static void test_a_named_file_stays_under_the_root(void)
{
    static const char *const text =
        "{\"force_include_files\": [\"../outside.lh\", \"a/../b.lh\","
        " \"..hidden.lh\", \"kept.lh\"]}";
    LspSettings *settings = lsp_settings_parse(text, strlen(text));
    LHAT_CHECK(settings != NULL, "the config parsed");
    if (settings == NULL) {
        return;
    }

    LHAT_TEST("a '..' segment is refused, and a name that merely starts with "
              "dots is not");
    LHAT_CHECK_EQ_INT((int)lsp_settings_force_count(settings), 2);
    LHAT_CHECK(strcmp(lsp_settings_force_at(settings, 0), "..hidden.lh") == 0,
               "expected ..hidden.lh, got %s",
               lsp_settings_force_at(settings, 0));
    LHAT_CHECK(strcmp(lsp_settings_force_at(settings, 1), "kept.lh") == 0,
               "expected kept.lh, got %s", lsp_settings_force_at(settings, 1));
    lsp_settings_free(settings);
}

static void test_reading_the_file(void)
{
    LHAT_TEST("what is not JSON, or is JSON of the wrong shape, is refused");
    LHAT_CHECK(lsp_settings_parse("", 0) == NULL, "expected nothing from \"\"");
    LHAT_CHECK(lsp_settings_parse("{", 1) == NULL, "expected nothing from \"{\"");
    // An array would read as an empty object and silently exclude nothing;
    // better to say so, which is what NULL becomes on the log.
    LHAT_CHECK(lsp_settings_parse("[1,2]", 5) == NULL,
               "expected nothing from an array");

    LHAT_TEST("an empty object is a config that says nothing");
    LspSettings *settings = lsp_settings_parse("{}", 2);
    LHAT_CHECK(settings != NULL, "expected an empty config to parse");
    if (settings != NULL) {
        LHAT_CHECK_EQ_INT((int)lsp_settings_exclude_count(settings), 0);
        LHAT_CHECK_EQ_INT((int)lsp_settings_force_count(settings), 0);
        LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "build"),
                           false);
        // Both ways round: with nothing written, the fallback is the answer.
        LHAT_CHECK_EQ_BOOL(lsp_settings_strict(settings, true), true);
        LHAT_CHECK_EQ_BOOL(lsp_settings_strict(settings, false), false);
        lsp_settings_free(settings);
    }
    LHAT_CHECK_EQ_BOOL(lsp_settings_strict(NULL, true), true);
    LHAT_CHECK_EQ_INT((int)lsp_settings_exclude_count(NULL), 0);
    LHAT_CHECK_EQ_INT((int)lsp_settings_force_count(NULL), 0);
    LHAT_CHECK(lsp_settings_force_at(NULL, 0) == NULL,
               "expected nothing from no settings at all");

    LHAT_TEST("a key this reader does not know loses itself and no more");
    static const char *const odd = "{\"nonsense\": 1, \"exclude\": [\"build/\"]}";
    settings = lsp_settings_parse(odd, strlen(odd));
    LHAT_CHECK(settings != NULL, "expected the rest to be kept");
    if (settings != NULL) {
        LHAT_CHECK_EQ_BOOL(
            lsp_settings_excludes_relative(settings, "build/x.lh"), true);
        lsp_settings_free(settings);
    }

    LHAT_TEST("and so does an entry of the wrong shape");
    static const char *const mixed = "{\"exclude\": [\"a\", 5, \"\", \"b/\"]}";
    settings = lsp_settings_parse(mixed, strlen(mixed));
    if (settings != NULL) {
        LHAT_CHECK_EQ_INT((int)lsp_settings_exclude_count(settings), 2);
        LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "a"), true);
        LHAT_CHECK_EQ_BOOL(lsp_settings_excludes_relative(settings, "b/x.lh"),
                           true);
        lsp_settings_free(settings);
    }

    LHAT_TEST("an \"exclude\" that is not an array carries no patterns");
    static const char *const scalar = "{\"exclude\": \"build/\"}";
    settings = lsp_settings_parse(scalar, strlen(scalar));
    LHAT_CHECK(settings != NULL, "expected the file to still parse");
    if (settings != NULL) {
        LHAT_CHECK_EQ_INT((int)lsp_settings_exclude_count(settings), 0);
        lsp_settings_free(settings);
    }

    LHAT_TEST("\"strict\" is read, and only when it is a bool");
    static const char *const relaxed = "{\"strict\": false}";
    settings = lsp_settings_parse(relaxed, strlen(relaxed));
    if (settings != NULL) {
        LHAT_CHECK_EQ_BOOL(lsp_settings_strict(settings, true), false);
        lsp_settings_free(settings);
    }
    static const char *const written = "{\"strict\": \"yes\"}";
    settings = lsp_settings_parse(written, strlen(written));
    if (settings != NULL) {
        LHAT_CHECK_EQ_BOOL(lsp_settings_strict(settings, true), true);
        LHAT_CHECK_EQ_BOOL(lsp_settings_strict(settings, false), false);
        lsp_settings_free(settings);
    }
}

// A pattern is written by a person, who may write it the way their own
// shell does. Both spellings mean the workspace-relative path.
static void test_a_pattern_is_tidied_once(void)
{
    LHAT_TEST("a backslash is a separator too");
    static const char *const windows = "{\"exclude\": [\"build\\\\debug\"]}";
    LspSettings *settings = lsp_settings_parse(windows, strlen(windows));
    LHAT_CHECK(settings != NULL, "the config parsed");
    if (settings != NULL) {
        LHAT_CHECK_EQ_BOOL(
            lsp_settings_excludes_relative(settings, "build/debug/x.lh"), true);
        lsp_settings_free(settings);
    }

    LHAT_TEST("and a leading '/' or './' says the root, which is where it is");
    expect_match("/build/", "build/x.lh", true);
    expect_match("./build/", "build/x.lh", true);
}

int main(void)
{
    test_a_directory_and_what_is_under_it();
    test_the_two_stars();
    test_what_the_language_does_not_have();
    test_an_absolute_path();
    test_a_named_file_survives_the_exclusion();
    test_a_named_file_is_not_a_pattern();
    test_a_named_file_stays_under_the_root();
    test_reading_the_file();
    test_a_pattern_is_tidied_once();
    return lhat_test_report("test_settings");
}
