#include "common.h"
#include "lhat.h"
#include "lhat/port.h"
#include "code.h"  // Diagnostic dump only; the timed path uses the public API.

static void dump_proto(const LhatProto *proto, int depth)
{
    printf("%*sbody: %u registers, %u parameters\n", depth * 2, "", proto->chunk.registers, proto->parameters);
    for (size_t i = 0; i < proto->chunk.count; ++i) {
        char text[128];
        lhat_chunk_print(&proto->chunk, i, text, sizeof text);
        printf("%*s%4zu  %s\n", depth * 2, "", i, text);
    }
    for (size_t i = 0; i < proto->proto_count; ++i) dump_proto(proto->protos[i], depth + 1);
}

int main(int argc, char **argv)
{
    const double started = now_ns();
    CompareArgs args = {0};
    const bool dump = argc == 3 && strcmp(argv[1], "--dump-bytecode") == 0;
    if (dump) args.path = argv[2];
    else if (!parse_args(argc, argv, &args)) return 2;
    int exit_code = 1;
    LhatMachine *machine = NULL;
    double *times = NULL;
    LhatProgram *program = lhat_program_new(true, lhat_load_file, NULL);
    if (!program) goto cleanup;
    const double check_start = now_ns();
    const LhatUnit *unit = lhat_program_check(program, args.path);
    const double check_ns = now_ns() - check_start;
    if (!unit || !lhat_unit_ok(unit)) {
        if (unit) {
            for (size_t i = 0; i < lhat_unit_diagnostic_count(unit); ++i) {
                char text[2048];
                lhat_unit_diagnostic_write(unit, i, false, text, sizeof text);
                fprintf(stderr, "%s\n", text);
            }
        } else fprintf(stderr, "cannot load %s\n", args.path);
        goto cleanup;
    }
    const double compile_start = now_ns();
    if (!lhat_program_compile(program)) {
        fprintf(stderr, "L^ compilation failed\n");
        goto cleanup;
    }
    const double compile_ns = now_ns() - compile_start;
    if (dump) {
        dump_proto(lhat_unit_proto(unit), 0);
        exit_code = 0;
        goto cleanup;
    }
    machine = lhat_machine_new();
    if (!machine) goto cleanup;
    if (!lhat_program_install(program, machine)) goto cleanup;
    LhatRunResult loaded = lhat_run(machine, lhat_unit_proto(unit));
    if (loaded.status != LHAT_RUN_OK) {
        fprintf(stderr, "L^ initialization failed: %s\n", lhat_run_status_message(loaded.status));
        goto cleanup;
    }
    LhatValue kernel = loaded.value;
    if (!lhat_machine_table_set(machine, lhat_machine_host_root(machine), lhat_integer(0), kernel, NULL)) {
        fprintf(stderr, "cannot root kernel\n");
        goto cleanup;
    }
    times = (double *)calloc((size_t)args.samples, sizeof(double));
    if (!times) goto cleanup;
    LhatValue inputs[] = { lhat_integer(args.rounds), lhat_integer(args.size), lhat_integer(args.seed) };
    double first_result_ns = 0, checksum = 0;
    for (int i = -args.warmups; i < args.samples; ++i) {
        const double before = now_ns();
        LhatRunResult ran = lhat_machine_call(machine, kernel, inputs, 3);
        const double elapsed = now_ns() - before;
        if (i == -args.warmups) first_result_ns = now_ns() - started;
        if (ran.status != LHAT_RUN_OK || !lhat_is_number(ran.value)) {
            fprintf(stderr, "L^ execution failed: %s at line %u\n", lhat_run_status_message(ran.status), ran.line);
            goto cleanup;
        }
        checksum = lhat_number_as_real(ran.value);
        if (!valid_result(&args, checksum)) goto cleanup;
        if (i >= 0) times[i] = elapsed;
    }
    printf("{\"schema\":1,\"engine\":\"lhat\",\"check_load_ns\":%.0f,\"compile_ns\":%.0f,"
           "\"first_result_from_main_ns\":%.0f,\"peak_process_bytes\":%zu,\"checksum\":%.17g,\"samples_ns\":[",
           check_ns, compile_ns, first_result_ns, peak_process_bytes(), checksum);
    for (int i = 0; i < args.samples; ++i) printf("%s%.0f", i ? "," : "", times[i]);
    printf("]}\n");
    exit_code = 0;
cleanup:
    free(times);
    if (machine) lhat_machine_dispose(machine);
    if (program) lhat_program_free(program);
    return exit_code;
}
