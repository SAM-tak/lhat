// L^ (lhat) -- LSP server: what the checker settled on at a position.

#include "resolution.h"

#include <ctype.h>
#include <string.h>

// A `definition` is where the name starts, and the cursor may be anywhere
// within it -- lhat_check_resolution_at spans a use, but a definition is one
// offset. So the position is walked back to the start of the name it is in.
//
// 01 の 3.1: the lexer takes any non-ASCII that is neither space nor a
// reserved mark as part of a name, so this does too rather than holding a
// narrower idea of one than the language has.
static uint32_t name_start_at(const char *text, size_t length, uint32_t offset)
{
    // A position past the end is one an editor may ask about -- a stale
    // cursor against a document the worker has since re-read. Walking back
    // from it would read what is not there.
    uint32_t at = offset <= length ? offset : (uint32_t)length;
    while (at > 0) {
        unsigned char c = (unsigned char)text[at - 1];
        if (!(isalnum(c) || c == '_' || c >= 0x80)) {
            break;
        }
        at--;
    }
    return at;
}

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
        unit, name_start_at(unit->source.text, unit->source.length, offset));
}
