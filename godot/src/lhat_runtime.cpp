#include "lhat_runtime.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "lhat.h"
#include "lhat_host.h"
#include "lhat_variant.h"

namespace godot {

namespace {

// 04 の 11.6改: a panic carries what the program wrote it with, and the
// status alone would drop exactly the part that says anything.
String run_failure(const String &path, const LhatRunResult &ran)
{
    String text = String::utf8(lhat_run_status_message(ran.status));
    if (ran.status == LHAT_RUN_PANIC) {
        text += ": " + host::text_of(ran.value);
    }
    return host::problem(path, text);
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
    ClassDB::bind_static_method(
        "LhatRuntime", D_METHOD("call_member", "path", "member", "arguments"),
        &LhatRuntime::call_member, DEFVAL(Array()));
    ClassDB::bind_static_method("LhatRuntime",
                                D_METHOD("dump_host_api", "path"),
                                &LhatRuntime::dump_host_api);
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
    host::Units units = host::units_for(path);

    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        said.push_back(host::problem(path, "out of memory"));
        return said;
    }

    lhat_program_check(program, units.path.utf8().get_data());
    host::diagnostics_into(program, said);
    lhat_program_free(program);
    return said;
}

PackedStringArray LhatRuntime::run(const String &path)
{
    PackedStringArray said;
    host::Units units = host::units_for(path);

    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        said.push_back(host::problem(path, "out of memory"));
        return said;
    }

    const LhatUnit *root =
        lhat_program_check(program, units.path.utf8().get_data());
    host::diagnostics_into(program, said);
    if (root == nullptr || lhat_program_has_errors(program)) {
        if (said.is_empty()) {
            said.push_back(host::problem(path, "there is nothing to run"));
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
        said.push_back(modules == nullptr
                           ? host::compile_failure(program, path)
                           : host::problem(path, "out of memory"));
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
        UtilityFunctions::print(host::text_of(ran.value));
    }

    lhat_machine_dispose(machine);
    lhat_program_free(program);
    return said;
}

Variant LhatRuntime::call_member(const String &path, const String &member,
                                 const Array &arguments)
{
    host::Units units = host::units_for(path);
    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        UtilityFunctions::push_error(host::problem(path, "out of memory"));
        return Variant();
    }

    const LhatUnit *root =
        lhat_program_check(program, units.path.utf8().get_data());
    if (root == nullptr || lhat_program_has_errors(program)) {
        PackedStringArray said;
        host::diagnostics_into(program, said);
        for (int i = 0; i < said.size(); i++) {
            UtilityFunctions::push_error(said[i]);
        }
        lhat_program_free(program);
        return Variant();
    }

    size_t count = 0;
    const LhatModule *modules = lhat_program_compile(program, &count);
    LhatMachine *machine = modules != nullptr ? lhat_machine_new() : nullptr;
    if (machine == nullptr) {
        UtilityFunctions::push_error(
            modules == nullptr ? host::compile_failure(program, path)
                               : host::problem(path, "out of memory"));
        lhat_program_free(program);
        return Variant();
    }
    lhat_machine_set_modules(machine, modules, count);
    lhat_program_install(program, machine);

    Variant answer;
    LhatRunResult ran =
        lhat_run(machine, modules[lhat_unit_index(root)].proto);
    if (ran.status != LHAT_RUN_OK) {
        UtilityFunctions::push_error(
            host::problem(path, lhat_run_status_message(ran.status)));
    } else if (!lhat_is_object_kind(ran.value, LHAT_OBJECT_TABLE)) {
        UtilityFunctions::push_error(host::problem(
            path, "this unit answers no table, so it has no members to call"));
    } else {
        // Nothing runs from here until the call, which is what keeps the
        // values built below reachable -- see lhat_variant.h.
        const LhatTable *answered = (const LhatTable *)lhat_as_object(ran.value);
        CharString name = member.utf8();
        LhatValue key = lhat_nil();
        LhatValue callee = lhat_nil();
        bool ready = lhat_machine_make_string(machine, name.get_data(),
                                              (size_t)name.length(), &key);
        if (ready) {
            callee = lhat_table_get(answered, key);
        }

        LocalVector<LhatValue> converted;
        for (int i = 0; ready && i < arguments.size(); i++) {
            LhatValue held = lhat_nil();
            if (!host::from_variant(machine, arguments[i], &held)) {
                UtilityFunctions::push_error(host::problem(
                    path, String("argument ") + String::num_int64(i + 1) +
                              String(" has no shape in L^")));
                ready = false;
                break;
            }
            converted.push_back(held);
        }

        if (ready && lhat_is_nil(callee)) {
            UtilityFunctions::push_error(
                host::problem(path, String("there is no member ") + member));
            ready = false;
        }
        if (ready) {
            LhatRunResult called = lhat_machine_call(
                machine, callee, converted.ptr(), converted.size());
            if (called.status != LHAT_RUN_OK) {
                UtilityFunctions::push_error(host::problem(
                    path + String(".") + member,
                    lhat_run_status_message(called.status)));
            } else {
                answer = host::to_variant(called.value);
            }
        }
    }

    lhat_machine_dispose(machine);
    lhat_program_free(program);
    return answer;
}

String LhatRuntime::dump_host_api(const String &path)
{
    // The registrations a script gets, and no others -- what the checker is
    // told here has to be what it is told when a unit is checked.
    host::Units units = host::units_for(String());
    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        return "out of memory";
    }

    size_t needed = lhat_program_dump_host_api(program, nullptr, 0);
    char *text = (char *)lhat_alloc(needed + 1);
    if (text == nullptr) {
        lhat_program_free(program);
        return "out of memory";
    }
    lhat_program_dump_host_api(program, text, needed + 1);
    String written = String::utf8(text, (int)needed);
    lhat_free(text);
    lhat_program_free(program);

    Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
    if (file.is_null()) {
        return String("could not write ") + path;
    }
    file->store_string(written);
    file->close();
    return String();
}

}  // namespace godot
