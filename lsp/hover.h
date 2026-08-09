// L^ (lhat) -- LSP server: what to show when a name is hovered over.
//
// 07 の 4 章. What it answers with is the definition the name reaches: the
// source that introduced it, and whatever was written about it in comments
// (01 の 6.4).

#ifndef LSP_HOVER_H
#define LSP_HOVER_H

#include <stdint.h>

#include "cJSON.h"
#include "program.h"

// A Hover for the name at `offset` (a byte offset into the unit's source), or
// NULL when nothing there has a definition to show. The caller owns it.
cJSON *lsp_hover_for_unit(const LhatUnit *unit, uint32_t offset);

#endif  // LSP_HOVER_H
