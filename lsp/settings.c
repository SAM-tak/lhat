// L^ (lhat) -- LSP server: lhat-lsp.json.

#include "settings.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "util.h"

struct LspSettings {
    char **exclude;  // owned, normalised at parse
    size_t exclude_count;
    char **force;  // owned, normalised at parse; named files, never patterns
    size_t force_count;
    int strict;  // -1 unwritten, 0 false, 1 true
};

// One pattern as the matcher wants it: '/'-separated, no leading "./" or
// '/', no trailing '/'. Done once here rather than in the matcher, which
// runs per directory entry -- and it is what makes "build" and "build/" the
// same pattern rather than two rules to keep in step.
static char *normalise_pattern(const char *text)
{
    while (text[0] == '.' && text[1] == '/') {
        text += 2;
    }
    while (*text == '/' || *text == '\\') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '/' || text[length - 1] == '\\')) {
        length--;
    }
    if (length == 0) {
        return NULL;
    }
    char *copy = lsp_strndup(text, length);
    if (copy == NULL) {
        return NULL;
    }
    for (char *at = copy; *at != '\0'; at++) {
        if (*at == '\\') {
            *at = '/';
        }
    }
    return copy;
}

// One named file. The same tidying a pattern gets, and then the one rule a
// pattern does not need: a ".." segment is refused rather than kept. Every
// other spelling that names nothing under the root simply names nothing, and
// the server says so when it cannot find it -- but a ".." can name a real
// file by a path this server folds nowhere, which would make one file two
// roots (the hazard scan_dir's comment describes).
static char *normalise_named_file(const char *text)
{
    char *path = normalise_pattern(text);
    if (path == NULL) {
        return NULL;
    }
    for (const char *at = path; at != NULL; at = strchr(at + 1, '/')) {
        const char *segment = at[0] == '/' ? at + 1 : at;
        if (segment[0] == '.' && segment[1] == '.' &&
            (segment[2] == '/' || segment[2] == '\0')) {
            free(path);
            return NULL;
        }
    }
    return path;
}

// `p` against `r`, both '/'-separated. Answers true for the path a pattern
// names and for anything under it, which is the whole of the "a directory
// and its contents" rule -- so a scan tests a directory and a file with the
// same call.
static bool glob_match(const char *p, const char *r)
{
    while (*p != '\0') {
        if (p[0] == '*' && p[1] == '*') {
            const char *rest = p + 2;
            if (*rest == '/') {
                // "**/name" is also "name": the segments it stands for may
                // be none, and then the '/' it was written with is not there
                // to be matched.
                rest++;
            }
            for (const char *at = r;; at++) {
                if (glob_match(rest, at)) {
                    return true;
                }
                if (*at == '\0') {
                    return false;
                }
            }
        }
        if (p[0] == '*') {
            const char *rest = p + 1;
            for (const char *at = r;; at++) {
                if (glob_match(rest, at)) {
                    return true;
                }
                // Tested after the attempt, so a split exactly at a '/' is
                // allowed -- that is the ancestor case, where the rest of
                // the pattern is empty and the tail below answers.
                if (*at == '\0' || *at == '/') {
                    return false;
                }
            }
        }
        // Everything else is literal, '!' and '?' and '[' included: there is
        // no negation and no character class to get wrong.
        if (*r == '\0' || *r != *p) {
            return false;
        }
        p++;
        r++;
    }
    return *r == '\0' || *r == '/';
}

LspSettings *lsp_settings_parse(const char *text, size_t length)
{
    if (text == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (root == NULL) {
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    LspSettings *settings = (LspSettings *)calloc(1, sizeof *settings);
    if (settings == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    settings->strict = -1;

    const cJSON *strict = cJSON_GetObjectItemCaseSensitive(root, "strict");
    if (cJSON_IsBool(strict)) {
        settings->strict = cJSON_IsTrue(strict) ? 1 : 0;
    }

    const cJSON *forced =
        cJSON_GetObjectItemCaseSensitive(root, "force_include_files");
    if (cJSON_IsArray(forced)) {
        int count = cJSON_GetArraySize(forced);
        if (count > 0) {
            settings->force = (char **)calloc((size_t)count,
                                              sizeof *settings->force);
        }
        for (int i = 0; settings->force != NULL && i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(forced, i);
            if (!cJSON_IsString(item) || item->valuestring == NULL) {
                continue;
            }
            char *named = normalise_named_file(item->valuestring);
            if (named != NULL) {
                settings->force[settings->force_count++] = named;
            }
        }
    }

    const cJSON *exclude = cJSON_GetObjectItemCaseSensitive(root, "exclude");
    if (cJSON_IsArray(exclude)) {
        int count = cJSON_GetArraySize(exclude);
        if (count > 0) {
            settings->exclude = (char **)calloc((size_t)count,
                                                sizeof *settings->exclude);
        }
        for (int i = 0; settings->exclude != NULL && i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(exclude, i);
            if (!cJSON_IsString(item) || item->valuestring == NULL) {
                continue;  // an entry of the wrong shape loses itself, no more
            }
            char *pattern = normalise_pattern(item->valuestring);
            if (pattern != NULL) {
                settings->exclude[settings->exclude_count++] = pattern;
            }
        }
    }

    cJSON_Delete(root);
    return settings;
}

void lsp_settings_free(LspSettings *settings)
{
    if (settings == NULL) {
        return;
    }
    for (size_t i = 0; i < settings->exclude_count; i++) {
        free(settings->exclude[i]);
    }
    free(settings->exclude);
    for (size_t i = 0; i < settings->force_count; i++) {
        free(settings->force[i]);
    }
    free(settings->force);
    free(settings);
}

bool lsp_settings_strict(const LspSettings *settings, bool fallback)
{
    if (settings == NULL || settings->strict < 0) {
        return fallback;
    }
    return settings->strict == 1;
}

size_t lsp_settings_exclude_count(const LspSettings *settings)
{
    return settings != NULL ? settings->exclude_count : 0;
}

size_t lsp_settings_force_count(const LspSettings *settings)
{
    return settings != NULL ? settings->force_count : 0;
}

const char *lsp_settings_force_at(const LspSettings *settings, size_t index)
{
    if (settings == NULL || index >= settings->force_count) {
        return NULL;
    }
    return settings->force[index];
}

bool lsp_settings_excludes_relative(const LspSettings *settings,
                                    const char *relative)
{
    if (settings == NULL || relative == NULL) {
        return false;
    }
    // Named before matched: what was named by hand outranks what a pattern
    // swept up, and no order among the patterns can change that.
    for (size_t i = 0; i < settings->force_count; i++) {
        if (strcmp(settings->force[i], relative) == 0) {
            return false;
        }
    }
    for (size_t i = 0; i < settings->exclude_count; i++) {
        if (glob_match(settings->exclude[i], relative)) {
            return true;
        }
    }
    return false;
}

bool lsp_settings_excludes_path(const LspSettings *settings,
                                const char *root_path, const char *path)
{
    if (settings == NULL || root_path == NULL || path == NULL) {
        return false;
    }
    size_t length = strlen(root_path);
    while (length > 0 && root_path[length - 1] == '/') {
        length--;  // a root of "c:/" leaves "c:", so the '/' below is the one
    }
    // The '/' test is what keeps a root of "c:/repo" from swallowing
    // "c:/repository/x.lh", which the prefix alone would.
    if (strncmp(path, root_path, length) != 0 || path[length] != '/') {
        return false;
    }
    return lsp_settings_excludes_relative(settings, path + length + 1);
}
