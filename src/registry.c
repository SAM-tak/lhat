// L^ (lhat) -- the one registry of what a host declared. See registry.h for
// why the identities live here rather than on a program.

#include "registry.h"

#include <string.h>

#include "gc.h"  // LHAT_GC_BLACK -- the heap the kinds are born on
#include "grow.h"
#include "lhat/config.h"
#include "lhat/port.h"

// One declaration of each shape. The tags are handed out by address, so an
// array of pointers rather than of structs: growing must not move them.
typedef struct {
    LhatHostDataTag **hostdata;
    size_t hostdata_count;
    size_t hostdata_capacity;

    LhatHostValueTag **hostvalue;
    size_t hostvalue_count;
    size_t hostvalue_capacity;

    // 04 の 2.4: what one errordef^-shaped declaration made. Kept whole
    // because a second declaration of the same name has to be compared
    // against the list it was made with, not only against its name.
    struct Declared {
        char *module;
        char *name;
        const LhatErrorKind *group;
        char **variant_names;
        const LhatErrorKind **variants;
        size_t variant_count;
    } *errors;
    size_t error_count;
    size_t error_capacity;

    // The kinds and the strings they carry. Born black for the reason
    // lhat_chunk_init gives: no machine's collection may write into them,
    // and here more than anywhere, since every machine of every program may
    // be reading one at once.
    LhatHeap heap;
    bool heap_ready;
} Registry;

static Registry one;

static LhatHeap *registry_heap(void)
{
    if (!one.heap_ready) {
        one.heap.white = LHAT_GC_BLACK;
        one.heap_ready = true;
    }
    return &one.heap;
}

static char *duplicate(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)lhat_alloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

static bool same(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

// ---------------------------------------------------------------------------
// 05 の 8.8: hostdata
// ---------------------------------------------------------------------------

const LhatHostDataTag *lhat_registry_hostdata(const char *module,
                                              const char *name)
{
    if (module == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < one.hostdata_count; i++) {
        if (same(one.hostdata[i]->module, module) &&
            same(one.hostdata[i]->name, name)) {
            return one.hostdata[i];
        }
    }

    LhatHostDataTag *tag = (LhatHostDataTag *)lhat_calloc(1, sizeof *tag);
    char *module_copy = duplicate(module);
    char *name_copy = duplicate(name);
    if (tag == NULL || module_copy == NULL || name_copy == NULL) {
        lhat_free(tag);
        lhat_free(module_copy);
        lhat_free(name_copy);
        return NULL;
    }
    tag->module = module_copy;
    tag->name = name_copy;

    LHAT_GROW(one.hostdata, one.hostdata_count, one.hostdata_capacity, 8, {
        lhat_free(tag);
        lhat_free(module_copy);
        lhat_free(name_copy);
        return NULL;
    });
    one.hostdata[one.hostdata_count++] = tag;
    return tag;
}

bool lhat_registry_set_release(const LhatHostDataTag *tag, LhatHostFn release,
                               void *context)
{
    if (tag == NULL || release == NULL) {
        return false;
    }
    LhatHostDataTag *mine = (LhatHostDataTag *)tag;
    if (mine->release != NULL) {
        // 8.8: the declaration says one way to hand a value back. A second
        // program registering the same dispose^ is that same declaration
        // made twice; a different one would be two types under one name.
        return mine->release == release && mine->release_context == context;
    }
    mine->release = release;
    mine->release_context = context;
    return true;
}

// ---------------------------------------------------------------------------
// 05 の 8.9: host values
// ---------------------------------------------------------------------------

const LhatHostValueTag *lhat_registry_hostvalue(const char *module,
                                                const char *name, size_t size)
{
    if (module == NULL || name == NULL || size == 0 ||
        size > LHAT_HOSTVALUE_MAX_BYTES) {
        return NULL;
    }
    for (size_t i = 0; i < one.hostvalue_count; i++) {
        if (!same(one.hostvalue[i]->module, module) ||
            !same(one.hostvalue[i]->name, name)) {
            continue;
        }
        // The width is what every frame holding one was laid out against, so
        // a second declaration of a different size is not the same type.
        return one.hostvalue[i]->size == size ? one.hostvalue[i] : NULL;
    }

    LhatHostValueTag *tag = (LhatHostValueTag *)lhat_calloc(1, sizeof *tag);
    char *module_copy = duplicate(module);
    char *name_copy = duplicate(name);
    if (tag == NULL || module_copy == NULL || name_copy == NULL) {
        lhat_free(tag);
        lhat_free(module_copy);
        lhat_free(name_copy);
        return NULL;
    }
    tag->module = module_copy;
    tag->name = name_copy;
    tag->size = size;
    tag->width = 1 + (size + 7) / 8;  // one head slot, then the bytes
    // The index is the process's, so a machine's array of members tables is
    // taken to the width of everything declared rather than to what one
    // program happened to declare.
    tag->index = one.hostvalue_count;

    LHAT_GROW(one.hostvalue, one.hostvalue_count, one.hostvalue_capacity, 4, {
        lhat_free(tag);
        lhat_free(module_copy);
        lhat_free(name_copy);
        return NULL;
    });
    one.hostvalue[one.hostvalue_count++] = tag;
    return tag;
}

bool lhat_registry_hostvalue_field(const LhatHostValueTag *tag,
                                   const char *name, size_t offset,
                                   LhatHostValueFieldKind kind)
{
    if (tag == NULL || name == NULL) {
        return false;
    }
    LhatHostValueTag *mine = (LhatHostValueTag *)tag;
    for (size_t i = 0; i < mine->field_count; i++) {
        if (!same(mine->fields[i].name, name)) {
            continue;
        }
        // Declared again by a second program: the same field, or two.
        return mine->fields[i].offset == offset && mine->fields[i].kind == kind;
    }

    char *name_copy = duplicate(name);
    if (name_copy == NULL) {
        return false;
    }
    LhatHostValueField *bigger = (LhatHostValueField *)lhat_realloc(
        mine->fields, (mine->field_count + 1) * sizeof *bigger);
    if (bigger == NULL) {
        lhat_free(name_copy);
        return false;
    }
    mine->fields = bigger;
    mine->fields[mine->field_count].name = name_copy;
    mine->fields[mine->field_count].offset = offset;
    mine->fields[mine->field_count].kind = kind;
    mine->field_count++;
    return true;
}

size_t lhat_registry_hostvalue_count(void)
{
    return one.hostvalue_count;
}

// ---------------------------------------------------------------------------
// 04 の 2.4: error kinds
// ---------------------------------------------------------------------------

// Whether a declaration already here was made with this very list.
static bool variants_match(const struct Declared *was,
                           const char *const *variant_names,
                           size_t variant_count)
{
    if (was->variant_count != variant_count) {
        return false;
    }
    for (size_t i = 0; i < variant_count; i++) {
        if (!same(was->variant_names[i], variant_names[i])) {
            return false;
        }
    }
    return true;
}

static void answer_with(const struct Declared *was,
                        const LhatErrorKind **out_group,
                        const LhatErrorKind **out_variants)
{
    if (out_group != NULL) {
        *out_group = was->group;
    }
    if (out_variants != NULL) {
        for (size_t i = 0; i < was->variant_count; i++) {
            out_variants[i] = was->variants[i];
        }
    }
}

static void free_declared(struct Declared *at, size_t filled)
{
    for (size_t i = 0; i < filled; i++) {
        lhat_free(at->variant_names[i]);
    }
    lhat_free(at->variant_names);
    lhat_free((void *)at->variants);
    lhat_free(at->module);
    lhat_free(at->name);
    memset(at, 0, sizeof *at);
}

bool lhat_registry_error_kind(const char *module, const char *name,
                              const char *const *variant_names,
                              size_t variant_count,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants)
{
    if (module == NULL || name == NULL) {
        return false;
    }
    for (size_t i = 0; i < one.error_count; i++) {
        struct Declared *was = &one.errors[i];
        if (!same(was->module, module) || !same(was->name, name)) {
            continue;
        }
        if (!variants_match(was, variant_names, variant_count)) {
            return false;  // 2.4: a different list is a different declaration
        }
        answer_with(was, out_group, out_variants);
        return true;
    }

    LhatHeap *heap = registry_heap();
    LhatString *group_name = lhat_string_new(heap, name, strlen(name));
    LhatErrorKind *group =
        group_name != NULL ? lhat_error_kind_new(heap, NULL, group_name) : NULL;
    if (group == NULL) {
        return false;
    }

    struct Declared made;
    memset(&made, 0, sizeof made);
    made.module = duplicate(module);
    made.name = duplicate(name);
    made.group = group;
    made.variant_count = variant_count;
    if (made.module == NULL || made.name == NULL) {
        free_declared(&made, 0);
        return false;
    }
    if (variant_count > 0) {
        made.variant_names = (char **)lhat_calloc(variant_count,
                                                  sizeof *made.variant_names);
        made.variants = (const LhatErrorKind **)lhat_calloc(
            variant_count, sizeof *made.variants);
        if (made.variant_names == NULL || made.variants == NULL) {
            free_declared(&made, 0);
            return false;
        }
    }

    for (size_t i = 0; i < variant_count; i++) {
        size_t name_length = strlen(name);
        size_t variant_length = strlen(variant_names[i]);
        char qualified[LHAT_QUALIFIED_NAME_BUFFER];
        size_t total = name_length + 1 + variant_length;
        LhatString *text = NULL;
        if (total < sizeof qualified) {
            // "IOError.NotFound" -- the spelling declare_error (compile.c)
            // makes for typeof^. The module is not part of it.
            memcpy(qualified, name, name_length);
            qualified[name_length] = '.';
            memcpy(qualified + name_length + 1, variant_names[i],
                   variant_length);
            text = lhat_string_new(heap, qualified, total);
        }
        LhatErrorKind *kind =
            text != NULL ? lhat_error_kind_new(heap, group, text) : NULL;
        made.variant_names[i] = kind != NULL ? duplicate(variant_names[i]) : NULL;
        if (kind == NULL || made.variant_names[i] == NULL) {
            // The kinds already on the heap stay -- it is freed whole, the
            // way a chunk's constants are, rather than unwound here.
            free_declared(&made, i);
            return false;
        }
        made.variants[i] = kind;
    }

    LHAT_GROW(one.errors, one.error_count, one.error_capacity, 4, {
        free_declared(&made, variant_count);
        return false;
    });
    one.errors[one.error_count++] = made;
    answer_with(&made, out_group, out_variants);
    return true;
}

// ---------------------------------------------------------------------------
// Letting go
// ---------------------------------------------------------------------------

void lhat_registry_dispose(void)
{
    for (size_t i = 0; i < one.hostdata_count; i++) {
        lhat_free((void *)one.hostdata[i]->module);
        lhat_free((void *)one.hostdata[i]->name);
        lhat_free(one.hostdata[i]);
    }
    lhat_free(one.hostdata);

    for (size_t i = 0; i < one.hostvalue_count; i++) {
        LhatHostValueTag *tag = one.hostvalue[i];
        for (size_t f = 0; f < tag->field_count; f++) {
            lhat_free((void *)tag->fields[f].name);
        }
        lhat_free(tag->fields);
        lhat_free((void *)tag->module);
        lhat_free((void *)tag->name);
        lhat_free(tag);
    }
    lhat_free(one.hostvalue);

    for (size_t i = 0; i < one.error_count; i++) {
        free_declared(&one.errors[i], one.errors[i].variant_count);
    }
    lhat_free(one.errors);

    if (one.heap_ready) {
        lhat_object_free_all(&one.heap);
    }
    memset(&one, 0, sizeof one);
}
