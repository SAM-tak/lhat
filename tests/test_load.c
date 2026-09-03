// L^ (lhat) -- tests for std.load (05 の 5.6).
//
// The text cases go through stdlibutil's one-file runner; the ones that
// need a second file on disk (a require^ from inside loaded text, a file
// load) drive the program themselves with a small in-memory loader, the
// way test_program.c does.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/load.h"
#include "../stdlib/thread.h"
#include "lhat/program.h"
#include "lhat/vm.h"

static const LhatTestRegister regs[] = {lhatstdlib_load_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// Loads text, narrows the answer, and runs `body` with it bound to `f`.
#define LOADED(source, name, body)                           \
    "import^ std.load\n"                                     \
    "let^ made = std.load.text(" source ", " name ")\n"      \
    "if^ made fits^ std.load.Error { return^ made.message }\n" \
    "let^ f = made\n" body

static void test_text(void)
{
    // 3.2: a script's top level is 'p^...', and the call's arguments are
    // what it reads as '...'.
    LHAT_TEST("loaded text runs as a script, its '...' the call's arguments");
    {
        LhatTestRan ran = run_source(LOADED(
            "\"let^ a = ...\\nreturn^ a[1] * 2\"", "\"gen\"",
            "let^ r = f(21)\n"
            "if^ r fits^ number^ { return^ r }\n"
            "return^ -1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }

    // A module^ unit answers the table of its public^ names -- built and
    // sealed as require^ would, but entering no registry: a second load is
    // a second table, and nothing private is in either.
    LHAT_TEST("a loaded module^ unit answers its public^ table, anew each time");
    {
        LhatTestRan ran = run_source(LOADED(
            "\"module^ stage\\n"
            "public^ let^ enter = f^ -> number^ { return^ 7 }\\n"
            "let^ hidden = 1\"",
            "\"stage.lh\"",
            "let^ api = f()\n"
            "let^ again = f()\n"
            "if^ api fits^ t^{} {\n"
            "    let^ enter = api[\"enter\"]\n"
            "    if^ enter fits^ f^ -> number^; {\n"
            "        return^ enter() * 100 + api.count^ * 10 + "
            "(if^ api is^ again: 1 el^: 0 ;)\n"
            "    }\n"
            "}\n"
            "return^ -1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 700 + 10 + 0);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and calling it with arguments is an arity fault");
    {
        LhatTestRan ran = run_source(LOADED(
            "\"module^ stage\\npublic^ let^ v = 1\"", "\"stage.lh\"",
            "let^ api = f(1)\n"
            "return^ 0\n"));
        LHAT_CHECK(ran.ok, "the program ran");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_ARITY);
        lhat_test_ran_dispose(&ran);
    }

    // What the checker said comes back in the error's message, one
    // diagnostic per line, named by what the load was called.
    LHAT_TEST("text the checker refuses answers Error.Rejected with its diagnostics");
    {
        LhatTestRan ran = run_source(
            "import^ std.load\n"
            "let^ made = std.load.text(\"let^ x : number^ = \\\"s\\\"\\n"
            "let^ y = nothere\", \"bad.lh\")\n"
            "if^ made fits^ std.load.Error.Rejected { return^ made.message }\n"
            "return^ \"accepted\"\n");
        LHAT_CHECK_RAN_TEXT(ran,
                            "bad.lh:1:20: error: this value does not fit where "
                            "it is written\n"
                            "bad.lh:2:10: error: no such name in scope: nothere");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a path the loader has nothing for answers Error.CannotRead");
    {
        LhatTestRan ran = run_source(
            "import^ std.load\n"
            "let^ made = std.load.file(\"nowhere.lh\")\n"
            "if^ made fits^ std.load.Error.CannotRead { return^ \"unread\" }\n"
            "return^ \"read\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "unread");
        lhat_test_ran_dispose(&ran);
    }

    // The closure is an ordinary value: a script may define things and
    // hand them out, and what it made lives on after the load.
    LHAT_TEST("what a loaded script made outlives the load");
    {
        LhatTestRan ran = run_source(LOADED(
            "\"var^ n = 0\\nreturn^ p^ -> number^ { n := n + 1 return^ n }\"",
            "\"counter\"",
            "let^ made_counter = f()\n"
            "if^ made_counter fits^ p^ -> number^; {\n"
            "    let^ one = made_counter()\n"
            "    return^ made_counter() * 10 + one\n"
            "}\n"
            "return^ -1\n"));
        LHAT_CHECK_RAN_INTEGER(ran, 21);
        lhat_test_ran_dispose(&ran);
    }
}

// ---- A program with files behind it -----------------------------------

typedef struct {
    const char *path;
    const char *text;
} File;

static char *load_from(void *context, const char *path, size_t *length)
{
    const File *files = (const File *)context;
    for (const File *f = files; f->path != NULL; f++) {
        if (strcmp(f->path, path) == 0) {
            size_t size = strlen(f->text);
            char *copy = (char *)malloc(size + 1);
            if (copy != NULL) {
                memcpy(copy, f->text, size + 1);
                *length = size;
            }
            return copy;
        }
    }
    return NULL;
}

// Checks, compiles, installs and runs main.lh over `files`, keeping the
// machine for the caller when `keep` is given.
static LhatRunResult run_files(const File *files, LhatProgram **out_program,
                               LhatMachine **keep)
{
    LhatRunResult failed;
    memset(&failed, 0, sizeof failed);
    failed.status = LHAT_RUN_TYPE_ERROR;

    LhatProgram *program = lhat_program_new(true, load_from, (void *)files);
    *out_program = program;
    if (!lhatstdlib_load_register(program) ||
        !lhatstdlib_thread_register(program)) {
        LHAT_CHECK(false, "registered");
        return failed;
    }
    const LhatUnit *root = lhat_program_check(program, "main.lh");
    LHAT_CHECK(root != NULL && !lhat_program_has_errors(program),
               "the program checked");
    if (root == NULL || lhat_program_has_errors(program) ||
        !lhat_program_compile(program)) {
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

static void test_files(void)
{
    // 5.1: a require^ inside loaded text is relative to the name the load
    // was given; what it brings in joins the program -- loaded once, so the
    // main unit's own require^ of the same file finds it already there.
    LHAT_TEST("a require^ inside loaded text is relative to its name, and loads once");
    {
        static const File files[] = {
            {"lib/shared.lh",
             "module^ ns.shared\n"
             "var^ loads = 0\n"
             "loads := loads + 1\n"
             "public^ let^ count = loads\n"},
            {"main.lh",
             "import^ std.load\n"
             "let^ made = std.load.text(\"require^ \\\"shared.lh\\\"\\n"
             "return^ ns.shared.count\", \"lib/gen.lh\")\n"
             "if^ made fits^ std.load.Error { return^ -1 }\n"
             "let^ f = made\n"
             "let^ inner = f()\n"
             "require^ \"lib/shared.lh\"\n"
             "if^ inner fits^ number^ { return^ inner * 10 + ns.shared.count }\n"
             "return^ -2\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatRunResult ran = run_files(files, &program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 11);
        lhat_program_free(program);
    }

    LHAT_TEST("a file loads through the program's loader");
    {
        static const File files[] = {
            {"stages/three.lh", "let^ args = ...\nreturn^ args.count^ + 3\n"},
            {"main.lh",
             "import^ std.load\n"
             "let^ made = std.load.file(\"stages/three.lh\")\n"
             "if^ made fits^ std.load.Error { return^ -1 }\n"
             "let^ f = made\n"
             "let^ r = f(\"a\", \"b\")\n"
             "if^ r fits^ number^ { return^ r }\n"
             "return^ -2\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatRunResult ran = run_files(files, &program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
        lhat_program_free(program);
    }

    // The body goes with the last closure of it. Two identical runs settle
    // what a run leaves on the heap; a load and a call between them leaves
    // nothing more once the collector has been through twice -- the body
    // goes on the first pass, and what its constants named, handed to the
    // machine as it goes (gc.c), is judged on the next.
    LHAT_TEST("a loaded script's body is collected with its closures");
    {
        static const File files[] = {
            {"main.lh", "L^.collectgarbage()\nreturn^ 0\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatMachine *machine = NULL;
        LhatRunResult first = run_files(files, &program, &machine);
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        LhatRunResult second = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(second.status, LHAT_RUN_OK);

        LhatProto *proto = NULL;
        LhatLoadStatus status = lhat_program_load_text(
            program, "gen", "let^ f = f^ -> number^ { return^ 1 }\nreturn^ f()",
            strlen("let^ f = f^ -> number^ { return^ 1 }\nreturn^ f()"), &proto);
        LHAT_CHECK_EQ_INT(status, LHAT_LOAD_OK);
        LhatValue closure = lhat_nil();
        LHAT_CHECK(lhat_machine_adopt_script(machine, proto, &closure),
                   "adopted");
        LHAT_CHECK(lhat_proto_is_owned(proto), "the machine owns it now");
        LhatRunResult called = lhat_machine_call(machine, closure, NULL, 0);
        LHAT_CHECK_EQ_INT(called.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(called.value), 1);

        // Nothing holds the closure now; a run collects the script with it,
        // and the run after leaves the heap as the identical earlier run
        // did.
        LhatRunResult third = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(third.status, LHAT_RUN_OK);
        LHAT_CHECK(third.collected > second.collected,
                   "the script and its closures were collected");
        LhatRunResult fourth = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(fourth.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(fourth.live, second.live);
        (void)first;
        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }

    // carry.h: a loaded body is this machine's heap's, so a closure of it
    // does not cross to a std.thread worker.
    LHAT_TEST("a loaded script's closure is refused by carry");
    {
        static const File files[] = {
            {"main.lh",
             "import^ std.load\n"
             "import^ std.thread\n"
             "let^ made = std.load.text(\"return^ 1\", \"gen\")\n"
             "if^ made fits^ std.load.Error { return^ \"unloaded\" }\n"
             "let^ f = made\n"
             "let^ h = std.thread.spawn(p^ ... { return^ 2 }, f)\n"
             "if^ h fits^ std.thread.ThreadError.BadArgument {\n"
             "    return^ h.message\n"
             "}\n"
             "if^ h fits^ std.thread.ThreadHandle { h.dispose() }\n"
             "return^ \"taken\"\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatMachine *machine = NULL;  // the string lives on it
        LhatRunResult ran = run_files(files, &program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING),
                   "a message came back");
        if (lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING)) {
            const LhatString *s = (const LhatString *)lhat_as_object(ran.value);
            LHAT_CHECK(strstr(s->text, "stays on its machine") != NULL,
                       "refused for the right reason: %s", s->text);
        }
        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }
}

int main(void)
{
    test_text();
    test_files();
    return lhat_test_report("test_load");
}
