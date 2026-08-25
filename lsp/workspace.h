// L^ (lhat) -- LSP server: the workspace's roots, checked and re-checked.
//
// The unit of re-checking here is a whole LhatProgram. Every *.lh file
// under the workspace root is its own root and gets its own LhatProgram; a
// file several roots require gets re-parsed once per root that reaches it.
// That duplication is the cost this design pays for not needing a cache
// inside program.h itself.
//
// 05 の 5.7 has since given program.h a way to invalidate one unit inside an
// already-checked program, which would make the re-check here incremental.
// Not taken yet.
//
// A reverse index (path -> the roots whose last check reached it) keeps a
// single edit from re-checking the whole workspace: changing one file
// re-checks that file's own root plus whatever roots' graphs passed through
// it, and nothing else.

#ifndef LSP_WORKSPACE_H
#define LSP_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

#include "port/thread.h"

#include "cJSON.h"
#include "program_internal.h"

#include "document_store.h"
#include "host_config.h"

typedef struct LspRoot {
    char *path;  // absolute, '/'-separated -- this root's own file (uri.h)
    LhatProgram program;
    bool checked;
    struct LspRoot *next;
} LspRoot;

// path -> the absolute root paths whose last check reached it.
typedef struct LspReverseEntry {
    char *path;
    char **roots;  // owned strings
    size_t root_count;
    size_t root_capacity;
    struct LspReverseEntry *next;
} LspReverseEntry;

typedef struct {
    char *root_path;  // absolute filesystem path; NULL in single-file mode
    LspDocumentStore documents;
    LspRoot *roots;
    LspReverseEntry *reverse;
    // lhat-host.json at root_path, parsed (host_config.h), or NULL when
    // there is none -- then bind falls back to the print/collectgarbage
    // minimum. Guarded by `lock` like roots/reverse: recheck_one_root
    // applies it, and the worker reloads it when the file changes.
    LspHostConfig *host_config;
    LhatMutex lock;  // guards roots/reverse/host_config. Reached from the
                 // main thread only before the worker starts
                 // ("initialized"'s discover_roots + the first config load,
                 // handlers/initialize.c); every other access is the worker
                 // thread's own (recheck_all/recheck_affected/
                 // collect_diagnostics, worker.c). didOpen/didChange touch
                 // documents and the queue, not this.
} LspWorkspace;

// Takes ownership of nothing; `root_path` (absolute, or NULL when the client
// gave no workspace folder) is copied.
void lsp_workspace_init(LspWorkspace *ws, const char *root_path);
void lsp_workspace_dispose(LspWorkspace *ws);

// Adds every *.lh under root_path as its own root, unchecked. A no-op in
// single-file mode (root_path == NULL).
void lsp_workspace_discover_roots(LspWorkspace *ws);

// (Re)loads lhat-host.json from under root_path -- the open document's text
// when the editor holds it, disk otherwise, the same two steps checking
// reads a unit by. Replaces whatever config was held before; a file that is
// gone or will not parse leaves none, and bind falls back to the minimum.
// A no-op in single-file mode.
void lsp_workspace_load_host_config(LspWorkspace *ws);

// Whether `path` (absolute, forward-slashed) is this workspace's
// lhat-host.json -- the worker's cue to reload the config and re-check
// everything rather than treat it as a unit.
bool lsp_workspace_is_host_config_path(const LspWorkspace *ws,
                                       const char *path);

// Whether `path` is one checking can take as a root -- a *.lh file. What
// keeps a stray non-unit file (lhat-host.json itself, or anything else the
// editor happens to open) from being registered as a root and read as L^.
bool lsp_workspace_is_unit_path(const char *path);

// Ensures `path` (absolute) is a known root, then re-checks its own root
// plus every root the reverse index says last reached `path`.
void lsp_workspace_recheck_affected(LspWorkspace *ws, const char *path);

// Re-checks every known root. worker.c calls this once, as its first act
// after discover_roots -- one hop per root rather than draining a freshly
// discovered workspace through recheck_affected one root at a time, which
// would re-check a root each time another root that reaches it came up
// next in the same batch (05 の 6.2's require^ walk makes reach transitive).
void lsp_workspace_recheck_all(LspWorkspace *ws);

// Calls `sink` once per distinct absolute path currently held by any root,
// with a cJSON Diagnostic[] array (03 の 1.1's three stages, one shape --
// see diagnostics.h). Ownership of the array passes to the sink.
typedef void (*LspDiagnosticsSink)(void *context, const char *path,
                                   cJSON *diagnostics);
void lsp_workspace_collect_diagnostics(LspWorkspace *ws,
                                       LspDiagnosticsSink sink, void *context);

// Finds the LhatUnit at `path` among every known root's units and calls
// `sink` with it while still holding the workspace lock -- a request
// handler (e.g. semantic tokens) needs the tree for one path, but a
// pointer handed back after unlocking could be freed by the worker
// thread's next recheck (recheck_one_root disposes and rebuilds a root's
// whole LhatProgram). Not called at all when `path` is not part of any
// checked root's graph.
typedef void (*LspUnitSink)(void *context, const LhatUnit *unit);
void lsp_workspace_with_unit(LspWorkspace *ws, const char *path,
                             LspUnitSink sink, void *context);

#endif  // LSP_WORKSPACE_H
