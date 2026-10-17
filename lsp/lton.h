// L^ (lhat) -- identify LTON documents; parsing uses lhat_parse_lton.
#ifndef LSP_LTON_H
#define LSP_LTON_H
#include <stdbool.h>
bool lsp_lton_is_path(const char *path);
struct LhatProgram;
struct LhatUnit;
// Resolve imported name chains without inferring the data table.
void lsp_lton_resolve(struct LhatProgram *program, struct LhatUnit *unit);
#endif
