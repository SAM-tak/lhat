// L^ (lhat) -- what a stdlib/ test needs beyond testutil.h.
//
// The other test executables reach into src/ and check a lexer or an arena.
// The stdlib ones cannot: a module in stdlib/ is written against
// include/lhat.h alone, so what there is to check is what an L^ program sees
// when it imports it. Every case here is therefore a whole source text --
// registered, checked, compiled and run -- with its answer asserted on.
//
// Two things follow from that and live here rather than in each test file.
// The first is the run itself, which is the same eight calls every time and
// differs only in which lhatstdlib_*_register is handed in. The second is
// redirection: std.io and std.debug write to the process's own streams, so
// the only way to assert on what they wrote is to put a file where stdout
// was. freopen alone cannot do that -- it has no way back, and LHAT_CHECK
// prints to stdout, so a test that never restored it would report into the
// captured file instead. The fd is duplicated and put back instead, which is
// POSIX (unistd.h) or the Windows CRT (io.h) rather than C11 -- so every file
// that includes this one defines _POSIX_C_SOURCE first, the way port/thread.c
// does and for the same reason: CMAKE_C_EXTENSIONS is OFF, so nothing else
// does it for us and glibc would leave dup and fileno undeclared. It cannot
// be done here instead -- a test file that has already included a libc header
// of its own is past the point where the macro would be read.

#ifndef LHAT_STDLIBUTIL_H
#define LHAT_STDLIBUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define LHAT_TEST_DUP _dup
#define LHAT_TEST_DUP2 _dup2
#define LHAT_TEST_FILENO _fileno
#define LHAT_TEST_CLOSE _close
#else
#include <unistd.h>
#define LHAT_TEST_DUP dup
#define LHAT_TEST_DUP2 dup2
#define LHAT_TEST_FILENO fileno
#define LHAT_TEST_CLOSE close
#endif

#include "object.h"
#include "program.h"
#include "testutil.h"
#include "value.h"
#include "vm.h"

// Which module a run registers. Every lhatstdlib_*_register has this shape,
// and a case names one of them where a case in another file names another.
typedef bool (*LhatTestRegister)(LhatProgram *program);

typedef struct {
    const char *path;
    const char *text;
} LhatTestFile;

static char *lhat_test_load_one(void *context, const char *path,
                                size_t *length)
{
    const LhatTestFile *file = (const LhatTestFile *)context;
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
} LhatTestRan;

static inline void lhat_test_ran_dispose(LhatTestRan *ran)
{
    free(ran->text);
    ran->text = NULL;
}

// Checks, compiles and runs one unit with `reg`'s module registered.
static inline LhatTestRan lhat_test_run(LhatTestRegister reg, const char *text)
{
    static LhatTestFile file;
    file.path = "main.lh";
    file.text = text;

    LhatTestRan out;
    out.ok = false;
    out.status = LHAT_RUN_OK;
    out.integer = 0;
    out.text = NULL;

    LhatProgram program;
    lhat_program_init(&program, true, lhat_test_load_one, &file);
    const LhatUnit *root = NULL;
    const LhatModule *modules = NULL;
    size_t count = 0;
    if (reg(&program)) {
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

// Whether the unit checks at all. A refusal is a type error rather than a
// run-time one wherever the checker can see it, so this is what those cases
// ask.
static inline bool lhat_test_check_text(LhatTestRegister reg, const char *text)
{
    static LhatTestFile file;
    file.path = "main.lh";
    file.text = text;

    LhatProgram program;
    lhat_program_init(&program, true, lhat_test_load_one, &file);
    bool registered = reg(&program);
    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    bool clean = registered && root != NULL &&
                 !lhat_program_has_errors(&program) &&
                 root->checked.diagnostic_count == 0;
    lhat_program_dispose(&program);
    return clean;
}

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
static inline bool lhat_test_redirect_begin(LhatTestRedirect *redirect,
                                            FILE *stream, const char *path,
                                            const char *mode)
{
    redirect->stream = stream;
    redirect->saved = LHAT_TEST_DUP(LHAT_TEST_FILENO(stream));
    if (redirect->saved < 0) {
        return false;
    }
    if (freopen(path, mode, stream) == NULL) {
        LHAT_TEST_CLOSE(redirect->saved);
        redirect->saved = -1;
        return false;
    }
    return true;
}

// Puts the stream back. Anything the case wrote is flushed to the file first,
// so it can be read afterwards.
static inline void lhat_test_redirect_end(LhatTestRedirect *redirect)
{
    fflush(redirect->stream);
    LHAT_TEST_DUP2(redirect->saved, LHAT_TEST_FILENO(redirect->stream));
    LHAT_TEST_CLOSE(redirect->saved);
    redirect->saved = -1;
    // The FILE still refers to the file it was reopened on; clear its state so
    // an eof or error left by the case does not follow the stream back.
    clearerr(redirect->stream);
}

// The whole of a file, NUL-terminated, truncated to fit. Answers how many
// bytes were read, or 0 when it could not be opened.
static inline size_t lhat_test_read_file(const char *path, char *buffer,
                                         size_t size)
{
    if (size == 0) {
        return 0;
    }
    buffer[0] = '\0';
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return 0;
    }
    size_t read = fread(buffer, 1, size - 1, stream);
    buffer[read] = '\0';
    fclose(stream);
    return read;
}

// Runs `text` with stdout pointed at `path`, and fills `buffer` with
// everything it wrote. Binary, so that the '\n' a write ends on stays one
// byte on Windows as well and a case can name the bytes it expects outright.
// `path` is removed on the way out, so nothing survives the case.
static inline LhatTestRan lhat_test_run_capturing(LhatTestRegister reg,
                                                  const char *path,
                                                  const char *text,
                                                  char *buffer, size_t size)
{
    // Emptied first: a case reads `buffer` whatever happened, and LHAT_CHECK
    // carries on past a failure rather than returning, so leaving it as it
    // came off the stack would make the report itself the crash.
    if (size > 0) {
        buffer[0] = '\0';
    }
    LhatTestRan out = {false, LHAT_RUN_OK, 0, NULL};
    LhatTestRedirect redirect;
    if (!lhat_test_redirect_begin(&redirect, stdout, path, "wb")) {
        LHAT_CHECK(false, "stdout could not be pointed at %s", path);
        return out;
    }
    out = lhat_test_run(reg, text);
    lhat_test_redirect_end(&redirect);
    lhat_test_read_file(path, buffer, size);
    remove(path);
    return out;
}

// A file with exactly these bytes in it, for a case to read back through L^ or
// to hand to a redirected stdin.
static inline bool lhat_test_write_file(const char *path, const char *bytes,
                                        size_t length)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        return false;
    }
    bool ok = length == 0 || fwrite(bytes, 1, length, stream) == length;
    return fclose(stream) == 0 && ok;
}

#endif  // LHAT_STDLIBUTIL_H
