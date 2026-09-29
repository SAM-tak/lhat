// L^ (lhat) -- project-boundary tests for the language server workspace.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "testutil.h"
#include "workspace.h"

static bool make_directory(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static void remove_directory(const char *path)
{
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    size_t length = strlen(text);
    return fwrite(text, 1, length, file) == length && fclose(file) == 0;
}

static bool make_temporary_directory(char *path, size_t capacity)
{
#ifdef _WIN32
    if (capacity < L_tmpnam || tmpnam(path) == NULL) {
        return false;
    }
    for (char *at = path; *at != '\0'; at++) {
        if (*at == '\\') {
            *at = '/';
        }
    }
    return _mkdir(path) == 0;
#else
    static const char pattern[] = "/tmp/lhat-workspace-XXXXXX";
    if (capacity < sizeof pattern) {
        return false;
    }
    memcpy(path, pattern, sizeof pattern);
    return mkdtemp(path) != NULL;
#endif
}

static void join(char *out, size_t capacity, const char *left, const char *right)
{
    snprintf(out, capacity, "%s/%s", left, right);
}

static LspProject *project_at(LspWorkspace *workspace, const char *root)
{
    for (LspProject *project = workspace->projects; project != NULL;
         project = project->next) {
        if (strcmp(project->root_path, root) == 0) {
            return project;
        }
    }
    return NULL;
}

static size_t root_count(const LspProject *project)
{
    size_t count = 0;
    for (const LspRoot *root = project->roots; root != NULL; root = root->next) {
        count++;
    }
    return count;
}

static void test_nearest_project_wins_inside_each_workspace_folder(void)
{
    char base[512];
    LHAT_REQUIRE(make_temporary_directory(base, sizeof base),
                 "could not create a temporary directory");

    char a[512], b[512], nested[512], hidden[512];
    char path[512];
    join(a, sizeof a, base, "a");
    join(b, sizeof b, base, "b");
    join(nested, sizeof nested, a, "nested");
    join(hidden, sizeof hidden, a, ".hidden");
    LHAT_REQUIRE(make_directory(a), "could not create %s", a);
    LHAT_REQUIRE(make_directory(b), "could not create %s", b);
    LHAT_REQUIRE(make_directory(nested), "could not create %s", nested);
    LHAT_REQUIRE(make_directory(hidden), "could not create %s", hidden);

    join(path, sizeof path, base, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, a, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{\"strict\": true}"), "could not write %s", path);
    join(path, sizeof path, a, "lhat-lsp.json");
    LHAT_REQUIRE(write_file(path,
                            "{\"strict\": true, "
                            "\"force_include_files\": [\"nested/inner.lh\"]}"),
                 "could not write %s", path);
    join(path, sizeof path, nested, "lhat-lsp.json");
    LHAT_REQUIRE(write_file(path, "{\"strict\": false}"), "could not write %s", path);
    join(path, sizeof path, hidden, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, b, "lhat-host.json");
    LHAT_REQUIRE(write_file(path, "{}"), "could not write %s", path);
    join(path, sizeof path, a, "outer.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, nested, "inner.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, hidden, "hidden.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);
    join(path, sizeof path, b, "other.lh");
    LHAT_REQUIRE(write_file(path, ""), "could not write %s", path);

    const char *folders[] = {a, b};
    LspWorkspace workspace;
    lsp_workspace_init(&workspace, folders, 2);
    lsp_workspace_discover_projects(&workspace);

    LHAT_TEST("a nearest config directory makes a separate project");
    LspProject *a_project = project_at(&workspace, a);
    LspProject *nested_project = project_at(&workspace, nested);
    LspProject *b_project = project_at(&workspace, b);
    LHAT_REQUIRE(a_project != NULL && nested_project != NULL && b_project != NULL,
                 "expected projects at both folders and the nested config");
    LHAT_CHECK_EQ_INT((int)root_count(a_project), 1);
    LHAT_CHECK_EQ_INT((int)root_count(nested_project), 1);
    LHAT_CHECK_EQ_INT((int)root_count(b_project), 1);

    LHAT_TEST("the two files are an atomic project boundary");
    LHAT_CHECK_EQ_BOOL(lsp_host_config_strict(a_project->host_config, false), true);
    LHAT_CHECK_EQ_BOOL(lsp_settings_strict(a_project->settings, false), true);
    LHAT_CHECK(nested_project->host_config == NULL,
               "nested project must not inherit the parent host config");
    LHAT_CHECK_EQ_BOOL(lsp_settings_strict(nested_project->settings, true), false);

    LHAT_TEST("a config above a workspace folder is not in scope");
    join(path, sizeof path, base, "lhat-host.json");
    LHAT_CHECK_EQ_BOOL(lsp_workspace_is_config_path(&workspace, path), false);

    LHAT_TEST("an opened file also resolves its ancestors on demand");
    LHAT_CHECK(project_at(&workspace, hidden) == NULL,
               "the background scan intentionally did not enter .hidden");
    join(path, sizeof path, hidden, "hidden.lh");
    lsp_workspace_recheck_affected(&workspace, path);
    LspProject *hidden_project = project_at(&workspace, hidden);
    LHAT_REQUIRE(hidden_project != NULL,
                 "opening a file must discover its nearest config directory");
    LHAT_CHECK_EQ_INT((int)root_count(hidden_project), 1);

    lsp_workspace_dispose(&workspace);

    join(path, sizeof path, a, "outer.lh"); remove(path);
    join(path, sizeof path, nested, "inner.lh"); remove(path);
    join(path, sizeof path, nested, "lhat-lsp.json"); remove(path);
    join(path, sizeof path, hidden, "hidden.lh"); remove(path);
    join(path, sizeof path, hidden, "lhat-host.json"); remove(path);
    join(path, sizeof path, a, "lhat-host.json"); remove(path);
    join(path, sizeof path, a, "lhat-lsp.json"); remove(path);
    join(path, sizeof path, b, "other.lh"); remove(path);
    join(path, sizeof path, b, "lhat-host.json"); remove(path);
    join(path, sizeof path, base, "lhat-host.json"); remove(path);
    remove_directory(nested);
    remove_directory(hidden);
    remove_directory(a);
    remove_directory(b);
    remove_directory(base);
}

static void count_unit(void *context, const LhatUnit *unit)
{
    (void)unit;
    (*(int *)context)++;
}

// How many units each of the two ways hands over for `path`: the last one
// checked, and the last one checked from what the path holds now.
static void handed(LspWorkspace *workspace, const char *path, int *last,
                   int *current)
{
    *last = 0;
    *current = 0;
    lsp_workspace_with_unit(workspace, path, count_unit, last);
    lsp_workspace_with_current_unit(workspace, path, count_unit, current);
}

static void open_text(LspWorkspace *workspace, const char *path,
                      const char *text, int version)
{
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, text, length + 1);
    lsp_document_store_put(&workspace->documents, path, copy, length, version);
}

// 07 §6: a fix is an edit against the text that was checked. Between an
// edit and the check that follows it, the unit is the text before the edit,
// and a request that rewrites the source is handed nothing.
static void test_current_unit(void)
{
    char base[512];
    LHAT_REQUIRE(make_temporary_directory(base, sizeof base),
                 "could not create a temporary directory");
    char config[512];
    char path[512];
    join(config, sizeof config, base, "lhat-host.json");
    LHAT_REQUIRE(write_file(config, "{}"), "could not write %s", config);
    join(path, sizeof path, base, "main.lh");
    LHAT_REQUIRE(write_file(path, "var^ a : t^ = { }\n"),
                 "could not write %s", path);

    const char *folders[] = {base};
    LspWorkspace workspace;
    lsp_workspace_init(&workspace, folders, 1);
    lsp_workspace_discover_projects(&workspace);

    int last = 0;
    int current = 0;
    LHAT_TEST("a unit checked from the open text is current");
    open_text(&workspace, path, "var^ a : t^ = { }\n", 1);
    lsp_workspace_recheck_affected(&workspace, path);
    handed(&workspace, path, &last, &current);
    LHAT_CHECK_EQ_INT(last, 1);
    LHAT_CHECK_EQ_INT(current, 1);

    LHAT_TEST("an edit the check has not caught up with is not");
    open_text(&workspace, path, "\nvar^ a : t^ = { }\n", 2);
    handed(&workspace, path, &last, &current);
    LHAT_CHECK_EQ_INT(last, 1);
    LHAT_CHECK_EQ_INT(current, 0);

    LHAT_TEST("and is once it has");
    lsp_workspace_recheck_affected(&workspace, path);
    handed(&workspace, path, &last, &current);
    LHAT_CHECK_EQ_INT(current, 1);

    // The check reads CRLF as LF, so the same text with other line endings
    // is the same text -- only an edit is a difference.
    LHAT_TEST("line endings the check normalises are no edit");
    open_text(&workspace, path, "\r\nvar^ a : t^ = { }\r\n", 3);
    handed(&workspace, path, &last, &current);
    LHAT_CHECK_EQ_INT(current, 1);

    lsp_workspace_dispose(&workspace);
    remove(path);
    remove(config);
    remove_directory(base);
}

static char *parity_load(void *context, const char *path, size_t *length)
{
    (void)path;
    const char *text = context;
    *length = strlen(text);
    char *copy = malloc(*length + 1);
    if (copy != NULL) memcpy(copy, text, *length + 1);
    return copy;
}

static void check_syntax_only_unit(void *context, const LhatUnit *unit)
{
    size_t *calls = context;
    (*calls)++;
    LHAT_CHECK(unit->loaded && unit->parsed.root != NULL, "syntax tree retained");
    LHAT_CHECK(unit->checked.types == NULL && unit->checked.diagnostic_count == 0,
               "no semantic checking for LTON");
    LHAT_CHECK(unit->proto == NULL, "no LTON bytecode in editor");
    LHAT_CHECK_EQ_INT(unit->program->types.type_count, 0);
}

static void test_lton_syntax_only_workspace(void)
{
    LHAT_TEST("large LTON roots and fresh requests allocate no semantic types");
    char base[512], path[512];
    LHAT_REQUIRE(make_temporary_directory(base, sizeof base), "temporary directory");
    join(path, sizeof path, base, "large.lton");
    FILE *file = fopen(path, "wb");
    LHAT_REQUIRE(file != NULL, "data file created");
    for (size_t i = 0; i < 2000; i++) {
        fprintf(file, "{ index = %zu, nested = { x = 1, y = 2 }, name = 'data' },\n", i);
    }
    fclose(file);
    const char *folders[] = {base};
    LspWorkspace workspace;
    lsp_workspace_init(&workspace, folders, 1);
    lsp_workspace_discover_projects(&workspace);
    lsp_workspace_recheck_all(&workspace);
    size_t calls = 0;
    lsp_workspace_with_unit(&workspace, path, check_syntax_only_unit, &calls);
    lsp_workspace_with_fresh_unit(&workspace, path, check_syntax_only_unit, &calls);
    LHAT_CHECK_EQ_INT(calls, 0);
    LHAT_CHECK_EQ_INT(root_count(project_at(&workspace, base)), 0);

    file = fopen(path, "rb");
    LHAT_REQUIRE(file != NULL, "open data text");
    fseek(file, 0, SEEK_END);
    size_t length = (size_t)ftell(file);
    rewind(file);
    char *text = malloc(length + 1);
    LHAT_REQUIRE(text != NULL, "open text buffer");
    LHAT_CHECK(fread(text, 1, length, file) == length, "read data");
    fclose(file);
    text[length] = '\0';
    lsp_document_store_put(&workspace.documents, path, text, length, 1);
    lsp_workspace_recheck_affected(&workspace, path);
    lsp_workspace_with_unit(&workspace, path, check_syntax_only_unit, &calls);
    lsp_workspace_with_fresh_unit(&workspace, path, check_syntax_only_unit, &calls);
    LHAT_CHECK_EQ_INT(calls, 2);
    LspProject *project = project_at(&workspace, base);
    LHAT_REQUIRE(project != NULL && project->roots != NULL, "LTON discovered");
    LHAT_CHECK(!lhat_program_compile(&project->roots->program), "parse-only results cannot compile");

    size_t bad_length = 0;
    char *bad = parity_load("value = { nested = },\n", path, &bad_length);
    lsp_document_store_put(&workspace.documents, path, bad, bad_length, 2);
    lsp_workspace_recheck_affected(&workspace, path);
    const LhatUnit *unit = project->roots->program.units;
    LHAT_REQUIRE(unit != NULL, "rechecked unit");
    LHAT_CHECK(unit->parsed.diagnostic_count > 0, "syntax errors retained after editing");
    LHAT_CHECK(lhat_program_has_errors(&project->roots->program), "syntax errors published");

    lsp_workspace_discover_projects(&workspace);
    lsp_workspace_recheck_all(&workspace);
    project = project_at(&workspace, base);
    LHAT_REQUIRE(project != NULL && project->roots != NULL, "open LTON survives config rediscovery");
    LHAT_CHECK(lhat_program_has_errors(&project->roots->program), "unsaved text rechecked");
    lsp_document_store_remove(&workspace.documents, path);
    lsp_workspace_recheck_affected(&workspace, path);
    LHAT_CHECK_EQ_INT(root_count(project), 0);
    calls = 0;
    lsp_workspace_with_unit(&workspace, path, check_syntax_only_unit, &calls);
    LHAT_CHECK_EQ_INT(calls, 0);
    lsp_workspace_recheck_affected(&workspace, path);
    LHAT_CHECK_EQ_INT(root_count(project), 0);
    lsp_workspace_dispose(&workspace);
    remove(path);
    remove_directory(base);
}

typedef struct {
    const LhatNode *nodes[256];
    size_t count;
} BindingTree;

static void binding_tree(void *context, const char *field, bool list,
                          const LhatNode *node)
{
    (void)field;
    (void)list;
    BindingTree *tree = context;
    if (tree->count >= sizeof tree->nodes / sizeof *tree->nodes) {
        LHAT_CHECK(false, "binding tree exceeds test capacity");
        return;
    }
    tree->nodes[tree->count++] = node;
    lhat_node_visit_children(node, binding_tree, tree);
}

static void test_execution_pipeline_parity(void)
{
    static const char *const sources[] = {
        "var^ x = 1\nvar^ f = p^ n:number^ {x := x + n}\nf(2)\nreturn^ x\n",
        "var^ t:t^{x:number^}|nil^ = nil^\nreturn^ t.x\n",
        "var^ x:number^ = 'wrong'\nreturn^ x\n",
        "let^ Base = def^{self^{x := 40}}\n"
        "let^ Derived = Base .. def^{self^{y := 2}}\n"
        "let^ d = Derived.new()\nreturn^ d.x + d.y\n",
        "return^ twice(21)\n",
        "var^ x = 42\nreturn^ $^^x\n",
        "let^ read = f^ -> number^ {return^ E.V.value}\nenum^ E {V = 42}\nreturn^ read()\n",
        "var^ root.a = 40\ndo^{var^ root.b = 2}\nreturn^ root.a + root.b\n",
        "let^ A = def^{self^{}, m = f^self^ -> number^ {return^ 40}}\n"
        "let^ D = A .. def^{self^{}, override^ m = f^self^ -> number^ {"
        "let^ read = f^ -> number^ {return^ super^()}\nreturn^ read() + 2}}\n"
        "return^ D.new().m()\n",
        "let^ Base = def^{self^{x := 0}, override^new = f^ n:number^ {self^{x = n}}}\n"
        "let^ Derived = Base .. def^{self^{y := 0}, override^new = f^ n:number^ {"
        "super^(n)\nself^{y = 2}}}\nlet^ d = Derived.new(40)\nreturn^ d.x + d.y\n",
        "let^ Base = def^{self^{}, owner = f^self^ -> any^ {return^ def^}}\n"
        "let^ Derived = Base .. def^{self^{}}\n"
        "if^ Derived.new().owner() is^ Derived {return^ 42}\nreturn^ 0\n",
        "let^ make = f^ ...:number^ -> (f^ -> number^;) {"
        "return^ f^ -> number^ {return^ (...[0] ?? 0)}}\n"
        "let^ read = make(42)\nreturn^ read()\n",
        "let^ read = f^ -> number^ {let^ n = ...[0]\n"
        "if^ n fits^ number^ {return^ n}\nreturn^ 0}\nreturn^ read()\n",
        "let^ count = f^ n:number^ -> number^ {"
        "let^ inner = f^ -> number^ {if^ n = 0 {return^ 0}\n"
        "return^ this^^(n - 1) + 1}\nreturn^ inner()}\nreturn^ count(42)\n",
        "for^ 40 to^ 40 {for^ 2 to^ 2 {"
        "let^ read = f^ {return^ it^^ + it^}\nreturn^ read()}}\n",
        "errordef^ E {A {n:number^}, B}\n"
        "let^ fail = f^ {return^ error^ E.A{n := 42}}\n"
        "return^ fail() catch^ if^ it^ fits^ E.A: it^.n el^: 0 ;\n",
        "import^ lib.draw\nreturn^ p^ {import^ lib.draw\nreturn^ lib.draw.line()}\n",
    };
    char base[512], config[512], path[512];
    LHAT_REQUIRE(make_temporary_directory(base, sizeof base), "temporary directory");
    join(config, sizeof config, base, "lhat-host.json");
    join(path, sizeof path, base, "main.lh");
    for (size_t mode = 0; mode < 2; mode++) {
        bool strict = mode == 0;
        char configuration[512];
        snprintf(configuration, sizeof configuration,
            "{\"strict\":%s,\"functions\":[{\"kind\":\"global\","
            "\"name\":\"doubleValue\",\"signature\":\"f^number^ -> number^;\"},"
            "{\"kind\":\"func\",\"module\":\"lib.draw\",\"name\":\"line\","
            "\"signature\":\"f^ -> number^;\"}],"
            "\"bindings\":[{\"name\":\"twice\",\"member\":\"L^.doubleValue\"}]}",
            strict ? "true" : "false");
        LHAT_REQUIRE(write_file(config, configuration),
                     "write configuration");
        for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
            LHAT_TEST("workspace and execution pipeline agree on diagnostics and bindings");
            LHAT_REQUIRE(write_file(path, sources[i]), "write source");
            const char *folders[] = {base};
            LspWorkspace workspace;
            lsp_workspace_init(&workspace, folders, 1);
            lsp_workspace_discover_projects(&workspace);
            lsp_workspace_recheck_affected(&workspace, path);
            LspProject *project = project_at(&workspace, base);
            LHAT_REQUIRE(project != NULL && project->roots != NULL, "workspace root");
            LhatProgram *editor = &project->roots->program;

            LhatProgram execution;
            lhat_program_init(&execution, strict, parity_load, (void *)sources[i]);
            lsp_host_config_apply(project->host_config, &execution);
            const LhatUnit *a = lhat_program_check(&execution, path);
            const LhatUnit *b = editor->units;
            LHAT_REQUIRE(a != NULL && b != NULL, "both entry points checked the source");
            LHAT_CHECK_EQ_BOOL(editor->strict, strict);
            LHAT_CHECK_EQ_BOOL(lhat_program_has_errors(&execution), lhat_program_has_errors(editor));
            LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(a), lhat_unit_diagnostic_count(b));
            LHAT_CHECK_EQ_INT(a->checked.diagnostic_count, b->checked.diagnostic_count);
            for (size_t j = 0; j < a->checked.diagnostic_count && j < b->checked.diagnostic_count; j++) {
                LHAT_CHECK_EQ_INT(a->checked.diagnostics[j].code, b->checked.diagnostics[j].code);
                LHAT_CHECK_EQ_INT(a->checked.diagnostics[j].offset, b->checked.diagnostics[j].offset);
                LHAT_CHECK_EQ_BOOL(a->checked.diagnostics[j].relaxed_ok, b->checked.diagnostics[j].relaxed_ok);
            }
            BindingTree left = {0}, right = {0};
            binding_tree(&left, NULL, false, a->parsed.root);
            binding_tree(&right, NULL, false, b->parsed.root);
            LHAT_CHECK_EQ_INT(left.count, right.count);
            for (size_t j = 0; j < left.count && j < right.count; j++) {
                const LhatNode *x = left.nodes[j]->checked_binding;
                const LhatNode *y = right.nodes[j]->checked_binding;
                const LhatNode *body_left = left.nodes[j]->checked_this_body;
                const LhatNode *body_right = right.nodes[j]->checked_this_body;
                const LhatNode *receiver_left = left.nodes[j]->checked_receiver;
                const LhatNode *receiver_right = right.nodes[j]->checked_receiver;
                LHAT_CHECK_EQ_BOOL(receiver_left != NULL, receiver_right != NULL);
                if (receiver_left != NULL && receiver_right != NULL) {
                    LHAT_CHECK_EQ_INT(receiver_left->offset, receiver_right->offset);
                }
                LHAT_CHECK_EQ_BOOL(body_left != NULL, body_right != NULL);
                if (body_left != NULL && body_right != NULL) {
                    LHAT_CHECK_EQ_INT(body_left->offset, body_right->offset);
                }
                LHAT_CHECK_EQ_BOOL(left.nodes[j]->checked_definition != NULL,
                                   right.nodes[j]->checked_definition != NULL);
                LHAT_CHECK_EQ_BOOL(left.nodes[j]->checked_import_global,
                                   right.nodes[j]->checked_import_global);
                LHAT_CHECK_EQ_BOOL(left.nodes[j]->checked_super_call,
                                   right.nodes[j]->checked_super_call);
                LHAT_CHECK_EQ_BOOL(left.nodes[j]->checked_scope_invalid,
                                   right.nodes[j]->checked_scope_invalid);
                const LhatModuleRoot *module_left = left.nodes[j]->checked_module_root;
                const LhatModuleRoot *module_right = right.nodes[j]->checked_module_root;
                LHAT_CHECK_EQ_BOOL(module_left != NULL, module_right != NULL);
                if (module_left != NULL && module_right != NULL) {
                    LHAT_CHECK_EQ_INT(module_left->declaration->offset, module_right->declaration->offset);
                    LHAT_CHECK_EQ_INT(module_left->length, module_right->length);
                    LHAT_CHECK(module_left->length == module_right->length &&
                               memcmp(module_left->name, module_right->name, module_left->length) == 0,
                               "both pipelines resolve the same module root");
                }
                const LhatTypeMember *host_left = left.nodes[j]->checked_host_member;
                const LhatTypeMember *host_right = right.nodes[j]->checked_host_member;
                LHAT_CHECK_EQ_BOOL(host_left != NULL, host_right != NULL);
                if (host_left != NULL && host_right != NULL) {
                    LHAT_CHECK_EQ_INT(host_left->name_length, host_right->name_length);
                    LHAT_CHECK(host_left->name_length == host_right->name_length &&
                               memcmp(host_left->name, host_right->name, host_left->name_length) == 0,
                               "both pipelines resolve the same host member");
                }
                LHAT_CHECK_EQ_BOOL(x != NULL, y != NULL);
                if (x != NULL && y != NULL) LHAT_CHECK_EQ_INT(x->offset, y->offset);
            }
            // CLI execution gates code generation on semantic errors, just
            // as the workspace does; the low-level emitter itself does not.
            bool compiled = !lhat_program_has_errors(&execution) &&
                            lhat_program_compile(&execution);
            LHAT_CHECK_EQ_BOOL(compiled, !lhat_program_has_errors(editor));
            LHAT_CHECK_EQ_BOOL(a->proto != NULL, b->proto != NULL);
            lhat_program_dispose(&execution);
            lsp_workspace_dispose(&workspace);
        }
    }
    remove(path);
    remove(config);
    remove_directory(base);
}

int main(void)
{
    test_lton_syntax_only_workspace();
    test_nearest_project_wins_inside_each_workspace_folder();
    test_current_unit();
    test_execution_pipeline_parity();
    return lhat_test_report("test_workspace_projects");
}
