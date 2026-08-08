// L^ (lhat) -- tests for std.thread and for the variadic host functions it
// is the first user of.
//
// Two things are pinned here. The first is 13.7 reaching a host registration:
// a signature ending in '...' makes the arity a floor rather than an exact
// count, and the tail arrives at the LhatHostFn uncollected. The second is
// what std.thread does with it -- that fn is a 'p^...' closure and nothing
// else, and that the four kinds a value crosses machines as make the round
// trip unchanged while everything else is refused by name.
//
// The threads themselves are real OS threads (port/thread.h), so every one
// started here is joined or disposed before the case ends: an unjoined one
// outliving its program would be reading a chunk that has been freed, which
// is the crash stdlib/thread.c's join_and_free comment describes.

#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "port.h"
#include "program.h"
#include "testutil.h"
#include "value.h"
#include "vm.h"

#include "../stdlib/thread.h"

typedef struct {
    const char *path;
    const char *text;
} File;

static char *load_one(void *context, const char *path, size_t *length)
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

// What a run answered, in a form that outlives the machine it ran on. The
// value itself does not: lhat_machine_dispose frees everything still on the
// heap, so a returned string has to be copied out before the run is torn down
// rather than read afterwards.
typedef struct {
    bool ok;  // false when anything before the run went wrong
    LhatRunStatus status;
    int64_t integer;  // 0 unless an integer came back
    char *text;       // owned, NUL-terminated; NULL unless a string came back
} Ran;

static void ran_dispose(Ran *ran) { free(ran->text); }

// Checks, compiles and runs one unit with std.thread registered.
static Ran run_source(const char *text)
{
    static File file;
    file.path = "main.lh";
    file.text = text;

    Ran out;
    out.ok = false;
    out.status = LHAT_RUN_OK;
    out.integer = 0;
    out.text = NULL;

    LhatProgram program;
    lhat_program_init(&program, true, load_one, &file);
    const LhatUnit *root = NULL;
    const LhatModule *modules = NULL;
    size_t count = 0;
    if (lhatstdlib_thread_register(&program)) {
        root = lhat_program_check(&program, "main.lh");
    }
    if (root != NULL && !lhat_program_has_errors(&program) &&
        root->checked.diagnostic_count == 0) {
        modules = lhat_program_compile(&program, &count);
    }
    if (modules != NULL) {
        LhatMachine *machine = lhat_machine_new();
        lhat_machine_set_modules(machine, modules, count);
        if (lhat_program_install(&program, machine)) {
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            out.ok = true;
            out.status = ran.status;
            if (lhat_is_integer(ran.value)) {
                out.integer = lhat_as_integer(ran.value);
            } else if (lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING)) {
                const LhatString *s =
                    (const LhatString *)lhat_as_object(ran.value);
                out.text = (char *)malloc(s->length + 1);
                if (out.text != NULL) {
                    memcpy(out.text, s->text, s->length);
                    out.text[s->length] = '\0';
                }
            }
        }
        lhat_machine_dispose(machine);
    }
    lhat_program_dispose(&program);
    return out;
}

// Whether the unit checks at all. The refusals below are type errors rather
// than run-time ones wherever the checker can see them, so this is what those
// cases ask.
static bool checks(const char *text)
{
    static File file;
    file.path = "main.lh";
    file.text = text;

    LhatProgram program;
    lhat_program_init(&program, true, load_one, &file);
    bool registered = lhatstdlib_thread_register(&program);
    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    bool clean = registered && root != NULL &&
                 !lhat_program_has_errors(&program) &&
                 root->checked.diagnostic_count == 0;
    lhat_program_dispose(&program);
    return clean;
}

// The preamble every case shares: spawn, join, and hand the answer back. Both
// results are unions (a ThreadHandle or an error either way), so both are
// narrowed with isa^ rather than read outright.
#define WITH_SPAWN(call)                                                   \
    "import^ std.thread\n"                                                 \
    "let^ h = " call "\n"                                                  \
    "if^ h isa^ std.thread.ThreadHandle {\n"                               \
    "    let^ answer = h.join()\n"                                         \
    "    if^ answer isa^ std.thread.ThreadError { return^ \"error\" }\n"   \
    "    return^ answer\n"                                                 \
    "}\n"                                                                  \
    "return^ \"refused\"\n"

static void test_spawn_shape(void)
{
    LHAT_TEST("a 'p^...' closure with no upvalues is what spawn takes");
    {
        Ran ran = run_source(WITH_SPAWN("std.thread.spawn(p^ ... "
                                        "{ return^ 42 })"));
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 42);
        ran_dispose(&ran);
    }

    // 13.7: the collector's element type is any^, so a plain 'p^ ...' is what
    // the declared parameter 'p^...' asks for. A written parameter is not:
    // conformance compares the lists, and one of those has a variadic tail
    // where the other has a slot of its own.
    LHAT_TEST("a closure with a parameter of its own is not a 'p^...'");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(p^ n:number^ { })\n"),
                   "the checker refuses it before anything runs");
    }

    LHAT_TEST("a closure taking nothing at all is not a 'p^...' either");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(p^ { })\n"),
                   "13.7's floor is one slot, and it has none");
    }

    // The result is the one half of the boundary the checker settles outright
    // -- it is copied back rather than forwarded, so nothing has to fit
    // through a variadic slot and the four kinds can be named.
    LHAT_TEST("a closure answering something that cannot cross is refused");
    {
        LHAT_CHECK(!checks("import^ std.thread\n"
                           "let^ h = std.thread.spawn(p^ ... "
                           "{ return^ {1, 2} })\n"),
                   "a table is not one of the four that cross");
    }
}

// The checker cannot see an upvalue in a type -- a closure's type says what it
// takes, not what it captured -- so this is spawn's own refusal at run time
// rather than a type error, and it needs a unit of its own to define the name
// the closure reaches for.
static void test_spawn_upvalue(void)
{
    LHAT_TEST("a closure that closes over a variable is refused at run time");
    {
        Ran ran = run_source("import^ std.thread\n"
                             "let^ n = 7\n"
                             "let^ h = std.thread.spawn(p^ ... { return^ n })\n"
                             "if^ h isa^ std.thread.ThreadError.NotSpawnable {\n"
                             "    return^ \"refused\"\n"
                             "}\n"
                             "return^ \"taken\"\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(ran.text != NULL && strcmp(ran.text, "refused") == 0,
                   "got \"%s\", want \"refused\"",
                   ran.text != NULL ? ran.text : "(not a string)");
        ran_dispose(&ran);
    }
}

static void test_arguments(void)
{
    // 13.7: what spawn was written past fn reaches thread_spawn as the tail of
    // its own arguments, is copied into the plain form, and is collected into
    // the callee's '...' by lhat_machine_call on the other machine.
    LHAT_TEST("what is written past fn arrives as fn's '...'");
    {
        Ran ran = run_source(
            WITH_SPAWN("std.thread.spawn(p^ ... {\n"
                       "    var^ total = 0\n"
                       "    for^ i, x in^ ... { total := total + x }\n"
                       "    return^ total\n"
                       "}, 3, 4, 5)"));
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 12);
        ran_dispose(&ran);
    }

    LHAT_TEST("no arguments at all leaves '...' empty");
    {
        Ran ran = run_source(
            WITH_SPAWN("std.thread.spawn(p^ ... {\n"
                       "    var^ total = 0\n"
                       "    for^ i, x in^ ... { total := total + 1 }\n"
                       "    return^ total\n"
                       "})"));
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 0);
        ran_dispose(&ran);
    }

    // A string is the one carried kind that allocates on both sides, so it is
    // what pins the copy rather than the value. Only the way back is asserted
    // on: a position of '...' is any^ (S16) and the only thing that can be
    // done with an any^ is to narrow it, which today means isa^ against a
    // registered type -- 'isa^ string^' does not compile yet (02 の 13.11's
    // general narrowing). So the string going in is pinned by its being
    // accepted rather than refused, and the string coming out by its bytes.
    LHAT_TEST("a string crosses to the thread and one comes back");
    {
        Ran ran = run_source(
            WITH_SPAWN("std.thread.spawn(p^ ... {\n"
                       "    var^ seen = 0\n"
                       "    for^ i, x in^ ... { seen := seen + 1 }\n"
                       "    if^ seen = 1 { return^ \"one carried\" }\n"
                       "    return^ \"\"\n"
                       "}, \"carried across\")"));
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(ran.text != NULL && strcmp(ran.text, "one carried") == 0,
                   "got \"%s\", want \"one carried\"",
                   ran.text != NULL ? ran.text : "(not a string)");
        ran_dispose(&ran);
    }

    // A collector is a table, and a table holds no nil^ -- so a nil^ written
    // among the arguments leaves no position behind. That is 13.7's own
    // behaviour and not spawn's: an ordinary L^ call written the same way
    // counts the same one position, which is what this pins.
    LHAT_TEST("a nil^ argument collapses the same way an ordinary call's does");
    {
        Ran spawned = run_source(
            WITH_SPAWN("std.thread.spawn(p^ ... {\n"
                       "    var^ n = 0\n"
                       "    for^ i, x in^ ... { n := n + 1 }\n"
                       "    return^ n\n"
                       "}, \"here\", nil^)"));
        Ran called = run_source("let^ count = p^ ... {\n"
                                "    var^ n = 0\n"
                                "    for^ i, x in^ ... { n := n + 1 }\n"
                                "    return^ n\n"
                                "}\n"
                                "return^ count(\"here\", nil^)\n");
        LHAT_CHECK(spawned.ok && called.ok, "both programs ran");
        LHAT_CHECK_EQ_INT(spawned.integer, called.integer);
        LHAT_CHECK_EQ_INT(spawned.integer, 1);
        ran_dispose(&spawned);
        ran_dispose(&called);
    }

    // 13.7's 'expr...' at a host call: the closure path unpacks a spread into
    // the frame it pushes, and the host path has no frame, so it packs one
    // array instead. Without that the table would arrive as a single argument.
    LHAT_TEST("a spread into spawn forwards the caller's own '...'");
    {
        Ran ran = run_source(
            "import^ std.thread\n"
            "let^ forward = p^ ... {\n"
            "    return^ std.thread.spawn(p^ ... {\n"
            "        var^ total = 0\n"
            "        for^ i, x in^ ... { total := total + x }\n"
            "        return^ total\n"
            "    }, ...)\n"
            "}\n"
            "let^ h = forward(6, 7, 8)\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    let^ answer = h.join()\n"
            "    if^ answer isa^ std.thread.ThreadError { return^ 0 - 1 }\n"
            "    return^ answer\n"
            "}\n"
            "return^ 0 - 2\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 21);
        ran_dispose(&ran);
    }

    // Only the four kinds cross, and spawn's own '...' is any^ so that a
    // forwarded collector fits (see the registration) -- which puts this
    // refusal at run time, by name, rather than in the checker.
    LHAT_TEST("a table is not an argument spawn can carry");
    {
        Ran ran = run_source(
            "import^ std.thread\n"
            "let^ h = std.thread.spawn(p^ ... { return^ 1 }, {1, 2})\n"
            "if^ h isa^ std.thread.ThreadError.BadArgument {\n"
            "    return^ \"refused\"\n"
            "}\n"
            "if^ h isa^ std.thread.ThreadHandle {\n"
            "    h.dispose()\n"
            "    return^ \"taken\"\n"
            "}\n"
            "return^ \"other\"\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(ran.text != NULL && strcmp(ran.text, "refused") == 0,
                   "got \"%s\", want \"refused\"",
                   ran.text != NULL ? ran.text : "(not a string)");
        ran_dispose(&ran);
    }
}

static void test_dispose(void)
{
    // stdlib/thread.c's thread_dispose: a handle let go without a join is
    // waited for there instead. Twenty of them at once is what turned the
    // detach-and-forget version into a use-after-free.
    LHAT_TEST("a handle disposed without a join waits for its thread");
    {
        Ran ran =
            run_source("import^ std.thread\n"
                       "var^ started = 0\n"
                       "for^ i from^ 1 to^ 20 {\n"
                       "    let^ h = std.thread.spawn(p^ ... { return^ 1 })\n"
                       "    if^ h isa^ std.thread.ThreadHandle {\n"
                       "        started := started + 1\n"
                       "        h.dispose()\n"
                       "    }\n"
                       "}\n"
                       "return^ started\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 20);
        ran_dispose(&ran);
    }

    LHAT_TEST("joining twice answers AlreadyJoined rather than waiting again");
    {
        Ran ran =
            run_source("import^ std.thread\n"
                       "let^ h = std.thread.spawn(p^ ... { return^ 1 })\n"
                       "if^ h isa^ std.thread.ThreadHandle {\n"
                       "    let^ first = h.join()\n"
                       "    let^ again = h.join()\n"
                       "    if^ again isa^ std.thread.ThreadError.AlreadyJoined {\n"
                       "        return^ 1\n"
                       "    }\n"
                       "    return^ 0\n"
                       "}\n"
                       "return^ 0 - 1\n");
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(ran.integer, 1);
        ran_dispose(&ran);
    }
}

int main(void)
{
    test_spawn_shape();
    test_spawn_upvalue();
    test_arguments();
    test_dispose();
    return lhat_test_report("test_thread");
}
