// An application owns the loop; L^ supplies its update callback.
#include <stdio.h>
#include <string.h>

#include "lhat.h"

typedef struct {
    int updates;
    double elapsed;
} App;

static void report(LhatMachine *machine, void *context,
                   const LhatValue *arguments, size_t count,
                   LhatValue *answers, int *answer_count)
{
    (void)answers;
    (void)answer_count;
    if (count != 1 || !lhat_is_number(arguments[0])) {
        lhat_machine_panic_text(machine, "app.report expects one number");
        return;
    }
    App *app = (App *)context;
    app->elapsed = lhat_is_integer(arguments[0])
        ? (double)lhat_as_integer(arguments[0])
        : (double)lhat_as_real(arguments[0]);
    ++app->updates;
    printf("update %d: %.6f\n", app->updates, app->elapsed);
}

static void diagnostics(const LhatProgram *program)
{
    for (size_t i = 0; i < lhat_program_diagnostic_count(program); ++i) {
        const LhatProgramDiagnostic *d = lhat_program_diagnostic(program, i);
        fprintf(stderr, "%s: %s\n", d->path, lhat_program_error_message(d->code));
    }
    for (const LhatUnit *u = lhat_program_units(program); u; u = lhat_unit_next(u)) {
        for (size_t i = 0; i < lhat_unit_diagnostic_count(u); ++i) {
            size_t size = lhat_unit_diagnostic_write(u, i, true, NULL, 0);
            char *text = (char *)lhat_alloc(size + 1);
            if (text) {
                lhat_unit_diagnostic_write(u, i, true, text, size + 1);
                fprintf(stderr, "%s\n", text);
                lhat_free(text);
            }
        }
    }
}

static bool run_ok(LhatMachine *machine, LhatRunResult result, const char *path)
{
    if (result.status == LHAT_RUN_OK) return true;
    size_t size = lhat_run_error_write(machine, result, path, NULL, 0);
    char *text = (char *)lhat_alloc(size + 1);
    if (text) {
        lhat_run_error_write(machine, result, path, text, size + 1);
        fprintf(stderr, "%s\n", text);
        lhat_free(text);
    } else {
        fprintf(stderr, "run failed: %s\n", lhat_run_status_message(result.status));
    }
    return false;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main.lh";
    App app = {0};
    int status = 1;
    LhatMachine *machine = NULL;
    LhatProgram *program = lhat_program_new(true, lhat_load_file, NULL);
    if (!program || !lhat_register_func(program, "app", "report", "p^number^;",
                                        report, &app)) {
        fprintf(stderr, "could not register app.report\n");
        goto done;
    }

    // Use the very same registrations for the language server and runtime.
    if (strcmp(path, "--dump-host-api") == 0) {
        size_t size = lhat_program_dump_host_api(program, NULL, 0);
        char *json = (char *)lhat_alloc(size + 1);
        if (!json) goto done;
        lhat_program_dump_host_api(program, json, size + 1);
        status = fwrite(json, 1, size, stdout) == size ? 0 : 1;
        lhat_free(json);
        goto done;
    }

    const LhatUnit *unit = lhat_program_check(program, path);
    if (!unit || lhat_program_has_errors(program) || !lhat_unit_ok(unit)) {
        diagnostics(program);
        goto done;
    }
    // Reject a wrong lifecycle signature before starting the application.
    if (!lhat_unit_export_conforms(unit, "update", "p^number^;")) {
        fprintf(stderr, "%s: publish update : p^number^;\n", path);
        goto done;
    }
    if (!lhat_program_compile(program)) {
        LhatCompileResult failure = lhat_program_compile_failure(program, NULL);
        char text[1024];
        lhat_compile_message_write(program, &failure, text, sizeof text);
        fprintf(stderr, "%s\n", text);
        goto done;
    }
    machine = lhat_machine_new();
    if (!machine || !lhat_program_install(program, machine)) {
        fprintf(stderr, "could not create or install the machine\n");
        goto done;
    }
    LhatRunResult ran = lhat_run(machine, lhat_unit_proto(unit));
    if (!run_ok(machine, ran, path)) goto done;

    // A C local is not a GC root. Keep the exports reachable between frames.
    LhatTable *roots = lhat_machine_host_root(machine);
    bool refused = false;
    if (!roots || !lhat_machine_table_set(machine, roots, lhat_integer(0),
                                         ran.value, &refused) || refused) {
        fprintf(stderr, "could not root the callbacks\n");
        goto done;
    }
    LhatValue callbacks = ran.value;
    for (int frame = 0; frame < 3; ++frame) {
        // Collect deliberately: callbacks must survive across host frames.
        lhat_machine_gc_collect(machine);
        LhatValue dt = lhat_real(1.0 / 60.0);
        ran = lhat_machine_call_member(machine, callbacks, "update", 6, &dt, 1);
        if (!run_ok(machine, ran, path)) goto done;
    }
    status = app.updates == 3 && app.elapsed > 0.049 && app.elapsed < 0.051 ? 0 : 1;

done:
    if (machine) lhat_machine_dispose(machine);
    if (program) lhat_program_free(program);
    // This standalone application owns every program in the process.
    lhat_registry_dispose();
    return status;
}
