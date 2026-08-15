#include "lhat_runtime.h"

#include <string.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "lhat.h"

namespace godot {

namespace {

// 05 の 5.1 folds separators away and keeps nothing else, so "res://a/b.lh"
// comes back from the language as "res:/a/b.lh" -- one slash, and no longer
// a path the engine will open. The scheme is therefore taken off before the
// language sees anything and put back by the loader, which leaves L^ working
// in a flat, '/'-separated space where a require^ resolves the way 5.1 says.
//
// One scheme per program follows from that. Mixing res:// and user:// units
// in one graph is not something a require^ can express here.
struct Entry {
    String base;
    String path;
};

Entry entry_of(const String &path)
{
    Entry entry;
    static const char *const schemes[] = {"res://", "user://"};
    for (const char *scheme : schemes) {
        if (path.begins_with(scheme)) {
            entry.base = scheme;
            entry.path = path.substr((int)strlen(scheme));
            return entry;
        }
    }
    entry.base = "res://";
    entry.path = path;
    return entry;
}

// FileAccess rather than stdio, which is the whole reason the language takes
// its loader rather than defaulting to one (port.h): an exported game has
// its units inside the .pck, where fopen reaches nothing.
char *load_unit(void *context, const char *path, size_t *length)
{
    const Entry *entry = (const Entry *)context;
    String full = entry->base + String::utf8(path);
    if (!FileAccess::file_exists(full)) {
        return nullptr;
    }

    PackedByteArray bytes = FileAccess::get_file_as_bytes(full);
    size_t size = (size_t)bytes.size();

    // One past the end, so the text is NUL terminated whatever it holds.
    char *buffer = (char *)lhat_alloc(size + 1);
    if (buffer == nullptr) {
        return nullptr;
    }
    if (size > 0) {
        memcpy(buffer, bytes.ptr(), size);
    }
    buffer[size] = '\0';
    *length = size;
    return buffer;
}

String text_of(LhatValue value)
{
    size_t needed = lhat_value_text(value, nullptr, 0);
    char *text = (char *)lhat_alloc(needed + 1);
    if (text == nullptr) {
        return String();
    }
    lhat_value_text(value, text, needed + 1);
    String out = String::utf8(text, (int)needed);
    lhat_free(text);
    return out;
}

// 05 の 8.2: the host decides what a program sees without a require^. print
// goes to the Output panel, which is the one thing a script writer will look
// for first.
LhatValue host_print(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    String line;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            line += " ";
        }
        line += text_of(arguments[i]);
    }
    UtilityFunctions::print(line);
    return lhat_nil();
}

bool bind_host_names(LhatProgram *program)
{
    return lhat_register_global(program, "print", "f^...->nil^;", host_print,
                               nullptr) &&
           lhat_bind_initial(program, "print", "L^.print") &&
           lhat_bind_initial(program, "collectgarbage", "L^.collectgarbage");
}

// One shape for what this side has to say, so that a line the extension
// wrote reads like a line a stage wrote.
String problem(const String &where, const String &what)
{
    return where + String(": error: ") + what;
}

// 05 の 6.2: the graph, not the one path that was asked for -- a unit fails
// on what one of its imports says as readily as on its own text.
void collect(const LhatProgram *program, PackedStringArray &said)
{
    // The program's own are about a unit rather than about a place inside
    // one, so they are written here rather than rendered as a report.
    size_t own = lhat_program_diagnostic_count(program);
    for (size_t i = 0; i < own; i++) {
        const LhatProgramDiagnostic *d = lhat_program_diagnostic(program, i);
        said.push_back(problem(String::utf8(d->path),
                               lhat_program_error_message(d->code)));
    }

    for (const LhatUnit *unit = lhat_program_units(program); unit != nullptr;
         unit = lhat_unit_next(unit)) {
        size_t count = lhat_unit_diagnostic_count(unit);
        for (size_t i = 0; i < count; i++) {
            // One line each: the Output panel and an Array both read better
            // for it than for the form that quotes the source.
            char room[1024];
            size_t needed =
                lhat_unit_diagnostic_write(unit, i, false, room, sizeof room);
            if (needed < sizeof room) {
                said.push_back(String::utf8(room, (int)needed));
                continue;
            }
            char *bigger = (char *)lhat_alloc(needed + 1);
            if (bigger == nullptr) {
                said.push_back(String::utf8(room));  // truncated, not silence
                continue;
            }
            lhat_unit_diagnostic_write(unit, i, false, bigger, needed + 1);
            said.push_back(String::utf8(bigger, (int)needed));
            lhat_free(bigger);
        }
    }
}

// 03 の 4.2 puts the refusals in the checker, so a compile that stops is a
// hole in it -- and says where, so whoever closes the hole is told.
String compile_failure(const LhatProgram *program, const String &path)
{
    const char *where = nullptr;
    LhatCompileResult failure = lhat_program_compile_failure(program, &where);

    String name = where != nullptr ? String::utf8(where) : path;
    String text = String::utf8(lhat_compile_status_message(failure.status));
    if (failure.name != nullptr) {
        text += ": " + String::utf8(failure.name, (int)failure.name_length);
    }
    if (failure.line == 0) {
        return problem(name, text);
    }
    return problem(name + String(":") + String::num_int64(failure.line) +
                       String(":") + String::num_int64(failure.column),
                   text);
}

// 04 の 11.6改: a panic carries what the program wrote it with, and the
// status alone would drop exactly the part that says anything.
String run_failure(const String &path, const LhatRunResult &ran)
{
    String text = String::utf8(lhat_run_status_message(ran.status));
    if (ran.status == LHAT_RUN_PANIC) {
        text += ": " + text_of(ran.value);
    }
    return problem(path, text);
}

}  // namespace

void LhatRuntime::_bind_methods()
{
    ClassDB::bind_static_method("LhatRuntime", D_METHOD("version"),
                                &LhatRuntime::version);
    ClassDB::bind_static_method("LhatRuntime",
                                D_METHOD("run_status_message", "status"),
                                &LhatRuntime::run_status_message);
    ClassDB::bind_static_method("LhatRuntime", D_METHOD("check", "path"),
                                &LhatRuntime::check);
    ClassDB::bind_static_method("LhatRuntime", D_METHOD("run", "path"),
                                &LhatRuntime::run);
}

String LhatRuntime::version()
{
    return String(LHAT_VERSION);
}

// A status the machine does not know answers "unknown" rather than reading
// past its own table, so the int arrives from GDScript unchecked on purpose.
String LhatRuntime::run_status_message(int status)
{
    return String(lhat_run_status_message((LhatRunStatus)status));
}

PackedStringArray LhatRuntime::check(const String &path)
{
    PackedStringArray said;
    Entry entry = entry_of(path);

    // 03 の 3.1: a file defaults to strict.
    LhatProgram *program = lhat_program_new(true, load_unit, &entry);
    if (program == nullptr || !bind_host_names(program)) {
        said.push_back(problem(path, "out of memory"));
        lhat_program_free(program);
        return said;
    }

    lhat_program_check(program, entry.path.utf8().get_data());
    collect(program, said);
    lhat_program_free(program);
    return said;
}

PackedStringArray LhatRuntime::run(const String &path)
{
    PackedStringArray said;
    Entry entry = entry_of(path);

    LhatProgram *program = lhat_program_new(true, load_unit, &entry);
    if (program == nullptr || !bind_host_names(program)) {
        said.push_back(problem(path, "out of memory"));
        lhat_program_free(program);
        return said;
    }

    const LhatUnit *root =
        lhat_program_check(program, entry.path.utf8().get_data());
    collect(program, said);
    if (root == nullptr || lhat_program_has_errors(program)) {
        if (said.is_empty()) {
            said.push_back(problem(path, "there is nothing to run"));
        }
        lhat_program_free(program);
        return said;
    }

    // 05 の 5.3: every unit compiles, and the machine is given the lot, so a
    // require^ inside one reaches another.
    size_t count = 0;
    const LhatModule *modules = lhat_program_compile(program, &count);
    LhatMachine *machine = modules != nullptr ? lhat_machine_new() : nullptr;
    if (machine == nullptr) {
        said.push_back(modules == nullptr ? compile_failure(program, path)
                                          : problem(path, "out of memory"));
        lhat_program_free(program);
        return said;
    }

    lhat_machine_set_modules(machine, modules, count);
    // 05 の 8.7: what was registered reaches the machine here, which is what
    // makes the names bound above answer something.
    lhat_program_install(program, machine);

    LhatRunResult ran =
        lhat_run(machine, modules[lhat_unit_index(root)].proto);
    if (ran.status != LHAT_RUN_OK) {
        said.push_back(run_failure(path, ran));
    } else if (!lhat_is_nil(ran.value)) {
        UtilityFunctions::print(text_of(ran.value));
    }

    lhat_machine_dispose(machine);
    lhat_program_free(program);
    return said;
}

}  // namespace godot
