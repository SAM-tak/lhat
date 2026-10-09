// A second, SDL-free embedding host for the public native-extension helpers.
#include <lhat/extension.h>
#include "testutil.h"

static int phase_order, disposed, opened, closed;
static int close_order[8], shutdown_order[8], shutdown_count;
static void shutdown_a(void) { LHAT_CHECK_EQ_INT(disposed, 2); shutdown_order[shutdown_count++] = 1; }
static void shutdown_b(void) { LHAT_CHECK_EQ_INT(disposed, 2); shutdown_order[shutdown_count++] = 2; }

static void cleanup(void *context)
{
    disposed++;
    lhat_free(context);
}

static void answer(LhatMachine *machine, void *context, const LhatValue *args,
                   size_t count, LhatValue *out, int *answers)
{
    (void) machine; (void) context; (void) args; (void) count;
    out[0] = lhat_integer(42);
    *answers = 1;
}

static const char *install_a(const LhatExtensionAPI *api, LhatProgram *program,
                              uint32_t phase, void **state)
{
    if (phase == LHAT_EXTENSION_TYPES) {
        if (phase_order++ != 0) return "wrong TYPES order";
        *state = api->lhat_alloc(1);
        if (*state == NULL) return "allocation failed";
        if (!api->lhat_program_on_dispose(program, cleanup, *state)) {
            api->lhat_free(*state);
            return "cleanup registration failed";
        }
        if (!api->lhat_register_type(program, "nativea", "Marker")) return "type failed";
    } else {
        if (phase_order++ != 2 || *state == NULL) return "wrong MEMBERS order or lost state";
        if (!api->lhat_register_func(program, "nativea", "answer", "f^ -> number^;", answer, NULL)
            || !api->lhat_register_func(program, "nativea", "accept", "f^nativeb.Marker -> number^;", answer, NULL))
            return "function registration failed";
    }
    return NULL;
}

static const char *install_b(const LhatExtensionAPI *api, LhatProgram *program,
                              uint32_t phase, void **state)
{
    (void) state;
    if (phase_order++ != (phase == LHAT_EXTENSION_TYPES ? 1 : 3)) return "wrong B order";
    if (phase == LHAT_EXTENSION_TYPES && !api->lhat_register_type(program, "nativeb", "Marker"))
        return "B type failed";
    return NULL;
}

static const LhatExtension a = {
    LHAT_EXTENSION_ABI, sizeof(LhatExtension), LHAT_VERSION, sizeof(LhatValue),
    "A", NULL, 0, install_a, shutdown_a
};
static const LhatExtension b = {
    LHAT_EXTENSION_ABI, sizeof(LhatExtension), LHAT_VERSION, sizeof(LhatValue),
    "B", NULL, 0, install_b, shutdown_b
};
static const LhatExtension *entry_a(void) { return &a; }
static const LhatExtension *entry_b(void) { return &b; }

static void *open_module(void *context, const char *path)
{
    (void) context;
    opened++;
    if (strcmp(path, "absent") == 0) return NULL;
    return strcmp(path, "b") == 0 ? (void *)&b : (void *)&a;
}
static LhatExtensionSymbol symbol(void *context, void *library, const char *name)
{
    LHAT_CHECK(strcmp(name, "lhat_extension_v2") == 0, "portable entry name");
    if (context != NULL) return NULL;
    return (LhatExtensionSymbol)(library == &a ? entry_a : entry_b);
}
static void close_module(void *context, void *library)
{
    (void) context;
    if (context == NULL) LHAT_CHECK_EQ_INT(shutdown_count, closed + 1);
    close_order[closed++] = library == &a ? 1 : 2;
}
static const char *loader_error(void *context) { (void)context; return "test loader refused"; }

static char *load_text(void *context, const char *path, size_t *length)
{
    (void) context; (void) path;
    const char *text = "module^app\nimport^nativea\npublic^let^result = nativea.answer()\n";
    *length = strlen(text);
    char *copy = lhat_alloc(*length + 1);
    if (copy != NULL) memcpy(copy, text, *length + 1);
    return copy;
}

static void exercise_programs(LhatExtensions *pool, const LhatExtensionModule **modules)
{
    LhatProgram *programs[2] = {NULL, NULL};
    for (int i = 0; i < 2; i++) {
        programs[i] = lhat_program_new(true, load_text, NULL);
        LHAT_REQUIRE(programs[i] != NULL, "program allocated");
        phase_order = 0;
        LHAT_REQUIRE(lhat_extensions_register(pool, programs[i], modules, 2), "%s", lhat_extensions_error(pool));
        LHAT_CHECK_EQ_INT(phase_order, 4);
        const LhatUnit *unit = lhat_program_check(programs[i], "main.lh");
        LHAT_REQUIRE(unit != NULL && lhat_program_compile(programs[i]), "plain host compiled the extension call");
        LhatMachine *machine = lhat_machine_new();
        LHAT_REQUIRE(machine != NULL && lhat_program_install(programs[i], machine), "host installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(unit));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LhatValue value = lhat_table_get_bytes((LhatTable *)lhat_as_object(ran.value), "result", 6);
        LHAT_CHECK_EQ_INT(lhat_number_as_real(value), 42);
        lhat_machine_dispose(machine);
    }
    LHAT_CHECK_EQ_INT(disposed, 0);
    lhat_program_free(programs[1]);
    lhat_program_free(programs[0]);
    LHAT_CHECK_EQ_INT(disposed, 2);
}

static void test_static(void)
{
    LHAT_TEST("static descriptors, independent programs and phase ordering");
    disposed = shutdown_count = 0;
    LhatExtensions *pool = lhat_extensions_new(NULL);
    LHAT_REQUIRE(pool != NULL, "pool");
    const LhatExtensionModule *modules[] = {
        lhat_extensions_add(pool, "a", &a), lhat_extensions_add(pool, "b", &b)
    };
    LHAT_REQUIRE(modules[0] && modules[1], "descriptors");
    LHAT_CHECK_EQ_PTR(lhat_extensions_add(pool, "a", &a), modules[0]);
    LHAT_CHECK(lhat_extensions_add(pool, "a", &b) == NULL, "key collision refused");
    LHAT_CHECK(lhat_extensions_load(pool, "absent") == NULL, "static-only pool does not load files");
    LhatExtension invalid = a;
    invalid.abi_version++;
    LHAT_CHECK(lhat_extensions_add(pool, "invalid", &invalid) == NULL, "ABI mismatch refused");
    invalid = a;
    invalid.lhat_version = "other";
    LHAT_CHECK(lhat_extensions_add(pool, "invalid", &invalid) == NULL, "version mismatch refused");
    exercise_programs(pool, modules);
    lhat_registry_dispose();
    lhat_extensions_free(pool);
    LHAT_CHECK_EQ_INT(shutdown_count, 2);
    LHAT_CHECK_EQ_INT(shutdown_order[0], 2);
    LHAT_CHECK_EQ_INT(shutdown_order[1], 1);
}

static void test_loader(void)
{
    LHAT_TEST("host loader, cache and reverse close order");
    disposed = opened = closed = shutdown_count = 0;
    LhatExtensionLoader loader = {NULL, open_module, symbol, close_module, loader_error};
    LhatExtensions *pool = lhat_extensions_new(&loader);
    LHAT_REQUIRE(pool != NULL, "pool");
    const LhatExtensionModule *modules[] = {
        lhat_extensions_load(pool, "a"), lhat_extensions_load(pool, "b")
    };
    LHAT_REQUIRE(modules[0] && modules[1], "loaded");
    LHAT_CHECK_EQ_PTR(lhat_extensions_load(pool, "a"), modules[0]);
    LHAT_CHECK_EQ_INT(opened, 2);
    LHAT_CHECK(lhat_extensions_load(pool, "absent") == NULL, "missing file");
    LHAT_CHECK(strstr(lhat_extensions_error(pool), "test loader refused") != NULL, "loader diagnostic retained");
    exercise_programs(pool, modules);
    LHAT_CHECK_EQ_INT(closed, 0);
    LHAT_CHECK_EQ_INT(shutdown_count, 0);
    lhat_registry_dispose();
    lhat_extensions_free(pool);
    LHAT_CHECK_EQ_INT(closed, 2);
    LHAT_CHECK_EQ_INT(close_order[0], 2);
    LHAT_CHECK_EQ_INT(close_order[1], 1);

    loader.context = &loader; // Refuse symbol lookup after a successful open.
    pool = lhat_extensions_new(&loader);
    LHAT_REQUIRE(pool != NULL, "second pool");
    LHAT_CHECK(lhat_extensions_load(pool, "a") == NULL, "missing entry refused");
    LHAT_CHECK(strstr(lhat_extensions_error(pool), "missing lhat_extension_v2") != NULL, "entry diagnostic");
    LHAT_CHECK_EQ_INT(closed, 3);
    lhat_extensions_free(pool);
    LHAT_CHECK_EQ_INT(closed, 3);
}

int main(void)
{
    test_static();
    test_loader();
    return lhat_test_report("test_extensions");
}
