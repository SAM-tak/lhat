#include "common.h"
#include "lua.h"
#include "lauxlib.h"

int main(int argc, char **argv)
{
    const double started = now_ns();
    CompareArgs args;
    if (!parse_args(argc, argv, &args)) return 2;
    int exit_code = 1;
    FILE *file = NULL;
    char *source = NULL;
    double *times = NULL;
    lua_State *state = NULL;
    const double read_start = now_ns();
    file = fopen(args.path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "cannot read %s\n", args.path);
        goto cleanup;
    }
    const long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) goto cleanup;
    source = (char *)malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) goto cleanup;
    source[length] = '\0';
    fclose(file);
    file = NULL;
    const double read_ns = now_ns() - read_start;
    state = luaL_newstate();
    if (!state) goto cleanup;
    // Match the other two hosts: no standard libraries or host functions.
    const double compile_start = now_ns();
    const int loaded = luaL_loadbufferx(state, source, (size_t)length, "=benchmark", "t");
    const double compile_ns = now_ns() - compile_start;
    if (loaded != LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK || !lua_isfunction(state, -1)) {
        fprintf(stderr, "Lua initialization failed: %s\n", lua_tostring(state, -1));
        goto cleanup;
    }
    // The returned function stays at stack slot 1 as a GC root.
    times = (double *)calloc((size_t)args.samples, sizeof(double));
    if (!times) goto cleanup;
    double checksum = 0, first_result_ns = 0;
    for (int i = -args.warmups; i < args.samples; ++i) {
        const double before = now_ns();
        lua_pushvalue(state, 1);
        lua_pushinteger(state, args.rounds);
        lua_pushinteger(state, args.size);
        lua_pushinteger(state, args.seed);
        const int status = lua_pcall(state, 3, 1, 0);
        const double elapsed = now_ns() - before;
        if (i == -args.warmups) first_result_ns = now_ns() - started;
        if (status != LUA_OK || lua_type(state, -1) != LUA_TNUMBER) {
            fprintf(stderr, "Lua execution failed: %s\n", lua_tostring(state, -1));
            goto cleanup;
        }
        checksum = lua_tonumber(state, -1);
        lua_pop(state, 1);
        if (!valid_result(&args, checksum)) goto cleanup;
        if (i >= 0) times[i] = elapsed;
    }
    printf("{\"schema\":1,\"engine\":\"lua\",\"version\":\"%s\",\"read_ns\":%.0f,\"compile_ns\":%.0f,"
           "\"typecheck_ns\":null,\"first_result_from_main_ns\":%.0f,\"peak_process_bytes\":%zu,"
           "\"checksum\":%.17g,\"samples_ns\":[", LUA_RELEASE, read_ns, compile_ns,
           first_result_ns, peak_process_bytes(), checksum);
    for (int i = 0; i < args.samples; ++i) printf("%s%.0f", i ? "," : "", times[i]);
    printf("]}\n");
    exit_code = 0;
cleanup:
    if (file) fclose(file);
    free(source);
    free(times);
    if (state) lua_close(state);
    return exit_code;
}
