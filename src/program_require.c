// L^ (lhat) -- a program: scripts loaded and reloaded at run time.

#include "message.h"
#include "program_internal.h"
#include "serialize.h"
#include "registry.h"

#include <stdlib.h>
#include <string.h>

#include "compile.h"  // [modules.require.once]: the units are compiled here too
#include "lhat/port.h"
#include "type.h"
#include "lhat/vm.h"

// ---------------------------------------------------------------------------
// [modules.require.load]: loading a script at run time
// ---------------------------------------------------------------------------

// Everything the four stages made, leaving the shell -- its path, its module
// name and its place in the list -- standing. [modules.require.reload] invalidates a unit
// with this; unit_dispose_contents below finishes the job for a unit that is
// going altogether.
//
// The proto is not touched: an invalidation retires it and a disposal frees
// it, and only the caller knows which this is.
static void unit_clear_stages(LhatUnit *unit)
{
    if (unit->loaded && unit->binary) {
        unit->loaded = false;
        unit->binary = false;
    }
    if (unit->loaded) {
#if LHAT_WITH_FRONTEND
        lhat_check_result_dispose(&unit->checked);
        lhat_parse_result_dispose(&unit->parsed);
        lhat_lexer_dispose(&unit->lexer);
#endif
        lhat_source_dispose(&unit->source);
        memset(&unit->checked, 0, sizeof unit->checked);
        memset(&unit->parsed, 0, sizeof unit->parsed);
        memset(&unit->lexer, 0, sizeof unit->lexer);
        memset(&unit->source, 0, sizeof unit->source);
        unit->loaded = false;
    }
    for (size_t i = 0; unit->export_names != NULL && i < unit->export_rt_count;
         i++) {
        lhat_free(unit->export_names[i]);
    }
    lhat_free(unit->export_names);
    unit->export_names = NULL;
    lhat_free(unit->export_rt);
    unit->export_rt = NULL;
    unit->export_rt_count = 0;
    lhat_reflection_free(unit->reflection);
    unit->reflection = NULL;
    lhat_free(unit->referenced);
    unit->referenced = NULL;
    unit->referenced_count = 0;
    unit->referenced_capacity = 0;
}

static void unit_dispose_contents(LhatUnit *unit)
{
    unit_clear_stages(unit);
    lhat_proto_free(unit->proto);
    unit->proto = NULL;
    lhat_free(unit->module_name);
    lhat_free(unit->path);
}

// A growing text, for the failure a load answers with.
typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} Said;

static void say(Said *s, const char *text, size_t length)
{
    if (s->length + length + 1 > s->capacity) {
        size_t wanted = (s->length + length + 1) * 2;
        char *bigger = (char *)lhat_realloc(s->text, wanted);
        if (bigger == NULL) {
            return;
        }
        s->text = bigger;
        s->capacity = wanted;
    }
    memcpy(s->text + s->length, text, length);
    s->length += length;
    s->text[s->length] = '\0';
}

// [l10n.compose.holes]: a failure a load says, about the unit it names.
const LhatMessageEntry prg_program_parts[] = {
    {"program.failed", "{path}: error: {message}"},
};

static void say_failed(Said *s, const char *path, const char *message)
{
    const LhatMessageArg args[] = {
        {"path", path, strlen(path)},
        {"message", message, strlen(message)},
    };
    const char *text = prg_program_parts[0].text;
    size_t needed = lhat_message_render(text, args, 2, NULL, 0);
    char *line = (char *)lhat_alloc(needed + 1);
    if (line == NULL) {
        return;
    }
    lhat_message_render(text, args, 2, line, needed + 1);
    say(s, line, needed);
    lhat_free(line);
}

static void say_unit(Said *s, const LhatUnit *unit)
{
    size_t count = lhat_unit_diagnostic_count(unit);
    for (size_t i = 0; i < count; i++) {
        size_t needed = lhat_unit_diagnostic_write(unit, i, false, NULL, 0);
        char *line = (char *)lhat_alloc(needed + 1);
        if (line == NULL) {
            return;
        }
        lhat_unit_diagnostic_write(unit, i, false, line, needed + 1);
        if (s->length > 0) {
            say(s, "\n", 1);
        }
        say(s, line, needed);
        lhat_free(line);
    }
}

// The program's own diagnostics a load added from `from` on (a unit that
// could not be read, a cycle, bytes another build wrote), as the text a
// caller reads back.
static void say_program(Said *s, const LhatProgram *program, size_t from)
{
    for (size_t i = from; i < program->diagnostic_count; i++) {
        const LhatProgramDiagnostic *d = &program->diagnostics[i];
        if (s->length > 0) {
            say(s, "\n", 1);
        }
        say_failed(s, d->path, lhat_program_error_message(d->code));
    }
}

// What a build of a placed unit leaves behind: the failure text on the
// program -- a compile that stopped names its reason when nothing else was
// said -- and the status a load answers. The unit keeps its proto on OK.
static LhatLoadStatus settle_placed(LhatProgram *program,
                                    const LhatUnit *unit, Said *said,
                                    bool built)
{
    if (!built && said->length == 0) {
        say_failed(said, unit->path,
                   lhat_compile_status_message(program->compile_status));
    }
    program->load_failure = said->text;
    if (built) {
        return LHAT_LOAD_OK;
    }
    // No text at all means the failure itself did not fit.
    return said->text != NULL ? LHAT_LOAD_REJECTED : LHAT_LOAD_OUT_OF_MEMORY;
}

// Takes the unit's source as already placed. What it requires joins the
// program as any unit does (checked into the program's arena, compiled by
// lhat_program_compile, registered when it first runs); the unit itself is
// checked into an arena of its own, compiled without [l10n.compose.no-plurals]'s guard and
// registry write, and left holding its proto for the caller to take.
#if LHAT_WITH_FRONTEND
static LhatLoadStatus build_placed(LhatProgram *program, LhatUnit *unit)
{
    LhatUnit *before = program->units;
    size_t reported_before = program->diagnostic_count;
    prg_check_parsed(program, unit, NULL);

    Said said;
    memset(&said, 0, sizeof said);
    // The program's own diagnostics, then what the stages said of the unit
    // and of every unit the load reached.
    say_program(&said, program, reported_before);
    say_unit(&said, unit);
    for (const LhatUnit *u = program->units; u != before; u = u->next) {
        say_unit(&said, u);
    }
    bool built = said.length == 0 && prg_compile_all(program) &&
                 prg_compile_one(program, unit, false) && prg_fill_unit_table(unit);
    return settle_placed(program, unit, &said, built);
}
#else
static LhatLoadStatus build_placed(LhatProgram *program, LhatUnit *unit)
{
    // [modules.binary.vm-only]: text, and nothing here reads text.
    (void)unit;
    program->load_failure =
        prg_duplicate("this build has no front end; only a binary unit runs");
    return LHAT_LOAD_REJECTED;
}
#endif  // LHAT_WITH_FRONTEND

// [modules.binary] with [lton.load.compiled]: the bytes a full build wrote for a script or
// an LTON file (lhat_program_write_text), read the way load_into reads a
// unit. A script is not one of the program's units, so what the bytes carry
// beyond the body -- exports, what a host asks of the declarations -- has
// nobody to answer to and is let go; what it requires joins the program as
// it would from text.
static LhatLoadStatus load_binary_placed(LhatProgram *program,
                                         LhatUnit *unit, const uint8_t *bytes,
                                         size_t length)
{
    size_t reported_before = program->diagnostic_count;
    LhatBinaryUnit read;
    unit->state = LHAT_UNIT_CHECKING;
    bool built = lhat_serialize_load(program, unit, bytes, length, &read);
    if (built) {
        unit->proto = read.proto;
        prg_stamp_source(read.proto, unit->path);
        lhat_free(read.module_name);
        for (size_t i = 0; i < read.export_count; i++) {
            lhat_free(read.export_names[i]);
        }
        lhat_free(read.export_names);
        lhat_free(read.export_rt);
        lhat_reflection_free(read.reflection);
        built = prg_compile_all(program) && prg_fill_unit_table(unit);
    }
    Said said;
    memset(&said, 0, sizeof said);
    say_program(&said, program, reported_before);
    return settle_placed(program, unit, &said, built);
}

// A unit of the program's making for what a caller holds: named, as data
// or not, and nothing else yet.
static LhatUnit *place(LhatProgram *program, const char *name,
                       const LhatLoadOptions *options)
{
    // [modules.hostdata.base]: the other way checking begins ([modules.require.load]). check_path's own call
    // does not cover this one -- a script that requires nothing never reaches
    // it -- and the pass does its work once whichever arrives first.
    prg_flatten_hostdata_bases(program);
    lhat_free(program->load_failure);
    program->load_failure = NULL;
    LhatUnit *unit = (LhatUnit *)lhat_calloc(1, sizeof *unit);
    if (unit == NULL) {
        return NULL;
    }
    unit->path = prg_resolve_against(program, NULL, name, strlen(name));
    if (unit->path == NULL) {
        lhat_free(unit);
        return NULL;
    }
    unit->program = program;
    // [modules.host.initial]: what prg_check_parsed reads to decide whether the host's initial
    // bindings are in scope. Set before anything is checked, since that is
    // the one moment it is asked.
    unit->as_data = options != NULL && !options->initial_bindings;
    unit->lton = options != NULL && options->lton;
    return unit;
}

static void forget(LhatUnit *unit)
{
    unit_dispose_contents(unit);
    lhat_free(unit);
}

char *lhat_program_read(LhatProgram *program, const char *path,
                        size_t *length)
{
    if (program == NULL || path == NULL || length == NULL ||
        program->load == NULL) {
        return NULL;
    }
    *length = 0;
    char *resolved = prg_resolve_against(program, NULL, path, strlen(path));
    if (resolved == NULL) return NULL;
    char *text = program->load(program->loader_context, resolved, length);
    lhat_free(resolved);
    return text;
}

LhatLoadStatus prg_load_text_held(LhatProgram *program, const char *name,
                                     const char *text, size_t length,
                                     const LhatLoadOptions *options,
                                     LhatProto **out)
{
    *out = NULL;
    LhatUnit *unit = place(program, name, options);
    if (unit == NULL) {
        return LHAT_LOAD_OUT_OF_MEMORY;
    }
    // [modules.binary.magic]: the first byte says which the caller holds.
    LhatLoadStatus status;
    if (lhat_serialize_is_binary(text, length)) {
        status = load_binary_placed(program, unit, (const uint8_t *)text,
                                    length);
    } else {
        lhat_source_init_from_string(&unit->source, unit->path, text, length);
        status = build_placed(program, unit);
    }
    if (status == LHAT_LOAD_OK) {
        *out = unit->proto;
        unit->proto = NULL;
    }
    forget(unit);
    return status;
}

LhatLoadStatus lhat_program_load_text_with(LhatProgram *program,
                                           const char *name, const char *text,
                                           size_t length,
                                           const LhatLoadOptions *options,
                                           LhatProto **out)
{
    lhat_program_hold(program);
    LhatLoadStatus status =
        prg_load_text_held(program, name, text, length, options, out);
    lhat_program_release(program);
    return status;
}

LhatLoadStatus lhat_program_write_text(LhatProgram *program, const char *name,
                                       const char *text, size_t length,
                                       const LhatLoadOptions *options,
                                       bool with_debug_names, uint8_t **out,
                                       size_t *out_length)
{
    *out = NULL;
    *out_length = 0;
    lhat_program_hold(program);
    LhatUnit *unit = place(program, name, options);
    if (unit == NULL) {
        lhat_program_release(program);
        return LHAT_LOAD_OUT_OF_MEMORY;
    }
    lhat_source_init_from_string(&unit->source, unit->path, text, length);
    LhatLoadStatus status = build_placed(program, unit);
    if (status == LHAT_LOAD_OK) {
        LhatWriteStatus written =
            lhat_serialize_write(unit, with_debug_names, out, out_length);
        if (written != LHAT_WRITE_OK) {
            lhat_free(program->load_failure);
            program->load_failure =
                prg_duplicate("the unit could not be written out");
            status = written == LHAT_WRITE_OUT_OF_MEMORY
                         ? LHAT_LOAD_OUT_OF_MEMORY
                         : LHAT_LOAD_REJECTED;
        }
    }
    forget(unit);
    lhat_program_release(program);
    return status;
}

bool lhat_program_is_binary_unit(const char *bytes, size_t length)
{
    return lhat_serialize_is_binary(bytes, length);
}

LhatLoadStatus lhat_program_load_text(LhatProgram *program, const char *name,
                                      const char *text, size_t length,
                                      LhatProto **out)
{
    LhatLoadOptions options = {0};
    options.initial_bindings = true;  // [modules.host.initial], as it always was
    lhat_program_hold(program);
    LhatLoadStatus status =
        prg_load_text_held(program, name, text, length, &options, out);
    lhat_program_release(program);
    return status;
}

LhatLoadStatus lhat_program_load_file(LhatProgram *program, const char *path,
                                      LhatProto **out)
{
    *out = NULL;
    lhat_free(program->load_failure);
    program->load_failure = NULL;
    char *resolved = prg_resolve_against(program, NULL, path, strlen(path));
    if (resolved == NULL) {
        return path[0] == '$' && program->project_root == NULL
                   ? LHAT_LOAD_CANNOT_READ : LHAT_LOAD_OUT_OF_MEMORY;
    }
    size_t length = 0;
    char *text = program->load != NULL
                     ? program->load(program->loader_context, resolved, &length)
                     : NULL;
    LhatLoadStatus status = LHAT_LOAD_CANNOT_READ;
    if (text != NULL) {
        // The read is outside the lock: what it touches is the loader the
        // host gave, not the program.
        LhatLoadOptions options = {0};
        options.initial_bindings = true;
        lhat_program_hold(program);
        status =
            prg_load_text_held(program, resolved, text, length, &options, out);
        lhat_program_release(program);
        lhat_free(text);
    }
    lhat_free(resolved);
    return status;
}

const char *lhat_program_load_failure(const LhatProgram *program)
{
    return program->load_failure != NULL ? program->load_failure : "";
}

void lhat_program_dispose(LhatProgram *program)
{
    // [modules.binary]: the strings; the types are the arena's.
    for (size_t i = 0; i < program->enum_identity_count; i++) {
        lhat_free(program->enum_identities[i].path);
        lhat_free(program->enum_identities[i].name);
    }
    lhat_free(program->enum_identities);
    lhat_free(program->signature_index);
    lhat_free(program->signatures);
    // [modules.host.import]: what the registrations left with the program, first and in
    // reverse -- everything of the program's own is still standing here, so
    // a disposal may read whatever it was written against, and a module may
    // undo what one it built on top of has not undone yet.
    while (program->disposal_count > 0) {
        struct LhatProgramDisposal *at =
            &program->disposals[--program->disposal_count];
        at->call(at->context);
    }
    lhat_free(program->disposals);
    program->disposals = NULL;
    program->disposal_capacity = 0;

    lhat_free(program->load_failure);
    program->load_failure = NULL;
    // [l10n.choose.program]: the catalogs this program was handed, and the language it drew
    // its messages in.
    for (size_t i = 0; i < program->catalog_count; i++) {
        lhat_catalog_dispose(&program->catalogs[i]);
    }
    lhat_free(program->catalogs);
    program->catalogs = NULL;
    program->catalog_count = 0;
    program->catalog_capacity = 0;
    lhat_free(program->language);
    program->language = NULL;
    for (size_t i = 0; i < program->host_entry_count; i++) {
        lhat_free(program->host_entries[i].module);
        lhat_free(program->host_entries[i].type);
        lhat_free(program->host_entries[i].parameter_types);
        lhat_free(program->host_entries[i].name);
        lhat_free(program->host_entries[i].signature_text);
        lhat_free(program->host_entries[i].const_text);
    }
    for (size_t i = 0; i < program->host_enum_count; i++) {
        LhatProgramEnum *e = &program->host_enums[i];
        for (size_t k = 0; k < e->count; k++) {
            lhat_free(e->members[k]);
        }
        lhat_free(e->members);
        lhat_free(e->values);
        lhat_free(e->module);
        lhat_free(e->type);
        lhat_free(e->name);
        // [modules.hostdata]: the tag is not the program's -- one declaration, one tag,
        // for the process (registry.h).
    }
    lhat_free(program->host_entries);
    program->host_entries = NULL;
    program->host_entry_count = 0;
    program->host_entry_capacity = 0;
    lhat_free(program->host_enums);
    program->host_enums = NULL;
    program->host_enum_count = 0;
    program->host_enum_capacity = 0;

    // [syntax.annot.host]: the declaration owns the two names it points at, and the
    // text sits beside it.
    for (size_t i = 0; i < program->annotation_count; i++) {
        lhat_free((void *)program->annotations[i].module);
        lhat_free((void *)program->annotations[i].name);
        lhat_free(program->annotation_signatures[i]);
        for (size_t k = 0; k < program->annotations[i].exclusive_count; k++) {
            lhat_free((void *)program->annotations[i].exclusives[k]);
        }
        lhat_free((void *)program->annotations[i].exclusives);
        for (size_t k = 0; k < program->annotations[i].requisite_count; k++) {
            lhat_free((void *)program->annotations[i].requisites[k]);
        }
        lhat_free((void *)program->annotations[i].requisites);
    }
    lhat_free(program->annotations);
    lhat_free(program->annotation_signatures);
    program->annotations = NULL;
    program->annotation_signatures = NULL;
    program->annotation_count = 0;
    program->annotation_capacity = 0;

    // host_type_entries[i] の module/name/tag は上の host_entries[i] が
    // 所有する同じポインタを指すだけなので、ここでは配列自体だけを解放
    // する(要素の中身は既に上で解放済み)。
    lhat_free(program->host_type_entries);
    program->host_type_entries = NULL;
    program->host_type_entry_count = 0;
    program->host_type_entry_capacity = 0;

    // [modules.hostvalue]: the tags and their fields belong to the process, not to
    // this program -- one declaration, one tag (registry.h). Only the array
    // of entries is the program's.
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
    lhat_program_forget_completions(program);
    // [modules.host.shared-registry]: the frontier itself. What it points at is on host_heap
    // and goes with it below.
    lhat_free(program->shared);
    program->shared = NULL;
    program->shared_count = 0;
    program->shared_capacity = 0;
    program->shared_ready = false;
    // [errors.grammar.types]: この program が登録した誤り種別のオブジェクト自身
    // (LhatErrorKind/LhatString)。chunk->heap と同じ扱いで、program の
    // 寿命が尽きるここでまとめて解放する。
    lhat_object_free_all(&program->host_heap);

    for (size_t i = 0; i < program->global_count; i++) {
        lhat_free(program->global_entries[i].name);
        lhat_free(program->global_entries[i].signature_text);
        lhat_free(program->global_entries[i].parameter_types);
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
        unit_dispose_contents(unit);
        lhat_free(unit);
        unit = next;
    }
    program->units = NULL;

    // [modules.require.reload]: what invalidations retired and the host never discarded.
    // The program outlives every machine that could hold a closure of one,
    // so here it is safe whether the host took a pass or not.
    lhat_program_discard_retired(program);
    // And what earlier discards handed over: the objects those bodies'
    // constants named, which a machine may have been holding. The machines
    // are gone by the time a program is, so this is where they end.
    lhat_object_free_all(&program->retired_objects);

    for (size_t i = 0; i < program->diagnostic_count; i++) {
        lhat_free(program->diagnostics[i].path);
    }
    lhat_free(program->diagnostics);
    program->diagnostics = NULL;
    program->diagnostic_count = 0;
    program->diagnostic_capacity = 0;

    lhat_type_arena_dispose(&program->types);
    lhat_free(program->project_root);
    program->project_root = NULL;
}

// The opaque forms a host uses (program.h): the by-value pair above wrapped
// around an allocation.
LhatProgram *lhat_program_new(bool strict, LhatProgramLoader load,
                              void *context)
{
    // Prepare the language's shared error before a host hands independently
    // registered programs to compiler threads. Checking/serialization may ask
    // for it even when the program has never executed a cast.
    if (lhat_registry_cast_failure() == NULL) {
        return NULL;
    }
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

const LhatProto *lhat_unit_proto(const LhatUnit *unit)
{
    return unit->proto;
}

const char *lhat_unit_module_name(const LhatUnit *unit)
{
    // [modules.require.reload]: the unit's own copy, so that this still answers after an
    // invalidation -- which is exactly when a host needs it, to tell each
    // machine to forget what the unit registered.
    return unit->module_name;
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
    if (unit == NULL || !unit->loaded || unit->state != LHAT_UNIT_DONE ||
        unit->lexer.diagnostic_count != 0 || unit->parsed.diagnostic_count != 0) {
        return false;
    }
#if LHAT_WITH_FRONTEND
    return lhat_check_error_count(&unit->checked) == 0;
#else
    return true;
#endif
}

LhatCompileStatus lhat_program_compile_status(const LhatProgram *program)
{
    return program->compile_status;
}

LhatCompileResult lhat_program_compile_failure(const LhatProgram *program,
                                               const char **path)
{
    if (path != NULL) {
        *path = program->compile_unit != NULL ? program->compile_unit->path
                                              : NULL;
    }
    return program->compile_result;
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

// ---------------------------------------------------------------------------
// [modules.require.reload]: a unit changed under the program
// ---------------------------------------------------------------------------

static bool among(LhatUnit *const *set, size_t count, const LhatUnit *unit)
{
    for (size_t i = 0; i < count; i++) {
        if (set[i] == unit) {
            return true;
        }
    }
    return false;
}

// `start` and every unit whose require^s reach it, however far around. Walks
// the graph backwards, which is what LhatUnit.referenced is kept for.
//
// A fixed point over the list rather than a traversal: the edges run the
// wrong way for one, a require^ graph may hold a cycle the checker reported
// and carried on past ([l10n.catalog.dump]), and a project's units are few enough that the
// difference is nothing. `set` has room for every unit.
static size_t reaching(LhatProgram *program, LhatUnit *start, LhatUnit **set)
{
    set[0] = start;
    size_t count = 1;
    for (bool grew = true; grew;) {
        grew = false;
        for (LhatUnit *u = program->units; u != NULL; u = u->next) {
            if (among(set, count, u)) {
                continue;
            }
            for (size_t i = 0; i < u->referenced_count; i++) {
                if (among(set, count, u->referenced[i])) {
                    set[count++] = u;
                    grew = true;
                    break;
                }
            }
        }
    }
    return count;
}

// Whether the text at the unit's path still reads as it did. A path that
// cannot be read answers false: a deleted file has to stop being what the
// program compiled.
static bool text_unchanged(LhatProgram *program, const LhatUnit *unit)
{
    if (program->load == NULL || unit->source_hash == 0) {
        return false;  // nothing to compare against
    }
    size_t length = 0;
    char *text = program->load(program->loader_context, unit->path, &length);
    if (text == NULL) {
        return false;
    }
    uint64_t now = prg_hash_text(text, length);
    lhat_free(text);
    return now == unit->source_hash;
}

// Retires the bodies of every unit in `set`, clears what was made of them,
// and takes back what they were blamed for. Answers `count`, or SIZE_MAX
// when there was no room -- in which case nothing was touched.
//
// Shared by the two invalidations, which differ only in how the set is
// chosen: one unit and what reaches it, or all of them.
static size_t retire_set(LhatProgram *program, LhatUnit **set, size_t count)
{
    // Room for every body about to be retired, taken before anything is
    // touched. A half-done invalidation would leave some units stale and
    // others holding bodies nothing points at any more, so the one place
    // this can fail is made to fail first.
    size_t wanted = program->retired_count + count;
    if (wanted > program->retired_capacity) {
        LhatProto **bigger = (LhatProto **)lhat_realloc(program->retired,
                                                        wanted * sizeof *bigger);
        if (bigger == NULL) {
            return SIZE_MAX;
        }
        program->retired = bigger;
        program->retired_capacity = wanted;
    }

    for (size_t i = 0; i < count; i++) {
        LhatUnit *u = set[i];
        // The body is retired, not freed: a closure made before now still
        // points into it, and so does the unit table of every other retired
        // body that required this one. Both worlds stay whole.
        if (u->proto != NULL) {
            program->retired[program->retired_count++] = u->proto;
            u->proto = NULL;
        }
        unit_clear_stages(u);
        u->state = LHAT_UNIT_STALE;
        u->source_hash = 0;
    }

    // What these units drew is about text that is gone. Left standing it
    // would outlive every fix -- lhat_program_has_errors reads the list, so
    // one require^ that once could not be read would make the program wrong
    // for ever, however the file was mended.
    size_t kept = 0;
    for (size_t i = 0; i < program->diagnostic_count; i++) {
        bool retired = false;
        for (size_t k = 0; k < count && !retired; k++) {
            retired = strcmp(program->diagnostics[i].path, set[k]->path) == 0;
        }
        if (retired) {
            lhat_free(program->diagnostics[i].path);
        } else {
            program->diagnostics[kept++] = program->diagnostics[i];
        }
    }
    program->diagnostic_count = kept;

    // And the same for the one compile failure the program remembers: its
    // position indexes a source this call just threw away.
    if (program->compile_unit != NULL &&
        among(set, count, program->compile_unit)) {
        program->compile_status = LHAT_COMPILE_OK;
        memset(&program->compile_result, 0, sizeof program->compile_result);
        program->compile_unit = NULL;
    }
    return count;
}

static size_t unit_total(const LhatProgram *program)
{
    size_t total = 0;
    for (const LhatUnit *u = program->units; u != NULL; u = u->next) {
        total++;
    }
    return total;
}

size_t prg_invalidate_held(LhatProgram *program, const char *path)
{
    if (program == NULL || path == NULL) {
        return SIZE_MAX;
    }
    char *resolved = prg_resolve_against(program, NULL, path, strlen(path));
    if (resolved == NULL) {
        return SIZE_MAX;
    }
    LhatUnit *start = prg_find_unit(program, resolved);
    lhat_free(resolved);
    if (start == NULL) {
        return SIZE_MAX;
    }
    // Already stale: the cascade ran and nothing has read it back yet.
    if (start->state == LHAT_UNIT_STALE) {
        return 0;
    }
    if (text_unchanged(program, start)) {
        return 0;
    }

    LhatUnit **set = (LhatUnit **)lhat_alloc(unit_total(program) * sizeof *set);
    if (set == NULL) {
        return SIZE_MAX;
    }
    size_t count = retire_set(program, set, reaching(program, start, set));
    lhat_free(set);
    return count;
}

size_t lhat_program_invalidate(LhatProgram *program, const char *path)
{
    lhat_program_hold(program);
    size_t retired = prg_invalidate_held(program, path);
    lhat_program_release(program);
    return retired;
}

void lhat_program_discard_retired(LhatProgram *program)
{
    if (program == NULL) {
        return;
    }
    lhat_program_hold(program);
    prg_discard_retired_held(program);
    lhat_program_release(program);
}

void prg_discard_retired_held(LhatProgram *program)
{
    for (size_t i = 0; i < program->retired_count; i++) {
        // The code goes; what its constants named does not. A machine may
        // still be holding one -- L^.modules is keyed by the strings a
        // unit's prologue loads, and those are the chunk's -- and a chunk's
        // objects were always meant to outlive every machine.
        lhat_proto_give_objects(program->retired[i], &program->retired_objects);
        lhat_proto_free(program->retired[i]);
    }
    lhat_free(program->retired);
    program->retired = NULL;
    program->retired_count = 0;
    program->retired_capacity = 0;
}

size_t lhat_program_retired_count(const LhatProgram *program)
{
    return program != NULL ? program->retired_count : 0;
}

// The bodies of one retired unit, nested ones included -- what a closure
// on some machine could still be running.
static size_t count_bodies(const LhatProto *proto)
{
    size_t count = 1;
    for (size_t i = 0; i < proto->proto_count; i++) {
        count += count_bodies(proto->protos[i]);
    }
    return count;
}

static size_t fill_bodies(const LhatProto *proto, const LhatProto **out,
                          size_t at)
{
    out[at++] = proto;
    for (size_t i = 0; i < proto->proto_count; i++) {
        at = fill_bodies(proto->protos[i], out, at);
    }
    return at;
}

// [modules.require.reload]'s question, asked outright: collect on every machine, then look for
// anything still holding a retired body. Held anywhere -- or a cleanup still
// waiting, whose run is code this walk cannot see into -- means the bodies
// wait for the next pass, which is the safe side of every answer here.
// Answers whether nothing retired is left waiting.
static bool discard_unheld_held(LhatProgram *program,
                                LhatMachine *const *machines,
                                size_t machine_count)
{
    if (program->retired_count == 0) {
        return true;
    }
    size_t bodies = 0;
    for (size_t i = 0; i < program->retired_count; i++) {
        bodies += count_bodies(program->retired[i]);
    }
    const LhatProto **flat =
        (const LhatProto **)lhat_alloc(bodies * sizeof *flat);
    if (flat == NULL) {
        return false;  // no room to ask safely, so keep them
    }
    size_t at = 0;
    for (size_t i = 0; i < program->retired_count; i++) {
        at = fill_bodies(program->retired[i], flat, at);
    }
    bool held = false;
    for (size_t i = 0; i < machine_count && !held; i++) {
        lhat_machine_gc_collect(machines[i]);
        held = lhat_machine_pending_disposals(machines[i]) > 0 ||
               lhat_machine_holds_body(machines[i], flat, bodies);
    }
    lhat_free(flat);
    if (held) {
        return false;
    }
    prg_discard_retired_held(program);
    return true;
}

bool lhat_program_discard_unheld(LhatProgram *program,
                                 LhatMachine *const *machines,
                                 size_t machine_count)
{
    if (program == NULL) {
        return true;
    }
    lhat_program_hold(program);
    bool discarded = discard_unheld_held(program, machines, machine_count);
    lhat_program_release(program);
    return discarded;
}

static size_t reload_held(LhatProgram *program, const char *path,
                          LhatMachine *const *machines, size_t machine_count)
{
    size_t retired = prg_invalidate_held(program, path);
    if (retired == 0 || retired == SIZE_MAX) {
        return retired;
    }

    // Forgetting reads the stale shells' module names, so it comes before
    // the recheck reads the path back in over them. Per machine: the
    // program does not know its machines (see the header).
    for (const LhatUnit *unit = lhat_program_units(program); unit != NULL;
         unit = lhat_unit_next(unit)) {
        if (lhat_unit_state(unit) != LHAT_UNIT_STALE) {
            continue;
        }
        const char *module = lhat_unit_module_name(unit);
        if (module == NULL) {
            continue;
        }
        for (size_t i = 0; i < machine_count; i++) {
            lhat_machine_forget_unit(machines[i], module);
        }
    }

    // Every stale unit is read back, not the edited path alone: the cascade
    // retired the requirers too, and a shell left with no body would be a
    // NULL proto to whoever runs it next. Checking a dependent pulls the
    // edited unit in on the way, so later rounds mostly find DONE.
    for (const LhatUnit *unit = lhat_program_units(program); unit != NULL;
         unit = lhat_unit_next(unit)) {
        if (lhat_unit_state(unit) == LHAT_UNIT_STALE) {
            prg_check_held(program, lhat_unit_path(unit));
        }
    }
    if (lhat_program_has_errors(program) || !prg_compile_all(program)) {
        // The old world keeps running and the retired list keeps waiting;
        // the diagnostics say why, and the next save tries again.
        return retired;
    }

    discard_unheld_held(program, machines, machine_count);
    return retired;
}

size_t lhat_reload(LhatProgram *program, const char *path,
                   LhatMachine *const *machines, size_t machine_count)
{
    lhat_program_hold(program);
    size_t retired = reload_held(program, path, machines, machine_count);
    lhat_program_release(program);
    return retired;
}
