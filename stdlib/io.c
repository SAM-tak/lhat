// L^ (lhat) -- sample standard library: std.io.
//
// print/readLine talk to the process's own stdin/stdout; File wraps a
// FILE* as a std.io.File hostdata (05 の 8.8) -- registering `dispose`
// is what makes it the host's to hand over and L^'s to give back (12.5).
//
// IOError is registered in C with lhat_register_error_kind rather than
// written as an errordef^ in an L^ unit: the whole point of this file is
// that std.io works without any .lh alongside it. A construction error
// (open failing) is built with lhat_machine_make_error and handed back as
// an ordinary value, the same shape 04 の 2.5's error^Kind{...} builds.
//
// std.io.open answers std.io.File|std.io.IOError.NotFound|std.io.IOError.
// Denied|std.error.OutOfMemory -- read it with try^/catch^, or narrow with
// fits^ against std.io.File (05 の 8.8's hostdata path). std.error.
// OutOfMemory (rather than a std.io.IOError.OutOfMemory of its own) is
// shared with every other stdlib module that can fail the same way --
// see error.h.

#include "error.h"
#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// What lhatstdlib_io_register made, threaded through as every registration's
// `context` (05 の 8.7) rather than kept in file-scope statics -- a second
// LhatProgram registering this module gets its own kinds/tag instead of
// silently overwriting the first program's, which would hand its already-
// checked code a different (04 の 2.4: identity-by-declaration) kind object
// than the one its signatures were checked against.
//
// Allocated once per lhatstdlib_io_register call and never freed -- program.h
// has no hook to run when its LhatProgram is disposed, so a host that
// registers this module many times over a process's life leaks one of these
// per registration. Fine for a program set up once and run, which is this
// sample's whole use case.
typedef struct {
    const LhatErrorKind *not_found;
    const LhatErrorKind *denied;
    const LhatErrorKind *eof;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
    const LhatHostDataTag *file_tag;
} IoModule;

typedef struct {
    FILE *stream;
} File;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static bool arg_text(LhatValue value, const char **out_text,
                     size_t *out_length)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *string = (const LhatString *)lhat_as_object(value);
    *out_text = string->text;
    *out_length = string->length;
    return true;
}

// A line without its newline, or IOError.Eof when there was none left. A
// fixed buffer is this sample's simplification -- a line longer than it
// comes back truncated rather than split across two reads.
static LhatValue read_line_from(LhatMachine *machine, const IoModule *module,
                                FILE *stream)
{
    char buffer[4096];
    if (stream == NULL || fgets(buffer, sizeof buffer, stream) == NULL) {
        return fail_with(machine, module->eof, "end of file");
    }
    size_t length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == '\n') {
        length--;
        if (length > 0 && buffer[length - 1] == '\r') {
            length--;
        }
    }
    LhatValue out = lhat_nil();
    return lhat_machine_make_string(machine, buffer, length, &out)
               ? out
               : fail_with(machine, module->out_of_memory, "out of memory");
}

// 13.7's '...', so what was handed over is written in the order it came,
// separated by a space and closed by one newline -- and nothing at all when
// nothing was. A newline between them would make the argument boundary
// indistinguishable from a newline inside a value, and a comma would read as
// the table syntax, leaving `print(pack^ f())` and `print(f()...)` looking
// alike. The reading is 02 の 14.17's tostring, lhat_value_text: a string^ is
// its own bytes, without the quotes that let 1 and "1" be read apart (those
// belong to 03 の 4 章's prompt, not to a writer). nil^ and bool^ are words,
// and everything else is spelled the way lhat_value_write spells it.
//
// A value whose room could not be found is skipped in silence: a p^ has no
// answer to hand an error back through, and one line of output is not worth
// stopping the machine over.
static void std_print(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)answers;
    (void)answer_count;
    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        size_t needed = lhat_value_text(arguments[i], NULL, 0);
        char *text = (char *)lhat_alloc(needed + 1);
        if (text == NULL) {
            continue;
        }
        lhat_value_text(arguments[i], text, needed + 1);
        if (written > 0) {
            fputc(' ', stdout);
        }
        fwrite(text, 1, needed, stdout);
        written++;
        lhat_free(text);
    }
    if (written > 0) {
        fputc('\n', stdout);
    }
    return;
}

static void std_read_line(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    const IoModule *module = (const IoModule *)context;
    answers[0] = read_line_from(machine, module, stdin);
    *answer_count = 1;
    return;
}

static void file_open(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)count;
    const IoModule *module = (const IoModule *)context;
    const char *path = NULL;
    size_t path_length = 0;
    const char *mode = NULL;
    size_t mode_length = 0;
    if (!arg_text(arguments[0], &path, &path_length) ||
        !arg_text(arguments[1], &mode, &mode_length)) {
        return;
    }
    // object.c の lhat_string_new が text[length] = '\0' を書くので、
    // どちらも fopen が読める NUL 終端済みのバイト列になっている。
    errno = 0;
    FILE *stream = fopen(path, mode);
    if (stream == NULL) {
        const LhatErrorKind *kind =
            errno == EACCES ? module->denied : module->not_found;
        answers[0] = fail_with(machine, kind, strerror(errno));
        *answer_count = 1;
        return;
    }
    File *handle = (File *)lhat_alloc(sizeof *handle);
    if (handle == NULL) {
        fclose(stream);
        answers[0] = fail_with(machine, module->out_of_memory, "out of memory");
        *answer_count = 1;
        return;
    }
    handle->stream = stream;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->file_tag, handle,
                                    &out)) {
        fclose(stream);
        lhat_free(handle);
        answers[0] = fail_with(machine, module->out_of_memory, "out of memory");
        *answer_count = 1;
        return;
    }
    answers[0] = out;
    *answer_count = 1;
    return;
}

static void file_read_line(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    answers[0] = read_line_from(machine, module, handle != NULL ? handle->stream
                                                          : NULL);
    *answer_count = 1;
    return;
}

static void file_write(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    const char *text = NULL;
    size_t length = 0;
    if (handle != NULL && handle->stream != NULL &&
        arg_text(arguments[1], &text, &length)) {
        fwrite(text, 1, length, handle->stream);
    }
    return;
}

// 05 の 8.8: what dispose^ gives back. The machine marks a hand-written call
// to this the same as a collection's own (vm.c's CALLMETHOD case), so this
// never runs twice on the same handle.
static void file_dispose(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    if (handle != NULL) {
        if (handle->stream != NULL) {
            fclose(handle->stream);
        }
        lhat_free(handle);
    }
    return;
}

bool lhatstdlib_io_register(LhatProgram *program)
{
    // 05 の 8.7: 登録は検査より前 -- std.error.OutOfMemory を使う側(この
    // モジュール)より前に確実に存在させる。lhatstdlib_error_register は
    // 冪等なので、他の stdlib モジュールが先に呼んでいても構わない。
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    // 05 の 8.7: every field of this is an identity, and an identity
    // belongs to the process rather than to a program -- one declaration,
    // one tag and one error kind, however many programs declare them. So
    // one of these serves them all, and a second registration writes the
    // same answers back into it.
    static IoModule shared;
    IoModule *module = &shared;
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"NotFound", "Denied", "Eof"};
    const LhatErrorKind *kinds[3];
    if (!lhat_register_error_kind(program, "std.io", "IOError", variants, 3,
                                  NULL, kinds)) {
        return false;
    }
    module->not_found = kinds[0];
    module->denied = kinds[1];
    module->eof = kinds[2];

    module->file_tag = lhat_register_hostdata_type(program, "std.io", "File");
    if (module->file_tag == NULL) {
        return false;
    }

    return lhat_register_func(program, "std.io", "print", "p^...;", std_print,
                              module) &&
           lhat_register_func(
               program, "std.io", "readLine",
               "f^ -> string^|std.io.IOError.Eof|std.error.OutOfMemory;",
               std_read_line, module) &&
           lhat_register_func(
               program, "std.io", "open",
               "f^string^, string^ -> std.io.File|std.io.IOError.NotFound"
               "|std.io.IOError.Denied|std.error.OutOfMemory;",
               file_open, module) &&
           lhat_register_member(
               program, "std.io", "File", "readLine",
               "f^self^ -> string^|std.io.IOError.Eof|std.error.OutOfMemory;",
               file_read_line, module) &&
           lhat_register_member(program, "std.io", "File", "write",
                                "p^self^, string^;", file_write, module) &&
           lhat_register_member(program, "std.io", "File", "dispose",
                                "p^self^;", file_dispose, module);
}
