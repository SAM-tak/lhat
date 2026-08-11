// L^ (lhat) -- the stdlib fixtures' bodies. See stdlibutil.h for what each
// is for.
//
// The redirection below duplicates and restores a stream's descriptor,
// which is POSIX (unistd.h) or the Windows CRT (io.h) rather than C11.
// CMAKE_C_EXTENSIONS is OFF, so glibc needs _POSIX_C_SOURCE said before any
// header -- said here, once, the way port/thread.c does, so no test file
// has to.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "stdlibutil.h"

#include <stdio.h>

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
#include "value.h"

typedef struct {
    const char *path;
    const char *text;
} LhatTestFile;

static char *load_one(void *context, const char *path, size_t *length)
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

static bool register_all(LhatProgram *program, const LhatTestRegister *regs,
                         size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!regs[i](program)) {
            return false;
        }
    }
    return true;
}

void lhat_test_ran_dispose(LhatTestRan *ran)
{
    free(ran->text);
    ran->text = NULL;
}

LhatTestRan lhat_test_run(const LhatTestRegister *regs, size_t count,
                          const char *text)
{
    LhatTestFile file = {"main.lh", text};

    LhatTestRan out;
    out.ok = false;
    out.status = LHAT_RUN_OK;
    out.integer = 0;
    out.text = NULL;

    LhatProgram program;
    lhat_program_init(&program, true, load_one, &file);
    const LhatUnit *root = NULL;
    const LhatModule *modules = NULL;
    size_t module_count = 0;
    if (register_all(&program, regs, count)) {
        root = lhat_program_check(&program, "main.lh");
    }
    if (root != NULL && !lhat_program_has_errors(&program) &&
        root->checked.diagnostic_count == 0) {
        modules = lhat_program_compile(&program, &module_count);
    }
    if (modules != NULL) {
        LhatMachine *machine = lhat_machine_new();
        lhat_machine_set_modules(machine, modules, module_count);
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

bool lhat_test_check_text(const LhatTestRegister *regs, size_t count,
                          const char *text)
{
    LhatTestFile file = {"main.lh", text};

    LhatProgram program;
    lhat_program_init(&program, true, load_one, &file);
    bool registered = register_all(&program, regs, count);
    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    bool clean = registered && root != NULL &&
                 !lhat_program_has_errors(&program) &&
                 root->checked.diagnostic_count == 0;
    lhat_program_dispose(&program);
    return clean;
}

bool lhat_test_redirect_begin(LhatTestRedirect *redirect, FILE *stream,
                              const char *path, const char *mode)
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

void lhat_test_redirect_end(LhatTestRedirect *redirect)
{
    fflush(redirect->stream);
    LHAT_TEST_DUP2(redirect->saved, LHAT_TEST_FILENO(redirect->stream));
    LHAT_TEST_CLOSE(redirect->saved);
    redirect->saved = -1;
    // The FILE still refers to the file it was reopened on; clear its state so
    // an eof or error left by the case does not follow the stream back.
    clearerr(redirect->stream);
}

size_t lhat_test_read_file(const char *path, char *buffer, size_t size)
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

bool lhat_test_write_file(const char *path, const char *bytes, size_t length)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        return false;
    }
    bool ok = length == 0 || fwrite(bytes, 1, length, stream) == length;
    return fclose(stream) == 0 && ok;
}

LhatTestRan lhat_test_run_capturing(const LhatTestRegister *regs, size_t count,
                                    const char *path, const char *text,
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
    out = lhat_test_run(regs, count, text);
    lhat_test_redirect_end(&redirect);
    lhat_test_read_file(path, buffer, size);
    remove(path);
    return out;
}
