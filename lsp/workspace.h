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
#include "settings.h"

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
    // lhat-lsp.json at root_path, parsed (settings.h), or NULL when there
    // is none -- then nothing is excluded and "strict" is the host config's.
    // Guarded by `lock` like the rest: the scan and every root registration
    // read it, and the worker reloads it when the file changes.
    LspSettings *settings;
    LhatMutex lock;  // guards roots/reverse/host_config/settings. Reached
                 // from the main thread only before the worker starts
                 // ("initialized"'s config loads + discover_roots,
                 // handlers/initialize.c); every other access is the worker
                 // thread's own (recheck_all/recheck_affected/
                 // collect_diagnostics, worker.c). didOpen/didChange touch
                 // documents and the queue, not this.
} LspWorkspace;

// Takes ownership of nothing; `root_path` (absolute, or NULL when the client
// gave no workspace folder) is copied.
void lsp_workspace_init(LspWorkspace *ws, const char *root_path);
void lsp_workspace_dispose(LspWorkspace *ws);

// Makes the set of roots agree with the settings: drops every root
// lhat-lsp.json now excludes, adds every *.lh under root_path that it does
// not, and then adds the files force_include_files names -- unchecked.
// Idempotent, so re-running it after the settings change both takes away
// what is newly excluded and brings back what is no longer. A no-op in
// single-file mode (root_path == NULL).
void lsp_workspace_discover_roots(LspWorkspace *ws);

// The absolute path of a workspace-relative name, malloc'd (the caller
// frees); NULL in single-file mode. What a force_include_files entry is
// resolved against, for a caller that has to say whether the file is there.
char *lsp_workspace_path_under_root(const LspWorkspace *ws,
                                    const char *relative);

// What a load found, for a caller with a connection to say it on -- the
// workspace itself has none (server.h owns the one place stdout is written).
// Shared by both configs: the four answers are the same four either way.
typedef enum {
    LSP_CONFIG_READ,        // parsed, and now in force
    LSP_CONFIG_ABSENT,      // nothing at that path
    LSP_CONFIG_UNREADABLE,  // there, but not JSON this reader takes
    LSP_CONFIG_NO_ROOT,     // single-file mode: nowhere to look at all
} LspConfigOutcome;

// (Re)loads a config from under root_path -- the open document's text when
// the editor holds it, disk otherwise, the same two steps checking reads a
// unit by. Replaces whatever was held before; a file that is gone or will
// not parse leaves none, and the fallback stands: the print/collectgarbage
// minimum for the host config, and nothing excluded for the settings.
//
// `looked_at` is filled with the path that was tried (malloc'd, the caller
// frees; NULL in single-file mode) so that what is said about the outcome
// can name the file rather than describe it.
LspConfigOutcome lsp_workspace_load_host_config(LspWorkspace *ws,
                                                char **looked_at);
LspConfigOutcome lsp_workspace_load_settings(LspWorkspace *ws,
                                             char **looked_at);

// Whether `path` (absolute, forward-slashed) is one of this workspace's two
// config files -- the worker's cue to reload it and re-check everything
// rather than treat it as a unit.
bool lsp_workspace_is_host_config_path(const LspWorkspace *ws,
                                       const char *path);
bool lsp_workspace_is_settings_path(const LspWorkspace *ws, const char *path);

// Whether `path` is one checking can take as a root -- a *.lh file. What
// keeps a stray non-unit file (lhat-host.json itself, or anything else the
// editor happens to open) from being registered as a root and read as L^.
bool lsp_workspace_is_unit_path(const char *path);

// 05 の 10 章: whether the file at `path` holds a compiled unit rather than
// text. `lhat --compile` writes a unit out under the name it had, so a
// binary unit is a *.lh like any other and only its bytes say which it is.
// Read from disk, because that is the only place they survive: what an
// editor shows of one has already been through a decoder, and the magic --
// a byte no UTF-8 text can start on -- comes back as a replacement
// character. False for a file that is not there to read.
bool lsp_workspace_is_binary_unit(const char *path);

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
