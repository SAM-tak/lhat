// L^ (lhat) -- a program: one unit and everything it requires.

#include "program_internal.h"

#include <stdlib.h>
#include <string.h>

#include "gc.h"  // LHAT_GC_BLACK -- host_error_heap の初期色 (04 の 12.4)
#include "grow.h"
#include "port.h"
#include "type.h"

#include "vm.h"  // 05 の 5.3: the units are compiled here too, not only checked

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// 5.3 keys the cache on the resolved path, so two spellings of one unit have
// to arrive at the same string. Separators are unified and '.' and '..' are
// folded away; nothing else is interpreted, since the loader may not be a
// file system at all.
static char *normalise_path(const char *path)
{
    size_t length = strlen(path);
    char *out = (char *)lhat_alloc(length + 1);
    if (out == NULL) {
        return NULL;
    }

    // Segment starts within `out`, so that '..' can rewind to the previous
    // one rather than being resolved textually against the whole string.
    size_t *starts = (size_t *)lhat_alloc((length + 2) * sizeof *starts);
    if (starts == NULL) {
        lhat_free(out);
        return NULL;
    }

    size_t written = 0;
    size_t depth = 0;
    size_t i = 0;
    while (i <= length) {
        size_t begin = i;
        while (i < length && path[i] != '/' && path[i] != '\\') {
            i++;
        }
        size_t segment = i - begin;

        if (segment == 0 || (segment == 1 && path[begin] == '.')) {
            // Nothing to add: an empty segment or a '.'.
        } else if (segment == 2 && path[begin] == '.' && path[begin + 1] == '.' &&
                   depth > 0) {
            written = starts[--depth];
            if (written > 0) {
                written--;  // drop the separator that led into the segment
            }
        } else {
            if (written > 0) {
                out[written++] = '/';
            }
            starts[depth++] = written;
            memcpy(out + written, path + begin, segment);
            written += segment;
        }

        if (i == length) {
            break;
        }
        i++;  // the separator
    }

    out[written] = '\0';
    lhat_free(starts);
    return out;
}

// 5.1: a require^ is relative to the unit that wrote it.
static char *resolve_against(const char *base, const char *relative,
                             size_t relative_length)
{
    size_t base_length = 0;
    for (size_t i = 0; base != NULL && base[i] != '\0'; i++) {
        if (base[i] == '/' || base[i] == '\\') {
            base_length = i + 1;
        }
    }

    char *joined = (char *)lhat_alloc(base_length + relative_length + 1);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined, base, base_length);
    memcpy(joined + base_length, relative, relative_length);
    joined[base_length + relative_length] = '\0';

    char *resolved = normalise_path(joined);
    lhat_free(joined);
    return resolved;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static char *duplicate(const char *text);

static void report(LhatProgram *program, LhatProgramErrorCode code,
                   const char *path)
{
    LHAT_GROW(program->diagnostics, program->diagnostic_count,
              program->diagnostic_capacity, 4, return);

    LhatProgramDiagnostic *d = &program->diagnostics[program->diagnostic_count];
    d->code = code;
    d->path = duplicate(path);
    if (d->path == NULL) {
        return;
    }
    program->diagnostic_count++;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

static char *duplicate(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)lhat_alloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

static LhatUnit *find_unit(LhatProgram *program, const char *path)
{
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (strcmp(u->path, path) == 0) {
            return u;
        }
    }
    return NULL;
}

static LhatUnit *check_path(LhatProgram *program, char *path);

// What the checker asks when it meets a require^ (05 の 6.1).
typedef struct {
    LhatProgram *program;
    LhatUnit *requiring;
} Resolution;

static LhatType *resolve_require(void *context, const char *path, size_t length,
                                 const char **module_name)
{
    Resolution *r = (Resolution *)context;
    char *resolved = resolve_against(r->requiring->path, path, length);
    if (resolved == NULL) {
        return NULL;
    }

    LhatUnit *unit = check_path(r->program, resolved);  // takes `resolved`
    if (unit == NULL || unit->state != LHAT_UNIT_DONE) {
        return NULL;
    }
    // 05 の 3 章: the unit outlives this program's checking, so handing the
    // text out rather than a copy is safe.
    if (module_name != NULL) {
        *module_name = unit->checked.module_name;
    }
    // A unit publishing nothing still loaded, so it answers with an empty
    // structure rather than with failure.
    return unit->checked.exports != NULL
               ? unit->checked.exports
               : lhat_type_table(&r->program->types);
}

// Takes ownership of `path`.
static LhatUnit *check_path(LhatProgram *program, char *path)
{
    LhatUnit *existing = find_unit(program, path);
    if (existing != NULL) {
        // 6.3: meeting a unit that is still being checked means the graph
        // has a cycle. Reported here, where both ends are known.
        if (existing->state == LHAT_UNIT_CHECKING) {
            report(program, LHAT_PROGRAM_ERR_CYCLE, path);
            lhat_free(path);
            return NULL;
        }
        // 5.3: loaded once. A second require^ gets the same unit.
        lhat_free(path);
        return existing;
    }

    LhatUnit *unit = (LhatUnit *)lhat_calloc(1, sizeof *unit);
    if (unit == NULL) {
        lhat_free(path);
        return NULL;
    }
    unit->path = path;
    unit->state = LHAT_UNIT_CHECKING;
    unit->next = program->units;
    program->units = unit;

    // 05 の 8.9: no loader is not an error of the program's -- it is a host
    // that never handed one over, and then nothing can be read.
    size_t length = 0;
    char *text = program->load != NULL
                     ? program->load(program->loader_context, unit->path,
                                     &length)
                     : NULL;
    if (text == NULL) {
        report(program, LHAT_PROGRAM_ERR_CANNOT_READ, unit->path);
        unit->state = LHAT_UNIT_FAILED;
        return unit;
    }

    lhat_source_init_from_string(&unit->source, unit->path, text, length);
    lhat_free(text);
    lhat_lexer_init(&unit->lexer, &unit->source);
    lhat_parse(&unit->lexer, &unit->parsed);
    unit->loaded = true;

    Resolution resolution;
    resolution.program = program;
    resolution.requiring = unit;

    LhatRequire require;
    require.resolve = resolve_require;
    require.context = &resolution;
    require.hosted = program->hosted;  // 05 の 8.7
    require.globals = program->globals;  // 05 の 8.6
    require.initial_names = (const char *const *)program->initial_names;
    require.initial_members = (const char *const *)program->initial_members;
    require.initial_count = program->initial_count;  // 05 の 8.2

    // The recursion is what puts the graph in dependency order (6.2): the
    // required unit finishes before this one's checking gets past the
    // require^ that asked for it.
    lhat_check_unit(unit->parsed.root, &unit->lexer, program->strict,
                    &program->types, &require, &unit->checked);

    unit->state = LHAT_UNIT_DONE;
    return unit;
}

// 05 の 5 章: the compile-time twin of resolve_require. The unit is already
// there -- checking put it there -- so this only has to say where it sits.
static size_t resolve_unit(void *context, const char *path, size_t length,
                           const char **module_name)
{
    Resolution *r = (Resolution *)context;
    char *resolved = resolve_against(r->requiring->path, path, length);
    if (resolved == NULL) {
        return LHAT_NO_UNIT;
    }
    LhatUnit *unit = find_unit(r->program, resolved);
    lhat_free(resolved);
    if (unit == NULL) {
        return LHAT_NO_UNIT;
    }
    if (module_name != NULL) {
        *module_name = unit->checked.module_name;
    }
    return unit->index;
}

// ---------------------------------------------------------------------------
// 05 の 8.7: what the host provides
// ---------------------------------------------------------------------------

// One thing the host registered, kept so that lhat_program_install can build
// the values once a machine exists. The type side is in `program->hosted`
// already, since the checker needs it before anything runs.
typedef struct LhatHostEntry {
    char *module;   // owned; the dotted path
    char *type;     // owned; NULL when the entry belongs to the module itself
    char *name;     // owned
    LhatHostFn call;  // NULL for a type, which carries no value of its own
    void *context;
    uint8_t parameters;
    bool has_variadic;  // 13.7: the signature ended in '...' -- see LhatHost
    bool takes_self;
    // 05 の 8.8: the tag values of this type carry. Kept on the entry so that
    // it lives as long as the program and points at the entry's own strings.
    LhatHostDataTag *tag;
} LhatHostEntry;

// 05 の 8.6: one member of L^ itself. Separate from the above because it does
// not land under L^.modules, so install puts it somewhere else.
typedef struct LhatGlobalEntry {
    char *name;  // owned
    LhatHostFn call;
    void *context;
    uint8_t parameters;
    bool has_variadic;
    bool takes_self;
} LhatGlobalEntry;

// The table type a dotted path names inside `owner`, made where the path does
// not reach one. The same walk 02 の 8.8 does, over text.
static LhatType *hosted_table(LhatProgram *program, LhatType *owner,
                              const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        LhatType *next = NULL;
        for (const LhatTypeMember *m = owner->v.table.members; m != NULL;
             m = m->next) {
            if (m->name_length == length &&
                memcmp(m->name, segment, length) == 0) {
                next = m->type;
                break;
            }
        }
        if (next == NULL) {
            next = lhat_type_table(&program->types);
            if (next == NULL ||
                lhat_type_add_member(&program->types, owner, segment, length,
                                     next) == NULL) {
                return NULL;
            }
            // 05 の 8.6: what the host registered is the machine's record of
            // it. Nothing written in L^ adds to it or writes over it -- the
            // host reaches it through this file's own API instead.
            next->v.table.sealed = true;
        }
        if (next->kind != LHAT_TYPE_TABLE) {
            return NULL;  // something that is not a module is already there
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

static LhatType *hosted_root(LhatProgram *program)
{
    if (program->hosted == NULL) {
        program->hosted = lhat_type_table(&program->types);
        if (program->hosted != NULL) {
            program->hosted->v.table.sealed = true;  // 05 の 8.6
        }
    }
    return program->hosted;
}

static const LhatTypeMember *hosted_member(const LhatType *table,
                                           const char *name)
{
    if (table == NULL || table->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    size_t length = strlen(name);
    for (const LhatTypeMember *m = table->v.table.members; m != NULL;
         m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m;
        }
    }
    return NULL;
}

static bool keep_entry(LhatProgram *program, const char *module,
                       const char *type, const char *name, LhatHostFn call,
                       void *context, const LhatType *signature)
{
    LHAT_GROW(program->host_entries, program->host_entry_count,
              program->host_entry_capacity, 8, return false);

    LhatHostEntry *entry = &program->host_entries[program->host_entry_count];
    memset(entry, 0, sizeof *entry);
    entry->module = duplicate(module);
    entry->name = duplicate(name);
    entry->type = type != NULL ? duplicate(type) : NULL;
    entry->call = call;
    entry->context = context;
    if (signature != NULL && signature->kind == LHAT_TYPE_FUNC) {
        size_t count = 0;
        for (const LhatTypeList *p = signature->v.func.params; p != NULL;
             p = p->next) {
            count++;
        }
        entry->parameters = (uint8_t)count;
        // 13.7: '...' is kept apart from the parameter list (check.c's
        // resolve_func_type), so what is counted above is the floor and this
        // is what says the count is one.
        entry->has_variadic = signature->v.func.variadic != NULL;
        entry->takes_self = signature->v.func.takes_self;
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL)) {
        return false;
    }
    program->host_entry_count++;
    return true;
}

const LhatHostDataTag *lhat_register_hostdata_type(LhatProgram *program,
                                                   const char *module,
                                                   const char *name)
{
    LhatType *root = hosted_root(program);
    if (root == NULL) {
        return NULL;
    }
    LhatType *table = hosted_table(program, root, module);
    if (table == NULL || hosted_member(table, name) != NULL) {
        return NULL;  // 8.7: one name, one thing
    }

    // 05 の 7.3's shape: what makes it its own type is the declaration, not
    // the members, so two host types that look alike stay apart. 02 の 8.8's
    // mark keeps a member from being added to it afterwards.
    LhatType *made = lhat_type_table(&program->types);
    if (made == NULL) {
        return NULL;
    }
    made->v.table.from_definition = true;
    made->v.table.nominal = true;
    if (lhat_type_add_member(&program->types, table, name, strlen(name),
                             made) == NULL ||
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL)) {
        return NULL;
    }

    // 8.8: the tag is identity and nothing else, so it is made here where
    // there is exactly one per registration.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];
    entry->tag = (LhatHostDataTag *)lhat_calloc(1, sizeof *entry->tag);
    if (entry->tag == NULL) {
        return NULL;
    }
    entry->tag->module = entry->module;
    entry->tag->name = entry->name;

    // 05 の 8.8 の isa^ 版: vm.c がコンパイル時に "module.Name" から
    // 引けるよう、host_entries(非公開)とは別に vm.h の形へ薄く複製する。
    // 文字列は entry->module/name(host_entries が所有)をそのまま指す
    // だけ -- ここでは複製しない。program_dispose も host_type_entries
    // 側の文字列は解放しない。
    if (program->host_type_entry_count == program->host_type_entry_capacity) {
        size_t grown = program->host_type_entry_capacity
                           ? program->host_type_entry_capacity * 2
                           : 4;
        LhatHostTypeEntry *bigger = (LhatHostTypeEntry *)lhat_realloc(
            program->host_type_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            return NULL;
        }
        program->host_type_entries = bigger;
        program->host_type_entry_capacity = grown;
    }
    LhatHostTypeEntry *type_entry =
        &program->host_type_entries[program->host_type_entry_count++];
    type_entry->module = entry->module;
    type_entry->name = entry->name;
    type_entry->tag = entry->tag;

    return entry->tag;
}

bool lhat_register_type(LhatProgram *program, const char *module,
                        const char *name)
{
    return lhat_register_hostdata_type(program, module, name) != NULL;
}

// lhat_register_error_kind の失敗経路が繰り返し要る後始末: variant_copies
// の先頭 filled_count 個(まだ何も書かれていない calloc 直後なら 0)と、
// 配列自体2つ。variants/variant_copies のどちらかが NULL でも(要素数0の
// 登録、あるいは calloc 失敗直後)、filled_count が 0 ならループが回らず
// 安全。
static void free_variant_arrays(char **variant_copies,
                                LhatErrorKind **variants, size_t filled_count)
{
    for (size_t i = 0; i < filled_count; i++) {
        lhat_free(variant_copies[i]);
    }
    lhat_free(variant_copies);
    lhat_free(variants);
}

// 04 の 12.4 の host 版: errordef^ を書かずに存在する誤り種別。型側は
// lhat_register_hostdata_type と同じ hosted_table/hosted_member を通る;
// 実行時側は check.c の check_errordef / vm.c の declare_error が unit の
// ASTから作るのと同じ2つの呼び出し(lhat_type_error_set/error_kind、
// lhat_error_kind_new)を、program->host_error_heap に対して行う。
bool lhat_register_error_kind(LhatProgram *program, const char *module,
                              const char *name,
                              const char *const *variant_names,
                              size_t variant_count,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants)
{
    if (program == NULL || module == NULL || name == NULL) {
        return false;
    }
    LhatType *root = hosted_root(program);
    LhatType *table = root != NULL ? hosted_table(program, root, module) : NULL;
    if (table == NULL || hosted_member(table, name) != NULL) {
        return false;  // 8.7: one name, one thing
    }

    LhatType *set = lhat_type_error_set(&program->types, name, strlen(name));
    if (set == NULL ||
        lhat_type_add_member(&program->types, table, name, strlen(name),
                             set) == NULL) {
        return false;
    }
    for (size_t i = 0; i < variant_count; i++) {
        if (lhat_type_error_kind(&program->types, set, variant_names[i],
                                 strlen(variant_names[i])) == NULL) {
            return false;
        }
    }

    // 04 の 2.3 と同じ形。途中で確保に失敗しても、既にヒープへ繋いだ分は
    // chunk の定数と同じ扱いで lhat_program_dispose がまとめて解放する --
    // 明示的に取り消さない。
    LhatString *group_name =
        lhat_string_new(&program->host_error_heap, name, strlen(name));
    LhatErrorKind *group =
        group_name != NULL
            ? lhat_error_kind_new(&program->host_error_heap, NULL, group_name)
            : NULL;
    if (group == NULL) {
        return false;
    }

    LhatErrorKind **variants = NULL;
    char **variant_copies = NULL;
    if (variant_count > 0) {
        variants =
            (LhatErrorKind **)lhat_calloc(variant_count, sizeof *variants);
        variant_copies =
            (char **)lhat_calloc(variant_count, sizeof *variant_copies);
        if (variants == NULL || variant_copies == NULL) {
            free_variant_arrays(variant_copies, variants, 0);
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
            // "IOError.NotFound" -- declare_error (vm.c) が typeof^ 用に
            // 作るのと同じ綴り。module は含まない。
            memcpy(qualified, name, name_length);
            qualified[name_length] = '.';
            memcpy(qualified + name_length + 1, variant_names[i],
                  variant_length);
            text = lhat_string_new(&program->host_error_heap, qualified, total);
        }
        LhatErrorKind *kind =
            text != NULL
                ? lhat_error_kind_new(&program->host_error_heap, group, text)
                : NULL;
        variant_copies[i] = kind != NULL ? duplicate(variant_names[i]) : NULL;
        if (kind == NULL || variant_copies[i] == NULL) {
            free_variant_arrays(variant_copies, variants, i + 1);
            return false;
        }
        variants[i] = kind;
    }

    if (program->host_error_entry_count == program->host_error_entry_capacity) {
        size_t grown = program->host_error_entry_capacity
                           ? program->host_error_entry_capacity * 2
                           : 4;
        LhatHostErrorKind *bigger = (LhatHostErrorKind *)lhat_realloc(
            program->host_error_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            free_variant_arrays(variant_copies, variants, variant_count);
            return false;
        }
        program->host_error_entries = bigger;
        program->host_error_entry_capacity = grown;
    }

    LhatHostErrorKind *entry =
        &program->host_error_entries[program->host_error_entry_count];
    entry->module = duplicate(module);
    entry->name = duplicate(name);
    entry->group = group;
    entry->variant_names = (const char *const *)variant_copies;
    entry->variants = (const LhatErrorKind *const *)variants;
    entry->variant_count = variant_count;
    if (entry->module == NULL || entry->name == NULL) {
        lhat_free((void *)entry->module);
        lhat_free((void *)entry->name);
        free_variant_arrays(variant_copies, variants, variant_count);
        return false;
    }
    program->host_error_entry_count++;

    if (out_group != NULL) {
        *out_group = group;
    }
    if (out_variants != NULL) {
        for (size_t i = 0; i < variant_count; i++) {
            out_variants[i] = variants[i];
        }
    }
    return true;
}

const LhatErrorKind *lhat_lookup_error_kind(const LhatProgram *program,
                                            const char *module,
                                            const char *name,
                                            const char *variant)
{
    if (program == NULL || module == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->host_error_entry_count; i++) {
        const LhatHostErrorKind *entry = &program->host_error_entries[i];
        if (strcmp(entry->module, module) != 0 ||
            strcmp(entry->name, name) != 0) {
            continue;
        }
        if (variant == NULL) {
            return entry->group;
        }
        for (size_t v = 0; v < entry->variant_count; v++) {
            if (strcmp(entry->variant_names[v], variant) == 0) {
                return entry->variants[v];
            }
        }
        return NULL;
    }
    return NULL;
}

static bool register_into(LhatProgram *program, LhatType *owner,
                          const char *module, const char *type,
                          const char *name, const char *signature,
                          LhatHostFn call, void *context)
{
    if (owner == NULL || call == NULL || hosted_member(owner, name) != NULL) {
        return false;
    }
    // 8.7: a signature may name the builtins and whatever was registered
    // before it. Nothing a require^ brings in -- that would put the answer
    // back at the mercy of the order units are checked in.
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted);
    if (written == NULL) {
        return false;
    }
    return lhat_type_add_member(&program->types, owner, name, strlen(name),
                                written) != NULL &&
           keep_entry(program, module, type, name, call, context, written);
}

bool lhat_register_member(LhatProgram *program, const char *module,
                          const char *type, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context)
{
    LhatType *table = hosted_table(program, hosted_root(program), module);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_TABLE) {
        return false;  // no such type registered under that module
    }
    if (!register_into(program, found->type, module, type, name, signature,
                       call, context)) {
        return false;
    }

    // 05 の 8.8: 02 の 12.5 reads a lifetime off whether the type carries a
    // dispose^, so registering one is what makes the value the host's to
    // take back. The tag carries it, since that is what a value still has
    // when a collection reaches it.
    if (strcmp(name, "dispose") == 0) {
        for (size_t i = 0; i < program->host_entry_count; i++) {
            LhatHostEntry *entry = &program->host_entries[i];
            if (entry->tag != NULL && entry->type == NULL &&
                strcmp(entry->module, module) == 0 &&
                strcmp(entry->name, type) == 0) {
                entry->tag->release = call;
                entry->tag->release_context = context;
                break;
            }
        }
    }
    return true;
}

bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context)
{
    return register_into(program, hosted_table(program, hosted_root(program),
                                               module),
                         module, NULL, name, signature, call, context);
}

// 05 の 8.9: how many payload bytes one field kind occupies, which is what
// registration checks offsets against.
static size_t hostvalue_field_bytes(LhatHostValueFieldKind kind)
{
    switch (kind) {
        case LHAT_HVFIELD_F32: return 4;
        case LHAT_HVFIELD_F64: return 8;
        case LHAT_HVFIELD_I8:  return 1;
        case LHAT_HVFIELD_I16: return 2;
        case LHAT_HVFIELD_I32: return 4;
        case LHAT_HVFIELD_I64: return 8;
        case LHAT_HVFIELD_U8:  return 1;
        case LHAT_HVFIELD_U16: return 2;
        case LHAT_HVFIELD_U32: return 4;
    }
    return 0;
}

const LhatHostValueTag *lhat_register_hostvalue_type(LhatProgram *program,
                                                     const char *module,
                                                     const char *name,
                                                     size_t size)
{
    if (program == NULL || module == NULL || name == NULL || size == 0 ||
        size > LHAT_HOSTVALUE_MAX_BYTES) {
        return NULL;
    }
    LhatType *root = hosted_root(program);
    if (root == NULL) {
        return NULL;
    }
    LhatType *table = hosted_table(program, root, module);
    if (table == NULL || hosted_member(table, name) != NULL) {
        return NULL;  // 8.7: one name, one thing
    }

    // Grown before anything below can half-succeed, so a refusal here leaves
    // the program exactly as it was.
    if (program->hostvalue_type_entry_count ==
        program->hostvalue_type_entry_capacity) {
        size_t grown_count = program->hostvalue_type_entry_capacity
                                 ? program->hostvalue_type_entry_capacity * 2
                                 : 4;
        LhatHostValueTypeEntry *bigger = (LhatHostValueTypeEntry *)lhat_realloc(
            program->hostvalue_type_entries, grown_count * sizeof *bigger);
        if (bigger == NULL) {
            return NULL;
        }
        program->hostvalue_type_entries = bigger;
        program->hostvalue_type_entry_capacity = grown_count;
    }

    // 8.9: the tag is identity, made here where there is exactly one per
    // registration. Unlike a hostdata tag it is owned by
    // hostvalue_type_entries rather than by a host entry, since the entry's
    // tag field is the hostdata-shaped one.
    LhatHostValueTag *tag = (LhatHostValueTag *)lhat_calloc(1, sizeof *tag);
    if (tag == NULL) {
        return NULL;
    }
    tag->size = size;
    tag->width = 1 + (size + 7) / 8;  // one head slot, then the bytes
    tag->index = program->hostvalue_type_entry_count;

    LhatType *made = lhat_type_hostvalue(&program->types, tag);
    if (made == NULL ||
        lhat_type_add_member(&program->types, table, name, strlen(name),
                             made) == NULL ||
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL)) {
        lhat_free(tag);
        return NULL;
    }
    // keep_entry has install make an empty table under the type's name in
    // L^.modules, exactly as for a hostdata type -- that table is where the
    // registered members land, and what the machine reads back as the
    // type's members table.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];
    tag->module = entry->module;
    tag->name = entry->name;

    LhatHostValueTypeEntry *type_entry =
        &program->hostvalue_type_entries[program->hostvalue_type_entry_count++];
    type_entry->module = entry->module;
    type_entry->name = entry->name;
    type_entry->tag = tag;

    return tag;
}

bool lhat_register_hostvalue_member(LhatProgram *program, const char *module,
                                    const char *type, const char *name,
                                    const char *signature, LhatHostFn call,
                                    void *context)
{
    LhatType *table = hosted_table(program, hosted_root(program), module);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_HOSTVALUE) {
        return false;  // no such value type registered under that module
    }
    // No dispose special case here: a host value has no lifetime to hand
    // over, which is half of what makes it one.
    return register_into(program, found->type, module, type, name, signature,
                         call, context);
}

bool lhat_register_hostvalue_field(LhatProgram *program, const char *module,
                                   const char *type, const char *field,
                                   size_t offset, LhatHostValueFieldKind kind)
{
    if (program == NULL || module == NULL || type == NULL || field == NULL) {
        return false;
    }
    // The tag comes from the registry rather than through the checker type,
    // so the const on the public pointer stays honest -- the program owns
    // what it hands out and may still write to it here.
    LhatHostValueTag *tag = NULL;
    for (size_t i = 0; i < program->hostvalue_type_entry_count; i++) {
        const LhatHostValueTypeEntry *entry =
            &program->hostvalue_type_entries[i];
        if (strcmp(entry->module, module) == 0 &&
            strcmp(entry->name, type) == 0) {
            tag = (LhatHostValueTag *)entry->tag;
            break;
        }
    }
    size_t bytes = hostvalue_field_bytes(kind);
    if (tag == NULL || bytes == 0 || offset > tag->size ||
        tag->size - offset < bytes) {
        return false;
    }

    // The checker side has to exist and not already answer the name --
    // neither as a field nor as a registered member.
    LhatType *table = hosted_table(program, hosted_root(program), module);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_HOSTVALUE ||
        hosted_member(found->type, field) != NULL) {
        return false;
    }

    char *copy = duplicate(field);
    LhatHostValueField *grown = (LhatHostValueField *)lhat_realloc(
        tag->fields, (tag->field_count + 1) * sizeof *grown);
    if (grown != NULL) {
        tag->fields = grown;
    }
    LhatType *number = copy != NULL && grown != NULL
                           ? lhat_type_simple(&program->types, LHAT_TYPE_NUMBER)
                           : NULL;
    if (number == NULL ||
        lhat_type_add_member(&program->types, found->type, copy, strlen(copy),
                             number) == NULL) {
        lhat_free(copy);
        return false;
    }

    LhatHostValueField *made = &tag->fields[tag->field_count++];
    made->name = copy;
    made->offset = offset;
    made->kind = kind;
    return true;
}

// 05 の 8.6: L^ itself. Kept apart from the entries above because those land
// under L^.modules and this one does not -- and because the checker reads it
// from a different place, its own L^ rather than the import^ registry.
bool lhat_register_global(LhatProgram *program, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context)
{
    if (program == NULL || name == NULL || call == NULL) {
        return false;
    }
    if (program->globals == NULL) {
        program->globals = lhat_type_table(&program->types);
        if (program->globals == NULL) {
            return false;
        }
    }
    if (hosted_member(program->globals, name) != NULL) {
        return false;  // 8.7: one name, one thing
    }
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted);
    if (written == NULL ||
        lhat_type_add_member(&program->types, program->globals, name,
                             strlen(name), written) == NULL) {
        return false;
    }

    LHAT_GROW(program->global_entries, program->global_count,
              program->global_capacity, 4, return false);
    LhatGlobalEntry *entry = &program->global_entries[program->global_count];
    memset(entry, 0, sizeof *entry);
    entry->name = duplicate(name);
    entry->call = call;
    entry->context = context;
    if (written->kind == LHAT_TYPE_FUNC) {
        size_t count = 0;
        for (const LhatTypeList *p = written->v.func.params; p != NULL;
             p = p->next) {
            count++;
        }
        entry->parameters = (uint8_t)count;
        entry->has_variadic = written->v.func.variadic != NULL;
        entry->takes_self = written->v.func.takes_self;
    }
    if (entry->name == NULL) {
        return false;
    }
    program->global_count++;
    return true;
}

// 05 の 8.2. `member` is written "L^.<name>", which is the only form for now:
// 8.6's table is where a host puts what it wants seen without a require^.
bool lhat_bind_initial(LhatProgram *program, const char *name,
                       const char *member)
{
    static const char prefix[] = "L^.";
    if (program == NULL || name == NULL || member == NULL ||
        strncmp(member, prefix, sizeof prefix - 1) != 0) {
        return false;
    }
    const char *reached = member + sizeof prefix - 1;
    if (*reached == '\0') {
        return false;
    }

    if (program->initial_count == program->initial_capacity) {
        size_t grown =
            program->initial_capacity ? program->initial_capacity * 2 : 4;
        char **names = (char **)lhat_realloc(program->initial_names,
                                             grown * sizeof *names);
        if (names == NULL) {
            return false;
        }
        program->initial_names = names;
        char **members = (char **)lhat_realloc(program->initial_members,
                                               grown * sizeof *members);
        if (members == NULL) {
            return false;
        }
        program->initial_members = members;
        program->initial_capacity = grown;
    }
    program->initial_names[program->initial_count] = duplicate(name);
    program->initial_members[program->initial_count] = duplicate(reached);
    if (program->initial_names[program->initial_count] == NULL ||
        program->initial_members[program->initial_count] == NULL) {
        return false;
    }
    program->initial_count++;
    return true;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

const LhatModule *lhat_program_compile(LhatProgram *program, size_t *count)
{
    if (count != NULL) {
        *count = program->module_count;
    }
    if (program->modules != NULL) {
        return program->modules;
    }

    size_t total = 0;
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        u->index = total++;
    }
    if (total == 0) {
        return NULL;
    }

    LhatModule *modules = (LhatModule *)lhat_calloc(total, sizeof *modules);
    if (modules == NULL) {
        return NULL;
    }

    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (!u->loaded || u->state != LHAT_UNIT_DONE) {
            continue;  // a unit that failed to check has nothing to compile
        }
        Resolution resolution;
        resolution.program = program;
        resolution.requiring = u;

        LhatUnits units;
        units.resolve = resolve_unit;
        units.context = &resolution;
        units.module_name = u->checked.module_name;
        units.initial_names = (const char *const *)program->initial_names;
        units.initial_members = (const char *const *)program->initial_members;
        units.initial_count = program->initial_count;  // 05 の 8.2
        units.host_errors = program->host_error_entries;  // 05 の 8.7 の誤り版
        units.host_error_count = program->host_error_entry_count;
        units.host_types = program->host_type_entries;  // 05 の 8.8 の isa^ 版
        units.host_type_count = program->host_type_entry_count;
        units.hostvalue_types = program->hostvalue_type_entries;  // 05 の 8.9
        units.hostvalue_type_count = program->hostvalue_type_entry_count;

        LhatProto *proto = NULL;
        LhatCompileStatus status =
            lhat_compile_module(u->parsed.root, &u->lexer, &units, &proto);
        if (status != LHAT_COMPILE_OK) {
            // Kept so the caller can say which form stopped it rather than
            // only that something did.
            program->compile_status = status;
            lhat_modules_free(modules, total);
            lhat_free(modules);
            return NULL;
        }
        modules[u->index].proto = proto;
        if (u->checked.module_name != NULL) {
            modules[u->index].module_name = duplicate(u->checked.module_name);
        }
    }

    program->modules = modules;
    program->module_count = total;
    if (count != NULL) {
        *count = total;
    }
    return modules;
}

bool lhat_program_install(const LhatProgram *program, LhatMachine *machine)
{
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *e = &program->host_entries[i];
        // A type registers as an empty table under its module; its members
        // are entries of their own and land in it as they come.
        LhatValue value = lhat_nil();
        if (e->call == NULL) {
            if (!lhat_machine_make_table(machine, &value)) {
                return false;
            }
        } else if (!lhat_machine_make_host(machine, e->call, e->context,
                                           e->parameters, e->has_variadic,
                                           e->takes_self, &value)) {
            return false;
        }
        if (!lhat_machine_register(machine, e->module, e->type, e->name,
                                   value)) {
            return false;
        }
    }
    // 05 の 8.6: what goes in L^ itself rather than under its registry.
    for (size_t i = 0; i < program->global_count; i++) {
        const LhatGlobalEntry *e = &program->global_entries[i];
        LhatValue value = lhat_nil();
        if (!lhat_machine_make_host(machine, e->call, e->context,
                                    e->parameters, e->has_variadic,
                                    e->takes_self, &value) ||
            !lhat_machine_set_global(machine, e->name, value)) {
            return false;
        }
    }
    // 05 の 8.9: a host value has no heap half to carry its members table,
    // so the machine keeps one per registered type, found by the tag's
    // index. The tables themselves are the ones the entry loop above put
    // under L^.modules -- reachable from the environment, so the collector
    // needs nothing new.
    if (program->hostvalue_type_entry_count > 0 &&
        !lhat_machine_bind_hostvalues(machine,
                                      program->hostvalue_type_entries,
                                      program->hostvalue_type_entry_count)) {
        return false;
    }
    return true;
}

void lhat_program_install_checks(const LhatProgram *program,
                                 LhatCheckSession *session)
{
    if (program == NULL) {
        return;
    }
    lhat_check_session_hosted(session, program->hosted, program->globals);
    // 8.2's arrays are the program's and outlive the session with it, which is
    // exactly what lhat_check_session_bind asks of a caller.
    lhat_check_session_bind(session, (const char *const *)program->initial_names,
                            (const char *const *)program->initial_members,
                            program->initial_count);
}

void lhat_program_install_compiles(const LhatProgram *program,
                                   LhatCompileSession *session)
{
    if (program == NULL) {
        return;
    }
    lhat_compile_session_hosted(session, program->host_error_entries,
                                program->host_error_entry_count,
                                program->host_type_entries,
                                program->host_type_entry_count);
    lhat_compile_session_bind(session,
                              (const char *const *)program->initial_names,
                              (const char *const *)program->initial_members,
                              program->initial_count);
}

void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context)
{
    memset(program, 0, sizeof *program);
    lhat_type_arena_init(&program->types);
    program->strict = strict;
    program->load = load;
    program->loader_context = context;
    // See lhat_proto_new's comment: born black so a machine reading a
    // host-registered error kind never writes into program->host_error_heap
    // -- required once more than one machine (std.thread) can read it.
    program->host_error_heap.white = LHAT_GC_BLACK;
}

void lhat_program_dispose(LhatProgram *program)
{
    lhat_modules_free(program->modules, program->module_count);
    lhat_free(program->modules);
    program->modules = NULL;
    program->module_count = 0;

    for (size_t i = 0; i < program->host_entry_count; i++) {
        lhat_free(program->host_entries[i].module);
        lhat_free(program->host_entries[i].type);
        lhat_free(program->host_entries[i].name);
        lhat_free(program->host_entries[i].tag);
    }
    lhat_free(program->host_entries);
    program->host_entries = NULL;
    program->host_entry_count = 0;
    program->host_entry_capacity = 0;

    // host_type_entries[i] の module/name/tag は上の host_entries[i] が
    // 所有する同じポインタを指すだけなので、ここでは配列自体だけを解放
    // する(要素の中身は既に上で解放済み)。
    lhat_free(program->host_type_entries);
    program->host_type_entries = NULL;
    program->host_type_entry_count = 0;
    program->host_type_entry_capacity = 0;

    // 05 の 8.9: unlike the two registries above, this one owns its tags and
    // their field arrays (the strings are host_entries', freed above).
    for (size_t i = 0; i < program->hostvalue_type_entry_count; i++) {
        LhatHostValueTag *tag =
            (LhatHostValueTag *)program->hostvalue_type_entries[i].tag;
        for (size_t f = 0; f < tag->field_count; f++) {
            lhat_free((void *)tag->fields[f].name);
        }
        lhat_free(tag->fields);
        lhat_free(tag);
    }
    lhat_free(program->hostvalue_type_entries);
    program->hostvalue_type_entries = NULL;
    program->hostvalue_type_entry_count = 0;
    program->hostvalue_type_entry_capacity = 0;

    for (size_t i = 0; i < program->host_error_entry_count; i++) {
        LhatHostErrorKind *entry = &program->host_error_entries[i];
        lhat_free((void *)entry->module);
        lhat_free((void *)entry->name);
        for (size_t v = 0; v < entry->variant_count; v++) {
            lhat_free((void *)entry->variant_names[v]);
        }
        lhat_free((void *)entry->variant_names);
        lhat_free((void *)entry->variants);
    }
    lhat_free(program->host_error_entries);
    program->host_error_entries = NULL;
    program->host_error_entry_count = 0;
    program->host_error_entry_capacity = 0;
    // 04 の 12.4: この program が登録した誤り種別のオブジェクト自身
    // (LhatErrorKind/LhatString)。chunk->heap と同じ扱いで、program の
    // 寿命が尽きるここでまとめて解放する。
    lhat_object_free_all(&program->host_error_heap);

    for (size_t i = 0; i < program->global_count; i++) {
        lhat_free(program->global_entries[i].name);
    }
    lhat_free(program->global_entries);
    program->global_entries = NULL;
    program->global_count = 0;
    program->global_capacity = 0;

    for (size_t i = 0; i < program->initial_count; i++) {
        lhat_free(program->initial_names[i]);
        lhat_free(program->initial_members[i]);
    }
    lhat_free(program->initial_names);
    lhat_free(program->initial_members);
    program->initial_names = NULL;
    program->initial_members = NULL;
    program->initial_count = 0;
    program->initial_capacity = 0;

    LhatUnit *unit = program->units;
    while (unit != NULL) {
        LhatUnit *next = unit->next;
        if (unit->loaded) {
            lhat_check_result_dispose(&unit->checked);
            lhat_parse_result_dispose(&unit->parsed);
            lhat_lexer_dispose(&unit->lexer);
            lhat_source_dispose(&unit->source);
        }
        lhat_free(unit->path);
        lhat_free(unit);
        unit = next;
    }
    program->units = NULL;

    for (size_t i = 0; i < program->diagnostic_count; i++) {
        lhat_free(program->diagnostics[i].path);
    }
    lhat_free(program->diagnostics);
    program->diagnostics = NULL;
    program->diagnostic_count = 0;
    program->diagnostic_capacity = 0;

    lhat_type_arena_dispose(&program->types);
}

// The opaque forms a host uses (program.h): the by-value pair above wrapped
// around an allocation.
LhatProgram *lhat_program_new(bool strict, LhatProgramLoader load,
                              void *context)
{
    LhatProgram *program = (LhatProgram *)lhat_alloc(sizeof *program);
    if (program != NULL) {
        lhat_program_init(program, strict, load, context);
    }
    return program;
}

void lhat_program_free(LhatProgram *program)
{
    if (program != NULL) {
        lhat_program_dispose(program);
        lhat_free(program);
    }
}

size_t lhat_unit_index(const LhatUnit *unit)
{
    return unit->index;
}

const char *lhat_unit_path(const LhatUnit *unit)
{
    return unit->path;
}

LhatUnitState lhat_unit_state(const LhatUnit *unit)
{
    return unit->state;
}

bool lhat_unit_ok(const LhatUnit *unit)
{
    return unit != NULL && unit->loaded && unit->state == LHAT_UNIT_DONE &&
           unit->lexer.diagnostic_count == 0 &&
           unit->parsed.diagnostic_count == 0 &&
           unit->checked.diagnostic_count == 0;
}

size_t lhat_program_diagnostic_count(const LhatProgram *program)
{
    return program->diagnostic_count;
}

const LhatProgramDiagnostic *lhat_program_diagnostic(const LhatProgram *program,
                                                     size_t index)
{
    return index < program->diagnostic_count ? &program->diagnostics[index]
                                             : NULL;
}

const LhatUnit *lhat_program_check(LhatProgram *program, const char *path)
{
    char *resolved = normalise_path(path);
    if (resolved == NULL) {
        return NULL;
    }
    LhatUnit *unit = check_path(program, resolved);
    return (unit != NULL && unit->loaded) ? unit : NULL;
}

bool lhat_program_has_errors(const LhatProgram *program)
{
    if (program->diagnostic_count > 0) {
        return true;
    }
    for (const LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (!u->loaded) {
            return true;
        }
        if (u->lexer.diagnostic_count > 0 || u->parsed.diagnostic_count > 0 ||
            u->checked.diagnostic_count > 0) {
            return true;
        }
    }
    return false;
}

const char *lhat_program_error_message(LhatProgramErrorCode code)
{
    switch (code) {
        case LHAT_PROGRAM_ERR_CANNOT_READ:
            return "this unit could not be read";
        case LHAT_PROGRAM_ERR_CYCLE:
            return "the units require each other; move the common part out";
    }
    return "unknown error";
}
