// L^ (lhat) -- LSP server: independently configured projects in a workspace.

#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

#include "diagnostics.h"
#include "lton.h"
#include "util.h"

#define LSP_HOST_CONFIG_NAME "lhat-host.json"
#define LSP_SETTINGS_NAME "lhat-lsp.json"

// ---------------------------------------------------------------------------
// Paths and project lookup
// ---------------------------------------------------------------------------

// `path` is root itself or lies below it on a path-component boundary. The
// server's path keys are URI-derived, forward-slashed absolute paths.
static bool path_is_under(const char *path, const char *root)
{
    if (path == NULL || root == NULL) {
        return false;
    }
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') {
        length--;
    }
    if (strncmp(path, root, length) != 0) {
        return false;
    }
    return path[length] == '\0' || path[length] == '/';
}

static const char *workspace_root_for_path(const LspWorkspace *ws,
                                            const char *path)
{
    const char *best = NULL;
    size_t best_length = 0;
    for (size_t i = 0; i < ws->workspace_count; i++) {
        const char *candidate = ws->workspace_paths[i];
        if (path_is_under(path, candidate) && strlen(candidate) > best_length) {
            best = candidate;
            best_length = strlen(candidate);
        }
    }
    return best;
}

static LspProject *project_at_root(LspWorkspace *ws, const char *root)
{
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        if (strcmp(project->root_path, root) == 0) {
            return project;
        }
    }
    return NULL;
}

// The nearest project boundary containing `path`, restricted to the longest
// workspace folder containing it. The fallback project at that folder makes
// this non-NULL for every in-workspace path after discovery.
static LspProject *project_for_path(LspWorkspace *ws, const char *path)
{
    const char *workspace_root = workspace_root_for_path(ws, path);
    if (workspace_root == NULL) {
        LspProject *best = NULL;
        size_t best_length = 0;
        for (LspProject *project = ws->projects; project != NULL;
             project = project->next) {
            if (project->unscoped && path_is_under(path, project->root_path) &&
                strlen(project->root_path) > best_length) {
                best = project;
                best_length = strlen(project->root_path);
            }
        }
        return best;
    }
    LspProject *best = NULL;
    size_t best_length = 0;
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        if (!path_is_under(project->root_path, workspace_root) ||
            !path_is_under(path, project->root_path)) {
            continue;
        }
        size_t length = strlen(project->root_path);
        if (length > best_length) {
            best = project;
            best_length = length;
        }
    }
    return best;
}

static char *path_under_root(const char *root, const char *name)
{
    if (root == NULL || name == NULL) {
        return NULL;
    }
    size_t root_length = strlen(root);
    bool needs_slash = root_length == 0 || root[root_length - 1] != '/';
    size_t name_length = strlen(name);
    char *path = (char *)malloc(root_length + (needs_slash ? 1 : 0) +
                                name_length + 1);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, root, root_length);
    size_t at = root_length;
    if (needs_slash) {
        path[at++] = '/';
    }
    memcpy(path + at, name, name_length + 1);
    return path;
}

static char *directory_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return NULL;
    }
    if (slash == path) {
        return lsp_strndup(path, 1);
    }
    return lsp_strndup(path, (size_t)(slash - path));
}

static bool has_file_name(const char *path, const char *name)
{
    size_t path_length = strlen(path);
    size_t name_length = strlen(name);
    return path_length >= name_length &&
           strcmp(path + path_length - name_length, name) == 0 &&
           (path_length == name_length || path[path_length - name_length - 1] == '/');
}

static bool file_or_open_document_exists(LspWorkspace *ws, const char *path)
{
    if (lsp_document_store_has(&ws->documents, path)) {
        return true;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

static bool directory_has_config(LspWorkspace *ws, const char *dir)
{
    char *host = path_under_root(dir, LSP_HOST_CONFIG_NAME);
    char *settings = path_under_root(dir, LSP_SETTINGS_NAME);
    bool found = (host != NULL && file_or_open_document_exists(ws, host)) ||
                 (settings != NULL && file_or_open_document_exists(ws, settings));
    free(host);
    free(settings);
    return found;
}

// ---------------------------------------------------------------------------
// The loader and host registrations
// ---------------------------------------------------------------------------

static char *lsp_program_load(void *context, const char *path, size_t *length)
{
    LspWorkspace *ws = (LspWorkspace *)context;
    char *text = lsp_document_store_copy(&ws->documents, path, length);
    if (text == NULL) {
        text = lhat_load_file(NULL, path, length);
    }
    if (text != NULL && lsp_lton_is_path(path)) {
        size_t whole = 0;
        char *wrapped = lsp_lton_wrap(text, *length, &whole);
        lhat_free(text);
        if (wrapped == NULL) {
            return NULL;
        }
        *length = whole;
        return wrapped;
    }
    return text;
}

static void lsp_stub_host_fn(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count,
                             LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
}

static void bind_host_names(const LspProject *project, LhatProgram *program)
{
    if (project != NULL && project->host_config != NULL) {
        lsp_host_config_apply(project->host_config, program);
        return;
    }
    lhat_register_global(program, "print", "f^...->nil^;", lsp_stub_host_fn, NULL);
    lhat_bind_initial(program, "print", "L^.print");
    lhat_bind_initial(program, "collectgarbage", "L^.collectgarbage");
}

static char *read_config_text(LspWorkspace *ws, const char *path,
                              size_t *length)
{
    char *text = lsp_document_store_copy(&ws->documents, path, length);
    return text != NULL ? text : lhat_load_file(NULL, path, length);
}

static void load_project_configs(LspWorkspace *ws, LspProject *project)
{
    char *host_path = path_under_root(project->root_path, LSP_HOST_CONFIG_NAME);
    char *settings_path = path_under_root(project->root_path, LSP_SETTINGS_NAME);

    size_t length = 0;
    char *text = host_path != NULL ? read_config_text(ws, host_path, &length) : NULL;
    project->host_config = text != NULL ? lsp_host_config_parse(text, length) : NULL;
    lhat_free(text);

    length = 0;
    text = settings_path != NULL ? read_config_text(ws, settings_path, &length) : NULL;
    project->settings = text != NULL ? lsp_settings_parse(text, length) : NULL;
    lhat_free(text);

    free(host_path);
    free(settings_path);
}

// ---------------------------------------------------------------------------
// Roots and their reverse index -- all private to one project
// ---------------------------------------------------------------------------

static LspReverseEntry *reverse_find(LspProject *project, const char *path)
{
    for (LspReverseEntry *entry = project->reverse; entry != NULL;
         entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            return entry;
        }
    }
    return NULL;
}

static LspReverseEntry *reverse_find_or_add(LspProject *project,
                                             const char *path)
{
    LspReverseEntry *entry = reverse_find(project, path);
    if (entry != NULL) {
        return entry;
    }
    entry = (LspReverseEntry *)calloc(1, sizeof *entry);
    if (entry == NULL) {
        return NULL;
    }
    entry->path = lsp_strdup(path);
    if (entry->path == NULL) {
        free(entry);
        return NULL;
    }
    entry->next = project->reverse;
    project->reverse = entry;
    return entry;
}

static void reverse_entry_add_root(LspReverseEntry *entry, const char *root)
{
    for (size_t i = 0; i < entry->root_count; i++) {
        if (strcmp(entry->roots[i], root) == 0) {
            return;
        }
    }
    if (entry->root_count == entry->root_capacity) {
        size_t grown = entry->root_capacity ? entry->root_capacity * 2 : 4;
        char **bigger = (char **)realloc(entry->roots, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        entry->roots = bigger;
        entry->root_capacity = grown;
    }
    entry->roots[entry->root_count] = lsp_strdup(root);
    if (entry->roots[entry->root_count] != NULL) {
        entry->root_count++;
    }
}

static void reverse_entry_remove_root(LspReverseEntry *entry, const char *root)
{
    for (size_t i = 0; i < entry->root_count; i++) {
        if (strcmp(entry->roots[i], root) == 0) {
            free(entry->roots[i]);
            entry->roots[i] = entry->roots[entry->root_count - 1];
            entry->root_count--;
            return;
        }
    }
}

static void reverse_remove_root_everywhere(LspProject *project,
                                           const char *root)
{
    for (LspReverseEntry *entry = project->reverse; entry != NULL;
         entry = entry->next) {
        reverse_entry_remove_root(entry, root);
    }
}

static LspRoot *root_find(LspProject *project, const char *path)
{
    for (LspRoot *root = project->roots; root != NULL; root = root->next) {
        if (strcmp(root->path, path) == 0) {
            return root;
        }
    }
    return NULL;
}

static LspRoot *root_find_or_add(LspProject *project, const char *path)
{
    LspRoot *root = root_find(project, path);
    if (root != NULL) {
        return root;
    }
    root = (LspRoot *)calloc(1, sizeof *root);
    if (root == NULL) {
        return NULL;
    }
    root->path = lsp_strdup(path);
    if (root->path == NULL) {
        free(root);
        return NULL;
    }
    root->next = project->roots;
    project->roots = root;
    return root;
}

static void recheck_one_root(LspWorkspace *ws, LspProject *project,
                             LspRoot *root)
{
    reverse_remove_root_everywhere(project, root->path);
    if (root->checked) {
        lhat_program_dispose(&root->program);
    }
    lhat_program_init(&root->program, true, lsp_program_load, ws);
    bind_host_names(project, &root->program);
    lhat_program_check(&root->program, root->path);

    bool clean = true;
    for (const LhatUnit *unit = root->program.units; clean && unit != NULL;
         unit = unit->next) {
        clean = lhat_unit_diagnostic_count(unit) == 0;
    }
    if (clean) {
        (void)lhat_program_compile(&root->program);
    }
    root->checked = true;

    for (const LhatUnit *unit = root->program.units; unit != NULL;
         unit = unit->next) {
        LspReverseEntry *entry = reverse_find_or_add(project, unit->path);
        if (entry != NULL) {
            reverse_entry_add_root(entry, root->path);
        }
    }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

static bool has_lh_extension(const char *path)
{
    size_t length = strlen(path);
    return length > 3 && strcmp(path + length - 3, ".lh") == 0;
}

static bool is_checkable(const char *path)
{
    return has_lh_extension(path) || lsp_lton_is_path(path);
}

static bool path_excluded(const LspProject *project, const char *path)
{
    return lsp_settings_excludes_path(project->settings, project->root_path, path);
}

static void drop_excluded_roots(LspProject *project)
{
    LspRoot **link = &project->roots;
    while (*link != NULL) {
        LspRoot *root = *link;
        if (!path_excluded(project, root->path)) {
            link = &root->next;
            continue;
        }
        reverse_remove_root_everywhere(project, root->path);
        *link = root->next;
        if (root->checked) {
            lhat_program_dispose(&root->program);
        }
        free(root->path);
        free(root);
    }
}

// A subdirectory that begins another project is not scanned by this one.
static bool starts_child_project(LspWorkspace *ws, const LspProject *project,
                                 const char *directory)
{
    return strcmp(directory, project->root_path) != 0 &&
           project_at_root(ws, directory) != NULL;
}

#ifdef _WIN32
static void scan_project_dir(LspWorkspace *ws, LspProject *project,
                             const char *dir)
{
    char pattern[MAX_PATH];
    if ((size_t)snprintf(pattern, sizeof pattern, "%s/*", dir) >= sizeof pattern) {
        return;
    }
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (data.cFileName[0] == '.') {
            continue;
        }
        char child[MAX_PATH];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir,
                             data.cFileName) >= sizeof child) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!starts_child_project(ws, project, child) &&
                !path_excluded(project, child)) {
                scan_project_dir(ws, project, child);
            }
        } else if (!path_excluded(project, child) && is_checkable(child)) {
            root_find_or_add(project, child);
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
}

static void discover_config_dirs_in(LspWorkspace *ws, const char *workspace,
                                    const char *dir)
{
    char pattern[MAX_PATH];
    if ((size_t)snprintf(pattern, sizeof pattern, "%s/*", dir) >= sizeof pattern) {
        return;
    }
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (data.cFileName[0] == '.') {
            continue;
        }
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        char child[MAX_PATH];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir,
                             data.cFileName) >= sizeof child) {
            continue;
        }
        if (workspace_root_for_path(ws, child) != workspace) {
            continue;
        }
        if (directory_has_config(ws, child) && project_at_root(ws, child) == NULL) {
            LspProject *project = (LspProject *)calloc(1, sizeof *project);
            if (project != NULL) {
                project->root_path = lsp_strdup(child);
                if (project->root_path != NULL) {
                    project->config_boundary = true;
                    project->next = ws->projects;
                    ws->projects = project;
                } else {
                    free(project);
                }
            }
        }
        discover_config_dirs_in(ws, workspace, child);
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
}
#else
static void scan_project_dir(LspWorkspace *ws, LspProject *project,
                             const char *dir)
{
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[4096];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir,
                             entry->d_name) >= sizeof child) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!starts_child_project(ws, project, child) &&
                !path_excluded(project, child)) {
                scan_project_dir(ws, project, child);
            }
        } else if (!path_excluded(project, child) && is_checkable(child)) {
            root_find_or_add(project, child);
        }
    }
    closedir(handle);
}

static void discover_config_dirs_in(LspWorkspace *ws, const char *workspace,
                                    const char *dir)
{
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[4096];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir,
                             entry->d_name) >= sizeof child) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (workspace_root_for_path(ws, child) != workspace) {
            continue;
        }
        if (directory_has_config(ws, child) && project_at_root(ws, child) == NULL) {
            LspProject *project = (LspProject *)calloc(1, sizeof *project);
            if (project != NULL) {
                project->root_path = lsp_strdup(child);
                if (project->root_path != NULL) {
                    project->config_boundary = true;
                    project->next = ws->projects;
                    ws->projects = project;
                } else {
                    free(project);
                }
            }
        }
        discover_config_dirs_in(ws, workspace, child);
    }
    closedir(handle);
}
#endif

static LspProject *add_project(LspWorkspace *ws, const char *root)
{
    LspProject *existing = project_at_root(ws, root);
    if (existing != NULL) {
        return existing;
    }
    LspProject *project = (LspProject *)calloc(1, sizeof *project);
    if (project == NULL) {
        return NULL;
    }
    project->root_path = lsp_strdup(root);
    if (project->root_path == NULL) {
        free(project);
        return NULL;
    }
    project->next = ws->projects;
    ws->projects = project;
    return project;
}

static LspProject *add_unscoped_project_for_path(LspWorkspace *ws,
                                                  const char *path)
{
    char *directory = directory_of(path);
    if (directory == NULL) {
        return NULL;
    }
    LspProject *project = project_at_root(ws, directory);
    if (project == NULL) {
        project = add_project(ws, directory);
        if (project != NULL) {
            project->unscoped = true;
        }
    }
    free(directory);
    return project != NULL && project->unscoped ? project : NULL;
}

static void discover_project_roots(LspWorkspace *ws, LspProject *project)
{
    drop_excluded_roots(project);
    scan_project_dir(ws, project, project->root_path);
    for (size_t i = 0; i < lsp_settings_force_count(project->settings); i++) {
        char *named = path_under_root(project->root_path,
                                      lsp_settings_force_at(project->settings, i));
        if (named != NULL && project_for_path(ws, named) == project &&
            lsp_workspace_is_unit_path(named)) {
            root_find_or_add(project, named);
        }
        free(named);
    }
}

// The full discovery pass is what makes background diagnostics cover every
// project. Requests for a newly opened file also resolve its own ancestors:
// that path is both cheaper and more exact for a directory the background
// scan intentionally skips (for example, a hidden one).
static LspProject *ensure_project_for_path(LspWorkspace *ws, const char *path)
{
    const char *workspace_root = workspace_root_for_path(ws, path);
    if (workspace_root == NULL) {
        return project_for_path(ws, path);
    }
    char *directory = directory_of(path);
    while (directory != NULL && path_is_under(directory, workspace_root)) {
        if (directory_has_config(ws, directory)) {
            LspProject *project = project_at_root(ws, directory);
            if (project == NULL) {
                project = add_project(ws, directory);
                if (project != NULL) {
                    project->config_boundary = true;
                    load_project_configs(ws, project);
                }
            } else if (!project->config_boundary) {
                project->config_boundary = true;
                load_project_configs(ws, project);
            }
            free(directory);
            return project;
        }
        if (strcmp(directory, workspace_root) == 0) {
            break;
        }
        char *parent = directory_of(directory);
        free(directory);
        directory = parent;
    }
    free(directory);
    return project_for_path(ws, path);
}

// ---------------------------------------------------------------------------
// Lifetime and project refresh
// ---------------------------------------------------------------------------

static void dispose_project(LspProject *project)
{
    LspRoot *root = project->roots;
    while (root != NULL) {
        LspRoot *next = root->next;
        if (root->checked) {
            lhat_program_dispose(&root->program);
        }
        free(root->path);
        free(root);
        root = next;
    }
    LspReverseEntry *entry = project->reverse;
    while (entry != NULL) {
        LspReverseEntry *next = entry->next;
        for (size_t i = 0; i < entry->root_count; i++) {
            free(entry->roots[i]);
        }
        free(entry->roots);
        free(entry->path);
        free(entry);
        entry = next;
    }
    lsp_host_config_free(project->host_config);
    lsp_settings_free(project->settings);
    free(project->root_path);
    free(project);
}

static void clear_projects(LspWorkspace *ws)
{
    LspProject *project = ws->projects;
    while (project != NULL) {
        LspProject *next = project->next;
        dispose_project(project);
        project = next;
    }
    ws->projects = NULL;
}

void lsp_workspace_init(LspWorkspace *ws,
                        const char *const *workspace_paths,
                        size_t workspace_count)
{
    memset(ws, 0, sizeof *ws);
    lsp_document_store_init(&ws->documents);
    lhat_mutex_init(&ws->lock);
    if (workspace_count == 0 || workspace_paths == NULL) {
        return;
    }
    ws->workspace_paths = (char **)calloc(workspace_count, sizeof *ws->workspace_paths);
    if (ws->workspace_paths == NULL) {
        return;
    }
    for (size_t i = 0; i < workspace_count; i++) {
        if (workspace_paths[i] == NULL) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < ws->workspace_count; j++) {
            duplicate = strcmp(ws->workspace_paths[j], workspace_paths[i]) == 0;
            if (duplicate) {
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        ws->workspace_paths[ws->workspace_count] = lsp_strdup(workspace_paths[i]);
        if (ws->workspace_paths[ws->workspace_count] != NULL) {
            ws->workspace_count++;
        }
    }
}

void lsp_workspace_dispose(LspWorkspace *ws)
{
    clear_projects(ws);
    for (size_t i = 0; i < ws->workspace_count; i++) {
        free(ws->workspace_paths[i]);
    }
    free(ws->workspace_paths);
    lsp_document_store_dispose(&ws->documents);
    lhat_mutex_destroy(&ws->lock);
}

void lsp_workspace_discover_projects(LspWorkspace *ws)
{
    lhat_mutex_lock(&ws->lock);
    clear_projects(ws);
    for (size_t i = 0; i < ws->workspace_count; i++) {
        // Always install the folder's fallback project. If it has a config,
        // it is also the nearest configuration project for files below it.
        add_project(ws, ws->workspace_paths[i]);
        discover_config_dirs_in(ws, ws->workspace_paths[i], ws->workspace_paths[i]);
    }
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        project->config_boundary =
            directory_has_config(ws, project->root_path);
        load_project_configs(ws, project);
    }
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        discover_project_roots(ws, project);
    }
    lhat_mutex_unlock(&ws->lock);
}

bool lsp_workspace_is_config_path(const LspWorkspace *ws, const char *path)
{
    return path != NULL && workspace_root_for_path(ws, path) != NULL &&
           (has_file_name(path, LSP_HOST_CONFIG_NAME) ||
            has_file_name(path, LSP_SETTINGS_NAME));
}

bool lsp_workspace_is_unit_path(const char *path)
{
    return path != NULL && is_checkable(path);
}

bool lsp_workspace_is_binary_unit(const char *path)
{
    if (path == NULL) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    char head[8];
    size_t read = fread(head, 1, sizeof head, file);
    fclose(file);
    return lhat_program_is_binary_unit(head, read);
}

// ---------------------------------------------------------------------------
// Rechecking and diagnostics
// ---------------------------------------------------------------------------

void lsp_workspace_recheck_affected(LspWorkspace *ws, const char *path)
{
    if (!lsp_workspace_is_unit_path(path)) {
        return;
    }
    lhat_mutex_lock(&ws->lock);
    LspProject *owner = ensure_project_for_path(ws, path);
    if (owner == NULL && workspace_root_for_path(ws, path) == NULL) {
        owner = add_unscoped_project_for_path(ws, path);
    }
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        bool should_add_own_root = project == owner && !path_excluded(project, path);
        LspReverseEntry *entry = reverse_find(project, path);
        if (!should_add_own_root && entry == NULL) {
            continue;
        }
        // recheck_one_root rebuilds `entry`, so copy the root names before
        // rechecking this file's own root.
        size_t count = entry != NULL ? entry->root_count : 0;
        char **affected = count ? (char **)calloc(count, sizeof *affected) : NULL;
        size_t copied = 0;
        if (affected != NULL) {
            for (size_t i = 0; i < count; i++) {
                affected[copied] = lsp_strdup(entry->roots[i]);
                if (affected[copied] != NULL) {
                    copied++;
                }
            }
        }
        if (should_add_own_root) {
            LspRoot *root = root_find_or_add(project, path);
            if (root != NULL) {
                recheck_one_root(ws, project, root);
            }
        }
        for (size_t i = 0; i < copied; i++) {
            if (strcmp(affected[i], path) != 0) {
                LspRoot *root = root_find_or_add(project, affected[i]);
                if (root != NULL) {
                    recheck_one_root(ws, project, root);
                }
            }
            free(affected[i]);
        }
        free(affected);
    }
    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_recheck_all(LspWorkspace *ws)
{
    lhat_mutex_lock(&ws->lock);
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        for (LspRoot *root = project->roots; root != NULL; root = root->next) {
            recheck_one_root(ws, project, root);
        }
    }
    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_collect_diagnostics(LspWorkspace *ws,
                                       LspDiagnosticsSink sink, void *context)
{
    lhat_mutex_lock(&ws->lock);
    char **seen = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (LspProject *project = ws->projects; project != NULL;
         project = project->next) {
        for (LspRoot *root = project->roots; root != NULL; root = root->next) {
            if (!root->checked) {
                continue;
            }
            for (const LhatUnit *unit = root->program.units; unit != NULL;
                 unit = unit->next) {
                LspProject *owner = unit->loaded ? project_for_path(ws, unit->path) : NULL;
                if (!unit->loaded || unit->path == NULL ||
                    (owner != NULL && owner != project)) {
                    continue;
                }
                bool already = false;
                for (size_t i = 0; i < count && !already; i++) {
                    already = strcmp(seen[i], unit->path) == 0;
                }
                if (already) {
                    continue;
                }
                if (count == capacity) {
                    size_t grown = capacity ? capacity * 2 : 16;
                    char **bigger = (char **)realloc(seen, grown * sizeof *bigger);
                    if (bigger == NULL) {
                        continue;
                    }
                    seen = bigger;
                    capacity = grown;
                }
                seen[count++] = unit->path;
                bool relaxed = !lsp_settings_strict(
                    project->settings,
                    lsp_host_config_strict(project->host_config, true));
                cJSON *diagnostics = lsp_diagnostics_for_unit(unit, relaxed);
                const char *failed_in = NULL;
                LhatCompileResult failure =
                    lhat_program_compile_failure(&root->program, &failed_in);
                if (failed_in != NULL && strcmp(failed_in, unit->path) == 0) {
                    lsp_diagnostics_add_compile_failure(diagnostics, unit, failure);
                }
                sink(context, unit->path, diagnostics);
            }
        }
    }
    free(seen);
    lhat_mutex_unlock(&ws->lock);
}

// ---------------------------------------------------------------------------
// Queries scoped to the requesting document's project
// ---------------------------------------------------------------------------

static bool with_unit_in_project(LspProject *project, const char *path,
                                 LspUnitSink sink, void *context)
{
    if (project == NULL) {
        return false;
    }
    for (LspRoot *root = project->roots; root != NULL; root = root->next) {
        if (!root->checked) {
            continue;
        }
        for (const LhatUnit *unit = root->program.units; unit != NULL;
             unit = unit->next) {
            if (unit->loaded && strcmp(unit->path, path) == 0) {
                sink(context, unit);
                return true;
            }
        }
    }
    return false;
}

void lsp_workspace_with_unit(LspWorkspace *ws, const char *path,
                             LspUnitSink sink, void *context)
{
    lhat_mutex_lock(&ws->lock);
    LspProject *owner = ensure_project_for_path(ws, path);
    bool found = with_unit_in_project(owner, path, sink, context);
    // A project may require^ an excluded file or one outside every workspace
    // folder. Its own project has no checked root for that file, but the
    // requiring project's checked copy is still useful for a location query.
    if (!found) {
        for (LspProject *project = ws->projects; project != NULL && !found;
             project = project->next) {
            if (project == owner) {
                continue;
            }
            found = with_unit_in_project(project, path, sink, context);
        }
    }
    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_with_fresh_unit(LspWorkspace *ws, const char *path,
                                   LspUnitSink sink, void *context)
{
    if (path == NULL) {
        return;
    }
    lhat_mutex_lock(&ws->lock);
    LspProject *project = ensure_project_for_path(ws, path);
    LhatProgram program;
    lhat_program_init(&program, true, lsp_program_load, ws);
    bind_host_names(project, &program);
    const LhatUnit *unit = lhat_program_check(&program, path);
    if (unit != NULL && unit->loaded) {
        sink(context, unit);
    }
    lhat_program_dispose(&program);
    lhat_mutex_unlock(&ws->lock);
}

static void with_every_unit_in_project(LspProject *project, LspUnitSink sink,
                                        void *context)
{
    char **seen = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (LspRoot *root = project != NULL ? project->roots : NULL; root != NULL;
         root = root->next) {
        if (!root->checked) {
            continue;
        }
        for (const LhatUnit *unit = root->program.units; unit != NULL;
             unit = unit->next) {
            if (!unit->loaded || unit->path == NULL) {
                continue;
            }
            bool already = false;
            for (size_t i = 0; i < count && !already; i++) {
                already = strcmp(seen[i], unit->path) == 0;
            }
            if (already) {
                continue;
            }
            if (count == capacity) {
                size_t grown = capacity ? capacity * 2 : 16;
                char **bigger = (char **)realloc(seen, grown * sizeof *bigger);
                if (bigger == NULL) {
                    continue;
                }
                seen = bigger;
                capacity = grown;
            }
            seen[count++] = unit->path;
            sink(context, unit);
        }
    }
    free(seen);
}

void lsp_workspace_with_every_unit(LspWorkspace *ws, const char *path,
                                   LspUnitSink sink, void *context)
{
    lhat_mutex_lock(&ws->lock);
    with_every_unit_in_project(ensure_project_for_path(ws, path), sink, context);
    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_free_strings(char **strings, size_t count)
{
    if (strings == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(strings[i]);
    }
    free(strings);
}

char **lsp_workspace_copy_unit_paths(LspWorkspace *ws, const char *path,
                                     size_t *count)
{
    *count = 0;
    lhat_mutex_lock(&ws->lock);
    LspProject *project = ensure_project_for_path(ws, path);
    size_t total = 0;
    for (LspRoot *root = project != NULL ? project->roots : NULL; root != NULL;
         root = root->next) {
        total++;
    }
    char **paths = total ? (char **)calloc(total, sizeof *paths) : NULL;
    if (paths != NULL) {
        for (LspRoot *root = project->roots; root != NULL; root = root->next) {
            char *copy = lsp_strdup(root->path);
            if (copy != NULL) {
                paths[(*count)++] = copy;
            }
        }
    }
    lhat_mutex_unlock(&ws->lock);
    return paths;
}

char **lsp_workspace_copy_module_names(LspWorkspace *ws, const char *path,
                                       size_t *count)
{
    lhat_mutex_lock(&ws->lock);
    LspProject *project = ensure_project_for_path(ws, path);
    char **modules = lsp_host_config_modules(
        project != NULL ? project->host_config : NULL, count);
    lhat_mutex_unlock(&ws->lock);
    return modules;
}

void lsp_workspace_free_exports(LspUnitExports *units, size_t count)
{
    if (units == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(units[i].path);
        free(units[i].module_name);
        lsp_workspace_free_strings(units[i].exports, units[i].export_count);
    }
    free(units);
}

static bool copy_exports_of(const LhatUnit *unit, LspUnitExports *into)
{
    const char *module = lhat_unit_module_name(unit);
    if (module == NULL || *module == '\0') {
        return false;
    }
    size_t count = lhat_unit_export_count(unit);
    memset(into, 0, sizeof *into);
    into->path = lsp_strdup(unit->path);
    into->module_name = lsp_strdup(module);
    into->exports = count ? (char **)calloc(count, sizeof *into->exports) : NULL;
    if (into->path == NULL || into->module_name == NULL ||
        (count > 0 && into->exports == NULL)) {
        free(into->path);
        free(into->module_name);
        free(into->exports);
        memset(into, 0, sizeof *into);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        LhatUnitText name = lhat_unit_export_name(unit, i);
        char *copy = name.text != NULL ? lsp_strndup(name.text, name.length) : NULL;
        if (copy != NULL) {
            into->exports[into->export_count++] = copy;
        }
    }
    return true;
}

LspUnitExports *lsp_workspace_copy_exports(LspWorkspace *ws,
                                            const char *path, size_t *count)
{
    *count = 0;
    lhat_mutex_lock(&ws->lock);
    LspProject *project = ensure_project_for_path(ws, path);
    LspUnitExports *units = NULL;
    size_t capacity = 0;
    for (LspRoot *root = project != NULL ? project->roots : NULL; root != NULL;
         root = root->next) {
        if (!root->checked) {
            continue;
        }
        for (const LhatUnit *unit = root->program.units; unit != NULL;
             unit = unit->next) {
            if (!unit->loaded || unit->path == NULL) {
                continue;
            }
            bool already = false;
            for (size_t i = 0; i < *count && !already; i++) {
                already = strcmp(units[i].path, unit->path) == 0;
            }
            if (already) {
                continue;
            }
            if (*count == capacity) {
                size_t grown = capacity ? capacity * 2 : 8;
                LspUnitExports *bigger =
                    (LspUnitExports *)realloc(units, grown * sizeof *bigger);
                if (bigger == NULL) {
                    continue;
                }
                units = bigger;
                capacity = grown;
            }
            if (copy_exports_of(unit, &units[*count])) {
                (*count)++;
            }
        }
    }
    lhat_mutex_unlock(&ws->lock);
    return units;
}
