// L^ (lhat) -- tests for std.lton.
//
// Two halves. The first is what an LTON text may say: the elements of 02 の
// 14.14 as they stand, the spelling of the language throughout, and
// arithmetic rather than only values worked out beforehand.
//
// The second is what it may not, which is the whole of what makes reading
// one safe. The text is read as the body of an f^, so 15.1 refuses a p^
// call -- and since everything with an effect is a p^, an LTON text cannot
// have one. 05 の 8.2's initial bindings are left out of scope on top of
// that, so a host's own conveniences are not a configuration file's to
// name. Both are asserted rather than assumed: they are the reason this
// module can be pointed at a file somebody else wrote.
//
// std.lton.load reads through the program's loader (8.9), so the cases that
// use it drive a program with a table of files behind it, the way
// test_load.c does. The rest go through parse, which touches no file at all.

#include <stdio.h>
#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/lton.h"

static const LhatTestRegister regs[] = {lhatstdlib_lton_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// Most cases parse a text and read one value out of it, so the preamble is
// written once.
#define PARSING(lton, answer)                     \
    "import^ std.lton\n"                          \
    "let^ t = try^ std.lton.parse(\"" lton "\")\n" \
    "return^ " answer "\n"

static void test_what_may_be_written(void)
{
    LHAT_TEST("the three element forms of 14.14, as they stand");
    {
        // A name and a value. `=` is the spelling 14.14改 recommends.
        LhatTestRan ran = run_source(PARSING("a = 1, b = 2", "t[\"a\"] + t[\"b\"]"));
        LHAT_CHECK_RAN_INTEGER(ran, 3);
        lhat_test_ran_dispose(&ran);

        // `:=` is read as well -- 14.14改 keeps both and calls `=` the one.
        ran = run_source(PARSING("a := 7", "t[\"a\"]"));
        LHAT_CHECK_RAN_INTEGER(ran, 7);
        lhat_test_ran_dispose(&ran);

        // Positional, counted from 1, and keys that no name could spell.
        ran = run_source(PARSING("10, 20, 30", "t[1] + t[3]"));
        LHAT_CHECK_RAN_INTEGER(ran, 40);
        lhat_test_ran_dispose(&ran);

        ran = run_source(PARSING("[0] = 5, [\\\"a b\\\"] = 6",
                                 "t[0] + t[\"a b\"]"));
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("nesting, and a trailing comma");
    {
        LhatTestRan ran = run_source(
            PARSING("w = { title = \\\"hi\\\", size = { x = 4, y = 5, }, },",
                    "t[\"w\"][\"size\"][\"x\"] * t[\"w\"][\"size\"][\"y\"]"));
        LHAT_CHECK_RAN_INTEGER(ran, 20);
        lhat_test_ran_dispose(&ran);
    }

    // The point of reading the text as L^ rather than as a closed grammar of
    // its own: a value may be written as the expression it is.
    LHAT_TEST("arithmetic, rather than a number worked out beforehand");
    {
        LhatTestRan ran = run_source(PARSING("width = 480 * 2", "t[\"width\"]"));
        LHAT_CHECK_RAN_INTEGER(ran, 960);
        lhat_test_ran_dispose(&ran);

        ran = run_source(PARSING("s = \\\"a\\\" .. \\\"b\\\"", "t[\"s\"]"));
        LHAT_CHECK_RAN_TEXT(ran, "ab");
        lhat_test_ran_dispose(&ran);
    }

    // Nothing here was written for LTON: the same lexer reads it, so the
    // comments, the escapes and the shapes of a number are the language's.
    LHAT_TEST("the spelling is the language's, not an imitation of it");
    {
        LhatTestRan ran = run_source(
            "import^ std.lton\n"
            "let^ t = try^ std.lton.parse(\n"
            "  \"# a comment\\n\"\n"
            "  .. \"n = 0x10,        # another\\n\"\n"
            "  .. \"s = \\\"tab\\\\there\\\",\\n\"\n"
            "  .. \"r = 1.5e2,\\n\")\n"
            "if^ (t[\"n\"] = 16) and^ (t[\"s\"] = \"tab\\there\")\n"
            "   and^ (t[\"r\"] = 150.0) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("an empty text is an empty table");
    {
        LhatTestRan ran = run_source(
            "import^ std.lton\n"
            "let^ t = try^ std.lton.parse(\"\")\n"
            "if^ t fits^ t^{} { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    // 04 の 11.3, as in std.json: a table does not hold nil^, so writing one
    // puts no key rather than putting an empty one.
    LHAT_TEST("nil^ puts no key");
    {
        LhatTestRan ran = run_source(
            "import^ std.lton\n"
            "let^ t = try^ std.lton.parse(\"a = nil^, b = 1\")\n"
            "if^ (t[\"a\"] is^ nil^) and^ (t[\"b\"] = 1) { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }
}

// The half that matters. Everything below is refused, and none of it is
// refused by a rule written for LTON.
static void test_what_may_not(void)
{
    // 02 の 15.1: an f^ may call only an f^. The text is read as the body of
    // one, so a call to anything with an effect is a type error before
    // anything runs.
    //
    // The p^ is written inside the text rather than reached for outside it,
    // because in this first cut there is nothing outside to reach (T1). That
    // makes two layers, and this asserts the one that will still be there
    // when the first goes: even handed a p^, an LTON text cannot call it.
    LHAT_TEST("a p^ call is refused, which is what keeps a text from acting");
    {
        LhatTestRan ran = run_source(
            "import^ std.lton\n"
            "let^ t = std.lton.parse(\"x = (p^ { })()\")\n"
            "if^ t fits^ std.lton.LtonError.Rejected { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);

        // And an f^ call written the same way goes through, so what is
        // being refused above is the p^ and not the calling.
        ran = run_source(
            "import^ std.lton\n"
            "let^ t = try^ std.lton.parse(\"x = (f^ -> number^ { return^ 9 })()\")\n"
            "return^ t[\"x\"]\n");
        LHAT_CHECK_RAN_INTEGER(ran, 9);
        lhat_test_ran_dispose(&ran);
    }

    // 05 の 8.2: and the host's initial bindings are not even names here, so
    // there is nothing to reach for in the first place. `print` is one the
    // cli binds; naming it inside an LTON text finds nothing.
    LHAT_TEST("an initial binding is not a name an LTON text can reach");
    {
        LhatTestRan ran = run_source(
            "import^ std.lton\n"
            "let^ t = std.lton.parse(\"x = print\")\n"
            "if^ t fits^ std.lton.LtonError.Rejected { return^ 1 }\n"
            "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a text that is not a list of elements is refused");
    {
        static const char *const refused[] = {
            "x = 1 +",        // nothing where a value was promised
            "x = ",           // the same
            "= 1",            // a value where a name was
            "}",              // a brace that closes nothing here
        };
        for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
            char source[256];
            snprintf(source, sizeof source,
                     "import^ std.lton\n"
                     "let^ t = std.lton.parse(\"%s\")\n"
                     "if^ t fits^ std.lton.LtonError.Rejected { return^ 1 }\n"
                     "return^ 0\n",
                     refused[i]);
            LhatTestRan ran = run_source(source);
            LHAT_CHECK_RAN_INTEGER(ran, 1);
            lhat_test_ran_dispose(&ran);
        }
    }
}

// ---------------------------------------------------------------------------
// load, which reads through the program's loader (8.9)
// ---------------------------------------------------------------------------

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

// Runs main.lh over `files`. `keep` takes the machine where the caller has
// to read the answer -- stdlibutil.h's rule: a value does not outlive the
// machine it was made on, so a string has to be read before the tear-down
// and not after it.
static LhatRunResult run_files(const File *files, LhatProgram **out_program,
                               LhatMachine **keep)
{
    LhatRunResult failed;
    memset(&failed, 0, sizeof failed);
    failed.status = LHAT_RUN_TYPE_ERROR;

    LhatProgram *program = lhat_program_new(true, load_from, (void *)files);
    *out_program = program;
    if (!lhatstdlib_lton_register(program)) {
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

static void test_load(void)
{
    // The example this module was written for, as it stands.
    LHAT_TEST("a configuration file, read whole");
    {
        static const File files[] = {
            {"conf.lton",
             "# conf for the test suite: a small window, its own save "
             "directory.\n"
             "identity = \"lhatove-suite\",\n"
             "version = \"12.0\",\n"
             "window = {\n"
             "    title = \"lhatove test suite\",\n"
             "    width = 480,\n"
             "    height = 320,\n"
             "    vsync = 0,\n"
             "},\n"},
            {"main.lh",
             "import^ std.lton\n"
             "let^ c = try^ std.lton.load(\"conf.lton\")\n"
             "if^ (c[\"identity\"] = \"lhatove-suite\")\n"
             "   and^ (c[\"window\"][\"title\"] = \"lhatove test suite\")\n"
             "   and^ (c[\"window\"][\"width\"] = 480)\n"
             "   and^ (c[\"window\"][\"vsync\"] = 0) { return^ 1 }\n"
             "return^ 0\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatRunResult ran = run_files(files, &program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
        lhat_program_free(program);
    }

    // 8.9: through the loader and nothing else, so a path the host does not
    // serve is not there however the file system feels about it.
    LHAT_TEST("a path the loader does not serve cannot be read");
    {
        static const File files[] = {
            {"main.lh",
             "import^ std.lton\n"
             "let^ c = std.lton.load(\"nowhere.lton\")\n"
             "if^ c fits^ std.lton.LtonError.CannotRead { return^ 1 }\n"
             "return^ 0\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatRunResult ran = run_files(files, &program, NULL);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
        lhat_program_free(program);
    }

    // The wrapper is one line, so a diagnostic about the text's second line
    // and after names the line the writer sees. Asserted by reading the
    // message the refusal carries.
    LHAT_TEST("a diagnostic names the line the writer wrote on");
    {
        static const File files[] = {
            {"bad.lton",
             "a = 1,\n"
             "b = 1 +,\n"
             "c = 3,\n"},
            {"main.lh",
             "import^ std.lton\n"
             "let^ c = std.lton.load(\"bad.lton\")\n"
             "if^ c fits^ std.lton.LtonError.Rejected { return^ c.message }\n"
             "return^ \"not refused\"\n"},
            {NULL, NULL},
        };
        LhatProgram *program = NULL;
        LhatMachine *machine = NULL;
        LhatRunResult ran = run_files(files, &program, &machine);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // Read before the machine goes: what a run answered is the machine's
        // (stdlibutil.h), and this one is a string.
        if (ran.status == LHAT_RUN_OK &&
            lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING)) {
            const LhatString *said =
                (const LhatString *)lhat_as_object(ran.value);
            LHAT_CHECK(strstr(said->text, "bad.lton:2:") != NULL,
                       "the message names line 2: %s", said->text);
        }
        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }
}

// ---------------------------------------------------------------------------
// The same two readings from C (lton.h)
// ---------------------------------------------------------------------------

// What a host reads a field with. The key has to be a string on the machine,
// and making one allocates -- which is safe here and is the point: the
// collector advances inside the interpreter's loop and in
// lhat_machine_collectgarbage and nowhere else, so a table held in a C
// variable keeps for as long as no L^ runs.
static LhatValue field(LhatMachine *machine, LhatValue table, const char *name)
{
    if (!lhat_is_object_kind(table, LHAT_OBJECT_TABLE)) {
        return lhat_nil();
    }
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, name, strlen(name), &key)) {
        return lhat_nil();
    }
    return lhat_table_get((const LhatTable *)lhat_as_object(table), key);
}

static void check_text(LhatValue value, const char *want, const char *what)
{
    const LhatString *said = lhat_is_object_kind(value, LHAT_OBJECT_STRING)
                                 ? (const LhatString *)lhat_as_object(value)
                                 : NULL;
    LHAT_CHECK(said != NULL && said->length == strlen(want) &&
                   memcmp(said->text, want, said->length) == 0,
               "%s is \"%s\"", what, want);
}

static const File conf_files[] = {
    {"conf.lton",
     "# conf for the test suite: a small window, its own save directory.\n"
     "identity = \"lhatove-suite\",\n"
     "window = { title = \"lhatove test suite\", width = 480, vsync = 0 },\n"},
    {"bad.lton", "a = 1,\n"
                 "b = 1 +,\n"
                 "c = 3,\n"},
    {NULL, NULL},
};

static void test_from_c(void)
{
    // Neither registered nor checked. The C entries take the program, so a
    // host that only means to read its own configuration never puts
    // std.lton where a script could reach it, and never has a root unit at
    // all -- which is the arrangement this asserts is enough.
    LHAT_TEST("a host reads a configuration with no registration and no unit");
    {
        LhatProgram *program =
            lhat_program_new(true, load_from, (void *)conf_files);
        LhatMachine *machine = lhat_machine_new();

        LhatValue conf = lhat_nil();
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_load(machine, program, "conf.lton", &conf),
            LHAT_LTON_OK);

        // Read out into what a host would keep, the way its own readConf
        // would: a straight run of lookups with no L^ in between.
        check_text(field(machine, conf, "identity"), "lhatove-suite",
                   "identity");
        LhatValue window = field(machine, conf, "window");
        check_text(field(machine, window, "title"), "lhatove test suite",
                   "window.title");
        LHAT_CHECK_EQ_INT(lhat_as_integer(field(machine, window, "width")),
                          480);
        LHAT_CHECK_EQ_INT(lhat_as_integer(field(machine, window, "vsync")), 0);
        // A key that is not there answers nil^ rather than failing (11.3),
        // which is what lets a host keep its own default.
        LHAT_CHECK(lhat_is_nil(field(machine, window, "height")),
                   "a field the file left out is nil^");

        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }

    LHAT_TEST("parse names the text, and a refusal says where");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LhatMachine *machine = lhat_machine_new();

        LhatValue table = lhat_nil();
        static const char text[] = "a = 1,\nb = 1 +,\n";
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_parse(machine, program, "settings.lton", text,
                                  sizeof text - 1, &table),
            LHAT_LTON_REJECTED);
        LHAT_CHECK(lhat_is_nil(table), "nothing came back");
        const char *said = lhat_program_load_failure(program);
        LHAT_CHECK(said != NULL && strstr(said, "settings.lton:2:") != NULL,
                   "the failure names the caller's name and line 2: %s",
                   said != NULL ? said : "(none)");

        // NULL for the name is what the L^ side passes.
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_parse(machine, program, NULL, "x = 1", 5, &table),
            LHAT_LTON_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(field(machine, table, "x")), 1);

        // An empty text is an empty table here too, so 0 is a length and
        // not a stand-in for "measure it yourself".
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_parse(machine, program, NULL, "", 0, &table),
            LHAT_LTON_OK);
        LHAT_CHECK(lhat_is_object_kind(table, LHAT_OBJECT_TABLE) &&
                       lhat_table_count((const LhatTable *)lhat_as_object(
                           table)) == 0,
                   "an empty text is an empty table");

        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }

    // 15.1 is the language's rule and not this module's, so it does not care
    // which side of the boundary asked.
    LHAT_TEST("a p^ call is refused from C as it is from L^");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LhatMachine *machine = lhat_machine_new();

        LhatValue table = lhat_nil();
        static const char text[] = "x = (p^ { })()";
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_parse(machine, program, NULL, text,
                                  sizeof text - 1, &table),
            LHAT_LTON_REJECTED);

        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }

    // 8.9: through the loader and nothing else. A program given none reads
    // nothing whatever the file system holds.
    LHAT_TEST("a path the loader does not serve answers CANNOT_READ");
    {
        LhatProgram *program =
            lhat_program_new(true, load_from, (void *)conf_files);
        LhatMachine *machine = lhat_machine_new();

        LhatValue table = lhat_nil();
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_load(machine, program, "nowhere.lton", &table),
            LHAT_LTON_CANNOT_READ);
        LHAT_CHECK(lhat_is_nil(table), "nothing came back");

        LhatProgram *loaderless = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK_EQ_INT(
            lhatstdlib_lton_load(machine, loaderless, "conf.lton", &table),
            LHAT_LTON_CANNOT_READ);
        lhat_program_free(loaderless);

        lhat_machine_dispose(machine);
        lhat_program_free(program);
    }
}

int main(void)
{
    test_what_may_be_written();
    test_what_may_not();
    test_load();
    test_from_c();
    return lhat_test_report("test_lton");
}
