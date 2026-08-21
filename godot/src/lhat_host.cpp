#include "lhat_host.h"

#include "lhat_godot_module.h"

#include <string.h>

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {
namespace host {

namespace {

char *copy_of(const char *text, size_t size, size_t *length)
{
    // One past the end, so the text is NUL terminated whatever it holds.
    char *buffer = (char *)lhat_alloc(size + 1);
    if (buffer == nullptr) {
        return nullptr;
    }
    if (size > 0) {
        memcpy(buffer, text, size);
    }
    buffer[size] = '\0';
    *length = size;
    return buffer;
}

// FileAccess rather than stdio, which is the whole reason the language takes
// its loader rather than defaulting to one (port.h): an exported game has
// its units inside the .pck, where fopen reaches nothing.
char *load_unit(void *context, const char *path, size_t *length)
{
    const Units *units = (const Units *)context;
    if (units->holding && units->path == String::utf8(path)) {
        const char *text = units->held.get_data();
        return copy_of(text, strlen(text), length);
    }

    String full = units->base + String::utf8(path);
    if (!FileAccess::file_exists(full)) {
        return nullptr;
    }
    PackedByteArray bytes = FileAccess::get_file_as_bytes(full);
    return copy_of((const char *)bytes.ptr(), (size_t)bytes.size(), length);
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

}  // namespace

Units units_for(const String &path)
{
    Units units;
    units.holding = false;
    units.godot = nullptr;

    static const char *const schemes[] = {"res://", "user://"};
    for (const char *scheme : schemes) {
        if (path.begins_with(scheme)) {
            units.base = scheme;
            units.path = path.substr((int)strlen(scheme));
            return units;
        }
    }
    units.base = "res://";
    units.path = path;
    return units;
}

void hold(Units *units, const String &text)
{
    units->held = text.utf8();
    units->holding = true;
}

LhatProgram *program_for(Units *units)
{
    // 03 の 3.1: a file defaults to strict.
    LhatProgram *program = lhat_program_new(true, load_unit, units);
    if (program == nullptr) {
        return nullptr;
    }
    if (!lhat_register_global(program, "print", "f^...->nil^;", host_print,
                              nullptr) ||
        !lhat_bind_initial(program, "print", "L^.print") ||
        !lhat_bind_initial(program, "collectgarbage", "L^.collectgarbage")) {
        lhat_program_free(program);
        return nullptr;
    }
    // 05 の 8.7: every registration belongs before the check, since the
    // checker has to know what a signature says.
    units->godot = register_godot(program);
    if (units->godot == nullptr) {
        lhat_program_free(program);
        return nullptr;
    }
    return program;
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

String problem(const String &where, const String &what)
{
    return where + String(": error: ") + what;
}

// 04 の 11.6改: a runtime fault said as much as the machine knows -- the
// line, the panic's own value, and the frames still standing (read before
// the machine is disposed or run again).
String run_problem(LhatMachine *machine, const String &where,
                   const LhatRunResult &ran)
{
    String text = String::utf8(lhat_run_status_message(ran.status));
    if (ran.status == LHAT_RUN_PANIC) {
        text += ": " + text_of(ran.value);
    }
    if (ran.line > 0) {
        text = String("line ") + String::num_int64(ran.line) + ": " + text;
    }
    if (machine != nullptr && lhat_machine_fault_depth(machine) >= 2) {
        size_t needed = lhat_machine_traceback(machine, nullptr, 0);
        char *spelt = (char *)lhat_alloc(needed + 1);
        if (spelt != nullptr) {
            lhat_machine_traceback(machine, spelt, needed + 1);
            text += String("\n") + String::utf8(spelt, (int)needed);
            lhat_free(spelt);
        }
    }
    return problem(where, text);
}

String indentation()
{
    EditorInterface *editor = Engine::get_singleton()->is_editor_hint()
                                  ? EditorInterface::get_singleton()
                                  : nullptr;
    Ref<EditorSettings> settings = editor != nullptr
                                       ? editor->get_editor_settings()
                                       : Ref<EditorSettings>();
    if (settings.is_null()) {
        return "\t";
    }
    // 0 is Tabs, 1 is Spaces -- the enum the setting is declared with.
    if ((int)settings->get_setting("text_editor/behavior/indent/type") == 0) {
        return "\t";
    }
    int wide = (int)settings->get_setting("text_editor/behavior/indent/size");
    return String(" ").repeat(wide > 0 ? wide : 4);
}

// 05 の 6.2: the graph, not the one path that was asked for -- a unit fails
// on what one of its imports says as readily as on its own text.
void diagnostics_into(const LhatProgram *program, PackedStringArray &said)
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

TypedArray<Dictionary> diagnostics_as_errors(const LhatProgram *program,
                                             const String &path)
{
    TypedArray<Dictionary> errors;

    size_t own = lhat_program_diagnostic_count(program);
    for (size_t i = 0; i < own; i++) {
        const LhatProgramDiagnostic *d = lhat_program_diagnostic(program, i);
        Dictionary error;
        error["path"] = String::utf8(d->path);
        // 05 の 6.2: this half is about a unit rather than a place inside
        // one, so there is no line to give. The editor counts from one and
        // subtracts, so saying zero puts the mark one line above the file.
        error["line"] = 1;
        error["column"] = 1;
        error["message"] = String::utf8(lhat_program_error_message(d->code));
        errors.push_back(error);
    }

    for (const LhatUnit *unit = lhat_program_units(program); unit != nullptr;
         unit = lhat_unit_next(unit)) {
        // The unit the editor is showing is the one it named; anything the
        // graph reached beyond that is named by its own path.
        const char *unit_path = lhat_unit_path(unit);
        String where = String::utf8(unit_path);
        size_t count = lhat_unit_diagnostic_count(unit);
        for (size_t i = 0; i < count; i++) {
            LhatUnitDiagnostic d = lhat_unit_diagnostic(unit, i);

            char room[512];
            char *message = room;
            size_t needed =
                lhat_unit_diagnostic_message(unit, i, room, sizeof room);
            if (needed >= sizeof room) {
                char *bigger = (char *)lhat_alloc(needed + 1);
                if (bigger != nullptr) {
                    lhat_unit_diagnostic_message(unit, i, bigger, needed + 1);
                    message = bigger;
                }
            }

            Dictionary error;
            error["path"] = path.ends_with(where) ? path : where;
            error["line"] = (int64_t)d.line;
            error["column"] = (int64_t)d.column;
            error["message"] = String::utf8(message);
            errors.push_back(error);

            if (message != room) {
                lhat_free(message);
            }
        }
    }
    return errors;
}

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

}  // namespace host
}  // namespace godot
