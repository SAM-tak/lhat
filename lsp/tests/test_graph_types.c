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
static void self_annotation(void)
{
    LHAT_TEST("a definition cannot use its own binding as its annotation");
    const char *sources[] = {
        "let^ nested = def^{}\n",
        "let^ nested:nested = def^{}\n",
        "let^ nested:nested|nil^ = def^{}\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        Unit u; check_text(&u, sources[i]);
        LhatUnit unit = as_unit(&u);
        cJSON *reply = lsp_graph_type_options(&unit, (uint32_t)(strstr(u.source.text, "nested") - u.source.text));
        LHAT_CHECK(reply && !has(reply, "nested") && !has(reply, "nested.Box^") && !has(reply, "nested|nil^"),
                   "the binding being annotated was offered as its own type");
        LHAT_CHECK(reply && has(reply, "any^"), "an invalid annotation must not prevent recovery");
        cJSON_Delete(reply); unit_dispose(&u);
    }
    LHAT_TEST("an enclosing definition remains valid inside its body and for unrelated fields");
    const char *source = "let^ nested = def^{ get = f^self^{ return^ self^ } }\nlet^ t = { nested:nested = nested.new() }\n";
    Unit u; check_text(&u, source);
    LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 0);
    LhatUnit unit = as_unit(&u);
    const char *positions[] = { strstr(source, "f^self^"), strstr(source, "nested:nested") };
    for (size_t i = 0; i < sizeof positions / sizeof *positions; i++) {
        cJSON *reply = lsp_graph_type_options(&unit, (uint32_t)(positions[i] - source));
        LHAT_CHECK(reply && has(reply, "nested"), "a valid enclosing type was filtered by spelling alone");
        cJSON_Delete(reply);
    }
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
static void signatures_and_declarations(void)
{
    LHAT_TEST("function arguments and results expose the same compatible type choices");
    Unit u;
    check_text(&u, "let^ f = f^ value:number^ { return^ value }\n");
    LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 0);
    LhatUnit unit = as_unit(&u);
    cJSON *argument = lsp_graph_type_options(&unit,
        (uint32_t)(strstr(u.source.text, "value:number^") - u.source.text));
    LHAT_CHECK(argument && has(argument, "number^") && !has(argument, "string^"),
               "argument options are not constrained by its type");
    cJSON_Delete(argument);
    cJSON *result = lsp_graph_type_options(&unit,
        (uint32_t)(strstr(u.source.text, "f^ value") - u.source.text));
    LHAT_CHECK(result && has(result, "number^") && !has(result, "string^"),
               "inferred result options are not constrained by its type");
    cJSON_Delete(result);
    cJSON *tree = lsp_ast_json_for_unit(&unit);
    char *printed = cJSON_PrintUnformatted(tree);
    LHAT_CHECK(printed && strstr(printed, "\"inferredReturnType\":\"number^\""),
               "function result type is missing from AST metadata");
    free(printed); cJSON_Delete(tree); unit_dispose(&u);

    LHAT_TEST("type table declarations expose choices for their keys");
    check_text(&u, "let^ Shape = t^{ count:number^ }\n");
    LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
    LHAT_CHECK_EQ_INT(u.checked.diagnostic_count, 0);
    unit = as_unit(&u);
    cJSON *member = lsp_graph_type_options(&unit,
        (uint32_t)(strstr(u.source.text, "count") - u.source.text));
    LHAT_CHECK(member && has(member, "number^"), "declared key has no type choices");
    cJSON_Delete(member); unit_dispose(&u);

}
static char *host_source(void *context, const char *path, size_t *length)
{
    (void)path;
    const char *text = context;
    *length = strlen(text);
    char *copy = malloc(*length + 1);
    if (copy) memcpy(copy, text, *length + 1);
    return copy;
}
static void multiple_results(void)
{
    LHAT_TEST("each inferred or annotated return slot has its own compatible candidates");
    const char *sources[] = {
        "let^ pair = f^{ return^ 1, \"x\" }\n",
        "let^ pair = f^ -> number^, string^ { return^ 1, \"x\" }\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        Unit u; check_text(&u, sources[i]);
        LHAT_CHECK_EQ_INT(u.parsed.diagnostic_count, 0);
        LhatUnit unit = as_unit(&u);
        uint32_t at = (uint32_t)(strstr(u.source.text, "f^") - u.source.text);
        cJSON *first = lsp_graph_type_options_result(&unit, at, 0);
        cJSON *second = lsp_graph_type_options_result(&unit, at, 1);
        LHAT_CHECK(first && has(first, "number^") && !has(first, "string^"), "first result candidates mixed with second");
        LHAT_CHECK(second && has(second, "string^") && !has(second, "number^"), "second result candidates mixed with first");
        cJSON *invalid = lsp_graph_type_options_result(&unit, at, 2);
        LHAT_CHECK(invalid == NULL, "out of range result accepted");
        cJSON_Delete(first); cJSON_Delete(second); cJSON_Delete(invalid); unit_dispose(&u);
    }
}
static void host_types(void)
{
    LHAT_TEST("qualified host types include compatible base types, not unrelated siblings");
    const char *source = "import^godot\nlet^ f = p^body:godot.Node2D {}\n";
    LhatProgram program;
    lhat_program_init(&program, true, host_source, (void *)source);
    LHAT_CHECK(lhat_register_hostdata_type(&program, "godot", "Node") != NULL, "base registration failed");
    LHAT_CHECK(lhat_register_hostdata_subtype(&program, "godot", "Node2D", "godot", "Node") != NULL, "subtype registration failed");
    LHAT_CHECK(lhat_register_hostdata_subtype(&program, "godot", "Node3D", "godot", "Node") != NULL, "sibling registration failed");
    const LhatUnit *unit = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(unit != NULL, "host unit was not checked");
    if (unit) {
        LHAT_CHECK_EQ_INT(unit->parsed.diagnostic_count, 0);
        LHAT_CHECK_EQ_INT(unit->checked.diagnostic_count, 0);
        cJSON *reply = lsp_graph_type_options(unit, (uint32_t)(strstr(source, "body") - source));
        LHAT_CHECK(reply && has(reply, "godot.Node2D") && has(reply, "godot.Node"), "compatible host types missing");
        LHAT_CHECK(!has(reply, "godot.Node3D"), "unrelated host type offered");
        cJSON_Delete(reply);
    }
    lhat_program_dispose(&program);
}
int main(void)
{
    candidates(); self_annotation(); members(); signatures_and_declarations(); host_types(); multiple_results();
    return lhat_test_report("test_graph_types");
}
