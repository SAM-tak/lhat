// L^ (lhat) -- a program: one unit and everything it requires.

#include "program.h"

#include <stdlib.h>
#include <string.h>

#include "port.h"

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
    if (program->diagnostic_count == program->diagnostic_capacity) {
        size_t grown =
            program->diagnostic_capacity ? program->diagnostic_capacity * 2 : 4;
        LhatProgramDiagnostic *bigger = (LhatProgramDiagnostic *)lhat_realloc(
            program->diagnostics, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        program->diagnostics = bigger;
        program->diagnostic_capacity = grown;
    }

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
            // 05 の 8.6改: what the host registered is the machine's record of
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
            program->hosted->v.table.sealed = true;  // 05 の 8.6改
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
    if (program->host_entry_count == program->host_entry_capacity) {
        size_t grown =
            program->host_entry_capacity ? program->host_entry_capacity * 2 : 8;
        LhatHostEntry *bigger = (LhatHostEntry *)lhat_realloc(
            program->host_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        program->host_entries = bigger;
        program->host_entry_capacity = grown;
    }

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
    return entry->tag;
}

bool lhat_register_type(LhatProgram *program, const char *module,
                        const char *name)
{
    return lhat_register_hostdata_type(program, module, name) != NULL;
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

    if (program->global_count == program->global_capacity) {
        size_t grown =
            program->global_capacity ? program->global_capacity * 2 : 4;
        LhatGlobalEntry *bigger = (LhatGlobalEntry *)lhat_realloc(
            program->global_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        program->global_entries = bigger;
        program->global_capacity = grown;
    }
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
                                           e->parameters, e->takes_self,
                                           &value)) {
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
                                    e->parameters, e->takes_self, &value) ||
            !lhat_machine_set_global(machine, e->name, value)) {
            return false;
        }
    }
    return true;
}

void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context)
{
    memset(program, 0, sizeof *program);
    lhat_type_arena_init(&program->types);
    program->strict = strict;
    program->load = load;
    program->loader_context = context;
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
