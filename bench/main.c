// L^ (lhat) -- what a member read costs, measured rather than assumed.
//
// 03 の 5.1 foretold specialised instructions for places where the type is
// settled, the first of them (5.11c) being in. The next candidate is the
// member read: 'x.m' is LHAT_BC_GETINDEX, a probe of the receiver's table.
// Replacing it with a slot read is a large change either way -- a vtable-like
// layout for the static form, a cache area in every chunk for the dynamic one
// -- and large changes want a number in front of them.
//
// WHAT THIS ANSWERS. Every case runs the same L^ loop with a different body,
// so the loop itself cancels in the differences:
//
//     member read = (a host member call) - (the same C function called
//                                           as a module function)
//     host call   = (that module function) - (a local f^ that does nothing)
//
// The ratio of those two is the input to the decision, and it does not depend
// on any particular host: what a binding would save by specialising the
// member read, against the boundary step that cannot be removed at all
// (the core will never know about ptrcall; the registered LhatHostFn is the
// boundary).
//
// This is not a test. A claim about time is not a thing that passes or fails,
// so it prints and asserts nothing -- with one exception, marked below.
// Build it with -DLHAT_BUILD_BENCH=ON and read the numbers; the numbers
// belong to the machine that produced them and to nothing else.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"
#include "port/thread.h"

// How many turns of the loop each case takes. Large enough that the
// millisecond clock (lhat_now_ms) has something to measure.
#define TURNS 3000000

// ---------------------------------------------------------------------------
// The program fixture the tests keep: a disk of literal files.

typedef struct {
    const char *path;
    const char *text;
} File;

static char *disk_load(void *context, const char *path, size_t *length)
{
    const File *file = (const File *)context;
    if (strcmp(file->path, path) != 0) {
        return NULL;
    }
    size_t size = strlen(file->text);
    char *copy = (char *)malloc(size + 1);
    if (copy != NULL) {
        memcpy(copy, file->text, size + 1);
        *length = size;
    }
    return copy;
}

// ---------------------------------------------------------------------------
// bench.Thing: a host type with a member, and the same C function again as a
// module function. Answering a constant is the point -- what is being timed
// is the way in, not the work.

typedef struct {
    int64_t value;
} Thing;

static const LhatHostDataTag *thing_tag;
static const LhatHostDataTag *derived_tag;
static Thing the_thing;

static void thing_read(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    const Thing *self = (const Thing *)lhat_hostdata_pointer(arguments[0],
                                                             thing_tag);
    answers[0] = self != NULL ? lhat_integer(self->value) : lhat_nil();
    *answer_count = 1;
}

static void make_thing(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    lhat_machine_make_hostdata(machine, (const LhatHostDataTag *)context,
                               &the_thing, &out);
    answers[0] = out;
    *answer_count = 1;
}

// 05 の 8.7: the boundary itself, with nothing behind it. Registered under
// several arities so that what an argument COSTS can be read off the slope
// -- the machine holds its stack split into payloads and tags (2.2), so a
// host taking `const LhatValue *` has its arguments rebuilt into one array
// on every call (vm.c's `gathered`). That rebuilding is what a boundary
// reading the slots by index would not do, and this is what says how much
// of a call it is.
static void answer_nothing(LhatMachine *machine, void *context,
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

static bool register_bench(LhatProgram *program)
{
    thing_tag = lhat_register_hostdata_type(program, "bench", "Thing");
    // 05 の 8.8改: the inherited case reads a member the base declared, which
    // the flatten put into this type's own table. Case 6 is what says that
    // costs the same as case 1 rather than adding a step.
    derived_tag = lhat_register_hostdata_subtype(program, "bench", "Derived",
                                                 "bench", "Thing");
    if (thing_tag == NULL || derived_tag == NULL) {
        return false;
    }
    return lhat_register_member(program, "bench", "Thing", "read",
                                "f^self^ -> number^;", thing_read, NULL) &&
           lhat_register_func(program, "bench", "read1",
                              "f^bench.Thing -> number^;", thing_read, NULL) &&
           lhat_register_func(program, "bench", "make",
                              "f^ -> bench.Thing;", make_thing,
                              (void *)thing_tag) &&
           lhat_register_func(program, "bench", "makeDerived",
                              "f^ -> bench.Derived;", make_thing,
                              (void *)derived_tag) &&
           lhat_register_func(program, "bench", "a0",
                              "f^ -> number^;", answer_nothing, NULL) &&
           lhat_register_func(program, "bench", "a1",
                              "f^number^ -> number^;", answer_nothing,
                              NULL) &&
           lhat_register_func(program, "bench", "a2",
                              "f^number^, number^ -> number^;",
                              answer_nothing, NULL) &&
           lhat_register_func(program, "bench", "a4",
                              "f^number^, number^, number^, number^"
                              " -> number^;", answer_nothing, NULL) &&
           lhat_register_func(program, "bench", "a8",
                              "f^number^, number^, number^, number^,"
                              " number^, number^, number^, number^"
                              " -> number^;", answer_nothing, NULL);
}

// ---------------------------------------------------------------------------
// One case: the same loop, a different body.

// `r1` and `loc` are bound to locals on purpose. Calling one of those reads
// no member at all, so the difference from `h.read()` is the receiver's
// member read and nothing else -- and all three then call something with one
// value in hand, so the argument costs the same everywhere.
//
// Reaching the module function through `bench.read1(h)` would not isolate
// anything: that reads a member off the module table on the way.
#define PREAMBLE                                                      \
    "import^ bench\n"                                                 \
    "let^ h = bench.make()\n"                                         \
    "let^ d = bench.makeDerived()\n"                                  \
    "let^ r1 = bench.read1\n"                                         \
    "let^ loc = f^ x:bench.Thing -> number^ { 1 }\n"                  \
    "let^ t = { k = 1 }\n"                                            \
    "let^ Box = def^{ read = f^self^ -> number^ { 1 } }\n"            \
    "let^ inst = Box.new()\n"                                         \
    "let^ a0 = bench.a0\n"    \
    "let^ a1 = bench.a1\n"    \
    "let^ a2 = bench.a2\n"    \
    "let^ a4 = bench.a4\n"    \
    "let^ a8 = bench.a8\n"    \
    "let^ L0 = f^  -> number^ { 1 }\n" \
    "let^ L1 = f^ p1:number^ -> number^ { 1 }\n" \
    "let^ L2 = f^ p1:number^, p2:number^ -> number^ { 1 }\n" \
    "let^ L4 = f^ p1:number^, p2:number^, p3:number^, p4:number^ -> number^ { 1 }\n" \
    "let^ L8 = f^ p1:number^, p2:number^, p3:number^, p4:number^, p5:number^, p6:number^, p7:number^, p8:number^ -> number^ { 1 }\n" \
    "var^ sink = 0\n"                                                 \
    "var^ i = 0\n"                                                    \
    "repeat^while^ i < "

// `body` is written inside the loop; `sink` keeps the answer from being
// something a compiler could decide is unwanted.
//
// Answers nanoseconds for one turn, or 0 where it could not be built.
// 09 の 2.2: a hook that does nothing, to price what an instruction pays
// while one is set -- the test, the frame lookup and the call -- against the
// same loop with none (the one not-taken branch every other case already
// pays). Set for the hooked case alone.
static void bench_hook(LhatMachine *machine, void *context,
                       LhatDebugEvent event, const LhatFrameInfo *where)
{
    (void)machine;
    (void)context;
    (void)event;
    (void)where;
}

static double time_once_hooked(const char *name, const char *body, bool hooked);

static double time_once(const char *name, const char *body)
{
    return time_once_hooked(name, body, false);
}

static double time_once_hooked(const char *name, const char *body, bool hooked)
{
    char source[2048];
    snprintf(source, sizeof source,
             PREAMBLE "%d {\n    %s\n    i := i + 1\n}\nreturn^ sink\n",
             TURNS, body);

    File file;
    file.path = "main.lh";
    file.text = source;

    LhatProgram *program = lhat_program_new(true, disk_load, &file);
    if (program == NULL || !register_bench(program)) {
        printf("  %-22s registration failed\n", name);
        lhat_program_free(program);
        return 0.0;
    }
    const LhatUnit *root = lhat_program_check(program, "main.lh");
    if (root == NULL || !lhat_unit_ok(root) || !lhat_program_compile(program)) {
        printf("  %-22s did not build\n", name);
        if (root != NULL && lhat_unit_diagnostic_count(root) > 0) {
            char said[256];
            lhat_unit_diagnostic_write(root, 0, false, said, sizeof said);
            printf("      %s\n", said);
        }
        lhat_program_free(program);
        return 0.0;
    }

    LhatMachine *machine = lhat_machine_new();
    lhat_program_install(program, machine);
    if (hooked) {
        lhat_machine_set_debug_hook(machine, bench_hook, NULL);
    }

    int64_t started = lhat_now_ms();
    LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
    int64_t took = lhat_now_ms() - started;

    lhat_machine_dispose(machine);
    lhat_program_free(program);

    if (ran.status != LHAT_RUN_OK) {
        printf("  %-22s stopped: %s\n", name,
               lhat_run_status_message(ran.status));
        return 0.0;
    }
    return (double)took * 1e6 / (double)TURNS;  // ms -> ns per turn
}

// The best of several runs. Noise on a desktop only ever makes a thing
// slower -- another process took the core, the clock granularity landed
// badly -- so the least of the readings is the closest to what the work
// actually costs, and it is the reading that repeats.
#define ROUNDS 5

static double run_case(const char *name, const char *body, double baseline)
{
    double best = 0.0;
    for (int i = 0; i < ROUNDS; i++) {
        double one = time_once(name, body);
        if (one <= 0.0) {
            return 0.0;  // it did not build or did not finish; already said so
        }
        if (best == 0.0 || one < best) {
            best = one;
        }
    }
    if (baseline > 0.0) {
        printf("  %-22s %7.2f ns   (net %6.2f)\n", name, best, best - baseline);
    } else {
        printf("  %-22s %7.2f ns\n", name, best);
    }
    return best;
}

int main(void)
{
    printf("lhat_bench -- %d turns per case\n", TURNS);
    printf("  (a claim about time belongs to the machine that made it)\n\n");

    the_thing.value = 1;

    double empty = run_case("0 empty loop", "sink := sink + 1", 0.0);
    double member = run_case("1 h.read()", "sink := sink + h.read()", empty);
    double func = run_case("2 r1(h)", "sink := sink + r1(h)", empty);
    double local = run_case("3 loc(h)", "sink := sink + loc(h)", empty);
    double index = run_case("4 t[\"k\"]", "sink := sink + t[\"k\"]", empty);
    double def = run_case("5 inst.read()", "sink := sink + inst.read()", empty);
    double inherited = run_case("6 d.read()  (inherited)",
                                "sink := sink + d.read()", empty);

    // 05 の 8.7 with 2.2: the same boundary crossed with 0, 1, 2, 4 and 8
    // arguments. What lies between them is what an argument costs to hand
    // over -- the rebuilding vm.c does because the stack holds no LhatValue
    // run any more. The 0-argument case is the crossing with none of that.
    double a0 = run_case("7 a0()", "sink := sink + a0()", empty);
    double a1 = run_case("8 a1(1)", "sink := sink + a1(1)", empty);
    double a2 = run_case("9 a2(1,2)", "sink := sink + a2(1,2)",
                         empty);
    double a4 = run_case("10 a4(1..4)",
                         "sink := sink + a4(1,2,3,4)", empty);
    double a8 = run_case("11 a8(1..8)",
                         "sink := sink + a8(1,2,3,4,5,6,7,8)", empty);

    // 1 and 2 call the same C function with one value; what 1 has and 2 has
    // not is the read of `read` off the receiver.
    double member_read = member - func;
    double whole = member - empty;

    // The same arities on the other side of no boundary. An L^ body is
    // handed its arguments in the registers the call site wrote them in --
    // nothing is rebuilt -- so what the two slopes differ by is the
    // rebuilding and not the writing, which both pay for alike.
    double l0 = run_case("12 L0()", "sink := sink + L0()", empty);
    double l8 = run_case("13 L8(1..8)",
                         "sink := sink + L8(1,2,3,4,5,6,7,8)", empty);

    // 09 の 2.2: the empty loop again, with a hook that does nothing set --
    // what an instruction pays while a debugger is attached, over what it
    // pays with none (which the empty loop above already includes: the one
    // branch not taken).
    double hooked_best = 0.0;
    for (int i = 0; i < ROUNDS; i++) {
        double one = time_once_hooked("14 empty loop, hooked",
                                      "sink := sink + 1", true);
        if (one > 0.0 && (hooked_best == 0.0 || one < hooked_best)) {
            hooked_best = one;
        }
    }
    printf("  %-22s %7.2f ns   (net %6.2f)\n", "14 empty loop, hooked",
           hooked_best, hooked_best - empty);

    printf("\n  crossing     = %6.2f ns   (7 - 0)  a host call taking nothing\n",
           a0 - empty);
    printf("  per argument = %6.2f ns   (11 - 7)/8  the rebuild, per value\n",
           (a8 - a0) / 8.0);
    printf("    1 arg %6.2f   2 args %6.2f   4 args %6.2f   8 args %6.2f\n",
           a1 - a0, a2 - a0, a4 - a0, a8 - a0);
    printf("  L^ per arg   = %6.2f ns   (13 - 12)/8  the same, no boundary\n",
           (l8 - l0) / 8.0);
    printf("  the rebuild  = %6.2f ns per argument   what a boundary reading\n",
           ((a8 - a0) - (l8 - l0)) / 8.0);
    printf("                                       the slots by index would not do\n");

    printf("\n  member read  = %6.2f ns   (1 - 2)  what a slot read replaces\n",
           member_read);
    printf("  whole call   = %6.2f ns   (1 - 0)  all of h.read()\n", whole);
    if (whole > 0.0) {
        printf("  share        = %5.0f%%             the member read's part\n",
               100.0 * member_read / whole);
    }

    // Worth saying out loud, because it is the opposite of what the shape of
    // the problem suggests: the boundary is not the expensive step. A host
    // function is a C call with no frame and no bytecode, where an L^ closure
    // is a frame, a window of registers and instructions to run.
    printf("\n  host vs L^   = %6.2f ns   (2 - 3)  negative means the C call\n",
           func - local);
    printf("                                      is the cheaper of the two\n");

    printf("\n  bare index   = %6.2f ns   (4 - 0)  a probe with no call\n",
           index - empty);
    printf("  def^ member  = %6.2f ns   (5 - 3)  14.7 walks the definition\n",
           def - local);

    // The one claim here: 05 の 8.8改 flattens an inherited member into the
    // derived type's own table, so reading one is the same read. A gap wider
    // than the noise would mean a step was added.
    printf("\n  inherited    = %6.2f ns   (6 - 1)  should be noise\n",
           inherited - member);
    return 0;
}
