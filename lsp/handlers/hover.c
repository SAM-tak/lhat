// L^ (lhat) -- LSP server: textDocument/hover.

#include "hover.h"

#include <stdlib.h>

// The hover-building logic, disambiguated from this handler's own header of
// the same basename -- a bare "hover.h" resolves to this file's neighbour
// first, the way semantic_tokens.c already has to say.
#include "../hover.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

// LSP gives a line and a UTF-16 character; lhat counts bytes. position.c goes
// the other way, so the walk here is the inverse of it and lives with the one
// handler that needs it.
static uint32_t byte_offset_of(const char *text, size_t length, int line,
                               int character)
{
    size_t i = 0;
    for (int seen = 0; seen < line && i < length; i++) {
        if (text[i] == '\n') {
            seen++;
        }
    }
    int units = 0;
    while (i < length && text[i] != '\n' && units < character) {
        unsigned char lead = (unsigned char)text[i];
        size_t sequence = 1;
        int width = 1;
        if (lead >= 0xF0) {
            sequence = 4;
            width = 2;  // beyond the BMP: a surrogate pair
        } else if (lead >= 0xE0) {
            sequence = 3;
        } else if (lead >= 0xC0) {
            sequence = 2;
        }
        if (i + sequence > length) {
            break;
        }
        i += sequence;
        units += width;
    }
    return (uint32_t)i;
}

// The position is turned into an offset here rather than by the caller,
// because only the unit has the text -- and it is only safe to touch while
// the workspace lock is held (workspace.h).
typedef struct {
    int line;
    int character;
    cJSON *result;
} HoverRequest;

static void collect(void *context, const LhatUnit *unit)
{
    HoverRequest *request = (HoverRequest *)context;
    uint32_t offset = byte_offset_of(unit->source.text, unit->source.length,
                                     request->line, request->character);
    request->result = lsp_hover_for_unit(unit, offset);
}

cJSON *lsp_handle_hover(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(params, "position");
    if (text_document == NULL || position == NULL) {
        return NULL;
    }
    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    const cJSON *line = cJSON_GetObjectItemCaseSensitive(position, "line");
    const cJSON *character =
        cJSON_GetObjectItemCaseSensitive(position, "character");
    if (!cJSON_IsString(uri_item) || !cJSON_IsNumber(line) ||
        !cJSON_IsNumber(character)) {
        return NULL;
    }

    char *path = lsp_uri_to_absolute_path(uri_item->valuestring);
    if (path == NULL) {
        return NULL;
    }

    HoverRequest request = {line->valueint, character->valueint, NULL};
    lsp_workspace_with_unit(&server->workspace, path, collect, &request);
    free(path);

    return request.result;  // NULL is a JSON null, which LSP reads as "nothing"
}
