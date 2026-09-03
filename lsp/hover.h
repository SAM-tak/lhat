// L^ (lhat) -- LSP server: what to show when a name is hovered over.
//
// 07 の 4 章. What it answers with is the definition the name reaches: the
// source that introduced it, and whatever was written about it in comments
// (01 の 6.4).
//
// In two halves, because the two may be in two units. What the checker
// settled the name to -- its type, and where its definition is -- is read
// off the unit the cursor is in; the definition's own line and the comments
// above it are cut from the unit that holds it, which 05 の 6.1 lets be
// another file. A unit pointer must not outlive the workspace lock
// (lsp/workspace.h), so nothing pointer-shaped crosses between the halves:
// the first leaves strings and offsets, the second reads them.

#ifndef LSP_HOVER_H
#define LSP_HOVER_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "program_internal.h"

#include "position.h"

typedef struct {
    // The name itself, for the editor to underline. Nothing to mark when
    // `has_range` is false.
    bool has_range;
    LspPosition from;
    LspPosition to;
    // What the checker settled on, written out (type.h's spelling); NULL
    // when nothing there has a type.
    char *type;
    // 14.15: the member still left abstract, as the note shown above the
    // block; NULL when none.
    char *abstract_note;
    // Where the definition is. `definition_path` names another unit, or is
    // NULL for the unit asked (or for no definition at all, when
    // `has_definition` is false).
    bool has_definition;
    char *definition_path;
    uint32_t definition;
    // What lsp_hover_describe fills from the defining unit: the definition's
    // first line and the comment written above it. NULL until then, and NULL
    // still when that unit could not be reached -- the type is shown alone.
    char *line;
    char *documentation;
} LspHoverPart;

// The half read off the unit the cursor is in. False when nothing at
// `offset` (a byte offset into the unit's source) has anything to show.
// When the definition is in this same unit the description is filled here
// too; otherwise `definition_path` says which unit to hand to
// lsp_hover_describe. The caller disposes `out` either way.
bool lsp_hover_locate(const LhatUnit *unit, uint32_t offset,
                      LspHoverPart *out);

// The half read off the unit the definition is in: the first line of the
// form that declared it, and what was written above. Leaves the part as it
// was when the unit holds no tree (a binary unit, 05 の 10 章).
void lsp_hover_describe(const LhatUnit *defining, LspHoverPart *part);

// The Hover the two halves make, or NULL when neither found anything to
// show. The caller owns it.
cJSON *lsp_hover_render(const LspHoverPart *part);

void lsp_hover_part_dispose(LspHoverPart *part);

// The three in one, for a name whose definition is in the same unit -- or
// none. A definition in another unit is rendered without its line, since
// only the caller with a workspace can reach that unit.
cJSON *lsp_hover_for_unit(const LhatUnit *unit, uint32_t offset);

#endif  // LSP_HOVER_H
