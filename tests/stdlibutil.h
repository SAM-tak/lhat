// L^ (lhat) -- what a stdlib/ test needs beyond testutil.h and fixture.h.
//
// The other test executables reach into src/ and check a lexer or an arena.
// The stdlib ones assert on what an L^ program sees when it imports a
// module: every case is a whole source text -- registered, checked,
// compiled and run -- with its answer asserted on. The run is the same
// eight calls every time and differs only in which lhatstdlib_*_register
// functions are handed in, so it lives here; so does redirection, since
// std.io and std.debug write to the process's own streams and the only way
// to assert on what they wrote is to put a file where stdout was.
//
// The bodies are in stdlibutil.c, inside the lhat_testkit library. That is
// also where the dup/fileno redirection plumbing lives, so no test file has
// to say _POSIX_C_SOURCE for itself.

#ifndef LHAT_STDLIBUTIL_H
#define LHAT_STDLIBUTIL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/program.h"
#include "testutil.h"
#include "lhat/vm.h"

// Which modules a run registers. Every lhatstdlib_*_register has this
// shape; a case hands in the ones it imports, usually one.
typedef bool (*LhatTestRegister)(LhatProgram *program);

// What a run answered, in a form that outlives the machine it ran on. The
// value itself does not: lhat_machine_dispose frees everything still on the
// heap, so a returned string has to be copied out before the run is torn
// down rather than read afterwards.
typedef struct {
    bool ok;  // false when anything before the run went wrong
    LhatRunStatus status;
    int64_t integer;  // 0 unless an integer came back
    char *text;       // owned, NUL-terminated; NULL unless a string came back
} LhatTestRan;

void lhat_test_ran_dispose(LhatTestRan *ran);

// Checks, compiles and runs one unit with the named modules registered.
LhatTestRan lhat_test_run(const LhatTestRegister *regs, size_t count,
                          const char *text);

// Whether the unit checks at all. A refusal is a type error rather than a
// run-time one wherever the checker can see it, so this is what those cases
// ask.
bool lhat_test_check_text(const LhatTestRegister *regs, size_t count,
                          const char *text);

// What a run answered, asserted as an exact integer. The status is asked
// alongside the value rather than left out of it: a run that stopped early
// answers a nil^, and an integer check on its own would report the 0 without
// saying that nothing got as far as returning.
#define LHAT_CHECK_RAN_INTEGER(ran, expected)             \
    do {                                                  \
        LHAT_CHECK((ran).ok, "the program ran");          \
        LHAT_CHECK_EQ_INT((ran).status, LHAT_RUN_OK);     \
        LHAT_CHECK_EQ_INT((ran).integer, (expected));     \
    } while (0)

// The same for a string. `text` is NULL when what came back was not one at
// all, which is the usual shape of a failure here -- an error value where a
// string was meant -- so it is named rather than dereferenced.
#define LHAT_CHECK_RAN_TEXT(ran, expected)                               \
    do {                                                                 \
        LHAT_CHECK((ran).ok, "the program ran");                         \
        LHAT_CHECK_EQ_INT((ran).status, LHAT_RUN_OK);                    \
        LHAT_CHECK((ran).text != NULL &&                                 \
                       strcmp((ran).text, (expected)) == 0,              \
                   "got \"%s\", want \"%s\"",                            \
                   (ran).text != NULL ? (ran).text : "(not a string)",   \
                   (expected));                                          \
    } while (0)

// ---- Redirection ----------------------------------------------------------

// One stream pointed at a file for as long as a case needs it. `saved` is the
// stream's own descriptor duplicated aside, which is what puts it back.
typedef struct {
    FILE *stream;
    int saved;
} LhatTestRedirect;

// Points `stream` at `path`, opened with `mode`. False if either half failed,
// in which case nothing was changed and _end must not be called.
bool lhat_test_redirect_begin(LhatTestRedirect *redirect, FILE *stream,
                              const char *path, const char *mode);

// Puts the stream back. Anything the case wrote is flushed to the file first,
// so it can be read afterwards.
void lhat_test_redirect_end(LhatTestRedirect *redirect);

// The whole of a file, NUL-terminated, truncated to fit. Answers how many
// bytes were read, or 0 when it could not be opened.
size_t lhat_test_read_file(const char *path, char *buffer, size_t size);

// A file with exactly these bytes in it, for a case to read back through L^
// or to hand to a redirected stdin.
bool lhat_test_write_file(const char *path, const char *bytes, size_t length);

// Runs `text` with stdout pointed at `path`, and fills `buffer` with
// everything it wrote. Binary, so that the '\n' a write ends on stays one
// byte on Windows as well and a case can name the bytes it expects outright.
// `path` is removed on the way out, so nothing survives the case.
LhatTestRan lhat_test_run_capturing(const LhatTestRegister *regs, size_t count,
                                    const char *path, const char *text,
                                    char *buffer, size_t size);

#endif  // LHAT_STDLIBUTIL_H
