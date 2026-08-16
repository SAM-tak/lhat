// L^ (lhat) -- LSP server: the type at a position, as text to keep.

#include "signature.h"

#include <stdlib.h>

#include "check.h"
#include "resolution.h"
#include "type.h"

char *lsp_signature_for_unit(const LhatUnit *unit, uint32_t offset)
{
    // Where a declaration is answered for as well as a use -- which is where
    // someone wanting to copy a signature is most likely standing (07 の 4 章,
    // resolution.h).
    const LhatResolution *resolved = lsp_resolution_at(unit, offset);
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
