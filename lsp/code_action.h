// L^ (lhat) -- LSP server: the fixes a diagnostic knows how to make, as LSP
// CodeAction objects (07 §6).
//
// The library works a fix out, since the stage that refused is the one that
// knows what would have been right (lhat/program.h's lhat_unit_diagnostic_fix).
// This only turns it into what LSP calls an edit: nothing here decides
// anything about the language, the way lsp/diagnostics.c decides nothing
// about what is wrong.

#ifndef LSP_CODE_ACTION_H
#define LSP_CODE_ACTION_H

#include <stdint.h>

#include "cJSON.h"
#include "program_internal.h"

// A cJSON array of CodeAction objects: one per fix of every diagnostic of
// `unit` whose place meets the byte range [from, to]. `uri` is the document
// the edits are against, spelled the way the client spelled it, since that is
// the key a WorkspaceEdit's `changes` is read by. Always an array, empty when
// there is nothing to offer; NULL only on allocation failure.
cJSON *lsp_code_actions_for_unit(const LhatUnit *unit, const char *uri,
                                 uint32_t from, uint32_t to);

// 07 §6: one CodeAction of kind source.fixAll holding every edit of
// lhat_unit_fix_all -- what an editor applies on save or from its "fix all"
// command. NULL when there is nothing to apply, since an action that does
// nothing is one more line in a menu.
cJSON *lsp_fix_all_for_unit(const LhatUnit *unit, const char *uri);

// Whether a CodeActionContext's `only` asks for `kind`. LSP's kinds are a
// dotted hierarchy, so asking for "source" asks for "source.fixAll" too.
// `only` NULL -- the client did not narrow -- asks for every kind.
bool lsp_code_action_wanted(const cJSON *only, const char *kind);

#endif  // LSP_CODE_ACTION_H
