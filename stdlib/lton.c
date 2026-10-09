// L^ (lhat) -- sample standard library: std.lton. What LTON is, and what may
// be written in one, is lton.h; this reads and writes its data tables.
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
#include <stdlib.h>
#include <math.h>
#include <locale.h>

#include "lhat/object.h"
#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

typedef struct {
    LhatProgram *program;
    const LhatErrorKind *cannot_read;
    const LhatErrorKind *rejected;
    const LhatErrorKind *unsupported;
    const LhatErrorKind *cycle;
    const LhatErrorKind *too_deep;
    const LhatErrorKind *cannot_write;
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

static LhatLtonStatus status_of(LhatLoadStatus status)
{
    switch (status) {
        case LHAT_LOAD_OK:
            return LHAT_LTON_OK;
        // Not reachable from parse -- the text is already in hand, and only
        // reading one through the loader can fail to find it. lton_load's
        // half below is where it comes from.
        case LHAT_LOAD_CANNOT_READ:
            return LHAT_LTON_CANNOT_READ;
        case LHAT_LOAD_REJECTED:
            return LHAT_LTON_REJECTED;
        case LHAT_LOAD_OUT_OF_MEMORY:
            break;
    }
    return LHAT_LTON_OUT_OF_MEMORY;
}

// 05 の 8.2: as data, so the host's initial bindings are not in scope.
static LhatLoadOptions as_data(void)
{
    LhatLoadOptions options;
    options.initial_bindings = false;
    return options;
}

// Wrap the text, have the program check and compile it as data, give the
// body to the machine and run it. What comes back is the table -- unlike
// std.load, which answers the closure and leaves the running to its caller.
LhatLtonStatus lhatstdlib_lton_parse(LhatMachine *machine,
                                     LhatProgram *program, const char *name,
                                     const char *text, size_t length,
                                     LhatValue *out)
{
    *out = lhat_nil();

    // 08 の 7改: bytes a full build wrote (lhatstdlib_lton_write) are the
    // wrapping already done, and a wrapper put around them would bury the
    // magic the loader tells them apart by. They go as they are.
    bool binary = lhat_program_is_binary_unit(text, length);
    size_t whole = length;
    char *source = binary ? NULL : wrapped(text, length, &whole);
    if (!binary && source == NULL) {
        return LHAT_LTON_OUT_OF_MEMORY;
    }

    LhatLoadOptions options = as_data();
    LhatProto *proto = NULL;
    LhatLoadStatus status = lhat_program_load_text_with(
        program, name != NULL ? name : "(lton)", binary ? text : source,
        whole, &options, &proto);
    lhat_free(source);
    if (status != LHAT_LOAD_OK) {
        return status_of(status);
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

LhatLtonStatus lhatstdlib_lton_write(LhatProgram *program, const char *name,
                                     const char *text, size_t length,
                                     bool with_debug_names, uint8_t **out,
                                     size_t *out_length)
{
    size_t whole = 0;
    char *source = wrapped(text, length, &whole);
    if (source == NULL) {
        return LHAT_LTON_OUT_OF_MEMORY;
    }
    LhatLoadOptions options = as_data();
    LhatLoadStatus status = lhat_program_write_text(
        program, name != NULL ? name : "(lton)", source, whole, &options,
        with_debug_names, out, out_length);
    lhat_free(source);
    return status_of(status);
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

// 08 § 7: the two reading errors distinguish missing text from rejection. A
// text that would not compile and
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

// Text serialization is independent of the frontend, including in VM-only
// builds. Only the active ancestor chain counts as a cycle: shared children
// are expanded at each occurrence.
#define LTON_MAX_DEPTH 96
typedef struct {
    char *text;
    size_t length, capacity;
    const LhatTable *ancestors[LTON_MAX_DEPTH];
    const LtonModule *module;
    const LhatErrorKind *error;
    const char *message;
} LtonWriter;

static void write_fail(LtonWriter *w, const LhatErrorKind *error, const char *message)
{
    if (w->error == NULL) { w->error = error; w->message = message; }
}

static void text_put(LtonWriter *w, const char *text, size_t length)
{
    if (w->error != NULL) return;
    if (length > SIZE_MAX - w->length - 1) {
        write_fail(w, w->module->out_of_memory, "LTON text is too large");
        return;
    }
    size_t needed = w->length + length + 1;
    if (needed > w->capacity) {
        size_t capacity = w->capacity > 0 ? w->capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = lhat_realloc(w->text, capacity);
        if (grown == NULL) {
            write_fail(w, w->module->out_of_memory, "out of memory");
            return;
        }
        w->text = grown;
        w->capacity = capacity;
    }
    memcpy(w->text + w->length, text, length);
    w->length += length;
    w->text[w->length] = '\0';
}

static void text_word(LtonWriter *w, const char *text) { text_put(w, text, strlen(text)); }
static void text_indent(LtonWriter *w, size_t depth)
{
    for (size_t i = 0; i < depth; i++) text_word(w, "    ");
}

// Preserve readable UTF-8, but escape arbitrary non-UTF-8 string bytes so
// the serialized source itself remains valid UTF-8.
static size_t text_utf8(const unsigned char *s, size_t length)
{
    if (length == 0) return 0;
    size_t n = s[0] >= 0xC2 && s[0] <= 0xDF ? 2 :
               s[0] >= 0xE0 && s[0] <= 0xEF ? 3 :
               s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
    if (n == 0 || length < n) return 0;
    for (size_t i = 1; i < n; i++) if ((s[i] & 0xC0) != 0x80) return 0;
    if ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ||
        (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90)) return 0;
    return n;
}

static void text_string(LtonWriter *w, const LhatString *s)
{
    text_word(w, "\"");
    for (size_t i = 0; i < s->length && w->error == NULL; i++) {
        unsigned char ch = (unsigned char)s->text[i];
        if (ch >= 128) {
            size_t n = text_utf8((const unsigned char *)s->text + i, s->length - i);
            if (n > 0) { text_put(w, s->text + i, n); i += n - 1; continue; }
        }
        switch (ch) {
            case '"': text_word(w, "\\\""); break;
            case '\\': text_word(w, "\\\\"); break;
            case '\n': text_word(w, "\\n"); break;
            case '\r': text_word(w, "\\r"); break;
            case '\t': text_word(w, "\\t"); break;
            default:
                if (ch < 32 || ch >= 127) {
                    char escaped[5];
                    snprintf(escaped, sizeof escaped, "\\x%02X", (unsigned)ch);
                    text_word(w, escaped);
                } else text_put(w, s->text + i, 1);
                break;
        }
    }
    text_word(w, "\"");
}

static int key_rank(LhatValue key)
{
    if (key.tag == LHAT_VALUE_INTEGER ||
        (key.tag == LHAT_VALUE_REAL && isfinite(key.as.real))) return 0;
    if (key.tag == LHAT_VALUE_BOOL) return 1;
    if (arg_string(key) != NULL) return 2;
    return -1;
}

static int compare_entries(const void *a, const void *b)
{
    LhatValue x = ((const LhatTableEntry *)a)->key;
    LhatValue y = ((const LhatTableEntry *)b)->key;
    int rank = key_rank(x), other = key_rank(y);
    if (rank != other) return rank < other ? -1 : 1;
    if (rank == 0) {
        double left = x.tag == LHAT_VALUE_INTEGER ? (double)x.as.integer : x.as.real;
        double right = y.tag == LHAT_VALUE_INTEGER ? (double)y.as.integer : y.as.real;
        if (left != right) return left < right ? -1 : 1;
        if (x.tag != y.tag) return x.tag == LHAT_VALUE_INTEGER ? -1 : 1;
        if (x.tag == LHAT_VALUE_INTEGER)
            return x.as.integer < y.as.integer ? -1 : x.as.integer > y.as.integer;
        return 0;
    }
    if (rank == 1) return (int)x.as.boolean - (int)y.as.boolean;
    const LhatString *left = arg_string(x), *right = arg_string(y);
    size_t length = left->length < right->length ? left->length : right->length;
    int order = memcmp(left->text, right->text, length);
    return order != 0 ? order : left->length < right->length ? -1 : left->length > right->length;
}

static bool name_key(const LhatString *s)
{
    if (s == NULL || s->length == 0) return false;
    for (size_t i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '_' || (i > 0 && c >= '0' && c <= '9'))) return false;
    }
    return true;
}

static void text_value(LtonWriter *w, LhatValue value, size_t depth);

static void text_table(LtonWriter *w, const LhatTable *table, size_t depth)
{
    if (depth >= LTON_MAX_DEPTH) {
        write_fail(w, w->module->too_deep, "LTON nesting exceeds 96 tables"); return;
    }
    for (size_t i = 0; i < depth; i++) {
        if (w->ancestors[i] == table) {
            write_fail(w, w->module->cycle, "cyclic table cannot be written as LTON"); return;
        }
    }
    if (table->is_definition || table->definition != NULL) {
        write_fail(w, w->module->unsupported, "only plain data tables can be written as LTON"); return;
    }
    w->ancestors[depth] = table;
    // 02 の 14.10: the sequence is written whole, nil^ positions included,
    // so it reads back the same length; the keyed half follows, sorted.
    size_t total = table->entry_count;
    if (total > SIZE_MAX / sizeof(LhatTableEntry)) {
        write_fail(w, w->module->out_of_memory, "table is too large"); return;
    }
    LhatTableEntry *entries = total > 0 ? lhat_alloc(total * sizeof *entries) : NULL;
    if (total > 0 && entries == NULL) {
        write_fail(w, w->module->out_of_memory, "out of memory"); return;
    }
    size_t count = 0;
    for (size_t i = 0; i < table->entry_capacity; i++) {
        if (!lhat_is_nil(table->entries[i].key)) entries[count++] = table->entries[i];
    }
    for (size_t i = 0; i < count; i++) {
        if (key_rank(entries[i].key) < 0) {
            write_fail(w, w->module->unsupported, "LTON keys must be finite numbers, booleans or strings");
            break;
        }
    }
    if (w->error == NULL && count > 1) qsort(entries, count, sizeof *entries, compare_entries);
    for (size_t i = 0; i < table->array_count && w->error == NULL; i++) {
        text_indent(w, depth);
        text_value(w, lhat_slots_get(table->array, i), depth + 1);
        text_word(w, ",\n");
    }
    for (size_t i = 0; i < count && w->error == NULL; i++) {
        LhatValue key = entries[i].key;
        text_indent(w, depth);
        const LhatString *name = arg_string(key);
        if (name_key(name)) text_put(w, name->text, name->length);
        else { text_word(w, "["); text_value(w, key, depth + 1); text_word(w, "]"); }
        text_word(w, " = ");
        text_value(w, entries[i].value, depth + 1);
        text_word(w, ",\n");
    }
    lhat_free(entries);
}

static void text_value(LtonWriter *w, LhatValue value, size_t depth)
{
    if (w->error != NULL) return;
    char number[96];
    switch (value.tag) {
        case LHAT_VALUE_NIL: text_word(w, "nil^"); return;
        case LHAT_VALUE_BOOL: text_word(w, value.as.boolean ? "true^" : "false^"); return;
        case LHAT_VALUE_INTEGER:
            if (value.as.integer == INT64_MIN) {
                text_word(w, "(-9223372036854775807 - 1)"); return;
            }
            snprintf(number, sizeof number, "%lld", (long long)value.as.integer);
            text_word(w, number); return;
        case LHAT_VALUE_REAL: {
            if (!isfinite(value.as.real)) break;
            snprintf(number, sizeof number, "%.17g", value.as.real);
            // Source always uses '.', regardless of the embedding host's locale.
            const char *point = localeconv()->decimal_point;
            char *at = point != NULL && *point != '\0' ? strstr(number, point) : NULL;
            if (at != NULL && strcmp(point, ".") != 0) {
                size_t length = strlen(point);
                memmove(at + 1, at + length, strlen(at + length) + 1);
                *at = '.';
            }
            text_word(w, number);
            if (strpbrk(number, ".eE") == NULL) text_word(w, ".0");
            return;
        }
        case LHAT_VALUE_OBJECT:
            if (arg_string(value) != NULL) { text_string(w, arg_string(value)); return; }
            if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
                const LhatTable *table = (const LhatTable *)lhat_as_object(value);
                text_word(w, "{\n");
                text_table(w, table, depth);
                text_indent(w, depth - 1);
                text_word(w, "}");
                return;
            }
            break;
        default: break;
    }
    write_fail(w, w->module->unsupported, "value cannot be represented as LTON data");
}

static void serialize_table(LtonWriter *w, LhatValue value)
{
    text_word(w, "");
    if (!lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        write_fail(w, w->module->unsupported, "LTON requires a table at the root"); return;
    }
    text_table(w, (const LhatTable *)lhat_as_object(value), 0);
}

static void lton_stringify(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)count;
    LtonWriter w = {0};
    w.module = context;
    serialize_table(&w, arguments[0]);
    if (w.error == NULL && !lhat_machine_make_string(machine, w.text, w.length, &answers[0]))
        write_fail(&w, w.module->out_of_memory, "out of memory");
    if (w.error != NULL) answers[0] = fail_with(machine, w.error, w.message);
    lhat_free(w.text);
    *answer_count = 1;
}

static void lton_save(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)count;
    LtonWriter w = {0};
    w.module = context;
    const LhatString *path = arg_string(arguments[0]);
    if (path == NULL || memchr(path->text, '\0', path->length) != NULL)
        write_fail(&w, w.module->cannot_write, "invalid file path");
    if (w.error == NULL) serialize_table(&w, arguments[1]);
    FILE *file = NULL;
    if (w.error == NULL) {
        char *name = c_string(path);
        if (name == NULL) write_fail(&w, w.module->out_of_memory, "out of memory");
        else file = lhat_fopen(name, "wb");
        lhat_free(name);
        if (file == NULL) write_fail(&w, w.module->cannot_write, "cannot open file for writing");
    }
    if (file != NULL) {
        bool ok = fwrite(w.text, 1, w.length, file) == w.length;
        if (fclose(file) != 0) ok = false;
        if (!ok) write_fail(&w, w.module->cannot_write, "could not write the complete LTON file");
    }
    lhat_free(w.text);
    *answer_count = 1;
    answers[0] = w.error != NULL ? fail_with(machine, w.error, w.message) : lhat_nil();
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

    static const char *const variants[] = {"CannotRead", "Rejected", "Unsupported", "Cycle", "TooDeep", "CannotWrite"};
    const LhatErrorKind *kinds[6];
    if (!lhat_register_error_kind(program, "std.lton", "LtonError", variants, 6,
                                  NULL, kinds)) {
        return false;
    }
    module->cannot_read = kinds[0];
    module->rejected = kinds[1];
    module->unsupported = kinds[2];
    module->cycle = kinds[3];
    module->too_deep = kinds[4];
    module->cannot_write = kinds[5];

    // Both are f^: reading a text as data has no effect of its own, and the
    // text cannot have one either (15.1, lton.h).
    return lhat_register_func(
               program, "std.lton", "parse",
               "f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;",
               lton_parse, module) &&
           lhat_register_func(
               program, "std.lton", "load",
               "f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;",
                lton_load, module) &&
           lhat_register_func(program, "std.lton", "stringify",
                "f^t^{} -> string^|std.lton.LtonError|std.error.OutOfMemory;",
                lton_stringify, module) &&
           lhat_register_func(program, "std.lton", "save",
                "p^string^,t^{} -> nil^|std.lton.LtonError|std.error.OutOfMemory;",
                lton_save, module);
}
