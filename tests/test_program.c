// L^ (lhat) -- tests for the unit graph.
//
// Section numbers refer to DesignDocuments/05-modules.md. The loader is
// replaced with a table held here, so what is pinned is the graph — the
// dependency order of 6.2, the single load of 5.3 and the refusal of 6.3 —
// rather than anything about a file system.

#include <stdio.h>
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

static const LhatUnit *unit_at(const LhatProgram *program, const char *path)
{
    for (const LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (strcmp(lhat_unit_path(u), path) == 0) {
            return u;
        }
    }
    return NULL;
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
// 05 の 5.7: a unit changes under a program that has already been compiled
// and run. What is pinned here is the whole exchange -- the cascade, the
// save that changed nothing, the machine's registry, and the old bodies
// staying whole -- because each half is useless without the others.
static void test_reloading(void)
{
    LhatProgram program;
    Disk disk;

    // Not const: a reload is a file whose text is no longer what it was.
    static File files[] = {
        {"leaf.lh",
         "module^ ns.leaf\n"
         "public^ let^ n = 1\n"},
        {"mid.lh",
         "module^ ns.mid\n"
         "require^ \"leaf.lh\"\n"
         "public^ let^ n = ns.leaf.n * 10\n"},
        {"main.lh",
         "require^ \"mid.lh\"\n"
         "return^ ns.mid.n\n"},
    };
    static const char *const leaf_was = "module^ ns.leaf\npublic^ let^ n = 1\n";
    static const char *const leaf_now = "module^ ns.leaf\npublic^ let^ n = 2\n";

    LHAT_TEST("a save that changed nothing retires nothing");
    {
        files[0].text = leaf_was;
        program_with(&program, &disk, files, 3);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") != NULL &&
                       lhat_program_compile(&program),
                   "the program checked and compiled");
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "leaf.lh"), 0);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);
        // And the unit is untouched: still done, still carrying its body.
        const LhatUnit *leaf = unit_at(&program, "leaf.lh");
        LHAT_CHECK(leaf != NULL && lhat_unit_state(leaf) == LHAT_UNIT_DONE &&
                       lhat_unit_proto(leaf) != NULL,
                   "the unit kept what was made of it");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a path the program never checked answers SIZE_MAX");
    {
        files[0].text = leaf_was;
        program_with(&program, &disk, files, 3);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") != NULL,
                   "the program checked");
        LHAT_CHECK(lhat_program_invalidate(&program, "nowhere.lh") == SIZE_MAX,
                   "no such unit");
    }
    lhat_program_dispose(&program);

    // The cascade: a requirer's checked types come from what the required
    // unit publishes, and its compiled body holds a table of the very protos
    // its require^s answer. Changing the leaf reaches all three.
    LHAT_TEST("an invalidation reaches every unit that requires the one that changed");
    {
        files[0].text = leaf_was;
        program_with(&program, &disk, files, 3);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") != NULL &&
                       lhat_program_compile(&program),
                   "the program checked and compiled");

        files[0].text = leaf_now;
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "leaf.lh"), 3);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 3);
        LHAT_CHECK_EQ_INT(unit_count(&program), 3);  // the shells stayed

        for (const LhatUnit *u = program.units; u != NULL; u = u->next) {
            LHAT_CHECK(lhat_unit_state(u) == LHAT_UNIT_STALE,
                       "%s went stale", lhat_unit_path(u));
            LHAT_CHECK(lhat_unit_proto(u) == NULL, "and gave up its body");
        }
        // 5.7: the module name outlives the check that read it, since it is
        // what the host has to hand every machine next.
        const LhatUnit *leaf = unit_at(&program, "leaf.lh");
        LHAT_CHECK(leaf != NULL &&
                       strcmp(lhat_unit_module_name(leaf), "ns.leaf") == 0,
                   "a stale unit still says what it registered as");

        lhat_program_discard_retired(&program);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);
    }
    lhat_program_dispose(&program);

    // A program-level diagnostic outlives the check that raised it, so an
    // invalidation has to take back the ones about text it just threw away
    // -- otherwise one unreadable require^ makes the program wrong for ever.
    LHAT_TEST("an invalidation takes back what the retired units were blamed for");
    {
        static File mending[] = {
            {"main.lh",
             "require^ \"gone.lh\"\n"
             "return^ 1\n"},
            {"gone.lh",
             "module^ ns.gone\n"
             "public^ let^ n = 5\n"},
        };
        // One file to begin with: the require^ reaches nothing.
        program_with(&program, &disk, mending, 1);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") != NULL,
                   "the root itself read");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_CANNOT_READ),
                   "and the missing unit was reported");

        // The file appears. Invalidating it reaches the unit that required
        // it -- an edge the checker recorded, since nothing here ever
        // compiled and the compiler is the other place edges come from.
        disk.count = 2;
        mending[0].text =
            "require^ \"gone.lh\"\n"
            "return^ ns.gone.n\n";
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "gone.lh"), 2);
        LHAT_CHECK(!has_program_error(&program, LHAT_PROGRAM_ERR_CANNOT_READ),
                   "and the report about it went with it");

        LHAT_CHECK(lhat_program_check(&program, "main.lh") != NULL &&
                       !lhat_program_has_errors(&program),
                   "the mended program has nothing against it");
    }
    lhat_program_dispose(&program);


    // The whole point, on one machine: without forgetting what the units
    // registered, a recompiled body finds the old table at its own guard
    // (vm.c's UNIT) and hands that back -- so the new text never runs.
    LHAT_TEST("a reloaded unit runs its new body only once the machine forgets the old");
    {
        files[0].text = leaf_was;
        program_with(&program, &disk, files, 3);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program),
                   "the program checked and compiled");

        LhatMachine *machine = lhat_machine_new();
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 10);

        files[0].text = leaf_now;
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "leaf.lh"), 3);
        root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       lhat_program_compile(&program),
                   "the program read the units again and compiled them");
        LHAT_CHECK(lhat_unit_proto(root) != NULL, "a new body for the root");

        // The new bodies, run against a machine that still holds the old
        // registry: every guard hits and nothing of the new text takes.
        ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 10);

        LHAT_CHECK(lhat_machine_forget_unit(machine, "ns.leaf"),
                   "the machine forgot the leaf");
        LHAT_CHECK(lhat_machine_forget_unit(machine, "ns.mid"),
                   "and the unit that published from it");
        LHAT_CHECK(!lhat_machine_forget_unit(machine, "ns.leaf"),
                   "forgetting twice says there was nothing there");
        LHAT_CHECK(!lhat_machine_forget_unit(machine, "ns.never"),
                   "and so does a name nothing registered under");

        ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 20);

        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);

    // Nothing is freed by an invalidation, so a closure made before one
    // keeps running the body it was made from. Under asan this is what says
    // the old world is whole rather than merely unvisited.
    LHAT_TEST("a body retired under a live closure still runs");
    {
        static File live[] = {
            {"lib.lh",
             "module^ ns.lib\n"
             "public^ let^ answer = f^ -> number^ { return^ 1 }\n"},
            {"main.lh",
             "require^ \"lib.lh\"\n"
             "return^ ns.lib.answer\n"},
        };
        live[0].text =
            "module^ ns.lib\n"
            "public^ let^ answer = f^ -> number^ { return^ 1 }\n";
        program_with(&program, &disk, live, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program),
                   "the program checked and compiled");

        LhatMachine *machine = lhat_machine_new();
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LhatValue held = ran.value;  // rooted through L^.modules.ns.lib

        live[0].text =
            "module^ ns.lib\n"
            "public^ let^ answer = f^ -> number^ { return^ 2 }\n";
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "lib.lh"), 2);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 2);

        LhatRunResult called = lhat_machine_call(machine, held, NULL, 0);
        LHAT_CHECK_EQ_INT(called.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(called.value), 1);

        // The closure is still on the machine, so the retired bodies stay:
        // discarding them here is exactly the use-after-free the host is
        // told to avoid. The machine goes first.
        lhat_machine_dispose(machine);
        lhat_program_discard_retired(&program);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);
    }
    lhat_program_dispose(&program);
}

// 05 の 5.7 with 02 の 10.7: the one order that has to hold when a host
// reloads a program under a machine that has been running it. A coroutine
// suspended inside a do^ … finally^ holds the closure it stopped in, and so
// the body that closure was made from. Retire that body, drop the coroutine,
// and until its cleanup has run there is still something pointing into what
// lhat_program_discard_retired is about to free.
//
// The whole of the answer is that lhat_machine_collectgarbage runs those
// cleanups itself, so lhat_machine_pending_disposals reads zero and the host
// has a question it can actually ask. Worth running under asan: without the
// drain this is a use-after-free the next time anything runs.
static void test_reloading_with_a_pending_cleanup(void)
{
    LhatProgram program;
    Disk disk;

    static File files[] = {
        {"gen.lh",
         "module^ ns.gen\n"
         "let^ made = p^ {\n"
         "  do^{\n"
         "    yield^ 1\n"
         "  finally^:\n"
         "    var^ t = { a := 1 }\n"
         "  }\n"
         "}\n"
         "public^ let^ held = made()\n"
         "held.start()\n"},
        {"main.lh",
         "require^ \"gen.lh\"\n"
         "return^ 1\n"},
    };

    LHAT_TEST("a suspended cleanup is run before its body may be discarded");
    {
        files[0].text =
            "module^ ns.gen\n"
            "let^ made = p^ {\n"
            "  do^{\n"
            "    yield^ 1\n"
            "  finally^:\n"
            "    var^ t = { a := 1 }\n"
            "  }\n"
            "}\n"
            "public^ let^ held = made()\n"
            "held.start()\n";
        program_with(&program, &disk, files, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       lhat_program_compile(&program),
                   "the program checked and compiled");

        LhatMachine *machine = lhat_machine_new();
        lhat_program_install(&program, machine);
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);

        // The reload. The bodies are retired, not freed -- the coroutine
        // suspended in one of them is still holding it.
        files[0].text =
            "module^ ns.gen\n"
            "let^ made = p^ {\n"
            "  do^{\n"
            "    yield^ 2\n"
            "  finally^:\n"
            "    var^ t = { a := 2 }\n"
            "  }\n"
            "}\n"
            "public^ let^ held = made()\n"
            "held.start()\n";
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "gen.lh"), 2);
        root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       lhat_program_compile(&program),
                   "the units read and compiled again");

        // 5.3: what still holds the old coroutine is the module table the
        // machine registered, so forgetting the unit is also what drops it.
        LHAT_CHECK(lhat_machine_forget_unit(machine, "ns.gen"),
                   "the machine forgot what the old unit registered");

        // And here is the whole point: the cycle finds the coroutine and
        // runs its finally^ -- against the old body, which is still there
        // because nothing has been discarded yet.
        lhat_machine_collectgarbage(machine);
        LHAT_CHECK_EQ_INT(lhat_machine_pending_disposals(machine), 0);

        // Only now may the old bodies go.
        lhat_program_discard_retired(&program);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);

        // The new ones run, and nothing reaches into what was freed.
        ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        lhat_machine_collectgarbage(machine);
        LHAT_CHECK_EQ_INT(lhat_machine_pending_disposals(machine), 0);

        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);

    // The same again where the cleanup panics. A fault leaves its frames
    // standing everywhere else, and here that would be the very trap this
    // test is about: the frames hold the closure the coroutine was
    // suspended in, so a host asking pending_disposals before discarding
    // would be told "nothing waiting" while a frame still pointed into the
    // body it is about to free. The cleanup is abandoned and its frames go.
    LHAT_TEST("and a cleanup that panics leaves nothing pointing into the body");
    {
        files[0].text =
            "module^ ns.gen\n"
            "let^ made = p^ {\n"
            "  do^{\n"
            "    yield^ 1\n"
            "  finally^:\n"
            "    panic^ \"from a cleanup\"\n"
            "  }\n"
            "}\n"
            "public^ let^ held = made()\n"
            "held.start()\n";
        program_with(&program, &disk, files, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       lhat_program_compile(&program),
                   "the program checked and compiled");
        if (root == NULL || lhat_unit_proto(root) == NULL) {
            lhat_program_dispose(&program);
            return;
        }

        LhatMachine *machine = lhat_machine_new();
        lhat_program_install(&program, machine);
        LHAT_CHECK_EQ_INT(lhat_run(machine, lhat_unit_proto(root)).status,
                          LHAT_RUN_OK);

        files[0].text =
            "module^ ns.gen\n"
            "let^ made = p^ {\n"
            "  do^{\n"
            "    yield^ 2\n"
            "  finally^:\n"
            "    panic^ \"from a cleanup\"\n"
            "  }\n"
            "}\n"
            "public^ let^ held = made()\n"
            "held.start()\n";
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "gen.lh"), 2);
        root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program),
                   "the units read and compiled again");
        LHAT_CHECK(lhat_machine_forget_unit(machine, "ns.gen"),
                   "the machine forgot what the old unit registered");

        lhat_machine_collectgarbage(machine);
        LHAT_CHECK_EQ_INT(lhat_machine_pending_disposals(machine), 0);
        // No frames of the abandoned cleanup left to walk, and so none
        // holding the body about to go.
        LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(machine), 0);

        lhat_program_discard_retired(&program);
        LHAT_CHECK_EQ_INT(lhat_run(machine, lhat_unit_proto(root)).status,
                          LHAT_RUN_OK);
        lhat_machine_collectgarbage(machine);
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);
}

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
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "every unit compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.7 reads an import^ root off L^.modules inside a nested body
    // rather than capturing it. A require^ root is not one: what a unit made
    // was made by running it, and 8.6's registry is the machine's own. So the
    // capture stays, and a body written around one still sees what it built.
    LHAT_TEST("a nested body still captures a require^ root");
    {
        static const File captured[] = {
            {"one.lh",
             "module^ ns.one\n"
             "public^ let^ v = 7\n"},
            {"main.lh",
             "require^ \"one.lh\"\n"
             "var^ read = f^ -> number^ { return^ ns.one.v }\n"
             "return^ read()\n"},
        };
        program_with(&program, &disk, captured, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // A require^ of a unit that never compiled -- a host that ran in spite
    // of what the checker said -- is refused where it runs, not before.
    LHAT_TEST("and a unit that never compiled is refused at the require^");
    {
        static const File pair[] = {
            {"one.lh", "module^ ns.one\npublic^ let^ v = (\n"},
            {"main.lh", "require^ \"one.lh\"\nreturn^ 1\n"},
        };
        program_with(&program, &disk, pair, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "one.lh does not parse");
        lhat_program_compile(&program);
        if (root != NULL && lhat_unit_proto(root) != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_NO_SUCH_UNIT);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 5.3: a program grows under a machine already running. The
    // second unit is checked and compiled after the first ran, reaches one
    // the first batch compiled and one of its own, and the registry on the
    // machine is what makes the shared one load once.
    LHAT_TEST("a unit checked after a compile compiles with the next call");
    {
        static const File later[] = {
            {"shared.lh",
             "module^ ns.shared\n"
             "var^ loads = 0\n"
             "loads := loads + 1\n"
             "public^ let^ count = loads\n"},
            {"extra.lh", "module^ ns.extra\npublic^ let^ v = 10\n"},
            {"first.lh", "require^ \"shared.lh\"\nreturn^ ns.shared.count\n"},
            {"second.lh",
             "require^ \"shared.lh\"\n"
             "require^ \"extra.lh\"\n"
             "return^ ns.shared.count * 100 + ns.extra.v\n"},
        };
        program_with(&program, &disk, later, 4);
        const LhatUnit *first = lhat_program_check(&program, "first.lh");
        LHAT_CHECK(lhat_program_compile(&program), "the first batch compiled");
        LhatMachine *machine = lhat_machine_new();
        if (first != NULL && lhat_unit_proto(first) != NULL) {
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(first));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
        }
        const LhatUnit *second = lhat_program_check(&program, "second.lh");
        LHAT_CHECK(second != NULL && !lhat_program_has_errors(&program),
                   "the second unit checked");
        LHAT_CHECK(lhat_program_compile(&program), "the second batch compiled");
        if (second != NULL && lhat_unit_proto(second) != NULL) {
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(second));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // shared.lh ran once, on the first run; extra.lh is new.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 110);
        }
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);
}

// 05 の 8.7: what the host registers, and how import^ reaches it.
// 05 の 8.6: one that goes into L^ itself rather than under its registry,
// so that 8.2's initial binding has something to name.
static void host_twice(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    int *calls = (int *)context;
    if (calls != NULL) {
        (*calls)++;
    }
    if (count != 1 || !lhat_is_integer(arguments[0])) {
        return;
    }
    answers[0] = lhat_integer(lhat_as_integer(arguments[0]) * 2);
    *answer_count = 1;
}

// Reads x and y off a table -- the signature above names them.
static void host_sum_xy(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    LhatValue kx = lhat_nil();
    LhatValue ky = lhat_nil();
    if (count != 1 || !lhat_is_object_kind(arguments[0], LHAT_OBJECT_TABLE) ||
        !lhat_machine_make_string(machine, "x", 1, &kx) ||
        !lhat_machine_make_string(machine, "y", 1, &ky)) {
        return;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(arguments[0]);
    LhatValue x = lhat_table_get(table, kx);
    LhatValue y = lhat_table_get(table, ky);
    if (!lhat_is_integer(x) || !lhat_is_integer(y)) {
        return;
    }
    answers[0] = lhat_integer(lhat_as_integer(x) + lhat_as_integer(y));
    *answer_count = 1;
}

// Defined with the variadic tests below; an arm above wants it too.
static void host_sum(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count);

// Answers 1 whatever it was given -- an arm that only has to be the one
// picked.
static void host_one(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    answers[0] = lhat_integer(1);
    *answer_count = 1;
}

// The same, answering something else -- so which arm ran can be read off
// the answer.
static void host_two(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    answers[0] = lhat_integer(2);
    *answer_count = 1;
}

static void host_add(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    int *calls = (int *)context;
    if (calls != NULL) {
        (*calls)++;
    }
    if (count != 2 || !lhat_is_integer(arguments[0]) ||
        !lhat_is_integer(arguments[1])) {
        return;
    }
    answers[0] = lhat_integer(lhat_as_integer(arguments[0]) +
                        lhat_as_integer(arguments[1]));
    *answer_count = 1;
}

// 02 の 11.8改 with 05 の 8.9: a host value carrying the unary '-'. The tag
// is handed over through the context, since a host function is given nothing
// else to read its own type by.
typedef struct {
    int64_t n;
} Counter;

// 05 の 8.9改: a host registered '-> T|nil^'. A positive argument answers
// the value arm, anything else the nil^ one -- which lands in the head slot
// of the room the width reserved, where ISNIL reads for it.
static void host_counter_maybe(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count,
                               LhatValue *answers, int *answer_count)
{
    if (count != 1 || !lhat_is_number(arguments[0]) ||
        lhat_as_real(arguments[0]) <= 0) {
        return;
    }
    Counter c = {7};
    LhatValue out = lhat_nil();
    answers[0] = lhat_make_hostvalue(machine, (const LhatHostValueTag *)context, &c,
                               &out)
               ? out
               : lhat_nil();
    *answer_count = 1;
}

static void host_counter_make(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count,
                              LhatValue *answers, int *answer_count)
{
    (void)arguments;
    if (count != 0) {
        return;
    }
    Counter c = {7};
    LhatValue out = lhat_nil();
    answers[0] = lhat_make_hostvalue(machine, (const LhatHostValueTag *)context, &c,
                               &out)
               ? out
               : lhat_nil();
    *answer_count = 1;
}

// f^self^ -> number^; -- one operand and no argument, which is the whole of
// what tells a unary operator from a binary one.
// 05 の 8.7改2: a host function that panics rather than answering.
static void host_refuse(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    lhat_machine_panic_text(machine, "width must not be negative");
    answers[0] = lhat_integer(99);
    *answer_count = 1;
    return;  // dropped
}

// The same, as a host value's unary '-'.
static void host_counter_refuse(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count,
                                LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    lhat_machine_panic_text(machine, "no negative of this");
    answers[0] = lhat_integer(99);
    *answer_count = 1;
}

// f^wide.V -> number^; -- reads the double the value carries. What the
// boundary laid into the frame is exactly what this sees.
static void host_wide_probe(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)machine;
    if (count != 1) {
        return;
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[0], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return;
    }
    double d;
    memcpy(&d, bytes, sizeof d);
    answers[0] = lhat_integer((int64_t)d);
    *answer_count = 1;
}

static void host_counter_negate(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count,
                                LhatValue *answers, int *answer_count)
{
    (void)machine;
    if (count != 1) {
        return;
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[0], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return;
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    answers[0] = lhat_integer(-c.n);
    *answer_count = 1;
}

// f^self^, o:number^ -> number^; -- the binary arm standing beside the unary
// one above, told apart by what each takes.
static void host_counter_minus(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count,
                               LhatValue *answers, int *answer_count)
{
    (void)machine;
    if (count != 2 || !lhat_is_integer(arguments[1])) {
        return;
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[0], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return;
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    answers[0] = lhat_integer(c.n - lhat_as_integer(arguments[1]));
    *answer_count = 1;
}

// f^self^, o:test.c.C -> bool^; -- 02 の 11.9改's op^= for a host value.
// It answers no to everything, including two whose bytes are the same, which
// is what tells it apart from 05 の 8.9's default: byte equality could never
// answer that, so a false here is the registration being asked.
static void host_counter_never_equal(LhatMachine *machine, void *context,
                                     const LhatValue *arguments, size_t count,
                                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    answers[0] = count == 2 ? lhat_bool(false) : lhat_nil();
    *answer_count = 1;
}

// f^lhs:number^, self^ -> number^; -- 02 の 11.3改's trailing self^, so the
// receiver is the operand written on the RIGHT and 'n + v' finds it.
static void host_counter_radd(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count,
                              LhatValue *answers, int *answer_count)
{
    (void)machine;
    if (count != 2 || !lhat_is_integer(arguments[0])) {
        return;
    }
    const void *bytes =
        lhat_hostvalue_data(arguments[1], (const LhatHostValueTag *)context);
    if (bytes == NULL) {
        return;
    }
    Counter c;
    memcpy(&c, bytes, sizeof c);
    answers[0] = lhat_integer(lhat_as_integer(arguments[0]) + c.n);
    *answer_count = 1;
}

// f^self^, o:string^ -> number^; -- the same count as the binary '-' arm,
// told apart by the type alone. What the counts could not settle.
static void host_counter_tagged(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count,
                                LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    answers[0] = count == 2 ? lhat_integer(99) : lhat_nil();
    *answer_count = 1;
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LHAT_CHECK(lhat_program_install(&program, machine),
                       "what was registered went into L^.modules");
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
            LHAT_CHECK_EQ_INT(calls, 1);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // A structural type in a signature names members, and the signature's
    // text is gone once lhat_type_of_text has read it -- the names have to
    // be the arena's own, or the checker reads freed memory when it later
    // matches a written table against them (ASan caught this in a host).
    LHAT_TEST("a signature's member names outlive the signature's text");
    {
        static const File files[] = {
            {"main.lh",
             "import^ system.gfx\n"
             "let^ p = { x = 2, y = 3 }\n"
             "return^ system.gfx.sum(p) + system.gfx.sum({ x = 1, y = 1 })\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "system.gfx", "sum",
                                      "f^t^{ x: number^, y: number^ } "
                                      "-> number^;",
                                      host_sum_xy, &calls),
                   "the registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked against the member names");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 4.5: what a unit published, asked as types -- the contract a
    // host holds (update is 'p^number^;') checked before anything runs.
    LHAT_TEST("a unit's exports answer their types, and whether they conform");
    {
        static const File files[] = {
            {"game.lh",
             "module^ game\n"
             "public^ let^ update = p^ dt:number^ { }\n"
             "public^ let^ title = \"x\"\n"
             "let^ hidden = 1\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatUnit *root = lhat_program_check(&program, "game.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        LHAT_CHECK_EQ_INT(lhat_unit_export_count(root), 2);
        LhatUnitText first = lhat_unit_export_name(root, 0);
        LHAT_CHECK(first.text != NULL && first.length == 6 &&
                       memcmp(first.text, "update", 6) == 0,
                   "the first export is update");
        LHAT_CHECK(lhat_unit_export_name(root, 2).text == NULL,
                   "and there is no third");
        char spelt[64];
        size_t needed = lhat_unit_export_type_text(root, "update", spelt,
                                                   sizeof spelt);
        LHAT_CHECK(needed < sizeof spelt && strcmp(spelt, "p^number^;") == 0,
                   "update is spelt p^number^; -- got %s", spelt);
        LHAT_CHECK(lhat_unit_export_type_text(root, "hidden", NULL, 0) ==
                       SIZE_MAX,
                   "a private name is not an export");
        // 4.5: the same answer as the walkable descriptor -- which lives
        // on the compiled body, so compiling is part of the ask.
        LHAT_CHECK(lhat_program_compile(&program), "the program compiles");
        const LhatRuntimeType *rt = lhat_unit_export_type(root, "update");
        LHAT_CHECK(rt != NULL, "update answers a descriptor");
        LHAT_CHECK(rt != NULL && rt->kind == LHAT_TYPE_RT_SUBROUTINE,
                   "and it is a subroutine's");
        LHAT_CHECK(lhat_unit_export_type(root, "update") == rt,
                   "asked twice, the descriptor is the same one");
        LHAT_CHECK(lhat_unit_export_type(root, "hidden") == NULL,
                   "a private name answers none");
        LHAT_CHECK(lhat_unit_export_conforms(root, "update", "p^number^;"),
                   "update keeps the contract");
        LHAT_CHECK(lhat_unit_export_conforms(root, "title", "string^"),
                   "and title is a string");
        LHAT_CHECK(!lhat_unit_export_conforms(root, "update", "p^string^;"),
                   "a different parameter does not conform");
        LHAT_CHECK(!lhat_unit_export_conforms(root, "update", "f^number^;"),
                   "nor does an f^ stand for a p^");
        LHAT_CHECK(!lhat_unit_export_conforms(root, "nothere", "number^"),
                   "an export that is not there conforms to nothing");
        LHAT_CHECK(!lhat_unit_export_conforms(root, "title", "not a type"),
                   "text that is no type conforms to nothing");
    }
    lhat_program_dispose(&program);

    // 05 の 8.2: a host may bind a name so that a program writes it with no
    // qualification. 8.1 is unchanged -- the language hands out nothing, and
    // a host that binds none leaves a program seeing nothing.
    // 03 の 3.4改3 with 05 の 8.7: what an operator demands of an
    // unannotated parameter is the union of the types that carry it, and a
    // registered type carries one as readily as a def^ written here. Before
    // this the registry was not looked at, so '+' fell to number^ alone
    // however many host types carried it.
    LHAT_TEST("a registered operator is a candidate for an unwritten parameter");
    {
        static const File uses[] = {
            {"main.lh",
             "import^ test.c\n"
             "let^ add = f^ x, y { return^ x + y }\n"},
        };
        program_with(&program, &disk, uses, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        LHAT_CHECK(tag != NULL, "the type registration took");
        LHAT_CHECK(lhat_register_hostvalue_member(
                       &program, "test.c", "C", "+",
                       "f^self^, number^ -> number^;", host_counter_minus,
                       (void *)tag),
                   "the operator registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && has_check_error(
                                       root, LHAT_CHECK_ERR_OPERATOR_UNSETTLED),
                   "number^ and test.c.C both carry '+', so nothing picks");
    }
    lhat_program_dispose(&program);

    // 3.4改3's first way out: name one of the candidates. The other side
    // follows from the arm that one picks.
    LHAT_TEST("and writing one of them settles it");
    {
        static const File written[] = {
            {"main.lh",
             "import^ test.c\n"
             "let^ add = f^ x:number^, y { return^ x + y }\n"},
        };
        program_with(&program, &disk, written, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        lhat_register_hostvalue_member(&program, "test.c", "C", "+",
                                       "f^self^, number^ -> number^;",
                                       host_counter_minus, (void *)tag);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the written side decides");
    }
    lhat_program_dispose(&program);

    // 05 の 8.1: what a unit did not import it cannot see, so a registration
    // it never named must not make its arithmetic ambiguous. This is the
    // guard on the whole change -- without it every unit in a program would
    // pay for a type one other unit imports.
    LHAT_TEST("but a registration this unit did not import is no candidate");
    {
        static const File apart[] = {
            {"main.lh", "let^ add = f^ x, y { return^ x + y }\n"},
        };
        program_with(&program, &disk, apart, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        lhat_register_hostvalue_member(&program, "test.c", "C", "+",
                                       "f^self^, number^ -> number^;",
                                       host_counter_minus, (void *)tag);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the built-in answers alone, as it always did");
    }
    lhat_program_dispose(&program);

    // 05 の 8.7: a namespace and something under it are one registry, so
    // importing both into one scope is importing one tree twice -- the
    // parent's own table holds the child. Either order (a LOVE2D binding's
    // love.getVersion beside love.graphics).
    LHAT_TEST("a module and one under it are imported into one scope");
    {
        static const File parent_first[] = {
            {"main.lh",
             "import^ lib\n"
             "import^ lib.draw\n"
             "var^ a : number^ = lib.version()\n"
             "var^ b : number^ = lib.draw.line()\n"},
        };
        program_with(&program, &disk, parent_first, 1);
        lhat_register_func(&program, "lib", "version", "f^ -> number^;",
                           host_one, NULL);
        lhat_register_func(&program, "lib.draw", "line", "f^ -> number^;",
                           host_one, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the parent first");
    }
    lhat_program_dispose(&program);
    {
        static const File child_first[] = {
            {"main.lh",
             "import^ lib.draw\n"
             "import^ lib\n"
             "var^ a : number^ = lib.version()\n"
             "var^ b : number^ = lib.draw.line()\n"},
        };
        program_with(&program, &disk, child_first, 1);
        lhat_register_func(&program, "lib", "version", "f^ -> number^;",
                           host_one, NULL);
        lhat_register_func(&program, "lib.draw", "line", "f^ -> number^;",
                           host_one, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "and the child first");
    }
    lhat_program_dispose(&program);

    // What was not imported is still out of reach: importing the child
    // leaves a stand-in holding that child and nothing else.
    LHAT_TEST("and importing the child alone brings none of the parent");
    {
        static const File child_only[] = {
            {"main.lh",
             "import^ lib.draw\n"
             "var^ a : number^ = lib.version()\n"},
        };
        program_with(&program, &disk, child_only, 1);
        lhat_register_func(&program, "lib", "version", "f^ -> number^;",
                           host_one, NULL);
        lhat_register_func(&program, "lib.draw", "line", "f^ -> number^;",
                           host_one, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_has_errors(&program),
                   "version is not reachable through the stand-in");
    }
    lhat_program_dispose(&program);

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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LHAT_CHECK(lhat_program_install(&program, machine),
                       "what was registered reached L^");
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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

static void held_make(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    Held *held = (Held *)context;
    held->live = 1;
    LhatValue out = lhat_nil();
    answers[0] = lhat_machine_make_hostdata(machine, held_tag, held, &out) ? out
                                                                 : lhat_nil();
    *answer_count = 1;
}

static void held_read(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    Held *self = (Held *)lhat_hostdata_pointer(arguments[0], held_tag);
    if (self == NULL) {
        wrong_type_reached++;
        return;
    }
    answers[0] = lhat_integer(self->value);
    *answer_count = 1;
}

static void held_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    Held *self = (Held *)lhat_hostdata_pointer(arguments[0], held_tag);
    if (self != NULL) {
        self->live = 0;
    }
}

static void other_make(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    answers[0] = lhat_machine_make_hostdata(machine, other_tag, NULL, &out) ? out
                                                                  : lhat_nil();
    *answer_count = 1;
}

// The very pointer held_make wraps, under the other tag.
static void other_make_same(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    answers[0] = lhat_machine_make_hostdata(machine, other_tag, context, &out)
               ? out
               : lhat_nil();
    *answer_count = 1;
}

// Answers its receiver -- a member whose type names its own.
static void host_self(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    answers[0] = count >= 1 ? arguments[0] : lhat_nil();
    *answer_count = 1;
}

static void test_host_data(void)
{
    LhatProgram program;
    Disk disk;
    Held held = {42, 0};

    // A type whose members answer and take the type itself is a cycle in
    // the checker's types. The install lowers every registration's
    // signature to the machine's descriptors, and a walk that did not
    // remember where it was ran for 3^32 steps here (a LOVE2D binding's
    // Transform: translate answers one, apply takes one).
    LHAT_TEST("a host type that answers and takes itself installs");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "let^ h = store.make()\n"
             "let^ t = h.twin()\n"
             "return^ h.with(t).read()\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        LHAT_CHECK(held_tag != NULL, "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "store", "make",
                                      "f^ -> store.Held;", held_make, &held) &&
                       lhat_register_member(&program, "store", "Held", "read",
                                            "f^self^ -> number^;", held_read,
                                            NULL) &&
                       lhat_register_member(&program, "store", "Held", "twin",
                                            "f^self^ -> store.Held;",
                                            host_self, NULL) &&
                       // 13.13: the type a member is registered on is Self^
                       // in its signature, the long spelling beside it.
                       lhat_register_member(&program, "store", "Held", "with",
                                            "f^self^, Self^ -> store.Held;",
                                            host_self, NULL),
                   "the registrations took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LHAT_CHECK(lhat_program_install(&program, machine), "installed");
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.8: a registered type is its tag. Lowering a registration's
    // signature used to convert the type's members, and through them every
    // type they named -- five types naming each other eight ways held six
    // million nodes (a LOVE2D binding's physics). A nominal leaf holds one.
    LHAT_TEST("types naming each other install as a handful of nodes");
    {
        static const File files[] = {
            {"main.lh", "return^ 1\n"},
        };
        program_with(&program, &disk, files, 1);
        static const char *const names[] = {"A", "B", "C", "D", "E"};
        for (size_t t = 0; t < 5; t++) {
            LHAT_CHECK(lhat_register_hostdata_type(&program, "m", names[t]) !=
                           NULL,
                       "the type registered");
        }
        char member[8];
        char signature[64];
        for (size_t t = 0; t < 5; t++) {
            for (size_t i = 0; i < 8; i++) {
                snprintf(member, sizeof member, "m%zu", i);
                snprintf(signature, sizeof signature,
                         "p^self^, m.%s -> m.%s;", names[(t + i + 1) % 5],
                         names[(t + i + 2) % 5]);
                LHAT_CHECK(lhat_register_member(&program, "m", names[t],
                                                member, signature, host_self,
                                                NULL),
                           "the member registered");
            }
        }
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LHAT_CHECK(lhat_program_install(&program, machine), "installed");
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(ran.live < 1000, "live objects after install: %zu",
                       ran.live);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.7改2: a registered enum is 02 の 19 章's enum -- singleton
    // members with their integers, fits^ against the declaration, and a
    // when^ naming every member proving exhaustive.
    LHAT_TEST("a registered enum reads as a declared one");
    {
        static const File files[] = {
            {"main.lh",
             "import^ k\n"
             "var^ n = 0\n"
             "let^ x = k.Mode.Walk\n"
             "if^ x.value = 5 { n := n + 1 }\n"
             "if^ x = k.Mode.Walk { n := n + 10 }\n"
             "if^ x fits^ k.Mode { n := n + 100 }\n"
             "n := n + for^x:\n"
             "    when^ k.Mode.Idle: 1000\n"
             "    when^ k.Mode.Walk: 2000\n"
             ";\n"
             "return^ n\n"},
        };
        program_with(&program, &disk, files, 1);
        static const char *const modes[] = { "Idle", "Walk" };
        static const int64_t mode_values[] = { 1, 5 };
        LHAT_CHECK(lhat_register_enum_valued(&program, "k", "Mode", modes,
                                             mode_values, 2),
                   "the enum registered");
        LHAT_CHECK(!lhat_register_enum(&program, "k", "Mode", modes, 2),
                   "and its name is taken");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "and compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 2111);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.9改: a host value crosses the host->L^ boundary as an
    // argument. Two stand at once in the caller's own rooms, a narrow
    // argument rides between them, and the widened frame is what the body
    // reads.
    // 05 の 8.7改: a registered constant is the value itself -- typed by
    // the checker, installed once, read off the module, a hostdata type
    // and a hostvalue type alike (and off a value of the type: the type
    // table is the members table).
    LHAT_TEST("registered constants read as themselves");
    {
        static const File files[] = {
            {"main.lh",
             "import^ k\n"
             "var^ a : number^ = k.LIMIT\n"
             "var^ b : number^ = k.RATE\n"
             "var^ c : bool^ = k.ON\n"
             "var^ s : string^ = k.NAME\n"
             "var^ d : number^ = k.T.KIND\n"
             "var^ e : number^ = k.V.MODE\n"
             "return^ a * 100 + d * 10 + e\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_const_integer(&program, "k", NULL, "LIMIT",
                                               42),
                   "an integer registered");
        LHAT_CHECK(lhat_register_const_real(&program, "k", NULL, "RATE",
                                            1.5),
                   "a real registered");
        LHAT_CHECK(lhat_register_const_bool(&program, "k", NULL, "ON", true),
                   "a bool registered");
        LHAT_CHECK(lhat_register_const_string(&program, "k", NULL, "NAME",
                                              "lhat"),
                   "a string registered");
        LHAT_CHECK(lhat_register_hostdata_type(&program, "k", "T") != NULL,
                   "a hostdata type stood up");
        LHAT_CHECK(lhat_register_const_integer(&program, "k", "T", "KIND", 7),
                   "and took a static constant");
        LHAT_CHECK(lhat_register_hostvalue_type(&program, "k", "V", 8) !=
                       NULL,
                   "a hostvalue type stood up");
        LHAT_CHECK(lhat_register_const_integer(&program, "k", "V", "MODE", 3),
                   "and took one too");
        LHAT_CHECK(!lhat_register_const_integer(&program, "k", NULL, "LIMIT",
                                                9),
                   "a name is taken whole");
        LHAT_CHECK(!lhat_register_func(&program, "k", "LIMIT", "f^ -> number^;",
                                       host_wide_probe, NULL),
                   "and no function shares it");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "and compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 4273);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and writing over one is refused where the checker stands");
    {
        static const File files[] = {
            {"main.lh", "import^ k\nk.LIMIT := 1\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_const_integer(&program, "k", NULL, "LIMIT",
                                               42),
                   "registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_has_errors(&program),
                   "the write is refused");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("host value arguments cross the boundary whole");
    {
        static const File files[] = {
            {"main.lh",
             "import^ wide\n"
             "public^let^ f = f^a:wide.V, b:wide.V, k:number^ -> number^{\n"
             "    return^ wide.probe(a) * 100000 + wide.probe(b) * 100 + k\n"
             "}\n"
             "return^ f\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "wide", "V", sizeof(double));
        LHAT_CHECK(tag != NULL, "the type registered");
        lhat_register_func(&program, "wide", "probe", "f^wide.V -> number^;",
                           host_wide_probe, (void *)tag);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "and compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            double first = 7.0;
            double second = 9.0;
            LhatHostValueRoom rooms[2];
            LhatValue handed[3];
            LHAT_CHECK(lhat_place_hostvalue(tag, &first, &rooms[0], &handed[0]),
                       "the first placed");
            LHAT_CHECK(lhat_place_hostvalue(tag, &second, &rooms[1],
                                            &handed[1]),
                       "and the second beside it");
            handed[2] = lhat_integer(3);
            LhatRunResult called =
                lhat_machine_call(machine, ran.value, handed, 3);
            LHAT_CHECK_EQ_INT(called.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(called.value), 700903);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            LHAT_CHECK_EQ_INT(held.live, 0);  // the dispose^ ran
            lhat_machine_dispose(machine);
        }
#if LHAT_WITH_RESOLUTIONS
        // 8.8: what the name holds is this declaration, so what writes a type
        // out says the declaration. The registration above is the only place
        // the tag comes from, which is why this is asked here rather than of
        // a type built by hand.
        if (root != NULL) {
            const char *use = strstr(root->source.text, "h.read()");
            const LhatResolution *r =
                use != NULL
                    ? lhat_check_resolution_at(
                          &root->checked, (uint32_t)(use - root->source.text))
                    : NULL;
            LHAT_CHECK(r != NULL && r->type != NULL, "expected h to resolve");
            if (r != NULL && r->type != NULL) {
                char written[64];
                size_t length = lhat_type_write_full(r->type, written,
                                                     sizeof written);
                LHAT_CHECK_EQ_STR(written, length, "store.Held");
            }
        }
#endif
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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

static void cell_make(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    int *cell = (int *)malloc(sizeof *cell);
    if (cell == NULL) {
        return;
    }
    *cell = 1;
    cells_live++;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, cell_tag, cell, &out)) {
        free(cell);
        cells_live--;
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static void cell_release(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    int *cell = (int *)lhat_hostdata_pointer(arguments[0], cell_tag);
    if (cell != NULL) {
        free(cell);
        cells_live--;
        cells_freed++;
    }
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
    bool compiled = lhat_program_compile(&program);
    if (compiled && root != NULL) {
        LhatMachine *machine = lhat_machine_new();
        lhat_program_install(&program, machine);
        lhat_run(machine, lhat_unit_proto(root));
        *live_after_run = cells_live;
        lhat_machine_dispose(machine);
    }
    *freed_total = cells_freed;
    lhat_program_dispose(&program);
}

// Check, compile, install, and run main.lh, as test_host_coroutine does.
static LhatRunResult run_main(LhatProgram *program)
{
    LhatRunResult failed;
    memset(&failed, 0, sizeof failed);
    failed.status = LHAT_RUN_TYPE_ERROR;
    const LhatUnit *root = lhat_program_check(program, "main.lh");
    LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
               "the program checked");
    if (!lhat_program_compile(program) || root == NULL) {
        LHAT_CHECK(false, "the program compiled");
        return failed;
    }
    LhatMachine *machine = lhat_machine_new();
    lhat_program_install(program, machine);
    LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
    lhat_machine_dispose(machine);
    return ran;
}

// 05 の 8.8: a hostdata stands for what the host made, so two wrappers of
// one host object are equal and key one table entry between them. is^ still
// asks for the wrapper itself, and a released wrapper is equal only to
// itself -- its pointer may already name something else.
// 05 の 5.7: an invalidation takes back what a unit was made into and
// nothing else. What the host registered has to come through untouched --
// 7.3 makes the tag's address the identity, so a value made after a reload
// has to be the same type as one made before it.

// 05 の 8.7 with 7.3: what a declaration is, is the C call that makes it --
// so making it twice, from two programs, comes away with one identity. The
// run time compares tags by address, and a second tag would make one host
// type into two that agree about everything except the one thing that
// decides. This is the root of what a program per script ran into.
static void test_declarations_are_the_process_s(void)
{
    LhatProgram a;
    LhatProgram b;
    Disk disk_a;
    Disk disk_b;
    static const File none[] = {{"main.lh", "return^ 1\n"}};

    LHAT_TEST("two programs declaring one type come away with one tag");
    {
        program_with(&a, &disk_a, none, 1);
        program_with(&b, &disk_b, none, 1);
        const LhatHostDataTag *first =
            lhat_register_hostdata_type(&a, "shared", "Thing");
        const LhatHostDataTag *second =
            lhat_register_hostdata_type(&b, "shared", "Thing");
        LHAT_CHECK(first != NULL, "the first declaration made one");
        LHAT_CHECK_EQ_PTR(second, first);

        // 8.7 still refuses a second one inside ONE program: that is a name
        // used twice, not a declaration made twice.
        LHAT_CHECK_EQ_PTR(lhat_register_hostdata_type(&a, "shared", "Thing"),
                          NULL);
    }
    lhat_program_dispose(&a);
    lhat_program_dispose(&b);

    LHAT_TEST("and one error declaration makes one set of kinds");
    {
        program_with(&a, &disk_a, none, 1);
        program_with(&b, &disk_b, none, 1);
        static const char *const variants[] = {"Torn", "Bent"};
        const LhatErrorKind *group_a = NULL;
        const LhatErrorKind *group_b = NULL;
        const LhatErrorKind *kinds_a[2];
        const LhatErrorKind *kinds_b[2];
        LHAT_CHECK(lhat_register_error_kind(&a, "shared", "Broken", variants,
                                            2, &group_a, kinds_a),
                   "the first declaration");
        LHAT_CHECK(lhat_register_error_kind(&b, "shared", "Broken", variants,
                                            2, &group_b, kinds_b),
                   "and the second is the same one made again");
        LHAT_CHECK_EQ_PTR(group_b, group_a);
        LHAT_CHECK_EQ_PTR(kinds_b[0], kinds_a[0]);
        LHAT_CHECK_EQ_PTR(kinds_b[1], kinds_a[1]);

        // 04 の 2.4: two lists are two declarations, and one name cannot be
        // both.
        static const char *const others[] = {"Torn"};
        LHAT_CHECK(!lhat_register_error_kind(&b, "shared", "Broken", others, 1,
                                             NULL, NULL),
                   "a different list under the same name is refused");
    }
    lhat_program_dispose(&a);
    lhat_program_dispose(&b);

    // 8.9: the same for a host value type, where the width is what every
    // frame holding one was laid out against -- so a second declaration at
    // another size is not the same type and cannot be allowed to look like
    // it.
    LHAT_TEST("a host value type is one declaration too, at one size");
    {
        program_with(&a, &disk_a, none, 1);
        program_with(&b, &disk_b, none, 1);
        const LhatHostValueTag *first =
            lhat_register_hostvalue_type(&a, "shared", "Pair", 8);
        const LhatHostValueTag *second =
            lhat_register_hostvalue_type(&b, "shared", "Pair", 8);
        LHAT_CHECK(first != NULL, "the first declaration made one");
        LHAT_CHECK_EQ_PTR(second, first);
        LHAT_CHECK_EQ_INT(second->index, first->index);
        LHAT_CHECK_EQ_INT(second->width, first->width);

        LhatProgram c;
        Disk disk_c;
        program_with(&c, &disk_c, none, 1);
        LHAT_CHECK_EQ_PTR(
            lhat_register_hostvalue_type(&c, "shared", "Pair", 16), NULL);
        lhat_program_dispose(&c);

        // A field declared twice is the same field; one that disagrees is
        // refused.
        LHAT_CHECK(lhat_register_hostvalue_field(&a, "shared", "Pair", "x", 0,
                                                 LHAT_HVFIELD_F32),
                   "the field declared");
        LHAT_CHECK(lhat_register_hostvalue_field(&b, "shared", "Pair", "x", 0,
                                                 LHAT_HVFIELD_F32),
                   "and declared again, the same way");
        LHAT_CHECK(!lhat_register_hostvalue_field(&b, "shared", "Pair", "x", 4,
                                                  LHAT_HVFIELD_F32),
                   "the same name at another offset is two fields, refused");
    }
    lhat_program_dispose(&a);
    lhat_program_dispose(&b);

    // 8.9 with registry.h: a tag's index is the process's, so a program that
    // declares only the later of two types has an index past its own count.
    // The machine's array of members tables is taken to the width the
    // indices reach, not to how many this program declared.
    LHAT_TEST("a program declaring only the later type still installs");
    {
        program_with(&a, &disk_a, none, 1);
        LHAT_CHECK(lhat_register_hostvalue_type(&a, "gap", "First", 8) != NULL,
                   "the first type, declared by this program alone");
        lhat_program_dispose(&a);

        program_with(&b, &disk_b, none, 1);
        const LhatHostValueTag *later =
            lhat_register_hostvalue_type(&b, "gap", "Second", 8);
        LHAT_CHECK(later != NULL && later->index > 0,
                   "the second type's index is past this program's count");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(machine != NULL && lhat_program_install(&b, machine),
                   "and the install still finds room for it");
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&b);
}

static void test_reloading_keeps_registrations(void)
{
    LhatProgram program;
    Disk disk;
    Held held = {42, 0};

    LHAT_TEST("what the host registered survives an invalidation");
    {
        static File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ a = store.make()\n"
             "return^ a.read()\n"},
        };
        files[0].text =
            "import^ store\n"
            "var^ a = store.make()\n"
            "return^ a.read()\n";
        program_with(&program, &disk, files, 1);
        const LhatHostDataTag *tag =
            lhat_register_hostdata_type(&program, "store", "Held");
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);
        held_tag = tag;

        LhatRunResult ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);

        files[0].text =
            "import^ store\n"
            "var^ a = store.make()\n"
            "var^ b = store.make()\n"
            "return^ a.read() + b.read() - 42\n";
        LHAT_CHECK_EQ_INT(lhat_program_invalidate(&program, "main.lh"), 1);

        // 8.7: one name, one thing -- the registration is still standing, so
        // a second one of the same name is still refused.
        LHAT_CHECK_EQ_PTR(lhat_register_hostdata_type(&program, "store",
                                                      "Held"),
                          NULL);
        lhat_program_discard_retired(&program);

        // And the unit reads back into that same world: the tag a value is
        // made with now is the one it was made with before.
        ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        LHAT_CHECK_EQ_PTR(held_tag, tag);
    }
    lhat_program_dispose(&program);
}

static void test_host_data_identity(void)
{
    LhatProgram program;
    Disk disk;
    Held held = {42, 0};

    LHAT_TEST("two wrappers of one host object are equal");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ a = store.make()\n"
             "var^ b = store.make()\n"
             "var^ t = { [a] = 1 }\n"
             "var^ r = 0\n"
             "if^ a = b { r := r + 1 }\n"
             "if^ a is^ b { r := r + 10 }\n"
             "if^ t[b] = 1 { r := r + 100 }\n"
             "return^ r\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);
        LhatRunResult ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // Equal, not the same wrapper, and one key between them.
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 101);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a released wrapper is equal only to itself");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ a = store.make()\n"
             "var^ b = store.make()\n"
             "a.dispose()\n"
             "var^ r = 0\n"
             "if^ a = b { r := r + 1 }\n"
             "if^ a = a { r := r + 10 }\n"
             "return^ r\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_member(&program, "store", "Held", "dispose", "p^self^;",
                             held_dispose, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);
        LhatRunResult ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 10);
    }
    lhat_program_dispose(&program);

    // 14.12 with 8.8: two registrations are disjoint, so where the checker
    // knows both it settles the comparison instead of leaving it to the run
    // (chk_infer's "these can never be equal"). The values meet through any^
    // here, which is where 7.3's rule is what answers.
    LHAT_TEST("wrappers of two types never equal, whatever the pointer");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "var^ h : any^ = store.make()\n"
             "var^ o : any^ = store.makeSame()\n"
             "var^ r = 0\n"
             "if^ h = o { r := r + 1 }\n"
             "return^ r\n"},
        };
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        other_tag = lhat_register_hostdata_type(&program, "store", "Other");
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_member(&program, "store", "Other", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);
        // The same pointer under another tag: 7.3 keeps them apart.
        lhat_register_func(&program, "store", "makeSame", "f^ -> store.Other;",
                           other_make_same, &held);
        LhatRunResult ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 0);
    }
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), -7);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.7改2: a host function asks for a panic and returns; the run
    // ends at the call with the host's value, the traceback standing there.
    LHAT_TEST("a host function's panic ends the run at the call, with its message");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "let^ draw = f^ w:number^ -> number^ {\n"
             "    let^ r = test.c.refuse(w)\n"  // no tail call: the frame stays
             "    return^ r\n"
             "}\n"
             "let^ got = draw(-1)\n"
             "return^ got\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "test.c", "refuse",
                                      "f^number^ -> number^;", host_refuse,
                                      NULL),
                   "the registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
            LHAT_CHECK(lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING),
                       "the message is the value");
            if (lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING)) {
                const LhatString *s =
                    (const LhatString *)lhat_as_object(ran.value);
                LHAT_CHECK(strcmp(s->text, "width must not be negative") == 0,
                           "got %s", s->text);
            }
            LHAT_CHECK_EQ_INT(ran.line, 3);
            // The frames stand: draw's frame at the call, the unit's below.
            LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(machine), 2);
            LhatFrameInfo info;
            LHAT_CHECK(lhat_machine_fault_frame(machine, 0, &info) &&
                           info.line == 3 && !info.top_level,
                       "the top frame is the call to the host");
            LHAT_CHECK(lhat_machine_fault_frame(machine, 1, &info) &&
                           info.line == 6 && info.top_level,
                       "and the unit's frame is under it");
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and so does a panic from a host value's operator");
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
        LHAT_CHECK(lhat_register_func(&program, "test.c", "make",
                                      "f^ -> test.c.C;", host_counter_make,
                                      (void *)tag),
                   "the maker registration took");
        LHAT_CHECK(lhat_register_hostvalue_member(&program, "test.c", "C", "-",
                                                  "f^self^ -> number^;",
                                                  host_counter_refuse,
                                                  (void *)tag),
                   "the operator registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
            LHAT_CHECK_EQ_INT(ran.line, 3);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // And across the host boundary itself: a host calling a host function
    // through lhat_machine_call gets the panic as the call's result.
    LHAT_TEST("and a host calling the host function gets the panic back");
    {
        static const File files[] = {
            {"main.lh", "import^ test.c\nreturn^ test.c.refuse\n"},
        };
        program_with(&program, &disk, files, 1);
        lhat_register_func(&program, "test.c", "refuse", "f^number^ -> number^;",
                           host_refuse, NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LhatValue argument = lhat_integer(-1);
            LhatRunResult called =
                lhat_machine_call(machine, ran.value, &argument, 1);
            LHAT_CHECK_EQ_INT(called.status, LHAT_RUN_PANIC);
            LHAT_CHECK(lhat_is_object_kind(called.value, LHAT_OBJECT_STRING),
                       "the message came across the boundary");
            // No frame of the machine's was involved, so the span is empty.
            LHAT_CHECK_EQ_INT(lhat_machine_fault_depth(machine), 0);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.9改: a host value stands in a union whose other arms the head
    // slot's tag tells apart, so a registration may answer 'T|nil^'. The
    // width is reserved either way; the nil^ writes the head and leaves the
    // slots behind it as they were, which is exactly what ISNIL reads.
    LHAT_TEST("a host value may be answered beside a nil^");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ some = test.c.maybe(1)\n"
             "var^ none = test.c.maybe(0)\n"
             "var^ seen = 0\n"
             "if^ some? { seen := seen + 1 }\n"
             "if^ none? { seen := seen + 10 }\n"
             "return^ seen\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        LHAT_CHECK(tag != NULL, "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "test.c", "maybe",
                                      "f^number^ -> test.c.C|nil^;",
                                      host_counter_maybe, (void *)tag),
                   "a union answer registers");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // Only the value arm was present.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 7000 + 500 + 170 + 99 - 99
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7670);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 13.7 with 14.12: a variadic arm is told apart from another exactly
    // where the machine's search can tell them apart -- a written position
    // the two disagree on, or an argument one arm has no place for. The
    // tail itself compares nothing (a LOVE2D binding's newSoundData(path)
    // beside newSoundData(samples, ...)).
    // 05 の 8.8 with 14.12: two registered types are two declarations, so
    // one arm may take each -- even where the two carry the same member
    // names, which is all a structural reading would have to go on (a
    // LOVE2D binding's File and FileData both answer getSize).
    LHAT_TEST("arms taking two registered types stand beside each other");
    {
        static const File files[] = {
            {"main.lh",
             "import^ store\n"
             "return^ store.take(store.make()) * 10 + "
             "store.take(store.makeOther())\n"},
        };
        static Held held = {7, 0};
        program_with(&program, &disk, files, 1);
        held_tag = lhat_register_hostdata_type(&program, "store", "Held");
        other_tag = lhat_register_hostdata_type(&program, "store", "Other");
        // The same member names on both -- what made the two overlap.
        lhat_register_member(&program, "store", "Held", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_member(&program, "store", "Other", "read",
                             "f^self^ -> number^;", held_read, NULL);
        lhat_register_func(&program, "store", "make", "f^ -> store.Held;",
                           held_make, &held);
        lhat_register_func(&program, "store", "makeOther",
                           "f^ -> store.Other;", other_make, &held);
        LHAT_CHECK(lhat_register_func(&program, "store", "take",
                                      "f^store.Held -> number^;", host_one,
                                      NULL),
                   "the first arm registered");
        LHAT_CHECK(lhat_register_func(&program, "store", "take",
                                      "f^store.Other -> number^;", host_two,
                                      NULL),
                   "and the arm taking the other type beside it");
        LHAT_CHECK(!lhat_register_func(&program, "store", "take",
                                       "f^store.Held -> number^;", host_one,
                                       NULL),
                   "while the same type twice is still refused");
        LhatRunResult ran = run_main(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // take(Held) ran host_one and take(Other) host_two: the arms are
        // told apart by the tag, at check time and at run time.
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 12);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a variadic arm stands beside one it parts from at a written position");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "return^ test.c.pick(\"s\") * 100 + test.c.pick(1, 2, 3) * 10 +\n"
             "       test.c.pick(7)\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_func(&program, "test.c", "pick",
                                      "f^string^ -> number^;", host_one,
                                      NULL),
                   "the string arm registered");
        LHAT_CHECK(lhat_register_func(&program, "test.c", "pick",
                                      "f^number^, number^, ...:number^ -> "
                                      "number^;",
                                      host_sum, NULL),
                   "the number arm with a tail registered beside it");
        LHAT_CHECK(lhat_register_func(&program, "test.c", "pick",
                                      "f^number^ -> number^;", host_twice,
                                      NULL),
                   "and one number alone, which the tailed arm's count "
                   "does not reach");
        LHAT_CHECK(!lhat_register_func(&program, "test.c", "pick",
                                       "f^number^, number^, string^, "
                                       "...:string^ -> number^;",
                                       host_sum, NULL),
                   "but a call of two numbers would fit this and the tailed "
                   "arm both");
        LHAT_CHECK(!lhat_register_func(&program, "test.c", "pick",
                                       "f^ ...:string^ -> number^;", host_sum,
                                       NULL),
                   "and a bare tail takes the one-string call too");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 1 * 100 + (1+2+3) * 10 + 7 * 2
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 100 + 60 + 14);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.9 with 02 の 11.9改: the bytes are what a host value answers
    // when nothing was written, and a written op^= is not nothing. These two
    // are byte for byte the same, so the registration is the only thing that
    // can make '=' say no.
    LHAT_TEST("a registered op^= decides a host value's equality");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ a = test.c.make()\n"
             "var^ b = test.c.make()\n"
             "if^ a = b { return^ 1 }\n"
             "if^ a \xE2\x89\xA0 b { return^ 2 }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        lhat_register_func(&program, "test.c", "make", "f^ -> test.c.C;",
                           host_counter_make, (void *)tag);
        LHAT_CHECK(lhat_register_hostvalue_member(
                       &program, "test.c", "C", "=",
                       "f^self^, test.c.C -> bool^;", host_counter_never_equal,
                       (void *)tag),
                   "the equality registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 2);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // And with nothing written, 8.9's own answer stands -- the same two
    // values, the same question, the other way round.
    LHAT_TEST("and without one the bytes still answer");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.c\n"
             "var^ a = test.c.make()\n"
             "var^ b = test.c.make()\n"
             "if^ a = b { return^ 1 }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        const LhatHostValueTag *tag = lhat_register_hostvalue_type(
            &program, "test.c", "C", sizeof(Counter));
        lhat_register_func(&program, "test.c", "make", "f^ -> test.c.C;",
                           host_counter_make, (void *)tag);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 149);  // 5 * 10 + 99
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);
}

// 02 の 13.8改: a host answering several values. The positions go into the
// machine's room and come back as the run every other producer makes.
static void host_divmod(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    if (count != 2 || !lhat_is_integer(arguments[0]) ||
        !lhat_is_integer(arguments[1]) ||
        lhat_as_integer(arguments[1]) == 0) {
        return;
    }
    (void)machine;
    int64_t a = lhat_as_integer(arguments[0]);
    int64_t b = lhat_as_integer(arguments[1]);
    // 02 の 13.8改: two answers, written where the machine handed the room.
    answers[0] = lhat_integer(a / b);
    answers[1] = lhat_integer(a % b);
    *answer_count = 2;
}

// 13.7 with 13.8改: the host arm gathers its own arguments, so a tuple spread
// into a variadic tail reaches it as ordinary arguments and nothing else.
static void host_sum(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    int64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (!lhat_is_integer(arguments[i])) {
            return;
        }
        total += lhat_as_integer(arguments[i]);
    }
    answers[0] = lhat_integer(total);
    *answer_count = 1;
}

// The same, but allocating after the room is filled -- what proves the
// positions are roots. A table made here would be swept if they were not.
static void host_divmod_then_allocate(LhatMachine *machine, void *context,
                                      const LhatValue *arguments, size_t count,
                                      LhatValue *answers, int *answer_count)
{
    (void)answers;
    host_divmod(machine, context, arguments, count, answers,
                answer_count);
    if (*answer_count == 0) {
        return;
    }
    // The room is a root, so what was written into it survives this.
    for (int i = 0; i < 64; i++) {
        LhatValue dropped = lhat_nil();
        lhat_machine_make_table(machine, &dropped);  // dropped at once
    }
}

// 05 の 8.7 with 13.8改: registered as answering several values but written
// to answer one. The two sides can only disagree by being built apart, and
// the machine is what catches it.
static void host_answers_one(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count,
                             LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    answers[0] = lhat_integer(7);
    *answer_count = 1;
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LHAT_CHECK(lhat_program_install(&program, machine), "installed");
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
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
        bool compiled = lhat_program_compile(&program);
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 31);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 8.9 with 13.8改: a position is one slot, and a host value is as
    // wide as its tag says. The signature registers -- it only describes
    // what the host answers -- and the refusal lands where L^ takes the
    // tuple apart, in the reader's own source.
    LHAT_TEST("a host value written as a position refuses at the use");
    {
        static const File files[] = {
            {"main.lh",
             "import^ test.w\n"
             "var^ a, b = test.w.bad()\n"
             "return^ 1\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostvalue_type(&program, "test.w", "W", 8) !=
                       NULL,
                   "the type registration took");
        LHAT_CHECK(lhat_register_func(&program, "test.w", "bad",
                                      "f^ -> (number^, test.w.W);",
                                      host_answers_one, NULL),
                   "the signature registration took");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES),
                   "the escape is refused where the tuple is taken apart");
    }
    lhat_program_dispose(&program);
}

// One registration of every kind, written back out: what the dump carries
// is what a reader (lhatls's host_config.c) re-plays against a program of
// its own, so each shape has to appear, spelled the way the matching
// lhat_register_* takes it back.
static void test_dump_host_api(void)
{
    LhatProgram program;
    Disk disk;
    int calls = 0;

    LHAT_TEST("the registrations dump as lhat-host.json");
    program_with(&program, &disk, NULL, 0);

    static const char *const variants[] = {"NotFound", "Eof"};
    LHAT_CHECK(lhat_register_error_kind(&program, "sys.io", "IOError",
                                        variants, 2, NULL, NULL),
               "the error kind registered");
    LHAT_CHECK(lhat_register_hostdata_type(&program, "sys.io", "File") != NULL,
               "the hostdata type registered");
    // 05 の 8.8改: and one under it. A host whose model is a class tree
    // writes the tree, and what the derived type inherits is reached through
    // this relation alone -- so a dump that leaves it out hands a reader
    // types that are not the ones registered here.
    LHAT_CHECK(lhat_register_hostdata_subtype(&program, "sys.io", "Socket",
                                              "sys.io", "File") != NULL,
               "the derived hostdata type registered");
    LHAT_CHECK(lhat_register_hostvalue_type(&program, "sys.geo", "Vec2",
                                            8) != NULL,
               "the hostvalue type registered");
    LHAT_CHECK(lhat_register_hostvalue_field(&program, "sys.geo", "Vec2", "x",
                                             0, LHAT_HVFIELD_F32),
               "the field registered");
    LHAT_CHECK(lhat_register_func(&program, "sys.io", "open",
                                  "f^string^ -> sys.io.File|sys.io.IOError.NotFound;",
                                  host_add, &calls),
               "the func registered");
    LHAT_CHECK(lhat_register_member(&program, "sys.io", "File", "close",
                                    "p^self^;", host_add, &calls),
               "the member registered");
    LHAT_CHECK(lhat_register_hostvalue_member(&program, "sys.geo", "Vec2",
                                              "+",
                                              "f^self^, sys.geo.Vec2 -> sys.geo.Vec2;",
                                              host_add, &calls),
               "the hostvalue member registered");
    LHAT_CHECK(lhat_register_global(&program, "twice",
                                    "f^number^ -> number^;", host_twice,
                                    &calls),
               "the global registered");
    LHAT_CHECK(lhat_bind_initial(&program, "twice", "L^.twice"),
               "the binding took");
    // 18.5's places go out by name rather than as the mask they arrive as:
    // the reader holds no C enum to read one against. Two are set here so
    // that the shape between them is pinned as well as the names.
    LHAT_CHECK(lhat_register_annotation(&program, "h", "badge",
                                        LHAT_ANNOTATION_FIELD |
                                            LHAT_ANNOTATION_PUBLIC |
                                            LHAT_ANNOTATION_FILEUNIQUE),
               "the annotation registered");
    // One place alone, and one that is a place no other test writes.
    LHAT_CHECK(lhat_register_annotation(&program, "h", "atop",
                                        LHAT_ANNOTATION_UNIT),
               "the unit annotation registered");

    size_t needed = lhat_program_dump_host_api(&program, NULL, 0);
    LHAT_CHECK(needed > 0, "measuring answered a size");
    char *text = (char *)malloc(needed + 1);
    if (text != NULL) {
        size_t written = lhat_program_dump_host_api(&program, text, needed + 1);
        LHAT_CHECK_EQ_INT(written, needed);

        static const char *const expected[] = {
            "{\n  \"strict\": true,\n  \"types\"",
            "{\"kind\": \"errordef\", \"module\": \"sys.io\", \"name\": "
            "\"IOError\", \"variants\": [\"NotFound\", \"Eof\"]}",
            "{\"kind\": \"hostdata\", \"module\": \"sys.io\", \"name\": "
            "\"File\"}",
            "{\"kind\": \"hostdata\", \"module\": \"sys.io\", \"name\": "
            "\"Socket\", \"base_module\": \"sys.io\", \"base_name\": "
            "\"File\"}",
            "{\"kind\": \"hostvalue\", \"module\": \"sys.geo\", \"name\": "
            "\"Vec2\", \"size\": 8, \"fields\": [{\"name\": \"x\", "
            "\"offset\": 0, \"type\": \"f32\"}]}",
            "{\"kind\": \"func\", \"module\": \"sys.io\", \"name\": \"open\", "
            "\"signature\": \"f^string^ -> sys.io.File|sys.io.IOError.NotFound;\"}",
            "{\"kind\": \"member\", \"module\": \"sys.io\", \"type\": "
            "\"File\", \"name\": \"close\", \"signature\": \"p^self^;\"}",
            "{\"kind\": \"hostvalue_member\", \"module\": \"sys.geo\", "
            "\"type\": \"Vec2\", \"name\": \"+\", \"signature\": "
            "\"f^self^, sys.geo.Vec2 -> sys.geo.Vec2;\"}",
            "{\"kind\": \"global\", \"name\": \"twice\", \"signature\": "
            "\"f^number^ -> number^;\"}",
            "{\"module\": \"h\", \"name\": \"badge\", \"targets\": "
            "{\"field\": true, \"public\": true, \"fileunique\": true}}",
            "{\"module\": \"h\", \"name\": \"atop\", \"targets\": "
            "{\"unit\": true}}",
            "{\"name\": \"twice\", \"member\": \"L^.twice\"}",
        };
        for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
            LHAT_CHECK(strstr(text, expected[i]) != NULL,
                       "the dump carries: %s", expected[i]);
        }

        // The two phases hold: every type declaration is written before the
        // first signature, so a reader registering in file order never meets
        // a name it has not seen.
        const char *functions = strstr(text, "\"functions\"");
        const char *hostvalue = strstr(text, "\"kind\": \"hostvalue\"");
        LHAT_CHECK(functions != NULL && hostvalue != NULL &&
                       hostvalue < functions,
                   "types come before functions");
        free(text);
    }
    lhat_program_dispose(&program);

    // 03 の 3.1: the dump names the value this program was made with, not
    // always true -- lsp/host_config.c reads it back for lsp/diagnostics.c.
    LHAT_TEST("and it names a relaxed program too");
    {
        Disk relaxed_disk;
        relaxed_disk.files = NULL;
        relaxed_disk.count = 0;
        relaxed_disk.reads = 0;
        LhatProgram relaxed;
        lhat_program_init(&relaxed, false, disk_load, &relaxed_disk);
        size_t bytes = lhat_program_dump_host_api(&relaxed, NULL, 0);
        char *dumped = (char *)malloc(bytes + 1);
        if (dumped != NULL) {
            lhat_program_dump_host_api(&relaxed, dumped, bytes + 1);
            LHAT_CHECK(strstr(dumped, "\"strict\": false") != NULL,
                       "got %s", dumped);
            free(dumped);
        }
        lhat_program_dispose(&relaxed);
    }
}

// What the stages reported, over the graph rather than over one file --
// program.h's own reading of it, which is what a host outside this tree has.
static void test_diagnostics(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a clean graph has nothing to report");
    {
        static const File clean[] = {
            {"lib/one.lh", "public^ let^ v = 1\n"},
            {"main.lh", "var^ o = require^ \"lib/one.lh\"\nvar^ n = o.v\n"},
        };
        program_with(&program, &disk, clean, 2);
        lhat_program_check(&program, "main.lh");
        size_t total = 0;
        for (const LhatUnit *u = lhat_program_units(&program); u != NULL;
             u = lhat_unit_next(u)) {
            total += lhat_unit_diagnostic_count(u);
            LHAT_CHECK(lhat_unit_source(u) != NULL, "the source is reachable");
        }
        LHAT_CHECK_EQ_INT(total, 0);
    }
    lhat_program_dispose(&program);

    // 6.2: what a host shows is the graph's, so a mistake in a required unit
    // is found by walking rather than by asking the unit that was named.
    LHAT_TEST("a required unit's own mistake is found by the walk");
    {
        static const File broken[] = {
            {"lib/one.lh", "public^ let^ v : number^ = \"text\"\n"},
            {"main.lh", "var^ o = require^ \"lib/one.lh\"\n"},
        };
        program_with(&program, &disk, broken, 2);
        lhat_program_check(&program, "main.lh");

        size_t found = 0;
        for (const LhatUnit *u = lhat_program_units(&program); u != NULL;
             u = lhat_unit_next(u)) {
            size_t said = lhat_unit_diagnostic_count(u);
            for (size_t i = 0; i < said; i++) {
                found++;
                LhatUnitDiagnostic d = lhat_unit_diagnostic(u, i);
                LHAT_CHECK_EQ_INT(d.stage, LHAT_STAGE_CHECKER);
                LHAT_CHECK(d.line == 1, "on the line it was written");

                char message[256];
                size_t needed =
                    lhat_unit_diagnostic_message(u, i, message, sizeof message);
                LHAT_CHECK(needed > 0 && needed < sizeof message,
                           "the message is written and fits");

                // The line a driver writes names the unit it was in, which
                // is the whole reason the walk is over units.
                char line[512];
                size_t whole =
                    lhat_unit_diagnostic_write(u, i, false, line, sizeof line);
                LHAT_CHECK(whole > needed, "the line says more than the message");
                LHAT_CHECK(strstr(line, "lib/one.lh") != NULL,
                           "and says which unit: %s", line);
                LHAT_CHECK(strstr(line, message) != NULL,
                           "and carries the message: %s", line);
            }
        }
        LHAT_CHECK_EQ_INT(found, 1);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("the lexer and the parser answer through the same reading");
    {
        static const File wrong[] = {{"main.lh", "var^ x = (1 + \n"}};
        program_with(&program, &disk, wrong, 1);
        lhat_program_check(&program, "main.lh");

        const LhatUnit *unit = lhat_program_units(&program);
        LHAT_CHECK(unit != NULL, "the unit is there");
        size_t said = unit != NULL ? lhat_unit_diagnostic_count(unit) : 0;
        LHAT_CHECK(said > 0, "and reported something");
        for (size_t i = 0; i < said; i++) {
            LhatUnitDiagnostic d = lhat_unit_diagnostic(unit, i);
            LHAT_CHECK(d.stage == LHAT_STAGE_LEXER ||
                           d.stage == LHAT_STAGE_PARSER,
                       "before the checker ever ran");
        }
    }
    lhat_program_dispose(&program);

    // Measuring is a call with (NULL, 0), the same as everywhere else that
    // fills a buffer -- and past the end there is nothing to measure.
    LHAT_TEST("measuring and overrunning follow the usual convention");
    {
        static const File broken[] = {
            {"main.lh", "var^ v : number^ = \"text\"\n"}};
        program_with(&program, &disk, broken, 1);
        lhat_program_check(&program, "main.lh");

        const LhatUnit *unit = lhat_program_units(&program);
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(unit), 1);

        size_t wanted = lhat_unit_diagnostic_message(unit, 0, NULL, 0);
        char message[256];
        size_t written =
            lhat_unit_diagnostic_message(unit, 0, message, sizeof message);
        LHAT_CHECK_EQ_INT(written, wanted);
        LHAT_CHECK_EQ_INT(strlen(message), wanted);

        char tiny[4];
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_message(unit, 0, tiny,
                                                       sizeof tiny),
                          wanted);
        LHAT_CHECK(strlen(tiny) == 3, "and fills what it was given");

        LhatUnitDiagnostic none = lhat_unit_diagnostic(unit, 1);
        LHAT_CHECK(none.offset == 0 && none.line == 0 && none.length == 0,
                   "past the end is zeroed");
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_message(unit, 1, NULL, 0), 0);
    }
    lhat_program_dispose(&program);

    // A unit that was never read has no positions to index and no stage that
    // ever ran; what stopped it is one of the program's own diagnostics.
    LHAT_TEST("a unit that could not be read reports through the program");
    {
        static const File missing[] = {
            {"main.lh", "var^ o = require^ \"lib/gone.lh\"\n"}};
        program_with(&program, &disk, missing, 1);
        lhat_program_check(&program, "main.lh");

        LHAT_CHECK(lhat_program_diagnostic_count(&program) > 0,
                   "the program says so");
        for (const LhatUnit *u = lhat_program_units(&program); u != NULL;
             u = lhat_unit_next(u)) {
            if (lhat_unit_source(u) == NULL) {
                LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(u), 0);
            }
        }
    }
    lhat_program_dispose(&program);

    // 03 の 3.1: exactly the three strict-only gap codes answer true --
    // an ordinary mismatch, found the same way under both, answers false.
    LHAT_TEST("lhat_unit_diagnostic_relaxed_ok answers only the strict-only gaps");
    {
        static const File gap[] = {
            {"main.lh", "let^ f = f^ n -> number^ { return^ 1 }\n"
                        "return^ f(1)\n"},
        };
        program_with(&program, &disk, gap, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(root), 1);
        LHAT_CHECK(lhat_unit_diagnostic_relaxed_ok(root, 0),
                   "an unannotated parameter with no requirement on it");
    }
    lhat_program_dispose(&program);
    {
        static const File mismatch[] = {
            {"main.lh", "var^ x : number^ = \"s\"\n"},
        };
        program_with(&program, &disk, mismatch, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK_EQ_INT(lhat_unit_diagnostic_count(root), 1);
        LHAT_CHECK(!lhat_unit_diagnostic_relaxed_ok(root, 0),
                   "a real mismatch is not one of the three");
    }
    lhat_program_dispose(&program);
    LHAT_CHECK(!lhat_unit_diagnostic_relaxed_ok(NULL, 0), "no unit answers false");
}

// 02 の 14.2 と 05 の 5.3: composing onto a definition another unit published.
// 14.2 fixes the chain where it is written, so this is a compile-time
// flattening like any other -- what crosses the boundary is the tree, and
// never a value.
static void test_composing_across_units(void)
{
    LhatProgram program;
    Disk disk;

    static const File files[] = {
        {"lib.lh",
         "module^ ns.lib\n"
         "public^ let^ Base = def^{\n"
         "  self^{ abstract^ n : number^ },\n"
         "  get = f^self^ -> number^ { return^ self^.n },\n"
         "}\n"
         "public^ let^ Middle = Base .. def^{\n"
         "  self^{},\n"
         "  twice = f^self^ -> number^ { return^ self^.get() * 2 },\n"
         "}\n"
         "let^ Hidden = def^{ self^{}, no = f^self^ -> number^ { return^ 0 } }\n"},
        {"main.lh",
         "let^ lib = require^ \"lib.lh\"\n"
         "let^ Mine = lib.Middle .. def^{\n"
         "  self^{ n = 21 },\n"
         "  own = f^self^ -> number^ { return^ 1 },\n"
         "}\n"
         "let^ takes = f^ x:lib.Base -> number^ { return^ x.get() }\n"
         "let^ m = Mine.new()\n"
         "return^ m.twice() + m.own() + takes(m)\n"},
    };

    LHAT_TEST("a definition another unit published is composed onto");
    {
        program_with(&program, &disk, files, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "every unit compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 42 through the base's own member, 1 of its own, and 21 again
            // through a parameter typed as the base -- 14.10's width
            // subtyping is what lets the composed one land there.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 64);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 02 の 14.7改2 with 03 の 4.3: the delegate the base declared, when the
    // base was written in another unit. The chain crosses (def_chain_across),
    // so the entry is found -- but its spelling is that unit's, and reading
    // it against this one's text names whatever happens to sit at the
    // offset. The checker had no such split and took the program, which is
    // 4.2's disagreement: it checked and would not run.
    LHAT_TEST("14.7改2: a delegate the base declared crosses units");
    {
        static const File delegating[] = {
            {"lib.lh",
         "module^ ns.lib\n"
         "public^ let^ Held = def^{ self^{ }, read = f^self^ -> number^ { return^ 7 } }\n"
         "public^ let^ Wrapper = def^{\n"
         "  self^{ abstract^ held : Held },\n"
         "  override^new = f^ { self^{ held = Held.new() } },\n"
         "  delegate^ self^.held,\n"
         "}\n"},
            {"main.lh",
         "let^ lib = require^ \"lib.lh\"\n"
         "let^ Mine = lib.Wrapper .. def^{\n"
         "  self^{ },\n"
         "  twice = f^self^ -> number^ { return^ self^.read() * 2 },\n"
         "}\n"
         "let^ m = Mine.new()\n"
         "return^ m.read() + m.twice()\n"},
        };
        program_with(&program, &disk, delegating, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "every unit compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            // 7 from outside the wrapper and 14 from inside one of its own
            // members -- both reach the delegate the other unit declared.
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 21);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 4 章: what another unit may name is what this one published.
    LHAT_TEST("but only what that unit published");
    {
        static const File hidden[] = {
            {"lib.lh",
             "module^ ns.lib\n"
             "let^ Hidden = def^{ self^{}, no = f^self^ -> number^ { return^ 0 } }\n"},
            {"main.lh",
             "let^ lib = require^ \"lib.lh\"\n"
             "let^ Mine = lib.Hidden .. def^{ self^{} }\n"},
        };
        program_with(&program, &disk, hidden, 2);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "the checker refuses it");
    }
    lhat_program_dispose(&program);

    // 05 の 5.3: a part flattened here was written elsewhere, so what is free
    // in it is that unit's. What it can reach from here is what lives under
    // L^.modules -- what that unit published, and the roots it imported.
    LHAT_TEST("and its body still names what its own unit named");
    {
        static const File shared[] = {
            {"lib.lh",
             "module^ ns.lib\n"
             "public^ let^ ten = f^ -> number^ { return^ 10 }\n"
             "let^ hidden = f^ -> number^ { return^ 1 }\n"
             "public^ let^ Base = def^{\n"
             "  self^{},\n"
             "  reach = f^self^ -> number^ { return^ ten() },\n"
             "}\n"
             "public^ let^ Sealed = def^{\n"
             "  self^{},\n"
             "  reach = f^self^ -> number^ { return^ hidden() },\n"
             "}\n"},
            {"main.lh",
             "let^ lib = require^ \"lib.lh\"\n"
             "let^ Mine = lib.Base .. def^{ self^{} }\n"
             "return^ Mine.new().reach()\n"},
        };
        program_with(&program, &disk, shared, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the program checked");
        bool compiled = lhat_program_compile(&program);
        LHAT_CHECK(compiled, "every unit compiled");
        if (compiled && root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 10);
            lhat_machine_dispose(machine);
        }
    }
    lhat_program_dispose(&program);

    // 05 の 4 章: a name that unit kept to itself is a register in a frame
    // this body does not have, and saying so is worth more than "no such
    // name" about a name plainly there in the other file.
    LHAT_TEST("but not what it kept to itself");
    {
        static const File kept[] = {
            {"lib.lh",
             "module^ ns.lib\n"
             "let^ hidden = f^ -> number^ { return^ 1 }\n"
             "public^ let^ Sealed = def^{\n"
             "  self^{},\n"
             "  reach = f^self^ -> number^ { return^ hidden() },\n"
             "}\n"},
            {"main.lh",
             "let^ lib = require^ \"lib.lh\"\n"
             "let^ Mine = lib.Sealed .. def^{ self^{} }\n"},
        };
        program_with(&program, &disk, kept, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the checker is content");
        LHAT_CHECK(!lhat_program_compile(&program),
                   "and the compile stops");
        LHAT_CHECK_EQ_INT(lhat_program_compile_status(&program),
                          LHAT_COMPILE_NOT_PUBLISHED);
    }
    lhat_program_dispose(&program);

    // 03 の 4.2: a form 14.2 cannot follow to a chain is a hole to report
    // where it is written, not instructions that fault where they run. A
    // definition reached through an ordinary table is one of those.
    LHAT_TEST("and a chain that cannot be followed is reported, not emitted");
    {
        static const File through_table[] = {
            {"main.lh",
             "let^ t = { Leaf = def^{ self^{}, one = f^self^ -> number^ "
             "{ return^ 1 } } }\n"
             "let^ M = t.Leaf .. def^{ self^{} }\n"},
        };
        program_with(&program, &disk, through_table, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the checker is content");
        LHAT_CHECK(!lhat_program_compile(&program),
                   "and the compile stops");
        LHAT_CHECK_EQ_INT(lhat_program_compile_status(&program),
                          LHAT_COMPILE_UNSUPPORTED);
    }
    lhat_program_dispose(&program);
}

// 02 の 18: an annotation is what the host registered and nothing else. The
// language never reads what one means -- what is pinned here is that it is
// carried, and that the three ways of writing one wrongly are all refused.
// 02 の 18.5: the registration and the shape of its arguments, which the
// tests below say together because almost every one of them has both to say.
// Two calls under one name rather than a parameter: what a registration can
// carry grows, and program.h gives each part its own way in.
static bool registered_with(LhatProgram *program, const char *module,
                            const char *name, uint32_t targets,
                            const char *signature)
{
    return lhat_register_annotation(program, module, name, targets) &&
           (signature == NULL ||
            lhat_register_annotation_signature(program, name, signature));
}

static void test_annotations(void)
{
    LhatProgram program;
    Disk disk;

    static const File wearing[] = {
        {"main.lh",
         "@tool\n"
         "module^ ns.main\n"
         "@icon(\"res://x.svg\")\n"
         "public^ let^ Thing = def^{\n"
         "  self^{ @export(0, 100) hp = 5 },\n"
         "  @rpc(\"any_peer\") go = p^self^ { },\n"
         "}\n"},
    };

    LHAT_TEST("what the host registered is written and carried");
    {
        program_with(&program, &disk, wearing, 1);
        LHAT_CHECK(lhat_register_annotation(&program, "godot", "tool",
                                            LHAT_ANNOTATION_UNIT),
                   "tool");
        LHAT_CHECK(registered_with(&program, "godot", "icon",
                                   LHAT_ANNOTATION_BINDING,
                                   "p^ string^;"),
                   "icon");
        LHAT_CHECK(registered_with(&program, "godot", "export",
                                   LHAT_ANNOTATION_FIELD,
                                   "p^ number^, number^;"),
                   "export");
        LHAT_CHECK(registered_with(&program, "godot", "rpc",
                                   LHAT_ANNOTATION_MEMBER,
                                   "p^ string^;"),
                   "rpc");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL, "the unit loaded");
        LHAT_CHECK(!lhat_program_has_errors(&program), "and checked clean");
    }
    lhat_program_dispose(&program);

    // 18.2 keeps the namespace flat, so a second registration of one name is
    // a collision rather than another arm -- unlike a member (14.12).
    LHAT_TEST("and a name is registered once");
    {
        program_with(&program, &disk, wearing, 1);
        LHAT_CHECK(lhat_register_annotation(&program, "a", "tool",
                                            LHAT_ANNOTATION_UNIT),
                   "the first takes");
        LHAT_CHECK(!lhat_register_annotation(&program, "b", "tool",
                                             LHAT_ANNOTATION_UNIT),
                   "the second is refused");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("an unregistered name is refused");
    {
        static const File unknown[] = {{"main.lh", "@nosuch\nlet^ x = 1\n"}};
        program_with(&program, &disk, unknown, 1);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "reported");
    }
    lhat_program_dispose(&program);

    // 18.5: where it may be written is what the registration says.
    LHAT_TEST("and so is one written where it was not registered for");
    {
        static const File misplaced[] = {
            {"main.lh", "@onlyfields\nlet^ x = 1\n"}};
        program_with(&program, &disk, misplaced, 1);
        lhat_register_annotation(&program, "h", "onlyfields",
                                 LHAT_ANNOTATION_FIELD);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "reported");
    }
    lhat_program_dispose(&program);

    // 18.4改: whether the binding is published is part of the place. A host
    // reaches a value through the table the unit answers with (05 の 5.5), so
    // an annotation about the value has no hold on a private name -- writing
    // one there would be a mark that quietly does nothing.
    LHAT_TEST("a public-only annotation is refused on a private binding");
    {
        static const File hidden[] = {
            {"main.lh", "module^ m\n@badge\nlet^ x = 1\n"}};
        program_with(&program, &disk, hidden, 1);
        lhat_register_annotation(&program, "h", "badge",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "reported");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and taken on a published one");
    {
        static const File shown[] = {
            {"main.lh", "module^ m\n@badge\npublic^let^ x = 1\n"}};
        program_with(&program, &disk, shown, 1);
        lhat_register_annotation(&program, "h", "badge",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "clean");
    }
    lhat_program_dispose(&program);

    // 18.5: FILEUNIQUE is a count rather than a place. The one just above
    // shows the same name twice being fine without it, which is what makes
    // this pair say something.
    LHAT_TEST("a file-unique annotation is refused a second time");
    {
        static const File twice[] = {
            {"main.lh",
             "module^ m\n@only\npublic^let^ a = 1\n@only\npublic^let^ b = 2\n"}};
        program_with(&program, &disk, twice, 1);
        lhat_register_annotation(&program, "h", "only",
                                 LHAT_ANNOTATION_PUBLIC |
                                     LHAT_ANNOTATION_FILEUNIQUE);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "reported");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and taken once");
    {
        static const File once[] = {
            {"main.lh", "module^ m\n@only\npublic^let^ a = 1\n"}};
        program_with(&program, &disk, once, 1);
        lhat_register_annotation(&program, "h", "only",
                                 LHAT_ANNOTATION_PUBLIC |
                                     LHAT_ANNOTATION_FILEUNIQUE);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "clean");
    }
    lhat_program_dispose(&program);

    // 03 の 3.4改2: a member calling one declared after it makes the checker
    // walk the statements again, and the second walk visits the same
    // annotation node. The same node is not a second writing.
    LHAT_TEST("one annotation over a forward self^ call is still once");
    {
        static const File fwd[] = {
            {"main.lh",
             "module^ m\n@only\npublic^let^ T = def^{\n"
             "    a = p^self^{ self^.b() },\n"
             "    b = p^self^{ },\n"
             "}\n"}};
        program_with(&program, &disk, fwd, 1);
        lhat_register_annotation(&program, "h", "only",
                                 LHAT_ANNOTATION_PUBLIC |
                                     LHAT_ANNOTATION_FILEUNIQUE);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "once is once");
    }
    lhat_program_dispose(&program);

    // Counted per registration, not between them: a host wanting two names to
    // be one choice is the one that knows they are, and counts them itself.
    LHAT_TEST("two file-unique annotations do not exclude each other");
    {
        static const File pair[] = {
            {"main.lh",
             "module^ m\n@one\npublic^let^ a = 1\n@two\npublic^let^ b = 2\n"}};
        program_with(&program, &disk, pair, 1);
        lhat_register_annotation(&program, "h", "one",
                                 LHAT_ANNOTATION_PUBLIC |
                                     LHAT_ANNOTATION_FILEUNIQUE);
        lhat_register_annotation(&program, "h", "two",
                                 LHAT_ANNOTATION_PUBLIC |
                                     LHAT_ANNOTATION_FILEUNIQUE);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "clean");
    }
    lhat_program_dispose(&program);

    // Registering for the binding alone keeps admitting both -- the name is
    // what such an annotation is about, not the value.
    LHAT_TEST("while a plain binding annotation takes either");
    {
        static const File both[] = {
            {"main.lh", "module^ m\n@icon\nlet^ a = 1\n@icon\npublic^let^ b = 2\n"}};
        program_with(&program, &disk, both, 1);
        lhat_register_annotation(&program, "h", "icon",
                                 LHAT_ANNOTATION_BINDING);
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program), "clean");
    }
    lhat_program_dispose(&program);

    // 18.3: an argument is a literal, and a bare name arrives as a string^ --
    // what the name means is the host's to decide.
    LHAT_TEST("arguments are asked what the registration said");
    {
        static const File args[] = {
            {"main.lh", "@ranged(0, -50, PROPERTY_HINT_ENUM)\nlet^ x = 1\n"}};
        program_with(&program, &disk, args, 1);
        LHAT_CHECK(registered_with(&program, "h", "ranged",
                                   LHAT_ANNOTATION_UNIT,
                                   "p^ number^, number^, string^;"),
                   "the signature parsed");
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(!lhat_program_has_errors(&program),
                   "a name stands where a string^ was asked for");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and refused when they do not fit");
    {
        static const File wrong[] = {
            {"main.lh", "@ranged(\"no\")\nlet^ x = 1\n"}};
        program_with(&program, &disk, wrong, 1);
        registered_with(&program, "h", "ranged",
                        LHAT_ANNOTATION_UNIT,
                        "p^ number^, number^;");
        lhat_program_check(&program, "main.lh");
        LHAT_CHECK(lhat_program_has_errors(&program), "reported");
    }
    lhat_program_dispose(&program);
    // 18.1: what the host reads back. The language never says what an
    // annotation means, so this is the whole of what it hands over -- the
    // name, and the literals written with it.
    LHAT_TEST("and the host reads back what was written");
    {
        program_with(&program, &disk, wearing, 1);
        lhat_register_annotation(&program, "godot", "tool",
                                 LHAT_ANNOTATION_UNIT);
        registered_with(&program, "godot", "icon",
                        LHAT_ANNOTATION_BINDING, "p^ string^;");
        registered_with(&program, "godot", "export",
                        LHAT_ANNOTATION_FIELD,
                        "p^ number^, number^;");
        registered_with(&program, "godot", "rpc",
                        LHAT_ANNOTATION_MEMBER, "p^ string^;");
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL && !lhat_program_has_errors(&program),
                   "checked clean");

        // The unit's own, written at its head.
        LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(unit, NULL, NULL), 1);
        LhatAnnotation tool = lhat_unit_annotation(unit, NULL, NULL, 0);
        LHAT_CHECK_EQ_STR(tool.name, tool.name_length, "tool");
        LHAT_CHECK_EQ_INT(tool.argument_count, 0);

        // The binding that holds the definition.
        LhatAnnotation icon = lhat_unit_annotation(unit, "Thing", NULL, 0);
        LHAT_CHECK_EQ_STR(icon.name, icon.name_length, "icon");
        LHAT_CHECK_EQ_INT(icon.argument_count, 1);
        LhatAnnotationArgument path = lhat_annotation_argument(icon, 0);
        LHAT_CHECK_EQ_INT(path.kind, LHAT_ANNOTATION_ARG_STRING);
        LHAT_CHECK_EQ_STR(path.text, path.length, "res://x.svg");

        // A field of its template, which is where an @export goes.
        LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(unit, "Thing", "hp"), 1);
        LhatAnnotation exported = lhat_unit_annotation(unit, "Thing", "hp", 0);
        LHAT_CHECK_EQ_STR(exported.name, exported.name_length, "export");
        LHAT_CHECK_EQ_INT(exported.argument_count, 2);
        LHAT_CHECK_EQ_REAL(lhat_annotation_argument(exported, 0).number, 0.0,
                             1e-9);
        LHAT_CHECK_EQ_REAL(lhat_annotation_argument(exported, 1).number,
                             100.0, 1e-9);

        // And a member of the definition, which is the other place.
        LhatAnnotation rpc = lhat_unit_annotation(unit, "Thing", "go", 0);
        LHAT_CHECK_EQ_STR(rpc.name, rpc.name_length, "rpc");

        // Nothing was written above these.
        LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(unit, "Thing", "nope"), 0);
        LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(unit, "Nope", NULL), 0);
    }
    lhat_program_dispose(&program);

    // 18.3: the four kinds, each arriving as itself. A name is not resolved
    // -- it is the spelling, for the host to read.
    LHAT_TEST("every kind of argument arrives as itself");
    {
        static const File kinds[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@kinds(-1.5, \"text\", SOME_NAME, true^, false^)\n"
             "let^ x = 1\n"},
        };
        program_with(&program, &disk, kinds, 1);
        LHAT_CHECK(
            registered_with(
                &program, "h", "kinds", LHAT_ANNOTATION_BINDING,
                "p^ number^, string^, string^, bool^, bool^;"),
            "the signature parsed");
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL && !lhat_program_has_errors(&program),
                   "checked clean");

        LhatAnnotation written = lhat_unit_annotation(unit, NULL, "x", 0);
        LHAT_CHECK_EQ_INT(written.argument_count, 5);

        LhatAnnotationArgument number = lhat_annotation_argument(written, 0);
        LHAT_CHECK_EQ_INT(number.kind, LHAT_ANNOTATION_ARG_NUMBER);
        // 18.3's leading '-' is applied on the way out, so a host reads one
        // number rather than the shape it was written as.
        LHAT_CHECK_EQ_REAL(number.number, -1.5, 1e-9);

        LhatAnnotationArgument text = lhat_annotation_argument(written, 1);
        LHAT_CHECK_EQ_INT(text.kind, LHAT_ANNOTATION_ARG_STRING);
        LHAT_CHECK_EQ_STR(text.text, text.length, "text");

        LhatAnnotationArgument named = lhat_annotation_argument(written, 2);
        LHAT_CHECK_EQ_INT(named.kind, LHAT_ANNOTATION_ARG_NAME);
        LHAT_CHECK_EQ_STR(named.text, named.length, "SOME_NAME");

        LHAT_CHECK_EQ_INT(lhat_annotation_argument(written, 3).kind,
                          LHAT_ANNOTATION_ARG_BOOL);
        LHAT_CHECK(lhat_annotation_argument(written, 3).boolean, "true^");
        LHAT_CHECK(!lhat_annotation_argument(written, 4).boolean, "false^");

        LHAT_CHECK_EQ_INT(lhat_annotation_argument(written, 5).kind,
                          LHAT_ANNOTATION_ARG_NUMBER);  // zeroed past the end
    }
    lhat_program_dispose(&program);
}

#if LHAT_WITH_COMMENTS
// Asked twice: once to measure with no buffer and once to fill one, which is
// the convention and has to answer the same either way.
static void expect_documentation(const LhatUnit *unit, const char *definition,
                                 const char *name, const char *want)
{
    char buffer[256];
    size_t needed = lhat_unit_documentation(unit, definition, name, NULL, 0);
    size_t filled =
        lhat_unit_documentation(unit, definition, name, buffer, sizeof buffer);
    LHAT_CHECK(needed == filled, "measured %zu, filled %zu", needed, filled);
    LHAT_CHECK(needed == strlen(want), "wanted %zu bytes for \"%s\", said %zu",
               strlen(want), want, needed);
    LHAT_CHECK(strcmp(buffer, want) == 0, "got \"%s\", want \"%s\"", buffer,
               want);
}

// 01 の 6.4: L^ has no spelling for a description -- the comment block
// written directly above a thing is what it says about it, the way Go has
// it. These are the four addresses a host reads one at, and the two rules
// that say where a block begins and ends.
static void test_documentation(void)
{
    LhatProgram program;
    Disk disk;

    static const File said[] = {
        {"main.lh",
         "# 在庫を扱う単位\n"
         "# 二行目もその続き\n"
         "module^ ns.main\n"
         "\n"
         "# 数えるもの\n"
         "public^ let^ Thing = def^{\n"
         "  self^{\n"
         "    # 残り\n"
         "    hp = 5,\n"
         "  },\n"
         "\n"
         "  # ここから下は後で消す\n"
         "\n"
         "  # 進める\n"
         "  go = p^self^ { },   # 行末の覚え書き\n"
         "\n"
         "  quiet = p^self^ { },\n"
         "}\n"
         "\n"
         "#[ 上限 ]#\n"
         "let^ cap = 9\n"},
    };

    LHAT_TEST("the block above a thing is what it says about itself");
    {
        program_with(&program, &disk, said, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL, "the unit loaded");
        LHAT_CHECK(!lhat_program_has_errors(&program), "and checked clean");

        // The head of the file. attach_comments hands what stands before the
        // first statement to that statement, so this is the module^ line's
        // block and the unit's alike -- written once, at the head.
        expect_documentation(root, NULL, NULL,
                             "在庫を扱う単位\n二行目もその続き");

        expect_documentation(root, "Thing", NULL, "数えるもの");
        expect_documentation(root, "Thing", "hp", "残り");
        expect_documentation(root, NULL, "cap", "上限");

        // A blank line ends a block: the note two lines up was written about
        // nothing that follows it, and the one left at the end of the line
        // is a remark on that line rather than a description.
        expect_documentation(root, "Thing", "go", "進める");

        // And what has nothing above it says nothing -- the trailing comment
        // belongs to the member whose line it ends.
        expect_documentation(root, "Thing", "quiet", "");

        // An address that names nothing answers the same way.
        expect_documentation(root, "Thing", "nosuch", "");
        expect_documentation(root, "Nosuch", NULL, "");
    }
    lhat_program_dispose(&program);


    // 18.4 writes an annotation between the block and the thing it is about,
    // and lhat_node_visit_children does not walk one -- so the span begins
    // after it. What stands between is not a blank line for all that.
    LHAT_TEST("an annotation between does not cut the block off");
    {
        static const File marked[] = {
            {"main.lh",
             "module^ ns.main\n"
             "\n"
             "# 回るスプライト\n"
             "@game\n"
             "public^ let^ Probe = def^{\n"
             "  self^{\n"
             "    # 一秒あたりの回転\n"
             "    @export_range(0, 10) speed = 1,\n"
             "  },\n"
             "}\n"},
        };
        program_with(&program, &disk, marked, 1);
        LHAT_CHECK(lhat_register_annotation(&program, "godot", "game",
                                            LHAT_ANNOTATION_PUBLIC),
                   "game");
        LHAT_CHECK(registered_with(&program, "godot", "export_range",
                                   LHAT_ANNOTATION_FIELD,
                                   "p^ number^, number^;"),
                   "export_range");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL, "the unit loaded");
        LHAT_CHECK(!lhat_program_has_errors(&program), "and checked clean");

        expect_documentation(root, "Probe", NULL, "回るスプライト");
        expect_documentation(root, "Probe", "speed", "一秒あたりの回転");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and a buffer too small is filled as far as it goes");
    {
        program_with(&program, &disk, said, 1);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL, "the unit loaded");

        char small[7];
        size_t needed =
            lhat_unit_documentation(root, "Thing", NULL, small, sizeof small);
        LHAT_CHECK(needed == strlen("数えるもの"), "the whole length is said");
        LHAT_CHECK(strlen(small) == sizeof small - 1, "and it is terminated");
        LHAT_CHECK(memcmp(small, "数えるもの", sizeof small - 1) == 0,
                   "with what fit");
    }
    lhat_program_dispose(&program);
}
#endif  // LHAT_WITH_COMMENTS


// 02 の 18.5.1: two names the host registered as one choice. FILEUNIQUE counts
// each registration on its own, so the pair is what only the host can say --
// and having said it, the checker is the one that says where.
static void test_annotation_exclusion(void)
{
    LhatProgram program;
    Disk disk;

    // Above two different declarations, which is the shape FILEUNIQUE cannot
    // see: each name is written once, so each is within its own count. The
    // second one is on line 4.
    static const File apart[] = {
        {"main.lh",
         "module^ ns.main\n"
         "@game\n"
         "public^ let^ A = def^{ }\n"
         "@tool\n"
         "public^ let^ B = def^{ }\n"}};

    LHAT_TEST("18.5.1: two answers to one question are refused together");
    {
        program_with(&program, &disk, apart, 1);
        LHAT_CHECK(lhat_register_annotation(&program, "godot", "game",
                                            LHAT_ANNOTATION_PUBLIC |
                                                LHAT_ANNOTATION_FILEUNIQUE),
                   "game");
        LHAT_CHECK(lhat_register_annotation(&program, "godot", "tool",
                                            LHAT_ANNOTATION_PUBLIC |
                                                LHAT_ANNOTATION_FILEUNIQUE),
                   "tool");
        LHAT_CHECK(lhat_register_annotation_exclusive(&program, "game", "tool"),
                   "game excludes tool");
        LHAT_CHECK(lhat_register_annotation_exclusive(&program, "tool", "game"),
                   "and tool excludes game");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE),
                   "reported as an exclusion");
        LHAT_CHECK(!has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_REPEATED),
                   "rather than as a repeat, which neither name is");
        // What moving this off the host was for: a place to jump to.
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 1 &&
                       root->checked.diagnostics[0].line == 4,
                   "at the second of the two");
    }
    lhat_program_dispose(&program);

    // Said from one side only. The checker reads it both ways, so which of
    // the two a file writes first is nothing the answer turns on.
    LHAT_TEST("and one side saying it is enough");
    {
        program_with(&program, &disk, apart, 1);
        lhat_register_annotation(&program, "godot", "game",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation(&program, "godot", "tool",
                                 LHAT_ANNOTATION_PUBLIC);
        LHAT_CHECK(lhat_register_annotation_exclusive(&program, "tool", "game"),
                   "only the one written second says it");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE),
                   "reported");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("whichever of the two comes first");
    {
        static const File reversed[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@tool\n"
             "public^ let^ A = def^{ }\n"
             "@game\n"
             "public^ let^ B = def^{ }\n"}};
        program_with(&program, &disk, reversed, 1);
        lhat_register_annotation(&program, "godot", "game",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation(&program, "godot", "tool",
                                 LHAT_ANNOTATION_PUBLIC);
        // The one that says it is now the one written first, so the report
        // has to come off the other name.
        lhat_register_annotation_exclusive(&program, "tool", "game");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE),
                   "reported the other way round");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and both above one declaration is the same refusal");
    {
        static const File together[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@game\n"
             "@tool\n"
             "public^ let^ A = def^{ }\n"}};
        program_with(&program, &disk, together, 1);
        lhat_register_annotation(&program, "godot", "game",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation(&program, "godot", "tool",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_exclusive(&program, "game", "tool");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE),
                   "reported");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a file writing one of them is what the pair is for");
    {
        static const File alone[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@game\n"
             "public^ let^ A = def^{ }\n"
             "public^ let^ B = def^{ }\n"}};
        program_with(&program, &disk, alone, 1);
        lhat_register_annotation(&program, "godot", "game",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation(&program, "godot", "tool",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_exclusive(&program, "game", "tool");
        lhat_register_annotation_exclusive(&program, "tool", "game");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "checked clean");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("what the registration will and will not take");
    {
        program_with(&program, &disk, NULL, 0);
        lhat_register_annotation(&program, "h", "one", LHAT_ANNOTATION_UNIT);
        LHAT_CHECK(!lhat_register_annotation_exclusive(&program, "nosuch",
                                                       "one"),
                   "a name that was never registered says nothing");
        LHAT_CHECK(!lhat_register_annotation_exclusive(&program, "one", "one"),
                   "and nothing excludes itself");
        // 18.2 keeps the namespace flat, so the other side is a name rather
        // than a registration -- and another host may be the one to bring it.
        LHAT_CHECK(lhat_register_annotation_exclusive(&program, "one",
                                                      "unheard"),
                   "but the other side need not be registered yet");
        LHAT_CHECK(lhat_register_annotation_exclusive(&program, "one",
                                                      "unheard"),
                   "and saying it twice is saying it once");
    }
    lhat_program_dispose(&program);

    // 18.3, now that the signature is said in its own call rather than beside
    // the targets.
    LHAT_TEST("a signature is said once");
    {
        program_with(&program, &disk, NULL, 0);
        lhat_register_annotation(&program, "h", "one", LHAT_ANNOTATION_UNIT);
        LHAT_CHECK(lhat_register_annotation_signature(&program, "one",
                                                      "p^ number^;"),
                   "the first takes");
        LHAT_CHECK(!lhat_register_annotation_signature(&program, "one",
                                                       "p^ string^;"),
                   "the second is refused");
        LHAT_CHECK(!lhat_register_annotation_signature(&program, "nosuch",
                                                       "p^;"),
                   "and so is one for a name that was never registered");
    }
    lhat_program_dispose(&program);
}

// 02 の 18.5.2: a name the host registered as half of something. Read on the
// declaration the annotation is written above, which is what tells it from
// the exclusion above -- that one is a file's to answer, this one a
// declaration's.
static void test_annotation_requisite(void)
{
    LhatProgram program;
    Disk disk;

    // 18.4改's target for both, so what varies between the cases below is
    // only which of them is written and where.
    static const File both[] = {
        {"main.lh",
         "module^ ns.main\n"
         "@icon(\"res://x.svg\")\n"
         "@export_class\n"
         "public^ let^ A = def^{ }\n"}};

    LHAT_TEST("18.5.2: a mark that needs another is taken beside it");
    {
        program_with(&program, &disk, both, 1);
        LHAT_CHECK(registered_with(&program, "godot", "icon",
                                   LHAT_ANNOTATION_PUBLIC, "p^ string^;"),
                   "icon");
        LHAT_CHECK(lhat_register_annotation(&program, "godot", "export_class",
                                            LHAT_ANNOTATION_PUBLIC),
                   "export_class");
        LHAT_CHECK(lhat_register_annotation_requisite(&program, "icon",
                                                      "export_class"),
                   "icon needs export_class");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "checked clean");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and refused without it");
    {
        static const File alone[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@icon(\"res://x.svg\")\n"
             "public^ let^ A = def^{ }\n"}};
        program_with(&program, &disk, alone, 1);
        registered_with(&program, "godot", "icon", LHAT_ANNOTATION_PUBLIC,
                        "p^ string^;");
        lhat_register_annotation(&program, "godot", "export_class",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_requisite(&program, "icon", "export_class");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_REQUISITE),
                   "reported");
        // The name the diagnostic carries is the one that is missing: the
        // caret is already on what was written, and what a reader is short
        // of is the other one.
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 1 &&
                       root->checked.diagnostics[0].name_length == 12 &&
                       memcmp(root->checked.diagnostics[0].name, "export_class",
                              12) == 0,
                   "and names what would have done");
        LHAT_CHECK(root != NULL && root->checked.diagnostics[0].line == 2,
                   "at the mark that was written");
    }
    lhat_program_dispose(&program);

    // The point of reading the declaration rather than the file. Both marks
    // are in the unit, and neither is beside the other.
    LHAT_TEST("and the other half has to be on the same declaration");
    {
        static const File apart[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@icon(\"res://x.svg\")\n"
             "public^ let^ A = def^{ }\n"
             "@export_class\n"
             "public^ let^ B = def^{ }\n"}};
        program_with(&program, &disk, apart, 1);
        registered_with(&program, "godot", "icon", LHAT_ANNOTATION_PUBLIC,
                        "p^ string^;");
        lhat_register_annotation(&program, "godot", "export_class",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_requisite(&program, "icon", "export_class");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_ANNOTATION_REQUISITE),
                   "reported, though both are in the file");
    }
    lhat_program_dispose(&program);

    // One way only: what needs the other is the one refused. export_class on
    // its own says something complete.
    LHAT_TEST("and the one it needs does not need it back");
    {
        static const File named[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@export_class\n"
             "public^ let^ A = def^{ }\n"}};
        program_with(&program, &disk, named, 1);
        registered_with(&program, "godot", "icon", LHAT_ANNOTATION_PUBLIC,
                        "p^ string^;");
        lhat_register_annotation(&program, "godot", "export_class",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_requisite(&program, "icon", "export_class");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "checked clean");
    }
    lhat_program_dispose(&program);

    // Several may be said, and any one of them does. A host naming two ways
    // to be complete is naming what would do, not a list to write out.
    LHAT_TEST("and any one of several is enough");
    {
        static const File either[] = {
            {"main.lh",
             "module^ ns.main\n"
             "@icon(\"res://x.svg\")\n"
             "@global\n"
             "public^ let^ A = def^{ }\n"}};
        program_with(&program, &disk, either, 1);
        registered_with(&program, "godot", "icon", LHAT_ANNOTATION_PUBLIC,
                        "p^ string^;");
        lhat_register_annotation(&program, "godot", "export_class",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation(&program, "godot", "global",
                                 LHAT_ANNOTATION_PUBLIC);
        lhat_register_annotation_requisite(&program, "icon", "export_class");
        lhat_register_annotation_requisite(&program, "icon", "global");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
                   "the second one did");
    }
    lhat_program_dispose(&program);

    LHAT_TEST("what the registration will and will not take");
    {
        program_with(&program, &disk, NULL, 0);
        lhat_register_annotation(&program, "h", "one", LHAT_ANNOTATION_UNIT);
        LHAT_CHECK(!lhat_register_annotation_requisite(&program, "nosuch",
                                                       "one"),
                   "a name that was never registered says nothing");
        LHAT_CHECK(!lhat_register_annotation_requisite(&program, "one", "one"),
                   "and nothing is its own other half");
        // 18.2 keeps the namespace flat, so the other side is a name rather
        // than a registration -- another host may be the one to bring it.
        LHAT_CHECK(lhat_register_annotation_requisite(&program, "one",
                                                      "unheard"),
                   "but the other side need not be registered yet");
        LHAT_CHECK(lhat_register_annotation_requisite(&program, "one",
                                                      "unheard"),
                   "and saying it twice is saying it once");
    }
    lhat_program_dispose(&program);
}


// 02 の 18.7改: which members a host may put its own value under. What is
// written is an ordinary member with an ordinary signature -- the body is
// where it says there is nothing to run.
static void test_empty_body(void)
{
    LhatProgram program;
    Disk disk;

    static const File shapes[] = {
        {"main.lh",
         "module^ ns.main\n"
         "public^ let^ Thing = def^{\n"
         "  self^{ hp = 1 },\n"
         "  hollow = p^self^, message:string^ { },\n"
         "  written = p^self^, message:string^ { self^.hp := 1 },\n"
         "  abstract^ waiting : p^string^;,\n"
         "  plain = 5,\n"
         "}\n"}};

    LHAT_TEST("18.7改: an empty body is what the tree says about it");
    {
        program_with(&program, &disk, shapes, 1);
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL && !lhat_program_has_errors(&program),
                   "checked clean");

        size_t count = lhat_unit_member_count(unit, "Thing");
        bool saw_hollow = false;
        bool saw_written = false;
        bool saw_waiting = false;
        bool saw_plain = false;
        for (size_t i = 0; i < count; i++) {
            LhatUnitMember member = lhat_unit_member(unit, "Thing", i);
            if (member.name == NULL) {
                continue;
            }
            if (member.name_length == 6 &&
                memcmp(member.name, "hollow", 6) == 0) {
                saw_hollow = true;
                LHAT_CHECK(member.empty_body, "the empty one is said to be");
                LHAT_CHECK(!member.declared,
                           "and is not a declaration -- it has a value");
                // The signature is read the way any member's is, which is
                // the whole point of writing it as one.
                LHAT_CHECK_EQ_INT(member.parameter_count, 1);
                LhatUnitParameter said =
                    lhat_unit_member_parameter(unit, "Thing", i, 0);
                LHAT_CHECK_EQ_STR(said.name, said.name_length, "message");
                LHAT_CHECK_EQ_INT(said.type, LHAT_UNIT_TYPE_STRING);
            } else if (member.name_length == 7 &&
                       memcmp(member.name, "written", 7) == 0) {
                saw_written = true;
                LHAT_CHECK(!member.empty_body, "a body of one statement is a "
                                               "body");
            } else if (member.name_length == 7 &&
                       memcmp(member.name, "waiting", 7) == 0) {
                saw_waiting = true;
                // 14.15 has no value at all, so there is no body to be empty.
                LHAT_CHECK(member.declared, "the declaration is one");
                LHAT_CHECK(!member.empty_body, "and is not this");
            } else if (member.name_length == 5 &&
                       memcmp(member.name, "plain", 5) == 0) {
                saw_plain = true;
                LHAT_CHECK(!member.empty_body,
                           "and neither is a member holding a value");
            }
        }
        LHAT_CHECK(saw_hollow && saw_written && saw_waiting && saw_plain,
                   "all four were walked");
    }
    lhat_program_dispose(&program);

    // 10.1 lets a body outside a loop carry finally^, and one written there
    // is written -- so both halves of the list are asked.
    LHAT_TEST("and a body carrying only a clause is not empty");
    {
        static const File clause[] = {
            {"main.lh",
             "module^ ns.main\n"
             "public^ let^ Thing = def^{\n"
             "  self^{ hp = 1 },\n"
             "  guarded = p^self^ { finally^: self^.hp := 2 },\n"
             "}\n"}};
        program_with(&program, &disk, clause, 1);
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL && !lhat_program_has_errors(&program),
                   "checked clean");
        size_t count = lhat_unit_member_count(unit, "Thing");
        bool saw = false;
        for (size_t i = 0; i < count; i++) {
            LhatUnitMember member = lhat_unit_member(unit, "Thing", i);
            if (member.name != NULL && member.name_length == 7 &&
                memcmp(member.name, "guarded", 7) == 0) {
                saw = true;
                LHAT_CHECK(!member.empty_body, "the clause is a body");
            }
        }
        LHAT_CHECK(saw, "the member was walked");
    }
    lhat_program_dispose(&program);
}

// 02 の 14.7改2: what a def^ writes, when one of the entries is a delegate.
// A host walks a definition's members by index, and both walks that answer
// it read the template off the entry with no key -- which the delegate also
// has. Reading its value as a list of fields is reading a union the wrong
// way, so this is a crash and not a wrong answer.
static void test_delegate_among_the_members(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("14.7改2: a delegate entry is not the template");
    {
        static const File files[] = {
            {"main.lh",
             "module^ ns.main\n"
             "let^ Inner = def^{ self^{ }, read = f^self^ -> number^ { return^ 1 } }\n"
             "public^ let^ Outer = def^{\n"
             "  self^{ ticks = 0 },\n"
             "  shared = Inner.new(),\n"
             "  delegate^ shared,\n"
             "  tick = p^self^ { self^.ticks := self^.ticks + 1 },\n"
             "}\n"}};
        program_with(&program, &disk, files, 1);
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL && !lhat_program_has_errors(&program),
                   "checked clean");

        // The entries the unit wrote: the two members and the one field.
        // What the delegate lends is not written here, so it is not among
        // them -- the same reading the checker has.
        size_t count = lhat_unit_member_count(unit, "Outer");
        LHAT_CHECK_EQ_INT(count, 3);
        bool saw_shared = false;
        bool saw_tick = false;
        bool saw_ticks = false;
        bool saw_read = false;
        for (size_t i = 0; i < count; i++) {
            LhatUnitMember member = lhat_unit_member(unit, "Outer", i);
            if (member.name == NULL) {
                continue;
            }
            if (member.name_length == 6 &&
                memcmp(member.name, "shared", 6) == 0) {
                saw_shared = true;
            } else if (member.name_length == 4 &&
                       memcmp(member.name, "tick", 4) == 0) {
                saw_tick = true;
            } else if (member.name_length == 5 &&
                       memcmp(member.name, "ticks", 5) == 0) {
                saw_ticks = true;
            } else if (member.name_length == 4 &&
                       memcmp(member.name, "read", 4) == 0) {
                saw_read = true;
            }
        }
        LHAT_CHECK(saw_shared && saw_tick && saw_ticks,
                   "the written entries were walked");
        LHAT_CHECK(!saw_read, "and what the delegate lends was not");
    }
    lhat_program_dispose(&program);
}

// 07 の 4 章 with 05 の 6.1: a unit publishes a type, and a member of it was
// written in the unit that published it -- so a reader standing on the member
// in another unit has somewhere to be sent, and it is not this file. The
// same-unit half is in lsp/tests/test_definition.c, where no program is
// needed.
#if LHAT_WITH_RESOLUTIONS
static void test_where_a_published_member_was_written(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("05 の 6.1: a member reached across units names the unit it "
              "was written in");
    static const File files[] = {
        {"lib.lh",
             "module^ store\n"
             "\n"
             "public^let^ Held = def^{\n"
             "    self^{ count = 0 },\n"
             "    read = f^self^ -> number^ { return^ self^.count },\n"
             "}\n"},
        {"main.lh",
             "require^ \"lib.lh\"\n"
             "let^ made = store.Held.new()\n"
             "let^ value = made.read()\n"},
    };
    program_with(&program, &disk, files, 2);

    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
               "the program checked");
    if (root != NULL) {
        const char *use = strstr(root->source.text, "Held.new()");
        LHAT_CHECK(use != NULL, "expected the member use to be there");
        if (use != NULL) {
            const LhatResolution *r = lhat_check_resolution_at(
                &root->checked, (uint32_t)(use - root->source.text));
            LHAT_CHECK(r != NULL && r->has_definition,
                       "expected the published member to say where it is");
            if (r != NULL && r->has_definition) {
                LHAT_CHECK(r->definition_path != NULL &&
                               strcmp(r->definition_path, "lib.lh") == 0,
                           "expected lib.lh, got %s",
                           r->definition_path != NULL ? r->definition_path
                                                      : "(nothing)");
                // 05 の 4 章 publishes names, so the place is the public^let^
                // that wrote this one.
                const LhatUnit *lib = program.units;
                while (lib != NULL && strcmp(lib->path, "lib.lh") != 0) {
                    lib = lib->next;
                }
                LHAT_CHECK(lib != NULL, "expected lib.lh to be in the graph");
                if (lib != NULL) {
                    const char *declared = strstr(lib->source.text, "Held =");
                    LHAT_CHECK(declared != NULL, "expected the declaration");
                    if (declared != NULL) {
                        LHAT_CHECK_EQ_INT(
                            r->definition,
                            (uint32_t)(declared - lib->source.text));
                    }
                }
            }
        }
    }
    lhat_program_dispose(&program);
}
#endif

// 05 の 5.7, bundled: lhat_reload is what an editor's save becomes --
// invalidate, forget on every machine handed in, recheck, recompile, and
// the retired bodies discarded only once no machine still holds one. What
// is pinned: the discard happens by itself when nothing holds, waits when
// something does, and what still runs keeps running either way.
static void test_reload_call(void)
{
    LhatProgram program;
    Disk disk;
    static File live[] = {
        {"lib.lh", NULL},
        {"main.lh",
         "require^ \"lib.lh\"\n"
         "return^ ns.lib.answer\n"},
    };

    LHAT_TEST("one call replaces a unit, on every machine, and frees");
    {
        live[0].text = "module^ ns.lib\n"
                       "public^ let^ answer = f^ -> number^ { return^ 1 }\n";
        program_with(&program, &disk, live, 2);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");
        LhatMachine *one = lhat_machine_new();
        LhatMachine *two = lhat_machine_new();
        LhatMachine *machines[2] = {one, two};
        LhatValue first = lhat_run(one, lhat_unit_proto(root)).value;
        LHAT_CHECK_EQ_INT(
            lhat_as_integer(lhat_machine_call(one, first, NULL, 0).value), 1);
        lhat_run(two, lhat_unit_proto(root));

        // Nothing but L^.modules holds the old world, so the one call
        // replaces it everywhere and the retired bodies go on the spot.
        live[0].text = "module^ ns.lib\n"
                       "public^ let^ answer = f^ -> number^ { return^ 2 }\n";
        LHAT_CHECK_EQ_INT(lhat_reload(&program, "lib.lh", machines, 2), 2);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);
        LhatValue keep = lhat_run(two, lhat_unit_proto(root)).value;
        LHAT_CHECK_EQ_INT(
            lhat_as_integer(lhat_machine_call(two, keep, NULL, 0).value), 2);
        LHAT_CHECK_EQ_INT(
            lhat_as_integer(
                lhat_machine_call(
                    one, lhat_run(one, lhat_unit_proto(root)).value, NULL, 0)
                    .value),
            2);

        // A host still holding an old closure -- on one machine of the two
        // -- holds the discard back: the reload happens, the freeing waits.
        LHAT_CHECK(lhat_machine_set_global(two, "Keep", keep), "held");
        live[0].text = "module^ ns.lib\n"
                       "public^ let^ answer = f^ -> number^ { return^ 3 }\n";
        LHAT_CHECK_EQ_INT(lhat_reload(&program, "lib.lh", machines, 2), 2);
        LHAT_CHECK(lhat_program_retired_count(&program) > 0,
                   "the held bodies wait");
        // And what was held still runs -- the whole point of waiting.
        LHAT_CHECK_EQ_INT(
            lhat_as_integer(lhat_machine_call(two, keep, NULL, 0).value), 2);

        // Dropped, the next reload takes the whole backlog with it.
        LHAT_CHECK(lhat_machine_set_global(two, "Keep", lhat_nil()),
                   "dropped");
        live[0].text = "module^ ns.lib\n"
                       "public^ let^ answer = f^ -> number^ { return^ 4 }\n";
        LHAT_CHECK_EQ_INT(lhat_reload(&program, "lib.lh", machines, 2), 2);
        LHAT_CHECK_EQ_INT(lhat_program_retired_count(&program), 0);
        LHAT_CHECK_EQ_INT(
            lhat_as_integer(
                lhat_machine_call(
                    two, lhat_run(two, lhat_unit_proto(root)).value, NULL, 0)
                    .value),
            4);

        lhat_machine_dispose(one);
        lhat_machine_dispose(two);
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
    test_diagnostics();
    test_composing_across_units();
    test_annotations();
    test_annotation_exclusion();
    test_annotation_requisite();
    test_empty_body();
    test_delegate_among_the_members();
#if LHAT_WITH_COMMENTS
    test_documentation();
#endif
    test_running();
    test_reloading();
    test_reloading_with_a_pending_cleanup();
    test_reload_call();
    test_hosting();
    test_hostvalue_escape();
    test_host_tuple();
    test_host_data();
    test_host_data_identity();
    test_reloading_keeps_registrations();
    test_declarations_are_the_process_s();
    test_host_data_release();
    test_dump_host_api();
#if LHAT_WITH_RESOLUTIONS
    test_where_a_published_member_was_written();
#endif
    return lhat_test_report("test_program");
}
