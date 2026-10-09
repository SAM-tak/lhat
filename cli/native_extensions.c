#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "native_extensions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static const char **paths;
static size_t count;
static LhatExtensions *pool;
static char error[512];

bool cli_extension_path(const char *path)
{
    const char **more = realloc(paths, (count + 1) * sizeof(*more));
    if (more == NULL) return false;
    paths = more;
    paths[count++] = path;
    return true;
}

static void *open_library(void *context, const char *path)
{
    (void)context;
#ifdef _WIN32
    // Resolve explicitly named files; include their directory in dependency lookup.
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(*wide)) : NULL;
    if (wide == NULL) { snprintf(error, sizeof(error), "Invalid UTF-8 path or out of memory"); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length);
    DWORD capacity = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *absolute = capacity ? malloc((size_t)capacity * sizeof(*absolute)) : NULL;
    HMODULE module = NULL;
    if (absolute != NULL && GetFullPathNameW(wide, capacity, absolute, NULL))
        module = LoadLibraryExW(absolute, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == NULL) snprintf(error, sizeof(error), "Windows loader error %lu", (unsigned long)GetLastError());
    free(absolute);
    free(wide);
    return module;
#else
    char *absolute = realpath(path, NULL);
    if (absolute == NULL) { snprintf(error, sizeof(error), "Could not resolve the library path"); return NULL; }
    void *module = dlopen(absolute, RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) snprintf(error, sizeof(error), "%s", dlerror());
    free(absolute);
    return module;
#endif
}
static LhatExtensionSymbol symbol(void *context, void *library, const char *name)
{
    (void)context;
#ifdef _WIN32
    return (LhatExtensionSymbol)GetProcAddress((HMODULE)library, name);
#else
    // POSIX specifies dlsym's conversion to a function pointer.
    return (LhatExtensionSymbol)dlsym(library, name);
#endif
}
static void close_library(void *context, void *library)
{
    (void)context;
#ifdef _WIN32
    FreeLibrary((HMODULE)library);
#else
    dlclose(library);
#endif
}
static const char *loader_error(void *context) { (void)context; return error; }

bool cli_extensions_register(LhatProgram *program)
{
    if (count == 0) return true;
    LhatExtensionLoader loader = {NULL, open_library, symbol, close_library, loader_error};
    pool = lhat_extensions_new(&loader);
    const LhatExtensionModule **modules = malloc(count * sizeof(*modules));
    bool ok = pool != NULL && modules != NULL;
    for (size_t i = 0; ok && i < count; ++i) {
        modules[i] = lhat_extensions_load(pool, paths[i]);
        ok = modules[i] != NULL;
    }
    if (ok) ok = lhat_extensions_register(pool, program, modules, count);
    if (!ok) fprintf(stderr, "lhat: extensions: %s\n",
        pool != NULL && modules != NULL ? lhat_extensions_error(pool) : "out of memory");
    free(modules);
    return ok;
}

void cli_extensions_dispose(void)
{
    // Registered before the diagnostic driver's atexit handler, so that driver
    // and the dump program are already gone when registry callbacks are released.
    if (pool != NULL) {
        lhat_registry_dispose();
        lhat_extensions_free(pool);
        pool = NULL;
    }
    free(paths);
    paths = NULL;
    count = 0;
}
