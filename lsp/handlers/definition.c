// L^ (lhat) -- LSP server: textDocument/definition.

#include "definition.h"

#include <stdlib.h>
#include <string.h>

// The answer itself, disambiguated from this handler's own header of the
// same basename -- a bare "definition.h" resolves to this file's neighbour
// first, the way hover.c already has to say.
#include "../definition.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "util.h"
#include "workspace.h"

// The two halves are asked of two different units -- the one the cursor is
// in, and the one the answer was written in -- and lsp_workspace_with_unit
// takes the workspace lock itself, so they are two calls in a row rather
// than one inside the other. What crosses between them is a copy: the unit a
// pointer names may be disposed and rebuilt by the worker's next recheck
// (workspace.h).
typedef struct {
    int line;
    int character;
    bool found;
    char *path;  // owned; the unit the definition was written in
    uint32_t offset;
} DefinitionRequest;

static void locate(void *context, const LhatUnit *unit)
{
    DefinitionRequest *request = (DefinitionRequest *)context;
    uint32_t offset = lsp_offset_at(unit->source.text, unit->source.length,
                                    request->line, request->character);
    LspDefinitionSite site;
    if (!lsp_definition_for_unit(unit, offset, &site)) {
        return;
    }
    request->found = true;
    request->offset = site.offset;
    // A member carries the path it was written in; a name a scope holds was
    // bound in the unit that is asking.
    request->path = lsp_strdup(site.path != NULL ? site.path : unit->path);
}

typedef struct {
    uint32_t offset;
    bool ready;
    LspPosition start;
    LspPosition end;
} RangeRequest;

static void measure(void *context, const LhatUnit *unit)
{
    RangeRequest *range = (RangeRequest *)context;
    uint32_t end = lsp_definition_name_end(unit->source.text,
                                           unit->source.length, range->offset);
    range->start =
        lsp_position_at(unit->source.text, unit->source.length, range->offset);
    range->end = lsp_position_at(unit->source.text, unit->source.length, end);
    range->ready = true;
}

static void add_position(cJSON *into, const char *field, LspPosition at)
{
    cJSON *position = cJSON_CreateObject();
    cJSON_AddItemToObject(into, field, position);
    cJSON_AddNumberToObject(position, "line", at.line);
    cJSON_AddNumberToObject(position, "character", at.character);
}

cJSON *lsp_handle_definition(LspServer *server, const cJSON *params)
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

    DefinitionRequest request;
    request.line = line->valueint;
    request.character = character->valueint;
    request.found = false;
    request.path = NULL;
    request.offset = 0;
    lsp_workspace_with_unit(&server->workspace, path, locate, &request);
    free(path);

    if (!request.found || request.path == NULL) {
        free(request.path);
        return NULL;  // a JSON null: nothing here says where it came from
    }

    RangeRequest range;
    range.offset = request.offset;
    range.ready = false;
    lsp_workspace_with_unit(&server->workspace, request.path, measure, &range);
    if (!range.ready) {
        // The unit is named by a member that outlived it, or is not part of
        // any checked root. Nothing to point at rather than a guess.
        free(request.path);
        return NULL;
    }

    char *uri = lsp_absolute_path_to_uri(request.path);
    free(request.path);
    if (uri == NULL) {
        return NULL;
    }

    cJSON *location = cJSON_CreateObject();
    cJSON_AddStringToObject(location, "uri", uri);
    free(uri);
    cJSON *span = cJSON_CreateObject();
    cJSON_AddItemToObject(location, "range", span);
    add_position(span, "start", range.start);
    add_position(span, "end", range.end);
    return location;
}
