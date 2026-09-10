// L^ (lhat) -- LSP server: lhat-lsp.json, how the tooling should treat this
// project.
//
// The other config beside it, lhat-host.json, is written by a machine:
// `lhat --dump-host-api lhat-host.json` replaces the whole file, so nothing
// hand-written survives there. What a project excludes from checking is
// something no generator can know, so it needs a file with a different
// writer -- this one, hand-written and kept in the repository.
//
// Editor-independent on purpose. "build/ holds generated files" is a fact
// about the project rather than a preference of whoever is editing it, so
// it does not belong in one editor's settings.
//
//   {
//     "exclude": ["build/", "**/node_modules/"],
//     "force_include_files": ["build/generated/api.lh"],
//     "strict": true
//   }
//
// `exclude` decides which files become roots (05 の 5 章's graph is still
// followed: a file another root require^s is read whether or not it is
// excluded). `force_include_files` names the few inside an excluded
// directory that are real sources after all. `strict` overrides the same
// field in lhat-host.json.
//
// THE FORCED ONES ARE NAMED, NOT MATCHED. A pattern there would have to be
// searched for, which means walking the directory the exclusion just told
// the scan to skip -- which is why gitignore cannot re-include under an
// excluded directory (gitignore(5)) and why it is a documented caveat there
// rather than a rule. A named file needs no searching: the scan prunes as
// freely as before and the path is added to the roots afterwards, by name.
// So the two features do not meet at all.
//
// Each is a path relative to the project root, and one that stays inside
// it: an absolute path could not be committed to the repository this file
// lives in, and a ".." would arrive unfolded where every other path key in
// this server is folded (lsp/uri.h) and register a second root for one file.
//
// THE PATTERNS ARE NOT GITIGNORE. A pattern is matched against the path
// relative to the project root, '/'-separated:
//
//   *      any run of characters that does not cross '/'
//   **     any run, '/' included; "**/" also matches no segments at all
//   rest   literal -- '!', '?' and '[' included, so there is no negation
//
// A pattern matches the path it names and everything under it, which makes
// "build" and "build/" the same pattern. Anchored at the project root
// unless it begins with "**/".

#ifndef LSP_SETTINGS_H
#define LSP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

// A parsed lhat-lsp.json. The patterns are copied out of the JSON at parse
// time rather than read off a kept tree: the matcher runs once per directory
// entry of a workspace scan, where walking cJSON would be the walk.
typedef struct LspSettings LspSettings;

// Parses `text`. NULL when it is not JSON, or is JSON that is not an object
// -- a file of the wrong shape is worth saying out loud rather than reading
// as an empty one. A well-formed object with keys this reader does not know
// keeps the ones it does (lsp/host_config.h says the same).
LspSettings *lsp_settings_parse(const char *text, size_t length);

void lsp_settings_free(LspSettings *settings);

// The "strict" written here, or `fallback` -- which is where lhat-host.json's
// own answer arrives from. NULL settings, no field, and a field that is not
// a bool all answer `fallback`.
bool lsp_settings_strict(const LspSettings *settings, bool fallback);

// How many patterns the file carries, for a server saying what it read.
size_t lsp_settings_exclude_count(const LspSettings *settings);

// The named files, in the order they were written -- workspace.c joins each
// to the project root and adds it as a root of its own. `at` answers NULL
// past the end. The strings live as long as `settings`.
size_t lsp_settings_force_count(const LspSettings *settings);
const char *lsp_settings_force_at(const LspSettings *settings, size_t index);

// Whether `relative` (project-relative, '/'-separated, no leading '/')
// is excluded. False when there are no settings, when there are no patterns,
// and for a file force_include_files names -- which is what makes a forced
// file behave like any other from there on: the scan keeps it, the sweep
// does not take it away, and opening it checks it.
bool lsp_settings_excludes_relative(const LspSettings *settings,
                                    const char *relative);

// The same for an absolute path, which is the form every path key in this
// server takes (lsp/uri.h). False when `settings` or `root_path` is NULL
// (single-file mode has no project to be outside of), and false for a path
// that is not under `root_path` -- an exclusion is a statement about this
// project and says nothing about anything else.
//
// Compared byte for byte, not case-folded: root_find and the document store
// already identify paths with strcmp, and a second notion of when two paths
// are the same would disagree with the one the rest of the server runs on.
bool lsp_settings_excludes_path(const LspSettings *settings,
                                const char *root_path, const char *path);

#endif  // LSP_SETTINGS_H
