// A real shared library, deliberately not linked to another lhat runtime.
#include <lhat/extension.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef PEER
#define MODULE "cli_peer"
#else
#define MODULE "cli_probe"
#endif
static void record(const char *stage) {
    const char *path = getenv("LHAT_CLI_EXTENSION_LOG");
    if (path != NULL) {
        FILE *file = fopen(path, "a");
        if (file != NULL) { fprintf(file, "%s %s\n", MODULE, stage); fclose(file); }
    }
}
static void dispose(void *context) { (void)context; record("program"); }
static void shutdown(void) { record("shutdown"); }
static void answer(LhatMachine *m, void *c, const LhatValue *a, size_t n, LhatValue *out, int *count) {
    (void)m; (void)c; (void)a; (void)n; out[0] = lhat_integer(42); *count = 1;
}
static const char *install(const LhatExtensionAPI *api, LhatProgram *p, uint32_t phase, void **state) {
    (void)state;
    if (phase == LHAT_EXTENSION_TYPES)
        return api->lhat_register_type(p, MODULE, "Marker") && api->lhat_program_on_dispose(p, dispose, NULL) ? NULL : "types failed";
#ifdef PEER
    const char *signature = "f^cli_probe.Marker -> number^;";
#else
    const char *signature = "f^ -> number^;";
#endif
    return api->lhat_register_func(p, MODULE, "answer", signature, answer, NULL) ? NULL : "members failed";
}
LHAT_EXTENSION_EXPORT const LhatExtension *lhat_extension_v2(void) {
    static const LhatExtension d = {LHAT_EXTENSION_ABI, sizeof(LhatExtension), LHAT_VERSION,
        sizeof(LhatValue), MODULE, NULL, 0, install, shutdown};
    return &d;
}
