// L^ (lhat) -- LSP server: the type at a position, as text to keep.

#include "signature.h"

#include <ctype.h>
#include <stdlib.h>

#include "check.h"
#include "type.h"

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

// A declaration binds a name rather than resolving one, so nothing is
// recorded against it (chk_record_resolution) -- and the cursor is on a
// declaration exactly when someone wants the signature of the thing being
// declared. What *is* recorded is every use, each saying where the name it
// reached was bound, so the answer is there under a different key: a use
// whose definition is the name the cursor is on.
//
// Read in order rather than by position, since this asks about `definition`
// and only `use` is sorted. A unit's worth of names is small, and this runs
// once per invocation of a command rather than per keystroke.
static const LhatResolution *resolution_for_declaration(const LhatUnit *unit,
                                                        uint32_t offset)
{
    const LhatCheckResult *checked = &unit->checked;
    for (size_t i = 0; i < checked->resolution_count; i++) {
        const LhatResolution *entry = &checked->resolutions[i];
        if (entry->has_definition && entry->definition == offset &&
            entry->type != NULL) {
            return entry;
        }
    }
    return NULL;
}

char *lsp_signature_for_unit(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL || unit->source.text == NULL) {
        return NULL;
    }

    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);
    if (resolved == NULL || resolved->type == NULL) {
        resolved = resolution_for_declaration(
            unit, name_start_at(unit->source.text, unit->source.length, offset));
    }
    if (resolved == NULL || resolved->type == NULL) {
        return NULL;
    }

    // Measure, then fill: type.h's contract, so what comes back is the whole
    // type rather than as much of it as some buffer happened to hold.
    size_t wanted = lhat_type_write_full(resolved->type, NULL, 0);
    char *text = (char *)malloc(wanted + 1);
    if (text == NULL) {
        return NULL;
    }
    lhat_type_write_full(resolved->type, text, wanted + 1);
    return text;
}
