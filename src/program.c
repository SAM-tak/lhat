// L^ (lhat) -- a program: one unit and everything it requires.

#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char *out = (char *)malloc(length + 1);
    if (out == NULL) {
        return NULL;
    }

    // Segment starts within `out`, so that '..' can rewind to the previous
    // one rather than being resolved textually against the whole string.
    size_t *starts = (size_t *)malloc((length + 2) * sizeof *starts);
    if (starts == NULL) {
        free(out);
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
    free(starts);
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

    char *joined = (char *)malloc(base_length + relative_length + 1);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined, base, base_length);
    memcpy(joined + base_length, relative, relative_length);
    joined[base_length + relative_length] = '\0';

    char *resolved = normalise_path(joined);
    free(joined);
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
        LhatProgramDiagnostic *bigger = (LhatProgramDiagnostic *)realloc(
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

// The raw bytes. Normalising the newlines and the BOM belongs to the source
// (01 の 1 章), which every loader's result goes through.
static char *read_file(void *context, const char *path, size_t *length)
{
    (void)context;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    char *buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    buffer[read] = '\0';
    *length = read;
    return buffer;
}

static char *duplicate(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
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
            free(path);
            return NULL;
        }
        // 5.3: loaded once. A second require^ gets the same unit.
        free(path);
        return existing;
    }

    LhatUnit *unit = (LhatUnit *)calloc(1, sizeof *unit);
    if (unit == NULL) {
        free(path);
        return NULL;
    }
    unit->path = path;
    unit->state = LHAT_UNIT_CHECKING;
    unit->next = program->units;
    program->units = unit;

    size_t length = 0;
    char *text = program->load(program->loader_context, unit->path, &length);
    if (text == NULL) {
        report(program, LHAT_PROGRAM_ERR_CANNOT_READ, unit->path);
        unit->state = LHAT_UNIT_FAILED;
        return unit;
    }

    lhat_source_init_from_string(&unit->source, unit->path, text, length);
    free(text);
    lhat_lexer_init(&unit->lexer, &unit->source);
    lhat_parse(&unit->lexer, &unit->parsed);
    unit->loaded = true;

    Resolution resolution;
    resolution.program = program;
    resolution.requiring = unit;

    LhatRequire require;
    require.resolve = resolve_require;
    require.context = &resolution;

    // The recursion is what puts the graph in dependency order (6.2): the
    // required unit finishes before this one's checking gets past the
    // require^ that asked for it.
    lhat_check_unit(unit->parsed.root, &unit->lexer, program->strict,
                    &program->types, &require, &unit->checked);

    unit->state = LHAT_UNIT_DONE;
    return unit;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void lhat_program_init(LhatProgram *program, bool strict)
{
    memset(program, 0, sizeof *program);
    lhat_type_arena_init(&program->types);
    program->strict = strict;
    program->load = read_file;
}

void lhat_program_set_loader(LhatProgram *program, LhatProgramLoader load,
                             void *context)
{
    program->load = load;
    program->loader_context = context;
}

void lhat_program_dispose(LhatProgram *program)
{
    LhatUnit *unit = program->units;
    while (unit != NULL) {
        LhatUnit *next = unit->next;
        if (unit->loaded) {
            lhat_check_result_dispose(&unit->checked);
            lhat_parse_result_dispose(&unit->parsed);
            lhat_lexer_dispose(&unit->lexer);
            lhat_source_dispose(&unit->source);
        }
        free(unit->path);
        free(unit);
        unit = next;
    }
    program->units = NULL;

    for (size_t i = 0; i < program->diagnostic_count; i++) {
        free(program->diagnostics[i].path);
    }
    free(program->diagnostics);
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
