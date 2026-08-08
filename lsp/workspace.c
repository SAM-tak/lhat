// L^ (lhat) -- LSP server: the workspace's roots, checked and re-checked.

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

#include "port.h"   // lhat_load_file
#include "value.h"  // lhat_nil
#include "vm.h"     // LhatMachine, LhatHostFn's shape

#include "diagnostics.h"
#include "util.h"

// ---------------------------------------------------------------------------
// The loader: the open documents first, disk next.
// ---------------------------------------------------------------------------
//
// Every path this server hands to program.h is an absolute one (uri.h's
// lsp_uri_to_absolute_path is the only place a URI is turned into one).
// program.c's normalise_path only unifies separators and folds '.'/'..' --
// it never rewrites an absolute path into a relative one -- and
// resolve_against joins a require^'s relative text onto the *last* segment
// of whoever wrote it, so require^ resolution stays absolute automatically.
// That also means single-file mode (no workspace folder, root_path == NULL)
// still resolves a require^ against disk correctly: nothing here needs to
// know a workspace root at all.

static char *lsp_program_load(void *context, const char *path, size_t *length)
{
    LspWorkspace *ws = (LspWorkspace *)context;
    char *overlay = lsp_document_store_copy(&ws->documents, path, length);
    if (overlay != NULL) {
        return overlay;
    }
    return lhat_load_file(NULL, path, length);
}

// 05 の 8.2/8.7: the checker has to know a host's names before it checks
// anything. This server never runs a program, so `call` is never invoked --
// it exists only because lhat_register_global requires one.
static LhatValue lsp_stub_host_fn(LhatMachine *machine, void *context,
                                  const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_nil();
}

// The same minimum cli/main.c's bind_host_names registers -- the same
// baseline the file/prompt defaults of 03 の 5 章 use. A script using a
// host's own API beyond this sees an undefined name here: this server has
// no way to learn what a real embedding registered, so anything past
// print/collectgarbage is a known false positive for now.
static void bind_host_names(LhatProgram *program)
{
    lhat_register_global(program, "print", "f^string^ -> nil^;",
                         lsp_stub_host_fn, NULL);
    lhat_bind_initial(program, "print", "L^.print");
    lhat_bind_initial(program, "collectgarbage", "L^.collectgarbage");
}

// ---------------------------------------------------------------------------
// The reverse index: path -> the roots whose last check reached it.
// ---------------------------------------------------------------------------

static LspReverseEntry *reverse_find(LspWorkspace *ws, const char *path)
{
    for (LspReverseEntry *e = ws->reverse; e != NULL; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            return e;
        }
    }
    return NULL;
}

static LspReverseEntry *reverse_find_or_add(LspWorkspace *ws, const char *path)
{
    LspReverseEntry *e = reverse_find(ws, path);
    if (e != NULL) {
        return e;
    }
    e = (LspReverseEntry *)malloc(sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    e->path = lsp_strdup(path);
    e->roots = NULL;
    e->root_count = 0;
    e->root_capacity = 0;
    e->next = ws->reverse;
    ws->reverse = e;
    return e;
}

static void reverse_entry_add_root(LspReverseEntry *e, const char *root_path)
{
    for (size_t i = 0; i < e->root_count; i++) {
        if (strcmp(e->roots[i], root_path) == 0) {
            return;
        }
    }
    if (e->root_count == e->root_capacity) {
        size_t grown = e->root_capacity ? e->root_capacity * 2 : 4;
        char **bigger = (char **)realloc(e->roots, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        e->roots = bigger;
        e->root_capacity = grown;
    }
    e->roots[e->root_count++] = lsp_strdup(root_path);
}

static void reverse_entry_remove_root(LspReverseEntry *e, const char *root_path)
{
    for (size_t i = 0; i < e->root_count; i++) {
        if (strcmp(e->roots[i], root_path) == 0) {
            free(e->roots[i]);
            e->roots[i] = e->roots[e->root_count - 1];
            e->root_count--;
            return;
        }
    }
}

// Removes `root_path` from every path it used to reach, ahead of
// re-checking it -- the check that follows rebuilds only what is still true.
static void reverse_remove_root_everywhere(LspWorkspace *ws,
                                           const char *root_path)
{
    for (LspReverseEntry *e = ws->reverse; e != NULL; e = e->next) {
        reverse_entry_remove_root(e, root_path);
    }
}

// ---------------------------------------------------------------------------
// Roots
// ---------------------------------------------------------------------------

static LspRoot *root_find(LspWorkspace *ws, const char *path)
{
    for (LspRoot *r = ws->roots; r != NULL; r = r->next) {
        if (strcmp(r->path, path) == 0) {
            return r;
        }
    }
    return NULL;
}

static LspRoot *root_find_or_add(LspWorkspace *ws, const char *path)
{
    LspRoot *r = root_find(ws, path);
    if (r != NULL) {
        return r;
    }
    r = (LspRoot *)malloc(sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    r->path = lsp_strdup(path);
    r->checked = false;
    r->next = ws->roots;
    ws->roots = r;
    return r;
}

// Re-checks `root` from scratch: a fresh LhatProgram (program.h has no way
// to invalidate one unit inside an existing one -- program.c's check_path
// reuses anything LHAT_UNIT_DONE outright), then rebuilds the reverse index
// entries this root contributes.
static void recheck_one_root(LspWorkspace *ws, LspRoot *root)
{
    reverse_remove_root_everywhere(ws, root->path);

    if (root->checked) {
        lhat_program_dispose(&root->program);
    }
    lhat_program_init(&root->program, true, lsp_program_load, ws);
    bind_host_names(&root->program);
    lhat_program_check(&root->program, root->path);
    root->checked = true;

    // Every unit this root's check reached -- 05 の 6.2 walks require^
    // transitively, so this already covers indirect dependents too: if A
    // requires B requires C, checking A from scratch visits C, and C's
    // reverse entry gains A directly. One hop here is enough.
    for (const LhatUnit *unit = root->program.units; unit != NULL;
         unit = unit->next) {
        LspReverseEntry *e = reverse_find_or_add(ws, unit->path);
        if (e != NULL) {
            reverse_entry_add_root(e, root->path);
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

static void add_lh_file(LspWorkspace *ws, const char *absolute_path)
{
    root_find_or_add(ws, absolute_path);
}

#ifdef _WIN32
// FindFirstFileA/FindNextFileA accept forward slashes as readily as
// backslashes, so this builds every path with '/' -- every other path key
// in this server (didOpen/didChange's uri.h conversion, LspRoot::path, the
// reverse index) is forward-slashed too, and root_find/root_find_or_add
// compare with a plain strcmp. A backslash sneaking in here would make a
// root discovered by scanning fail to match the same file's path from an
// LSP notification, and re-register it as a second, permanently duplicate
// root.
static void scan_dir(LspWorkspace *ws, const char *dir)
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
            continue;  // "." / ".." / hidden dirs such as .git
        }
        char child[MAX_PATH];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir,
                             data.cFileName) >= sizeof child) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(ws, child);
        } else if (has_lh_extension(child)) {
            add_lh_file(ws, child);
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
}
#else
static void scan_dir(LspWorkspace *ws, const char *dir)
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
            scan_dir(ws, child);
        } else if (has_lh_extension(child)) {
            add_lh_file(ws, child);
        }
    }
    closedir(handle);
}
#endif

void lsp_workspace_discover_roots(LspWorkspace *ws)
{
    if (ws->root_path == NULL) {
        return;
    }
    lhat_mutex_lock(&ws->lock);
    scan_dir(ws, ws->root_path);
    lhat_mutex_unlock(&ws->lock);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void lsp_workspace_init(LspWorkspace *ws, const char *root_path)
{
    ws->root_path = root_path != NULL ? lsp_strdup(root_path) : NULL;
    lsp_document_store_init(&ws->documents);
    ws->roots = NULL;
    ws->reverse = NULL;
    lhat_mutex_init(&ws->lock);
}

void lsp_workspace_dispose(LspWorkspace *ws)
{
    LspRoot *r = ws->roots;
    while (r != NULL) {
        LspRoot *next = r->next;
        if (r->checked) {
            lhat_program_dispose(&r->program);
        }
        free(r->path);
        free(r);
        r = next;
    }
    ws->roots = NULL;

    LspReverseEntry *e = ws->reverse;
    while (e != NULL) {
        LspReverseEntry *next = e->next;
        for (size_t i = 0; i < e->root_count; i++) {
            free(e->roots[i]);
        }
        free(e->roots);
        free(e->path);
        free(e);
        e = next;
    }
    ws->reverse = NULL;

    lsp_document_store_dispose(&ws->documents);
    free(ws->root_path);
    lhat_mutex_destroy(&ws->lock);
}

void lsp_workspace_recheck_affected(LspWorkspace *ws, const char *path)
{
    lhat_mutex_lock(&ws->lock);

    // The roots that reached `path` before this change -- collected up
    // front, since recheck_one_root rebuilds the reverse index as it goes.
    char **affected = NULL;
    size_t affected_count = 0;
    LspReverseEntry *entry = reverse_find(ws, path);
    if (entry != NULL && entry->root_count > 0) {
        affected = (char **)malloc(entry->root_count * sizeof *affected);
        if (affected != NULL) {
            for (size_t i = 0; i < entry->root_count; i++) {
                affected[affected_count++] = lsp_strdup(entry->roots[i]);
            }
        }
    }

    LspRoot *own_root = root_find_or_add(ws, path);
    if (own_root != NULL) {
        recheck_one_root(ws, own_root);
    }

    for (size_t i = 0; i < affected_count; i++) {
        if (strcmp(affected[i], path) != 0) {
            LspRoot *r = root_find_or_add(ws, affected[i]);
            if (r != NULL) {
                recheck_one_root(ws, r);
            }
        }
        free(affected[i]);
    }
    free(affected);

    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_recheck_all(LspWorkspace *ws)
{
    lhat_mutex_lock(&ws->lock);
    for (LspRoot *r = ws->roots; r != NULL; r = r->next) {
        recheck_one_root(ws, r);
    }
    lhat_mutex_unlock(&ws->lock);
}

void lsp_workspace_collect_diagnostics(LspWorkspace *ws,
                                       LspDiagnosticsSink sink, void *context)
{
    lhat_mutex_lock(&ws->lock);

    char **seen = NULL;
    size_t seen_count = 0;
    size_t seen_capacity = 0;

    for (LspRoot *r = ws->roots; r != NULL; r = r->next) {
        if (!r->checked) {
            continue;
        }
        for (const LhatUnit *unit = r->program.units; unit != NULL;
             unit = unit->next) {
            if (!unit->loaded) {
                continue;
            }
            bool already = false;
            for (size_t i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], unit->path) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (seen_count == seen_capacity) {
                size_t grown = seen_capacity ? seen_capacity * 2 : 16;
                char **bigger = (char **)realloc(seen, grown * sizeof *bigger);
                if (bigger == NULL) {
                    continue;
                }
                seen = bigger;
                seen_capacity = grown;
            }
            seen[seen_count++] = unit->path;  // borrowed; lives as long as r

            cJSON *diags = lsp_diagnostics_for_unit(unit);
            sink(context, unit->path, diags);
        }
    }
    free(seen);
    lhat_mutex_unlock(&ws->lock);
}
