// L^ (lhat) -- tests for the unit graph.
//
// Section numbers refer to DesignDocuments/05-modules.md. The loader is
// replaced with a table held here, so what is pinned is the graph — the
// dependency order of 6.2, the single load of 5.3 and the refusal of 6.3 —
// rather than anything about a file system.

#include <stdlib.h>
#include <string.h>

#include "program.h"
#include "testutil.h"
#include "value.h"
#include "vm.h"

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
    lhat_program_init(program, true);
    lhat_program_set_loader(program, disk_load, disk);
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
         "let^ secret = 1\n"},
        {"main.lh",
         "let^ g = require^ \"lib/geometry.lh\"\n"
         "let^ d : number^ = g.dist(1, 2)\n"},
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
             "let^ g = require^ \"lib/geometry.lh\"\n"
             "let^ d = g.dist(1, \"text\")\n"},
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
             "let^ i = require^ \"inner.lh\"\n"
             "public^ let^ w : number^ = i.v\n"},
            {"main.lh",
             "let^ o = require^ \"lib/outer.lh\"\n"
             "let^ n : number^ = o.w\n"},
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
             "let^ s = require^ \"../shared.lh\"\n"
             "public^ let^ w : number^ = s.v\n"},
            {"main.lh", "let^ u = require^ \"lib/user.lh\"\n"},
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
             "let^ c = require^ \"common.lh\"\n"
             "public^ let^ x : number^ = c.v\n"},
            {"lib/b.lh",
             "let^ c = require^ \"common.lh\"\n"
             "public^ let^ y : number^ = c.v\n"},
            {"main.lh",
             "let^ a = require^ \"lib/a.lh\"\n"
             "let^ b = require^ \"lib/b.lh\"\n"
             "let^ n : number^ = a.x + b.y\n"},
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
             "let^ a = require^ \"lib/common.lh\"\n"
             "let^ b = require^ \"./lib/../lib/common.lh\"\n"},
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
            {"main.lh", "let^ g = require^ \"nowhere.lh\"\n"},
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
             "let^ b = require^ \"b.lh\"\n"
             "public^ let^ x = 1\n"},
            {"b.lh",
             "let^ a = require^ \"a.lh\"\n"
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
            {"a.lh", "let^ b = require^ \"b.lh\"\n"},
            {"b.lh", "let^ c = require^ \"c.lh\"\n"},
            {"c.lh", "let^ a = require^ \"a.lh\"\n"},
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
             "let^ c = require^ \"common.lh\"\n"
             "public^ let^ x : number^ = c.v\n"},
            {"b.lh",
             "let^ c = require^ \"common.lh\"\n"
             "public^ let^ y : number^ = c.v\n"},
            {"main.lh",
             "let^ a = require^ \"a.lh\"\n"
             "let^ b = require^ \"b.lh\"\n"},
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

    LHAT_TEST("and the expression form binds it under a name of its own");
    {
        static const File files[] = {
            {"main.lh",
             "let^ g = import^ system.gfx\n"
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
             "let^ h = store.make()\n"
             "let^ v : number^ = h.read()\n"
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
             "let^ o = store.makeOther()\n"
             "let^ v = o.read()\n"
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
             "let^ h : store.Held = store.makeOther()\n"},
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

int main(void)
{
    test_dependencies();
    test_loading();
    test_cycles();
    test_running();
    test_hosting();
    test_host_data();
    return lhat_test_report("test_program");
}
