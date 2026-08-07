// L^ (lhat) -- LSP server: LhatUnit diagnostics -> LSP Diagnostic[].

#include "diagnostics.h"

#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"

typedef struct {
    int line;
    int character;
} LspPosition;

// One code point from `text[*at..)`, `*at` moved past it. Malformed input is
// read one byte at a time, same as error.c's next_character -- but this
// counts UTF-16 units rather than terminal cells, so it is not the same
// function wearing a different name; the two answer different questions
// from the same kind of walk.
static uint32_t decode_and_advance(const char *text, size_t length, size_t *at)
{
    size_t i = *at;
    unsigned char lead = (unsigned char)text[i];
    size_t follow = 0;
    uint32_t code = lead;

    if (lead >= 0xF0u) {
        follow = 3;
        code = lead & 0x07u;
    } else if (lead >= 0xE0u) {
        follow = 2;
        code = lead & 0x0Fu;
    } else if (lead >= 0xC0u) {
        follow = 1;
        code = lead & 0x1Fu;
    }
    if (i + follow >= length) {
        *at = i + 1;
        return lead;
    }
    for (size_t k = 1; k <= follow; k++) {
        unsigned char byte = (unsigned char)text[i + k];
        if ((byte & 0xC0u) != 0x80u) {
            *at = i + 1;
            return lead;
        }
        code = (code << 6) | (byte & 0x3Fu);
    }
    *at = i + follow + 1;
    return code;
}

// LSP's Position: 0-based line, and character in UTF-16 code units -- a
// supplementary-plane code point (>= U+10000) is a surrogate pair and counts
// as two.
static LspPosition position_at(const char *text, size_t text_length,
                               uint32_t byte_offset)
{
    size_t offset = byte_offset <= text_length ? byte_offset : text_length;

    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n') {
        line_start--;
    }
    int line = 0;
    for (size_t i = 0; i < line_start; i++) {
        if (text[i] == '\n') {
            line++;
        }
    }

    int character = 0;
    size_t at = line_start;
    while (at < offset) {
        uint32_t code = decode_and_advance(text, text_length, &at);
        character += code >= 0x10000u ? 2 : 1;
    }

    LspPosition pos;
    pos.line = line;
    pos.character = character;
    return pos;
}

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
    LspPosition start = position_at(text, text_length, offset);
    LspPosition end = position_at(text, text_length, offset + marked);

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
