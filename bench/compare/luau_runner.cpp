#include "common.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "lua.h"
#include "lualib.h"
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    const double started = now_ns();
    CompareArgs args = {};
    const bool dump = argc == 3 && strcmp(argv[1], "--dump-bytecode") == 0;
    if (dump) args.path = argv[2];
    else if (!parse_args(argc, argv, &args)) return 2;
    try {
        const double read_start = now_ns();
        std::ifstream file(args.path, std::ios::binary);
        if (!file) { fprintf(stderr, "cannot load %s\n", args.path); return 1; }
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const double read_ns = now_ns() - read_start;
        Luau::CompileOptions options;
        options.optimizationLevel = COMPARE_LUAU_OPT_LEVEL;
        options.debugLevel = 0;
        if (dump) {
            Luau::BytecodeBuilder builder;
            builder.setDumpFlags(Luau::BytecodeBuilder::Dump_Code);
            Luau::compileOrThrow(builder, source, options);
            printf("%s", builder.dumpEverything().c_str());
            return 0;
        }
        const double compile_start = now_ns();
        const std::string bytecode = Luau::compile(source, options);
        const double compile_ns = now_ns() - compile_start;
        lua_State *state = luaL_newstate();
        if (!state) { fprintf(stderr, "cannot create Luau state\n"); return 1; }
        // These cases use no standard-library functions. Keep both hosts at
        // core-only registration so startup/memory do not include Luau-only
        // library initialization.
        luaL_sandbox(state);
        if (luau_load(state, "=benchmark", bytecode.data(), bytecode.size(), 0) != 0 ||
            lua_pcall(state, 0, 1, 0) != 0 || !lua_isfunction(state, -1)) {
            fprintf(stderr, "Luau initialization failed: %s\n", lua_tostring(state, -1));
            lua_close(state);
            return 1;
        }
        // Keep the kernel on the stack throughout; it remains a GC root.
        std::vector<double> times(args.samples);
        double first_result_ns = 0, checksum = 0;
        for (int i = -args.warmups; i < args.samples; ++i) {
            const double before = now_ns();
            lua_pushvalue(state, 1);
            lua_pushnumber(state, args.rounds);
            lua_pushnumber(state, args.size);
            lua_pushnumber(state, args.seed);
            if (lua_pcall(state, 3, 1, 0) != 0 || !lua_isnumber(state, -1)) {
                fprintf(stderr, "Luau execution failed: %s\n", lua_tostring(state, -1));
                lua_close(state);
                return 1;
            }
            const double elapsed = now_ns() - before;
            if (i == -args.warmups) first_result_ns = now_ns() - started;
            checksum = lua_tonumber(state, -1);
            lua_pop(state, 1);
            if (!valid_result(&args, checksum)) { lua_close(state); return 1; }
            if (i >= 0) times[i] = elapsed;
        }
        printf("{\"schema\":1,\"engine\":\"luau\",\"read_ns\":%.0f,\"compile_ns\":%.0f,"
               "\"typecheck_ns\":null,\"first_result_from_main_ns\":%.0f,\"peak_process_bytes\":%zu,"
               "\"checksum\":%.17g,\"samples_ns\":[", read_ns, compile_ns, first_result_ns, peak_process_bytes(), checksum);
        for (int i = 0; i < args.samples; ++i) printf("%s%.0f", i ? "," : "", times[i]);
        printf("]}\n");
        lua_close(state);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "Luau runner: %s\n", error.what());
        return 1;
    }
}
