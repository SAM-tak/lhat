// L^ (lhat) -- LSP server: where the name at a position was written.
//
// 07 の 4 章 records, for every name the checker resolved, where the meaning
// it reached was declared -- a binding's own place for a name a scope holds
// (8 章), and the member's written place for one found in a type (14.10).
// That is the whole of what going to a definition needs; the walk is the
// checker's and is not done a second time here.
//
// What has no place to point at answers nothing, and that is the right
// answer rather than a missing one: a name the host registered was declared
// in C (05 の 8.7), and a built-in of a coroutine or a string (15.6改, 14.19)
// was declared by the language itself.

#ifndef LSP_DEFINITION_H
#define LSP_DEFINITION_H

#include <stdbool.h>
#include <stdint.h>

#include "program_internal.h"

// Where the name at `offset` was written. `path` is NULL when it was written
// in this same unit, and otherwise borrows the unit path the member carried
// (type.h's declared_in), which lives as long as the program. False when
// nothing there resolves, or when what it resolved to has no written place.
typedef struct {
    const char *path;
    uint32_t offset;
} LspDefinitionSite;

bool lsp_definition_for_unit(const LhatUnit *unit, uint32_t offset,
                             LspDefinitionSite *out);

// How far the name at `offset` runs, so a range can be made of it: 01 の 3.1
// says which bytes a name is made of, and the record keeps only where it
// starts.
uint32_t lsp_definition_name_end(const char *text, size_t length,
                                 uint32_t offset);

#endif  // LSP_DEFINITION_H
