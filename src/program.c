// L^ (lhat) -- a program: one unit and everything it requires.

#include "message.h"
#include "program_internal.h"
#include "serialize.h"

#include <math.h>   // isnan, isinf: lhat_program_dump_host_api's reals
#include <stdio.h>  // snprintf: lhat_program_dump_host_api's numbers
#include <stdlib.h>
#include <string.h>

#include "compile.h"  // [modules.require.once]: the units are compiled here too
#include "gc.h"  // LHAT_GC_BLACK -- host_heap の初期色 ([errors.grammar.types])
#include "grow.h"
#include "lhat/error.h"  // one shape for what a stage reported ([pipeline.stages.tree])
#include "lhat/port.h"
#include "rttype.h"
#include "type.h"
#include "lhat/vm.h"

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// [l10n.compose.no-plurals] keys the cache on the resolved path, so two spellings of one unit have
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

    // A POSIX absolute path opens with the separator, and the empty segment
    // in front of it is the one this loop must not drop -- without this,
    // /home/x normalises to home/x and the unit never opens. A Windows
    // absolute path opens with a drive letter instead, which is why this
    // went unseen. `root` is then the floor '..' cannot step below, and the
    // point after which a separator is already there.
    size_t root = 0;
    if (length > 0 && (path[0] == '/' || path[0] == '\\')) {
        out[written++] = '/';
        root = 1;
        i = 1;
    }

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
            if (written > root) {
                written--;  // drop the separator that led into the segment
            }
        } else {
            if (written > root) {
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

bool lhat_program_set_project_root(LhatProgram *program, const char *path)
{
    if (program == NULL) return false;
    char *root = path != NULL ? normalise_path(path) : NULL;
    if (path != NULL && root == NULL) return false;
    lhat_free(program->project_root);
    program->project_root = root;
    return true;
}

static bool absolute_path(const char *path, size_t length)
{
    return length > 0 && (path[0] == '/' || path[0] == '\\' ||
                          (length > 1 && path[1] == ':'));
}

// [modules.require.path]: a require^ is relative to the unit that wrote it.
char *prg_resolve_against(const LhatProgram *program, const char *base, const char *relative,
                             size_t relative_length)
{
    if (relative_length > 0 && relative[0] == '$') {
        if (program == NULL || program->project_root == NULL) return NULL;
        const char *root = program->project_root;
        size_t root_length = strlen(root);
        char *joined = lhat_alloc(root_length + relative_length + 1);
        if (joined == NULL) return NULL;
        memcpy(joined, root, root_length);
        size_t at = root_length;
        if (at > 0 && root[at - 1] != '/') joined[at++] = '/';
        memcpy(joined + at, relative + 1, relative_length - 1);
        joined[at + relative_length - 1] = '\0';
        char *resolved = normalise_path(joined);
        lhat_free(joined);
        return resolved;
    }
    if (absolute_path(relative, relative_length)) base = NULL;
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
    if (base_length > 0) memcpy(joined, base, base_length);
    memcpy(joined + base_length, relative, relative_length);
    joined[base_length + relative_length] = '\0';

    char *resolved = normalise_path(joined);
    lhat_free(joined);
    return resolved;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static void report(LhatProgram *program, LhatProgramErrorCode code,
                   const char *path)
{
    LHAT_GROW(program->diagnostics, program->diagnostic_count,
              program->diagnostic_capacity, 4, return);

    LhatProgramDiagnostic *d = &program->diagnostics[program->diagnostic_count];
    d->code = code;
    d->path = prg_duplicate(path);
    if (d->path == NULL) {
        return;
    }
    program->diagnostic_count++;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

char *prg_duplicate(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)lhat_alloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

LhatUnit *prg_find_unit(LhatProgram *program, const char *path)
{
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (strcmp(u->path, path) == 0) {
            return u;
        }
    }
    return NULL;
}

static LhatUnit *check_path(LhatProgram *program, char *path);

// [modules.require.once]: `from` require^s `to`. Answers the position in `from`'s list,
// which is the number the compiler writes into a UNIT instruction, or
// LHAT_NO_UNIT when there is no room.
//
// [pipeline.bc.coroutine] reads these backwards to find every unit a changed one reaches, and
// that is why the checker records them too rather than only the compiler:
// a unit the checker rejected never compiles, so its edges would never be
// known -- and an editor's units are rejected all day.
static size_t remember_edge(LhatUnit *from, LhatUnit *to)
{
    for (size_t i = 0; i < from->referenced_count; i++) {
        if (from->referenced[i] == to) {
            return i;
        }
    }
    LHAT_GROW(from->referenced, from->referenced_count,
              from->referenced_capacity, 4, return LHAT_NO_UNIT);
    from->referenced[from->referenced_count] = to;
    return from->referenced_count++;
}

// What the checker asks when it meets a require^ ([modules.deps.static]).
typedef struct {
    LhatProgram *program;
    LhatUnit *requiring;
} Resolution;

static LhatType *resolve_require(void *context, const char *path, size_t length,
                                 const char **module_name,
                                 const char **unit_path)
{
    Resolution *r = (Resolution *)context;
    char *resolved = prg_resolve_against(r->program, r->requiring->path, path, length);
    if (resolved == NULL) {
        return NULL;
    }

    LhatUnit *unit = check_path(r->program, resolved);  // takes `resolved`
    if (unit == NULL) {
        return NULL;
    }
    // [modules.require.reload]: recorded before the state is judged. A require^ that reached a
    // unit which would not read is still an edge -- when that file appears
    // and is invalidated, this unit is what has to be checked again.
    (void)remember_edge(r->requiring, unit);
    if (unit->state != LHAT_UNIT_DONE) {
        return NULL;
    }
    // [modules.binary]: a text unit reaching a binary one has no exports to read
    // -- a program is one or the other.
    if (unit->binary) {
        report(r->program, LHAT_PROGRAM_ERR_MIXED, unit->path);
        return NULL;
    }
    // [modules.unit]: the unit outlives this program's checking, so handing the
    // text out rather than a copy is safe -- and its path the same.
    if (module_name != NULL) {
        *module_name = unit->checked.module_name;
    }
    if (unit_path != NULL) {
        *unit_path = unit->path;
    }
    // A unit publishing nothing still loaded, so it answers with an empty
    // structure rather than with failure.
    return unit->checked.exports != NULL
               ? unit->checked.exports
               : lhat_type_table(&r->program->types);
}

// [modules.require.reload]: FNV-1a over a unit's text, so that a save which changed nothing
// can be told from one that did. Not a security question -- a collision costs
// a reload that was not needed, or misses one that was, and the host may
// always invalidate a path it knows changed.
uint64_t prg_hash_text(const char *text, size_t length)
{
    uint64_t hash = 1469598103934665603u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 1099511628211u;
    }
    // Zero is "nothing was read yet", so a text that hashes there takes the
    // next value along rather than looking unread.
    return hash != 0 ? hash : 1u;
}

// Reads the unit's text through the program's loader and takes it as far as
// checking. Shared by a unit met for the first time and by one [modules.require.reload] made
// stale, which is the whole reason it stands apart from check_path.
static void load_into(LhatProgram *program, LhatUnit *unit)
{
    unit->state = LHAT_UNIT_CHECKING;

    // [modules.hostvalue]: no loader is not an error of the program's -- it is a host
    // that never handed one over, and then nothing can be read.
    size_t length = 0;
    char *text = program->load != NULL
                     ? program->load(program->loader_context, unit->path,
                                     &length)
                     : NULL;
    if (text == NULL) {
        report(program, LHAT_PROGRAM_ERR_CANNOT_READ, unit->path);
        unit->state = LHAT_UNIT_FAILED;
        return;
    }

    unit->source_hash = prg_hash_text(text, length);

    // [modules.binary]: the bytes say which they are. A binary unit is built
    // rather than read -- no tree, no check -- and lands in the same state
    // a checked one does, so that everything after this treats the two
    // alike.
    if (lhat_serialize_is_binary(text, length)) {
        LhatBinaryUnit read;
        bool ok = lhat_serialize_load(program, unit, (const uint8_t *)text,
                                      length, &read);
        lhat_free(text);
        if (!ok) {
            unit->state = LHAT_UNIT_FAILED;
            return;
        }
        unit->binary = true;
        unit->loaded = true;
        unit->proto = read.proto;
        prg_stamp_source(read.proto, unit->path);
        lhat_free(unit->module_name);
        unit->module_name = read.module_name;
        unit->export_names = read.export_names;
        unit->export_rt = read.export_rt;
        unit->export_rt_count = read.export_count;
        unit->reflection = read.reflection;
        unit->state = LHAT_UNIT_DONE;
        return;
    }

#if LHAT_WITH_FRONTEND
    lhat_source_init_from_string(&unit->source, unit->path, text, length);
    lhat_free(text);
    prg_check_parsed(program, unit, &program->types);
#else
    // [modules.binary.vm-only]: text, and nothing here reads text.
    lhat_free(text);
    report(program, LHAT_PROGRAM_ERR_NO_FRONTEND, unit->path);
    unit->state = LHAT_UNIT_FAILED;
#endif
}

// Takes ownership of `path`.
static LhatUnit *check_path(LhatProgram *program, char *path)
{
    // [modules.hostdata.base]: registration has closed by the time anything is checked,
    // so this is where a host type comes to carry its base's members.
    prg_flatten_hostdata_bases(program);

    LhatUnit *existing = prg_find_unit(program, path);
    if (existing != NULL) {
        // [l10n.catalog.dump]: meeting a unit that is still being checked means the graph
        // has a cycle. Reported here, where both ends are known.
        if (existing->state == LHAT_UNIT_CHECKING) {
            report(program, LHAT_PROGRAM_ERR_CYCLE, path);
            lhat_free(path);
            return NULL;
        }
        lhat_free(path);
        // [modules.require.reload]: an invalidated unit is read again here -- the shell is
        // the same one the host may be holding, and only what was made of it
        // went. Every other state is [modules.require.once]'s "loaded once": a second require^
        // gets the same unit.
        if (existing->state == LHAT_UNIT_STALE) {
            load_into(program, existing);
        }
        return existing;
    }

    LhatUnit *unit = (LhatUnit *)lhat_calloc(1, sizeof *unit);
    if (unit == NULL) {
        lhat_free(path);
        return NULL;
    }
    unit->path = path;
    unit->program = program;
    unit->state = LHAT_UNIT_CHECKING;
    unit->next = program->units;
    program->units = unit;

    load_into(program, unit);
    return unit;
}

// Lexes, parses and checks a unit whose source is in place. `arena` is the
// program's for a unit of the program ([lton.nil]: what it publishes has to
// outlive it) and NULL for a loaded script ([modules.require.load]), whose types nobody else
// will point at -- the result then owns them.
#if LHAT_WITH_FRONTEND
void prg_check_parsed(LhatProgram *program, LhatUnit *unit,
                         LhatTypeArena *arena)
{
    lhat_lexer_init(&unit->lexer, &unit->source);
    size_t path_length = strlen(unit->path);
    if (unit->lton || (program->syntax_only && path_length >= 5 && strcmp(unit->path + path_length - 5, ".lton") == 0))
        lhat_parse_lton(&unit->lexer, &unit->parsed);
    else lhat_parse(&unit->lexer, &unit->parsed);
    unit->loaded = true;
    unit->state = LHAT_UNIT_CHECKING;

    if (program->syntax_only) {
        unit->state = LHAT_UNIT_DONE;
        return;
    }

    Resolution resolution;
    resolution.program = program;
    resolution.requiring = unit;

    LhatRequire require;
    require.resolve = resolve_require;
    require.context = &resolution;
    require.hosted = program->hosted;  // [modules.host.import]
    require.globals = program->globals;  // [modules.host.machine]
    // [modules.host.initial]: the initial bindings are the host's convenience for a
    // program it means to run. A text read as data is not that -- what a
    // host bound for its own units is no part of what a configuration file
    // may name -- so LhatLoadOptions can leave them out.
    bool initial = !unit->as_data;
    require.initial_names =
        initial ? (const char *const *)program->initial_names : NULL;
    require.initial_members =
        initial ? (const char *const *)program->initial_members : NULL;
    require.initial_count = initial ? program->initial_count : 0;
    require.annotations = program->annotations;  // [syntax.annot.host]
    require.annotation_count = program->annotation_count;

    // The recursion is what puts the graph in dependency order ([modules.deps.order]): the
    // required unit finishes before this one's checking gets past the
    // require^ that asked for it.
    lhat_check_unit(unit->parsed.root, &unit->lexer, program->strict, arena,
                    &require, &unit->checked);

    // [modules.require.reload]: kept past the check that read it, since an invalidation
    // throws the check away and the name is what a machine holds the unit
    // under. A second check of the same unit may read a different one.
    lhat_free(unit->module_name);
    unit->module_name = unit->checked.module_name != NULL
                            ? prg_duplicate(unit->checked.module_name)
                            : NULL;

    unit->state = LHAT_UNIT_DONE;
}
#endif  // LHAT_WITH_FRONTEND

// [modules.require]: the compile-time twin of resolve_require. The unit is already
// there -- checking put it there -- so this only has to say where it sits.
#if LHAT_WITH_FRONTEND
static size_t resolve_unit(void *context, const char *path, size_t length,
                           const char **module_name)
{
    Resolution *r = (Resolution *)context;
    char *resolved = prg_resolve_against(r->program, r->requiring->path, path, length);
    if (resolved == NULL) {
        return LHAT_NO_UNIT;
    }
    LhatUnit *unit = prg_find_unit(r->program, resolved);
    lhat_free(resolved);
    if (unit == NULL) {
        return LHAT_NO_UNIT;
    }
    if (module_name != NULL) {
        *module_name = unit->checked.module_name;
    }
    // The number is the requiring unit's own -- a position in its table
    // (LhatUnitTable), which this list becomes once everything compiled.
    return remember_edge(r->requiring, unit);
}

// ---------------------------------------------------------------------------
// [syntax.annot]: what a unit wrote as annotations
// ---------------------------------------------------------------------------

static bool unit_name_is(const LhatUnit *unit, const LhatNode *node,
                         const char *name)
{
    const char *spelt = NULL;
    size_t length = 0;
    if (node == NULL || name == NULL ||
        !lhat_node_name(node, unit->lexer.source->text, unit->lexer.strings,
                        &spelt, &length)) {
        return false;
    }
    return strlen(name) == length && memcmp(spelt, name, length) == 0;
}

// The top-level binding of `name`, or NULL. [syntax.annot.where] puts an annotation on the
// declaration, so this is where one is looked for.
static const LhatNode *unit_top_binding(const LhatUnit *unit,
                                        const char *name)
{
    if (unit == NULL || !unit->loaded || unit->parsed.root == NULL) {
        return NULL;
    }
    for (const LhatNode *s = unit->parsed.root->v.list.items; s != NULL;
         s = s->next) {
        if (s->kind != LHAT_NODE_DEFINE || s->v.binding.targets == NULL ||
            s->v.binding.targets->next != NULL) {
            continue;
        }
        if (unit_name_is(unit, s->v.binding.targets, name)) {
            return s;
        }
    }
    return NULL;
}

// A member of the definition, or a field of its template -- [syntax.annot.where] puts an
// annotation on both, and a host asking for `hp` does not care which it is.
static const LhatNode *definition_entry(const LhatUnit *unit,
                                        const LhatNode *definition,
                                        const char *name)
{
    if (definition == NULL) {
        return NULL;
    }
    // [syntax.obj.compose]: a definition is often written as a composition, and what a host
    // asks about may have been written in any part of it. The right side is
    // what overrides, so it is asked first.
    if (definition->kind == LHAT_NODE_BINARY &&
        definition->v.binary.op == LHAT_OP_CONCAT) {
        const LhatNode *found =
            definition_entry(unit, definition->v.binary.right, name);
        return found != NULL
                   ? found
                   : definition_entry(unit, definition->v.binary.left, name);
    }
    // A name standing for a definition written elsewhere in this unit.
    if (definition->kind == LHAT_NODE_IDENT ||
        definition->kind == LHAT_NODE_MEMBER) {
        return NULL;  // [modules.require.once]: another unit's tree is not this one's to walk
    }
    if (definition->kind != LHAT_NODE_DEF) {
        return NULL;
    }
    for (const LhatNode *entry = definition->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key != NULL) {
            if (unit_name_is(unit, entry->v.entry.key, name)) {
                return entry;
            }
            continue;
        }
        // [syntax.obj.methods] with [syntax.obj.delegate]: the template has no key -- and neither has the
        // delegate entry, so what marks the template is the absence of both.
        // What a delegate lends is not written here, and this answers what
        // the unit's tree says.
        if (entry->v.entry.modifier == LHAT_DEF_DELEGATE) {
            continue;
        }
        const LhatNode *template = entry->v.entry.value;
        if (template == NULL) {
            continue;
        }
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            if (unit_name_is(unit, field->v.entry.key, name)) {
                return field;
            }
        }
    }
    return NULL;
}

static const LhatNode *unit_annotations_of(const LhatUnit *unit,
                                           const char *definition,
                                           const char *name)
{
    if (unit == NULL || !unit->loaded || unit->parsed.root == NULL) {
        return NULL;
    }
    if (definition == NULL && name == NULL) {
        return unit->parsed.root->v.list.annotations;
    }
    if (definition == NULL) {
        const LhatNode *binding = unit_top_binding(unit, name);
        return binding != NULL ? binding->v.binding.annotations : NULL;
    }

    const LhatNode *binding = unit_top_binding(unit, definition);
    if (binding == NULL) {
        return NULL;
    }
    if (name == NULL) {
        return binding->v.binding.annotations;
    }
    const LhatNode *entry =
        definition_entry(unit, binding->v.binding.values, name);
    return entry != NULL ? entry->v.entry.annotations : NULL;
}

static size_t tree_annotation_count(const LhatUnit *unit, const char *definition,
                                  const char *name)
{
    size_t count = 0;
    for (const LhatNode *at = unit_annotations_of(unit, definition, name);
         at != NULL; at = at->next) {
        count++;
    }
    return count;
}

static LhatAnnotation tree_annotation(const LhatUnit *unit,
                                    const char *definition, const char *name,
                                    size_t index)
{
    LhatAnnotation out;
    memset(&out, 0, sizeof out);

    const LhatNode *at = unit_annotations_of(unit, definition, name);
    for (size_t i = 0; at != NULL && i < index; i++) {
        at = at->next;
    }
    if (at == NULL) {
        return out;
    }

    const char *spelt = NULL;
    size_t length = 0;
    if (lhat_node_name(at->v.named.name, unit->lexer.source->text,
                       unit->lexer.strings, &spelt, &length)) {
        out.name = spelt;
        out.name_length = length;
    }
    for (const LhatNode *argument = at->v.named.members; argument != NULL;
         argument = argument->next) {
        out.argument_count++;
    }
    out.written = at;
    out.unit = unit;
    return out;
}

static LhatAnnotationArgument tree_argument(LhatAnnotation annotation,
                                                size_t at)
{
    LhatAnnotationArgument out;
    memset(&out, 0, sizeof out);

    const LhatNode *node = (const LhatNode *)annotation.written;
    const LhatUnit *unit = (const LhatUnit *)annotation.unit;
    if (node == NULL || unit == NULL) {
        return out;
    }
    const LhatNode *argument = node->v.named.members;
    for (size_t i = 0; argument != NULL && i < at; i++) {
        argument = argument->next;
    }
    if (argument == NULL) {
        return out;
    }

    // [syntax.annot.literal-args]'s leading '-' is kept as the unary it is, and applied here so a
    // host reads one number rather than a shape.
    LhatReal sign = 1.0;
    if (argument->kind == LHAT_NODE_UNARY) {
        sign = -1.0;
        argument = argument->v.unary.operand;
        if (argument == NULL) {
            return out;
        }
    }

    switch (argument->kind) {
        case LHAT_NODE_INT:
            out.kind = LHAT_ANNOTATION_ARG_NUMBER;
            out.number = sign * (LhatReal)argument->v.integer.value;
            break;
        case LHAT_NODE_FLOAT:
            out.kind = LHAT_ANNOTATION_ARG_NUMBER;
            out.number = sign * argument->v.real;
            break;
        case LHAT_NODE_STRING:
            out.kind = LHAT_ANNOTATION_ARG_STRING;
            out.text = unit->lexer.strings + argument->v.string.offset;
            out.length = argument->v.string.length;
            break;
        case LHAT_NODE_IDENT:
            out.kind = LHAT_ANNOTATION_ARG_NAME;
            out.text = unit->lexer.source->text + argument->v.name.offset;
            out.length = argument->v.name.length;
            break;
        case LHAT_NODE_HAT_IDENT:
            out.kind = LHAT_ANNOTATION_ARG_BOOL;
            out.boolean = argument->v.name.length == 5 &&
                          memcmp(unit->lexer.source->text + argument->v.name.offset,
                                 "true^", 5) == 0;
            break;
        default:
            break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// [lex.comment.kept]: what a unit says about itself
// ---------------------------------------------------------------------------

// The node a description is read off, for the same four addresses the
// annotation reader takes. Only the unit's own differs from that reader:
// attach_comments hands what stands before the first statement to that
// statement rather than to the root, so the block at the head of the file is
// found there.
static const LhatNode *unit_documented_node(const LhatUnit *unit,
                                            const char *definition,
                                            const char *name)
{
    if (unit == NULL || !unit->loaded || unit->parsed.root == NULL) {
        return NULL;
    }
    if (definition == NULL && name == NULL) {
        return unit->parsed.root->v.list.items;
    }
    if (definition == NULL) {
        return unit_top_binding(unit, name);
    }

    const LhatNode *binding = unit_top_binding(unit, definition);
    if (binding == NULL || name == NULL) {
        return binding;
    }
    return definition_entry(unit, binding->v.binding.values, name);
}

static size_t tree_documentation(const LhatUnit *unit, const char *definition,
                               const char *name, char *out, size_t capacity)
{
    const LhatNode *node = unit_documented_node(unit, definition, name);
    if (node == NULL) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return lhat_node_documentation(node, unit->lexer.source->text,
                                   unit->lexer.source->length, out, capacity);
}


// The entries of a definition and of its template, in written order, as one
// run -- which is what a host asking "what does this class declare" wants.
static const LhatNode *definition_entry_at(const LhatUnit *unit,
                                           const LhatNode *definition,
                                           size_t index, size_t *seen)
{
    if (definition == NULL) {
        return NULL;
    }
    if (definition->kind == LHAT_NODE_BINARY &&
        definition->v.binary.op == LHAT_OP_CONCAT) {
        const LhatNode *found = definition_entry_at(
            unit, definition->v.binary.left, index, seen);
        return found != NULL ? found
                             : definition_entry_at(
                                   unit, definition->v.binary.right, index,
                                   seen);
    }
    if (definition->kind != LHAT_NODE_DEF) {
        return NULL;
    }
    for (const LhatNode *entry = definition->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key != NULL) {
            if ((*seen)++ == index) {
                return entry;
            }
            continue;
        }
        // [syntax.obj.delegate]: the delegate entry has no key either -- see
        // definition_entry above, which counts the same entries this does.
        if (entry->v.entry.modifier == LHAT_DEF_DELEGATE) {
            continue;
        }
        const LhatNode *template = entry->v.entry.value;
        if (template == NULL) {
            continue;
        }
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            if (field->v.entry.key == NULL) {
                continue;
            }
            if ((*seen)++ == index) {
                return field;
            }
        }
    }
    return NULL;
}

// The entry a member index names, which every question about that member
// starts from.
static const LhatNode *member_entry(const LhatUnit *unit,
                                    const char *definition, size_t index)
{
    const LhatNode *binding = unit_top_binding(unit, definition);
    if (binding == NULL) {
        return NULL;
    }
    size_t seen = 0;
    return definition_entry_at(unit, binding->v.binding.values, index, &seen);
}

// The parameters a member wrote, whether it wrote a signature ([syntax.obj.abstract]) or the
// f^ / p^ itself. [syntax.type.param-names]: self^ is a mark saying a call hands the receiver
// over, not a parameter a call writes -- so it is not one here either.
static const LhatNode *member_params(const LhatUnit *unit,
                                     const LhatNode *entry)
{
    if (entry == NULL) {
        return NULL;
    }
    const LhatNode *written = entry->v.entry.value;
    if (written == NULL || (written->kind != LHAT_NODE_TYPE_FUNC &&
                            written->kind != LHAT_NODE_FUNC)) {
        return NULL;
    }
    const LhatNode *param = written->v.func.params;
    const char *spelt = NULL;
    size_t length = 0;
    if (param != NULL &&
        lhat_node_name(param->v.param.name, unit->lexer.source->text,
                       unit->lexer.strings, &spelt, &length) &&
        length == 5 && memcmp(spelt, "self^", 5) == 0) {
        param = param->next;
    }
    return param;
}

// A declaration has none, which is what makes it a declaration.
static const LhatNode *member_body(const LhatNode *entry)
{
    if (entry == NULL || entry->v.entry.value == NULL ||
        entry->v.entry.value->kind != LHAT_NODE_FUNC) {
        return NULL;
    }
    return entry->v.entry.value->v.func.body;
}

static LhatUnitTypeKind written_type_kind(const LhatUnit *unit,
                                          const LhatNode *type)
{
    const char *spelt = NULL;
    size_t length = 0;
    if (type == NULL || type->kind != LHAT_NODE_TYPE_NAME ||
        !lhat_node_name(type, unit->lexer.source->text, unit->lexer.strings,
                        &spelt, &length)) {
        return LHAT_UNIT_TYPE_OTHER;
    }
    if (length == 7 && memcmp(spelt, "number^", 7) == 0) {
        return LHAT_UNIT_TYPE_NUMBER;
    }
    if (length == 7 && memcmp(spelt, "string^", 7) == 0) {
        return LHAT_UNIT_TYPE_STRING;
    }
    if (length == 5 && memcmp(spelt, "bool^", 5) == 0) {
        return LHAT_UNIT_TYPE_BOOL;
    }
    return LHAT_UNIT_TYPE_OTHER;
}

// What a value is written as, for a field given one instead of a type. [syntax.obj.template]
// lets 'speed = 1' stand without saying number^, and a host putting a widget
// on that field has nothing else to read: this runs before anything has, so
// there is no value to look at, only the way one was spelt.
//
// Only a literal answers. Anything worked out is the checker's to know, and
// what a host does with the answer -- pick a spin box or a text field -- is
// not worth being wrong about.
static LhatUnitTypeKind value_type_kind(const LhatUnit *unit,
                                        const LhatNode *value)
{
    if (value == NULL) {
        return LHAT_UNIT_TYPE_OTHER;
    }
    switch (value->kind) {
        case LHAT_NODE_INT:
        case LHAT_NODE_FLOAT:
            return LHAT_UNIT_TYPE_NUMBER;
        // [lex.string.interp]: an interpolation is a string however many holes it has.
        case LHAT_NODE_STRING:
        case LHAT_NODE_INTERP:
            return LHAT_UNIT_TYPE_STRING;
        case LHAT_NODE_HAT_IDENT:
            break;  // true^ and false^ arrive as these
        default:
            return LHAT_UNIT_TYPE_OTHER;
    }

    const char *spelt = NULL;
    size_t length = 0;
    if (!lhat_node_name(value, unit->lexer.source->text, unit->lexer.strings,
                        &spelt, &length)) {
        return LHAT_UNIT_TYPE_OTHER;
    }
    if ((length == 5 && memcmp(spelt, "true^", 5) == 0) ||
        (length == 6 && memcmp(spelt, "false^", 6) == 0)) {
        return LHAT_UNIT_TYPE_BOOL;
    }
    return LHAT_UNIT_TYPE_OTHER;
}

// One pass answers both the count and the one at an index, since neither is
// worth an array the unit would have to keep.
typedef struct {
    const LhatUnit *unit;
    size_t wanted;
    size_t seen;
    LhatUnitText found;
} NameWalk;

static void walk_written_names(NameWalk *walk, const LhatNode *node);

static void written_names_child(void *context, const char *field, bool in_list,
                                const LhatNode *child)
{
    (void)field;
    (void)in_list;
    walk_written_names((NameWalk *)context, child);
}

static void walk_written_names(NameWalk *walk, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    if (node->kind == LHAT_NODE_CALL) {
        for (const LhatNode *at = node->v.access.argument; at != NULL;
             at = at->next) {
            LhatUnitText text;
            text.text = NULL;
            text.length = 0;
            // [lex.ident.id-hat]: id^name and "name" answer the same bytes, and which
            // one was written is the reader's business rather than a host's.
            if (at->kind == LHAT_NODE_STRING) {
                text.text = walk->unit->lexer.strings + at->v.string.offset;
                text.length = at->v.string.length;
            } else if (at->kind != LHAT_NODE_NAME ||
                       !lhat_node_name(at, walk->unit->lexer.source->text,
                                       walk->unit->lexer.strings, &text.text,
                                       &text.length)) {
                continue;
            }
            if (walk->seen++ == walk->wanted) {
                walk->found = text;
            }
        }
    }
    lhat_node_visit_children(node, written_names_child, walk);
}

static size_t tree_member_count(const LhatUnit *unit, const char *definition)
{
    const LhatNode *binding = unit_top_binding(unit, definition);
    if (binding == NULL) {
        return 0;
    }
    size_t seen = 0;
    definition_entry_at(unit, binding->v.binding.values, (size_t)-1, &seen);
    return seen;
}

static LhatUnitMember tree_member(const LhatUnit *unit, const char *definition,
                                size_t index)
{
    LhatUnitMember out;
    memset(&out, 0, sizeof out);

    const LhatNode *entry = member_entry(unit, definition, index);
    if (entry == NULL) {
        return out;
    }

    const char *spelt = NULL;
    size_t length = 0;
    if (lhat_node_name(entry->v.entry.key, unit->lexer.source->text,
                       unit->lexer.strings, &spelt, &length)) {
        out.name = spelt;
        out.name_length = length;
    }
    out.declared = entry->v.entry.declared;
    // [syntax.annot.host-fills]: what a writer writes when there is nothing for the language to
    // run. The clauses a block may carry ([modules.open]) count as something written,
    // so both halves of the list have to be empty for this to be true.
    const LhatNode *body = member_body(entry);
    out.empty_body = body != NULL && body->kind == LHAT_NODE_BLOCK &&
                     body->v.list.items == NULL && body->v.list.extra == NULL &&
                     body->v.list.arms == NULL;
    // The written type first: [syntax.obj.template]'s 'name : type = value' says it outright.
    // Where only a value was written, the way it was spelt is what is left.
    out.type = written_type_kind(unit, entry->v.entry.type);
    if (out.type == LHAT_UNIT_TYPE_OTHER) {
        out.type = value_type_kind(unit, entry->v.entry.value);
    }
    for (const LhatNode *at = member_params(unit, entry); at != NULL;
         at = at->next) {
        out.parameter_count++;
    }
    return out;
}

static LhatUnitParameter tree_member_parameter(const LhatUnit *unit,
                                             const char *definition,
                                             size_t member, size_t at)
{
    LhatUnitParameter out;
    memset(&out, 0, sizeof out);

    const LhatNode *entry = member_entry(unit, definition, member);
    if (entry == NULL) {
        return out;
    }
    const LhatNode *param = member_params(unit, entry);
    for (size_t i = 0; param != NULL && i < at; i++) {
        param = param->next;
    }
    if (param == NULL) {
        return out;
    }

    const char *spelt = NULL;
    size_t length = 0;
    if (lhat_node_name(param->v.param.name, unit->lexer.source->text,
                       unit->lexer.strings, &spelt, &length)) {
        out.name = spelt;
        out.name_length = length;
    }
    out.variadic = param->v.param.variadic;
    out.type = written_type_kind(unit, param->v.param.type);
    return out;
}

static size_t tree_written_name_count(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member)
{
    NameWalk walk;
    walk.unit = unit;
    walk.wanted = (size_t)-1;
    walk.seen = 0;
    memset(&walk.found, 0, sizeof walk.found);
    const LhatNode *entry = member_entry(unit, definition, member);
    walk_written_names(&walk, member_body(entry));
    return walk.seen;
}

static LhatUnitText tree_written_name(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member, size_t at)
{
    NameWalk walk;
    walk.unit = unit;
    walk.wanted = at;
    walk.seen = 0;
    memset(&walk.found, 0, sizeof walk.found);
    const LhatNode *entry = member_entry(unit, definition, member);
    walk_written_names(&walk, member_body(entry));
    return walk.found;
}
#endif  // LHAT_WITH_FRONTEND

// ---------------------------------------------------------------------------
// [modules.binary.reflect]: a binary unit answers [syntax.annot] off the records the bytes carried
// ---------------------------------------------------------------------------
//
// The addresses are the four lhat_unit_annotation_count names (program.h).
// The records hold the top-level bindings in written order, each with the
// members of the definition it holds -- what the walkers above answered
// when the unit was written, kept because the bytes have no tree. [modules.binary.vm-only]: a
// build without the front end has only these, and answers nothing else.

static bool reflected_named(const LhatReflectedText *text, const char *name)
{
    return text->text != NULL && strlen(name) == text->length &&
           memcmp(text->text, name, text->length) == 0;
}

static const LhatReflectedBinding *reflected_binding(const LhatUnit *unit,
                                                     const char *name)
{
    if (unit == NULL || unit->reflection == NULL || name == NULL) {
        return NULL;
    }
    const LhatReflection *r = unit->reflection;
    for (size_t i = 0; i < r->binding_count; i++) {
        if (reflected_named(&r->bindings[i].name, name)) {
            return &r->bindings[i];
        }
    }
    return NULL;
}

// The member at an index, or -- given a name -- the one definition_entry
// would find: the right side of a composition overrides, and the records
// run left to right, so the last written wins.
static const LhatReflectedMember *reflected_member(const LhatUnit *unit,
                                                   const char *definition,
                                                   const char *name,
                                                   size_t index)
{
    const LhatReflectedBinding *binding = reflected_binding(unit, definition);
    if (binding == NULL) {
        return NULL;
    }
    if (name == NULL) {
        return index < binding->member_count ? &binding->members[index] : NULL;
    }
    for (size_t i = binding->member_count; i-- > 0;) {
        if (reflected_named(&binding->members[i].name, name)) {
            return &binding->members[i];
        }
    }
    return NULL;
}

static const LhatReflectedAbout *reflected_about(const LhatUnit *unit,
                                                 const char *definition,
                                                 const char *name)
{
    if (unit == NULL || unit->reflection == NULL) {
        return NULL;
    }
    if (definition == NULL && name == NULL) {
        return &unit->reflection->about;
    }
    if (definition == NULL || name == NULL) {
        const LhatReflectedBinding *binding =
            reflected_binding(unit, definition != NULL ? definition : name);
        return binding != NULL ? &binding->about : NULL;
    }
    const LhatReflectedMember *member =
        reflected_member(unit, definition, name, 0);
    return member != NULL ? &member->about : NULL;
}

size_t lhat_unit_annotation_count(const LhatUnit *unit, const char *definition,
                                  const char *name)
{
    if (unit != NULL && unit->binary) {
        const LhatReflectedAbout *about =
            reflected_about(unit, definition, name);
        return about != NULL ? about->annotation_count : 0;
    }
#if LHAT_WITH_FRONTEND
    return tree_annotation_count(unit, definition, name);
#else
    (void)definition;
    (void)name;
    return 0;
#endif
}

LhatAnnotation lhat_unit_annotation(const LhatUnit *unit,
                                    const char *definition, const char *name,
                                    size_t index)
{
    LhatAnnotation out;
    memset(&out, 0, sizeof out);
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_annotation(unit, definition, name, index);
#else
        (void)definition;
        (void)name;
        (void)index;
        return out;
#endif
    }
    const LhatReflectedAbout *about = reflected_about(unit, definition, name);
    if (about != NULL && index < about->annotation_count) {
        const LhatReflectedAnnotation *a = &about->annotations[index];
        out.name = a->name.text;
        out.name_length = a->name.length;
        out.argument_count = a->argument_count;
        out.written = a;
        out.unit = unit;
    }
    return out;
}

LhatAnnotationArgument lhat_annotation_argument(LhatAnnotation annotation,
                                                size_t at)
{
    LhatAnnotationArgument out;
    memset(&out, 0, sizeof out);
    const LhatUnit *unit = (const LhatUnit *)annotation.unit;
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_argument(annotation, at);
#else
        (void)at;
        return out;
#endif
    }
    const LhatReflectedAnnotation *a =
        (const LhatReflectedAnnotation *)annotation.written;
    if (a != NULL && at < a->argument_count) {
        const LhatReflectedArgument *arg = &a->arguments[at];
        out.kind = arg->kind;
        out.number = arg->number;
        out.boolean = arg->boolean;
        out.text = arg->text.text;
        out.length = arg->text.length;
    }
    return out;
}

size_t lhat_unit_documentation(const LhatUnit *unit, const char *definition,
                               const char *name, char *out, size_t capacity)
{
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_documentation(unit, definition, name, out, capacity);
#else
        (void)definition;
        (void)name;
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
#endif
    }
    const LhatReflectedAbout *about = reflected_about(unit, definition, name);
    const char *text = about != NULL ? about->documentation.text : NULL;
    size_t length = text != NULL ? about->documentation.length : 0;
    if (out != NULL && capacity > 0) {
        size_t n = length < capacity - 1 ? length : capacity - 1;
        if (n > 0) {
            memcpy(out, text, n);
        }
        out[n] = '\0';
    }
    return length;
}

size_t lhat_unit_member_count(const LhatUnit *unit, const char *definition)
{
    if (unit != NULL && unit->binary) {
        const LhatReflectedBinding *binding =
            reflected_binding(unit, definition);
        return binding != NULL ? binding->member_count : 0;
    }
#if LHAT_WITH_FRONTEND
    return tree_member_count(unit, definition);
#else
    (void)definition;
    return 0;
#endif
}

LhatUnitMember lhat_unit_member(const LhatUnit *unit, const char *definition,
                                size_t index)
{
    LhatUnitMember out;
    memset(&out, 0, sizeof out);
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_member(unit, definition, index);
#else
        (void)definition;
        (void)index;
        return out;
#endif
    }
    const LhatReflectedMember *m =
        reflected_member(unit, definition, NULL, index);
    if (m != NULL) {
        out.name = m->name.text;
        out.name_length = m->name.length;
        out.declared = m->declared;
        out.empty_body = m->empty_body;
        out.parameter_count = m->parameter_count;
        out.type = m->type;
    }
    return out;
}

LhatUnitParameter lhat_unit_member_parameter(const LhatUnit *unit,
                                             const char *definition,
                                             size_t member, size_t at)
{
    LhatUnitParameter out;
    memset(&out, 0, sizeof out);
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_member_parameter(unit, definition, member, at);
#else
        (void)definition;
        (void)member;
        (void)at;
        return out;
#endif
    }
    const LhatReflectedMember *m =
        reflected_member(unit, definition, NULL, member);
    if (m != NULL && at < m->parameter_count) {
        const LhatReflectedParameter *p = &m->parameters[at];
        out.name = p->name.text;
        out.name_length = p->name.length;
        out.type = p->type;
        out.variadic = p->variadic;
    }
    return out;
}

size_t lhat_unit_member_written_name_count(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member)
{
    if (unit != NULL && unit->binary) {
        const LhatReflectedMember *m =
            reflected_member(unit, definition, NULL, member);
        return m != NULL ? m->written_count : 0;
    }
#if LHAT_WITH_FRONTEND
    return tree_written_name_count(unit, definition, member);
#else
    (void)definition;
    (void)member;
    return 0;
#endif
}

LhatUnitText lhat_unit_member_written_name(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member, size_t at)
{
    LhatUnitText out;
    out.text = NULL;
    out.length = 0;
    if (unit == NULL || !unit->binary) {
#if LHAT_WITH_FRONTEND
        return tree_written_name(unit, definition, member, at);
#else
        (void)definition;
        (void)member;
        (void)at;
        return out;
#endif
    }
    const LhatReflectedMember *m =
        reflected_member(unit, definition, NULL, member);
    if (m != NULL && at < m->written_count) {
        out.text = m->written_names[at].text;
        out.length = m->written_names[at].length;
    }
    return out;
}

// ---------------------------------------------------------------------------
// [modules.public.host]: what the unit published, as types
// ---------------------------------------------------------------------------

static const LhatTypeMember *export_named(const LhatUnit *unit,
                                          const char *name)
{
    if (unit == NULL || !unit->loaded || unit->checked.exports == NULL ||
        name == NULL) {
        return NULL;
    }
    size_t length = strlen(name);
    for (const LhatTypeMember *m = unit->checked.exports->v.table.members;
         m != NULL; m = m->next) {
        if (m->name_length == length && memcmp(m->name, name, length) == 0) {
            return m;
        }
    }
    return NULL;
}

// [modules.binary]: a binary unit answers off the descriptors it carried; the
// index of a name in them is what lhat_unit_export_type reads by.
static size_t binary_export_index(const LhatUnit *unit, const char *name)
{
    for (size_t i = 0; unit->export_names != NULL && i < unit->export_rt_count;
         i++) {
        if (strcmp(unit->export_names[i], name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

size_t lhat_unit_export_count(const LhatUnit *unit)
{
    size_t count = 0;
    if (unit != NULL && unit->binary) {
        return unit->export_rt_count;
    }
    if (unit != NULL && unit->loaded && unit->checked.exports != NULL) {
        for (const LhatTypeMember *m = unit->checked.exports->v.table.members;
             m != NULL; m = m->next) {
            count++;
        }
    }
    return count;
}

LhatUnitText lhat_unit_export_name(const LhatUnit *unit, size_t index)
{
    LhatUnitText text;
    text.text = NULL;
    text.length = 0;
    if (unit != NULL && unit->binary) {
        if (index < unit->export_rt_count) {
            text.text = unit->export_names[index];
            text.length = strlen(text.text);
        }
        return text;
    }
    if (unit == NULL || !unit->loaded || unit->checked.exports == NULL) {
        return text;
    }
    for (const LhatTypeMember *m = unit->checked.exports->v.table.members;
         m != NULL; m = m->next, index--) {
        if (index == 0) {
            text.text = m->name;
            text.length = m->name_length;
            return text;
        }
    }
    return text;
}

const struct LhatRuntimeType *lhat_unit_export_type(const LhatUnit *unit,
                                                    const char *name)
{
    if (unit != NULL && unit->binary && name != NULL) {
        size_t at = binary_export_index(unit, name);
        return at != SIZE_MAX ? unit->export_rt[at] : NULL;
    }
    const LhatTypeMember *m = export_named(unit, name);
    if (m == NULL || unit->proto == NULL) {
        return NULL;
    }
    size_t count = 0;
    size_t at = SIZE_MAX;
    for (const LhatTypeMember *walk = unit->checked.exports->v.table.members;
         walk != NULL; walk = walk->next) {
        if (walk == m) {
            at = count;
        }
        count++;
    }
    // The cache fill is the answering -- logically const, as a member
    // cache's is.
    LhatUnit *filling = (LhatUnit *)unit;
    if (filling->export_rt == NULL) {
        filling->export_rt = (struct LhatRuntimeType **)lhat_calloc(
            count, sizeof *filling->export_rt);
        if (filling->export_rt == NULL) {
            return NULL;
        }
        filling->export_rt_count = count;
    }
    if (at >= filling->export_rt_count) {
        return NULL;
    }
    if (filling->export_rt[at] == NULL) {
        filling->export_rt[at] =
            lhat_rt_from_checked(&unit->proto->chunk.heap, m->type, NULL);
    }
    return filling->export_rt[at];
}

size_t lhat_unit_export_type_text(const LhatUnit *unit, const char *name,
                                  char *out, size_t capacity)
{
    if (unit != NULL && unit->binary) {
        const struct LhatRuntimeType *rt = lhat_unit_export_type(unit, name);
        if (rt == NULL) {
            if (out != NULL && capacity > 0) {
                out[0] = '\0';
            }
            return SIZE_MAX;
        }
        return lhat_runtime_type_write(rt, out, capacity);
    }
    const LhatTypeMember *m = export_named(unit, name);
    if (m == NULL) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return SIZE_MAX;
    }
    return lhat_type_write_full(m->type, out, capacity);
}

#if LHAT_WITH_FRONTEND
bool lhat_unit_export_conforms(const LhatUnit *unit, const char *name,
                               const char *signature)
{
    // [modules.binary]: a binary unit carries no checker type to conform, and
    // this question is the checker's -- it answers no.
    const LhatTypeMember *m = export_named(unit, name);
    if (m == NULL || signature == NULL || unit->program == NULL) {
        return false;
    }
    // [modules.host.import]: read the way a registration's signature is, against what the
    // program registered -- so a host type may be named.
    LhatProgram *program = unit->program;
    const LhatType *wanted = lhat_type_of_text(signature, strlen(signature),
                                               &program->types,
                                               program->hosted, NULL);
    return wanted != NULL && lhat_type_conforms(m->type, wanted);
}
#else
bool lhat_unit_export_conforms(const LhatUnit *unit, const char *name,
                               const char *signature)
{
    (void)unit;
    (void)name;
    (void)signature;
    return false;  // [modules.binary.vm-only]: the checker's question, and there is no checker
}
#endif  // LHAT_WITH_FRONTEND

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// [errors.runtime.traceback]: the unit's path, stamped onto every proto of its tree so a
// traceback can name the file a frame belongs to. Debug only -- a copy per
// proto, freed with it.
void prg_stamp_source(LhatProto *proto, const char *path)
{
    if (proto == NULL || path == NULL) {
        return;
    }
    size_t length = strlen(path);
    proto->source_name = (char *)lhat_alloc(length + 1);
    if (proto->source_name != NULL) {
        memcpy(proto->source_name, path, length + 1);
    }
    for (size_t i = 0; i < proto->proto_count; i++) {
        prg_stamp_source(proto->protos[i], path);
    }
}

// The second pass: a unit's table is filled once every unit of the batch
// has its proto, since the list is in the order the units were reached and
// not in dependency order. Takes the list the first pass built.
bool prg_fill_unit_table(LhatUnit *u)
{
    const LhatProto **protos = NULL;
    if (u->referenced_count > 0) {
        protos = (const LhatProto **)lhat_alloc(u->referenced_count *
                                                sizeof *protos);
        if (protos == NULL) {
            return false;
        }
        for (size_t i = 0; i < u->referenced_count; i++) {
            protos[i] = u->referenced[i]->proto;  // NULL where it never compiled
        }
    }
    if (!lhat_proto_give_units(u->proto, protos, u->referenced_count)) {
        lhat_free((void *)protos);
        return false;
    }
    // [modules.require.reload]: the list stays. It is the only record of which way the
    // graph runs, and lhat_program_invalidate reads it backwards to find
    // every unit a changed one reaches.
    return true;
}

#if LHAT_WITH_FRONTEND
static char *enum_require_path(const LhatProgram *program, const char *path, bool *out_of_memory)
{
    const char *root = program->project_root;
    if (root != NULL) {
        size_t n = strlen(root);
        const char *relative = NULL;
        if (n == 0 && !absolute_path(path, strlen(path))) relative = path;
        else if (n > 0 && strncmp(path, root, n) == 0 &&
                 (root[n - 1] == '/' || path[n] == '/'))
            relative = path + n + (root[n - 1] != '/');
        if (relative != NULL) {
            size_t length = strlen(relative);
            char *out = lhat_alloc(length + 2);
            if (out != NULL) { out[0] = '$'; memcpy(out + 1, relative, length + 1); }
            else *out_of_memory = true;
            return out;
        }
    }
    if (!absolute_path(path, strlen(path))) return NULL;
    char *out = prg_duplicate(path);
    if (out == NULL) *out_of_memory = true;
    return out;
}

// Compiles one checked unit into u->proto. `registers` is [modules.require.once]'s guard and
// registry write for a module^ unit -- off for a loaded one ([modules.require.load]), which
// answers its table to whoever called it and enters no registry. False
// leaves the failure where lhat_program_compile_failure reads it.
bool prg_compile_one(LhatProgram *program, LhatUnit *u, bool registers)
{
    Resolution resolution;
    resolution.program = program;
    resolution.requiring = u;

    LhatUnits units = {0};
    bool out_of_memory = false;
    char *enum_path = registers && u->checked.module_name != NULL
        ? enum_require_path(program, u->path, &out_of_memory) : NULL;
    if (out_of_memory) {
        program->compile_status = LHAT_COMPILE_OUT_OF_MEMORY;
        program->compile_unit = u;
        memset(&program->compile_result, 0, sizeof program->compile_result);
        program->compile_result.status = LHAT_COMPILE_OUT_OF_MEMORY;
        return false;
    }
    units.enum_require_path = enum_path;
    units.resolve = resolve_unit;
    units.context = &resolution;
    units.module_name = u->checked.module_name;
    units.registers = registers;
        units.host_errors = program->host_error_entries;  // [modules.host.import] の誤り版
        units.host_error_count = program->host_error_entry_count;
        units.host_types = program->host_type_entries;  // [modules.hostdata] の fits^ 版
        units.host_type_count = program->host_type_entry_count;
        units.hostvalue_types = program->hostvalue_type_entries;  // [modules.hostvalue]
        units.hostvalue_type_count = program->hostvalue_type_entry_count;

    lhat_free(u->referenced);
    u->referenced = NULL;
    u->referenced_count = 0;
    u->referenced_capacity = 0;
    LhatCompileResult compiled =
        lhat_compile_module(&u->checked, &u->lexer, &units, &u->proto);
    lhat_free(enum_path);
    if (compiled.status != LHAT_COMPILE_OK) {
        // Kept so the caller can say which form stopped it rather than
        // only that something did -- and which unit, since the position
        // in it indexes that unit's source and no other.
        program->compile_status = compiled.status;
        program->compile_result = compiled;
        program->compile_unit = u;
        return false;
    }
    prg_stamp_source(u->proto, u->path);
    return true;
}
#endif  // LHAT_WITH_FRONTEND

void lhat_program_set_lock(LhatProgram *program, LhatProgramLockFn lock,
                           LhatProgramLockFn unlock, void *context)
{
    if (program == NULL) {
        return;
    }
    program->lock = lock;
    program->unlock = unlock;
    program->lock_context = context;
}

// [modules.host.lock]: taken at the public entries and nowhere else, so the pair
// never has to nest. A program with no lock pays a branch.
void lhat_program_hold(LhatProgram *program)
{
    if (program != NULL && program->lock != NULL) {
        program->lock(program->lock_context);
    }
}

void lhat_program_release(LhatProgram *program)
{
    if (program != NULL && program->unlock != NULL) {
        program->unlock(program->lock_context);
    }
}

// ---------------------------------------------------------------------------
// [l10n.choose.program]: the language a program draws its messages in, and the catalogs it
// was handed.

// How close `tag` is to the language `wanted`: 2 for the same tag, 1 for the
// language alone (a `ja` catalog answers a `ja-JP` program), 0 for neither.
// Tags are matched without case, as [l10n.catalog.format] says.
static int tag_rank(const char *tag, const char *wanted)
{
    size_t i = 0;
    for (; tag[i] != '\0' && wanted[i] != '\0'; i++) {
        char a = tag[i] >= 'A' && tag[i] <= 'Z' ? (char)(tag[i] + 32) : tag[i];
        char b = wanted[i] >= 'A' && wanted[i] <= 'Z' ? (char)(wanted[i] + 32)
                                                      : wanted[i];
        if (a != b) {
            return 0;
        }
    }
    if (tag[i] == '\0' && wanted[i] == '\0') {
        return 2;
    }
    // `ja` answers `ja-JP`; `ja-JP` does not answer `ja`, and `jam` neither.
    return tag[i] == '\0' && wanted[i] == '-' ? 1 : 0;
}

// Where this language's catalog of this source goes: the one already held,
// or a new one at the end. NULL when there was no room for another.
static LhatCatalog *catalog_for(LhatProgram *program, const char *tag,
                                const char *source)
{
    for (size_t i = 0; i < program->catalog_count; i++) {
        LhatCatalog *at = &program->catalogs[i];
        if (tag_rank(at->tag, tag) == 2 && strcmp(at->source, source) == 0) {
            return at;
        }
    }
    if (program->catalog_count == program->catalog_capacity) {
        size_t grown =
            program->catalog_capacity != 0 ? program->catalog_capacity * 2 : 4;
        LhatCatalog *bigger = (LhatCatalog *)lhat_realloc(
            program->catalogs, grown * sizeof *bigger);
        if (bigger == NULL) {
            return NULL;
        }
        program->catalogs = bigger;
        program->catalog_capacity = grown;
    }
    LhatCatalog *fresh = &program->catalogs[program->catalog_count++];
    memset(fresh, 0, sizeof *fresh);
    return fresh;
}

size_t lhat_program_load_catalog(LhatProgram *program, const char *tag,
                                 const char *source,
                                 const LhatMessageEntry *english,
                                 size_t english_count, const char *text,
                                 size_t length)
{
    if (program == NULL || tag == NULL || source == NULL || text == NULL) {
        return 0;
    }
    lhat_program_hold(program);
    LhatCatalog *at = catalog_for(program, tag, source);
    size_t held = at != NULL ? lhat_catalog_load(at, tag, source, english,
                                                 english_count, text, length)
                             : 0;
    lhat_program_release(program);
    return held;
}

size_t lhat_program_load_language(LhatProgram *program, const char *tag,
                                  const char *source, const char *text,
                                  size_t length)
{
    return lhat_program_load_catalog(program, tag, source, NULL, 0, text,
                                     length);
}

bool lhat_program_set_language(LhatProgram *program, const char *tag)
{
    if (program == NULL) {
        return false;
    }
    lhat_program_hold(program);
    char *copy = NULL;
    bool ok = true;
    if (tag != NULL) {
        size_t room = strlen(tag) + 1;
        copy = (char *)lhat_alloc(room);
        ok = copy != NULL;
        if (ok) {
            memcpy(copy, tag, room);
        }
    }
    if (ok) {
        lhat_free(program->language);
        program->language = copy;
    }
    lhat_program_release(program);
    return ok;
}

const char *lhat_program_language(const LhatProgram *program)
{
    return program != NULL && program->language != NULL ? program->language
                                                        : "en";
}

// [l10n.principle.english]: the text to draw `id` from -- this program's language, or the
// English the build holds when that language has nothing for it. Read while
// a message is being written, which the lock is already around, so it takes
// none of its own.
const char *lhat_program_text(const LhatProgram *program, const char *id,
                              const char *english)
{
    if (program == NULL || program->language == NULL || id == NULL) {
        return english;
    }
    const char *found = NULL;
    int best = 0;
    for (size_t i = 0; i < program->catalog_count; i++) {
        const LhatCatalog *at = &program->catalogs[i];
        int rank = tag_rank(at->tag, program->language);
        if (rank <= best) {
            continue;  // a closer catalog already answered
        }
        const char *text = lhat_catalog_text(at, id);
        if (text != NULL) {
            found = text;
            best = rank;
        }
    }
    return found != NULL ? found : english;
}

// [lsp.hover]. Freed here rather than where they are made, since the program
// is what outlives them. The three counts are left alone: what they say is
// which registrations the cache was emptied against, and an empty cache
// against the current ones is exactly what this leaves behind.
void lhat_program_forget_completions(LhatProgram *program)
{
    if (program == NULL) {
        return;
    }
    if (program->completions != NULL) {
        for (size_t i = 0; i < LHAT_COMPLETION_REMEMBERED; i++) {
            lhat_free(program->completions[i].items);
        }
        lhat_free(program->completions);
        program->completions = NULL;
    }
}

// The compile itself. Called with the lock already held, by the entries
// that compile as part of something larger (a load, a reload).
bool prg_compile_all(LhatProgram *program)
{
    bool ok = true;
#if LHAT_WITH_FRONTEND
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (!u->loaded || u->state != LHAT_UNIT_DONE || u->proto != NULL) {
            continue;  // failed to check, or compiled by an earlier call
        }
        if (!prg_compile_one(program, u, true)) {
            ok = false;
            break;
        }
    }
#endif
    // What compiled gets its table either way: a unit the first pass made
    // is runnable on its own, and a host that stops at `false` loses
    // nothing by it.
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (u->proto != NULL && u->proto->units == NULL &&
            !prg_fill_unit_table(u)) {
            program->compile_status = LHAT_COMPILE_TOO_COMPLEX;
            ok = false;
        }
    }
    return ok;
}

bool lhat_program_compile(LhatProgram *program)
{
    if (program->syntax_only) return false;
    lhat_program_hold(program);
    bool ok = prg_compile_all(program);
    lhat_program_release(program);
    return ok;
}

bool lhat_program_on_dispose(LhatProgram *program, LhatProgramDisposeFn call,
                             void *context)
{
    if (program == NULL || call == NULL) {
        return false;
    }
    LHAT_GROW(program->disposals, program->disposal_count,
              program->disposal_capacity, 4, return false);
    program->disposals[program->disposal_count].call = call;
    program->disposals[program->disposal_count].context = context;
    program->disposal_count++;
    return true;
}

// ---------------------------------------------------------------------------
// What the stages reported
// ---------------------------------------------------------------------------

const LhatUnit *lhat_program_units(const LhatProgram *program)
{
    return program != NULL ? program->units : NULL;
}

const LhatUnit *lhat_unit_next(const LhatUnit *unit)
{
    return unit != NULL ? unit->next : NULL;
}

const LhatSource *lhat_unit_source(const LhatUnit *unit)
{
    return (unit != NULL && unit->loaded) ? &unit->source : NULL;
}

size_t lhat_unit_diagnostic_count(const LhatUnit *unit)
{
    if (unit == NULL || !unit->loaded) {
        return 0;
    }
    return unit->lexer.diagnostic_count + unit->parsed.diagnostic_count +
           unit->checked.diagnostic_count;
}

bool lhat_unit_stage_of(const LhatUnit *unit, size_t index, LhatStage *stage,
                        size_t *within)
{
    if (unit == NULL || !unit->loaded) {
        return false;
    }
    if (index < unit->lexer.diagnostic_count) {
        *stage = LHAT_STAGE_LEXER;
        *within = index;
        return true;
    }
    index -= unit->lexer.diagnostic_count;
    if (index < unit->parsed.diagnostic_count) {
        *stage = LHAT_STAGE_PARSER;
        *within = index;
        return true;
    }
    index -= unit->parsed.diagnostic_count;
    if (index < unit->checked.diagnostic_count) {
        *stage = LHAT_STAGE_CHECKER;
        *within = index;
        return true;
    }
    return false;
}

LhatUnitDiagnostic lhat_unit_diagnostic(const LhatUnit *unit, size_t index)
{
    LhatUnitDiagnostic out;
    memset(&out, 0, sizeof out);

    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!lhat_unit_stage_of(unit, index, &stage, &within)) {
        return out;
    }
    out.stage = stage;

    switch (stage) {
        case LHAT_STAGE_LEXER: {
            // The lexer points at a place: what went wrong is one byte in,
            // and there is no name to underline.
            const LhatDiagnostic *d = &unit->lexer.diagnostics[within];
            out.offset = d->offset;
            out.line = d->line;
            out.column = d->column;
            out.length = 0;
            break;
        }
        case LHAT_STAGE_PARSER: {
            const LhatParseDiagnostic *d = &unit->parsed.diagnostics[within];
            out.offset = d->offset;
            out.line = d->line;
            out.column = d->column;
            out.length = d->length;
            break;
        }
        case LHAT_STAGE_CHECKER: {
            // A diagnostic may mention a missing member while pointing at
            // the construction, so its source span can differ from the name.
            const LhatCheckDiagnostic *d = &unit->checked.diagnostics[within];
            out.offset = d->offset;
            out.line = d->line;
            out.column = d->column;
            out.length = d->highlight_length != 0 ? d->highlight_length : d->name_length;
            break;
        }
    }
    return out;
}

bool lhat_unit_diagnostic_relaxed_ok(const LhatUnit *unit, size_t index)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!lhat_unit_stage_of(unit, index, &stage, &within) || stage != LHAT_STAGE_CHECKER) {
        return false;
    }
    return unit->checked.diagnostics[within].relaxed_ok;
}

size_t lhat_unit_diagnostic_message(const LhatUnit *unit, size_t index,
                                    char *out, size_t capacity)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!lhat_unit_stage_of(unit, index, &stage, &within)) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }

#if !LHAT_WITH_FRONTEND
    // [modules.binary.vm-only]: a binary unit reports nothing of its own.
    (void)within;
    if (out != NULL && capacity > 0) {
        out[0] = '\0';
    }
    return 0;
#else
    switch (stage) {
        case LHAT_STAGE_LEXER: {
            // A literal, so it is copied rather than written.
            LhatErrorCode code = unit->lexer.diagnostics[within].code;
            const char *text =
                lhat_program_text(unit->program, lhat_lexer_error_id(code),
                                  lhat_lexer_error_message(code));
            size_t length = strlen(text);
            if (out != NULL && capacity > 0) {
                size_t fits = length < capacity - 1 ? length : capacity - 1;
                memcpy(out, text, fits);
                out[fits] = '\0';
            }
            return length;
        }
        case LHAT_STAGE_PARSER:
            return lhat_parse_message_write(
                unit->program, &unit->parsed.diagnostics[within], out,
                capacity);
        case LHAT_STAGE_CHECKER:
            return lhat_check_message_write(
                unit->program, &unit->checked.diagnostics[within], out,
                capacity);
    }
    return 0;
#endif
}

const char *lhat_unit_diagnostic_id(const LhatUnit *unit, size_t index)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!lhat_unit_stage_of(unit, index, &stage, &within)) {
        return NULL;
    }
#if !LHAT_WITH_FRONTEND
    (void)within;
    return NULL;  // [modules.binary.vm-only]: a binary unit reports nothing of its own
#else
    switch (stage) {
        case LHAT_STAGE_LEXER:
            return lhat_lexer_error_id(unit->lexer.diagnostics[within].code);
        case LHAT_STAGE_PARSER:
            return lhat_parse_message_id(&unit->parsed.diagnostics[within]);
        case LHAT_STAGE_CHECKER:
            return lhat_check_message_id(&unit->checked.diagnostics[within]);
    }
    return NULL;
#endif
}

size_t lhat_unit_diagnostic_write(const LhatUnit *unit, size_t index,
                                  bool rich, char *out, size_t capacity)
{
    // Wide enough for every message the stages actually write; the heap is
    // for the rare one that names something long.
    char room[512];
    char *message = room;
    size_t needed = lhat_unit_diagnostic_message(unit, index, room, sizeof room);
    if (needed >= sizeof room) {
        char *bigger = (char *)lhat_alloc(needed + 1);
        if (bigger != NULL) {
            lhat_unit_diagnostic_message(unit, index, bigger, needed + 1);
            message = bigger;
        }
    }

    LhatUnitDiagnostic d = lhat_unit_diagnostic(unit, index);
    LhatReport report;
    report.kind = unit->program != NULL && !unit->program->strict &&
                          lhat_unit_diagnostic_relaxed_ok(unit, index)
                      ? LHAT_REPORT_WARNING : LHAT_REPORT_ERROR;
    report.message = message;
    report.offset = d.offset;
    report.line = d.line;
    report.column = d.column;
    report.length = d.length;

    size_t written = lhat_report_write(unit->program, &report,
                                       lhat_unit_source(unit),
                                       lhat_unit_path(unit), rich, out,
                                       capacity);
    if (message != room) {
        lhat_free(message);
    }
    return written;
}

// The check itself, with the lock already held -- what a reload calls
// round after round.
const LhatUnit *prg_check_held(LhatProgram *program, const char *path)
{
    char *resolved = prg_resolve_against(program, NULL, path, strlen(path));
    if (resolved == NULL) {
        return NULL;
    }
    LhatUnit *unit = check_path(program, resolved);
    return (unit != NULL && unit->loaded) ? unit : NULL;
}

#if LHAT_WITH_FRONTEND
// [modules.require.once]: a unit named under another is a member of that unit's table in
// L^.modules, beside what that unit published -- so the name it would stand
// at may not be one the other publishes. Asked of the whole program, since no
// one check need have read both units in.
static void report_colliding_units(LhatProgram *program)
{
    for (const LhatUnit *child = program->units; child != NULL;
         child = child->next) {
        const char *name = child->module_name;
        const char *dot = name != NULL ? strrchr(name, '.') : NULL;
        if (dot == NULL) {
            continue;
        }
        size_t prefix = (size_t)(dot - name);
        for (const LhatUnit *parent = program->units; parent != NULL;
             parent = parent->next) {
            const char *above = parent->module_name;
            if (above == NULL || strlen(above) != prefix ||
                strncmp(above, name, prefix) != 0 ||
                lhat_type_find_member(parent->checked.exports, dot + 1,
                                      strlen(dot + 1)) == NULL) {
                continue;
            }
            bool said = false;
            for (size_t i = 0; i < program->diagnostic_count && !said; i++) {
                said = program->diagnostics[i].code ==
                           LHAT_PROGRAM_ERR_MODULE_COLLIDES &&
                       strcmp(program->diagnostics[i].path, child->path) == 0;
            }
            if (!said) {
                report(program, LHAT_PROGRAM_ERR_MODULE_COLLIDES, child->path);
            }
        }
    }
}
#endif

const LhatUnit *lhat_program_check(LhatProgram *program, const char *path)
{
    lhat_program_hold(program);
    const LhatUnit *unit = prg_check_held(program, path);
#if LHAT_WITH_FRONTEND
    report_colliding_units(program);
#endif
    lhat_program_release(program);
    return unit;
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
        if (u->lexer.diagnostic_count > 0 || u->parsed.diagnostic_count > 0) {
            return true;
        }
#if LHAT_WITH_FRONTEND
        if (lhat_check_error_count(&u->checked) > 0) {
            return true;
        }
#endif
    }
    return false;
}

// [l10n.id]: the ID and the English text of every LhatProgramErrorCode, indexed by
// the code. The ID is the one a translation is written against and never
// changes meaning; the English is the reference every other language translates
// ([l10n.principle.english]).
static const LhatMessageEntry PROGRAM_MESSAGES[] = {
    [LHAT_PROGRAM_ERR_CANNOT_READ] =
        {"program.cannot-read", "this unit could not be read"},
    [LHAT_PROGRAM_ERR_CYCLE] = {"program.cycle",
        "the units require each other; move the common part out"},
    [LHAT_PROGRAM_ERR_BAD_BINARY] = {"program.bad-binary",
        "this binary unit was not written by this build of the "
        "library, or has been damaged"},
    [LHAT_PROGRAM_ERR_HOST_MISMATCH] = {"program.host-mismatch",
        "this binary unit names a host type, kind or enum that "
        "this program did not register the same way"},
    [LHAT_PROGRAM_ERR_MIXED] = {"program.mixed",
        "a program is text or binary throughout; this unit is "
        "the other kind"},
    [LHAT_PROGRAM_ERR_MODULE_COLLIDES] = {"program.module-collides",
        "this unit is named under another unit, which already publishes "
        "the name it would stand at"},
    [LHAT_PROGRAM_ERR_NO_SIGNATURE] = {"program.no-signature",
        "this build has no front end to read a signature with, "
        "and the signature table does not hold this one"},
    [LHAT_PROGRAM_ERR_NO_FRONTEND] = {"program.no-frontend",
        "this build has no front end; only a binary unit runs"},
};

const char *lhat_program_error_message(LhatProgramErrorCode code)
{
    const LhatMessageEntry *entry = LHAT_MESSAGE_AT(PROGRAM_MESSAGES, code);
    return entry != NULL ? entry->text : "unknown error";
}

LHAT_MESSAGE_TABLES(lhat_program_message_tables,
    {PROGRAM_MESSAGES, LHAT_MESSAGE_COUNT(PROGRAM_MESSAGES)},
    {prg_program_parts, LHAT_MESSAGE_COUNT(prg_program_parts)})

const char *lhat_program_error_id(LhatProgramErrorCode code)
{
    const LhatMessageEntry *entry = LHAT_MESSAGE_AT(PROGRAM_MESSAGES, code);
    return entry != NULL ? entry->id : NULL;
}

// ---------------------------------------------------------------------------
// [modules.binary]: what serialize.c asks of the program
// ---------------------------------------------------------------------------

LhatUnit *lhat_program_require_unit(LhatProgram *program, LhatUnit *from,
                                    const char *relative, size_t length)
{
    char *resolved = prg_resolve_against(from->program, from->path, relative, length);
    if (resolved == NULL) {
        return NULL;
    }
    LhatUnit *unit = check_path(program, resolved);  // takes `resolved`
    if (unit == NULL) {
        return NULL;
    }
    (void)remember_edge(from, unit);
    return unit;
}

char *lhat_program_resolve_path(const LhatUnit *from, const char *relative,
                                size_t length)
{
    return prg_resolve_against(from->program, from->path, relative, length);
}

void lhat_program_report(LhatProgram *program, LhatProgramErrorCode code,
                         const char *path)
{
    report(program, code, path);
}

const LhatType *lhat_program_enum_identity(LhatProgram *program,
                                           const char *path, const char *name)
{
    for (size_t i = 0; i < program->enum_identity_count; i++) {
        const LhatEnumIdentity *e = &program->enum_identities[i];
        if (strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
            return e->decl;
        }
    }
    LHAT_GROW(program->enum_identities, program->enum_identity_count,
              program->enum_identity_capacity, 4, return NULL);
    LhatEnumIdentity *e =
        &program->enum_identities[program->enum_identity_count];
    e->path = prg_duplicate(path);
    e->name = prg_duplicate(name);
    // The declaration's name is borrowed by the type (type.h), so it is
    // the owned copy that is handed over.
    e->decl = e->path != NULL && e->name != NULL
                  ? lhat_type_enum_decl(&program->types, e->name,
                                        strlen(e->name))
                  : NULL;
    if (e->decl == NULL) {
        lhat_free(e->path);
        lhat_free(e->name);
        return NULL;
    }
    program->enum_identity_count++;
    return e->decl;
}

LhatWriteStatus lhat_unit_write_binary(const LhatUnit *unit,
                                        bool with_debug_names,
                                        uint8_t **bytes, size_t *length)
{
#if LHAT_WITH_FRONTEND
    return lhat_serialize_write(unit, with_debug_names, bytes, length);
#else
    (void)unit;
    (void)with_debug_names;
    (void)bytes;
    (void)length;
    return LHAT_WRITE_NO_FRONTEND;  // [modules.binary.vm-only]: what it was written from is gone
#endif
}

bool lhat_program_write_signatures(const LhatProgram *program,
                                   uint8_t **bytes, size_t *length)
{
    return lhat_serialize_write_signatures(program, bytes, length);
}

bool lhat_program_read_signatures(LhatProgram *program, const uint8_t *bytes,
                                  size_t length)
{
    if (program == NULL || bytes == NULL) {
        return false;
    }
    uint8_t *copy = (uint8_t *)lhat_alloc(length ? length : 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, bytes, length);
    LhatSignatureIndex *index = NULL;
    size_t count = 0;
    if (!lhat_serialize_index_signatures(program, copy, length, &index,
                                         &count)) {
        lhat_free(copy);
        return false;
    }
    lhat_free(program->signature_index);
    lhat_free(program->signatures);
    program->signatures = copy;
    program->signature_length = length;
    program->signature_index = index;
    program->signature_count = count;
    return true;
}

const LhatSignatureIndex *prg_find_signature(const LhatProgram *program,
                                                const char *text)
{
    if (program == NULL || text == NULL || program->signature_count == 0) {
        return NULL;
    }
    size_t low = 0;
    size_t high = program->signature_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int order = strcmp(program->signature_index[mid].text, text);
        if (order == 0) {
            return &program->signature_index[mid];
        }
        if (order < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return NULL;
}

const LhatRuntimeType *lhat_program_signature_type(LhatProgram *program,
                                                   const char *text)
{
    const LhatSignatureIndex *e = prg_find_signature(program, text);
    return e != NULL ? lhat_serialize_read_signature(
                           program, program->signatures + e->body_offset,
                           e->body_length)
                     : NULL;
}

// ---------------------------------------------------------------------------
// Dumping the registrations (lhat-host.json)
// ---------------------------------------------------------------------------
//
// The JSON is written by hand rather than through a JSON library: the core
// depends on nothing but the C library, and what has to be written is one
// fixed shape. The same moving cursor error.c writes reports with.

typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} DumpWriter;

static void dump_put(DumpWriter *w, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (w->out != NULL && w->used + 1 < w->capacity) {
            w->out[w->used] = text[i];
        }
        w->used++;
    }
}

static void dump_text(DumpWriter *w, const char *text)
{
    dump_put(w, text, strlen(text));
}

// A JSON string literal. Signatures and names are plain ASCII in practice,
// but '"' and '\' still have to be escaped for the output to stay JSON at
// all, and a control character (which nothing registered should carry)
// must not pass through raw.
static void dump_string(DumpWriter *w, const char *text)
{
    dump_put(w, "\"", 1);
    for (const char *p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dump_put(w, "\\", 1);
            dump_put(w, (const char *)p, 1);
        } else if (c < 0x20) {
            char hex[7];
            snprintf(hex, sizeof hex, "\\u%04x", c);
            dump_text(w, hex);
        } else {
            dump_put(w, (const char *)p, 1);
        }
    }
    dump_put(w, "\"", 1);
}

static void dump_number(DumpWriter *w, size_t value)
{
    char digits[32];
    snprintf(digits, sizeof digits, "%zu", value);
    dump_text(w, digits);
}

static const char *hostvalue_field_kind_name(LhatHostValueFieldKind kind)
{
    switch (kind) {
        case LHAT_HVFIELD_F32: return "f32";
        case LHAT_HVFIELD_F64: return "f64";
        case LHAT_HVFIELD_I8:  return "i8";
        case LHAT_HVFIELD_I16: return "i16";
        case LHAT_HVFIELD_I32: return "i32";
        case LHAT_HVFIELD_I64: return "i64";
        case LHAT_HVFIELD_U8:  return "u8";
        case LHAT_HVFIELD_U16: return "u16";
        case LHAT_HVFIELD_U32: return "u32";
    }
    return "?";
}

static const LhatHostValueTypeEntry *find_hostvalue_type(
    const LhatProgram *program, const char *module, const char *name)
{
    for (size_t i = 0; i < program->hostvalue_type_entry_count; i++) {
        const LhatHostValueTypeEntry *entry =
            &program->hostvalue_type_entries[i];
        if (strcmp(entry->module, module) == 0 &&
            strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

// Writes ",\n" between array items: `first` starts true and this flips it.
static void dump_comma(DumpWriter *w, bool *first)
{
    if (!*first) {
        dump_text(w, ",\n");
    } else {
        dump_text(w, "\n");
        *first = false;
    }
}

// [syntax.annot.host]'s places, spelled. Has to be kept in step with
// LhatAnnotationTarget in lhat/object.h -- this is the only place a new one
// would have to be named, so it is the only place that would notice.
static const struct {
    uint32_t bit;
    const char *name;
} annotation_targets[] = {
    {LHAT_ANNOTATION_UNIT, "unit"},
    {LHAT_ANNOTATION_BINDING, "binding"},
    {LHAT_ANNOTATION_FIELD, "field"},
    {LHAT_ANNOTATION_MEMBER, "member"},
    {LHAT_ANNOTATION_PUBLIC, "public"},
    {LHAT_ANNOTATION_FILEUNIQUE, "fileunique"},  // a count, not a place
};

// A mask reads as a mask only to something holding the same enum, and the
// reader is a language server that holds none of C's. So the places are
// written out: the ones that are set, each true, and nothing for the rest.
//
// A reader takes `false` too and passes over it, and passes over a name it
// does not know -- so a file written by a later dump, with a place this one
// never heard of, still reads as far as it goes.
static void dump_targets(DumpWriter *w, uint32_t targets)
{
    dump_text(w, "{");
    bool first = true;
    for (size_t i = 0;
         i < sizeof annotation_targets / sizeof annotation_targets[0]; i++) {
        if ((targets & annotation_targets[i].bit) == 0) {
            continue;
        }
        if (!first) {
            dump_text(w, ", ");
        }
        first = false;
        dump_string(w, annotation_targets[i].name);
        dump_text(w, ": true");
    }
    dump_text(w, "}");
}

size_t lhat_program_dump_host_api(const LhatProgram *program, char *out,
                                  size_t capacity)
{
    DumpWriter w;
    w.out = out;
    w.capacity = capacity;
    w.used = 0;

    dump_text(&w, "{\n  \"strict\": ");
    dump_text(&w, program->strict ? "true" : "false");
    dump_text(&w, ",\n  \"types\": [");
    bool first = true;

    // Error kinds first: their variants are names a signature writes
    // qualified ("std.io.IOError.Eof"), the same standing as a type's.
    for (size_t i = 0; i < program->host_error_entry_count; i++) {
        const LhatHostErrorKind *entry = &program->host_error_entries[i];
        dump_comma(&w, &first);
        dump_text(&w, "    {\"kind\": \"errordef\", \"module\": ");
        dump_string(&w, entry->module);
        dump_text(&w, ", \"name\": ");
        dump_string(&w, entry->name);
        dump_text(&w, ", \"variants\": [");
        for (size_t v = 0; v < entry->variant_count; v++) {
            if (v > 0) {
                dump_text(&w, ", ");
            }
            dump_string(&w, entry->variant_names[v]);
        }
        dump_text(&w, "]}");
    }

    // Type declarations: a host entry with no call is one. Whether it was a
    // hostdata or a hostvalue registration is what the entry's tag says --
    // only lhat_register_hostdata_type puts one on the entry; a hostvalue
    // type's tag lives in hostvalue_type_entries instead.
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *entry = &program->host_entries[i];
        if (entry->call != NULL || entry->type != NULL ||
            entry->const_kind != LHAT_HOST_CONST_NONE) {
            continue;
        }
        dump_comma(&w, &first);
        if (entry->tag != NULL) {
            dump_text(&w, "    {\"kind\": \"hostdata\", \"module\": ");
            dump_string(&w, entry->module);
            dump_text(&w, ", \"name\": ");
            dump_string(&w, entry->name);
            // [modules.hostdata.base]: a host whose model is a class tree writes the tree
            // rather than flattening it, and what a derived type inherits is
            // reached through this link -- so a dump without it describes a
            // Sprite2D that has none of Object's members, and every reader
            // of the dump (the language server among them) says so.
            if (entry->tag->base != NULL) {
                dump_text(&w, ", \"base_module\": ");
                dump_string(&w, entry->tag->base->module);
                dump_text(&w, ", \"base_name\": ");
                dump_string(&w, entry->tag->base->name);
            }
            dump_text(&w, "}");
            continue;
        }
        const LhatHostValueTypeEntry *value_type =
            find_hostvalue_type(program, entry->module, entry->name);
        if (value_type == NULL) {
            // Unreachable today -- every call-less, type-less entry comes
            // from one of the two type registrations -- but writing nothing
            // beats writing a lie if a third kind ever appears.
            dump_text(&w, "    {}");
            continue;
        }
        dump_text(&w, "    {\"kind\": \"hostvalue\", \"module\": ");
        dump_string(&w, entry->module);
        dump_text(&w, ", \"name\": ");
        dump_string(&w, entry->name);
        dump_text(&w, ", \"size\": ");
        dump_number(&w, value_type->tag->size);
        dump_text(&w, ", \"fields\": [");
        for (size_t f = 0; f < value_type->tag->field_count; f++) {
            const LhatHostValueField *field = &value_type->tag->fields[f];
            if (f > 0) {
                dump_text(&w, ", ");
            }
            dump_text(&w, "{\"name\": ");
            dump_string(&w, field->name);
            dump_text(&w, ", \"offset\": ");
            dump_number(&w, field->offset);
            dump_text(&w, ", \"type\": ");
            dump_string(&w, hostvalue_field_kind_name(field->kind));
            dump_text(&w, "}");
        }
        dump_text(&w, "]}");
    }
    // [modules.host.no-string-lookup]: an enum is a declaration the signatures name, the same
    // standing as a type's -- and one may stand under a host type
    // ("godot.Node.InternalMode"), so the types come first and the enums
    // close the section. A reader registering in file order then never
    // meets a name it has not seen, which the dump test pins.
    for (size_t i = 0; i < program->host_enum_count; i++) {
        const LhatProgramEnum *e = &program->host_enums[i];
        dump_comma(&w, &first);
        dump_text(&w, "    {\"kind\": \"enum\", \"module\": ");
        dump_string(&w, e->module);
        if (e->type != NULL) {
            dump_text(&w, ", \"type\": ");
            dump_string(&w, e->type);
        }
        dump_text(&w, ", \"name\": ");
        dump_string(&w, e->name);
        dump_text(&w, ", \"members\": [");
        for (size_t k = 0; k < e->count; k++) {
            if (k > 0) {
                dump_text(&w, ", ");
            }
            dump_string(&w, e->members[k]);
        }
        dump_text(&w, "], \"values\": [");
        char number[32];
        for (size_t k = 0; k < e->count; k++) {
            snprintf(number, sizeof number, k > 0 ? ", %lld" : "%lld",
                     (long long)e->values[k]);
            dump_text(&w, number);
        }
        dump_text(&w, "]}");
    }
    dump_text(&w, first ? "],\n" : "\n  ],\n");

    dump_text(&w, "  \"functions\": [");
    first = true;
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *entry = &program->host_entries[i];
        if (entry->call == NULL &&
            entry->const_kind == LHAT_HOST_CONST_NONE) {
            continue;
        }
        dump_comma(&w, &first);
        if (entry->const_kind != LHAT_HOST_CONST_NONE) {
            dump_text(&w, "    {\"kind\": \"const\", \"module\": ");
            dump_string(&w, entry->module);
            if (entry->type != NULL) {
                dump_text(&w, ", \"type\": ");
                dump_string(&w, entry->type);
            }
            dump_text(&w, ", \"name\": ");
            dump_string(&w, entry->name);
            char number[64];
            switch (entry->const_kind) {
                case LHAT_HOST_CONST_INTEGER:
                    // JSON has one kind of number, and a reader holding it as
                    // a double (lsp/host_config.c's cJSON) keeps an integer
                    // exactly only within 2^53 -- past that the digits go out
                    // as a string, which the reader takes back as they are.
                    if (entry->const_integer >= -(INT64_C(1) << 53) &&
                        entry->const_integer <= (INT64_C(1) << 53)) {
                        snprintf(number, sizeof number,
                                 ", \"value_kind\": \"integer\", \"value\": %lld",
                                 (long long)entry->const_integer);
                    } else {
                        snprintf(number, sizeof number,
                                 ", \"value_kind\": \"integer\", \"value\": \"%lld\"",
                                 (long long)entry->const_integer);
                    }
                    dump_text(&w, number);
                    break;
                case LHAT_HOST_CONST_REAL:
                    // JSON has no spelling for the infinities or NaN, so
                    // those go out as strings. Asked rather than left to
                    // printf, which spells a NaN differently per C library
                    // ("-nan(ind)" on MSVC); a NaN's sign is not kept.
                    if (isnan(entry->const_real)) {
                        dump_text(&w, ", \"value_kind\": \"real\", \"value\": \"nan\"");
                    } else if (isinf(entry->const_real)) {
                        dump_text(&w, entry->const_real > 0
                                          ? ", \"value_kind\": \"real\", \"value\": \"inf\""
                                          : ", \"value_kind\": \"real\", \"value\": \"-inf\"");
                    } else {
                        snprintf(number, sizeof number,
                                 ", \"value_kind\": \"real\", \"value\": %.17g",
                                 entry->const_real);
                        dump_text(&w, number);
                    }
                    break;
                case LHAT_HOST_CONST_BOOL:
                    dump_text(&w, entry->const_bool
                                      ? ", \"value_kind\": \"bool\", "
                                        "\"value\": true"
                                      : ", \"value_kind\": \"bool\", "
                                        "\"value\": false");
                    break;
                default:
                    dump_text(&w, ", \"value_kind\": \"string\", "
                                  "\"value\": ");
                    dump_string(&w, entry->const_text);
                    break;
            }
            dump_text(&w, "}");
            continue;
        }
        if (entry->type == NULL) {
            dump_text(&w, "    {\"kind\": \"func\", \"module\": ");
            dump_string(&w, entry->module);
        } else {
            bool hostvalue = find_hostvalue_type(program, entry->module,
                                                 entry->type) != NULL;
            dump_text(&w, hostvalue
                              ? "    {\"kind\": \"hostvalue_member\", \"module\": "
                              : "    {\"kind\": \"member\", \"module\": ");
            dump_string(&w, entry->module);
            dump_text(&w, ", \"type\": ");
            dump_string(&w, entry->type);
        }
        dump_text(&w, ", \"name\": ");
        dump_string(&w, entry->name);
        dump_text(&w, ", \"signature\": ");
        dump_string(&w, entry->signature_text != NULL ? entry->signature_text
                                                      : "");
        dump_text(&w, "}");
    }
    for (size_t i = 0; i < program->global_count; i++) {
        const LhatGlobalEntry *entry = &program->global_entries[i];
        dump_comma(&w, &first);
        dump_text(&w, "    {\"kind\": \"global\", \"name\": ");
        dump_string(&w, entry->name);
        dump_text(&w, ", \"signature\": ");
        dump_string(&w, entry->signature_text != NULL ? entry->signature_text
                                                      : "");
        dump_text(&w, "}");
    }
    dump_text(&w, first ? "],\n" : "\n  ],\n");

    // [syntax.annot.host]: what a program may write as an annotation. The language
    // server has no way to run this host's C, so what it registered is told
    // here the way its types and functions are.
    dump_text(&w, "  \"annotations\": [");
    first = true;
    for (size_t i = 0; i < program->annotation_count; i++) {
        const LhatAnnotationDecl *decl = &program->annotations[i];
        dump_comma(&w, &first);
        dump_text(&w, "    {\"module\": ");
        dump_string(&w, decl->module);
        dump_text(&w, ", \"name\": ");
        dump_string(&w, decl->name);
        dump_text(&w, ", \"targets\": ");
        dump_targets(&w, decl->targets);
        if (program->annotation_signatures[i] != NULL) {
            dump_text(&w, ", \"signature\": ");
            dump_string(&w, program->annotation_signatures[i]);
        }
        // [syntax.annot.host.two-answers]: written as the names themselves rather than as anything
        // resolved, since that is how the registration said them and a
        // reader hands them back the same way.
        if (decl->exclusive_count > 0) {
            dump_text(&w, ", \"exclusives\": [");
            for (size_t k = 0; k < decl->exclusive_count; k++) {
                if (k > 0) {
                    dump_text(&w, ", ");
                }
                dump_string(&w, decl->exclusives[k]);
            }
            dump_text(&w, "]");
        }
        // [syntax.annot.host.alone]: the same, and read back the same way. Both columns are
        // left out where nothing was said, so a registration that has only
        // targets to give writes only targets.
        if (decl->requisite_count > 0) {
            dump_text(&w, ", \"requisites\": [");
            for (size_t k = 0; k < decl->requisite_count; k++) {
                if (k > 0) {
                    dump_text(&w, ", ");
                }
                dump_string(&w, decl->requisites[k]);
            }
            dump_text(&w, "]");
        }
        dump_text(&w, "}");
    }
    dump_text(&w, first ? "],\n" : "\n  ],\n");

    dump_text(&w, "  \"bindings\": [");
    first = true;
    for (size_t i = 0; i < program->initial_count; i++) {
        dump_comma(&w, &first);
        dump_text(&w, "    {\"name\": ");
        dump_string(&w, program->initial_names[i]);
        // Written back in the spelling lhat_bind_initial takes, prefix and
        // all, so a reader hands the string straight back to it.
        dump_text(&w, ", \"member\": \"L^.");
        // The stored member has the "L^." prefix already stripped
        // (lhat_bind_initial keeps only what it reached), so it is a bare
        // member name with nothing in it to escape beyond what dump_string
        // handles.
        for (const char *p = program->initial_members[i]; *p != '\0'; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                dump_put(&w, "\\", 1);
            }
            dump_put(&w, (const char *)p, 1);
        }
        dump_text(&w, "\"}");
    }
    dump_text(&w, first ? "]\n" : "\n  ]\n");
    dump_text(&w, "}\n");

    if (w.out != NULL && w.capacity > 0) {
        w.out[w.used < w.capacity ? w.used : w.capacity - 1] = '\0';
    }
    return w.used;
}
