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

// LSP's DiagnosticSeverity: 1 Error, 2 Warning.
#define LSP_SEVERITY_ERROR 1
#define LSP_SEVERITY_WARNING 2

// `span` is in bytes, and zero marks one character rather than a span --
// the same convention error.h's LhatReport uses.
static cJSON *make_diagnostic(const LhatUnit *unit, uint32_t offset,
                              uint32_t span, int severity,
                              const char *message)
{
    uint32_t marked = span > 0 ? span : 1;
    // 08-lton.md: a .lton's text is the file's own inside a wrapper, so what
    // is marked is the file's position and not the wrapped text's.
    LspPosition start = lsp_unit_position_at(unit, offset);
    LspPosition end = lsp_unit_position_at(unit, offset + marked);

    cJSON *range = cJSON_CreateObject();
    cJSON_AddItemToObject(range, "start", make_position(start));
    cJSON_AddItemToObject(range, "end", make_position(end));

    cJSON *diag = cJSON_CreateObject();
    cJSON_AddItemToObject(diag, "range", range);
    cJSON_AddNumberToObject(diag, "severity", severity);
    cJSON_AddStringToObject(diag, "source", "lhat");
    cJSON_AddStringToObject(diag, "message", message);
    return diag;
}

cJSON *lsp_diagnostics_for_unit(const LhatUnit *unit, bool project_relaxed)
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
        // 03 の 3.1: a relaxed host would not have stopped on the three
        // strict-only gap diagnostics -- advisory there, fatal everywhere
        // else, and fatal everywhere when the host is not declared relaxed.
        int severity = project_relaxed && lhat_unit_diagnostic_relaxed_ok(unit, i)
                           ? LSP_SEVERITY_WARNING
                           : LSP_SEVERITY_ERROR;
        cJSON_AddItemToArray(array,
                             make_diagnostic(unit, d.offset, d.length,
                                             severity, message));
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
    // A compile refusal stops every host, strict or relaxed -- always Error.
    cJSON_AddItemToArray(array,
                         make_diagnostic(unit, failure.offset,
                                         failure.name_length,
                                         LSP_SEVERITY_ERROR, message));
}
