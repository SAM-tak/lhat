// L^ (lhat) -- sample standard library: std.lton. What LTON is, and what may
// be written in one, is lton.h; this is how the text becomes a table.
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

// The wrapper. It has to fit on one line: what follows it is the LTON text
// laid down verbatim, so every line of that text after the first keeps the
// line number it had. Starting the text on a line of its own instead would
// shift all of them by one, and a diagnostic that names the wrong line is
// worse than one that names the wrong column of the first.
//
// 05 の 3.2 makes a script's top level a p^, which may call an f^; inside
// the f^ is where 15.1 holds, and that is the LTON text.
#define LTON_PROLOGUE "return^ (f^ -> t^{} { return^ {"
#define LTON_EPILOGUE "\n} })()\n"

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

// The half both calls share once the text is in hand: wrap it, have the
// program check and compile it as data, give the body to the machine and
// run it. What comes back is the table -- unlike std.load, which answers
// the closure and leaves the running to its caller.
static LhatValue read_text(LhatMachine *machine, const LtonModule *module,
                           const char *name, const char *text, size_t length)
{
    size_t whole = 0;
    char *source = wrapped(text, length, &whole);
    if (source == NULL) {
        return fail_with(machine, module->out_of_memory, "out of memory");
    }

    // 05 の 8.2: as data, so the host's initial bindings are not in scope.
    LhatLoadOptions options;
    options.initial_bindings = false;

    LhatProto *proto = NULL;
    LhatLoadStatus status = lhat_program_load_text_with(
        module->program, name, source, whole, &options, &proto);
    lhat_free(source);

    switch (status) {
        case LHAT_LOAD_OK:
            break;
        case LHAT_LOAD_CANNOT_READ:
            return fail_with(machine, module->cannot_read,
                             "this text could not be read");
        case LHAT_LOAD_REJECTED:
            // The checker's own diagnostics, which is where "an f^ may not
            // call a p^" arrives when a text tried to have an effect.
            return fail_with(machine, module->rejected,
                             lhat_program_load_failure(module->program));
        case LHAT_LOAD_OUT_OF_MEMORY:
            return fail_with(machine, module->out_of_memory, "out of memory");
    }

    LhatValue closure = lhat_nil();
    if (!lhat_machine_adopt_script(machine, proto, &closure)) {
        lhat_proto_free(proto);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    LhatRunResult ran = lhat_machine_call(machine, closure, NULL, 0);
    if (ran.status != LHAT_RUN_OK) {
        return fail_with(machine, module->rejected,
                         "this text did not finish");
    }
    return ran.value;
}

static LhatValue lton_parse(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)count;
    const LtonModule *module = (const LtonModule *)context;
    const LhatString *text = arg_string(arguments[0]);
    if (text == NULL) {
        return fail_with(machine, module->rejected, "not a text to read");
    }
    return read_text(machine, module, "(lton)", text->text, text->length);
}

static LhatValue lton_load(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)count;
    const LtonModule *module = (const LtonModule *)context;
    const LhatString *path = arg_string(arguments[0]);
    if (path == NULL) {
        return fail_with(machine, module->cannot_read, "not a path");
    }
    char *named = c_string(path);
    if (named == NULL) {
        return fail_with(machine, module->out_of_memory, "out of memory");
    }

    // 05 の 8.9: through the program's loader and not through the file
    // system directly, so a host that handed none over reads nothing.
    LhatProgram *program = module->program;
    size_t length = 0;
    char *text = lhat_program_read(program, named, &length);
    if (text == NULL) {
        LhatValue failed =
            fail_with(machine, module->cannot_read, "no such file");
        lhat_free(named);
        return failed;
    }
    LhatValue answered = read_text(machine, module, named, text, length);
    lhat_free(text);
    lhat_free(named);
    return answered;
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
