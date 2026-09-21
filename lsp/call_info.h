#ifndef LSP_CALL_INFO_H
#define LSP_CALL_INFO_H

#include "cJSON.h"
#include "program_internal.h"

// Semantic call slots, including declaration-only names/defaults when known.
// No layout information, and no execution of the callee or its defaults.
cJSON *lsp_call_info(const LhatUnit *unit, const LhatNode *node);

#endif
