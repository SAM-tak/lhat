// L^ (lhat) -- LSP server: the syntax tree as JSON, for lhat/ast.
//
// Defined in DesignDocuments/06-visual-editor.md の 4 章. The visual editor
// reads this instead of parsing L^ for itself, so that one parser -- the
// language's own -- decides what a unit means.

#ifndef LSP_AST_JSON_H
#define LSP_AST_JSON_H

#include "cJSON.h"
#include "program.h"

// The unit's tree, plus the source it was read from. Returns NULL only when
// the unit holds no tree (out of memory, or never parsed).
//
// The caller owns the result.
cJSON *lsp_ast_json_for_unit(const LhatUnit *unit);

#endif  // LSP_AST_JSON_H
