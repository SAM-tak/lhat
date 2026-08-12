// L^ (lhat) -- tests for the unit graph.
//
// Section numbers refer to DesignDocuments/05-modules.md. The loader is
// replaced with a table held here, so what is pinned is the graph — the
// dependency order of 6.2, the single load of 5.3 and the refusal of 6.3 —
// rather than anything about a file system.

#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"
#include "program_internal.h"
#include "testutil.h"
#include "lhat/value.h"
#include "lhat/vm.h"

typedef struct {
    const char *path;
    const char *text;
} File;

typedef struct {
    const File *files;
    size_t count;
    size_t reads;  // 5.3: a unit is read once however often it is required
} Disk;

static char *disk_load(void *context, const char *path, size_t *length)
{
    Disk *disk = (Disk *)context;
    for (size_t i = 0; i < disk->count; i++) {
        if (strcmp(disk->files[i].path, path) != 0) {
            continue;
        }
        disk->reads++;
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

static void program_with(LhatProgram *program, Disk *disk, const File *files,
                         size_t count)
{
    disk->files = files;
    disk->count = count;
    disk->reads = 0;
    lhat_program_init(program, true, disk_load, disk);
}

static bool has_program_error(const LhatProgram *program,
                              LhatProgramErrorCode code)
{
    for (size_t i = 0; i < program->diagnostic_count; i++) {
        if (program->diagnostics[i].code == code) {
            return true;
        }
    }
    return false;
}

static size_t unit_count(const LhatProgram *program)
{
    size_t n = 0;
    for (const LhatUnit *u = program->units; u != NULL; u = u->next) {
        n++;
    }
    return n;
}

static void test_dependencies(void)
{
    LhatProgram program;
    Disk disk;

    // 6.2: a unit is checked only after the units it requires, which is what
    // lets the type of an imported name be known at all.
    static const File simple[] = {
        {"lib/geometry.lh",
         "module^ ns.geometry\n"
         "public^ let^ dist = f^ a:number^, b:number^ -> number^ { return^ a }\n"
         "var^ secret = 1\n"},
        {"main.lh",
         "var^ g = require^ \"lib/geometry.lh\"\n"
         "var^ d : number^ = g.dist(1, 2)\n"},
    };

    LHAT_TEST("a required unit is checked first");
    program_with(&program, &disk, simple, 2);
    {
        const LhatUnit *main_unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(main_unit != NULL, "the unit loaded");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors");
        LHAT_CHECK_EQ_INT(unit_count(&program), 2);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("an error in a required unit's use is reported");
    {
        static const File bad[] = {
            {"lib/geometry.lh",
             "public^ let^ dist = f^ a:number^, b:number^ -> number^ { return^ a }\n"},
            {"main.lh",
             "var^ g = require^ \"lib/geometry.lh\"\n"
             "var^ d = g.dist(1, \"text\")\n"},
        };
        program_with(&program, &disk, bad, 2);
        const LhatUnit *main_unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(main_unit != NULL, "the unit loaded");
        LHAT_CHECK(main_unit != NULL && main_unit->checked.diagnostic_count > 0,
                   "reported");
    }
    lhat_program_dispose(&program);

    // 5.1: the path is relative to the unit that wrote it.
    LHAT_TEST("a require^ is relative to its own unit");
    {
        static const File nested[] = {
            {"lib/inner.lh", "public^ let^ v = 1\n"},
            {"lib/outer.lh",
             "var^ i = require^ \"inner.lh\"\n"
             "public^ let^ w : number^ = i.v\n"},
            {"main.lh",
             "var^ o = require^ \"lib/outer.lh\"\n"
             "var^ n : number^ = o.w\n"},
        };
        program_with(&program, &disk, nested, 3);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors");
        LHAT_CHECK_EQ_INT(unit_count(&program), 3);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("'..' walks back out");
    {
        static const File updown[] = {
            {"shared.lh", "public^ let^ v = 1\n"},
            {"lib/user.lh",
             "var^ s = require^ \"../shared.lh\"\n"
             "public^ let^ w : number^ = s.v\n"},
            {"main.lh", "var^ u = require^ \"lib/user.lh\"\n"},
        };
        program_with(&program, &disk, updown, 3);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors");
    }
    lhat_program_dispose(&program);
}

static void test_loading(void)
{
    LhatProgram program;
    Disk disk;

    // 5.3: required twice, read once, and the same unit both times.
    LHAT_TEST("a unit is loaded once");
    {
        static const File shared[] = {
            {"lib/common.lh", "public^ let^ v = 1\n"},
            {"lib/a.lh",
             "var^ c = require^ \"common.lh\"\n"
             "public^ let^ x : number^ = c.v\n"},
            {"lib/b.lh",
             "var^ c = require^ \"common.lh\"\n"
             "public^ let^ y : number^ = c.v\n"},
            {"main.lh",
             "var^ a = require^ \"lib/a.lh\"\n"
             "var^ b = require^ \"lib/b.lh\"\n"
             "var^ n : number^ = a.x + b.y\n"},
        };
        program_with(&program, &disk, shared, 4);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors");
        LHAT_CHECK_EQ_INT(unit_count(&program), 4);
        LHAT_CHECK_EQ_INT(disk.reads, 4);  // not five: common.lh read once
    }
    lhat_program_dispose(&program);

    // Two spellings of one path are one unit, which is what keys 5.3.
    LHAT_TEST("a path is normalised before it is looked up");
    {
        static const File aliased[] = {
            {"lib/common.lh", "public^ let^ v = 1\n"},
            {"main.lh",
             "var^ a = require^ \"lib/common.lh\"\n"
             "var^ b = require^ \"./lib/../lib/common.lh\"\n"},
        };
        program_with(&program, &disk, aliased, 2);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors");
        LHAT_CHECK_EQ_INT(unit_count(&program), 2);
        LHAT_CHECK_EQ_INT(disk.reads, 2);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a unit that is not there is reported");
    {
        static const File missing[] = {
            {"main.lh", "var^ g = require^ \"nowhere.lh\"\n"},
        };
        program_with(&program, &disk, missing, 1);
        const LhatUnit *main_unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(main_unit != NULL, "the main unit loaded");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CANNOT_READ),
                   "the missing unit is named");
        LHAT_CHECK(main_unit != NULL && main_unit->checked.diagnostic_count > 0,
                   "and the require^ is marked");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a program whose root is missing yields nothing");
    {
        static const File empty[] = {{"other.lh", ""}};
        program_with(&program, &disk, empty, 1);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL, "no unit");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CANNOT_READ),
                   "reported");
    }
    lhat_program_dispose(&program);
}

// 6.3: refused, because allowing it makes what is visible depend on which
// side was read first.
static void test_cycles(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("two units requiring each other is an error");
    {
        static const File cycle[] = {
            {"a.lh",
             "var^ b = require^ \"b.lh\"\n"
             "public^ let^ x = 1\n"},
            {"b.lh",
             "var^ a = require^ \"a.lh\"\n"
             "public^ let^ y = 1\n"},
        };
        program_with(&program, &disk, cycle, 2);
        lhat_program_check(&program, "a.lh");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CYCLE),
                   "the cycle is named");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a longer cycle is caught too");
    {
        static const File cycle[] = {
            {"a.lh", "var^ b = require^ \"b.lh\"\n"},
            {"b.lh", "var^ c = require^ \"c.lh\"\n"},
            {"c.lh", "var^ a = require^ \"a.lh\"\n"},
        };
        program_with(&program, &disk, cycle, 3);
        lhat_program_check(&program, "a.lh");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CYCLE),
                   "the cycle is named");
    }
    lhat_program_dispose(&program);

    // A unit reached twice by different routes is not a cycle, since the
    // first visit finished before the second began.
    LHAT_TEST("a diamond is not a cycle");
    {
        static const File diamond[] = {
            {"common.lh", "public^ let^ v = 1\n"},
            {"a.lh",
             "var^ c = require^ \"common.lh\"\n"
             "public^ let^ x : number^ = c.v\n"},
            {"b.lh",
             "var^ c = require^ \"common.lh\"\n"
             "public^ let^ y : number^ = c.v\n"},
            {"main.lh",
             "var^ a = require^ \"a.lh\"\n"
             "var^ b = require^ \"b.lh\"\n"},
        };
        program_with(&program, &disk, diamond, 4);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!has_program_error(&program, LHAT_PROGRAM_ERR_CYCLE),
                   "no cycle");
        LHAT_CHECK(!lhat_program_has_errors(&program), "no errors at all");
    }
    lhat_program_dispose(&program);
}

// 05 の 5.3: the units compile too, and the machine is given the lot so a
// require^ inside one can reach another. What is pinned here is that the
// unit runs once however many times it is required, and that both requirers
// see the very thing it made.
static void test_running(void)
{
    LhatProgram program;
    Disk disk;

    static const File files[] = {
        {"side.lh",
         "module^ ns.side\n"
         "public^ let^ marks = { n := 0 }\n"
         "marks.n := marks.n + 1\n"},
        {"mid.lh",
         "module^ ns.mid\n"
         "require^ \"side.lh\"\n"
         "public^ let^ seen = ns.side.marks\n"},
        {"main.lh",
         "require^ \"side.lh\"\n"
         "require^ \"mid.lh\"\n"
         "return^ ns.side.marks.n * 10 + ns.mid.seen.n\n"},
    };

    LHAT_TEST("a required unit runs once and both requirers see the one it made");
    {
        program_with(&program, &disk, files, 3);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        LHAT_CHECK(modules != NULL, "every unit compiled");
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 1 for the side effect, and the same table read through both.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 11);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.6: what require^ answers is sealed once the unit has
    // built it. check.c refuses a write named against it, but a t^{ … }
    // parameter carries no mark of this -- so the machine asks again where
    // the write happens.
    LHAT_TEST("a module table refuses a write that reached it as an argument");
    {
        static const File reading[] = {
            {"one.lh",
             "module^ ns.one\n"
             "public^ let^ v = 7\n"},
            {"main.lh",
             "var^ poke = p^ x:t^{ v:number^ } { x.v := 99 }\n"
             "var^ m = require^ \"one.lh\"\n"
             "poke(m)\n"
             "return^ m.v\n"},
        };
        program_with(&program, &disk, reading, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_SEALED);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 8.6: the registry is where the guard looks, so what ran is in it.
    LHAT_TEST("and L^.modules holds what was registered");
    {
        static const File reading[] = {
            {"one.lh",
             "module^ ns.one\n"
             "public^ let^ v = 7\n"},
            {"main.lh",
             "require^ \"one.lh\"\n"
             "return^ L^.modules.ns.one.v\n"},
        };
        program_with(&program, &disk, reading, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // A machine that was never given the units cannot answer a require^.
    LHAT_TEST("and a machine without the units refuses");
    {
        static const File pair[] = {
            {"one.lh", "module^ ns.one\npublic^ let^ v = 1\n"},
            {"main.lh", "require^ \"one.lh\"\nreturn^ ns.one.v\n"},
        };
        program_with(&program, &disk, pair, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_NO_SUCH_UNIT);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);
}

// 05 の 8.7: what the host registers, and how import^ reaches it.
// 05 の 8.6: one that goes into L^ itself rather than under its registry,
// so that 8.2's initial binding has something to name.
static LhatValue host_twice(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    int *calls = (int *)context;
    if (calls != NULL) {
        (*calls)++;
    }
    if (count != 1 || !lhat_is_integer(arguments[0])) {
        return lhat_nil();
    }
    return lhat_integer(lhat_as_integer(arguments[0]) * 2);
}

static LhatValue host_add(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    (void)machine;
    int *calls = (int *)context;
    if (calls != NULL) {
        (*calls)++;
    }
    if (count != 2 || !lhat_is_integer(arguments[0]) ||
        !lhat_is_integer(arguments[1])) {
        return lhat_nil();
    }
    return lhat_integer(lhat_as_integer(arguments[0]) +
                        lhat_as_integer(arguments[1]));
}

// 02 の 11.8改 with 05 の 8.9: a host value carrying the unary '-'. The tag
// is handed over through the context, since a host function is given nothing
// else to read its own type by.
typedef struct {
    int64_t n;
} Counter;

static LhatValue host_counter_make(LhatMachine *machine, void *context,
                                   const LhatValue *arguments, size_t count)
{
    (void)arguments;
    if (count != 0) {
        return lhat_nil();
    }
    Counter c = {7};
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, (const LhatHostValueTag *)context, &c,
                               &out)
               ? out
               : lhat_nil();
}

// f^self^ -> number^; -- one operand and no argument, which is the whole of
// what tells a unary operator from a binary one.
static LhatValue host_counter_negate(LhatMachine *machine, void *context,
                                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count != 1) {
        return lhat_nil();
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[0], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return lhat_nil();
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    return lhat_integer(-c.n);
}

// f^self^, o:number^ -> number^; -- the binary arm standing beside the unary
// one above, told apart by what each takes.
static LhatValue host_counter_minus(LhatMachine *machine, void *context,
                                    const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count != 2 || !lhat_is_integer(arguments[1])) {
        return lhat_nil();
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[0], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return lhat_nil();
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    return lhat_integer(c.n - lhat_as_integer(arguments[1]));
}

// f^lhs:number^, self^ -> number^; -- 02 の 11.3改's trailing self^, so the
// receiver is the operand written on the RIGHT and 'n + v' finds it.
static LhatValue host_counter_radd(LhatMachine *machine, void *context,
                                   const LhatValue *arguments, size_t count)
{
    (void)machine;
    if (count != 2 || !lhat_is_integer(arguments[0])) {
        return lhat_nil();
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[1], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return lhat_nil();
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    return lhat_integer(lhat_as_integer(arguments[0]) + c.n);
}

// f^self^, o:string^ -> number^; -- the same count as the binary '-' arm,
// told apart by the type alone. What the counts could not settle.
static LhatValue host_counter_tagged(LhatMachine *machine, void *context,
                                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    return count == 2 ? lhat_integer(99) : lhat_nil();
}

static bool has_check_error(const LhatUnit *unit, LhatCheckErrorCode code)
{
    if (unit == NULL) {
        return false;
    }
    for (size_t i = 0; i < unit->checked.diagnostic_count; i++) {
        if (unit->checked.diagnostics[i].code == code) {
            return true;
        }
    }
    return false;
}

static void test_hosting(void)
{
    LhatProgram program;
    Disk disk;
    int calls = 0;

    LHAT_TEST("a host subroutine is registered, imported and called");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.gfx\n"
             "return^ system.gfx.add(2, 3)\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "system.gfx", "add",
                                      "f^number^, number^ -> number^;",
                                      host_add, &calls),
                   "the registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LHAT_CHECK(lhat_program_install(&program, machine),
                       "what was registered went into L^.modules");
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
            LHAT_CHECK_EQ_INT(calls, 1);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.2: a host may bind a name so that a program writes it with no
    // qualification. 8.1 is unchanged -- the language hands out nothing, and
    // a host that binds none leaves a program seeing nothing.
    LHAT_TEST("a host-bound name is written without any qualification");
    {
        static const File files[] = {
            {"main.lh", "return^ twice(21)\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_global(&program, "twice",
                                        "f^number^ -> number^;", host_twice,
                                        &calls),
                   "the registration took");
        LHAT_CHECK(lhat_bind_initial(&program, "twice", "L^.twice"),
                   "the binding took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LHAT_CHECK(lhat_program_install(&program, machine),
                       "what was registered reached L^");
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // What it reaches stays readable as L^.<member>, since 8.1 keeps the hat
    // identifier out of the spellings a var^ can make.
    LHAT_TEST("a var^ of the same spelling shadows it, and L^ still reaches it");
    {
        static const File files[] = {
            {"main.lh",
             "var^ twice = f^ n:number^ -> number^ { return^ L^.twice(n) + 1 }\n"
             "return^ twice(20)\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_global(&program, "twice", "f^number^ -> number^;",
                             host_twice, NULL);
        lhat_bind_initial(&program, "twice", "L^.twice");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 41);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 8.1: nothing is handed out on its own. A name the host did not bind is
    // missing exactly as it was before.
    LHAT_TEST("and a name nobody bound is still missing");
    {
        static const File files[] = {
            {"main.lh", "return^ twice(21)\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL, "the unit loaded");
        LHAT_CHECK(root == NULL || root->checked.diagnostic_count > 0,
                   "an unbound name is reported");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and the expression form binds it under a name of its own");
    {
        static const File files[] = {
            {"main.lh",
             "var^ g = import^ system.gfx\n"
             "return^ g.add(20, 22)\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_func(&program, "system.gfx", "add",
                           "f^number^, number^ -> number^;", host_add, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // The signature is what the checker reads, so a call that does not fit it
    // is refused before anything runs.
    LHAT_TEST("and the signature is checked at the call");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.gfx\n"
             "return^ system.gfx.add(\"two\", 3)\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_func(&program, "system.gfx", "add",
                           "f^number^, number^ -> number^;", host_add, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_MISMATCH),
                   "a string where number^ was written is refused");
    }
    lhat_program_dispose(&program);

    // 8.7: import^ reaches the host registry and nothing else. A unit read
    // from a file comes in with require^, whatever L^.modules holds -- which
    // is what keeps the answer free of the order units are checked in.
    LHAT_TEST("but import^ does not reach a unit read from a file");
    {
        static const File files[] = {
            {"one.lh", "module^ ns.one\npublic^ let^ v = 1\n"},
            {"main.lh",
             "require^ \"one.lh\"\n"
             "import^ ns.one\n"},
        };
        program_with(&program, &disk, files, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_NOT_HOSTED),
                   "even after a require^ put it in L^.modules");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("nor one the host never registered");
    {
        static const File files[] = {{"main.lh", "import^ nowhere.at.all\n"}};
        program_with(&program, &disk, files, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_NOT_HOSTED),
                   "there is nothing of that name");
    }
    lhat_program_dispose(&program);

    // 8.7: a signature may name a type registered before it, which is what
    // lets a pair that name each other be registered as two bare types first.
    LHAT_TEST("a signature may name a type registered earlier");
    {
        program_with(&program, &disk, NULL, 0);
        LHAT_CHECK(lhat_register_type(&program, "system.gfx", "Texture"),
                   "the type took");
        LHAT_CHECK(lhat_register_func(&program, "system.gfx", "load",
                                      "f^string^ -> system.gfx.Texture;",
                                      host_add, NULL),
                   "and a signature naming it took");
        LHAT_CHECK(!lhat_register_func(&program, "system.gfx", "later",
                                       "f^ -> system.gfx.Missing;", host_add,
                                       NULL),
                   "but one naming what is not there does not");
        LHAT_CHECK(!lhat_register_type(&program, "system.gfx", "Texture"),
                   "and one name holds one thing");
    }
    lhat_program_dispose(&program);
}

// 05 の 8.8: something the host made, reached through a pointer L^ holds and
// never looks into.
typedef struct {
    int value;
    int live;  // shared, so the test can see a dispose^ happen
} Held;

static const LhatHostDataTag *held_tag;
static const LhatHostDataTag *other_tag;
static int wrong_type_reached;

static LhatValue held_make(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)arguments;
    (void)count;
    Held *held = (Held *)context;
    held->live = 1;
    LhatValue out = lhat_nil();
    return lhat_machine_make_hostdata(machine, held_tag, held, &out) ? out
                                                                 : lhat_nil();
}

static LhatValue held_read(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    Held *self = (Held *)lhat_hostdata_pointer(arguments[0], held_tag);
    if (self == NULL) {
        wrong_type_reached++;
        return lhat_nil();
    }
    return lhat_integer(self->value);
}

static LhatValue held_dispose(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    Held *self = (Held *)lhat_hostdata_pointer(arguments[0], held_tag);
    if (self != NULL) {
        self->live = 0;
    }
    return lhat_nil();
}

static LhatValue other_make(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    return lhat_machine_make_hostdata(machine, other_tag, NULL, &out) ? out
                                                                  : lhat_nil();
}

static void test_host_data(void)
{
    LhatProgram program;
    Disk disk;
    Held held = {42, 0};

    LHAT_TEST("a host value carries a pointer and answers its own members");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ h = store.make()\n"
             "var^ v : number^ = h.read()\n"
             "h.dispose()\n"
             "return^ v\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        LHAT_CHECK(held_tag != NULL, "the type registered");
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        // 12.5 reads dispose^ off the type like any other, so registering one
        // is what makes the value the host's to take back.
        lhat_register_member(&program, "store", "Held", "dispose", "p^self^;",
                             held_dispose, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);

        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            LHAT_CHECK_EQ_INT(held.live, 0);  // the dispose^ ran
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 7.3: identity is the declaration. Two host types of the same shape are
    // still two types, and the tag is what says so where a pointer would
    // otherwise be read as the wrong thing.
    LHAT_TEST("and a value of another type does not reach its C");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ o = store.makeOther()\n"
             "var^ v = o.read()\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        wrong_type_reached = 0;
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        other_tag = lhat_register_hostdata_type(&program, "store", "Other");
        LHAT_CHECK(held_tag != other_tag, "the tags are distinct");
        // The same C function on both, which is what makes the tag the only
        // thing standing between them.
        lhat_register_member(&program, "store", "Other", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_func(&program, "store", "makeOther", "f^ -> store.Other;",
                           other_make, NULL);

        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(wrong_type_reached, 1);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 11.3 stays structural for everything else, but not here: a host type is
    // nominal, so one cannot be written where the other is wanted.
    LHAT_TEST("and the checker keeps the two apart as well");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ h : store.Held = store.makeOther()\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        other_tag = lhat_register_hostdata_type(&program, "store", "Other");
        lhat_register_func(&program, "store", "makeOther", "f^ -> store.Other;",
                           other_make, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_MISMATCH),
                   "an Other is not a Held");
    }
    lhat_program_dispose(&program);
}

// 05 の 8.8 with 02 の 10.7: what the host made goes back whether or not the
// program said so, and goes back once.
static int cells_live;
static int cells_freed;
static const LhatHostDataTag *cell_tag;

static LhatValue cell_make(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)arguments;
    (void)count;
    int *cell = (int *)malloc(sizeof *cell);
    if (cell == NULL) {
        return lhat_nil();
    }
    *cell = 1;
    cells_live++;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, cell_tag, cell, &out)) {
        free(cell);
        cells_live--;
        return lhat_nil();
    }
    return out;
}

static LhatValue cell_release(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    int *cell = (int *)lhat_hostdata_pointer(arguments[0], cell_tag);
    if (cell != NULL) {
        free(cell);
        cells_live--;
        cells_freed++;
    }
    return lhat_nil();
}

// Runs one program against a store of cells and reports what was left.
static void with_cells(const char *text, int *live_after_run, int *freed_total)
{
    LhatProgram program;
    Disk disk;
    const File files[] = {{"main.lh", text}};

    cells_live = 0;
    cells_freed = 0;
    program_with(&program, &disk, files, 1);
    cell_tag = lhat_register_hostdata_type(&program, "store", "Cell");
    // 12.5: registering this is what makes the value the host's to take back.
    lhat_register_member(&program, "store", "Cell", "dispose", "p^self^;",
                         cell_release, NULL);
    lhat_register_func(&program, "store", "make", "f^ -> store.Cell;",
                       cell_make, NULL);

    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    size_t count = 0;
    const LhatModule *modules = lhat_program_compile(&program, &count);
    if (modules != NULL && root != NULL) {
        LhatMachine *machine = lhat_machine_new();
        lhat_machine_set_modules(machine, modules, count);
        lhat_program_install(&program, machine);
        lhat_run(machine, modules[root->index].proto);
        *live_after_run = cells_live;
        lhat_machine_dispose(machine);
    }
    *freed_total = cells_freed;
    lhat_program_dispose(&program);
}

static void test_host_data_release(void)
{
    int live = -1;
    int freed = -1;

    LHAT_TEST("a dispose^ written by hand gives the pointer back");
    with_cells("import^ store\nvar^ c = store.make()\nc.dispose()\nreturn^ 0\n",
               &live, &freed);
    LHAT_CHECK_EQ_INT(live, 0);
    // 10.7: and the machine going does not give it back a second time.
    LHAT_CHECK_EQ_INT(freed, 1);

    LHAT_TEST("and one nobody disposed of goes back when the machine does");
    with_cells("import^ store\nvar^ c = store.make()\nreturn^ 0\n", &live,
               &freed);
    LHAT_CHECK_EQ_INT(live, 1);   // still the host's while the machine lives
    LHAT_CHECK_EQ_INT(freed, 1);  // and given back when it went

    // 10.7's last resort: what became unreachable is collected, and the
    // pointer goes back then rather than at the end.
    LHAT_TEST("and one that became unreachable goes back at the collection");
    with_cells("import^ store\n"
               "var^ drop = p^ { var^ c = store.make() }\n"
               "drop()\n"
               "L^.collectgarbage()\n"
               "return^ 0\n",
               &live, &freed);
    LHAT_CHECK_EQ_INT(live, 0);
    LHAT_CHECK_EQ_INT(freed, 1);

    LHAT_TEST("and one disposed of then collected goes back once");
    with_cells("import^ store\n"
               "var^ drop = p^ { var^ c = store.make()  c.dispose() }\n"
               "drop()\n"
               "L^.collectgarbage()\n"
               "return^ 0\n",
               &live, &freed);
    LHAT_CHECK_EQ_INT(live, 0);
    LHAT_CHECK_EQ_INT(freed, 1);
}

// 05 の 8.9: the seam a shared build has to use, and the order it has that a
// linker seam does not.
static long registered_allocations;

static void *counting_alloc(void *context, size_t size)
{
    (void)context;
    registered_allocations++;
    return malloc(size);
}

static void *counting_calloc(void *context, size_t count, size_t size)
{
    (void)context;
    registered_allocations++;
    return calloc(count, size);
}

static void *counting_realloc(void *context, void *pointer, size_t size)
{
    (void)context;
    if (pointer == NULL) {
        registered_allocations++;
    }
    return realloc(pointer, size);
}

static void counting_free(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}

static void test_port(void)
{
    LhatProgram program;

    // A program is given its loader or it reads nothing: 8.9 keeps an
    // embedded language off a file system it was not pointed at.
    LHAT_TEST("a program with no loader reads nothing");
    {
        lhat_program_init(&program, true, NULL, NULL);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL,
                   "nothing was read");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CANNOT_READ),
                   "and it was reported rather than passing quietly");
    }
    lhat_program_dispose(&program);

    // The registration is refused once anything has been taken, since what is
    // already held would then go back to a different free.
    LHAT_TEST("an allocator registered after the first allocation is refused");
    {
        LhatAllocator mine = {counting_alloc, counting_calloc, counting_realloc,
                              counting_free, NULL};
        // The program above took memory for its unit and its diagnostic, so
        // the refusal here is about the order and not about being second.
        LHAT_CHECK(!lhat_set_allocator(&mine), "refused");
        LHAT_CHECK_EQ_INT(registered_allocations, 0);
    }

    LHAT_TEST("and one missing a function is refused whenever it comes");
    {
        LhatAllocator partial = {counting_alloc, NULL, counting_realloc,
                                 counting_free, NULL};
        LHAT_CHECK(!lhat_set_allocator(&partial), "all four or none");
    }
}

// 05 の 8.9: a host value lives in stack slots and nowhere else. A table
// element is the first place one would escape to, and the checker refuses
// it by name. The maker is never called -- the check is the whole test.
static void test_hostvalue_escape(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a host value stored into a table is refused by name");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.v\n"
             "var^ t = { test.v.make() }\n"
             "return^ 1\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostvalue_type(&program, "test.v", "V", 8) !=
                       NULL,
                   "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "test.v", "make",
                                      "f^ -> test.v.V;", host_add, NULL),
                   "the maker registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES),
                   "the escape is refused by name");
    }
    lhat_program_dispose(&program);

    // 02 の 11.8改: the unary '-' over a host value. call_operator's host arm
    // builds its own operand array and counts what it hands over, so a unary
    // one reaches C with a single argument -- the closure arm passing is no
    // evidence for this path.
    LHAT_TEST("a host value answers the unary '-'");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ v = test.c.make()\n"
             "return^ -v\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag =
            lhat_register_hostvalue_type(&program, "test.c", "C",
                                         sizeof(Counter));
        LHAT_CHECK(tag != NULL, "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "test.c", "make",
                                      "f^ -> test.c.C;", host_counter_make,
                                      (void *)tag),
                   "the maker registration took");
        LHAT_CHECK(lhat_register_hostvalue_member(&program, "test.c", "C", "-",
                                                  "f^self^ -> number^;",
                                                  host_counter_negate,
                                                  (void *)tag),
                   "the operator registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), -7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 02 の 14.17 with 05 の 8.9: every value can be written down, a host
    // value included. Nothing is registered under tostring here, so this is
    // the built-in answering -- and what a value with no spelling of its own
    // answers is what it is, the way a coroutine answers 'c^'. The library
    // that wants better registers one, which 14.17 lets win over this.
    LHAT_TEST("a host value with no tostring answers its type's name");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ v = test.c.make()\n"
             "if^ $\"{v}\" = \"<test.c.C>\" and^\n"
             "   v.tostring() = \"<test.c.C>\" { return^ 1 }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        LHAT_CHECK(tag != NULL, "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "test.c", "make",
                                      "f^ -> test.c.C;", host_counter_make,
                                      (void *)tag),
                   "the maker registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 02 の 14.12 with 05 の 8.7: one registered name carrying several
    // signatures. A host has no body for the compiler to read a descriptor
    // out of, so the registration lowers the one it was written with and the
    // machine's search reads that -- the same search that resolves a written
    // overload^.
    //
    // Four arms over one type: '-' unary and binary (told apart by the
    // count), '+' with a trailing self^ (11.3改, so the receiver is the right
    // operand), and a second binary '-' taking a string^ where the first
    // takes a number^ -- which counts alone could never tell apart.
    LHAT_TEST("a registered name carries several signatures");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ v = test.c.make()\n"
             "return^ (0 - -v) * 1000 + (v - 2) * 100 + (10 + v) * 10 +\n"
             "       (v - \"x\") - 99\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        LHAT_CHECK(tag != NULL, "the type registration took");
        lhat_register_func(&program, "test.c", "make", "f^ -> test.c.C;",
                           host_counter_make, (void *)tag);
        LHAT_CHECK(lhat_register_hostvalue_member(&program, "test.c", "C", "-",
                                                  "f^self^ -> number^;",
                                                  host_counter_negate,
                                                  (void *)tag),
                   "the unary arm registered");
        LHAT_CHECK(lhat_register_hostvalue_member(
                       &program, "test.c", "C", "-",
                       "f^self^, number^ -> number^;", host_counter_minus,
                       (void *)tag),
                   "the binary arm registered beside it");
        LHAT_CHECK(lhat_register_hostvalue_member(
                       &program, "test.c", "C", "-",
                       "f^self^, string^ -> number^;", host_counter_tagged,
                       (void *)tag),
                   "and one of the same count, told apart by type");
        LHAT_CHECK(lhat_register_hostvalue_member(
                       &program, "test.c", "C", "+",
                       "f^number^, self^ -> number^;", host_counter_radd,
                       (void *)tag),
                   "the right-operand arm registered");
        // 14.12 forbids arms that could take the same call, and the
        // registration is where that is settled for a host.
        LHAT_CHECK(!lhat_register_hostvalue_member(
                       &program, "test.c", "C", "-",
                       "f^self^, number^ -> number^;", host_counter_minus,
                       (void *)tag),
                   "an arm overlapping one already there is refused");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 7000 + 500 + 170 + 99 - 99
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7670);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // The descriptors a registration lowered are on the machine's heap and
    // nothing but the host holds them, so gc.c reaches them from there -- a
    // proto's live in its chunk and never face this.
    //
    // The reach is load-bearing: with it removed, the collections here free
    // fourteen descriptors -- counted at sweep_some, in the asan build as
    // well as the ordinary one. What is not settled is why AddressSanitizer
    // says nothing about the calls that follow, which read them; both the
    // count and its silence were measured, and they have not been reconciled.
    // So this case passes either way, and is a regression rather than a proof.
    LHAT_TEST("and the lowered signatures survive a collection");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ v = test.c.make()\n"
             // The collector runs a step at a time (config.h), so one call is
             // a step and not a cycle -- enough of them is what reaches the
             // sweep an unreached descriptor would be freed by.
             "repeat^ 200 { collectgarbage() }\n"
             "return^ (v - 2) * 10 + (v - \"x\")\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        lhat_register_func(&program, "test.c", "make", "f^ -> test.c.C;",
                           host_counter_make, (void *)tag);
        lhat_register_hostvalue_member(&program, "test.c", "C", "-",
                                       "f^self^, number^ -> number^;",
                                       host_counter_minus, (void *)tag);
        lhat_register_hostvalue_member(&program, "test.c", "C", "-",
                                       "f^self^, string^ -> number^;",
                                       host_counter_tagged, (void *)tag);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 149);  // 5 * 10 + 99
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);
}

// 02 の 13.8改: a host answering several values. The positions go into the
// machine's room and come back as the run every other producer makes.
static LhatValue host_divmod(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)context;
    if (count != 2 || !lhat_is_integer(arguments[0]) ||
        !lhat_is_integer(arguments[1]) ||
        lhat_as_integer(arguments[1]) == 0) {
        return lhat_nil();
    }
    int64_t a = lhat_as_integer(arguments[0]);
    int64_t b = lhat_as_integer(arguments[1]);
    LhatValue out[2] = { lhat_integer(a / b), lhat_integer(a % b) };
    LhatValue answer = lhat_nil();
    if (!lhat_make_tuple(machine, out, 2, &answer)) {
        return lhat_nil();
    }
    return answer;
}

// 13.7 with 13.8改: the host arm gathers its own arguments, so a tuple spread
// into a variadic tail reaches it as ordinary arguments and nothing else.
static LhatValue host_sum(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    int64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (!lhat_is_integer(arguments[i])) {
            return lhat_nil();
        }
        total += lhat_as_integer(arguments[i]);
    }
    return lhat_integer(total);
}

// The same, but allocating after the room is filled -- what proves the
// positions are roots. A table made here would be swept if they were not.
static LhatValue host_divmod_then_allocate(LhatMachine *machine, void *context,
                                           const LhatValue *arguments,
                                           size_t count)
{
    LhatValue answer = host_divmod(machine, context, arguments, count);
    if (!lhat_is_run(answer)) {
        return answer;
    }
    for (int i = 0; i < 64; i++) {
        LhatValue dropped = lhat_nil();
        lhat_machine_make_table(machine, &dropped);  // dropped at once
    }
    return answer;
}

// 05 の 8.7 with 13.8改: registered as answering several values but written
// to answer one. The two sides can only disagree by being built apart, and
// the machine is what catches it.
static LhatValue host_answers_one(LhatMachine *machine, void *context,
                                  const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_integer(7);
}

static void test_host_tuple(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a host answers several values");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.num\n"
             "var^ q, r = system.num.divmod(7, 2)\n"
             "return^ q * 10 + r\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "system.num", "divmod",
                                      "f^number^, number^ -> "
                                      "(number^, number^);",
                                      host_divmod, NULL),
                   "the registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LHAT_CHECK(lhat_program_install(&program, machine), "installed");
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 31);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // The positions sit in the machine's room while the host runs on, so a
    // collection in between has to reach them.
    LHAT_TEST("and they survive what the host allocates afterwards");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.num\n"
             "var^ q, r = system.num.divmod(7, 2)\n"
             "return^ q * 10 + r\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_func(&program, "system.num", "divmod",
                           "f^number^, number^ -> (number^, number^);",
                           host_divmod_then_allocate, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 31);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // The hole this closed: the host path used to ignore what the call site
    // reserved, so a registration promising two values and a C function
    // answering one left the position slots holding whatever was there.
    LHAT_TEST("a host that answers one where two were promised faults");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.num\n"
             "var^ t = pack^ system.num.divmod(7, 2)\n"
             "return^ t[1]\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_func(&program, "system.num", "divmod",
                           "f^number^, number^ -> (number^, number^);",
                           host_answers_one, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_TUPLE_ARITY);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 13.8改: the other direction. A unit answering a tuple hands the host
    // its positions, and `value` is position 1 so a host written before
    // tuples still reads something it can use.
    LHAT_TEST("a unit answers several values to the host");
    {
        static const File files[] = {
            {"main.lh",
             "var^ divmod = f^ a:number^, b:number^ -> (number^, number^) {\n"
             "  return^ a // b, a % b }\n"
             "var^ q, r = divmod(7, 2)\n"
             "return^ q, r\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(ran.position_count, 2);
            if (ran.position_count == 2) {
                LHAT_CHECK_EQ_INT(lhat_as_integer(ran.positions[0]), 3);
                LHAT_CHECK_EQ_INT(lhat_as_integer(ran.positions[1]), 1);
            }
            // Position 1, so reading only `value` still reads a number.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // An ordinary answer leaves the count at zero, which is what keeps every
    // host written before this reading exactly what it always read.
    LHAT_TEST("and one value leaves no positions behind");
    {
        static const File files[] = {
            {"main.lh", "return^ 5\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(ran.position_count, 0);
            LHAT_CHECK(ran.positions == NULL, "nothing to point at");
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 13.7: 'expr...' forwards a tuple the same way it forwards a collected
    // tail. The host arm builds its argument array itself, so it expands the
    // run on a path of its own -- the closure arm passing is no evidence.
    LHAT_TEST("a tuple spreads into a host's variadic tail");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.num\n"
             "var^ f = f^ -> (number^, number^) { return^ 10, 20 }\n"
             "return^ system.num.sum(1, f()...)\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "system.num", "sum",
                                      "f^number^, ...:number^ -> number^;",
                                      host_sum, NULL),
                   "the registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules != NULL && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_machine_set_modules(machine, modules, count);
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 31);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.9 with 13.8改: a position is one slot, and a host value is as
    // wide as its tag says. Refused where the signature is read, which is
    // the asymmetry closing this opened.
    LHAT_TEST("a host value written as a position is refused");
    {
        static const File files[] = {
            {"main.lh", "import^ test.w\nreturn^ 1\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostvalue_type(&program, "test.w", "W", 8) !=
                       NULL,
                   "the type registration took");
        LHAT_CHECK(!lhat_register_func(&program, "test.w", "bad",
                                       "f^ -> (number^, test.w.W);",
                                       host_answers_one, NULL),
                   "a host value cannot be a position");
    }
    lhat_program_dispose(&program);
}

int main(void)
{
    // 8.9: before anything is taken, so the refusal above is about the order
    // rather than about this test running second.
    test_port();
    test_dependencies();
    test_loading();
    test_cycles();
    test_running();
    test_hosting();
    test_hostvalue_escape();
    test_host_tuple();
    test_host_data();
    test_host_data_release();
    return lhat_test_report("test_program");
}
