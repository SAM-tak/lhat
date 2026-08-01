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

int main(void)
{
    test_dependencies();
    test_loading();
    test_cycles();
    return lhat_test_report("test_program");
}
