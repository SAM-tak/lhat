// L^ (lhat) -- LSP server: LhatFix -> LSP CodeAction[].

#include "code_action.h"

#include <stdlib.h>
#include <string.h>

#include "position.h"

// LSP's CodeActionKind for a fix of a diagnostic.
#define LSP_QUICK_FIX "quickfix"

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

// The title, in the program's language (10 §7.1). Wide enough for every one
// the library writes; the heap is for a translation that runs long.
static cJSON *title_json(const LhatUnit *unit, size_t index, size_t which)
{
    char room[256];
    size_t needed =
        lhat_unit_diagnostic_fix_title(unit, index, which, room, sizeof room);
    if (needed < sizeof room) {
        return cJSON_CreateString(room);
    }
    char *bigger = (char *)malloc(needed + 1);
    if (bigger == NULL) {
        return cJSON_CreateString(room);  // cut, but better than silence
    }
    lhat_unit_diagnostic_fix_title(unit, index, which, bigger, needed + 1);
    cJSON *title = cJSON_CreateString(bigger);
    free(bigger);
    return title;
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
            cJSON *edits = cJSON_CreateArray();
            for (size_t e = 0; e < fix.edit_count; e++) {
                cJSON_AddItemToArray(edits, edit_json(unit, &fix.edits[e]));
            }
            cJSON *changes = cJSON_CreateObject();
            cJSON_AddItemToObject(changes, uri, edits);
            cJSON *edit = cJSON_CreateObject();
            cJSON_AddItemToObject(edit, "changes", changes);

            cJSON *action = cJSON_CreateObject();
            cJSON_AddItemToObject(action, "title", title_json(unit, i, which));
            cJSON_AddStringToObject(action, "kind", LSP_QUICK_FIX);
            // 07 §6: a fix the library calls machine-applicable is the one an
            // editor may put first and apply without being read. A suggested
            // one is a guess, and stands in the list like any other.
            if (fix.confidence == LHAT_FIX_MACHINE && fixes == 1) {
                cJSON_AddBoolToObject(action, "isPreferred", true);
            }
            cJSON_AddItemToObject(action, "edit", edit);
            cJSON_AddItemToArray(array, action);
        }
    }
    return array;
}
