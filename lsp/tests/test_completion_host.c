// L^ (lhat) -- LSP server tests: offering a name the unit has not taken in
// (07 の 4 章 with 05 の 8.7).
//
// Apart from test_completion.c because this half needs a real LhatProgram:
// what import^ would reach is program->hosted, the one nested table the
// registrations build, and no standalone unit has one. The fixture is
// test_host_config.c's -- a fake disk, a config, lsp_host_config_apply --
// since that is exactly the setup that fills the table.

#include <stdlib.h>
#include <string.h>

#include "program_internal.h"

#include "completion.h"
#include "host_config.h"
#include "testutil.h"

// The same table-driven fake disk test_host_config.c reads units from.
typedef struct {
    const char *path;
    const char *text;
} File;

typedef struct {
    const File *files;
    size_t count;
} Disk;

static char *disk_load(void *context, const char *path, size_t *length)
{
    Disk *disk = (Disk *)context;
    for (size_t i = 0; i < disk->count; i++) {
        if (strcmp(disk->files[i].path, path) != 0) {
            continue;
        }
        size_t size = strlen(disk->files[i].text);
        char *copy = (char *)malloc(size + 1);
        if (copy != NULL) {
            memcpy(copy, disk->files[i].text, size + 1);
            *length = size;
        }
        return copy;
    }
    return NULL;
}

// Enough of a host to have a module worth reaching for: std.io with an
// `open` nothing else spells.
static const char *const CONFIG =
    "{\n"
    "  \"types\": [\n"
    "    {\"kind\": \"hostdata\", \"module\": \"std.io\", \"name\": \"File\"}\n"
    "  ],\n"
    "  \"functions\": [\n"
    "    {\"kind\": \"func\", \"module\": \"std.io\", \"name\": \"open\","
    " \"signature\": \"f^string^ -> std.io.File;\"},\n"
    "    {\"kind\": \"func\", \"module\": \"std.math\", \"name\": \"floor\","
    " \"signature\": \"f^number^ -> number^;\"}\n"
    "  ]\n"
    "}\n";

// A whole session: the config applied, the source checked, the completion
// asked where `needle` ends. `run` is handed the items and the unit.
typedef void (*Asking)(const char *label, cJSON *items);

static void ask(const char *label, const char *source, const char *needle,
                Asking run)
{
    File file = {"main.lh", source};
    Disk disk;
    disk.files = &file;
    disk.count = 1;

    LspHostConfig *config = lsp_host_config_parse(CONFIG, strlen(CONFIG));
    LHAT_CHECK(config != NULL, "%s: the config parsed", label);
    if (config == NULL) {
        return;
    }
    LhatProgram program;
    lhat_program_init(&program, true, disk_load, &disk);
    lsp_host_config_apply(config, &program);

    const LhatUnit *unit = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(unit != NULL, "%s: the unit was read", label);
    if (unit != NULL) {
        const char *found = strstr(unit->source.text, needle);
        LHAT_CHECK(found != NULL, "%s: expected \"%s\" in the source", label,
                   needle);
        if (found != NULL) {
            uint32_t at = (uint32_t)(found - unit->source.text) +
                          (uint32_t)strlen(needle);
            cJSON *items = lsp_completion_for_unit(unit, at, NULL, 0);
            run(label, items);
            cJSON_Delete(items);
        }
    }
    lhat_program_dispose(&program);
    lsp_host_config_free(config);
}

static const cJSON *item_named(const cJSON *items, const char *label)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strcmp(at->valuestring, label) == 0) {
            return item;
        }
    }
    return NULL;
}

static const char *extra_text(const cJSON *item)
{
    const cJSON *edits =
        cJSON_GetObjectItemCaseSensitive(item, "additionalTextEdits");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(edits, 0), "newText");
    return cJSON_IsString(text) ? text->valuestring : NULL;
}

static int times_offered(const cJSON *items, const char *label)
{
    int seen = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strcmp(at->valuestring, label) == 0) {
            seen++;
        }
    }
    return seen;
}

static const char *written_by(const cJSON *item)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(item, "textEdit"), "newText");
    return cJSON_IsString(text) ? text->valuestring : NULL;
}

static void expect_taken_in(const char *label, cJSON *items)
{
    const cJSON *open = item_named(items, "open");
    LHAT_CHECK(open != NULL, "%s: expected open to be offered", label);
    if (open == NULL) {
        return;
    }
    // 05 の 8.7 binds the root and leaves the rest as members, so what gets
    // written is the whole path and not the member alone.
    const char *whole = written_by(open);
    LHAT_CHECK(whole != NULL && strcmp(whole, "std.io.open") == 0,
               "%s: expected std.io.open to be written, got %s", label,
               whole != NULL ? whole : "nothing");
    const char *line = extra_text(open);
    LHAT_CHECK(line != NULL && strcmp(line, "import^ std.io\n") == 0,
               "%s: expected the import^ line, got %s", label,
               line != NULL ? line : "nothing");
    // A module of its own holds no name a writer takes; only its members do.
    LHAT_CHECK(item_named(items, "io") == NULL,
               "%s: expected the module itself not to be offered", label);
}

static void expect_no_line(const char *label, cJSON *items)
{
    const cJSON *open = item_named(items, "open");
    LHAT_CHECK(open != NULL, "%s: expected open to still be offered", label);
    LHAT_CHECK(open == NULL || extra_text(open) == NULL,
               "%s: expected no second import^", label);
}

static void test_a_module_not_taken_in(void)
{
    LHAT_TEST("05 の 8.7: a registered module's members are offered with the "
              "import^ that reaches them");
    ask("plain", "let^ a = 1\nop\n", "op", expect_taken_in);

    LHAT_TEST("and the same under a module^ or a comment block");
    ask("module^", "module^ demo.here\nlet^ a = 1\nop\n", "\nop",
        expect_taken_in);
    ask("comments", "# What this is for.\nlet^ a = 1\nop\n", "\nop",
        expect_taken_in);

    LHAT_TEST("but a module already taken in needs no second line");
    ask("taken in", "import^ std.io\nlet^ a = 1\nop\n", "\nop",
        expect_no_line);

    // 05 の 8.7: the registry is one nested table, so the parent holds the
    // child -- 'import^ std' puts std.io within reach as well.
    LHAT_TEST("and neither does one whose parent was taken in");
    ask("parent", "import^ std\nlet^ a = 1\nop\n", "\nop", expect_no_line);
}

static void expect_shadowed(const char *label, cJSON *items)
{
    // 8 章: the writer can already reach a name of that spelling, so that is
    // the one they meant. Offering the other as well would put two `open`s
    // in the list with no way to tell them apart.
    LHAT_CHECK(times_offered(items, "open") == 1,
               "%s: expected one open, got %d", label,
               times_offered(items, "open"));
    const cJSON *open = item_named(items, "open");
    LHAT_CHECK(open == NULL || extra_text(open) == NULL,
               "%s: expected the binding rather than the module's", label);
}

static void test_what_is_already_reachable(void)
{
    LHAT_TEST("8 章: a name in scope keeps its spelling to itself");
    ask("shadowed", "let^ open = 1\nop\n", "\nop", expect_shadowed);
}

static void expect_after_a_dot(const char *label, cJSON *items)
{
    // 14.10: the receiver answers, and nothing is offered that would need a
    // line added to reach it.
    LHAT_CHECK(item_named(items, "floor") == NULL,
               "%s: expected no module member after a dot", label);
    LHAT_CHECK(item_named(items, "let^") == NULL,
               "%s: expected no word after a dot", label);
}

static void test_a_dot_still_answers_alone(void)
{
    LHAT_TEST("14.10: and after a dot the receiver still answers alone");
    ask("after a dot", "let^ t = { x = 1 }\nlet^ n = t.\n", "t.",
        expect_after_a_dot);
}

int main(void)
{
    test_a_module_not_taken_in();
    test_what_is_already_reachable();
    test_a_dot_still_answers_alone();
    return lhat_test_report("test_completion_host");
}
