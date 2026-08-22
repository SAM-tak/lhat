// L^ (lhat) -- tests for the coroutine at the host boundary (05 の 8.8).
//
// Section numbers refer to DesignDocuments/05-modules.md unless prefixed.
// What is pinned: a coroutine whose body the host wrote in C
// (lhat_machine_make_coroutine) lets a for^ walk a hostdata value through a
// registered iterate^; any coroutine is drivable from C
// (lhat_machine_resume / lhat_machine_coroutine_done); a yieldable
// procedure called across the boundary answers its coroutine
// (lhat_machine_call); and the release of a host walk's state runs exactly
// once whichever way the walk ends -- its natural end, an explicit
// dispose(), or the machine going away.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "program_internal.h"
#include "fixture.h"
#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

// ---------------------------------------------------------------------------
// The program fixture, as test_program.c keeps it: a disk of literal files.

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

static void program_with(LhatProgram *program, Disk *disk, const File *files,
                         size_t count)
{
    disk->files = files;
    disk->count = count;
    lhat_program_init(program, true, disk_load, disk);
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

// ---------------------------------------------------------------------------
// seq.Range: a hostdata value the host can walk -- the shape of a Godot
// packed array, cut down to two numbers.

typedef struct {
    int64_t from;
    int64_t to;  // inclusive
} Range;

// One walk of one Range -- made per iterate() call, freed by the release.
typedef struct {
    int64_t at;
    int64_t to;
    bool pair;        // yield (ordinal, value) tuples instead of values
    int64_t ordinal;  // 1, 2, 3 ... when `pair`
} RangeWalk;

static const LhatHostDataTag *range_tag;
static const LhatHostDataTag *pairs_tag;
static Range range_value;     // one per test; make() rewrites it
static int walk_releases;     // how many times the release ran
static LhatValue last_sent;   // what the most recent step was handed

static bool range_step(LhatMachine *machine, void *context, LhatValue sent,
                       LhatValue *out)
{
    RangeWalk *walk = (RangeWalk *)context;
    last_sent = sent;
    if (walk->at > walk->to) {
        return false;
    }
    if (walk->pair) {
        LhatValue pair[2] = {lhat_integer(walk->ordinal++),
                             lhat_integer(walk->at++)};
        return lhat_make_tuple(machine, pair, 2, out);
    }
    *out = lhat_integer(walk->at++);
    return true;
}

static LhatValue walk_release(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    walk_releases++;
    free(context);
    return lhat_nil();
}

static LhatValue make_walk(LhatMachine *machine, LhatValue over,
                           const LhatHostDataTag *tag, bool pair)
{
    Range *self = (Range *)lhat_hostdata_pointer(over, tag);
    if (self == NULL) {
        return lhat_nil();
    }
    RangeWalk *walk = (RangeWalk *)malloc(sizeof *walk);
    if (walk == NULL) {
        return lhat_nil();
    }
    walk->at = self->from;
    walk->to = self->to;
    walk->pair = pair;
    walk->ordinal = 1;
    LhatValue out = lhat_nil();
    // `over` rides along as `held`, so the collector keeps the hostdata for
    // as long as the walk lives -- 8.8's recipe.
    if (!lhat_machine_make_coroutine(machine, range_step, walk, walk_release,
                                     over, &out)) {
        free(walk);
        return lhat_nil();
    }
    return out;
}

static LhatValue range_iterate(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return make_walk(machine, arguments[0], range_tag, false);
}

static LhatValue pairs_iterate(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return make_walk(machine, arguments[0], pairs_tag, true);
}

static LhatValue range_span(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    Range *self = (Range *)lhat_hostdata_pointer(arguments[0], range_tag);
    if (self == NULL) {
        return lhat_nil();
    }
    return lhat_integer(self->to - self->from + 1);
}

static LhatValue range_make_with(LhatMachine *machine,
                                 const LhatHostDataTag *tag,
                                 const LhatValue *arguments)
{
    range_value.from = lhat_as_integer(arguments[0]);
    range_value.to = lhat_as_integer(arguments[1]);
    LhatValue out = lhat_nil();
    return lhat_machine_make_hostdata(machine, tag, &range_value, &out)
               ? out
               : lhat_nil();
}

static LhatValue range_make(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return range_make_with(machine, range_tag, arguments);
}

static LhatValue pairs_make(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return range_make_with(machine, pairs_tag, arguments);
}

// Runs `main.lh` of an already-registered program and answers the result.
// The machine is disposed unless `keep` takes it over.
static LhatRunResult run_program(LhatProgram *program, LhatMachine **keep)
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
    if (keep != NULL) {
        *keep = machine;
    } else {
        lhat_machine_dispose(machine);
    }
    return ran;
}

// ---------------------------------------------------------------------------
// 8.8: `for^ x in^ value` walks what a registered iterate^ answers.

static void test_walks_hostdata(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("for^ walks a hostdata value through a registered iterate^");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.make(3, 6)\n"
             "var^ total = 0\n"
             "for^ x in^ r { total := total + x }\n"
             "return^ total\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        LHAT_CHECK(range_tag != NULL, "the type registered");
        lhat_register_member(&program, "seq", "Range", "iterate^",
                             "f^self^ -> c^{p^ -> number^};",
                             range_iterate, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 18);  // 3+4+5+6
        // The walk ran out, so its state went back right there -- once.
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }
    lhat_program_dispose(&program);

    // 02 の 14 章 (ハットは名前の一部): everywhere but a plain table the two
    // spellings reach the same member, so a bare registration is the same
    // declaration -- checked and run alike.
    LHAT_TEST("and a bare `iterate` registration reads the same");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.make(1, 3)\n"
             "var^ total = 0\n"
             "for^ x in^ r { total := total + x }\n"
             "return^ total\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        lhat_register_member(&program, "seq", "Range", "iterate",
                             "f^self^ -> c^{p^ -> number^};",
                             range_iterate, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 6);
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }
    lhat_program_dispose(&program);

    // 02 の 13.8改: a step may answer lhat_make_tuple's tuple, which is what
    // a `for^ k, v` walk takes apart.
    LHAT_TEST("and a step yielding a tuple feeds for^ k, v");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.make(10, 12)\n"
             "var^ ordinals = 0\n"
             "var^ values = 0\n"
             "for^ k, v in^ r {\n"
             "    ordinals := ordinals + k\n"
             "    values := values + v\n"
             "}\n"
             "return^ ordinals * 1000 + values\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        pairs_tag = lhat_register_hostdata_type(&program, "seq", "Pairs");
        lhat_register_member(&program, "seq", "Pairs", "iterate^",
                             "f^self^ -> c^{p^ -> (number^, number^)};",
                             pairs_iterate, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Pairs;", pairs_make,
                           NULL);
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // ordinals 1+2+3 = 6, values 10+11+12 = 33
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 6033);
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }
    lhat_program_dispose(&program);

    // 02 の 14.17改: everywhere but a plain table the two spellings are one
    // member, in both directions -- the bare call reaches the hatted
    // registration too.
    LHAT_TEST("and the bare .iterate() reaches a hatted registration");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ w = seq.make(4, 6).iterate()\n"
             "var^ first = w.start() ?? 0\n"
             "return^ first\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        lhat_register_member(&program, "seq", "Range", "iterate^",
                             "f^self^ -> c^{p^ -> number^};",
                             range_iterate, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 4);
    }
    lhat_program_dispose(&program);

    // A hostdata type is a table only in how its members are reached; one
    // that registered no iterate has nothing behind it to walk, and the
    // checker says so rather than leaving the loop to walk an empty table.
    LHAT_TEST("and one that registered no iterate is refused");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.make(1, 3)\n"
             "for^ x in^ r { }\n"},
        };
        program_with(&program, &disk, files, 1);
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_NOT_COROUTINE),
                   "nothing to walk");
    }
    lhat_program_dispose(&program);
}

// ---------------------------------------------------------------------------
// The other two ways a walk ends: an explicit dispose(), and the machine.

static void test_release_once(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("dispose() gives a host walk's state back, once");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ fresh = seq.make(1, 9).iterate^()\n"
             "fresh.dispose()\n"
             "var^ opened = seq.make(1, 9).iterate^()\n"
             "opened.start()\n"
             "opened.dispose()\n"
             "opened.dispose()\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        lhat_register_member(&program, "seq", "Range", "iterate^",
                             "f^self^ -> c^{p^ -> number^};",
                             range_iterate, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // One release per walk -- a fresh one, and a started one disposed
        // twice. The machine's own end added none: both were released.
        LHAT_CHECK_EQ_INT(walk_releases, 2);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and the machine's end is the last resort");
    {
        LhatMachine *m = lhat_machine_new();
        RangeWalk *walk = (RangeWalk *)malloc(sizeof *walk);
        walk->at = 1;
        walk->to = 5;
        walk->pair = false;
        walk->ordinal = 1;
        walk_releases = 0;
        LhatValue co = lhat_nil();
        LHAT_CHECK(lhat_machine_make_coroutine(m, range_step, walk,
                                               walk_release, lhat_nil(), &co),
                   "made");
        // One step taken, the walk still open when the machine goes.
        LhatRunResult one = lhat_machine_resume(m, co, NULL, 0);
        LHAT_CHECK_EQ_INT(one.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(one.value), 1);
        LHAT_CHECK_EQ_INT(walk_releases, 0);
        lhat_machine_dispose(m);
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }
}

// ---------------------------------------------------------------------------
// lhat_machine_resume: any coroutine, driven from C.

static void test_driven_from_c(void)
{
    LHAT_TEST("a host coroutine is made and driven from C");
    {
        LhatMachine *m = lhat_machine_new();
        RangeWalk *walk = (RangeWalk *)malloc(sizeof *walk);
        walk->at = 1;
        walk->to = 3;
        walk->pair = false;
        walk->ordinal = 1;
        walk_releases = 0;
        LhatValue co = lhat_nil();
        LHAT_CHECK(lhat_machine_make_coroutine(m, range_step, walk,
                                               walk_release, lhat_nil(), &co),
                   "made");
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "not done yet");
        int64_t sum = 0;
        for (int i = 0; i < 3; i++) {
            LhatRunResult step = lhat_machine_resume(m, co, NULL, 0);
            LHAT_CHECK_EQ_INT(step.status, LHAT_RUN_OK);
            sum += lhat_as_integer(step.value);
        }
        LHAT_CHECK_EQ_INT(sum, 6);
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "the end not seen yet");
        LhatRunResult over = lhat_machine_resume(m, co, NULL, 0);
        LHAT_CHECK_EQ_INT(over.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_nil(over.value), "the end answers nil^");
        LHAT_CHECK(lhat_machine_coroutine_done(co), "done");
        LHAT_CHECK_EQ_INT(walk_releases, 1);
        // 15.2: a finished coroutine faults, at this boundary as anywhere.
        LhatRunResult dead = lhat_machine_resume(m, co, NULL, 0);
        LHAT_CHECK_EQ_INT(dead.status, LHAT_RUN_DEAD_COROUTINE);
        lhat_machine_dispose(m);
        LHAT_CHECK_EQ_INT(walk_releases, 1);  // and was not released again
    }

    LHAT_TEST("and what resume sends reaches the step");
    {
        LhatMachine *m = lhat_machine_new();
        RangeWalk *walk = (RangeWalk *)malloc(sizeof *walk);
        walk->at = 1;
        walk->to = 100;
        walk->pair = false;
        walk->ordinal = 1;
        walk_releases = 0;
        LhatValue co = lhat_nil();
        lhat_machine_make_coroutine(m, range_step, walk, walk_release,
                                    lhat_nil(), &co);
        last_sent = lhat_nil();
        LhatValue told = lhat_integer(42);
        lhat_machine_resume(m, co, &told, 1);
        LHAT_CHECK(lhat_is_integer(last_sent), "the step saw a value");
        LHAT_CHECK_EQ_INT(lhat_as_integer(last_sent), 42);
        lhat_machine_dispose(m);
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }

    // 02 の 13.8改: a tuple crosses the boundary as positions, `value`
    // standing as position 1 the way lhat_run already answers one.
    LHAT_TEST("and a tuple-yielding step crosses as positions");
    {
        LhatMachine *m = lhat_machine_new();
        RangeWalk *walk = (RangeWalk *)malloc(sizeof *walk);
        walk->at = 7;
        walk->to = 7;
        walk->pair = true;
        walk->ordinal = 1;
        walk_releases = 0;
        LhatValue co = lhat_nil();
        lhat_machine_make_coroutine(m, range_step, walk, walk_release,
                                    lhat_nil(), &co);
        LhatRunResult step = lhat_machine_resume(m, co, NULL, 0);
        LHAT_CHECK_EQ_INT(step.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(step.position_count, 2);
        if (step.position_count == 2) {
            LHAT_CHECK_EQ_INT(lhat_as_integer(step.positions[0]), 1);
            LHAT_CHECK_EQ_INT(lhat_as_integer(step.positions[1]), 7);
            LHAT_CHECK_EQ_INT(lhat_as_integer(step.value), 1);
        }
        lhat_machine_dispose(m);
    }

    // 16.3: a table's walk is a coroutine too, so the same call drives one.
    LHAT_TEST("and a table's walk is drivable the same way");
    {
        Run r;
        run_text(&r,
                 "var^ t = { 5, 6, 7 }\n"
                 "return^ t.iterate^()\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_COROUTINE),
                   "a walk came back");
        LhatValue co = r.ran.value;
        LhatRunResult one = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(one.status, LHAT_RUN_OK);
        // The pair crosses as positions: (1, 5).
        LHAT_CHECK_EQ_INT(one.position_count, 2);
        if (one.position_count == 2) {
            LHAT_CHECK_EQ_INT(lhat_as_integer(one.positions[0]), 1);
            LHAT_CHECK_EQ_INT(lhat_as_integer(one.positions[1]), 5);
        }
        lhat_machine_resume(r.machine, co, NULL, 0);
        lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "three pairs taken");
        LhatRunResult over = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(over.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_nil(over.value), "the end answers nil^");
        LHAT_CHECK(lhat_machine_coroutine_done(co), "done");
        run_dispose(&r);
    }
}

// ---------------------------------------------------------------------------
// 02 の 15.5 at the boundary: calling a yieldable procedure answers its
// coroutine (lua_newthread's shape), and resume drives the body on the
// machine's own frames.

static void test_yieldable_call(void)
{
    LHAT_TEST("calling a yieldable procedure from C answers its coroutine");
    {
        Run r;
        run_text(&r,
                 "var^ gen = p^ from:number^ {\n"
                 "    yield^ from\n"
                 "    yield^ from + 1\n"
                 "    return^ from + 2\n"
                 "}\n"
                 "return^ gen\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        LhatValue args[1] = {lhat_integer(5)};
        LhatRunResult made = lhat_machine_call(r.machine, r.ran.value, args, 1);
        LHAT_CHECK_EQ_INT(made.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(made.value, LHAT_OBJECT_COROUTINE),
                   "a coroutine came back, the body not started");
        LhatValue co = made.value;
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "fresh");

        // Resume subsumes start: the first one runs the body from the top.
        LhatRunResult one = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(one.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(one.value), 5);
        LhatRunResult two = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(lhat_as_integer(two.value), 6);
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "still suspended");
        // 13.9: the last answer is the return's -- union(Y, T), told apart
        // by done() and nothing else.
        LhatRunResult three = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(three.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(three.value), 7);
        LHAT_CHECK(lhat_machine_coroutine_done(co), "the return ended it");
        LhatRunResult dead = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(dead.status, LHAT_RUN_DEAD_COROUTINE);
        run_dispose(&r);
    }

    LHAT_TEST("and what resume sends arrives at the yield^");
    {
        Run r;
        run_text(&r,
                 "var^ echo = p^ {\n"
                 "    var^ got = yield^ 1\n"
                 "    yield^ got * 10\n"
                 "}\n"
                 "return^ echo\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        LhatRunResult made =
            lhat_machine_call(r.machine, r.ran.value, NULL, 0);
        LHAT_CHECK_EQ_INT(made.status, LHAT_RUN_OK);
        LhatValue co = made.value;
        // The first resume's sent is discarded -- no yield^ awaits it yet.
        LhatRunResult one = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(lhat_as_integer(one.value), 1);
        LhatValue four = lhat_integer(4);
        LhatRunResult two = lhat_machine_resume(r.machine, co, &four, 1);
        LHAT_CHECK_EQ_INT(two.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(two.value), 40);
        LhatRunResult over = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(over.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_nil(over.value), "the body ended with no value");
        LHAT_CHECK(lhat_machine_coroutine_done(co), "done");
        run_dispose(&r);
    }

    // 13.8改: and a resume may send several -- the run arrives whole at the
    // yield^'s own binding, from C exactly as from a written resume(a, b).
    LHAT_TEST("and a several-send crosses the boundary");
    {
        Run r;
        run_checked_text(&r,
                         "var^ gen = p^ {\n"
                         "    let^ a:number^, b:number^ = yield^ 0\n"
                         "    return^ a * 100 + b\n"
                         "}\n"
                         "return^ gen\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        LhatRunResult made =
            lhat_machine_call(r.machine, r.ran.value, NULL, 0);
        LHAT_CHECK_EQ_INT(made.status, LHAT_RUN_OK);
        LhatValue co = made.value;
        LhatRunResult one = lhat_machine_resume(r.machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(one.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(one.value), 0);
        LhatValue pair[2] = {lhat_integer(3), lhat_integer(4)};
        LhatRunResult two = lhat_machine_resume(r.machine, co, pair, 2);
        LHAT_CHECK_EQ_INT(two.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(two.value), 304);
        LHAT_CHECK(lhat_machine_coroutine_done(co), "done");

        // 13.9: the count a resume sends is the R's, at this boundary too.
        LhatRunResult again =
            lhat_machine_call(r.machine, r.ran.value, NULL, 0);
        LhatValue other = again.value;
        lhat_machine_resume(r.machine, other, NULL, 0);
        LhatValue lone = lhat_integer(1);
        LhatRunResult wrong = lhat_machine_resume(r.machine, other, &lone, 1);
        LHAT_CHECK_EQ_INT(wrong.status, LHAT_RUN_ARITY);
        run_dispose(&r);
    }
}

// ---------------------------------------------------------------------------
// lhat_machine_call_member on a hostdata value: its members are the
// registered type's, reached the way an instruction reaches them.

static void test_call_member_hostdata(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a hostdata member is callable from the host");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "return^ seq.make(2, 5)\n"},
        };
        program_with(&program, &disk, files, 1);
        range_tag = lhat_register_hostdata_type(&program, "seq", "Range");
        lhat_register_member(&program, "seq", "Range", "span",
                             "f^self^ -> number^;", range_span, NULL);
        lhat_register_func(&program, "seq", "make",
                           "f^number^, number^ -> seq.Range;", range_make,
                           NULL);
        LhatMachine *machine = NULL;
        LhatRunResult ran = run_program(&program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(ran.value, LHAT_OBJECT_HOSTDATA),
                   "the hostdata came back");
        LhatRunResult spanned = lhat_machine_call_member(
            machine, ran.value, "span", 4, NULL, 0);
        LHAT_CHECK_EQ_INT(spanned.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(spanned.value), 4);  // 2..5
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);
}

// ---------------------------------------------------------------------------
// 8.9 meets 8.8: a walk whose steps answer host values. Each is written out
// whole into the focus, so two elements alive at once cannot share the one
// scratch the boundary owns -- which is exactly what this pins.

typedef struct {
    float x;
} Vec;

typedef struct {
    int64_t at;   // 1-origin
    int64_t to;
    bool pair;    // yield (ordinal, value) -- refused by the checker
} VecWalk;

static const LhatHostValueTag *vec_tag;

static bool vec_step(LhatMachine *machine, void *context, LhatValue sent,
                     LhatValue *out)
{
    (void)sent;
    VecWalk *walk = (VecWalk *)context;
    if (walk->at > walk->to) {
        return false;
    }
    Vec v;
    v.x = (float)(walk->at * 10);
    walk->at++;
    LhatValue value = lhat_nil();
    if (!lhat_make_hostvalue(machine, vec_tag, &v, &value)) {
        return false;
    }
    if (walk->pair) {
        LhatValue both[2] = {lhat_integer(walk->at - 1), value};
        return lhat_make_tuple(machine, both, 2, out);
    }
    *out = value;
    return true;
}

static LhatValue vec_iterate_with(LhatMachine *machine, LhatValue over,
                                  bool pair)
{
    VecWalk *walk = (VecWalk *)malloc(sizeof *walk);
    if (walk == NULL) {
        return lhat_nil();
    }
    walk->at = 1;
    walk->to = 3;
    walk->pair = pair;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_coroutine(machine, vec_step, walk, walk_release,
                                     over, &out)) {
        free(walk);
        return lhat_nil();
    }
    return out;
}

static LhatValue vec_iterate(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return vec_iterate_with(machine, arguments[0], false);
}

static LhatValue vec_pair_iterate(LhatMachine *machine, void *context,
                                  const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    return vec_iterate_with(machine, arguments[0], true);
}

static LhatValue vec_seq_make(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)arguments;
    (void)count;
    range_value.from = 1;
    range_value.to = 3;
    LhatValue out = lhat_nil();
    return lhat_machine_make_hostdata(machine, range_tag, &range_value, &out)
               ? out
               : lhat_nil();
}

static void test_walk_yields_host_values(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a walk hands each host value over whole");
    {
        // Three elements, summed off the focus: were the focus still a
        // pointer into the boundary's scratch, every turn would read the
        // value the latest step wrote and the sum would be 3 * 30.
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.vecs()\n"
             "var^ total = 0\n"
             "for^ v in^ r { total := total + v.x }\n"
             "return^ total\n"},
        };
        program_with(&program, &disk, files, 1);
        walk_releases = 0;
        range_tag = lhat_register_hostdata_type(&program, "seq", "Vecs");
        LHAT_CHECK(range_tag != NULL, "the hostdata type registered");
        vec_tag = lhat_register_hostvalue_type(&program, "seq", "Vec",
                                               sizeof(Vec));
        LHAT_CHECK(vec_tag != NULL, "the value type registered");
        LHAT_CHECK(lhat_register_hostvalue_field(&program, "seq", "Vec", "x",
                                                 offsetof(Vec, x),
                                                 LHAT_HVFIELD_F32),
                   "the field registered");
        LHAT_CHECK(lhat_register_member(&program, "seq", "Vecs", "iterate",
                                        "f^self^ -> c^{p^ -> seq.Vec};",
                                        vec_iterate, NULL),
                   "the iterate registered");
        LHAT_CHECK(lhat_register_func(&program, "seq", "vecs",
                                      "f^ -> seq.Vecs;", vec_seq_make, NULL),
                   "the maker registered");
        LhatRunResult ran = run_program(&program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // 10 + 20 + 30, not 30 + 30 + 30.
        LHAT_CHECK_EQ_INT((int64_t)lhat_as_real(ran.value), 60);
        LHAT_CHECK_EQ_INT(walk_releases, 1);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("a host value in a tuple position is refused as an escape");
    {
        // 8.9: a tuple crosses as copied values, and a host value among them
        // would arrive as a pointer into scratch the next step overwrites.
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "var^ r = seq.vecs()\n"
             "for^ i, v in^ r { }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        range_tag = lhat_register_hostdata_type(&program, "seq", "Vecs");
        vec_tag = lhat_register_hostvalue_type(&program, "seq", "Vec",
                                               sizeof(Vec));
        LHAT_CHECK(lhat_register_member(
                       &program, "seq", "Vecs", "iterate",
                       "f^self^ -> c^{p^ -> (number^, seq.Vec)};",
                       vec_pair_iterate, NULL),
                   "the pair iterate registered");
        LHAT_CHECK(lhat_register_func(&program, "seq", "vecs",
                                      "f^ -> seq.Vecs;", vec_seq_make, NULL),
                   "the maker registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_check_error(root, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES),
                   "the escape is refused");
    }
    lhat_program_dispose(&program);
}

// ---------------------------------------------------------------------------
// 8.9改2: the C boundary passes host values whole -- resume's answers as the
// pointer form aimed at the machine's scratch (the tuple positions'
// lifetime), and resume's sends expanded into the suspended frame.

static const LhatHostValueTag *vec_other_tag;

static float float_at(const void *data)
{
    float f;
    memcpy(&f, data, sizeof f);
    return f;
}

static LhatValue vec_mk(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)count;
    Vec v;
    v.x = lhat_is_real(arguments[0])
              ? (float)lhat_as_real(arguments[0])
              : (float)lhat_as_integer(arguments[0]);
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, vec_tag, &v, &out) ? out : lhat_nil();
}

static void test_boundary_host_values(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a yield^'s host value crosses to the C caller whole");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "let^ gen = f^ -> c^{f^ -> seq.Vec -> seq.Vec} {\n"
             "    yield^ seq.mk(1.5)\n"
             "    return^ seq.mk(2.5)\n"
             "}\n"
             "return^ gen\n"},
        };
        program_with(&program, &disk, files, 1);
        vec_tag = lhat_register_hostvalue_type(&program, "seq", "Vec",
                                               sizeof(Vec));
        LHAT_CHECK(lhat_register_hostvalue_field(&program, "seq", "Vec", "x",
                                                 offsetof(Vec, x),
                                                 LHAT_HVFIELD_F32),
                   "the field registered");
        LHAT_CHECK(lhat_register_func(&program, "seq", "mk",
                                      "f^number^ -> seq.Vec;", vec_mk, NULL),
                   "the maker registered");
        LhatMachine *machine = NULL;
        LhatRunResult ran = run_program(&program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LhatRunResult made =
            lhat_machine_call(machine, ran.value, NULL, 0);
        LhatValue co = made.value;

        LhatRunResult one = lhat_machine_resume(machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(one.status, LHAT_RUN_OK);
        const void *first = lhat_hostvalue_data(one.value, vec_tag);
        LHAT_CHECK(first != NULL, "the yield's value came over whole");
        LHAT_CHECK(first == NULL || float_at(first) == 1.5f, "its bytes");
        LHAT_CHECK(!lhat_machine_coroutine_done(co), "still suspended");

        // 13.9: the return's T crosses the same way, done() telling it
        // apart. The scratch is one room -- the earlier pointer now reads
        // the later value, which is the "copy it out to keep it" contract
        // the tuple positions already have.
        LhatRunResult two = lhat_machine_resume(machine, co, NULL, 0);
        LHAT_CHECK_EQ_INT(two.status, LHAT_RUN_OK);
        const void *second = lhat_hostvalue_data(two.value, vec_tag);
        LHAT_CHECK(second != NULL, "the return's value came over whole");
        LHAT_CHECK(second == NULL || float_at(second) == 2.5f, "its bytes");
        LHAT_CHECK(lhat_machine_coroutine_done(co), "the return ended it");
        LHAT_CHECK(first == second, "one scratch, not a copy per answer");
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and a resume's send arrives at the yield^ whole");
    {
        static const File files[] = {
            {"main.lh",
             "import^ seq\n"
             "let^ echo = f^ -> c^{f^seq.Vec -> number^} {\n"
             "    var^ got : seq.Vec = yield^ 0\n"
             "    yield^ got.x\n"
             "}\n"
             "return^ echo\n"},
        };
        program_with(&program, &disk, files, 1);
        vec_tag = lhat_register_hostvalue_type(&program, "seq", "Vec",
                                               sizeof(Vec));
        vec_other_tag = lhat_register_hostvalue_type(&program, "seq", "Other",
                                                     sizeof(Vec));
        LHAT_CHECK(lhat_register_hostvalue_field(&program, "seq", "Vec", "x",
                                                 offsetof(Vec, x),
                                                 LHAT_HVFIELD_F32),
                   "the field registered");
        LhatMachine *machine = NULL;
        LhatRunResult ran = run_program(&program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LhatValue co =
            lhat_machine_call(machine, ran.value, NULL, 0).value;
        lhat_machine_resume(machine, co, NULL, 0);  // to the first yield^

        Vec v;
        v.x = 4.5f;
        LhatValue sent = lhat_nil();
        LHAT_CHECK(lhat_make_hostvalue(machine, vec_tag, &v, &sent),
                   "the send was made");
        LhatRunResult got = lhat_machine_resume(machine, co, &sent, 1);
        LHAT_CHECK_EQ_INT(got.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_real(got.value) &&
                       lhat_as_real(got.value) == 4.5,
                   "the body read the sent value's field");

        // The R's own tag holds the send: another registration's value is
        // refused, and so is one mixed into a run.
        LhatValue wrong = lhat_nil();
        LHAT_CHECK(lhat_make_hostvalue(machine, vec_other_tag, &v, &wrong),
                   "the stranger was made");
        LhatRunResult refused = lhat_machine_resume(machine, co, &wrong, 1);
        LHAT_CHECK_EQ_INT(refused.status, LHAT_RUN_TYPE_ERROR);
        LhatValue mixed[2];
        LHAT_CHECK(lhat_make_hostvalue(machine, vec_tag, &v, &mixed[0]),
                   "the mixed one was made");
        mixed[1] = lhat_integer(1);
        LhatRunResult two_seats =
            lhat_machine_resume(machine, co, mixed, 2);
        LHAT_CHECK_EQ_INT(two_seats.status, LHAT_RUN_TYPE_ERROR);
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);

    LHAT_TEST("and a host walk's value passes straight through");
    {
        static const File files[] = {
            {"main.lh", "import^ seq\nreturn^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        vec_tag = lhat_register_hostvalue_type(&program, "seq", "Vec",
                                               sizeof(Vec));
        LhatMachine *machine = NULL;
        LhatRunResult ran = run_program(&program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        walk_releases = 0;
        LhatValue walk = vec_iterate_with(machine, lhat_nil(), false);
        LhatRunResult step = lhat_machine_resume(machine, walk, NULL, 0);
        LHAT_CHECK_EQ_INT(step.status, LHAT_RUN_OK);
        const void *bytes = lhat_hostvalue_data(step.value, vec_tag);
        LHAT_CHECK(bytes != NULL, "the step's value came over whole");
        LHAT_CHECK(bytes == NULL || float_at(bytes) == 10.0f, "its bytes");
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);
}

int main(void)
{
    test_walks_hostdata();
    test_release_once();
    test_driven_from_c();
    test_yieldable_call();
    test_call_member_hostdata();
    test_walk_yields_host_values();
    test_boundary_host_values();
    return lhat_test_report("test_host_coroutine");
}
