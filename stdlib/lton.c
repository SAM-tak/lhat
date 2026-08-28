// L^ (lhat) -- sample standard library: std.lton. What LTON is, and what may
// be written in one, is lton.h; this is how the text becomes a table.
//
// The reading is written as the C entries lton.h names, and the two host
// functions are those with the status turned into an L^ error. A host that
// only wants to read its own configuration calls the C ones and never
// registers the module.
//
// The text is wrapped and handed to the program's own front end rather than
// parsed here. That is what makes the spelling L^'s -- the comments, the
// escapes, the shapes of a number -- and what makes 02 の 15.1 do the
// keeping: read as the body of an f^, a text that calls a p^ is refused by
// the checker, with no rule written here for it.

#include "error.h"
#include "lton.h"

#include <stdio.h>
#include <string.h>

#include "lhat/object.h"
#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

typedef struct {
    LhatProgram *program;
    const LhatErrorKind *cannot_read;
    const LhatErrorKind *rejected;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
} LtonModule;

// The wrapper is stdlib/lton.h's, named there rather than here so that the
// language server wraps a .lton the same way (lsp/lton.c).
#define LTON_PROLOGUE LHATSTDLIB_LTON_PROLOGUE
#define LTON_EPILOGUE LHATSTDLIB_LTON_EPILOGUE

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static const LhatString *arg_string(LhatValue value)
{
    return lhat_is_object_kind(value, LHAT_OBJECT_STRING)
               ? (const LhatString *)lhat_as_object(value)
               : NULL;
}

// A string's bytes as a C string -- the loader wants one, and an L^ string
// need not end in NUL.
static char *c_string(const LhatString *string)
{
    char *copy = (char *)lhat_alloc(string->length + 1);
    if (copy != NULL) {
        memcpy(copy, string->text, string->length);
        copy[string->length] = '\0';
    }
    return copy;
}

// The text with the wrapper around it. `out_length` is the whole of it.
static char *wrapped(const char *text, size_t length, size_t *out_length)
{
    size_t before = strlen(LTON_PROLOGUE);
    size_t after = strlen(LTON_EPILOGUE);
    char *made = (char *)lhat_alloc(before + length + after + 1);
    if (made == NULL) {
        return NULL;
    }
    memcpy(made, LTON_PROLOGUE, before);
    memcpy(made + before, text, length);
    memcpy(made + before + length, LTON_EPILOGUE, after + 1);
    *out_length = before + length + after;
    return made;
}

// ---------------------------------------------------------------------------
// The reading itself, which is what a host names too (lton.h)
// ---------------------------------------------------------------------------

// Wrap the text, have the program check and compile it as data, give the
// body to the machine and run it. What comes back is the table -- unlike
// std.load, which answers the closure and leaves the running to its caller.
LhatLtonStatus lhatstdlib_lton_parse(LhatMachine *machine,
                                     LhatProgram *program, const char *name,
                                     const char *text, size_t length,
                                     LhatValue *out)
{
    *out = lhat_nil();

    size_t whole = 0;
    char *source = wrapped(text, length, &whole);
    if (source == NULL) {
        return LHAT_LTON_OUT_OF_MEMORY;
    }

    // 05 の 8.2: as data, so the host's initial bindings are not in scope.
    LhatLoadOptions options;
    options.initial_bindings = false;

    LhatProto *proto = NULL;
    LhatLoadStatus status =
        lhat_program_load_text_with(program, name != NULL ? name : "(lton)",
                                    source, whole, &options, &proto);
    lhat_free(source);

    switch (status) {
        case LHAT_LOAD_OK:
            break;
        // Not reachable from here -- the text is already in hand, and only
        // reading one through the loader can fail to find it. lton_load's
        // half below is where it comes from.
        case LHAT_LOAD_CANNOT_READ:
            return LHAT_LTON_CANNOT_READ;
        case LHAT_LOAD_REJECTED:
            return LHAT_LTON_REJECTED;
        case LHAT_LOAD_OUT_OF_MEMORY:
            return LHAT_LTON_OUT_OF_MEMORY;
    }

    LhatValue closure = lhat_nil();
    if (!lhat_machine_adopt_script(machine, proto, &closure)) {
        lhat_proto_free(proto);
        return LHAT_LTON_OUT_OF_MEMORY;
    }
    LhatRunResult ran = lhat_machine_call(machine, closure, NULL, 0);
    if (ran.status != LHAT_RUN_OK) {
        return LHAT_LTON_FAULTED;
    }
    *out = ran.value;
    return LHAT_LTON_OK;
}

LhatLtonStatus lhatstdlib_lton_load(LhatMachine *machine, LhatProgram *program,
                                    const char *path, LhatValue *out)
{
    *out = lhat_nil();

    // 05 の 8.9: through the program's loader and not through the file
    // system directly, so a host that handed none over reads nothing.
    size_t length = 0;
    char *text = lhat_program_read(program, path, &length);
    if (text == NULL) {
        return LHAT_LTON_CANNOT_READ;
    }
    LhatLtonStatus status =
        lhatstdlib_lton_parse(machine, program, path, text, length, out);
    lhat_free(text);
    return status;
}

// ---------------------------------------------------------------------------
// The same two, as L^ sees them
// ---------------------------------------------------------------------------

// 08 の 7: LtonError has two variants, so a text that would not compile and
// one that ran and stopped both answer Rejected. What differs is the
// message -- which is the distinction C keeps, since the two are read from
// different places (lton.h).
static LhatValue answer(LhatMachine *machine, const LtonModule *module,
                        LhatLtonStatus status, LhatValue table)
{
    switch (status) {
        case LHAT_LTON_OK:
            return table;
        case LHAT_LTON_CANNOT_READ:
            return fail_with(machine, module->cannot_read, "no such file");
        case LHAT_LTON_REJECTED:
            // The checker's own diagnostics, which is where "an f^ may not
            // call a p^" arrives when a text tried to have an effect.
            return fail_with(machine, module->rejected,
                             lhat_program_load_failure(module->program));
        case LHAT_LTON_FAULTED:
            return fail_with(machine, module->rejected,
                             "this text did not finish");
        case LHAT_LTON_OUT_OF_MEMORY:
            return fail_with(machine, module->out_of_memory, "out of memory");
    }
    return lhat_nil();
}

static void lton_parse(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)count;
    const LtonModule *module = (const LtonModule *)context;
    const LhatString *text = arg_string(arguments[0]);
    if (text == NULL) {
        answers[0] = fail_with(machine, module->rejected, "not a text to read");
        *answer_count = 1;
        return;
    }
    LhatValue table = lhat_nil();
    LhatLtonStatus status = lhatstdlib_lton_parse(
        machine, module->program, NULL, text->text, text->length, &table);
    answers[0] = answer(machine, module, status, table);
    *answer_count = 1;
}

static void lton_load(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)count;
    const LtonModule *module = (const LtonModule *)context;
    const LhatString *path = arg_string(arguments[0]);
    if (path == NULL) {
        answers[0] = fail_with(machine, module->cannot_read, "not a path");
        *answer_count = 1;
        return;
    }
    char *named = c_string(path);
    if (named == NULL) {
        answers[0] = fail_with(machine, module->out_of_memory, "out of memory");
        *answer_count = 1;
        return;
    }
    LhatValue table = lhat_nil();
    LhatLtonStatus status =
        lhatstdlib_lton_load(machine, module->program, named, &table);
    lhat_free(named);
    answers[0] = answer(machine, module, status, table);
    *answer_count = 1;
}

bool lhatstdlib_lton_register(LhatProgram *program)
{
    // 05 の 8.7: registration before checking -- std.error.OutOfMemory has to
    // exist before this module's signatures name it. The call is idempotent.
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    // The program is this module's own, so unlike the modules that hold only
    // identities this one is per program and goes when the program does.
    LtonModule *module = (LtonModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    if (!lhat_program_on_dispose(program, lhat_free, module)) {
        lhat_free(module);
        return false;
    }
    module->program = program;
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"CannotRead", "Rejected"};
    const LhatErrorKind *kinds[2];
    if (!lhat_register_error_kind(program, "std.lton", "LtonError", variants, 2,
                                  NULL, kinds)) {
        return false;
    }
    module->cannot_read = kinds[0];
    module->rejected = kinds[1];

    // Both are f^: reading a text as data has no effect of its own, and the
    // text cannot have one either (15.1, lton.h).
    return lhat_register_func(
               program, "std.lton", "parse",
               "f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;",
               lton_parse, module) &&
           lhat_register_func(
               program, "std.lton", "load",
               "f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;",
               lton_load, module);
}
