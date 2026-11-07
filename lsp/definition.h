// L^ (lhat) -- LSP server: where the name at a position was written.
//
// [lsp.hover] records, for every name the checker resolved, where the meaning
// it reached was declared -- a binding's own place for a name a scope holds
// ([lton.host]), and the member's written place for one found in a type ([syntax.obj.structure]).
// That is the whole of what going to a definition needs; the walk is the
// checker's and is not done a second time here.
//
// What has no place to point at answers nothing, and that is the right
// answer rather than a missing one: a name the host registered was declared
// in C ([modules.host.import]), and a built-in of a coroutine or a string ([syntax.sub.coroutine-members], [syntax.obj.substring])
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

// [lsp.hover.try]: the places in this unit the try^ at `offset` sends errors. A route
// out of the unit points at its module^ when it declares one, and otherwise
// has nowhere to point. Answers how many were written to `out`.
size_t lsp_definition_try_targets(const LhatUnit *unit, uint32_t offset,
                                  uint32_t *out, size_t capacity);

// The complete identifier under the cursor; source length when none covers it.
uint32_t lsp_definition_name_start(const LhatSource *source, uint32_t offset);

// How far the name at `offset` runs, so a range can be made of it: [lex.ident.chars]
// says which bytes a name is made of, and the record keeps only where it
// starts.
uint32_t lsp_definition_name_end(const char *text, size_t length,
                                 uint32_t offset);

#endif  // LSP_DEFINITION_H
