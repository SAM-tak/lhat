// L^ (lhat) -- the smallest host: proof that an installed tree compiles,
// links and runs from include/lhat.h alone. Registers one function, runs
// one program, checks one answer.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

static LhatValue host_twice(struct LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    if (count != 1 || !lhat_is_integer(arguments[0])) {
        return lhat_nil();
    }
    return lhat_integer(lhat_as_integer(arguments[0]) * 2);
}

static char *load_main(void *context, const char *path, size_t *length)
{
    (void)context;
    static const char text[] = "return^ twice(21)\n";
    if (strcmp(path, "main.lh") != 0) {
        return NULL;
    }
    char *copy = (char *)malloc(sizeof text);
    if (copy != NULL) {
        memcpy(copy, text, sizeof text);
        *length = sizeof text - 1;
    }
    return copy;
}

int main(void)
{
    LhatProgram *program = lhat_program_new(true, load_main, NULL);
    if (program == NULL ||
        !lhat_register_global(program, "twice", "f^number^ -> number^;",
                              host_twice, NULL) ||
        !lhat_bind_initial(program, "twice", "L^.twice")) {
        fprintf(stderr, "registration failed\n");
        return 1;
    }

    const LhatUnit *root = lhat_program_check(program, "main.lh");
    if (root == NULL || lhat_program_has_errors(program) ||
        !lhat_unit_ok(root)) {
        fprintf(stderr, "check failed\n");
        return 1;
    }

    size_t count = 0;
    const LhatModule *modules = lhat_program_compile(program, &count);
    LhatMachine *machine = modules != NULL ? lhat_machine_new() : NULL;
    if (machine == NULL) {
        fprintf(stderr, "compile failed\n");
        return 1;
    }
    lhat_machine_set_modules(machine, modules, count);
    lhat_program_install(program, machine);
    LhatRunResult ran = lhat_run(machine, modules[lhat_unit_index(root)].proto);

    int status = 1;
    if (ran.status == LHAT_RUN_OK && lhat_is_integer(ran.value) &&
        lhat_as_integer(ran.value) == 42) {
        printf("installed lhat answers 42\n");
        status = 0;
    } else {
        fprintf(stderr, "run answered the wrong thing (status %d)\n",
                (int)ran.status);
    }
    lhat_machine_dispose(machine);
    lhat_program_free(program);
    return status;
}
