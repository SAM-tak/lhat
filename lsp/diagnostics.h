// L^ (lhat) -- LSP server: LhatUnit diagnostics -> LSP Diagnostic[].
//
// lhat's line/column count Unicode code points (src/lexer.c). LSP's
// Position.character counts UTF-16 code units. 03-compilation-pipeline.md's
// 1.3節 already keeps two counts of a column apart -- what a diagnostic
// names (code points) and where a terminal's mark lands (cells, UAX #11).
// This file adds a third and is the only place it is computed, from the
// byte offset every one of lhat's diagnostics also carries.

#ifndef LSP_DIAGNOSTICS_H
#define LSP_DIAGNOSTICS_H

#include "cJSON.h"
#include "program.h"

// A cJSON array of LSP Diagnostic objects covering every lexer, parser and
// checker diagnostic `unit` carries -- 03 の 1.3's "the stage is its own,
// the rendering is shared" read onto JSON instead of text. Always an array,
// empty when there is nothing to report; NULL only on allocation failure.
cJSON *lsp_diagnostics_for_unit(const LhatUnit *unit);

#endif  // LSP_DIAGNOSTICS_H
