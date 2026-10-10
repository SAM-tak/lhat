// Public C ABI for native L^ extensions. Do not link a second copy of lhat.
// Use the matching L^ public headers; call non-inline APIs through this table.
#ifndef LHAT_EXTENSION_H
#define LHAT_EXTENSION_H

#include <lhat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LHAT_EXTENSION_ABI 2u
#define LHAT_EXTENSION_TYPES 0u
#define LHAT_EXTENSION_MEMBERS 1u

#if defined(_WIN32)
#define LHAT_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LHAT_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

// Append-only within an ABI version. Changing a field requires a new version.
#define LHAT_HOST_FUNCTIONS(X) \
    X(void *, lhat_alloc, (size_t size)) \
    X(void, lhat_free, (void *pointer)) \
    X(bool, lhat_register_type, (LhatProgram *program, const char *module, const char *name)) \
    X(const LhatHostDataTag *, lhat_register_hostdata_type, (LhatProgram *program, const char *module, const char *name)) \
    X(const LhatHostDataTag *, lhat_register_hostdata_subtype, (LhatProgram *program, const char *module, const char *name, const char *base_module, const char *base_name)) \
    X(bool, lhat_register_hostdata_shared, (LhatProgram *program, const char *module, const char *name, LhatHostHoldFn retain, LhatHostHoldFn let_go, void *context)) \
    X(bool, lhat_register_member, (LhatProgram *program, const char *module, const char *type, const char *name, const char *signature, LhatHostFn call, void *context)) \
    X(bool, lhat_register_func, (LhatProgram *program, const char *module, const char *name, const char *signature, LhatHostFn call, void *context)) \
    X(bool, lhat_register_global, (LhatProgram *program, const char *name, const char *signature, LhatHostFn call, void *context)) \
    X(bool, lhat_register_enum, (LhatProgram *program, const char *module, const char *type, const char *name, const char *const *members, size_t count)) \
    X(bool, lhat_register_enum_valued, (LhatProgram *program, const char *module, const char *type, const char *name, const char *const *members, const int64_t *values, size_t count)) \
    X(bool, lhat_register_error_kind, (LhatProgram *program, const char *module, const char *name, const char *const *variant_names, size_t variant_count, const LhatErrorKind **out_group, const LhatErrorKind **out_variants)) \
    X(bool, lhat_program_on_dispose, (LhatProgram *program, LhatProgramDisposeFn call, void *context)) \
    X(bool, lhat_machine_make_table, (LhatMachine *machine, LhatValue *out)) \
    X(bool, lhat_machine_table_set, (LhatMachine *machine, LhatTable *table, LhatValue key, LhatValue value, bool *refused)) \
    X(bool, lhat_machine_make_hostdata, (LhatMachine *machine, const LhatHostDataTag *tag, void *pointer, LhatValue *out)) \
    X(void *, lhat_hostdata_pointer, (LhatValue value, const LhatHostDataTag *tag)) \
    X(bool, lhat_machine_make_error, (LhatMachine *machine, const LhatErrorKind *kind, const char *message, LhatValue cause, LhatValue *out)) \
    X(bool, lhat_machine_make_string, (LhatMachine *machine, const char *text, size_t length, LhatValue *out)) \
    X(bool, lhat_machine_registered, (LhatMachine *machine, const char *module, const char *type, const char *name, LhatValue *out)) \
    X(LhatTable *, lhat_machine_host_root, (LhatMachine *machine)) \
    X(LhatRunResult, lhat_machine_call, (LhatMachine *machine, LhatValue callee, const LhatValue *arguments, size_t count)) \
    X(bool, lhat_machine_panic_text, (LhatMachine *machine, const char *text)) \
    X(LhatValue, lhat_machine_weak_cache_get, (LhatMachine *machine, const void *key)) \
    X(bool, lhat_machine_weak_cache_put, (LhatMachine *machine, const void *key, LhatValue value)) \
    X(void, lhat_machine_weak_cache_forget, (LhatMachine *machine, const void *key)) \
    X(LhatValue, lhat_table_get, (const LhatTable *table, LhatValue key)) \
    X(LhatValue, lhat_table_get_bytes, (const LhatTable *table, const char *name, size_t length)) \
    X(bool, lhat_table_set, (LhatTable *table, LhatValue key, LhatValue value, bool *refused)) \
    X(size_t, lhat_table_length, (const LhatTable *table)) \
    X(size_t, lhat_value_text, (LhatValue value, char *out, size_t capacity)) \
    X(bool, lhat_value_equal, (LhatValue a, LhatValue b)) \
    X(void *, lhat_lookup_host_context, (const LhatProgram *program, const char *module, const char *type, const char *name))

typedef struct LhatExtensionAPI {
    uint32_t abi_version;
    size_t struct_size;
#define LHAT_API_FIELD(result, name, args) result (*name) args;
    LHAT_HOST_FUNCTIONS(LHAT_API_FIELD)
#undef LHAT_API_FIELD
} LhatExtensionAPI;

typedef struct LhatExtension {
    uint32_t abi_version;
    size_t struct_size;
    const char *lhat_version;
    size_t value_size;
    const char *name;
    // A full build's --dump-signatures output, embedded in this library.
    // May be absent for bootstrapping in a full build; required by VM-only.
    const uint8_t *signatures;
    size_t signatures_size;
    // Called for TYPES, then MEMBERS, after the host registers its own API.
    // All extensions finish TYPES before any enters MEMBERS. *state starts
    // NULL and is shared by these two calls for this program only. Register
    // program_on_dispose if state needs freeing. Return NULL on success or
    // an error message on failure. Never throw across the ABI.
    const char *(*register_bindings)(const LhatExtensionAPI *host,
                                    LhatProgram *program, uint32_t phase,
                                    void **state);
    // Optional process/module cleanup, called by extensions_free before unload.
    // All programs, machines, carried values and registry callbacks must already
    // be gone. Runs in reverse load order, also for static descriptors. May be
    // called without register_bindings ever having run. Never throw or reenter
    // the pool. Hosts must keep this pool until their final shutdown, not restart.
    void (*shutdown)(void);
} LhatExtension;

// Export this symbol from a shared library. A statically linked extension can
// hand its descriptor directly to lhat_extensions_add instead.
LHAT_EXTENSION_EXPORT const LhatExtension *lhat_extension_v2(void);

// The core knows no OS loader, filesystem, search path or manifest format.
// Callbacks run synchronously on the host's registration thread. Their context
// must outlive the pool; error (optional) supplies the most recent loader error.
typedef void (*LhatExtensionSymbol)(void);
typedef struct LhatExtensionLoader {
    void *context;
    void *(*open)(void *context, const char *path);
    LhatExtensionSymbol (*symbol)(void *context, void *library, const char *name);
    void (*close)(void *context, void *library);
    const char *(*error)(void *context);
} LhatExtensionLoader;

typedef struct LhatExtensions LhatExtensions;
typedef struct LhatExtensionModule LhatExtensionModule;

// NULL loader permits static descriptors only. No process-global pool: the
// host owns one and can reuse its modules across several programs/restarts.
LhatExtensions *lhat_extensions_new(const LhatExtensionLoader *loader);
// Call shutdown and close libraries in reverse load order, AFTER all their programs/machines,
// carried values, and process-wide registry callbacks have been released.
// In the usual single-host process: registry_dispose, then extensions_free.
// This function does not dispose the registry or any program for the caller.
void lhat_extensions_free(LhatExtensions *extensions);

// Keys/paths are copied and compared exactly. Hosts normalize paths and decide
// which files are allowed. Repeated loads return the same module without opening
// it again. Static descriptors (including signatures) must outlive the pool.
const LhatExtensionModule *lhat_extensions_load(LhatExtensions *extensions,
                                               const char *path);
const LhatExtensionModule *lhat_extensions_add(LhatExtensions *extensions,
                                              const char *key,
                                              const LhatExtension *descriptor);

// Register the selected modules, not every cached module: all TYPES, then all
// MEMBERS. Call before checking units. Each invocation has fresh per-program
// state; extensions own its cleanup via program_on_dispose, also on failure.
// VM-only reads each module's embedded signatures immediately before its call.
// A failed registration is not rolled back: discard the program. Calls on a
// pool and registrations must be serialized by the host.
bool lhat_extensions_register(LhatExtensions *extensions, LhatProgram *program,
                              const LhatExtensionModule *const *modules,
                              size_t count);
// Valid until the next pool operation. Empty after a successful operation.
const char *lhat_extensions_error(const LhatExtensions *extensions);

#ifdef __cplusplus
}
#endif
#endif
