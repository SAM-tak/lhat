// L^ (lhat) -- LSP server: textDocument/documentSymbol.
//
// The one request answered from the editor's own text rather than from a
// checked unit. An outline needs only the tree (document_symbol.h), and the
// editor asks for it the moment a file opens -- before the worker has
// checked anything (workspace.h), when lsp_workspace_with_unit would answer
// nothing and the outline would stay empty until the next edit. Parsing the
// open text here is what the workspace's loader does anyway (lsp_program_load
// reads the store first), on the one file asked about, with no lock to hold.

#include "document_symbol.h"

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/source.h"
#include "parser.h"

// The serialiser, disambiguated from this handler's own header of the same
// basename, the way ast.c and definition.c already have to say.
#include "../document_symbol.h"

#include "lton.h"
#include "server.h"
#include "uri.h"
#include "workspace.h"

cJSON *lsp_handle_document_symbol(LspServer *server, const cJSON *params)
{
    if (params == NULL) {
        return NULL;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (text_document == NULL) {
        return NULL;
    }
    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(text_document, "uri");
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
        return NULL;  // not open: nothing to outline
    }
    // 08 章: an LTON file is the inside of a table literal, read through the
    // wrapper (lsp/lton.h). Positions come back out through
    // lsp_unit_position_at, which knows the unit by its path.
    if (lsp_lton_is_path(path)) {
        size_t whole = 0;
        char *wrapped = lsp_lton_wrap(text, length, &whole);
        free(text);
        if (wrapped == NULL) {
            free(path);
            return NULL;
        }
        text = wrapped;
        length = whole;
    }

    LhatUnit unit;
    memset(&unit, 0, sizeof unit);
    unit.path = path;
    unit.loaded = true;
    cJSON *symbols = NULL;
    if (lhat_source_init_from_string(&unit.source, path, text, length)) {
        lhat_lexer_init(&unit.lexer, &unit.source);
        lhat_parse(&unit.lexer, &unit.parsed);
        symbols = lsp_document_symbols_for_unit(&unit);
        lhat_parse_result_dispose(&unit.parsed);
        lhat_lexer_dispose(&unit.lexer);
        lhat_source_dispose(&unit.source);
    }
    free(text);
    free(path);
    return symbols;
}
