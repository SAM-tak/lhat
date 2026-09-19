#ifndef LSP_GRAPH_TYPES_H
#define LSP_GRAPH_TYPES_H
#include "cJSON.h"
#include "program_internal.h"
cJSON *lsp_graph_type_options(const LhatUnit *unit, uint32_t offset);
cJSON *lsp_graph_type_options_result(const LhatUnit *unit, uint32_t offset, int result_index);
#endif
