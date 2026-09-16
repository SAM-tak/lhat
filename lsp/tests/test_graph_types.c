#include <string.h>
#include <stdlib.h>
#include "fixture.h"
#include "ast_json.h"
#include "graph_types.h"

static LhatUnit as_unit(const Unit *u)
{
    LhatUnit unit = {0};
    unit.path = (char *)"test.lh"; unit.loaded = true;
    unit.source = u->source; unit.lexer = u->lexer;
    unit.parsed = u->parsed; unit.checked = u->checked;
    return unit;
}
static bool has(cJSON *reply, const char *text)
{
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(reply, "candidates"))
        if (strcmp(item->valuestring, text) == 0) return true;
    return false;
}
static void candidates(void)
{
    LHAT_TEST("type options use checker types, scoped aliases and compatible initializers");
    Unit u;
    check_text(&u, "let^ Numeric = number^|nil^\nlet^ value = 42\nlet^ text = \"hi\"\n");
    LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 0);
    LhatUnit unit = as_unit(&u);
    cJSON *reply = lsp_graph_type_options(&unit, (uint32_t)(strstr(u.source.text, "value") - u.source.text));
    LHAT_CHECK(reply != NULL && has(reply, "number^") && has(reply, "Numeric"), "numeric options missing");
    LHAT_CHECK(!has(reply, "string^") && !has(reply, "text") && !has(reply, "value"), "non-types or incompatible types offered");
    cJSON_Delete(reply);
    cJSON *tree = lsp_ast_json_for_unit(&unit);
    char *printed = cJSON_PrintUnformatted(tree);
    LHAT_CHECK(printed && strstr(printed, "\"inferredType\":\"number^\""), "unused bindings need inferred types too");
    free(printed); cJSON_Delete(tree);
    unit_dispose(&u);
}
static void members(void)
{
    LHAT_TEST("ordinary, computed and definition members accept checked annotations");
    const char *sources[] = {
        "let^ value = { count:number^ = 42, [\"name\"]:string^ = \"hi\" }\n",
        "let^ Value = def^{ count:number^ = 42, self^{ speed:number^ = 4, abstract^name:string^ } }\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        Unit u; check_text(&u, sources[i]);
        LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
        LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 0);
        LhatUnit unit = as_unit(&u);
        cJSON *reply = lsp_graph_type_options(&unit, (uint32_t)(strstr(u.source.text, "count") - u.source.text));
        LHAT_CHECK(reply && has(reply, "number^") && !has(reply, "string^"), "member options not constrained");
        cJSON_Delete(reply); unit_dispose(&u);
    }
    const char *bad[] = { "let^ value = { count:string^ = 42 }", "let^ Value = def^{ count:string^ = 42 }", "let^ value = { [\"count\"]:string^ = 42 }" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        Unit u; check_text(&u, bad[i]);
        LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
        LHAT_CHECK(has_error(&u, LHAT_CHECK_ERR_MISMATCH), "annotation mismatch was ignored");
        unit_dispose(&u);
    }
    LHAT_TEST("annotated table members retain runtime values");
    Run r; run_checked_text(&r, "let^ t = { count:number^ = 42 }\nreturn^ t.count");
    CHECK_INTEGER(&r, 42); run_dispose(&r);
}
int main(void)
{
    candidates(); members();
    return lhat_test_report("test_graph_types");
}
