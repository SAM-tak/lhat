// L^ (lhat) -- LSP server: what the checker settled on at a position.

#include "resolution.h"

#include <string.h>

#include "definition.h"

// Read in order rather than by position, since this asks about `definition`
// and only `use` is sorted. A unit's worth of names is small, and a hover or
// a command runs once where the walk runs per keystroke.
//
// Only a definition in this unit is one this offset can mean: a record that
// points into another unit has an offset of that unit's, and a require^'s
// is 0 -- exactly where every unit's own first declaration also stands.
static const LhatResolution *resolution_for_declaration(const LhatUnit *unit,
                                                        uint32_t offset)
{
    const LhatCheckResult *checked = &unit->checked;
    for (size_t i = 0; i < checked->resolution_count; i++) {
        const LhatResolution *entry = &checked->resolutions[i];
        if (!entry->has_definition || entry->definition != offset ||
            entry->type == NULL) {
            continue;
        }
        if (entry->definition_path != NULL &&
            (unit->path == NULL ||
             strcmp(entry->definition_path, unit->path) != 0)) {
            continue;
        }
        return entry;
    }
    return NULL;
}

const LhatResolution *lsp_resolution_at(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL || unit->source.text == NULL) {
        return NULL;
    }
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);
    if (resolved != NULL && resolved->type != NULL) {
        return resolved;
    }
    return resolution_for_declaration(
        unit, lsp_definition_name_start(&unit->source, offset));
}
