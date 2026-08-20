// L^ (lhat) -- LSP server: LhatUnit diagnostics -> LSP Diagnostic[].

#include "diagnostics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "position.h"

static cJSON *make_position(LspPosition pos)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "line", pos.line);
    cJSON_AddNumberToObject(obj, "character", pos.character);
    return obj;
}

// `span` is in bytes, and zero marks one character rather than a span --
// the same convention error.h's LhatReport uses.
static cJSON *make_diagnostic(const char *text, size_t text_length,
                              uint32_t offset, uint32_t span,
                              const char *message)
{
    uint32_t marked = span > 0 ? span : 1;
    LspPosition start = lsp_position_at(text, text_length, offset);
    LspPosition end = lsp_position_at(text, text_length, offset + marked);

    cJSON *range = cJSON_CreateObject();
    cJSON_AddItemToObject(range, "start", make_position(start));
    cJSON_AddItemToObject(range, "end", make_position(end));

    cJSON *diag = cJSON_CreateObject();
    cJSON_AddItemToObject(diag, "range", range);
    cJSON_AddNumberToObject(diag, "severity", 1);  // Error; lhat has no other severity today
    cJSON_AddStringToObject(diag, "source", "lhat");
    cJSON_AddStringToObject(diag, "message", message);
    return diag;
}

cJSON *lsp_diagnostics_for_unit(const LhatUnit *unit)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }
    const LhatSource *source = lhat_unit_source(unit);
    if (source == NULL) {
        return array;
    }

    size_t count = lhat_unit_diagnostic_count(unit);
    for (size_t i = 0; i < count; i++) {
        char room[256];
        char *message = room;
        char *bigger = NULL;
        size_t needed =
            lhat_unit_diagnostic_message(unit, i, room, sizeof room);
        if (needed >= sizeof room) {
            bigger = (char *)malloc(needed + 1);
            if (bigger != NULL) {
                lhat_unit_diagnostic_message(unit, i, bigger, needed + 1);
                message = bigger;
            }
        }

        LhatUnitDiagnostic d = lhat_unit_diagnostic(unit, i);
        cJSON_AddItemToArray(array,
                             make_diagnostic(source->text, source->length,
                                             d.offset, d.length, message));
        free(bigger);
    }

    return array;
}

void lsp_diagnostics_add_compile_failure(cJSON *array, const LhatUnit *unit,
                                         LhatCompileResult failure)
{
    if (array == NULL || failure.status == LHAT_COMPILE_OK) {
        return;
    }
    const LhatSource *source = lhat_unit_source(unit);
    if (source == NULL) {
        return;
    }
    char room[256];
    const char *message = lhat_compile_status_message(failure.status);
    if (failure.name != NULL) {
        snprintf(room, sizeof room, "%s: %.*s", message,
                 (int)failure.name_length, failure.name);
        message = room;
    }
    cJSON_AddItemToArray(array,
                         make_diagnostic(source->text, source->length,
                                         failure.offset, failure.name_length,
                                         message));
}
