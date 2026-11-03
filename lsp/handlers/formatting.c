// L^ (lhat) -- LSP server: textDocument/formatting ([lsp.format]).
//
// The editor's own text, formatted by the library (src/format.h) and handed
// back as one edit over the whole of it. What the editor says about tabs is
// the starting point; a .editorconfig that governs the file has the last
// word, so the editor and `lhat --format` write the same file.

#include "formatting.h"

#include <stdlib.h>
#include <string.h>

#include "format.h"
#include "lhat/port.h"

#include "lton.h"
#include "position.h"
#include "server.h"
#include "uri.h"

cJSON *lsp_handle_formatting(LspServer *server, const cJSON *params)
{
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *uri_item =
        cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    if (!cJSON_IsString(uri_item)) {
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
        return NULL;
    }

    LhatFormatOptions options;
    lhat_format_defaults(&options);
    const cJSON *asked = cJSON_GetObjectItemCaseSensitive(params, "options");
    const cJSON *tab_size = cJSON_GetObjectItemCaseSensitive(asked, "tabSize");
    const cJSON *spaces = cJSON_GetObjectItemCaseSensitive(asked, "insertSpaces");
    if (cJSON_IsNumber(tab_size) && tab_size->valueint > 0) {
        options.indent_size = options.tab_width = (uint32_t)tab_size->valueint;
    }
    if (cJSON_IsBool(spaces)) {
        options.indent_with_tabs = cJSON_IsFalse(spaces);
    }
    lhat_format_read_editorconfig(path, &options);

    char *formatted = NULL;
    size_t formatted_length = 0;
    cJSON *edits = NULL;
    if (lhat_format(text, length, lsp_lton_is_path(path), &options, &formatted,
                    &formatted_length) == LHAT_FORMAT_OK) {
        // An answer of no edits rather than none: the text was read, and
        // there was nothing to change.
        edits = cJSON_CreateArray();
        if (formatted_length != length || memcmp(formatted, text, length) != 0) {
            cJSON *edit = cJSON_CreateObject();
            cJSON_AddItemToObject(edit, "range",
                lsp_range_json((LspPosition){0, 0},
                               lsp_position_at(text, length, (uint32_t)length)));
            cJSON_AddStringToObject(edit, "newText", formatted);
            cJSON_AddItemToArray(edits, edit);
        }
    }
    lhat_free(formatted);
    free(text);
    free(path);
    return edits;
}
