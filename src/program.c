// L^ (lhat) -- a program: one unit and everything it requires.

#include "program_internal.h"
#include "serialize.h"
#include "registry.h"

#include <stdio.h>  // snprintf: lhat_program_dump_host_api's numbers
#include <stdlib.h>
#include <string.h>

#include "compile.h"  // 05 の 5.3: the units are compiled here too
#include "gc.h"  // LHAT_GC_BLACK -- host_heap の初期色 (04 の 12.4)
#include "grow.h"
#include "lhat/error.h"  // one shape for what a stage reported (03 の 1.1)
#include "lhat/port.h"
#include "rttype.h"
#include "type.h"
#include "lhat/vm.h"

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
static const LhatSignatureIndex *find_signature(const LhatProgram *program,
                                                const char *text);
#if LHAT_WITH_FRONTEND
static void check_parsed(LhatProgram *program, LhatUnit *unit,
                         LhatTypeArena *arena);
#endif
// 05 の 8.8改: gives every derived host type what its base registered. Both
// of the two ways checking starts call it; it does its work once.
static void flatten_hostdata_bases(LhatProgram *program);
static void stamp_source(LhatProto *proto, const char *path);

// 05 の 5.3: `from` require^s `to`. Answers the position in `from`'s list,
// which is the number the compiler writes into a UNIT instruction, or
// LHAT_NO_UNIT when there is no room.
//
// 5.7 reads these backwards to find every unit a changed one reaches, and
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
    if (unit == NULL) {
        return NULL;
    }
    // 5.7: recorded before the state is judged. A require^ that reached a
    // unit which would not read is still an edge -- when that file appears
    // and is invalidated, this unit is what has to be checked again.
    (void)remember_edge(r->requiring, unit);
    if (unit->state != LHAT_UNIT_DONE) {
        return NULL;
    }
    // 05 の 10 章: a text unit reaching a binary one has no exports to read
    // -- a program is one or the other.
    if (unit->binary) {
        report(r->program, LHAT_PROGRAM_ERR_MIXED, unit->path);
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

// 05 の 5.7: FNV-1a over a unit's text, so that a save which changed nothing
// can be told from one that did. Not a security question -- a collision costs
// a reload that was not needed, or misses one that was, and the host may
// always invalidate a path it knows changed.
static uint64_t hash_text(const char *text, size_t length)
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
// checking. Shared by a unit met for the first time and by one 5.7 made
// stale, which is the whole reason it stands apart from check_path.
static void load_into(LhatProgram *program, LhatUnit *unit)
{
    unit->state = LHAT_UNIT_CHECKING;

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
        return;
    }

    unit->source_hash = hash_text(text, length);

    // 05 の 10 章: the bytes say which they are. A binary unit is built
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
        stamp_source(read.proto, unit->path);
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
    check_parsed(program, unit, &program->types);
#else
    // 05 の 10.8: text, and nothing here reads text.
    lhat_free(text);
    report(program, LHAT_PROGRAM_ERR_NO_FRONTEND, unit->path);
    unit->state = LHAT_UNIT_FAILED;
#endif
}

// Takes ownership of `path`.
static LhatUnit *check_path(LhatProgram *program, char *path)
{
    // 05 の 8.8改: registration has closed by the time anything is checked,
    // so this is where a host type comes to carry its base's members.
    flatten_hostdata_bases(program);

    LhatUnit *existing = find_unit(program, path);
    if (existing != NULL) {
        // 6.3: meeting a unit that is still being checked means the graph
        // has a cycle. Reported here, where both ends are known.
        if (existing->state == LHAT_UNIT_CHECKING) {
            report(program, LHAT_PROGRAM_ERR_CYCLE, path);
            lhat_free(path);
            return NULL;
        }
        lhat_free(path);
        // 05 の 5.7: an invalidated unit is read again here -- the shell is
        // the same one the host may be holding, and only what was made of it
        // went. Every other state is 5.3's "loaded once": a second require^
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
// program's for a unit of the program (6 章: what it publishes has to
// outlive it) and NULL for a loaded script (5.6), whose types nobody else
// will point at -- the result then owns them.
#if LHAT_WITH_FRONTEND
static void check_parsed(LhatProgram *program, LhatUnit *unit,
                         LhatTypeArena *arena)
{
    lhat_lexer_init(&unit->lexer, &unit->source);
    lhat_parse(&unit->lexer, &unit->parsed);
    unit->loaded = true;
    unit->state = LHAT_UNIT_CHECKING;

    Resolution resolution;
    resolution.program = program;
    resolution.requiring = unit;

    LhatRequire require;
    require.resolve = resolve_require;
    require.context = &resolution;
    require.hosted = program->hosted;  // 05 の 8.7
    require.globals = program->globals;  // 05 の 8.6
    // 05 の 8.2: the initial bindings are the host's convenience for a
    // program it means to run. A text read as data is not that -- what a
    // host bound for its own units is no part of what a configuration file
    // may name -- so LhatLoadOptions can leave them out.
    bool initial = !unit->as_data;
    require.initial_names =
        initial ? (const char *const *)program->initial_names : NULL;
    require.initial_members =
        initial ? (const char *const *)program->initial_members : NULL;
    require.initial_count = initial ? program->initial_count : 0;
    require.annotations = program->annotations;  // 02 の 18.5
    require.annotation_count = program->annotation_count;

    // The recursion is what puts the graph in dependency order (6.2): the
    // required unit finishes before this one's checking gets past the
    // require^ that asked for it.
    lhat_check_unit(unit->parsed.root, &unit->lexer, program->strict, arena,
                    &require, &unit->checked);

    // 05 の 5.7: kept past the check that read it, since an invalidation
    // throws the check away and the name is what a machine holds the unit
    // under. A second check of the same unit may read a different one.
    lhat_free(unit->module_name);
    unit->module_name = unit->checked.module_name != NULL
                            ? duplicate(unit->checked.module_name)
                            : NULL;

    unit->state = LHAT_UNIT_DONE;
}
#endif  // LHAT_WITH_FRONTEND

// 05 の 5 章: the compile-time twin of resolve_require. The unit is already
// there -- checking put it there -- so this only has to say where it sits.
#if LHAT_WITH_FRONTEND
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
    // The number is the requiring unit's own -- a position in its table
    // (LhatUnitTable), which this list becomes once everything compiled.
    return remember_edge(r->requiring, unit);
}

// 02 の 14.2: the tree of a unit already parsed, for a composition in another
// unit that has to be flattened where it is written. Everything the graph
// reached is still here -- 6.2 checked it before any of this compiled -- so
// this only has to find it.
static bool resolve_unit_body(void *context, size_t unit,
                              const LhatNode **out_statements,
                              const LhatLexer **out_lexer)
{
    Resolution *r = (Resolution *)context;
    if (unit >= r->requiring->referenced_count) {
        return false;
    }
    const LhatUnit *u = r->requiring->referenced[unit];
    if (!u->loaded || u->parsed.root == NULL) {
        return false;
    }
    *out_statements = u->parsed.root->v.list.items;
    *out_lexer = &u->lexer;
    return *out_statements != NULL;
}


// ---------------------------------------------------------------------------
// 02 の 18: what a unit wrote as annotations
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

// The top-level binding of `name`, or NULL. 18.4 puts an annotation on the
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

// A member of the definition, or a field of its template -- 18.4 puts an
// annotation on both, and a host asking for `hp` does not care which it is.
static const LhatNode *definition_entry(const LhatUnit *unit,
                                        const LhatNode *definition,
                                        const char *name)
{
    if (definition == NULL) {
        return NULL;
    }
    // 14.5: a definition is often written as a composition, and what a host
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
        return NULL;  // 5.3: another unit's tree is not this one's to walk
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
        // 14.3 with 14.7改2: the template has no key -- and neither has the
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

    // 18.3's leading '-' is kept as the unary it is, and applied here so a
    // host reads one number rather than a shape.
    double sign = 1.0;
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
            out.number = sign * (double)argument->v.integer.value;
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
// 01 の 6.4: what a unit says about itself
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
        // 14.7改2: the delegate entry has no key either -- see
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

// The parameters a member wrote, whether it wrote a signature (14.15) or the
// f^ / p^ itself. 13.4: self^ is a mark saying a call hands the receiver
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

// What a value is written as, for a field given one instead of a type. 14.6
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
        // 01 の 5.4: an interpolation is a string however many holes it has.
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
            // 01 の 3.3: id^name and "name" answer the same bytes, and which
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
    // 18.7改: what a writer writes when there is nothing for the language to
    // run. The clauses a block may carry (9 章) count as something written,
    // so both halves of the list have to be empty for this to be true.
    const LhatNode *body = member_body(entry);
    out.empty_body = body != NULL && body->kind == LHAT_NODE_BLOCK &&
                     body->v.list.items == NULL && body->v.list.extra == NULL;
    // The written type first: 14.6's 'name : type = value' says it outright.
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
// 05 の 10.6: a binary unit answers 02 の 18 off the records the bytes carried
// ---------------------------------------------------------------------------
//
// The addresses are the four lhat_unit_annotation_count names (program.h).
// The records hold the top-level bindings in written order, each with the
// members of the definition it holds -- what the walkers above answered
// when the unit was written, kept because the bytes have no tree. 10.8: a
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
// 05 の 4.5: what the unit published, as types
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

// 05 の 10 章: a binary unit answers off the descriptors it carried; the
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
            lhat_rt_from_checked(&unit->proto->chunk.heap, m->type);
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
    // 05 の 10 章: a binary unit carries no checker type to conform, and
    // this question is the checker's -- it answers no.
    const LhatTypeMember *m = export_named(unit, name);
    if (m == NULL || signature == NULL || unit->program == NULL) {
        return false;
    }
    // 8.7: read the way a registration's signature is, against what the
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
    return false;  // 10.8: the checker's question, and there is no checker
}
#endif  // LHAT_WITH_FRONTEND

// ---------------------------------------------------------------------------
// 05 の 8.7: what the host provides
// ---------------------------------------------------------------------------


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
            next->v.table.is_module = true;  // and named through, not held
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
            program->hosted->v.table.is_module = true;
        }
    }
    return program->hosted;
}

static const LhatTypeMember *hosted_member(const LhatType *table,
                                           const char *name)
{
    // 05 の 8.9: a host value type keeps its members in the same half of the
    // union a table does, and 8.7 registers into one exactly as into the
    // other -- so both are searched here.
    //
    // The type's OWN members and no link: 8.8改 puts a derived type under
    // its base, and registering a name the base already has is registering
    // it here, not finding it there.
    return lhat_type_own_member(table, name, strlen(name));
}

// 02 の 14.12 with 05 の 8.7: a registration's signature as the descriptors
// the machine's overload search reads -- rttype.c's conversion, onto the
// program's own heap (host_heap), once, at registration. A parameter whose
// descriptor comes back NULL is not compared, so the search falls back on
// the counts. 13.4 keeps self^ out of the list either way, so the count is
// the same one the entry recorded.
static LhatRuntimeType **lower_host_params(LhatProgram *program,
                                           const LhatType *signature,
                                           uint8_t count)
{
    if (count == 0 || signature == NULL ||
        signature->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    LhatRuntimeType **types =
        (LhatRuntimeType **)lhat_alloc((size_t)count * sizeof *types);
    if (types == NULL) {
        return NULL;
    }
    size_t at = 0;
    for (const LhatTypeList *p = signature->v.func.params;
         p != NULL && at < count; p = p->next) {
        types[at++] = lhat_rt_from_checked(&program->host_heap, p->type);
    }
    while (at < count) {
        types[at++] = NULL;
    }
    return types;
}

// What a machine's LhatHost takes over: its own copy of the pointers, since
// the host frees the array with itself, while the nodes stay the program's.
static LhatRuntimeType **borrowed_params(const LhatRuntimeType *const *types,
                                         uint8_t count)
{
    if (types == NULL || count == 0) {
        return NULL;
    }
    LhatRuntimeType **copy =
        (LhatRuntimeType **)lhat_alloc((size_t)count * sizeof *copy);
    if (copy != NULL) {
        memcpy(copy, types, (size_t)count * sizeof *copy);
    }
    return copy;
}

static bool keep_entry(LhatProgram *program, const char *module,
                       const char *type, const char *name, LhatHostFn call,
                       void *context, const LhatType *signature,
                       const char *signature_text)
{
    LHAT_GROW(program->host_entries, program->host_entry_count,
              program->host_entry_capacity, 8, return false);

    LhatHostEntry *entry = &program->host_entries[program->host_entry_count];
    memset(entry, 0, sizeof *entry);
    entry->module = duplicate(module);
    entry->name = duplicate(name);
    entry->type = type != NULL ? duplicate(type) : NULL;
    entry->signature_text =
        signature_text != NULL ? duplicate(signature_text) : NULL;
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
        entry->self_last = signature->v.func.self_last;
        entry->parameter_types =
            lower_host_params(program, signature, entry->parameters);
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL) ||
        (signature_text != NULL && entry->signature_text == NULL) ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
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
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL, NULL)) {
        return NULL;
    }

    // 8.8: the tag is identity and nothing else, so there is exactly one per
    // DECLARATION -- and the declaration is this C call, however many
    // programs make it. The registry (registry.h) is where the one lives; a
    // second program registering std.io.File comes away with the same tag,
    // which is what lets the two agree at run time about what a value is.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];
    entry->tag = (LhatHostDataTag *)lhat_registry_hostdata(module, name);
    if (entry->tag == NULL) {
        return NULL;
    }
    // And the checker's type says which declaration it is, so what writes a
    // type out can name it rather than spelling the shape -- 8.8 puts
    // identity in the declaration, and a shape written in its place is a
    // wider type that anything of the same members satisfies.
    made->v.table.hostdata_tag = entry->tag;

    // 05 の 8.8 の fits^ 版: vm.c がコンパイル時に "module.Name" から
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
    // The array grows with lhat_realloc, which does not zero, so every field
    // is written here or it is whatever was in that memory.
    type_entry->flattened = false;

    return entry->tag;
}

bool lhat_register_type(LhatProgram *program, const char *module,
                        const char *name)
{
    return lhat_register_hostdata_type(program, module, name) != NULL;
}

// 05 の 8.8改: the checker's type for a registered hostdata name, which is
// what the flatten pass moves members between. hosted_table would make a
// module that is not there, but every type asked about here registered
// through it already, so it only ever finds one.
static LhatType *hosted_type_of(LhatProgram *program, const char *module,
                                const char *name)
{
    if (program->hosted == NULL) {
        return NULL;
    }
    LhatType *table = hosted_table(program, program->hosted, module);
    const LhatTypeMember *member =
        table != NULL ? hosted_member(table, name) : NULL;
    return member != NULL ? member->type : NULL;
}

static LhatHostTypeEntry *host_type_entry_of(LhatProgram *program,
                                             const LhatHostDataTag *tag)
{
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        if (program->host_type_entries[i].tag == tag) {
            return &program->host_type_entries[i];
        }
    }
    return NULL;  // registered by another program; nothing here to read
}

// 8.8改: gives one type what its base registered, the base having been given
// its own first -- so a chain of any depth settles whatever order the
// entries happen to be in.
//
// A name the derived type registered for itself is left alone. That is what
// overriding is: 8.8's "what was registered cannot be written over" is about
// L^ writing into a host type, and this is one registration standing in
// front of another.
static void flatten_host_type(LhatProgram *program, LhatHostTypeEntry *entry)
{
    if (entry == NULL || entry->flattened) {
        return;
    }
    entry->flattened = true;

    const LhatHostDataTag *base_tag = entry->tag->base;
    if (base_tag == NULL) {
        return;
    }
    LhatHostTypeEntry *base_entry = host_type_entry_of(program, base_tag);
    flatten_host_type(program, base_entry);

    LhatType *derived = hosted_type_of(program, entry->module, entry->name);
    LhatType *base = hosted_type_of(program, base_tag->module, base_tag->name);
    if (derived == NULL || base == NULL) {
        return;
    }
    // Linked, not copied. A binding declares a class per engine class and
    // each one carries its whole ancestry, so copying makes every class pay
    // for every class above it -- a walk inside a walk, and the editor
    // waits for all of it on each save (03 の 1.1). chk_find_member follows
    // this the way the machine follows the tag chain, and a name the derived
    // type declares itself still wins because its own list is asked first.
    derived->v.table.base = base;
}

// 8.8改: run once, when registration has closed. The moment is the first
// check: program.h already says registering after one "is too late and
// answers false", so there is nothing left to arrive.
//
// Doing this at each lhat_register_hostdata_subtype instead would be too
// early -- program.h's own advice for types that name each other is to
// register the bare types first and give them their members after, and a
// tree registered that way has nothing to inherit at the moment the relation
// is declared.
//
// The types written into are the program's own (&program->types), so nothing
// shared is touched. The tag's release is NOT copied down: the tag belongs to
// the process (registry.h), and lhat_hostdata_release walks the chain for it.
static void flatten_hostdata_bases(LhatProgram *program)
{
    if (program == NULL || program->hostdata_flattened) {
        return;
    }
    program->hostdata_flattened = true;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        flatten_host_type(program, &program->host_type_entries[i]);
    }
}

// 05 の 8.8改: the same declaration, under a type declared earlier. The base
// is looked up rather than made, so naming one that is not there is refused
// -- a tree written out of order would otherwise settle silently on a base
// that arrived later meaning something else.
//
// Nothing is inherited here. What the derived type carries is settled when
// registration closes (flatten_hostdata_bases), so members may be registered
// on either type before or after this call.
const LhatHostDataTag *lhat_register_hostdata_subtype(LhatProgram *program,
                                                      const char *module,
                                                      const char *name,
                                                      const char *base_module,
                                                      const char *base_name)
{
    if (program == NULL || base_module == NULL || base_name == NULL) {
        return NULL;
    }
    const LhatHostDataTag *base = NULL;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *at = &program->host_type_entries[i];
        if (strcmp(at->module, base_module) == 0 &&
            strcmp(at->name, base_name) == 0) {
            base = at->tag;
            break;
        }
    }
    if (base == NULL) {
        return NULL;
    }

    const LhatHostDataTag *tag =
        lhat_register_hostdata_type(program, module, name);
    if (tag == NULL) {
        return NULL;
    }
    // The tag is the process's, so this is where two programs are made to
    // agree about what a name is under (registry.h).
    return lhat_registry_set_hostdata_base(tag, base) ? tag : NULL;
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
// lhat_error_kind_new)を、program->host_heap に対して行う。
// 04 の 2.7: `local` is which top, the C side of errordef^ / localerrordef^.
// The two public entries below are this with that decided.
static bool register_error_kind(LhatProgram *program, const char *module,
                                const char *name,
                                const char *const *variant_names,
                                size_t variant_count, bool local,
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

    LhatType *set =
        lhat_type_error_set(&program->types, name, strlen(name), local);
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

    // 04 の 2.4: a kind is the declaration it came from, and the declaration
    // is this C call -- so the kinds are the registry's (registry.h) and a
    // second program declaring the same ones comes away with the very same
    // objects. A declaration of the same name with a different list of
    // variants is refused there: two lists are two declarations.
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
    const LhatErrorKind *group = NULL;
    if (!lhat_registry_error_kind(module, name, variant_names, variant_count,
                                  local, &group,
                                  (const LhatErrorKind **)variants)) {
        free_variant_arrays(variant_copies, variants, 0);
        return false;
    }
    for (size_t i = 0; i < variant_count; i++) {
        variant_copies[i] = duplicate(variant_names[i]);
        if (variant_copies[i] == NULL) {
            free_variant_arrays(variant_copies, variants, i);
            return false;
        }
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

bool lhat_register_error_kind(LhatProgram *program, const char *module,
                              const char *name,
                              const char *const *variant_names,
                              size_t variant_count,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants)
{
    return register_error_kind(program, module, name, variant_names,
                               variant_count, false, out_group, out_variants);
}

bool lhat_register_local_error_kind(LhatProgram *program, const char *module,
                                    const char *name,
                                    const char *const *variant_names,
                                    size_t variant_count,
                                    const LhatErrorKind **out_group,
                                    const LhatErrorKind **out_variants)
{
    return register_error_kind(program, module, name, variant_names,
                               variant_count, true, out_group, out_variants);
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

void *lhat_lookup_host_context(const LhatProgram *program, const char *module,
                               const char *type, const char *name)
{
    if (program == NULL || module == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *entry = &program->host_entries[i];
        if (strcmp(entry->module, module) != 0 ||
            strcmp(entry->name, name) != 0) {
            continue;
        }
        if ((type == NULL) != (entry->type == NULL)) {
            continue;
        }
        if (type != NULL && strcmp(entry->type, type) != 0) {
            continue;
        }
        return entry->context;
    }
    return NULL;
}

// 02 の 14.12: whether two registrations of one name could take the same
// call. Arms may stand together only where no call fits both -- which is what
// makes the search that resolves one able to stop at the first that fits.
//
// Told apart by anything that decides a call: how many parameters, whether a
// tail collects more, which operand the receiver is (11.3改), and failing all
// of those, a parameter position where neither type admits the other's values.
#if LHAT_WITH_FRONTEND
static bool host_arms_overlap(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL || a->kind != LHAT_TYPE_FUNC ||
        b->kind != LHAT_TYPE_FUNC) {
        return true;  // nothing to tell them apart with
    }
    if (a->v.func.takes_self != b->v.func.takes_self ||
        a->v.func.self_last != b->v.func.self_last) {
        return false;
    }
    // Two arms are told apart exactly where the machine's search (vm.c's
    // fits_call) can tell them apart: by a written parameter position the
    // two disagree on, or by one arm having no place for an argument the
    // other requires. 13.7: a variadic tail makes the count a floor and is
    // not compared by type at the call, so past its written parameters an
    // arm takes anything -- 'f(string^)' and 'f(number^, ...)' part at the
    // first position, 'f(number^)' and 'f(number^, number^, ...)' at the
    // second, and 'f(string^, ...)' against 'f(string^, font, ...)' is one
    // call fitting both, so it is refused.
    const LhatTypeList *pa = a->v.func.params;
    const LhatTypeList *pb = b->v.func.params;
    for (;; pa = pa != NULL ? pa->next : NULL, pb = pb != NULL ? pb->next : NULL) {
        if (pa == NULL && pb == NULL) {
            return true;  // a call of exactly this count fits both
        }
        bool a_open = pa != NULL || a->v.func.variadic != NULL;
        bool b_open = pb != NULL || b->v.func.variadic != NULL;
        if (!a_open || !b_open) {
            return false;  // one takes no argument here; the other needs one
        }
        if (pa != NULL && pb != NULL &&
            lhat_type_disjoint(pa->type, pb->type)) {
            return false;
        }
    }
}
#endif  // LHAT_WITH_FRONTEND

#if LHAT_WITH_FRONTEND
static bool register_into(LhatProgram *program, LhatType *owner,
                          const char *module, const char *type,
                          const char *name, const char *signature,
                          LhatHostFn call, void *context)
{
    if (owner == NULL || call == NULL) {
        return false;
    }
    // 8.7: a signature may name the builtins and whatever was registered
    // before it. Nothing a require^ brings in -- that would put the answer
    // back at the mercy of the order units are checked in. A member's may
    // write the type it is registered on as 13.13's Self^.
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted,
                                          type != NULL ? owner : NULL);
    if (written == NULL) {
        return false;
    }
    // 02 の 13.8改: the room a boundary answers into is LHAT_MAX_TUPLE
    // wide, and the reason it is that wide is that the machine's own is.
    // A signature promising more than the machine can carry is refused
    // HERE, where it is declared -- not found out later, when a host has
    // already written past the room it was handed. What the compiler
    // refuses at a written call site (compile.c), this refuses at a
    // registered one.
    if (written->kind == LHAT_TYPE_FUNC &&
        lhat_type_tuple_arm_width(written->v.func.result) >
            LHAT_MAX_TUPLE) {
        return false;
    }

    // 02 の 14.12: a name already registered gains an arm rather than being
    // refused. The type becomes the intersection of what was there and what
    // is written now -- which is exactly what an overload^ makes of a member
    // written twice, so a call site reads the two the same way.
    LhatTypeMember *existing = (LhatTypeMember *)hosted_member(owner, name);
    if (existing != NULL) {
        if (existing->type == NULL) {
            return false;
        }
        const LhatType *arms = existing->type;
        if (arms->kind == LHAT_TYPE_INTERSECT) {
            for (const LhatTypeList *arm = arms->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (host_arms_overlap(arm->type, written)) {
                    return false;
                }
            }
        } else if (host_arms_overlap(arms, written)) {
            return false;
        }
        LhatType *joined =
            lhat_type_intersect(&program->types, existing->type, written);
        if (joined == NULL) {
            return false;
        }
        existing->type = joined;
        return keep_entry(program, module, type, name, call, context, written,
                          signature);
    }

    return lhat_type_add_member(&program->types, owner, name, strlen(name),
                                written) != NULL &&
           keep_entry(program, module, type, name, call, context, written,
                      signature);
}
#else
// 05 の 10.7: what keep_entry takes from a parsed signature, taken from the
// descriptor the table holds instead -- the parts are the parameters, the
// marks are the receiver and the variadic tail.
static bool keep_entry_rt(LhatProgram *program, const char *module,
                          const char *type, const char *name, LhatHostFn call,
                          void *context, const LhatRuntimeType *rt,
                          const char *signature_text)
{
    LHAT_GROW(program->host_entries, program->host_entry_count,
              program->host_entry_capacity, 8, return false);
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count];
    memset(entry, 0, sizeof *entry);
    entry->module = duplicate(module);
    entry->name = duplicate(name);
    entry->type = type != NULL ? duplicate(type) : NULL;
    entry->signature_text = duplicate(signature_text);
    entry->call = call;
    entry->context = context;
    if (rt->part_count > 0xFF) {
        return false;
    }
    entry->parameters = (uint8_t)rt->part_count;
    entry->has_variadic = rt->variadic != NULL;
    entry->takes_self = rt->takes_self;
    entry->self_last = rt->self_last;
    if (entry->parameters > 0) {
        entry->parameter_types = borrowed_params(
            (const LhatRuntimeType *const *)rt->parts, entry->parameters);
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL) ||
        entry->signature_text == NULL ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
        return false;
    }
    program->host_entry_count++;
    return true;
}

// 10.8: the signature table stands in for the text. The checker's side
// (hosted) is not built -- nothing here reads it.
static bool register_into(LhatProgram *program, LhatType *owner,
                          const char *module, const char *type,
                          const char *name, const char *signature,
                          LhatHostFn call, void *context)
{
    if (owner == NULL || call == NULL || signature == NULL) {
        return false;
    }
    if (find_signature(program, signature) == NULL) {
        report(program, LHAT_PROGRAM_ERR_NO_SIGNATURE, signature);
        return false;
    }
    const LhatRuntimeType *rt = lhat_program_signature_type(program, signature);
    return rt != NULL && keep_entry_rt(program, module, type, name, call,
                                       context, rt, signature);
}
#endif  // LHAT_WITH_FRONTEND

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
    //
    // The tag is the process's (registry.h), so a second program declaring
    // the same dispose^ is that one declaration made twice and settles on
    // the same answer. A different one would be two ways of handing back
    // one type, which is refused.
    if (strcmp(name, "dispose") == 0) {
        for (size_t i = 0; i < program->host_entry_count; i++) {
            LhatHostEntry *entry = &program->host_entries[i];
            if (entry->tag != NULL && entry->type == NULL &&
                strcmp(entry->module, module) == 0 &&
                strcmp(entry->name, type) == 0) {
                return lhat_registry_set_release(entry->tag, call, context);
            }
        }
    }
    return true;
}

// 02 の 18.5: an annotation is checked and never installed, so this records
// only what the checker asks -- where it may be written, and what shape its
// arguments have. No LhatHostEntry: there is no value to build for a machine.
bool lhat_register_annotation(LhatProgram *program, const char *module,
                              const char *name, uint32_t targets)
{
    if (program == NULL || module == NULL || name == NULL || targets == 0) {
        return false;
    }
    // 8.7: one name, one thing. Unlike a member, a second registration is not
    // another arm -- 18.2 keeps the namespace flat, and two hosts wanting the
    // same spelling is a collision to report rather than to merge.
    for (size_t i = 0; i < program->annotation_count; i++) {
        if (strcmp(program->annotations[i].name, name) == 0) {
            return false;
        }
    }

    char *kept_module = duplicate(module);
    char *kept_name = duplicate(name);
    if (kept_module == NULL || kept_name == NULL) {
        lhat_free(kept_module);
        lhat_free(kept_name);
        return false;
    }

    // The text grows alongside the declarations, so a refusal leaves the two
    // the same length.
    size_t at = program->annotation_count;
    if (at == program->annotation_capacity) {
        size_t grown = program->annotation_capacity
                           ? program->annotation_capacity * 2 : 4;
        LhatAnnotationDecl *decls = (LhatAnnotationDecl *)lhat_realloc(
            program->annotations, grown * sizeof *decls);
        char **texts = (char **)lhat_realloc(program->annotation_signatures,
                                             grown * sizeof *texts);
        if (decls != NULL) {
            program->annotations = decls;
        }
        if (texts != NULL) {
            program->annotation_signatures = texts;
        }
        if (decls == NULL || texts == NULL) {
            lhat_free(kept_module);
            lhat_free(kept_name);
            return false;
        }
        program->annotation_capacity = grown;
    }

    memset(&program->annotations[at], 0, sizeof program->annotations[at]);
    program->annotations[at].module = kept_module;
    program->annotations[at].name = kept_name;
    program->annotations[at].targets = targets;
    program->annotation_signatures[at] = NULL;
    program->annotation_count++;
    return true;
}

// The registration of `name`, or NULL. 18.2's flat namespace is what lets the
// name alone find it.
static LhatAnnotationDecl *annotation_named(LhatProgram *program,
                                            const char *name, size_t *at)
{
    if (program == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->annotation_count; i++) {
        if (strcmp(program->annotations[i].name, name) == 0) {
            if (at != NULL) {
                *at = i;
            }
            return &program->annotations[i];
        }
    }
    return NULL;
}

#if LHAT_WITH_FRONTEND
bool lhat_register_annotation_signature(LhatProgram *program,
                                        const char *name,
                                        const char *signature)
{
    size_t at = 0;
    LhatAnnotationDecl *decl = annotation_named(program, name, &at);
    if (decl == NULL || signature == NULL || decl->signature != NULL) {
        return false;  // no such name, nothing said, or said once already
    }

    const LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                                &program->types,
                                                program->hosted, NULL);
    char *kept = written != NULL ? duplicate(signature) : NULL;
    if (written == NULL || kept == NULL) {
        lhat_free(kept);
        return false;
    }
    decl->signature = written;
    program->annotation_signatures[at] = kept;
    return true;
}
#else
bool lhat_register_annotation_signature(LhatProgram *program,
                                        const char *name,
                                        const char *signature)
{
    // 10.8: an annotation is the checker's to read, and there is no
    // checker; the text is kept for a dump, the type is not made.
    size_t at = 0;
    LhatAnnotationDecl *decl = annotation_named(program, name, &at);
    if (decl == NULL || signature == NULL ||
        program->annotation_signatures[at] != NULL) {
        return false;
    }
    program->annotation_signatures[at] = duplicate(signature);
    return program->annotation_signatures[at] != NULL;
}
#endif  // LHAT_WITH_FRONTEND

bool lhat_register_annotation_exclusive(LhatProgram *program,
                                        const char *name, const char *other)
{
    LhatAnnotationDecl *decl = annotation_named(program, name, NULL);
    if (decl == NULL || other == NULL || strcmp(name, other) == 0) {
        return false;  // nothing excludes itself
    }
    for (size_t i = 0; i < decl->exclusive_count; i++) {
        if (strcmp(decl->exclusives[i], other) == 0) {
            return true;  // said twice is said once
        }
    }

    char *kept = duplicate(other);
    const char **grown = (const char **)lhat_realloc(
        (void *)decl->exclusives,
        (decl->exclusive_count + 1) * sizeof *decl->exclusives);
    if (kept == NULL || grown == NULL) {
        lhat_free(kept);
        if (grown != NULL) {
            decl->exclusives = grown;
        }
        return false;
    }
    decl->exclusives = grown;
    decl->exclusives[decl->exclusive_count++] = kept;
    return true;
}

bool lhat_register_annotation_requisite(LhatProgram *program,
                                        const char *name, const char *other)
{
    LhatAnnotationDecl *decl = annotation_named(program, name, NULL);
    if (decl == NULL || other == NULL || strcmp(name, other) == 0) {
        return false;  // nothing is its own other half
    }
    for (size_t i = 0; i < decl->requisite_count; i++) {
        if (strcmp(decl->requisites[i], other) == 0) {
            return true;  // said twice is said once
        }
    }

    char *kept = duplicate(other);
    const char **grown = (const char **)lhat_realloc(
        (void *)decl->requisites,
        (decl->requisite_count + 1) * sizeof *decl->requisites);
    if (kept == NULL || grown == NULL) {
        lhat_free(kept);
        if (grown != NULL) {
            decl->requisites = grown;
        }
        return false;
    }
    decl->requisites = grown;
    decl->requisites[decl->requisite_count++] = kept;
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

// 05 の 8.7改: the shared road of the four constant registrations. No
// signature text and no lhat_type_of_text: an enum dump registers
// thousands of these, and lhat_type_simple answers the one shared node
// per kind. A constant takes its name whole -- an existing member,
// constant or subroutine, refuses it, since a value is no overload arm.
static bool register_const(LhatProgram *program, const char *module,
                           const char *type, const char *name,
                           LhatHostConstKind kind, int64_t whole,
                           double real, bool truth, const char *text)
{
    if (program == NULL || module == NULL || name == NULL ||
        (kind == LHAT_HOST_CONST_STRING && text == NULL)) {
        return false;
    }
    LhatType *owner =
        hosted_table(program, hosted_root(program), module);
    if (type != NULL) {
        const LhatTypeMember *found = hosted_member(owner, type);
        // 8.8's nominal table or 8.9's host value type -- both carry the
        // member list a static lands in.
        if (found == NULL || (found->type->kind != LHAT_TYPE_TABLE &&
                              found->type->kind != LHAT_TYPE_HOSTVALUE)) {
            return false;
        }
        owner = found->type;
    }
    if (owner == NULL || hosted_member(owner, name) != NULL) {
        return false;
    }
    LhatType *written = lhat_type_simple(
        &program->types, kind == LHAT_HOST_CONST_STRING ? LHAT_TYPE_STRING
                         : kind == LHAT_HOST_CONST_BOOL ? LHAT_TYPE_BOOL
                                                        : LHAT_TYPE_NUMBER);
    if (written == NULL ||
        lhat_type_add_member(&program->types, owner, name, strlen(name),
                             written) == NULL) {
        return false;
    }
    if (!keep_entry(program, module, type, name, NULL, NULL, NULL, NULL)) {
        return false;
    }
    LhatHostEntry *entry =
        &program->host_entries[program->host_entry_count - 1];
    entry->const_kind = kind;
    entry->const_integer = whole;
    entry->const_real = real;
    entry->const_bool = truth;
    if (text != NULL) {
        entry->const_text = duplicate(text);
        if (entry->const_text == NULL) {
            return false;
        }
    }
    return true;
}

bool lhat_register_enum_valued(LhatProgram *program, const char *module,
                               const char *type, const char *name,
                               const char *const *members,
                               const int64_t *values, size_t count)
{
    if (program == NULL || module == NULL || name == NULL ||
        members == NULL || count == 0) {
        return false;
    }
    LhatType *owner = hosted_table(program, hosted_root(program), module);
    if (type != NULL) {
        const LhatTypeMember *found = hosted_member(owner, type);
        // 8.8's nominal table or 8.9's host value type -- the same member
        // list a static constant lands in.
        if (found == NULL || (found->type->kind != LHAT_TYPE_TABLE &&
                              found->type->kind != LHAT_TYPE_HOSTVALUE)) {
            return false;
        }
        owner = found->type;
    }
    if (owner == NULL || hosted_member(owner, name) != NULL) {
        return false;
    }
    LHAT_GROW(program->host_enums, program->host_enum_count,
              program->host_enum_capacity, 4, return false);
    LhatProgramEnum *e = &program->host_enums[program->host_enum_count];
    memset(e, 0, sizeof *e);
    e->module = duplicate(module);
    e->type = type != NULL ? duplicate(type) : NULL;
    e->name = duplicate(name);
    e->members = (char **)lhat_calloc(count, sizeof *e->members);
    e->values = (int64_t *)lhat_alloc(count * sizeof *e->values);
    if (e->module == NULL || (type != NULL && e->type == NULL) ||
        e->name == NULL || e->members == NULL || e->values == NULL) {
        return false;
    }
    e->count = count;
    for (size_t i = 0; i < count; i++) {
        if (members[i] == NULL) {
            return false;
        }
        e->members[i] = duplicate(members[i]);
        if (e->members[i] == NULL) {
            return false;
        }
        e->values[i] = values != NULL ? values[i] : (int64_t)i + 1;
    }

    // The checker's two tiers, 02 の 19 章's -- a unit's when^ over the
    // members proves exhaustive the way a declared enum's does.
    LhatType *decl =
        lhat_type_enum_decl(&program->types, e->name, strlen(e->name));
    LhatType *number = lhat_type_simple(&program->types, LHAT_TYPE_NUMBER);
    if (decl == NULL || number == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (lhat_type_enum_member(&program->types, decl, e->members[i],
                                  strlen(e->members[i]), number) == NULL) {
            return false;
        }
    }
    if (lhat_type_add_member(&program->types, owner, e->name,
                             strlen(e->name), decl) == NULL) {
        return false;
    }

    // The identity fits^ compares, shared by every machine of this program.
    e->decl_rt = lhat_type_rt_new(&program->host_heap, LHAT_TYPE_RT_ENUM);
    if (e->decl_rt == NULL) {
        return false;
    }
    e->decl_rt->enum_decl = decl;
    e->decl_rt->enum_name =
        lhat_string_new(&program->host_heap, e->name, strlen(e->name));
    program->host_enum_count++;
    return true;
}

bool lhat_register_enum(LhatProgram *program, const char *module,
                        const char *type, const char *name,
                        const char *const *members, size_t count)
{
    return lhat_register_enum_valued(program, module, type, name, members,
                                     NULL, count);
}

bool lhat_register_const_integer(LhatProgram *program, const char *module,
                                 const char *type, const char *name,
                                 int64_t value)
{
    return register_const(program, module, type, name,
                          LHAT_HOST_CONST_INTEGER, value, 0.0, false, NULL);
}

bool lhat_register_const_real(LhatProgram *program, const char *module,
                              const char *type, const char *name,
                              double value)
{
    return register_const(program, module, type, name, LHAT_HOST_CONST_REAL,
                          0, value, false, NULL);
}

bool lhat_register_const_bool(LhatProgram *program, const char *module,
                              const char *type, const char *name, bool value)
{
    return register_const(program, module, type, name, LHAT_HOST_CONST_BOOL,
                          0, 0.0, value, NULL);
}

bool lhat_register_const_string(LhatProgram *program, const char *module,
                                const char *type, const char *name,
                                const char *text)
{
    return register_const(program, module, type, name,
                          LHAT_HOST_CONST_STRING, 0, 0.0, false, text);
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

    // 8.9: the tag is identity, and there is one per DECLARATION rather than
    // one per program -- the registry (registry.h) keeps it, and a second
    // program declaring the same type comes away with the same one. A
    // declaration of the same name at a different size is refused there:
    // the width is what every frame holding one was laid out against.
    LhatHostValueTag *tag =
        (LhatHostValueTag *)lhat_registry_hostvalue(module, name, size);
    if (tag == NULL) {
        return NULL;
    }

    LhatType *made = lhat_type_hostvalue(&program->types, tag);
    if (made == NULL ||
        lhat_type_add_member(&program->types, table, name, strlen(name),
                             made) == NULL ||
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL, NULL)) {
        return NULL;
    }
    // keep_entry has install make an empty table under the type's name in
    // L^.modules, exactly as for a hostdata type -- that table is where the
    // registered members land, and what the machine reads back as the
    // type's members table.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];

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

    // The field belongs to the tag, and the tag to the process (registry.h)
    // -- so a second program declaring the same field settles on the one
    // that is there, and one that declares it at a different offset or of a
    // different kind is refused.
    if (!lhat_registry_hostvalue_field(tag, field, offset, kind)) {
        return false;
    }
    // The checker's side is this program's, and is added whether the field
    // was already declared or not.
    LhatType *number = lhat_type_simple(&program->types, LHAT_TYPE_NUMBER);
    if (number == NULL ||
        lhat_type_add_member(&program->types, found->type, field,
                             strlen(field), number) == NULL) {
        return false;
    }
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
#if LHAT_WITH_FRONTEND
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted,
                                          NULL);
    if (written == NULL ||
        lhat_type_add_member(&program->types, program->globals, name,
                             strlen(name), written) == NULL) {
        return false;
    }
#else
    // 10.8: the table stands in for the text, as in register_into.
    if (signature == NULL || find_signature(program, signature) == NULL) {
        report(program, LHAT_PROGRAM_ERR_NO_SIGNATURE,
               signature != NULL ? signature : name);
        return false;
    }
    const LhatRuntimeType *held = lhat_program_signature_type(program, signature);
    if (held == NULL || held->part_count > 0xFF) {
        return false;
    }
#endif

    LHAT_GROW(program->global_entries, program->global_count,
              program->global_capacity, 4, return false);
    LhatGlobalEntry *entry = &program->global_entries[program->global_count];
    memset(entry, 0, sizeof *entry);
    entry->name = duplicate(name);
    entry->signature_text = duplicate(signature);
    entry->call = call;
    entry->context = context;
#if LHAT_WITH_FRONTEND
    if (written->kind == LHAT_TYPE_FUNC) {
        size_t count = 0;
        for (const LhatTypeList *p = written->v.func.params; p != NULL;
             p = p->next) {
            count++;
        }
        entry->parameters = (uint8_t)count;
        entry->has_variadic = written->v.func.variadic != NULL;
        entry->takes_self = written->v.func.takes_self;
        entry->self_last = written->v.func.self_last;
        entry->parameter_types =
            lower_host_params(program, written, entry->parameters);
    }
#else
    entry->parameters = (uint8_t)held->part_count;
    entry->has_variadic = held->variadic != NULL;
    entry->takes_self = held->takes_self;
    entry->self_last = held->self_last;
    if (entry->parameters > 0) {
        entry->parameter_types = borrowed_params(
            (const LhatRuntimeType *const *)held->parts, entry->parameters);
    }
#endif
    if (entry->name == NULL || entry->signature_text == NULL ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
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

// 04 の 11.6改: the unit's path, stamped onto every proto of its tree so a
// traceback can name the file a frame belongs to. Debug only -- a copy per
// proto, freed with it.
static void stamp_source(LhatProto *proto, const char *path)
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
        stamp_source(proto->protos[i], path);
    }
}

// The second pass: a unit's table is filled once every unit of the batch
// has its proto, since the list is in the order the units were reached and
// not in dependency order. Takes the list the first pass built.
static bool fill_unit_table(LhatUnit *u)
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
    // 05 の 5.7: the list stays. It is the only record of which way the
    // graph runs, and lhat_program_invalidate reads it backwards to find
    // every unit a changed one reaches.
    return true;
}

// Compiles one checked unit into u->proto. `registers` is 5.3's guard and
// registry write for a module^ unit -- off for a loaded one (5.6), which
// answers its table to whoever called it and enters no registry. False
// leaves the failure where lhat_program_compile_failure reads it.
#if LHAT_WITH_FRONTEND
static bool compile_one(LhatProgram *program, LhatUnit *u, bool registers)
{
    Resolution resolution;
    resolution.program = program;
    resolution.requiring = u;

    LhatUnits units;
    units.resolve = resolve_unit;
    units.body = resolve_unit_body;
    units.context = &resolution;
    units.module_name = u->checked.module_name;
    units.registers = registers;
        units.initial_names = (const char *const *)program->initial_names;
        units.initial_members = (const char *const *)program->initial_members;
        units.initial_count = program->initial_count;  // 05 の 8.2
        units.host_errors = program->host_error_entries;  // 05 の 8.7 の誤り版
        units.host_error_count = program->host_error_entry_count;
        units.host_types = program->host_type_entries;  // 05 の 8.8 の fits^ 版
        units.host_type_count = program->host_type_entry_count;
        units.hostvalue_types = program->hostvalue_type_entries;  // 05 の 8.9
        units.hostvalue_type_count = program->hostvalue_type_entry_count;

    lhat_free(u->referenced);
    u->referenced = NULL;
    u->referenced_count = 0;
    u->referenced_capacity = 0;
    LhatCompileResult compiled =
        lhat_compile_module(u->parsed.root, &u->lexer, &units, &u->proto);
    if (compiled.status != LHAT_COMPILE_OK) {
        // Kept so the caller can say which form stopped it rather than
        // only that something did -- and which unit, since the position
        // in it indexes that unit's source and no other.
        program->compile_status = compiled.status;
        program->compile_result = compiled;
        program->compile_unit = u;
        return false;
    }
    stamp_source(u->proto, u->path);
    return true;
}
#endif  // LHAT_WITH_FRONTEND

bool lhat_program_compile(LhatProgram *program)
{
    bool ok = true;
#if LHAT_WITH_FRONTEND
    for (LhatUnit *u = program->units; u != NULL; u = u->next) {
        if (!u->loaded || u->state != LHAT_UNIT_DONE || u->proto != NULL) {
            continue;  // failed to check, or compiled by an earlier call
        }
        if (!compile_one(program, u, true)) {
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
            !fill_unit_table(u)) {
            program->compile_status = LHAT_COMPILE_TOO_COMPLEX;
            ok = false;
        }
    }
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

// One registration made into a value on `machine` and put where it belongs.
// The place is passed separately from the entry so that an inherited member
// can be installed under the type that inherited it -- which may sit in
// another module than the one that declared it.
static bool install_entry(LhatMachine *machine, const LhatHostEntry *e,
                          const char *module, const char *type,
                          const char *name)
{
    // A type registers as an empty table under its module; its members
    // are entries of their own and land in it as they come.
    LhatValue value = lhat_nil();
    if (e->const_kind == LHAT_HOST_CONST_INTEGER) {
        value = lhat_integer(e->const_integer);
    } else if (e->const_kind == LHAT_HOST_CONST_REAL) {
        value = lhat_real(e->const_real);
    } else if (e->const_kind == LHAT_HOST_CONST_BOOL) {
        value = lhat_bool(e->const_bool);
    } else if (e->const_kind == LHAT_HOST_CONST_STRING) {
        if (!lhat_machine_make_string(machine, e->const_text,
                                      strlen(e->const_text), &value)) {
            return false;
        }
    } else if (e->call == NULL) {
        if (!lhat_machine_make_table(machine, &value)) {
            return false;
        }
    } else if (!lhat_machine_make_host(
                   machine, e->call, e->context, e->parameters,
                   e->has_variadic, e->takes_self, e->self_last,
                   borrowed_params((const LhatRuntimeType *const *)
                                       e->parameter_types,
                                   e->parameters),
                   &value)) {
        return false;
    }
    return lhat_machine_register(machine, module, type, name, value);
}

bool lhat_program_install(const LhatProgram *program, LhatMachine *machine)
{
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *e = &program->host_entries[i];
        if (!install_entry(machine, e, e->module, e->type, e->name)) {
            return false;
        }
    }

    // 05 の 8.8改: and now what each type is declared under. The machine
    // reads a hostdata value's members off the type's own table under
    // L^.modules (lhat_machine_make_hostdata), which is a different place
    // from the checker's type -- so what the checker does with the base
    // link on the type has to be done here too, or a derived value would
    // check as having a member and then not find it.
    //
    // A LINK, one per type, and the walk climbs it (vm.c's
    // lhat_machine_link_hostdata_base). Copying the base's members down
    // instead meant a pass over every registration for every type and every
    // one of its ancestors, with another such pass inside it to decide
    // which declaration was nearest -- and a binding with a class per
    // engine class waited seconds for that on every load. Nearest still
    // wins, now because the walk meets it first.
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *te = &program->host_type_entries[i];
        const LhatHostDataTag *base = te->tag->base;
        if (base == NULL) {
            continue;
        }
        if (!lhat_machine_link_hostdata_base(machine, te->module, te->name,
                                             base->module, base->name)) {
            return false;
        }
    }

    // 05 の 8.7改2: the enums, one value object per machine.
    for (size_t i = 0; i < program->host_enum_count; i++) {
        const LhatProgramEnum *e = &program->host_enums[i];
        LhatValue value = lhat_nil();
        if (!lhat_machine_make_enum(machine, e->name, e->decl_rt,
                                    (const char *const *)e->members,
                                    e->values, e->count, &value) ||
            !lhat_machine_register(machine, e->module, e->type, e->name,
                                   value)) {
            return false;
        }
    }
    // 05 の 8.6: what goes in L^ itself rather than under its registry.
    for (size_t i = 0; i < program->global_count; i++) {
        const LhatGlobalEntry *e = &program->global_entries[i];
        LhatValue value = lhat_nil();
        if (!lhat_machine_make_host(
                machine, e->call, e->context, e->parameters, e->has_variadic,
                e->takes_self, e->self_last,
                borrowed_params((const LhatRuntimeType *const *)
                                    e->parameter_types,
                                e->parameters),
                &value) ||
            !lhat_machine_set_global(machine, e->name, value)) {
            return false;
        }
    }
    // 05 の 8.9: a host value has no heap half to carry its members table,
    // so the machine keeps one per registered type, found by the tag's
    // index. The tables themselves are the ones the entry loop above put
    // under L^.modules -- reachable from the environment, so the collector
    // needs nothing new.
    //
    // 5.7 with registry.h: the index is the process's, so the array is taken
    // to the width every declared type reaches rather than to how many this
    // program declared -- otherwise a program declaring the second of two
    // types would index past its own array.
    if (program->hostvalue_type_entry_count > 0 &&
        !lhat_machine_bind_hostvalues(machine,
                                      program->hostvalue_type_entries,
                                      program->hostvalue_type_entry_count,
                                      lhat_registry_hostvalue_count())) {
        return false;
    }
    return true;
}

#if LHAT_WITH_FRONTEND
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
#else
void lhat_program_install_checks(const LhatProgram *program,
                                 LhatCheckSession *session)
{
    (void)program;
    (void)session;  // 10.8: no prompt without the front end
}

void lhat_program_install_compiles(const LhatProgram *program,
                                   LhatCompileSession *session)
{
    (void)program;
    (void)session;
}
#endif  // LHAT_WITH_FRONTEND

void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context)
{
    memset(program, 0, sizeof *program);
    lhat_type_arena_init(&program->types);
    program->strict = strict;
    program->load = load;
    program->loader_context = context;
    // See lhat_proto_new's comment: born black so a machine reading a
    // host-registered error kind never writes into program->host_heap
    // -- required once more than one machine (std.thread) can read it.
    program->host_heap.white = LHAT_GC_BLACK;
    // 5.7: what discarded bodies leave behind. Black for the same reason --
    // they came off chunk heaps that were born black, and no machine's
    // collection may write into them.
    program->retired_objects.white = LHAT_GC_BLACK;
}

// ---------------------------------------------------------------------------
// 05 の 5.6: loading a script at run time
// ---------------------------------------------------------------------------

// Everything the four stages made, leaving the shell -- its path, its module
// name and its place in the list -- standing. 05 の 5.7 invalidates a unit
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
        const char *message = lhat_program_error_message(d->code);
        if (s->length > 0) {
            say(s, "\n", 1);
        }
        say(s, d->path, strlen(d->path));
        say(s, ": error: ", 9);
        say(s, message, strlen(message));
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
        const char *message =
            lhat_compile_status_message(program->compile_status);
        say(said, unit->path, strlen(unit->path));
        say(said, ": error: ", 9);
        say(said, message, strlen(message));
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
// checked into an arena of its own, compiled without 5.3's guard and
// registry write, and left holding its proto for the caller to take.
#if LHAT_WITH_FRONTEND
static LhatLoadStatus build_placed(LhatProgram *program, LhatUnit *unit)
{
    LhatUnit *before = program->units;
    size_t reported_before = program->diagnostic_count;
    check_parsed(program, unit, NULL);

    Said said;
    memset(&said, 0, sizeof said);
    // The program's own diagnostics, then what the stages said of the unit
    // and of every unit the load reached.
    say_program(&said, program, reported_before);
    say_unit(&said, unit);
    for (const LhatUnit *u = program->units; u != before; u = u->next) {
        say_unit(&said, u);
    }
    bool built = said.length == 0 && lhat_program_compile(program) &&
                 compile_one(program, unit, false) && fill_unit_table(unit);
    return settle_placed(program, unit, &said, built);
}
#else
static LhatLoadStatus build_placed(LhatProgram *program, LhatUnit *unit)
{
    // 10.8: text, and nothing here reads text.
    (void)unit;
    program->load_failure =
        duplicate("this build has no front end; only a binary unit runs");
    return LHAT_LOAD_REJECTED;
}
#endif  // LHAT_WITH_FRONTEND

// 05 の 10 章 with 08 の 7改: the bytes a full build wrote for a script or
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
        stamp_source(read.proto, unit->path);
        lhat_free(read.module_name);
        for (size_t i = 0; i < read.export_count; i++) {
            lhat_free(read.export_names[i]);
        }
        lhat_free(read.export_names);
        lhat_free(read.export_rt);
        lhat_reflection_free(read.reflection);
        built = lhat_program_compile(program) && fill_unit_table(unit);
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
    // 05 の 8.8改: the other way checking begins (5.6). check_path's own call
    // does not cover this one -- a script that requires nothing never reaches
    // it -- and the pass does its work once whichever arrives first.
    flatten_hostdata_bases(program);
    lhat_free(program->load_failure);
    program->load_failure = NULL;
    LhatUnit *unit = (LhatUnit *)lhat_calloc(1, sizeof *unit);
    if (unit == NULL) {
        return NULL;
    }
    unit->path = normalise_path(name);
    if (unit->path == NULL) {
        lhat_free(unit);
        return NULL;
    }
    unit->program = program;
    // 05 の 8.2: what check_parsed reads to decide whether the host's initial
    // bindings are in scope. Set before anything is checked, since that is
    // the one moment it is asked.
    unit->as_data = options != NULL && !options->initial_bindings;
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
    return program->load(program->loader_context, path, length);
}

LhatLoadStatus lhat_program_load_text_with(LhatProgram *program,
                                           const char *name, const char *text,
                                           size_t length,
                                           const LhatLoadOptions *options,
                                           LhatProto **out)
{
    *out = NULL;
    LhatUnit *unit = place(program, name, options);
    if (unit == NULL) {
        return LHAT_LOAD_OUT_OF_MEMORY;
    }
    // 10.1: the first byte says which the caller holds.
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

LhatLoadStatus lhat_program_write_text(LhatProgram *program, const char *name,
                                       const char *text, size_t length,
                                       const LhatLoadOptions *options,
                                       bool with_debug_names, uint8_t **out,
                                       size_t *out_length)
{
    *out = NULL;
    *out_length = 0;
    LhatUnit *unit = place(program, name, options);
    if (unit == NULL) {
        return LHAT_LOAD_OUT_OF_MEMORY;
    }
    lhat_source_init_from_string(&unit->source, unit->path, text, length);
    LhatLoadStatus status = build_placed(program, unit);
    if (status == LHAT_LOAD_OK &&
        !lhat_serialize_write(unit, with_debug_names, out, out_length)) {
        lhat_free(program->load_failure);
        program->load_failure = duplicate("the unit could not be written out");
        status = LHAT_LOAD_REJECTED;
    }
    forget(unit);
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
    LhatLoadOptions options;
    options.initial_bindings = true;  // 8.2, as it always was
    return lhat_program_load_text_with(program, name, text, length, &options,
                                       out);
}

LhatLoadStatus lhat_program_load_file(LhatProgram *program, const char *path,
                                      LhatProto **out)
{
    *out = NULL;
    lhat_free(program->load_failure);
    program->load_failure = NULL;
    char *resolved = normalise_path(path);
    if (resolved == NULL) {
        return LHAT_LOAD_OUT_OF_MEMORY;
    }
    size_t length = 0;
    char *text = program->load != NULL
                     ? program->load(program->loader_context, resolved, &length)
                     : NULL;
    LhatLoadStatus status = LHAT_LOAD_CANNOT_READ;
    if (text != NULL) {
        status = lhat_program_load_text(program, resolved, text, length, out);
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
    // 05 の 10 章: the strings; the types are the arena's.
    for (size_t i = 0; i < program->enum_identity_count; i++) {
        lhat_free(program->enum_identities[i].path);
        lhat_free(program->enum_identities[i].name);
    }
    lhat_free(program->enum_identities);
    lhat_free(program->signature_index);
    lhat_free(program->signatures);
    // 05 の 8.7: what the registrations left with the program, first and in
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
        // 8.8: the tag is not the program's -- one declaration, one tag,
        // for the process (registry.h).
    }
    lhat_free(program->host_entries);
    program->host_entries = NULL;
    program->host_entry_count = 0;
    program->host_entry_capacity = 0;

    // 02 の 18.5: the declaration owns the two names it points at, and the
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

    // 05 の 8.9: the tags and their fields belong to the process, not to
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
    // 04 の 12.4: この program が登録した誤り種別のオブジェクト自身
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

    // 05 の 5.7: what invalidations retired and the host never discarded.
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

const LhatProto *lhat_unit_proto(const LhatUnit *unit)
{
    return unit->proto;
}

const char *lhat_unit_module_name(const LhatUnit *unit)
{
    // 05 の 5.7: the unit's own copy, so that this still answers after an
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
    return unit != NULL && unit->loaded && unit->state == LHAT_UNIT_DONE &&
           unit->lexer.diagnostic_count == 0 &&
           unit->parsed.diagnostic_count == 0 &&
           unit->checked.diagnostic_count == 0;
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
// 05 の 5.7: a unit changed under the program
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
// and carried on past (6.3), and a project's units are few enough that the
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
    uint64_t now = hash_text(text, length);
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

size_t lhat_program_invalidate(LhatProgram *program, const char *path)
{
    if (program == NULL || path == NULL) {
        return SIZE_MAX;
    }
    char *resolved = normalise_path(path);
    if (resolved == NULL) {
        return SIZE_MAX;
    }
    LhatUnit *start = find_unit(program, resolved);
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

void lhat_program_discard_retired(LhatProgram *program)
{
    if (program == NULL) {
        return;
    }
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

size_t lhat_reload(LhatProgram *program, const char *path,
                   LhatMachine *const *machines, size_t machine_count)
{
    size_t retired = lhat_program_invalidate(program, path);
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
            lhat_program_check(program, lhat_unit_path(unit));
        }
    }
    if (lhat_program_has_errors(program) || !lhat_program_compile(program)) {
        // The old world keeps running and the retired list keeps waiting;
        // the diagnostics say why, and the next save tries again.
        return retired;
    }

    // 5.7's question, asked outright: collect on every machine, then look
    // for anything still holding a retired body. Held anywhere -- or a
    // cleanup still waiting, whose run is code this walk cannot see into --
    // means the bodies wait for the next reload, which is the safe side of
    // every answer here.
    size_t bodies = 0;
    for (size_t i = 0; i < program->retired_count; i++) {
        bodies += count_bodies(program->retired[i]);
    }
    const LhatProto **flat =
        (const LhatProto **)lhat_alloc(bodies * sizeof *flat);
    if (flat == NULL) {
        return retired;  // no room to ask safely, so keep them
    }
    size_t at = 0;
    for (size_t i = 0; i < program->retired_count; i++) {
        at = fill_bodies(program->retired[i], flat, at);
    }
    bool held = false;
    for (size_t i = 0; i < machine_count && !held; i++) {
        lhat_machine_collectgarbage(machines[i]);
        held = lhat_machine_pending_disposals(machines[i]) > 0 ||
               lhat_machine_holds_body(machines[i], flat, bodies);
    }
    lhat_free(flat);
    if (!held) {
        lhat_program_discard_retired(program);
    }
    return retired;
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

// One index over three arrays, in the order 03 の 1.1 runs them. Answers
// false past the end, which is what makes every entry point here one bounds
// test rather than three.
static bool stage_of(const LhatUnit *unit, size_t index, LhatStage *stage,
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
    if (!stage_of(unit, index, &stage, &within)) {
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
            // 07 の 4 章: what the checker underlines is the name it is
            // talking about, which is why its span is spelled name_length.
            const LhatCheckDiagnostic *d = &unit->checked.diagnostics[within];
            out.offset = d->offset;
            out.line = d->line;
            out.column = d->column;
            out.length = d->name_length;
            break;
        }
    }
    return out;
}

bool lhat_unit_diagnostic_relaxed_ok(const LhatUnit *unit, size_t index)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!stage_of(unit, index, &stage, &within) || stage != LHAT_STAGE_CHECKER) {
        return false;
    }
    // 03 の 3.1's three: a gap left in a result, a parameter or a binding,
    // reaching a place with nothing else to say about it. The only codes
    // check.c reports behind `c->strict &&` -- everything else is reported
    // the same under both, so relaxed would not have waved it through.
    switch (unit->checked.diagnostics[within].code) {
        case LHAT_CHECK_ERR_RESULT_UNDECIDED:
        case LHAT_CHECK_ERR_PARAM_UNDECIDED:
        case LHAT_CHECK_ERR_TYPE_UNDECIDED:
            return true;
        default:
            return false;
    }
}

size_t lhat_unit_diagnostic_message(const LhatUnit *unit, size_t index,
                                    char *out, size_t capacity)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!stage_of(unit, index, &stage, &within)) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }

#if !LHAT_WITH_FRONTEND
    // 10.8: a binary unit reports nothing of its own.
    (void)within;
    if (out != NULL && capacity > 0) {
        out[0] = '\0';
    }
    return 0;
#else
    switch (stage) {
        case LHAT_STAGE_LEXER: {
            // A literal, so it is copied rather than written.
            const char *text =
                lhat_lexer_error_message(unit->lexer.diagnostics[within].code);
            size_t length = strlen(text);
            if (out != NULL && capacity > 0) {
                size_t fits = length < capacity - 1 ? length : capacity - 1;
                memcpy(out, text, fits);
                out[fits] = '\0';
            }
            return length;
        }
        case LHAT_STAGE_PARSER:
            return lhat_parse_message_write(&unit->parsed.diagnostics[within],
                                            out, capacity);
        case LHAT_STAGE_CHECKER:
            return lhat_check_message_write(&unit->checked.diagnostics[within],
                                            out, capacity);
    }
    return 0;
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
    report.kind = LHAT_REPORT_ERROR;
    report.message = message;
    report.offset = d.offset;
    report.line = d.line;
    report.column = d.column;
    report.length = d.length;

    size_t written = lhat_report_write(&report, lhat_unit_source(unit),
                                       lhat_unit_path(unit), rich, out,
                                       capacity);
    if (message != room) {
        lhat_free(message);
    }
    return written;
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
        case LHAT_PROGRAM_ERR_BAD_BINARY:
            return "this binary unit was not written by this build of the "
                   "library, or has been damaged";
        case LHAT_PROGRAM_ERR_HOST_MISMATCH:
            return "this binary unit names a host type, kind or enum that "
                   "this program did not register the same way";
        case LHAT_PROGRAM_ERR_MIXED:
            return "a program is text or binary throughout; this unit is "
                   "the other kind";
        case LHAT_PROGRAM_ERR_NO_SIGNATURE:
            return "this build has no front end to read a signature with, "
                   "and the signature table does not hold this one";
        case LHAT_PROGRAM_ERR_NO_FRONTEND:
            return "this build has no front end; only a binary unit runs";
    }
    return "unknown error";
}

// ---------------------------------------------------------------------------
// 05 の 10 章: what serialize.c asks of the program
// ---------------------------------------------------------------------------

LhatUnit *lhat_program_require_unit(LhatProgram *program, LhatUnit *from,
                                    const char *relative, size_t length)
{
    char *resolved = resolve_against(from->path, relative, length);
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
    return resolve_against(from->path, relative, length);
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
    e->path = duplicate(path);
    e->name = duplicate(name);
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

bool lhat_unit_write_binary(const LhatUnit *unit, bool with_debug_names,
                            uint8_t **bytes, size_t *length)
{
#if LHAT_WITH_FRONTEND
    return lhat_serialize_write(unit, with_debug_names, bytes, length);
#else
    (void)unit;
    (void)with_debug_names;
    (void)bytes;
    (void)length;
    return false;  // 10.8: what a binary unit was written from is gone
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

static const LhatSignatureIndex *find_signature(const LhatProgram *program,
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
    const LhatSignatureIndex *e = find_signature(program, text);
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

// 02 の 18.5's places, spelled. Has to be kept in step with
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
            // 05 の 8.8改: a host whose model is a class tree writes the tree
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
    // 8.7改2: an enum is a declaration the signatures name, the same
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
                    snprintf(number, sizeof number,
                             ", \"value_kind\": \"integer\", \"value\": %lld",
                             (long long)entry->const_integer);
                    dump_text(&w, number);
                    break;
                case LHAT_HOST_CONST_REAL:
                    snprintf(number, sizeof number,
                             ", \"value_kind\": \"real\", \"value\": %.17g",
                             entry->const_real);
                    dump_text(&w, number);
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

    // 02 の 18.5: what a program may write as an annotation. The language
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
        // 18.5.1: written as the names themselves rather than as anything
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
        // 18.5.2: the same, and read back the same way. Both columns are
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
