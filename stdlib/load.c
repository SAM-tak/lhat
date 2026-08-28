// L^ (lhat) -- sample standard library: std.load (05 の 5.6).
//
// Two calls into the program (lhat_program_load_text / _file) and one into
// the machine (lhat_machine_adopt_script): the program checks and compiles
// the text as a unit of its own and forgets it, and the machine takes the
// proto over, so that it goes with the last closure of it. Nothing is kept
// here -- the module's state is the program it was registered into.

#include "load.h"

#include <string.h>

typedef struct {
    LhatProgram *program;
    const LhatErrorKind *cannot_read;
    const LhatErrorKind *rejected;
} LoadModule;

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

// A string's bytes as a C string -- the loader and the program want one,
// and an L^ string need not end in NUL.
static char *c_string(const LhatString *string)
{
    char *copy = (char *)lhat_alloc(string->length + 1);
    if (copy != NULL) {
        memcpy(copy, string->text, string->length);
        copy[string->length] = '\0';
    }
    return copy;
}

// The half both calls share: the proto the program answered becomes the
// machine's, and the closure is the answer.
static LhatValue adopt(LhatMachine *machine, const LoadModule *module,
                       LhatLoadStatus status, LhatProto *proto)
{
    switch (status) {
        case LHAT_LOAD_OK:
            break;
        case LHAT_LOAD_CANNOT_READ:
            return fail_with(machine, module->cannot_read,
                             "this unit could not be read");
        case LHAT_LOAD_REJECTED:
            return fail_with(machine, module->rejected,
                             lhat_program_load_failure(module->program));
        case LHAT_LOAD_OUT_OF_MEMORY:
            return fail_with(machine, module->rejected, "out of memory");
    }
    LhatValue closure = lhat_nil();
    if (!lhat_machine_adopt_script(machine, proto, &closure)) {
        lhat_proto_free(proto);
        return fail_with(machine, module->rejected, "out of memory");
    }
    return closure;
}

static void load_text(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)count;
    const LoadModule *module = (const LoadModule *)context;
    const LhatString *text = arg_string(arguments[0]);
    const LhatString *name = arg_string(arguments[1]);
    if (text == NULL || name == NULL) {
        answers[0] = fail_with(machine, module->rejected, "not a string");
        *answer_count = 1;
        return;
    }
    char *named = c_string(name);
    if (named == NULL) {
        answers[0] = fail_with(machine, module->rejected, "out of memory");
        *answer_count = 1;
        return;
    }
    LhatProto *proto = NULL;
    LhatLoadStatus status = lhat_program_load_text(
        module->program, named, text->text, text->length, &proto);
    lhat_free(named);
    answers[0] = adopt(machine, module, status, proto);
    *answer_count = 1;
}

static void load_file(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)count;
    const LoadModule *module = (const LoadModule *)context;
    const LhatString *path = arg_string(arguments[0]);
    if (path == NULL) {
        answers[0] = fail_with(machine, module->cannot_read, "not a path");
        *answer_count = 1;
        return;
    }
    char *named = c_string(path);
    if (named == NULL) {
        answers[0] = fail_with(machine, module->rejected, "out of memory");
        *answer_count = 1;
        return;
    }
    LhatProto *proto = NULL;
    LhatLoadStatus status =
        lhat_program_load_file(module->program, named, &proto);
    lhat_free(named);
    answers[0] = adopt(machine, module, status, proto);
    *answer_count = 1;
}

bool lhatstdlib_load_register(LhatProgram *program)
{
    LoadModule *module = (LoadModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    // 05 の 8.7: the program gives this back when it goes -- a register
    // that answers bool hands its caller no handle to free it with.
    if (!lhat_program_on_dispose(program, lhat_free, module)) {
        lhat_free(module);
        return false;
    }
    module->program = program;
    static const char *const variants[] = { "CannotRead", "Rejected" };
    if (!lhat_register_error_kind(program, "std.load", "Error", variants, 2,
                                  NULL, NULL)) {
        return false;
    }
    module->cannot_read =
        lhat_lookup_error_kind(program, "std.load", "Error", "CannotRead");
    module->rejected =
        lhat_lookup_error_kind(program, "std.load", "Error", "Rejected");

    // 13.4: registration signatures take no parameter names. A load is an
    // effect -- it writes the program and may read a file -- so both are
    // p^. The closure answered is 'p^... -> any^;' whatever the unit was
    // (load.h): a module^ unit's refuses arguments when called, as any
    // body taking none does.
    return lhat_register_func(program, "std.load", "file",
                              "p^string^ -> p^... -> any^; | std.load.Error;",
                              load_file, module) &&
           lhat_register_func(
               program, "std.load", "text",
               "p^string^, string^ -> p^... -> any^; | std.load.Error;",
               load_text, module);
}
