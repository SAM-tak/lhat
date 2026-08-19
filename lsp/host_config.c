// L^ (lhat) -- LSP server: lhat-host.json, what a host's C would register.

#include "host_config.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "lhat/object.h"  // LhatHostValueFieldKind, LhatValue for the stub
#include "lhat/vm.h"      // LhatMachine, LhatHostFn's shape

struct LspHostConfig {
    cJSON *root;  // the parsed file; entries are read straight off it
};

// Never called: this server never runs a program, and lhat_register_* only
// requires the pointer to be there. The same shape workspace.c's stub has,
// duplicated so this file does not reach into workspace.c's statics.
static LhatValue stub_host_fn(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_nil();
}

LspHostConfig *lsp_host_config_parse(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (root == NULL) {
        return NULL;
    }
    LspHostConfig *config = (LspHostConfig *)malloc(sizeof *config);
    if (config == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    config->root = root;
    return config;
}

void lsp_host_config_free(LspHostConfig *config)
{
    if (config != NULL) {
        cJSON_Delete(config->root);
        free(config);
    }
}

static const char *string_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// 02 の 18.5's places, as lhat_program_dump_host_api writes them: an object
// keyed by name rather than a mask, since nothing outside C holds the enum.
//
// A name written false is passed over, and so is one this does not know --
// a dump from a later build, naming a place added since, still reads as far
// as it goes rather than being refused whole.
static uint32_t targets_of(const cJSON *object)
{
    static const struct {
        const char *name;
        uint32_t bit;
    } places[] = {
        {"unit", LHAT_ANNOTATION_UNIT},
        {"binding", LHAT_ANNOTATION_BINDING},
        {"field", LHAT_ANNOTATION_FIELD},
        {"member", LHAT_ANNOTATION_MEMBER},
        {"public", LHAT_ANNOTATION_PUBLIC},
        {"fileunique", LHAT_ANNOTATION_FILEUNIQUE},  // a count, not a place
    };

    uint32_t targets = 0;
    const cJSON *at = NULL;
    cJSON_ArrayForEach(at, object) {
        if (at->string == NULL || !cJSON_IsTrue(at)) {
            continue;
        }
        for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
            if (strcmp(at->string, places[i].name) == 0) {
                targets |= places[i].bit;
                break;
            }
        }
    }
    return targets;
}

static bool field_kind_of(const char *name, LhatHostValueFieldKind *out)
{
    static const struct {
        const char *name;
        LhatHostValueFieldKind kind;
    } kinds[] = {
        {"f32", LHAT_HVFIELD_F32}, {"f64", LHAT_HVFIELD_F64},
        {"i8", LHAT_HVFIELD_I8},   {"i16", LHAT_HVFIELD_I16},
        {"i32", LHAT_HVFIELD_I32}, {"i64", LHAT_HVFIELD_I64},
        {"u8", LHAT_HVFIELD_U8},   {"u16", LHAT_HVFIELD_U16},
        {"u32", LHAT_HVFIELD_U32},
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        if (strcmp(kinds[i].name, name) == 0) {
            *out = kinds[i].kind;
            return true;
        }
    }
    return false;
}

static void apply_type(const cJSON *entry, LhatProgram *program)
{
    const char *kind = string_of(entry, "kind");
    const char *module = string_of(entry, "module");
    const char *name = string_of(entry, "name");
    if (kind == NULL || module == NULL || name == NULL) {
        return;
    }

    if (strcmp(kind, "errordef") == 0) {
        const cJSON *variants =
            cJSON_GetObjectItemCaseSensitive(entry, "variants");
        int count = cJSON_IsArray(variants) ? cJSON_GetArraySize(variants) : 0;
        const char **names = NULL;
        if (count > 0) {
            names = (const char **)malloc((size_t)count * sizeof *names);
            if (names == NULL) {
                return;
            }
            for (int i = 0; i < count; i++) {
                const cJSON *v = cJSON_GetArrayItem(variants, i);
                names[i] = cJSON_IsString(v) ? v->valuestring : "";
            }
        }
        lhat_register_error_kind(program, module, name, names, (size_t)count,
                                 NULL, NULL);
        free(names);
        return;
    }

    if (strcmp(kind, "hostdata") == 0) {
        lhat_register_hostdata_type(program, module, name);
        return;
    }

    if (strcmp(kind, "hostvalue") == 0) {
        const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(entry, "size");
        if (!cJSON_IsNumber(size_item) || size_item->valueint <= 0) {
            return;
        }
        if (lhat_register_hostvalue_type(program, module, name,
                                         (size_t)size_item->valueint) == NULL) {
            return;
        }
        const cJSON *fields = cJSON_GetObjectItemCaseSensitive(entry, "fields");
        const cJSON *field = NULL;
        cJSON_ArrayForEach(field, fields) {
            const char *field_name = string_of(field, "name");
            const char *type_name = string_of(field, "type");
            const cJSON *offset =
                cJSON_GetObjectItemCaseSensitive(field, "offset");
            LhatHostValueFieldKind field_kind;
            if (field_name == NULL || type_name == NULL ||
                !cJSON_IsNumber(offset) || offset->valueint < 0 ||
                !field_kind_of(type_name, &field_kind)) {
                continue;
            }
            lhat_register_hostvalue_field(program, module, name, field_name,
                                          (size_t)offset->valueint,
                                          field_kind);
        }
    }
}

static void apply_function(const cJSON *entry, LhatProgram *program)
{
    const char *kind = string_of(entry, "kind");
    const char *name = string_of(entry, "name");
    const char *signature = string_of(entry, "signature");
    if (kind == NULL || name == NULL || signature == NULL) {
        return;
    }

    if (strcmp(kind, "global") == 0) {
        lhat_register_global(program, name, signature, stub_host_fn, NULL);
        return;
    }

    const char *module = string_of(entry, "module");
    if (module == NULL) {
        return;
    }
    if (strcmp(kind, "func") == 0) {
        lhat_register_func(program, module, name, signature, stub_host_fn,
                           NULL);
        return;
    }

    const char *type = string_of(entry, "type");
    if (type == NULL) {
        return;
    }
    if (strcmp(kind, "member") == 0) {
        lhat_register_member(program, module, type, name, signature,
                             stub_host_fn, NULL);
    } else if (strcmp(kind, "hostvalue_member") == 0) {
        lhat_register_hostvalue_member(program, module, type, name, signature,
                                       stub_host_fn, NULL);
    }
}

void lsp_host_config_apply(const LspHostConfig *config, LhatProgram *program)
{
    if (config == NULL) {
        return;
    }

    const cJSON *entry = NULL;
    const cJSON *types = cJSON_GetObjectItemCaseSensitive(config->root, "types");
    cJSON_ArrayForEach(entry, types) {
        apply_type(entry, program);
    }

    const cJSON *functions =
        cJSON_GetObjectItemCaseSensitive(config->root, "functions");
    cJSON_ArrayForEach(entry, functions) {
        apply_function(entry, program);
    }

    // 02 の 18.5: without these every @name a unit writes is one the checker
    // has never heard of, which is a red line under correct code.
    const cJSON *annotations =
        cJSON_GetObjectItemCaseSensitive(config->root, "annotations");
    cJSON_ArrayForEach(entry, annotations) {
        const char *module = string_of(entry, "module");
        const char *name = string_of(entry, "name");
        const char *signature = string_of(entry, "signature");
        const cJSON *targets =
            cJSON_GetObjectItemCaseSensitive(entry, "targets");
        if (module == NULL || name == NULL || !cJSON_IsObject(targets) ||
            !lhat_register_annotation(program, module, name,
                                      targets_of(targets))) {
            continue;
        }
        if (signature != NULL) {
            lhat_register_annotation_signature(program, name, signature);
        }
        // 18.5.1: the names it was registered as an alternative to, handed
        // back one at a time -- the same way they were said.
        const cJSON *exclusives =
            cJSON_GetObjectItemCaseSensitive(entry, "exclusives");
        const cJSON *other = NULL;
        cJSON_ArrayForEach(other, exclusives) {
            if (cJSON_IsString(other) && other->valuestring != NULL) {
                lhat_register_annotation_exclusive(program, name,
                                                   other->valuestring);
            }
        }
        // 18.5.2: and the names one of which has to stand beside it.
        const cJSON *requisites =
            cJSON_GetObjectItemCaseSensitive(entry, "requisites");
        cJSON_ArrayForEach(other, requisites) {
            if (cJSON_IsString(other) && other->valuestring != NULL) {
                lhat_register_annotation_requisite(program, name,
                                                   other->valuestring);
            }
        }
    }

    const cJSON *bindings =
        cJSON_GetObjectItemCaseSensitive(config->root, "bindings");
    cJSON_ArrayForEach(entry, bindings) {
        const char *name = string_of(entry, "name");
        const char *member = string_of(entry, "member");
        if (name != NULL && member != NULL) {
            lhat_bind_initial(program, name, member);
        }
    }
}
