// L^ (lhat) -- LSP server: textDocument/references and textDocument/rename.
//
// One question under two names. Both begin by asking which declaration the
// cursor is about (lsp/references.h) and then walk every unit of every
// checked root for the uses pointing at it; they differ only in what they
// build from the answer -- a flat list of Locations, or a WorkspaceEdit
// grouping the same places by file.
//
// prepareRename is the third, and it is the same walk stopped after the
// first step: what it answers is the range the editor puts its box over, and
// refusing there is what keeps a writer from typing a new name for something
// that was never going to be renamed.

#include "references.h"

#include <stdlib.h>
#include <string.h>

// The logic, disambiguated from this handler's own header of the same
// basename -- the way hover.c and definition.c already have to say.
#include "../references.h"

#include "position.h"
#include "server.h"
#include "uri.h"
#include "util.h"
#include "workspace.h"

// ---------------------------------------------------------------------------
// Asking which declaration
// ---------------------------------------------------------------------------

typedef struct {
    int line;
    int character;
    bool found;
    // Copies: the target borrows from a unit's source, and the unit is only
    // ours for as long as the sink runs.
    char *path;
    uint32_t offset;
    char *name;
    uint32_t name_length;
    // Where the name stands under the cursor, in the file that was asked
    // about. prepareRename draws its box here rather than over the
    // declaration, which may be in another file entirely.
    cJSON *here;
} Asked;

static void take_target(void *context, const LhatUnit *unit)
{
    Asked *asked = (Asked *)context;
    uint32_t offset =
        lsp_unit_offset_at(unit, asked->line, asked->character);
    LspReferenceTarget target;
    if (!lsp_references_target(unit, offset, &target)) {
        return;
    }
    asked->path = lsp_strdup(target.path);
    asked->name = lsp_strndup(target.name, target.name_length);
    asked->offset = target.offset;
    asked->name_length = target.name_length;
    asked->found = asked->path != NULL && asked->name != NULL;
    if (asked->found) {
        // The target's name is the cursor's name -- reaching it required the
        // same spelling -- so its length measures the range here too.
        uint32_t from = (uint32_t)(target.name - unit->source.text);
        asked->here = lsp_unit_range_json(unit, from,
                                          from + target.name_length);
    }
}

static void asked_dispose(Asked *asked)
{
    free(asked->path);
    free(asked->name);
    cJSON_Delete(asked->here);
    asked->path = NULL;
    asked->name = NULL;
    asked->here = NULL;
}

// The position of `params`, and the file it names. False when the request is
// not shaped like a position one; `*path` is the caller's to free.
static bool position_of(const cJSON *params, char **path, int *line,
                        int *character)
{
    if (params == NULL) {
        return false;
    }
    const cJSON *text_document =
        cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(params, "position");
    const cJSON *uri =
        cJSON_GetObjectItemCaseSensitive(text_document, "uri");
    const cJSON *at_line = cJSON_GetObjectItemCaseSensitive(position, "line");
    const cJSON *at_character =
        cJSON_GetObjectItemCaseSensitive(position, "character");
    if (!cJSON_IsString(uri) || !cJSON_IsNumber(at_line) ||
        !cJSON_IsNumber(at_character)) {
        return false;
    }
    *path = lsp_uri_to_absolute_path(uri->valuestring);
    *line = at_line->valueint;
    *character = at_character->valueint;
    return *path != NULL;
}

// Asks the workspace which declaration the cursor is about. False leaves
// nothing to free.
static bool target_at(LspServer *server, const cJSON *params, Asked *asked,
                      char **path)
{
    memset(asked, 0, sizeof *asked);
    if (!position_of(params, path, &asked->line, &asked->character)) {
        return false;
    }
    lsp_workspace_with_unit(&server->workspace, *path, take_target, asked);
    if (!asked->found) {
        asked_dispose(asked);
        free(*path);
        *path = NULL;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Walking every unit for the uses
// ---------------------------------------------------------------------------

typedef struct {
    const Asked *asked;
    bool with_declaration;
    // Where each place goes. `flat` is a Locations array; `grouped` is a
    // WorkspaceEdit's `changes`, one TextEdit array per URI. Exactly one is
    // set, which is the whole of the difference between the two requests.
    cJSON *flat;
    cJSON *grouped;
    char *new_name;
} Walk;

// The array this file's edits go into, made on first use so a file with no
// use of the name gets no empty entry.
static cJSON *edits_for(Walk *walk, const LhatUnit *unit)
{
    char *uri = lsp_absolute_path_to_uri(unit->path);
    if (uri == NULL) {
        return NULL;
    }
    cJSON *edits = cJSON_GetObjectItemCaseSensitive(walk->grouped, uri);
    if (edits == NULL) {
        edits = cJSON_AddArrayToObject(walk->grouped, uri);
    }
    free(uri);
    return edits;
}

static void note(void *context, const LhatUnit *unit, uint32_t from,
                 uint32_t to)
{
    Walk *walk = (Walk *)context;
    cJSON *range = lsp_unit_range_json(unit, from, to);
    if (range == NULL) {
        return;
    }
    if (walk->grouped != NULL) {
        cJSON *edits = edits_for(walk, unit);
        cJSON *edit = edits != NULL ? cJSON_CreateObject() : NULL;
        if (edit == NULL) {
            cJSON_Delete(range);
            return;
        }
        cJSON_AddItemToArray(edits, edit);
        cJSON_AddItemToObject(edit, "range", range);
        cJSON_AddStringToObject(edit, "newText", walk->new_name);
        return;
    }
    char *uri = lsp_absolute_path_to_uri(unit->path);
    cJSON *location = uri != NULL ? cJSON_CreateObject() : NULL;
    if (location == NULL) {
        free(uri);
        cJSON_Delete(range);
        return;
    }
    cJSON_AddItemToArray(walk->flat, location);
    cJSON_AddStringToObject(location, "uri", uri);
    free(uri);
    cJSON_AddItemToObject(location, "range", range);
}

static void walk_unit(void *context, const LhatUnit *unit)
{
    Walk *walk = (Walk *)context;
    LspReferenceTarget target;
    target.path = walk->asked->path;
    target.offset = walk->asked->offset;
    target.name = walk->asked->name;
    target.name_length = walk->asked->name_length;

    // The declaration first, so a file that holds it reads from the top down
    // the way its text does.
    uint32_t from = 0;
    uint32_t to = 0;
    if (walk->with_declaration &&
        lsp_references_declaration_in(unit, &target, &from, &to)) {
        note(walk, unit, from, to);
    }
    lsp_references_in_unit(unit, &target, note, walk);
}

// ---------------------------------------------------------------------------
// The three requests
// ---------------------------------------------------------------------------

cJSON *lsp_handle_references(LspServer *server, const cJSON *params)
{
    Asked asked;
    char *path = NULL;
    if (!target_at(server, params, &asked, &path)) {
        return NULL;
    }
    free(path);

    const cJSON *context = cJSON_GetObjectItemCaseSensitive(params, "context");
    const cJSON *wants = cJSON_GetObjectItemCaseSensitive(
        context, "includeDeclaration");

    Walk walk;
    memset(&walk, 0, sizeof walk);
    walk.asked = &asked;
    walk.with_declaration = !cJSON_IsBool(wants) || cJSON_IsTrue(wants);
    walk.flat = cJSON_CreateArray();
    if (walk.flat != NULL) {
        lsp_workspace_with_every_unit(&server->workspace, walk_unit, &walk);
    }
    asked_dispose(&asked);
    return walk.flat;
}

cJSON *lsp_handle_prepare_rename(LspServer *server, const cJSON *params)
{
    Asked asked;
    char *path = NULL;
    if (!target_at(server, params, &asked, &path)) {
        // No range means "there is nothing here to rename", which a client
        // shows instead of opening a box over something bound to fail.
        return NULL;
    }
    free(path);
    cJSON *range = asked.here;
    asked.here = NULL;  // handed over
    asked_dispose(&asked);
    return range;
}

cJSON *lsp_handle_rename(LspServer *server, const cJSON *params)
{
    const cJSON *fresh = cJSON_GetObjectItemCaseSensitive(params, "newName");
    if (!cJSON_IsString(fresh) ||
        !lsp_references_is_name(fresh->valuestring,
                                strlen(fresh->valuestring))) {
        return NULL;  // 01 の 3.1: not a name the lexer would read back
    }
    Asked asked;
    char *path = NULL;
    if (!target_at(server, params, &asked, &path)) {
        return NULL;
    }
    free(path);

    Walk walk;
    memset(&walk, 0, sizeof walk);
    walk.asked = &asked;
    walk.with_declaration = true;  // a rename that left it behind renames nothing
    walk.new_name = fresh->valuestring;
    cJSON *edit = cJSON_CreateObject();
    walk.grouped = edit != NULL ? cJSON_AddObjectToObject(edit, "changes")
                                : NULL;
    if (walk.grouped != NULL) {
        lsp_workspace_with_every_unit(&server->workspace, walk_unit, &walk);
    }
    asked_dispose(&asked);
    return edit;
}
