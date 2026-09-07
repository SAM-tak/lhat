// L^ (lhat) -- LSP server: textDocument/completion.
//
// The one request that does not read the workspace's checked units. It is
// asked on the keystroke that made the text -- the '.' has only now been
// typed -- and the worker waits out a debounce before it re-checks, so the
// unit the roots hold has never seen the dot. So the text is taken from the
// document store and checked here, on this thread
// (lsp_workspace_with_fresh_unit).
//
// Two of the four questions never get that far: a module path and a
// require^'s string are read off the text, because neither has a tree
// worth asking (a half-written path resolves to nothing, and an
// unterminated string is one error token running to the end of the file).

#include "completion.h"

#include <stdlib.h>
#include <string.h>

// The logic, disambiguated from this handler's own header of the same
// basename -- the way hover.c and definition.c already have to say.
#include "../completion.h"

#include "lhat/port.h"  // lhat_free: the document store allocates with it

#include "position.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

typedef struct {
    int line;
    int character;
    cJSON *items;
} CompletionRequest;

static void collect(void *context, const LhatUnit *unit)
{
    CompletionRequest *request = (CompletionRequest *)context;
    // The unit's own conversion: an LTON file's text is the file's wrapped
    // (lsp/lton.h), so a position and an offset are not the same thing there.
    uint32_t offset =
        lsp_unit_offset_at(unit, request->line, request->character);
    request->items = lsp_completion_for_unit(unit, offset);
}

// What the two textual questions answer with, or NULL when neither applies.
// `text` is the whole document and `offset` the cursor. They are asked
// before the tree because each is a word being typed that has a better
// answer than the language's own words would be.
static cJSON *answer_from_the_text(LspServer *server, const char *path,
                                   const char *text, size_t length,
                                   uint32_t offset)
{
    uint32_t from = 0;
    if (lsp_completion_require_prefix(text, length, offset, &from)) {
        size_t count = 0;
        char **paths =
            lsp_workspace_copy_unit_paths(&server->workspace, &count);
        cJSON *items = lsp_completion_path_items(
            path, (const char *const *)paths, count);
        lsp_workspace_free_strings(paths, count);
        return items;
    }
    if (lsp_completion_import_prefix(text, length, offset, &from)) {
        size_t count = 0;
        char **modules =
            lsp_workspace_copy_module_names(&server->workspace, &count);
        cJSON *items = lsp_completion_module_items(
            (const char *const *)modules, count, text + from, offset - from);
        lsp_workspace_free_strings(modules, count);
        return items;
    }
    return NULL;
}

cJSON *lsp_handle_completion(LspServer *server, const cJSON *params)
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

    size_t length = 0;
    char *text = lsp_document_store_copy(&server->workspace.documents, path,
                                         &length);
    if (text == NULL) {
        free(path);
        return NULL;  // not open: there is no text to be asked about
    }

    // The two textual questions first. A '.' inside 'import^ std.' would
    // otherwise fall through to a member lookup that finds nothing, and the
    // check it costs would be spent for that nothing.
    uint32_t offset =
        lsp_offset_at(text, length, line->valueint, character->valueint);
    cJSON *items = answer_from_the_text(server, path, text, length, offset);
    lhat_free(text);

    if (items == NULL) {
        CompletionRequest request;
        request.line = line->valueint;
        request.character = character->valueint;
        request.items = NULL;
        lsp_workspace_with_fresh_unit(&server->workspace, path, collect,
                                      &request);
        items = request.items;
    }
    free(path);
    if (items == NULL) {
        return NULL;  // a JSON null, which the editor reads as "nothing"
    }

    // A CompletionList rather than a bare array, so that isIncomplete can be
    // said: the set is the whole of what may stand there, so the editor
    // filters it down as the name is typed and does not ask again.
    cJSON *list = cJSON_CreateObject();
    if (list == NULL) {
        cJSON_Delete(items);
        return NULL;
    }
    cJSON_AddBoolToObject(list, "isIncomplete", false);
    cJSON_AddItemToObject(list, "items", items);
    return list;
}
