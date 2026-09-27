// L^ (lhat) -- LSP server: LhatFix -> LSP CodeAction[].

#include "code_action.h"

#include <stdlib.h>
#include <string.h>

#include "position.h"

// LSP's CodeActionKinds: a fix of a diagnostic, and every fix at once.
#define LSP_QUICK_FIX "quickfix"
#define LSP_FIX_ALL "source.fixAll"

// Whether a diagnostic's place meets the range the editor asked about. A
// diagnostic with no width still meets the position it stands at -- a cursor
// sitting where a token is missing is exactly the case a fix is wanted in.
static bool meets(uint32_t offset, uint32_t length, uint32_t from, uint32_t to)
{
    uint32_t end = offset + (length > 0 ? length : 1);
    return offset <= to && from < end;
}

static cJSON *edit_json(const LhatUnit *unit, const LhatFixEdit *edit)
{
    cJSON *one = cJSON_CreateObject();
    cJSON_AddItemToObject(one, "range",
                          lsp_unit_range_json(unit, edit->offset,
                                              edit->offset + edit->length));
    cJSON_AddStringToObject(one, "newText",
                            edit->text != NULL ? edit->text : "");
    return one;
}

// A title, the fix-all one where `index` is SIZE_MAX.
static size_t write_title(const LhatUnit *unit, size_t index, size_t which,
                          char *out, size_t capacity)
{
    return index == SIZE_MAX
               ? lhat_unit_fix_all_title(unit, out, capacity)
               : lhat_unit_diagnostic_fix_title(unit, index, which, out,
                                                capacity);
}

// The title, in the program's language (10 §7.1). Wide enough for every one
// the library writes; the heap is for a translation that runs long.
static cJSON *title_json(const LhatUnit *unit, size_t index, size_t which)
{
    char room[256];
    size_t needed = write_title(unit, index, which, room, sizeof room);
    if (needed < sizeof room) {
        return cJSON_CreateString(room);
    }
    char *bigger = (char *)malloc(needed + 1);
    if (bigger == NULL) {
        return cJSON_CreateString(room);  // cut, but better than silence
    }
    write_title(unit, index, which, bigger, needed + 1);
    cJSON *title = cJSON_CreateString(bigger);
    free(bigger);
    return title;
}

// A CodeAction of `kind` whose WorkspaceEdit makes `edits` in `uri`.
static cJSON *action_json(const LhatUnit *unit, const char *uri,
                          const char *kind, cJSON *title,
                          const LhatFixEdit *edits, size_t count)
{
    cJSON *list = cJSON_CreateArray();
    for (size_t e = 0; e < count; e++) {
        cJSON_AddItemToArray(list, edit_json(unit, &edits[e]));
    }
    cJSON *changes = cJSON_CreateObject();
    cJSON_AddItemToObject(changes, uri, list);
    cJSON *edit = cJSON_CreateObject();
    cJSON_AddItemToObject(edit, "changes", changes);

    cJSON *action = cJSON_CreateObject();
    cJSON_AddItemToObject(action, "title", title);
    cJSON_AddStringToObject(action, "kind", kind);
    cJSON_AddItemToObject(action, "edit", edit);
    return action;
}

cJSON *lsp_code_actions_for_unit(const LhatUnit *unit, const char *uri,
                                 uint32_t from, uint32_t to)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL || unit == NULL || uri == NULL) {
        return array;
    }
    size_t count = lhat_unit_diagnostic_count(unit);
    for (size_t i = 0; i < count; i++) {
        LhatUnitDiagnostic d = lhat_unit_diagnostic(unit, i);
        if (!meets(d.offset, d.length, from, to)) {
            continue;
        }
        size_t fixes = lhat_unit_diagnostic_fix_count(unit, i);
        for (size_t which = 0; which < fixes; which++) {
            LhatFix fix;
            if (!lhat_unit_diagnostic_fix(unit, i, which, &fix)) {
                continue;
            }
            cJSON *action =
                action_json(unit, uri, LSP_QUICK_FIX, title_json(unit, i, which),
                            fix.edits, fix.edit_count);
            // 07 §6: a fix the library calls machine-applicable is the one an
            // editor may put first and apply without being read. A suggested
            // one is a guess, and stands in the list like any other.
            if (fix.confidence == LHAT_FIX_MACHINE && fixes == 1) {
                cJSON_AddBoolToObject(action, "isPreferred", true);
            }
            cJSON_AddItemToArray(array, action);
        }
    }
    return array;
}

cJSON *lsp_fix_all_for_unit(const LhatUnit *unit, const char *uri)
{
    if (unit == NULL || uri == NULL) {
        return NULL;
    }
    size_t count = lhat_unit_fix_all(unit, NULL, 0);
    if (count == 0) {
        return NULL;
    }
    LhatFixEdit *edits = (LhatFixEdit *)malloc(count * sizeof *edits);
    if (edits == NULL) {
        return NULL;
    }
    lhat_unit_fix_all(unit, edits, count);
    cJSON *action = action_json(unit, uri, LSP_FIX_ALL,
                                title_json(unit, SIZE_MAX, 0), edits, count);
    free(edits);
    return action;
}

bool lsp_code_action_wanted(const cJSON *only, const char *kind)
{
    if (only == NULL) {
        return true;
    }
    size_t length = strlen(kind);
    const cJSON *asked = NULL;
    cJSON_ArrayForEach(asked, only)
    {
        if (!cJSON_IsString(asked)) {
            continue;
        }
        size_t prefix = strlen(asked->valuestring);
        if (prefix <= length &&
            strncmp(kind, asked->valuestring, prefix) == 0 &&
            (kind[prefix] == '\0' || kind[prefix] == '.')) {
            return true;
        }
    }
    return false;
}
