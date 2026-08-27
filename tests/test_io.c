// L^ (lhat) -- tests for std.io.
//
// Three things this module does that nothing else in stdlib/ does: it writes
// to the process's own stdout, it reads from its stdin, and it hands a FILE*
// over as a std.io.File hostdata (05 の 8.8) for an L^ program to give back.
// The first two are pinned by putting a file where the stream was
// (stdlibutil.h's redirect), the third by writing a file through L^ and
// reading it back through L^.
//
// A File is disposed exactly once per case, on the line that finished with
// it: vm.c marks a dispose^ written by hand so the collector does not repeat
// it, but nothing stops a second one written by hand, and that would be
// fclose and free on a chunk already freed.
//
// The failures asserted on are the ones an L^ program can reach: a path that
// is not there, and a read past the last line. IOError.Denied is left alone
// -- what sets errno to EACCES rather than something else is the platform's
// to decide, and stdlib/io.c folds everything but EACCES into NotFound.

#include <stdio.h>
#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/io.h"

// The file a case writes through L^ or prepares for it, and the one stdout is
// pointed at while a case runs. Both are removed by the case that made them.
#define SCRATCH "test_io_scratch.txt"
#define CAPTURED "test_io_out.txt"

// A name nothing will have made. Relative, so it is looked for in the
// directory ctest runs the executable in rather than anywhere in particular.
#define MISSING "test_io_no_such_file.txt"

static const LhatTestRegister regs[] = {lhatstdlib_io_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

// Runs `text` with stdout pointed at CAPTURED -- stdlibutil.h's, shared with
// test_debug.c, which captures the same way for the same reason.
static LhatTestRan run_capturing(const char *text, char *buffer, size_t size)
{
    return lhat_test_run_capturing(regs, 1, CAPTURED, text, buffer, size);
}

// Runs `text` with stdin coming from `bytes` instead of the terminal.
static LhatTestRan run_reading(const char *text, const char *bytes)
{
    LhatTestRan none = {false, LHAT_RUN_OK, 0, NULL};
    if (!lhat_test_write_file(SCRATCH, bytes, strlen(bytes))) {
        LHAT_CHECK(false, "could not write " SCRATCH);
        return none;
    }
    LhatTestRedirect redirect;
    if (!lhat_test_redirect_begin(&redirect, stdin, SCRATCH, "rb")) {
        LHAT_CHECK(false, "stdin could not be pointed at " SCRATCH);
        remove(SCRATCH);
        return none;
    }
    LhatTestRan ran = run_source(text);
    lhat_test_redirect_end(&redirect);
    remove(SCRATCH);
    return ran;
}

// The line a case read, handed back once it is known to be one. readLine
// answers a string^ or an error, so the string is taken by name rather than
// by ruling the errors out -- 13.11's fits^ reaches a builtin now, and
// asking for what is wanted leaves nothing in the returned type that the
// case did not mean. The failures answer words of their own, so a case that
// goes wrong says which half did.
#define TEXT_OF(name)                                                     \
    "    if^ " name " fits^ string^ { return^ " name " }\n"                \
    "    if^ " name " fits^ std.io.IOError.Eof { return^ \"eof\" }\n"      \
    "    return^ \"error\"\n"

static void test_print(void)
{
    LHAT_TEST("print writes its argument and a newline to stdout");
    {
        char written[256];
        LhatTestRan ran = run_capturing("import^ std.io\n"
                                        "std.io.print(\"to stdout\")\n"
                                        "return^ 1\n",
                                        written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "to stdout\n");
        lhat_test_ran_dispose(&ran);
    }

    // 13.7's '...': any type, as many as were given, separated by a space and
    // closed by one newline. The reading is 02 の 14.17's tostring, so a
    // string^ comes out unquoted while nil^ and bool^ come out as words. No
    // arguments writes nothing at all -- not even the newline.
    LHAT_TEST("print writes its arguments on one line, space separated");
    {
        char written[256];
        LhatTestRan ran =
            run_capturing("import^ std.io\n"
                          "std.io.print(1, \"two\", nil^, true^)\n"
                          "std.io.print()\n"
                          "return^ 1\n",
                          written, sizeof written);
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK_EQ_STR(written, strlen(written), "1 two nil true\n");
        lhat_test_ran_dispose(&ran);
    }

    // A p^ rather than an f^, which is what keeps it out of a pure function --
    // std.debug.log is the one name registered on the other side of that
    // (02 の 10.6, and tests/test_debug.c).
    LHAT_TEST("print is a p^, so an f^ cannot call it");
    {
        LHAT_CHECK(
            !checks("import^ std.io\n"
                    "let^ noisy = f^ -> number^ {\n"
                    "    std.io.print(\"from a pure one\")\n"
                    "    return^ 1\n"
                    "}\n"
                    "return^ noisy()\n"),
            "the checker refuses it before anything runs");
        LHAT_CHECK(
            checks("import^ std.io\n"
                   "let^ noisy = p^ {\n"
                   "    std.io.print(\"from a procedure\")\n"
                   "}\n"
                   "noisy()\n"
                   "return^ 1\n"),
            "and takes the same body in a p^, so it is the f^ it refused");
    }
}

static void test_open(void)
{
    LHAT_TEST("opening a path that is not there answers NotFound");
    {
        remove(MISSING);
        LhatTestRan ran =
            run_source("import^ std.io\n"
                       "let^ f = std.io.open(\"" MISSING "\", \"r\")\n"
                       "if^ f fits^ std.io.IOError.NotFound {\n"
                       "    return^ \"not found\"\n"
                       "}\n"
                       "if^ f fits^ std.io.File {\n"
                       "    f.dispose()\n"
                       "    return^ \"opened\"\n"
                       "}\n"
                       "return^ \"other\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "not found");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what was written through a File is read back through one");
    {
        remove(SCRATCH);
        LhatTestRan ran =
            run_source("import^ std.io\n"
                       "let^ out = std.io.open(\"" SCRATCH "\", \"w\")\n"
                       "if^ out fits^ std.io.File {\n"
                       "    out.write(\"alpha\\n\")\n"
                       "    out.write(\"beta\\n\")\n"
                       "    out.dispose()\n"
                       "}\n"
                       "let^ back = std.io.open(\"" SCRATCH "\", \"r\")\n"
                       "if^ back fits^ std.io.File {\n"
                       "    let^ line = back.readLine()\n"
                       "    back.dispose()\n" TEXT_OF("line")
                       "}\n"
                       "return^ \"not opened\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "alpha");
        lhat_test_ran_dispose(&ran);
        remove(SCRATCH);
    }
}

static void test_read_line(void)
{
    // Both endings, because the file is opened in text mode: Windows turns
    // the \r\n back into a \n before std.io.io ever sees it, and elsewhere
    // read_line_from strips the \r itself. Either way neither ending reaches
    // the L^ program.
    LHAT_TEST("a line arrives without its ending, \\n or \\r\\n");
    {
        LHAT_CHECK(lhat_test_write_file(SCRATCH, "alpha\r\nbeta\n", 12),
                   "the file to read was written");
        LhatTestRan first =
            run_source("import^ std.io\n"
                       "let^ back = std.io.open(\"" SCRATCH "\", \"r\")\n"
                       "if^ back fits^ std.io.File {\n"
                       "    let^ line = back.readLine()\n"
                       "    back.dispose()\n" TEXT_OF("line")
                       "}\n"
                       "return^ \"not opened\"\n");
        LHAT_CHECK_RAN_TEXT(first, "alpha");
        lhat_test_ran_dispose(&first);

        LhatTestRan second =
            run_source("import^ std.io\n"
                       "let^ back = std.io.open(\"" SCRATCH "\", \"r\")\n"
                       "if^ back fits^ std.io.File {\n"
                       "    let^ skipped = back.readLine()\n"
                       "    let^ line = back.readLine()\n"
                       "    back.dispose()\n" TEXT_OF("line")
                       "}\n"
                       "return^ \"not opened\"\n");
        LHAT_CHECK_RAN_TEXT(second, "beta");
        lhat_test_ran_dispose(&second);
        remove(SCRATCH);
    }

    LHAT_TEST("reading past the last line answers Eof");
    {
        LHAT_CHECK(lhat_test_write_file(SCRATCH, "only\n", 5),
                   "the file to read was written");
        LhatTestRan ran =
            run_source("import^ std.io\n"
                       "let^ back = std.io.open(\"" SCRATCH "\", \"r\")\n"
                       "if^ back fits^ std.io.File {\n"
                       "    let^ line = back.readLine()\n"
                       "    let^ past = back.readLine()\n"
                       "    back.dispose()\n"
                       "    if^ past fits^ std.io.IOError.Eof {\n"
                       "        return^ \"eof\"\n"
                       "    }\n"
                       "    return^ \"more\"\n"
                       "}\n"
                       "return^ \"not opened\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "eof");
        lhat_test_ran_dispose(&ran);
        remove(SCRATCH);
    }
}

static void test_read_line_from_stdin(void)
{
    LHAT_TEST("readLine takes a line from stdin");
    {
        LhatTestRan ran = run_reading("import^ std.io\n"
                                      "let^ line = std.io.readLine()\n"
                                      TEXT_OF("line"),
                                      "typed in\nand more\n");
        LHAT_CHECK_RAN_TEXT(ran, "typed in");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("readLine with nothing left on stdin answers Eof");
    {
        LhatTestRan ran =
            run_reading("import^ std.io\n"
                        "let^ line = std.io.readLine()\n"
                        "if^ line fits^ std.io.IOError.Eof { return^ \"eof\" }\n"
                        "return^ \"read\"\n",
                        "");
        LHAT_CHECK_RAN_TEXT(ran, "eof");
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_print();
    test_open();
    test_read_line();
    test_read_line_from_stdin();
    return lhat_test_report("test_io");
}
