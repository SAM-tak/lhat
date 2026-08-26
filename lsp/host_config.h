// L^ (lhat) -- LSP server: lhat-host.json, what a host's C would register.
//
// This server type-checks workspaces written against embeddings it cannot
// run: the host's registrations (05 の 8.7) live in C the server never
// links. lhat-host.json carries them as text -- the very JSON
// `lhat --dump-host-api` (lhat_program_dump_host_api) writes, or any other
// host's equivalent -- and applying it re-plays the registrations against
// a fresh LhatProgram with a stub callback, which checking never calls.
//
// Without the file the server falls back to the two names cli/main.c binds
// unconditionally (print, collectgarbage), and anything else a script uses
// from its host is a known false positive.

#ifndef LSP_HOST_CONFIG_H
#define LSP_HOST_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "program_internal.h"

// A parsed lhat-host.json, ready to apply to any number of programs --
// workspace.c re-checks a root by rebuilding its whole LhatProgram, so one
// load serves every rebuild until the file changes.
typedef struct LspHostConfig LspHostConfig;

// Parses `text`. NULL when it is not JSON at all; a well-formed file with
// entries this reader does not recognise loses those entries and keeps the
// rest -- a workspace with a half-written config is better served by the
// registrations that do parse than by none.
LspHostConfig *lsp_host_config_parse(const char *text, size_t length);

void lsp_host_config_free(LspHostConfig *config);

// 03-compilation-pipeline.md の 3.1: the "strict" lhat_program_dump_host_api
// wrote, or `fallback` when there is no config (NULL) or the file predates
// the field. lhatls checks strict regardless of this -- what it changes is
// how the diagnostics it finds should be shown.
bool lsp_host_config_strict(const LspHostConfig *config, bool fallback);

// Re-plays the registrations into `program`, types first, then signatures,
// then bindings -- the order lhat_program_dump_host_api writes them in and
// the one that keeps every name a signature mentions already registered.
// Individual failures (an unparseable signature, a duplicate name) skip
// that entry and go on.
void lsp_host_config_apply(const LspHostConfig *config, LhatProgram *program);

// How many entries the file carries, for a server saying out loud what it
// read. Counted rather than remembered: the arrays are the parsed JSON and
// nothing else holds a tally. Any of the three may be NULL.
void lsp_host_config_counts(const LspHostConfig *config, size_t *types,
                            size_t *functions, size_t *annotations);

#endif  // LSP_HOST_CONFIG_H
