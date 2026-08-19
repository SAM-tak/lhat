// L^ (lhat) -- a program: one unit and everything it requires.

#include "program_internal.h"

#include <stdio.h>  // snprintf: lhat_program_dump_host_api's numbers
#include <stdlib.h>
#include <string.h>

#include "compile.h"  // 05 の 5.3: the units are compiled here too
#include "gc.h"  // LHAT_GC_BLACK -- host_error_heap の初期色 (04 の 12.4)
#include "grow.h"
#include "lhat/error.h"  // one shape for what a stage reported (03 の 1.1)
#include "lhat/port.h"
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
    require.annotations = program->annotations;  // 02 の 18.5
    require.annotation_count = program->annotation_count;

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

// 02 の 14.2: the tree of a unit already parsed, for a composition in another
// unit that has to be flattened where it is written. Everything the graph
// reached is still here -- 6.2 checked it before any of this compiled -- so
// this only has to find it.
static bool resolve_unit_body(void *context, size_t unit,
                              const LhatNode **out_statements,
                              const LhatLexer **out_lexer)
{
    Resolution *r = (Resolution *)context;
    for (LhatUnit *u = r->program->units; u != NULL; u = u->next) {
        if (!u->loaded || u->index != unit || u->parsed.root == NULL) {
            continue;
        }
        *out_statements = u->parsed.root->v.list.items;
        *out_lexer = &u->lexer;
        return *out_statements != NULL;
    }
    return false;
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
        // 14.3: the one entry with no key is the template, whose own entries
        // are the fields.
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

size_t lhat_unit_annotation_count(const LhatUnit *unit, const char *definition,
                                  const char *name)
{
    size_t count = 0;
    for (const LhatNode *at = unit_annotations_of(unit, definition, name);
         at != NULL; at = at->next) {
        count++;
    }
    return count;
}

LhatAnnotation lhat_unit_annotation(const LhatUnit *unit,
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

LhatAnnotationArgument lhat_annotation_argument(LhatAnnotation annotation,
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

size_t lhat_unit_documentation(const LhatUnit *unit, const char *definition,
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

size_t lhat_unit_member_count(const LhatUnit *unit, const char *definition)
{
    const LhatNode *binding = unit_top_binding(unit, definition);
    if (binding == NULL) {
        return 0;
    }
    size_t seen = 0;
    definition_entry_at(unit, binding->v.binding.values, (size_t)-1, &seen);
    return seen;
}

LhatUnitMember lhat_unit_member(const LhatUnit *unit, const char *definition,
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

LhatUnitParameter lhat_unit_member_parameter(const LhatUnit *unit,
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

size_t lhat_unit_member_written_name_count(const LhatUnit *unit,
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

LhatUnitText lhat_unit_member_written_name(const LhatUnit *unit,
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
    // The signature as the registration wrote it, owned; NULL for a type
    // declaration, which has none. `signature` below is what checking uses;
    // this is for writing the registration back out
    // (lhat_program_dump_host_api) -- the parsed type cannot be turned back
    // into text without losing the names it was written with (a hostdata
    // type prints structurally, an error kind loses its module prefix), so
    // the text itself is what survives.
    char *signature_text;
    LhatHostFn call;  // NULL for a type, which carries no value of its own
    void *context;
    uint8_t parameters;
    bool has_variadic;  // 13.7: the signature ended in '...' -- see LhatHost
    bool takes_self;
    bool self_last;     // 02 の 11.3改: the receiver is the right operand
    // 02 の 14.12: what install lowers into the descriptor the machine's
    // overload search reads. Points into the program's type arena, which
    // outlives every install -- nothing here is owned.
    const LhatType *signature;
    // 05 の 8.8: the tag values of this type carry. Kept on the entry so that
    // it lives as long as the program and points at the entry's own strings.
    LhatHostDataTag *tag;
} LhatHostEntry;

// 05 の 8.6: one member of L^ itself. Separate from the above because it does
// not land under L^.modules, so install puts it somewhere else.
typedef struct LhatGlobalEntry {
    char *name;  // owned
    char *signature_text;  // owned, as on LhatHostEntry
    LhatHostFn call;
    void *context;
    uint8_t parameters;
    bool has_variadic;
    bool takes_self;
    bool self_last;
    const LhatType *signature;  // 14.12, as on LhatHostEntry
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
    if (table == NULL || (table->kind != LHAT_TYPE_TABLE &&
                          table->kind != LHAT_TYPE_HOSTVALUE)) {
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

// 02 の 14.12 with 05 の 8.7: a registration's signature, built again as the
// descriptor the machine's overload search reads. A unit's own comes out of
// the compiler (compile.c's lower_type, over the syntax tree); a host has no
// body to compile, so this walks the resolved type instead.
//
// Built through lhat_machine_make_type so that the nodes land on the
// machine's heap where the collector sees them -- 8.7 keeps the heap to
// vm.c, and this side only says what shape to make.
//
// NULL means "asks nothing", which is what a kind with no runtime spelling
// answers as well as a genuine failure. Both are safe in the same way: a
// parameter whose descriptor is missing is not compared, so the search falls
// back on the counts. lower_type takes the same posture for the same reason.
#define LOWER_MAX_DEPTH 32

static LhatRuntimeType *lower_host_type(LhatMachine *machine,
                                        const LhatType *type, int depth)
{
    if (machine == NULL || type == NULL || depth > LOWER_MAX_DEPTH) {
        return NULL;
    }

    LhatRuntimeTypeKind kind;
    switch (type->kind) {
        case LHAT_TYPE_ANY:    kind = LHAT_TYPE_RT_ANY; break;
        case LHAT_TYPE_NIL:    kind = LHAT_TYPE_RT_NIL; break;
        case LHAT_TYPE_BOOL:   kind = LHAT_TYPE_RT_BOOL; break;
        case LHAT_TYPE_NUMBER: kind = LHAT_TYPE_RT_NUMBER; break;
        case LHAT_TYPE_STRING: kind = LHAT_TYPE_RT_STRING; break;
        case LHAT_TYPE_TABLE:  kind = LHAT_TYPE_RT_TABLE; break;
        case LHAT_TYPE_ERROR:  kind = LHAT_TYPE_RT_ERROR; break;
        case LHAT_TYPE_FUNC:   kind = LHAT_TYPE_RT_SUBROUTINE; break;
        case LHAT_TYPE_CORO:   kind = LHAT_TYPE_RT_COROUTINE; break;
        case LHAT_TYPE_TUPLE:  kind = LHAT_TYPE_RT_TUPLE; break;
        case LHAT_TYPE_UNION:  kind = LHAT_TYPE_RT_UNION; break;
        case LHAT_TYPE_INTERSECT: kind = LHAT_TYPE_RT_INTERSECT; break;
        case LHAT_TYPE_HOSTVALUE: kind = LHAT_TYPE_RT_HOSTVALUE; break;
        case LHAT_TYPE_HOSTVALUE_BOX:
            kind = LHAT_TYPE_RT_HOSTVALUE_BOX;
            break;
        // 04 の 2.4: a kind's identity is the LhatErrorKind the machine made,
        // and nothing here can reach one. Asks nothing rather than asking
        // something weaker than what was written.
        default:
            return NULL;
    }

    LhatRuntimeType *out = lhat_machine_make_type(machine, kind);
    if (out == NULL) {
        return NULL;
    }

    switch (type->kind) {
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
        case LHAT_TYPE_TUPLE:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                LhatRuntimeType *part =
                    lower_host_type(machine, arm->type, depth + 1);
                // An arm that asks nothing takes the whole type with it: a
                // union is only as exact as its widest arm, and one asking
                // nothing would make the union ask nothing while still
                // looking like a real question. Same for a tuple's position,
                // where the width would otherwise come out wrong.
                if (part == NULL || !lhat_type_rt_add_part(out, part)) {
                    return NULL;
                }
            }
            break;

        case LHAT_TYPE_HOSTVALUE:
        case LHAT_TYPE_HOSTVALUE_BOX:
            out->hostvalue_tag = type->v.table.hostvalue_tag;
            break;

        case LHAT_TYPE_TABLE:
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                LhatValue key = lhat_nil();
                if (!lhat_machine_make_string(machine, m->name, m->name_length,
                                              &key)) {
                    return NULL;
                }
                // 14.10 asks that the member is there; a member whose own
                // type asks nothing still says that much, so unlike a union's
                // arm this one is kept with a NULL type.
                if (!lhat_type_rt_add_member(
                        out, (const LhatString *)lhat_as_object(key),
                        lower_host_type(machine, m->type, depth + 1))) {
                    return NULL;
                }
            }
            out->variadic = lower_host_type(machine, type->v.table.variadic,
                                            depth + 1);
            // 14.16: the order a table's members come in is not the writer's,
            // so the canonical one is settled here, once.
            lhat_type_rt_sort_members(out);
            break;

        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                LhatRuntimeType *part =
                    lower_host_type(machine, p->type, depth + 1);
                if (part == NULL || !lhat_type_rt_add_part(out, part)) {
                    return NULL;
                }
            }
            out->result = lower_host_type(machine, type->v.func.result,
                                          depth + 1);
            out->variadic = lower_host_type(machine, type->v.func.variadic,
                                            depth + 1);
            out->is_function = type->v.func.is_function;
            out->takes_self = type->v.func.takes_self;
            break;

        case LHAT_TYPE_CORO:
            out->receive = lower_host_type(machine, type->v.coroutine.receive,
                                           depth + 1);
            out->produce = lower_host_type(machine, type->v.coroutine.produce,
                                           depth + 1);
            out->result = lower_host_type(machine, type->v.coroutine.result,
                                          depth + 1);
            out->is_function = type->v.coroutine.is_function;
            break;

        default:
            break;
    }
    return out;
}

// The parameter types of one registration, in order, as the array a LhatHost
// takes over. 13.4 keeps self^ out of the list either way, so the count here
// is the same one keep_entry recorded.
static LhatRuntimeType **lower_host_params(LhatMachine *machine,
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
        types[at++] = lower_host_type(machine, p->type, 0);
    }
    while (at < count) {
        types[at++] = NULL;
    }
    return types;
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
        // 14.12: kept whole rather than reduced to counts, since install
        // lowers it into the descriptor the overload search reads.
        entry->signature = signature;
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL) ||
        (signature_text != NULL && entry->signature_text == NULL)) {
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

    // 8.8: the tag is identity and nothing else, so it is made here where
    // there is exactly one per registration.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];
    entry->tag = (LhatHostDataTag *)lhat_calloc(1, sizeof *entry->tag);
    if (entry->tag == NULL) {
        return NULL;
    }
    entry->tag->module = entry->module;
    entry->tag->name = entry->name;
    // And the checker's type says which declaration it is, so what writes a
    // type out can name it rather than spelling the shape -- 8.8 puts
    // identity in the declaration, and a shape written in its place is a
    // wider type that anything of the same members satisfies.
    made->v.table.hostdata_tag = entry->tag;

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
    // 13.7: a variadic tail makes the count a floor, so one of those overlaps
    // anything at or above its own count. Kept simple: two arms are told
    // apart by their counts only where neither collects a tail.
    if (a->v.func.variadic != NULL || b->v.func.variadic != NULL) {
        return true;
    }
    const LhatTypeList *pa = a->v.func.params;
    const LhatTypeList *pb = b->v.func.params;
    for (;; pa = pa->next, pb = pb->next) {
        if (pa == NULL || pb == NULL) {
            return pa == pb;  // same length: nothing above told them apart
        }
        if (lhat_type_disjoint(pa->type, pb->type)) {
            return false;
        }
    }
}

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
    // back at the mercy of the order units are checked in.
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted);
    if (written == NULL) {
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
                                                program->hosted);
    char *kept = written != NULL ? duplicate(signature) : NULL;
    if (written == NULL || kept == NULL) {
        lhat_free(kept);
        return false;
    }
    decl->signature = written;
    program->annotation_signatures[at] = kept;
    return true;
}

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
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL, NULL)) {
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
    entry->signature_text = duplicate(signature);
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
        entry->self_last = written->v.func.self_last;
        entry->signature = written;
    }
    if (entry->name == NULL || entry->signature_text == NULL) {
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
        units.body = resolve_unit_body;
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
        LhatCompileResult compiled =
            lhat_compile_module(u->parsed.root, &u->lexer, &units, &proto);
        if (compiled.status != LHAT_COMPILE_OK) {
            // Kept so the caller can say which form stopped it rather than
            // only that something did -- and which unit, since the position
            // in it indexes that unit's source and no other.
            program->compile_status = compiled.status;
            program->compile_result = compiled;
            program->compile_unit = u;
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
        } else if (!lhat_machine_make_host(
                       machine, e->call, e->context, e->parameters,
                       e->has_variadic, e->takes_self, e->self_last,
                       lower_host_params(machine, e->signature, e->parameters),
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
        if (!lhat_machine_make_host(
                machine, e->call, e->context, e->parameters, e->has_variadic,
                e->takes_self, e->self_last,
                lower_host_params(machine, e->signature, e->parameters),
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
        lhat_free(program->host_entries[i].signature_text);
        lhat_free(program->host_entries[i].tag);
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
        lhat_free(program->global_entries[i].signature_text);
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
    }
    return "unknown error";
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

    dump_text(&w, "{\n  \"types\": [");
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
        if (entry->call != NULL || entry->type != NULL) {
            continue;
        }
        dump_comma(&w, &first);
        if (entry->tag != NULL) {
            dump_text(&w, "    {\"kind\": \"hostdata\", \"module\": ");
            dump_string(&w, entry->module);
            dump_text(&w, ", \"name\": ");
            dump_string(&w, entry->name);
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
    dump_text(&w, first ? "],\n" : "\n  ],\n");

    dump_text(&w, "  \"functions\": [");
    first = true;
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *entry = &program->host_entries[i];
        if (entry->call == NULL) {
            continue;
        }
        dump_comma(&w, &first);
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
        // 18.5改: written as the names themselves rather than as anything
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
