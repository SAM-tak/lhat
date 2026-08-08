// L^ (lhat) -- LSP server: LhatUnit diagnostics -> LSP Diagnostic[].

#include "diagnostics.h"

#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"

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
    const char *text = unit->source.text;
    size_t text_length = unit->source.length;

    for (size_t i = 0; i < unit->lexer.diagnostic_count; i++) {
        const LhatDiagnostic *d = &unit->lexer.diagnostics[i];
        cJSON_AddItemToArray(array, make_diagnostic(text, text_length,
            d->offset, 0, lhat_lexer_error_message(d->code)));
    }

    for (size_t i = 0; i < unit->parsed.diagnostic_count; i++) {
        const LhatParseDiagnostic *d = &unit->parsed.diagnostics[i];
        char room[256];
        size_t needed = lhat_parse_message_write(d, room, sizeof room);
        char *message = room;
        char *bigger = NULL;
        if (needed >= sizeof room) {
            bigger = (char *)malloc(needed + 1);
            if (bigger != NULL) {
                lhat_parse_message_write(d, bigger, needed + 1);
                message = bigger;
            }
        }
        cJSON_AddItemToArray(array, make_diagnostic(text, text_length,
            d->offset, d->length, message));
        free(bigger);
    }

    for (size_t i = 0; i < unit->checked.diagnostic_count; i++) {
        const LhatCheckDiagnostic *d = &unit->checked.diagnostics[i];
        char room[256];
        size_t needed = lhat_check_message_write(d, room, sizeof room);
        char *message = room;
        char *bigger = NULL;
        if (needed >= sizeof room) {
            bigger = (char *)malloc(needed + 1);
            if (bigger != NULL) {
                lhat_check_message_write(d, bigger, needed + 1);
                message = bigger;
            }
        }
        cJSON_AddItemToArray(array, make_diagnostic(text, text_length,
            d->offset, d->name_length, message));
        free(bigger);
    }

    return array;
}
