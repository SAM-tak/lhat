// L^ (lhat) -- LSP server: project roots, checked and re-checked.
//
// The unit of re-checking here is a whole LhatProgram. Every *.lh file
// under a project root is its own root and gets its own LhatProgram; a
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

// One independently configured part of a workspace. A project begins at a
// directory containing either lhat-host.json or lhat-lsp.json; the fallback
// project for a workspace folder begins at that folder itself. The two config
// files deliberately share this boundary: they are two writers' halves of
// one project configuration, not parent/child layers to combine.
typedef struct LspProject {
    char *root_path;  // absolute filesystem path; this project's boundary
    LspRoot *roots;
    LspReverseEntry *reverse;
    LspHostConfig *host_config;
    LspSettings *settings;
    bool config_boundary;
    // A file opened without a containing workspace still gets checked, but
    // has no configuration-search authority. Such projects contain only
    // roots introduced by that file's document events and are never scanned.
    bool unscoped;
    struct LspProject *next;
} LspProject;

typedef struct {
    // The folders the client put in scope. They are hard search boundaries:
    // a document never obtains a config from above its longest matching one.
    char **workspace_paths;
    size_t workspace_count;
    LspDocumentStore documents;
    LspProject *projects;
    // One lock covers the collection and every project's checking state.
    LhatMutex lock;
} LspWorkspace;

// Takes ownership of nothing. Every path is absolute and is copied. Passing
// no folders leaves the server in single-file mode: it deliberately does not
// search above an arbitrary file, because no client workspace granted that
// directory as configuration scope.
void lsp_workspace_init(LspWorkspace *ws,
                        const char *const *workspace_paths,
                        size_t workspace_count);
void lsp_workspace_dispose(LspWorkspace *ws);

// Finds every configuration directory below each workspace folder, then
// makes an isolated project for it (plus a fallback project at the folder
// itself). Config lookup for an .lh file is therefore nearest-wins, bounded
// by its workspace folder. Replaces old projects but preserves open document
// text, so it is safe after either config file is edited, created, or removed.
void lsp_workspace_discover_projects(LspWorkspace *ws);

// Whether `path` names either config file within a workspace folder. This is
// intentionally name-based rather than limited to an already-known project:
// saving a new config creates a project boundary on the next worker round.
bool lsp_workspace_is_config_path(const LspWorkspace *ws, const char *path);

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

// Resolves `path` to its nearest project, then re-checks its own root plus
// every root that project's reverse index says last reached `path`.
void lsp_workspace_recheck_affected(LspWorkspace *ws, const char *path);

// Re-checks every known root. worker.c calls this once after project
// discovery -- one hop per root rather than draining a freshly discovered
// workspace through recheck_affected one root at a time, which
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

// The same, but checked here and now from the editor's current text rather
// than found among the roots -- the one question that cannot wait for the
// worker. A completion is asked on the keystroke that made the text, and the
// worker debounces (worker.c), so the roots' copy has never seen the '.'
// being asked about. Costs one check of this unit and whatever it require^s,
// on the calling thread, holding the lock.
void lsp_workspace_with_fresh_unit(LspWorkspace *ws, const char *path,
                                   LspUnitSink sink, void *context);

// Every unit of the requesting document's checked project, once each. One
// unit may stand in several roots' graphs, and the path is its identity.
//
// Under the lock throughout, so the sink does its reading and keeps
// nothing -- the same terms lsp_workspace_with_unit sets.
void lsp_workspace_with_every_unit(LspWorkspace *ws, const char *path,
                                   LspUnitSink sink, void *context);

// Owned copies of what a completion offers as paths and as module names.
// Copies rather than pointers because both live behind the lock and the
// worker may replace them: the roots on a settings change, the host config
// when lhat-host.json does. NULL with a zero count when there are none.
char **lsp_workspace_copy_unit_paths(LspWorkspace *ws, const char *path,
                                     size_t *count);
char **lsp_workspace_copy_module_names(LspWorkspace *ws, const char *path,
                                       size_t *count);
void lsp_workspace_free_strings(char **strings, size_t count);

// 05 の 5.5: a unit of this workspace that declared a module^, with what
// it publishes. What a completion offers to take in with a bare require^,
// which binds the unit under the path it declared for itself.
//
// A unit that declared none is left out. 5.5 refuses the short form for
// one, and the long form picks a name -- which is the writer's to pick,
// not the server's.
//
// Only roots the worker has already checked answer, so a workspace whose
// first pass has not finished offers nothing rather than something stale.
// Copies, for the reason above.
typedef struct {
    char *path;         // absolute; a require^ names it relative to itself
    char *module_name;  // what module^ declared
    char **exports;
    size_t export_count;
} LspUnitExports;

LspUnitExports *lsp_workspace_copy_exports(LspWorkspace *ws,
                                            const char *path, size_t *count);
void lsp_workspace_free_exports(LspUnitExports *units, size_t count);

#endif  // LSP_WORKSPACE_H
