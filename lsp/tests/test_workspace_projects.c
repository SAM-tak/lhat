// L^ (lhat) -- project-boundary tests for the language server workspace.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "testutil.h"
#include "workspace.h"

static bool make_directory(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static void remove_directory(const char *path)
{
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    size_t length = strlen(text);
    return fwrite(text, 1, length, file) == length && fclose(file) == 0;
}

static bool make_temporary_directory(char *path, size_t capacity)
{
#ifdef _WIN32
    if (capacity < L_tmpnam || tmpnam(path) == NULL) {
        return false;
    }
    for (char *at = path; *at != '\0'; at++) {
        if (*at == '\\') {
            *at = '/';
        }
    }
    return _mkdir(path) == 0;
#else
    static const char pattern[] = "/tmp/lhat-workspace-XXXXXX";
    if (capacity < sizeof pattern) {
        return false;
    }
    memcpy(path, pattern, sizeof pattern);
    return mkdtemp(path) != NULL;
#endif
}

static void join(char *out, size_t capacity, const char *left, const char *right)
{
    snprintf(out, capacity, "%s/%s", left, right);
}

static LspProject *project_at(LspWorkspace *workspace, const char *root)
{
    for (LspProject *project = workspace->projects; project != NULL;
         project = project->next) {
        if (strcmp(project->root_path, root) == 0) {
            return project;
        }
    }
    return NULL;
}

static size_t root_count(const LspProject *project)
{
    size_t count = 0;
    for (const LspRoot *root = project->roots; root != NULL; root = root->next) {
        count++;
    }
    return count;
}

static void test_nearest_project_wins_inside_each_workspace_folder(void)
{
    char base[512];
    LHAT_REQUIRE(make_temporary_directory(base, sizeof base),
                 "could not create a temporary directory");

    char a[512], b[512], nested[512], hidden[512];
    char path[512];
    join(a, sizeof a, base, "a");
    join(b, sizeof b, base, "b");
    join(nested, sizeof nested, a, "nested");
    join(hidden, sizeof hidden, a, ".hidden");
    LHAT_REQUIRE(make_directory(a), "could not create %s", a);
    LHAT_REQUIRE(make_directory(b), "could not create %s", b);
    LHAT_REQUIRE(make_directory(nested), "could not create %s", nested);
    LHAT_REQUIRE(make_directory(hidden), "could not create %s", hidden);

    join(path, sizeof path, base, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, a, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{\"strict\": true}"), "could not write %s", path);
    join(path, sizeof path, a, "lhat-lsp.json");
    LHAT_REQUIRE(write_file(path,
                            "{\"strict\": true, "
                            "\"force_include_files\": [\"nested/inner.lh\"]}"),
                 "could not write %s", path);
    join(path, sizeof path, nested, "lhat-lsp.json");
    LHAT_REQUIRE(write_file(path, "{\"strict\": false}"), "could not write %s", path);
    join(path, sizeof path, hidden, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, b, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, a, "outer.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, nested, "inner.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, hidden, "hidden.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, b, "other.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);

    const char *folders[] = {a, b};
    LspWorkspace workspace;
    lsp_workspace_init(&workspace, folders, 2);
    lsp_workspace_discover_projects(&workspace);

    LHAT_TEST("a nearest config directory makes a separate project");
    LspProject *a_project = project_at(&workspace, a);
    LspProject *nested_project = project_at(&workspace, nested);
    LspProject *b_project = project_at(&workspace, b);
    LHAT_REQUIRE(a_project != NULL && nested_project != NULL && b_project != NULL,
                 "expected projects at both folders and the nested config");
    LHAT_CHECK_EQ_INT((int)root_count(a_project), 1);
    LHAT_CHECK_EQ_INT((int)root_count(nested_project), 1);
    LHAT_CHECK_EQ_INT((int)root_count(b_project), 1);

    LHAT_TEST("the two files are an atomic project boundary");
    LHAT_CHECK_EQ_BOOL(lsp_host_config_strict(a_project->host_config, false), true);
    LHAT_CHECK_EQ_BOOL(lsp_settings_strict(a_project->settings, false), true);
    LHAT_CHECK(nested_project->host_config == NULL,
               "nested project must not inherit the parent host config");
    LHAT_CHECK_EQ_BOOL(lsp_settings_strict(nested_project->settings, true), false);

    LHAT_TEST("a config above a workspace folder is not in scope");
    join(path, sizeof path, base, "lhat-host.json");
    LHAT_CHECK_EQ_BOOL(lsp_workspace_is_config_path(&workspace, path), false);

    LHAT_TEST("an opened file also resolves its ancestors on demand");
    LHAT_CHECK(project_at(&workspace, hidden) == NULL,
               "the background scan intentionally did not enter .hidden");
    join(path, sizeof path, hidden, "hidden.lh");
    lsp_workspace_recheck_affected(&workspace, path);
    LspProject *hidden_project = project_at(&workspace, hidden);
    LHAT_REQUIRE(hidden_project != NULL,
                 "opening a file must discover its nearest config directory");
    LHAT_CHECK_EQ_INT((int)root_count(hidden_project), 1);

    lsp_workspace_dispose(&workspace);

    join(path, sizeof path, a, "outer.lh"); remove(path);
    join(path, sizeof path, nested, "inner.lh"); remove(path);
    join(path, sizeof path, nested, "lhat-lsp.json"); remove(path);
    join(path, sizeof path, hidden, "hidden.lh"); remove(path);
    join(path, sizeof path, hidden, "lhat-host.json"); remove(path);
    join(path, sizeof path, a, "lhat-host.json"); remove(path);
    join(path, sizeof path, a, "lhat-lsp.json"); remove(path);
    join(path, sizeof path, b, "other.lh"); remove(path);
    join(path, sizeof path, b, "lhat-host.json"); remove(path);
    join(path, sizeof path, base, "lhat-host.json"); remove(path);
    remove_directory(nested);
    remove_directory(hidden);
    remove_directory(a);
    remove_directory(b);
    remove_directory(base);
}

int main(void)
{
    test_nearest_project_wins_inside_each_workspace_folder();
    return lhat_test_report("test_workspace_projects");
}
