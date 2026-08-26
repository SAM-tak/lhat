// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.
//
// What each name means is the language's answer (lhat/semantic.h), so what
// is left here is the shape LSP wants it in: a legend of names, and the
// delta-encoded quintuples the spec asks the data array to be.
//
// The classification used to live here, and moved when a second editor
// wanted it -- Godot's script editor colours a .lh from the same answers.
// Reading a unit is not the language server's business alone.

#include "semantic_tokens.h"

#include <stdint.h>
#include <stdlib.h>

#include "lhat/semantic.h"

#include "position.h"

// The order is the legend: an index into this array is what a token carries,
// so it has to match LhatSemanticKind one for one.
const char *const LSP_SEMANTIC_TOKEN_TYPES[] = {
    "namespace", "type", "class", "parameter", "variable", "function",
    "property",
};
const size_t LSP_SEMANTIC_TOKEN_TYPES_COUNT =
    sizeof LSP_SEMANTIC_TOKEN_TYPES / sizeof LSP_SEMANTIC_TOKEN_TYPES[0];

const char *const LSP_SEMANTIC_TOKEN_MODIFIERS[] = {
    "declaration",
    "readonly",  // 8.9: bound by a let^ rather than a var^
    // The spec's own word for a name the standard library provides, which is
    // what 14.19's members are here -- the language answers them itself.
    "defaultLibrary",
};
const size_t LSP_SEMANTIC_TOKEN_MODIFIERS_COUNT =
    sizeof LSP_SEMANTIC_TOKEN_MODIFIERS / sizeof LSP_SEMANTIC_TOKEN_MODIFIERS[0];

enum {
    SEM_MOD_DECLARATION = 1u << 0,
    SEM_MOD_READONLY = 1u << 1,
    SEM_MOD_BUILTIN = 1u << 2,
};

cJSON *lsp_semantic_tokens_for_unit(const LhatUnit *unit)
{
    cJSON *data = cJSON_CreateArray();
    if (data == NULL) {
        return NULL;
    }

    size_t count = lhat_unit_semantic_names(unit, NULL, 0);
    if (count == 0) {
        return data;
    }
    LhatSemanticName *names =
        (LhatSemanticName *)malloc(count * sizeof *names);
    if (names == NULL) {
        return data;  // an empty answer beats none; the file still opens
    }
    lhat_unit_semantic_names(unit, names, count);

    int prev_line = 0;
    int prev_char = 0;

    for (size_t i = 0; i < count; i++) {
        const LhatSemanticName *name = &names[i];

        LspPosition start = lsp_unit_position_at(unit, name->offset);
        LspPosition end =
            lsp_unit_position_at(unit, name->offset + name->length);
        if (end.line != start.line) {
            continue;  // an identifier never spans a line; be safe anyway
        }

        int delta_line = start.line - prev_line;
        int delta_char =
            delta_line == 0 ? start.character - prev_char : start.character;
        int modifiers = (name->declaration ? SEM_MOD_DECLARATION : 0) |
                        (name->readonly ? SEM_MOD_READONLY : 0) |
                        (name->builtin ? SEM_MOD_BUILTIN : 0);

        cJSON_AddItemToArray(data, cJSON_CreateNumber(delta_line));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(delta_char));
        cJSON_AddItemToArray(data,
                             cJSON_CreateNumber(end.character - start.character));
        cJSON_AddItemToArray(data, cJSON_CreateNumber((int)name->kind));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(modifiers));

        prev_line = start.line;
        prev_char = start.character;
    }

    free(names);
    return data;
}
