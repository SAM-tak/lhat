// L^ (lhat) -- LSP server: code switched off with '#[~ ... ]#' (01 の 6.5).
//
// The lexer sees a block comment there, and the language never looks inside.
// The tools do: lhat/ast lists what one holds among the statements around it
// (06 の 4.1), and lhat/toggleDisabledCode writes the markers and takes them
// away again.

#ifndef LSP_DISABLED_CODE_H
#define LSP_DISABLED_CODE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "program_internal.h"

// What one '#[~ ... ]#' holds, parsed on its own. Used in place: the lexer
// keeps a pointer to the source beside it.
typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
} LspDisabledCode;

// Reads the body of `comment`, which was lexed from `text`. Everything else
// is blanked to spaces with the line breaks kept, so every offset, line and
// column in what parses is the file's own and nothing has to be mapped back.
// False only when memory ran out: a body that does not parse still answers
// true, with the diagnostics in the lexer and the parse result.
#if LHAT_WITH_COMMENTS
bool lsp_disabled_code_parse(LspDisabledCode *out, const char *text,
                             const LhatComment *comment);
void lsp_disabled_code_dispose(LspDisabledCode *code);
#endif

// Where `node` lists statements, by the name lhat_node_visit_children gives
// the place -- the one place disabled code is read as code. NULL for a node
// that lists none.
const char *lsp_disabled_code_statements_field(const LhatNode *node);

// lhat/toggleDisabledCode for a selection of the unit's text, in bytes:
// { "edits": TextEdit[] }, or { "refusal": string } saying why nothing is
// done. Inside disabled code -- the innermost, where they nest -- the markers
// are taken away. Anywhere else the selection is widened to the whole
// statements it touches and those are wrapped.
cJSON *lsp_disabled_code_toggle(const LhatUnit *unit, uint32_t from,
                                uint32_t to);

#endif  // LSP_DISABLED_CODE_H
