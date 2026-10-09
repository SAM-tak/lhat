// Native binding modules. Loading policy belongs to the embedding host.
#include "lhat/extension.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const LhatExtensionAPI api = {
    LHAT_EXTENSION_ABI, sizeof(LhatExtensionAPI),
#define LHAT_API_ADDRESS(result, name, args) &name,
    LHAT_HOST_FUNCTIONS(LHAT_API_ADDRESS)
#undef LHAT_API_ADDRESS
};

struct LhatExtensionModule {
    struct LhatExtensionModule *next;
    LhatExtensions *owner;
    char *key;
    void *library;
    const LhatExtension *descriptor;
};

struct LhatExtensions {
    LhatExtensionLoader loader;
    LhatExtensionModule *modules;
    char error[1024];
};

static bool fail(LhatExtensions *pool, const char *format, ...)
{
    if (pool != NULL) {
        va_list args;
        va_start(args, format);
        vsnprintf(pool->error, sizeof(pool->error), format, args);
        va_end(args);
    }
    return false;
}

LhatExtensions *lhat_extensions_new(const LhatExtensionLoader *loader)
{
    if (loader != NULL && (loader->open == NULL || loader->symbol == NULL
                           || loader->close == NULL)) return NULL;
    LhatExtensions *pool = lhat_alloc(sizeof(*pool));
    if (pool == NULL) return NULL;
    memset(pool, 0, sizeof(*pool));
    if (loader != NULL) pool->loader = *loader;
    return pool;
}

void lhat_extensions_free(LhatExtensions *pool)
{
    if (pool == NULL) return;
    while (pool->modules != NULL) {
        LhatExtensionModule *module = pool->modules;
        pool->modules = module->next;
        if (module->library != NULL)
            pool->loader.close(pool->loader.context, module->library);
        lhat_free(module->key);
        lhat_free(module);
    }
    lhat_free(pool);
}

const char *lhat_extensions_error(const LhatExtensions *pool)
{
    return pool != NULL ? pool->error : "No extension pool";
}

static LhatExtensionModule *find(LhatExtensions *pool, const char *key)
{
    for (LhatExtensionModule *m = pool->modules; m != NULL; m = m->next)
        if (strcmp(m->key, key) == 0) return m;
    return NULL;
}

static bool validate(LhatExtensions *pool, const char *key, const LhatExtension *d)
{
    if (d == NULL || d->abi_version != LHAT_EXTENSION_ABI
        || d->struct_size != sizeof(*d))
        return fail(pool, "%s: incompatible extension ABI", key);
    if (d->lhat_version == NULL || strcmp(d->lhat_version, LHAT_VERSION) != 0
        || d->value_size != sizeof(LhatValue))
        return fail(pool, "%s: incompatible L^ version or value layout (host %s)", key, LHAT_VERSION);
    if (d->name == NULL || d->register_bindings == NULL)
        return fail(pool, "%s: incomplete extension descriptor", key);
    return true;
}

const LhatExtensionModule *lhat_extensions_add(LhatExtensions *pool,
                                              const char *key,
                                              const LhatExtension *descriptor)
{
    if (pool == NULL) return NULL;
    pool->error[0] = '\0';
    if (key == NULL || key[0] == '\0') {
        fail(pool, "An extension needs a nonempty key");
        return NULL;
    }
    LhatExtensionModule *module = find(pool, key);
    if (module != NULL) {
        if (module->descriptor == descriptor) return module;
        fail(pool, "%s: extension key already belongs to another descriptor", key);
        return NULL;
    }
    if (!validate(pool, key, descriptor)) return NULL;
    module = lhat_alloc(sizeof(*module));
    char *copy = lhat_alloc(strlen(key) + 1);
    if (module == NULL || copy == NULL) {
        lhat_free(module);
        lhat_free(copy);
        fail(pool, "%s: out of memory", key);
        return NULL;
    }
    strcpy(copy, key);
    *module = (LhatExtensionModule){pool->modules, pool, copy, NULL, descriptor};
    pool->modules = module;
    return module;
}

const LhatExtensionModule *lhat_extensions_load(LhatExtensions *pool, const char *path)
{
    if (pool == NULL) return NULL;
    pool->error[0] = '\0';
    if (path == NULL || path[0] == '\0') {
        fail(pool, "An extension needs a nonempty path");
        return NULL;
    }
    LhatExtensionModule *cached = find(pool, path);
    if (cached != NULL) return cached;
    if (pool->loader.open == NULL) {
        fail(pool, "%s: no shared-library loader supplied", path);
        return NULL;
    }
    void *library = pool->loader.open(pool->loader.context, path);
    if (library == NULL) {
        const char *why = pool->loader.error != NULL
            ? pool->loader.error(pool->loader.context) : NULL;
        fail(pool, "Could not load %s: %s", path, why != NULL ? why : "loader refused");
        return NULL;
    }
    LhatExtensionSymbol symbol = pool->loader.symbol(pool->loader.context, library,
                                                     "lhat_extension_v1");
    const LhatExtensionModule *module = NULL;
    if (symbol == NULL) {
        fail(pool, "%s: missing lhat_extension_v1 entry point", path);
    } else {
        const LhatExtension *(*entry)(void) = (const LhatExtension *(*)(void)) symbol;
        module = lhat_extensions_add(pool, path, entry());
    }
    if (module == NULL) {
        pool->loader.close(pool->loader.context, library);
    } else {
        // add creates this module; cached paths returned before open above.
        ((LhatExtensionModule *)module)->library = library;
    }
    return module;
}

bool lhat_extensions_register(LhatExtensions *pool, LhatProgram *program,
                              const LhatExtensionModule *const *modules,
                              size_t count)
{
    if (pool == NULL) return false;
    pool->error[0] = '\0';
    if (program == NULL || (count != 0 && modules == NULL)
        || count > SIZE_MAX / sizeof(void *))
        return fail(pool, "Invalid extension registration arguments");
    for (size_t i = 0; i < count; i++) {
        if (modules[i] == NULL || modules[i]->owner != pool)
            return fail(pool, "Extension module belongs to another pool");
        for (size_t j = 0; j < i; j++)
            if (modules[i] == modules[j])
                return fail(pool, "%s: duplicate extension registration", modules[i]->key);
    }
    if (count == 0) return true;
    void **states = lhat_alloc(count * sizeof(*states));
    if (states == NULL) return fail(pool, "Out of memory registering extensions");
    memset(states, 0, count * sizeof(*states));
    bool ok = true;
    for (uint32_t phase = LHAT_EXTENSION_TYPES; phase <= LHAT_EXTENSION_MEMBERS && ok; phase++) {
        for (size_t i = 0; i < count; i++) {
            const LhatExtension *d = modules[i]->descriptor;
            const char *key = modules[i]->key;
#if !LHAT_WITH_FRONTEND
            if (d->signatures == NULL || d->signatures_size == 0) {
                ok = fail(pool, "%s: VM-only requires an embedded signature table", key);
                break;
            }
            // Registration resolves descriptors immediately; replacing the
            // table leaves previously registered types/functions intact.
            if (!lhat_program_read_signatures(program, d->signatures, d->signatures_size)) {
                ok = fail(pool, "%s: invalid or incompatible embedded signature table", key);
                break;
            }
#endif
            const char *error = d->register_bindings(&api, program, phase, &states[i]);
            if (error != NULL) {
                ok = fail(pool, "%s (%s): %s", key,
                          phase == LHAT_EXTENSION_TYPES ? "types" : "members", error);
                break;
            }
        }
    }
    lhat_free(states);
    return ok;
}
