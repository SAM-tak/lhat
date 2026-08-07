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
// Denied -- read it with try^/catch^, not isa^: compile_isa (vm.c) only
// resolves an error kind (04 の 6.1), not a hostdata type like File, so
// `x isa^ std.io.File` fails to compile (LHAT_COMPILE_UNSUPPORTED).

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
static LhatValue read_line_from(LhatMachine *machine,
                                const LhatErrorKind *eof_kind, FILE *stream)
{
    char buffer[4096];
    if (stream == NULL || fgets(buffer, sizeof buffer, stream) == NULL) {
        return fail_with(machine, eof_kind, "end of file");
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
               : lhat_nil();
}

static LhatValue std_print(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    const char *text = NULL;
    size_t length = 0;
    if (arg_text(arguments[0], &text, &length)) {
        fwrite(text, 1, length, stdout);
        fputc('\n', stdout);
    }
    return lhat_nil();
}

static LhatValue std_read_line(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)arguments;
    (void)count;
    const IoModule *module = (const IoModule *)context;
    return read_line_from(machine, module->eof, stdin);
}

static LhatValue file_open(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)count;
    const IoModule *module = (const IoModule *)context;
    const char *path = NULL;
    size_t path_length = 0;
    const char *mode = NULL;
    size_t mode_length = 0;
    if (!arg_text(arguments[0], &path, &path_length) ||
        !arg_text(arguments[1], &mode, &mode_length)) {
        return lhat_nil();
    }
    // object.c の lhat_string_new が text[length] = '\0' を書くので、
    // どちらも fopen が読める NUL 終端済みのバイト列になっている。
    errno = 0;
    FILE *stream = fopen(path, mode);
    if (stream == NULL) {
        const LhatErrorKind *kind =
            errno == EACCES ? module->denied : module->not_found;
        return fail_with(machine, kind, strerror(errno));
    }
    File *handle = (File *)lhat_alloc(sizeof *handle);
    if (handle == NULL) {
        fclose(stream);
        return lhat_nil();
    }
    handle->stream = stream;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->file_tag, handle,
                                    &out)) {
        fclose(stream);
        lhat_free(handle);
        return lhat_nil();
    }
    return out;
}

static LhatValue file_read_line(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count)
{
    (void)count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    return read_line_from(machine, module->eof,
                          handle != NULL ? handle->stream : NULL);
}

static LhatValue file_write(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    const char *text = NULL;
    size_t length = 0;
    if (handle != NULL && handle->stream != NULL &&
        arg_text(arguments[1], &text, &length)) {
        fwrite(text, 1, length, handle->stream);
    }
    return lhat_nil();
}

// 05 の 8.8: what dispose^ gives back. The machine marks a hand-written call
// to this the same as a collection's own (vm.c's CALLMETHOD case), so this
// never runs twice on the same handle.
static LhatValue file_dispose(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const IoModule *module = (const IoModule *)context;
    File *handle =
        (File *)lhat_hostdata_pointer(arguments[0], module->file_tag);
    if (handle != NULL) {
        if (handle->stream != NULL) {
            fclose(handle->stream);
        }
        lhat_free(handle);
    }
    return lhat_nil();
}

bool lhatstdlib_io_register(LhatProgram *program)
{
    IoModule *module = (IoModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }

    static const char *const variants[] = {"NotFound", "Denied", "Eof"};
    const LhatErrorKind *kinds[3];
    if (!lhat_register_error_kind(program, "std.io", "IOError", variants, 3,
                                  NULL, kinds)) {
        lhat_free(module);
        return false;
    }
    module->not_found = kinds[0];
    module->denied = kinds[1];
    module->eof = kinds[2];

    module->file_tag = lhat_register_hostdata_type(program, "std.io", "File");
    if (module->file_tag == NULL) {
        lhat_free(module);
        return false;
    }

    return lhat_register_func(program, "std.io", "print", "p^string^;",
                              std_print, module) &&
           lhat_register_func(program, "std.io", "readLine",
                              "f^ -> string^|std.io.IOError.Eof;",
                              std_read_line, module) &&
           lhat_register_func(
               program, "std.io", "open",
               "f^string^, string^ -> "
               "std.io.File|std.io.IOError.NotFound|std.io.IOError.Denied;",
               file_open, module) &&
           lhat_register_member(program, "std.io", "File", "readLine",
                                "f^self^ -> string^|std.io.IOError.Eof;",
                                file_read_line, module) &&
           lhat_register_member(program, "std.io", "File", "write",
                                "p^self^, string^;", file_write, module) &&
           lhat_register_member(program, "std.io", "File", "dispose",
                                "p^self^;", file_dispose, module);
}
