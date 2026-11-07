// L^ (lhat) -- LSP server: textDocument/hover ([lsp.hover]).
//
// Shows the definition a name reaches. The checker recorded where each name
// it resolved was bound (check.h's LhatResolution), so this reads that answer
// rather than working [lton.host]'s scoping out again -- a second implementation
// would be one more thing to keep in step, and would disagree exactly where
// the rules are hard.
//
// Callable declarations show one signature: written parameter names with
// checked types, without the body. Other declarations retain their source
// preview and inferred type. Documentation follows either form.
//
// The line and the comments are cut from the unit that holds the definition,
// which [modules.deps.static] lets be another unit than the one the cursor is in. So the
// answer is made in two halves (hover.h): what this unit knows, then what the
// defining one shows -- and the search for the declaring form only ever runs
// over the tree the offset belongs to.

#include "hover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "compile.h"
#include "resolution.h"
#include "type.h"
#include "util.h"

static bool append(char **buffer, size_t *used, size_t *capacity,
                   const char *text, size_t length);
static bool append_text(char **buffer, size_t *used, size_t *capacity,
                        const char *text);

typedef struct {
    char *type;
    bool named;
    bool annotation_matches;
} HoverParameter;

typedef struct LspHoverCallable {
    // A parameter-free signature supplies the modifiers and return spelling,
    // including coroutine and tuple results, from the shared type writer.
    char *empty;
    size_t prefix_length;
    HoverParameter *parameters;
    size_t count;
    bool has_source;
    uint32_t source_offset;
} LspHoverCallable;

static char *type_text(const LhatType *type)
{
    size_t size = lhat_type_write(type, NULL, 0) + 1;
    char *text = malloc(size);
    if (text != NULL) lhat_type_write(type, text, size);
    return text;
}

static void callable_dispose(LspHoverCallable *callable)
{
    if (callable == NULL) return;
    for (size_t i = 0; i < callable->count; i++)
        free(callable->parameters[i].type);
    free(callable->parameters);
    free(callable->empty);
    free(callable);
}

static LspHoverCallable *callable_parts(const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_FUNC ||
        type->specialization_base != NULL) return NULL;
    LspHoverCallable *parts = calloc(1, sizeof *parts);
    if (parts == NULL) return NULL;
    size_t count = type->v.func.takes_self + (type->v.func.variadic != NULL);
    for (const LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) count++;
    parts->parameters = calloc(count + 1, sizeof *parts->parameters);
    if (parts->parameters == NULL) { free(parts); return NULL; }
    const char *self = type->v.func.mutable_self ? "mutable^self^" : "self^";
    if (type->v.func.takes_self && !type->v.func.self_last)
        parts->parameters[parts->count++].type = lsp_strdup(self);
    for (const LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) {
        parts->parameters[parts->count++] = (HoverParameter){type_text(p->type), true, false};
    }
    if (type->v.func.variadic != NULL) {
        parts->parameters[parts->count++] = (HoverParameter){type_text(type->v.func.variadic), true, false};
    }
    if (type->v.func.takes_self && type->v.func.self_last)
        parts->parameters[parts->count++].type = lsp_strdup(self);
    LhatType empty = *type;
    empty.v.func.params = NULL;
    empty.v.func.variadic = NULL;
    empty.v.func.takes_self = false;
    parts->empty = type_text(&empty);
    parts->prefix_length = 2;
    parts->has_source = type->source_body != NULL;
    if (parts->has_source) parts->source_offset = type->source_body->offset;
    // Preserve a short written type name only when it denotes the checked
    // slot. Store the comparison, never an arena pointer across unit locks.
    const LhatNode *param = parts->has_source ? type->source_body->v.func.params : NULL;
    const LhatTypeList *slot = type->v.func.params;
    for (size_t i = 0; param != NULL && i < parts->count; i++, param = param->next) {
        if (!parts->parameters[i].named) continue;
        const LhatType *inferred = param->v.param.variadic ? type->v.func.variadic
                                      : slot != NULL ? slot->type : NULL;
        const LhatNode *annotation = param->v.param.type;
        parts->parameters[i].annotation_matches = annotation != NULL &&
            lhat_type_equal(inferred, annotation->checked_type);
        if (!param->v.param.variadic && slot != NULL) slot = slot->next;
    }
    bool ok = parts->empty != NULL;
    for (size_t i = 0; i < parts->count; i++) ok = ok && parts->parameters[i].type != NULL;
    if (!ok) { callable_dispose(parts); return NULL; }
    return parts;
}

// Keep source offsets within their owning unit, including malformed edits.
static bool append_source(char **text, size_t *used, size_t *capacity,
                          const LhatUnit *unit, uint32_t start, uint32_t end)
{
    return start <= end && end <= unit->source.length &&
           append(text, used, capacity, unit->source.text + start, end - start);
}

// The declaration's selected target and value, not its first sibling.
static const LhatNode *declaration_value(const LhatNode *node, uint32_t offset,
                                         const LhatNode **name)
{
    if (node == NULL) return NULL;
    if (node->kind == LHAT_NODE_DEFINE || node->kind == LHAT_NODE_REASSIGN) {
        const LhatNode *value = node->v.binding.values;
        for (const LhatNode *target = node->v.binding.targets; target != NULL;
             target = target->next) {
            const LhatNode *n = target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
            if (n != NULL && offset >= lhat_node_span_start(n) && offset < n->end) {
                *name = n;
                return value;
            }
            if (value != NULL) value = value->next;
        }
    } else if (node->kind == LHAT_NODE_TABLE_ENTRY && !node->v.entry.declared) {
        *name = node->v.entry.key;
        return node->v.entry.value;
    }
    return NULL;
}

static char *callable_summary(const LhatUnit *unit, const LhatNode *node,
                              const LspHoverPart *part)
{
    const LhatNode *name = NULL;
    const LhatNode *func = declaration_value(node, part->definition, &name);
    const LspHoverCallable *parts = part->callable;
    if (name == NULL || (parts == NULL &&
        (func == NULL || func->kind != LHAT_NODE_FUNC || part->type == NULL))) return NULL;
    // An overload selected at a call may belong to a different declaration.
    // Do not borrow parameter names from an unrelated arm.
    bool names = parts != NULL && func != NULL && func->kind == LHAT_NODE_FUNC &&
                 (!parts->has_source || parts->source_offset == func->offset);
    size_t count = 0;
    if (names) {
        for (const LhatNode *p = func->v.func.params; p != NULL; p = p->next) count++;
        names = count == parts->count;
    }

    char *text = NULL;
    size_t used = 0, capacity = 0;
    uint32_t prefix_end = lhat_node_span_start(name);
    if (node->kind == LHAT_NODE_DEFINE || node->kind == LHAT_NODE_REASSIGN)
        prefix_end = lhat_node_span_start(node->v.binding.targets);
    bool ok = append_source(&text, &used, &capacity, unit, lhat_node_span_start(node), prefix_end) &&
              append_source(&text, &used, &capacity, unit, lhat_node_span_start(name), name->end);
    if (!names) {
        ok = ok && append_text(&text, &used, &capacity, ": ") &&
             part->type != NULL && append_text(&text, &used, &capacity, part->type);
        if (!ok) { free(text); return NULL; }
        return text;
    }
    ok = ok && append_text(&text, &used, &capacity, " = ") &&
         append(&text, &used, &capacity, parts->empty, parts->prefix_length);
    const LhatNode *param = func->v.func.params;
    for (size_t i = 0; ok && i < parts->count; i++, param = param->next) {
        if (i != 0) ok = append_text(&text, &used, &capacity, ", ");
        if (parts->parameters[i].named) {
            const LhatNode *n = param->v.param.name;
            ok = ok && (param->v.param.variadic
                ? append_text(&text, &used, &capacity, "...")
                : n != NULL && append_source(&text, &used, &capacity, unit,
                                             lhat_node_span_start(n), n->end));
            ok = ok && append_text(&text, &used, &capacity, ":");
        }
        const LhatNode *annotation = param->v.param.type;
        uint32_t start = annotation != NULL ? lhat_node_span_start(annotation) : 0;
        bool written = parts->parameters[i].annotation_matches && annotation != NULL &&
            annotation->end >= start && annotation->end <= unit->source.length &&
            annotation->end - start < strlen(parts->parameters[i].type) &&
            annotation->end - start <= 120 &&
            memchr(unit->source.text + start, '\n', annotation->end - start) == NULL;
        ok = ok && (written ? append_source(&text, &used, &capacity, unit, start, annotation->end)
                            : append_text(&text, &used, &capacity, parts->parameters[i].type));
        if (param->v.param.fallback != NULL)
            ok = ok && append_text(&text, &used, &capacity, " = …");
    }
    ok = ok && append_text(&text, &used, &capacity, parts->empty + parts->prefix_length);
    if (!ok) { free(text); return NULL; }
    return text;
}

// The statement that introduced the name at `offset` -- the innermost node
// whose span covers it and which is a form that binds. Found by walking, since
// a binding records only where its name stands.
typedef struct {
    uint32_t wanted;
    const LhatNode *best;
} DefinitionSearch;

static bool binds_a_name(const LhatNode *node)
{
    switch (node->kind) {
        case LHAT_NODE_DEFINE:
        case LHAT_NODE_REASSIGN:
        case LHAT_NODE_PARAM:
        case LHAT_NODE_ERRORDEF:
        case LHAT_NODE_ERROR_KIND:
        case LHAT_NODE_ENUMDEF:
        case LHAT_NODE_ENUM_MEMBER:
        case LHAT_NODE_MODULE:
        case LHAT_NODE_TABLE_ENTRY:
        case LHAT_NODE_MEMBER_DECL:
        case LHAT_NODE_IMPORT_STMT:
        case LHAT_NODE_REQUIRE_STMT:
            return true;
        default:
            return false;
    }
}

static void search_child(void *context, const char *field, bool in_list,
                         const LhatNode *child);

static void search(const LhatNode *node, DefinitionSearch *state)
{
    uint32_t start = lhat_node_span_start(node);
    if (state->wanted < start || state->wanted >= node->end) {
        return;
    }
    // Innermost wins: this runs before descending, so a child that also
    // covers the position replaces what was found here.
    if (binds_a_name(node)) {
        state->best = node;
    }
    // [syntax.stmt.define] with [syntax.type.signature]: 'let^ x:number^ = 1' spells its target as a PARAM, but
    // the form that declares x is the let^ -- its first line is what the
    // reader wants, and [lex.comment.kept]'s comment above it is attached to it, not
    // to the annotation. So a definition's targets are not descended into;
    // what it binds the name *to* still is, since a def^ or an f^ standing
    // there declares members and parameters of its own.
    if (node->kind == LHAT_NODE_DEFINE) {
        for (const LhatNode *value = node->v.binding.values; value != NULL;
             value = value->next) {
            search(value, state);
        }
        return;
    }
    lhat_node_visit_children(node, search_child, state);
}

static void search_child(void *context, const char *field, bool in_list,
                         const LhatNode *child)
{
    (void)field;
    (void)in_list;
    search(child, (DefinitionSearch *)context);
}

// The innermost written name covering `offset`. Used to keep the search
// below to names: a binding's span runs to the end of what it binds, and
// answering anywhere inside that would put a definition under the cursor
// over every space in a body pages long.
typedef struct {
    uint32_t wanted;
    const LhatNode *best;
} NameSearch;

// A hat identifier is not among these. [lex.hat.no-reserved] makes it the spelling every
// keyword takes, and no form declares one -- there is no 'let^ true^ = …' --
// so a position on one has no declaration to find. Where a hat identifier
// does name something bound (self^, class^), it is a use and the checker
// answered for it already, before this fallback is reached.
static bool is_written_name(const LhatNode *node)
{
    switch (node->kind) {
        case LHAT_NODE_IDENT:
        case LHAT_NODE_NAME:
        case LHAT_NODE_TYPE_NAME:
            return true;
        default:
            return false;
    }
}

static void name_child(void *context, const char *field, bool in_list,
                       const LhatNode *child);

static void name_search(const LhatNode *node, NameSearch *state)
{
    uint32_t start = lhat_node_span_start(node);
    if (state->wanted < start || state->wanted >= node->end) {
        return;
    }
    if (is_written_name(node)) {
        state->best = node;
    }
    lhat_node_visit_children(node, name_child, state);
}

static void name_child(void *context, const char *field, bool in_list,
                       const LhatNode *child)
{
    (void)field;
    (void)in_list;
    name_search(child, (NameSearch *)context);
}

// A declaration declares a name rather than using one, so the checker
// records no resolution against it -- 07 の module^ is the plainest case,
// and 'let^ x = …' or a parameter is the same. Those are found by position:
// the innermost form that binds, around the name the cursor is actually on.
//
// A use is answered from the checker instead (lsp_hover_locate), and this
// runs only where no resolution covers the offset, so the two never
// disagree about the same position.
static const LhatNode *declaration_at(const LhatNode *root, uint32_t offset,
                                      const LhatNode **name)
{
    NameSearch found = {offset, NULL};
    name_search(root, &found);
    if (found.best == NULL) {
        return NULL;
    }
    DefinitionSearch state = {lhat_node_span_start(found.best), NULL};
    search(root, &state);
    *name = found.best;
    return state.best;
}

// The first line of what a node covers, so that a definition whose body runs
// for pages still shows as one line.
static void first_line(const LhatUnit *unit, const LhatNode *node,
                       const char **text, size_t *length)
{
    uint32_t start = lhat_node_span_start(node);
    uint32_t end = node->end;
    if (end > unit->source.length) {
        end = (uint32_t)unit->source.length;
    }
    const char *from = unit->source.text + start;
    size_t span = end > start ? end - start : 0;
    const char *newline = (const char *)memchr(from, '\n', span);
    if (newline != NULL) {
        span = (size_t)(newline - from);
    }
    // A trailing '{' left dangling reads worse than one kept, so nothing is
    // trimmed beyond the whitespace before the break.
    while (span > 0 && (from[span - 1] == ' ' || from[span - 1] == '\r')) {
        span--;
    }
    *text = from;
    *length = span;
}

// What the defining unit shows: a callable summary or a source preview,
// and the comment block written above it,
// which is what it says about itself. There is one reading of that block
// (ast.c), so what a hover shows and what a host reads through
// lhat_unit_documentation cannot drift apart. Both copied out, since the
// unit is only safe to read while it is held (hover.h).
static void describe_node(const LhatUnit *unit, const LhatNode *node,
                          LspHoverPart *part)
{
    // An unused declaration has no use resolution, but its literal was checked.
    if (part->type == NULL && part->callable == NULL) {
        const LhatNode *name = NULL;
        const LhatNode *value = declaration_value(node, part->definition, &name);
        if (value != NULL && value->kind == LHAT_NODE_FUNC) {
            part->callable = callable_parts(value->checked_type);
            if (part->callable != NULL) part->type = type_text(value->checked_type);
        }
    }
    const char *line = NULL;
    size_t line_length = 0;
    first_line(unit, node, &line, &line_length);
    free(part->line);
    part->line = callable_summary(unit, node, part);
    if (part->line != NULL) {
        free(part->type);
        part->type = NULL;
    } else {
        part->line = lsp_strndup(line, line_length);
    }

    free(part->documentation);
    part->documentation = NULL;
    size_t needed = lhat_node_documentation(node, unit->source.text,
                                            unit->source.length, NULL, 0);
    if (needed > 0) {
        part->documentation = (char *)malloc(needed + 1);
        if (part->documentation != NULL) {
            lhat_node_documentation(node, unit->source.text,
                                    unit->source.length, part->documentation,
                                    needed + 1);
        }
    }
}

// [syntax.obj.abstract]: the member that is still a hole, in italics above the block. The
// name is the one [syntax.obj.new]'s refusal would name (LHAT_CHECK_ERR_STILL_ABSTRACT
// reports this same member), so a reader who goes on to write new() meets the
// same word rather than a second account of the same fact.
//
// Asterisks rather than underscores: a name may begin with '_' ([lex.ident.chars]),
// and '_(abstract: _want)_' is not the emphasis it looks like.
static char *abstract_note_for(const LhatTypeMember *unfilled)
{
    static const char opening[] = "*(abstract: ";
    static const char closing[] = ")*";
    size_t room = sizeof opening - 1 + unfilled->name_length + sizeof closing;
    char *note = (char *)malloc(room);
    if (note == NULL) {
        return NULL;
    }
    size_t used = sizeof opening - 1;
    memcpy(note, opening, used);
    memcpy(note + used, unfilled->name, unfilled->name_length);
    used += unfilled->name_length;
    memcpy(note + used, closing, sizeof closing);  // the NUL comes with it
    return note;
}

typedef struct {
    uint32_t offset;
    const LhatNode *test;
    const LhatNode *asked;
} FitsSearch;

static void fits_search(const LhatNode *node, FitsSearch *state);

static void fits_search_child(void *context, const char *field, bool in_list,
                              const LhatNode *child)
{
    (void)field;
    (void)in_list;
    fits_search(child, (FitsSearch *)context);
}

static void fits_search(const LhatNode *node, FitsSearch *state)
{
    if (node == NULL || state->offset < lhat_node_span_start(node) ||
        state->offset >= node->end) return;
    if (node->kind == LHAT_NODE_BINARY && node->v.binary.op == LHAT_OP_FITS &&
        state->offset >= node->offset && state->offset < node->offset + 5) {
        state->test = node;
        state->asked = node->v.binary.right;
    } else if (node->kind == LHAT_NODE_COMPARE_CHAIN) {
        const LhatNode *operand = node->v.chain.operands;
        for (const LhatNode *op = node->v.chain.operators;
             op != NULL && operand != NULL; op = op->next) {
            operand = operand->next;
            if (op->v.unary.op == LHAT_OP_FITS &&
                state->offset >= op->offset && state->offset < op->offset + 5) {
                state->test = op;
                state->asked = operand;
            }
        }
    }
    lhat_node_visit_children(node, fits_search_child, state);
}

static bool fits_hover(const LhatUnit *unit, uint32_t offset, LspHoverPart *out)
{
    FitsSearch found = {offset, NULL, NULL};
    fits_search(unit->parsed.root, &found);
    if (found.test == NULL || found.asked == NULL ||
        found.test->offset + 5 > unit->source.length ||
        memcmp(unit->source.text + found.test->offset, "fits^", 5) != 0)
        return false; // Do not expose a desugared test as a written operator.
    int answer = lhat_compile_fits_answer(&unit->lexer, found.asked,
                                          found.test->checked_fits_type);
    out->line = lsp_strdup("fits^");
    out->fits_evaluation = answer < 0 ? LSP_HOVER_FITS_RUNTIME
        : answer ? LSP_HOVER_FITS_TRUE : LSP_HOVER_FITS_FALSE;
    out->has_range = true;
    out->from = lsp_unit_position_at(unit, found.test->offset);
    out->to = lsp_unit_position_at(unit, found.test->offset + 5);
    return true;
}

// [lsp.hover.try]: one line of what a hover on a try^ says. Strings and numbers
// only, since a part may outlive the unit it was read off.
typedef struct LspHoverTryRoute {
    char *errors;  // NULL for the errors of the body's own call
    LhatTryRouteKind kind;
    int line;
    int column;
    char *target;  // how the place reads; NULL when out of the unit
} LspHoverTryRoute;

static char *try_target_text(const LhatUnit *unit, const LhatTryRoute *route)
{
    if (route->kind == LHAT_TRY_TO_BODY) {
        return lsp_strdup(route->target->v.func.is_function ? "f^" : "p^");
    }
    if (route->kind == LHAT_TRY_TO_UNIT) {
        return NULL;
    }
    const LhatNode *condition = route->target->v.clause.condition;
    if (condition == NULL) {
        return lsp_strdup("catch^:");
    }
    uint32_t from = lhat_node_span_start(condition);
    size_t length = condition->end - from;
    char *text = malloc(sizeof "catch^ :" + length);
    if (text != NULL) {
        sprintf(text, "catch^ %.*s:", (int)length, unit->source.text + from);
    }
    return text;
}

static bool try_hover(const LhatUnit *unit, uint32_t offset, LspHoverPart *out)
{
    size_t count = 0;
    uint32_t at = 0;
    const LhatTryRoute *route;
    for (size_t cursor = 0;
         (route = lhat_check_try_route_at(&unit->checked, offset, &cursor)) != NULL;) {
        at = route->at;
        count++;
    }
    if (count == 0) {
        return false;
    }
    out->routes = calloc(count, sizeof *out->routes);
    if (out->routes == NULL) {
        return false;
    }
    size_t cursor = 0;
    while ((route = lhat_check_try_route_at(&unit->checked, offset, &cursor)) != NULL) {
        LspHoverTryRoute *line = &out->routes[out->route_count++];
        line->kind = route->kind;
        line->errors = route->errors != NULL ? type_text(route->errors) : NULL;
        line->target = try_target_text(unit, route);
        if (route->target != NULL) {
            LspPosition place = lsp_unit_position_at(unit, route->target->offset);
            line->line = place.line + 1;
            line->column = place.character + 1;
        }
    }
    out->has_range = true;
    out->from = lsp_unit_position_at(unit, at);
    out->to = lsp_unit_position_at(unit, at + (uint32_t)(sizeof "try^" - 1));
    return true;
}

bool lsp_hover_locate(const LhatUnit *unit, uint32_t offset, LspHoverPart *out)
{
    memset(out, 0, sizeof *out);
    if (unit == NULL || unit->parsed.root == NULL) {
        return false;
    }
    if (fits_hover(unit, offset, out)) return true;
    if (try_hover(unit, offset, out)) return true;
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);

    // [modules.deps.static]: a definition in another unit is an offset into that unit's
    // text, and nothing in this tree stands at it -- searching here would
    // find whatever happens to cover the same number.
    bool elsewhere = resolved != NULL && resolved->has_definition &&
                     resolved->definition_path != NULL &&
                     (unit->path == NULL ||
                      strcmp(resolved->definition_path, unit->path) != 0);

    const LhatNode *definition = NULL;
    // Where the answer is about, when it did not come from a resolution --
    // so that the editor still underlines the name rather than guessing.
    const LhatNode *declared_name = NULL;
    if (resolved != NULL && resolved->has_definition && !elsewhere) {
        DefinitionSearch state = {resolved->definition, NULL};
        search(unit->parsed.root, &state);
        definition = state.best;
    } else if (resolved == NULL) {
        // Not a name that was used: a declaration, which says what it is
        // where it stands.
        definition = declaration_at(unit->parsed.root, offset, &declared_name);
    }
    // A declaration binds a name rather than resolving one, so nothing is
    // recorded against it -- and standing on the name a let^ introduces is
    // where a reader asks what it is. resolution.h finds the answer under
    // another key: a use pointing back here. Only the type is taken from it;
    // where the name stands is still this position, which is what the range
    // below and the line are about.
    const LhatResolution *typed = resolved != NULL && resolved->type != NULL
                                      ? resolved
                                      : lsp_resolution_at(unit, offset);

    // A member resolves in a type rather than in a scope ([syntax.obj.structure]), so there
    // may be no place in this source to show -- and for a host registration
    // there is none anywhere. What it is is still known, and that is the
    // answer.
    if (definition == NULL && !elsewhere &&
        (typed == NULL || typed->type == NULL)) {
        return false;
    }

    if (typed != NULL && typed->type != NULL) {
        // Keep structural elision, but allocate the measured size so a long
        // parameter cannot cut off the return type or closing punctuation.
        const LhatType *shown = resolved != NULL && resolved->call_signature != NULL
                                   ? resolved->call_signature : typed->type;
        out->type = type_text(shown);
        out->callable = callable_parts(shown);
        // [syntax.obj.abstract] with [syntax.obj.new]: a definition still holding a member nothing has
        // provided is one to compose onto, not one to make anything of --
        // and [syntax.obj.new] refuses its new. The written form does not say so
        // anywhere the reader is looking: 'Node' and 'Sprite2D' are the same
        // three words until the self^{ } section is read line by line.
        const LhatTypeMember *unfilled =
            lhat_check_unimplemented_member(typed->type);
        if (unfilled != NULL) {
            out->abstract_note = abstract_note_for(unfilled);
        }
    }

    // The range of the name itself, so the editor underlines what was asked
    // about rather than guessing a word boundary. A use has it from the
    // resolution; a declaration from the name the search settled on.
    uint32_t from_offset = 0;
    uint32_t to_offset = 0;
    if (resolved != NULL) {
        from_offset = resolved->use;
        to_offset = resolved->use_end;
    } else if (declared_name != NULL) {
        from_offset = lhat_node_span_start(declared_name);
        to_offset = declared_name->end;
    }
    if (to_offset > from_offset) {
        out->has_range = true;
        out->from = lsp_unit_position_at(unit, from_offset);
        out->to = lsp_unit_position_at(unit, to_offset);
    }

    if (elsewhere) {
        out->has_definition = true;
        out->definition = resolved->definition;
        out->definition_path = lsp_strdup(resolved->definition_path);
    } else if (definition != NULL) {
        out->has_definition = true;
        out->definition = resolved != NULL ? resolved->definition
                              : lhat_node_span_start(declared_name);
        describe_node(unit, definition, out);
    }
    return true;
}

void lsp_hover_describe(const LhatUnit *defining, LspHoverPart *part)
{
    if (defining == NULL || defining->parsed.root == NULL ||
        !part->has_definition) {
        return;
    }
    DefinitionSearch state = {part->definition, NULL};
    search(defining->parsed.root, &state);
    const LhatNode *node = state.best;
    // A require^'s path points at the unit's start (check.h), and a unit that
    // opens with a comment or a blank line has nothing standing at byte 0.
    // What there is to show of it is its first statement -- the module^ when
    // it declares one, whose comment is the unit's own description ([lsp.hover]
    // 章).
    if (node == NULL && part->definition == 0) {
        const LhatNode *root = defining->parsed.root;
        node = root->kind == LHAT_NODE_BLOCK ? root->v.list.items : root;
    }
    if (node != NULL) {
        describe_node(defining, node, part);
    }
}

// Appends `text` to a growing buffer; false when out of memory.
static bool append(char **buffer, size_t *used, size_t *capacity,
                   const char *text, size_t length)
{
    if (*used + length + 1 > *capacity) {
        size_t wanted = (*used + length + 1) * 2;
        char *grown = (char *)realloc(*buffer, wanted);
        if (grown == NULL) {
            return false;
        }
        *buffer = grown;
        *capacity = wanted;
    }
    memcpy(*buffer + *used, text, length);
    *used += length;
    (*buffer)[*used] = '\0';
    return true;
}

static bool append_text(char **buffer, size_t *used, size_t *capacity,
                        const char *text)
{
    return append(buffer, used, capacity, text, strlen(text));
}

// [lsp.hover.try]: a heading, then a line for each place in the order the arms stand.
static bool append_try_routes(char **buffer, size_t *used, size_t *capacity,
                              const LspHoverPart *part, bool japanese)
{
    bool ok = append_text(buffer, used, capacity,
                          japanese ? "**誤りの行き先**\n" : "**Where errors go**\n");
    for (size_t i = 0; ok && i < part->route_count; i++) {
        const LspHoverTryRoute *route = &part->routes[i];
        ok = append_text(buffer, used, capacity, "\n- ");
        if (route->errors != NULL) {
            ok = ok && append_text(buffer, used, capacity, "`") &&
                 append_text(buffer, used, capacity, route->errors) &&
                 append_text(buffer, used, capacity, "`");
        } else {
            ok = ok && append_text(buffer, used, capacity,
                                   japanese ? "再帰呼び出しの誤り"
                                            : "errors of the recursive call");
        }
        if (route->kind == LHAT_TRY_TO_UNIT) {
            ok = ok && append_text(buffer, used, capacity,
                                   japanese
                                       ? " → 単位の外（実行元、または `require^` の値へ）"
                                       : " → out of the unit (to what ran it, or the value of its `require^`)");
            continue;
        }
        char place[32];
        snprintf(place, sizeof place, " → %d:%d `", route->line, route->column);
        ok = ok && append_text(buffer, used, capacity, place) &&
             route->target != NULL &&
             append_text(buffer, used, capacity, route->target) &&
             append_text(buffer, used, capacity, "`");
        if (route->kind == LHAT_TRY_TO_BODY) {
            ok = ok && append_text(buffer, used, capacity,
                                   japanese ? "（返り値に合流）" : " (joins its result)");
        }
    }
    return ok;
}

cJSON *lsp_hover_render(const LspHoverPart *part)
{
    return lsp_hover_render_localized(part, NULL);
}

cJSON *lsp_hover_render_localized(const LspHoverPart *part, const char *language)
{
    if (part->line == NULL && part->type == NULL && part->route_count == 0) {
        return NULL;
    }
    bool japanese = language != NULL &&
        (strcmp(language, "ja") == 0 || strncmp(language, "ja-", 3) == 0);

    // The note, then a fenced block for the definition line with the type
    // under it, then whatever was written about it -- joined with blank lines,
    // which is how Markdown keeps them as paragraphs. Markdown rather than
    // plain text so the definition keeps the editor's code styling.
    char *joined = NULL;
    size_t used = 0;
    size_t capacity = 0;
    bool ok = true;
    if (part->abstract_note != NULL) {
        ok = ok && append_text(&joined, &used, &capacity, part->abstract_note) &&
             append_text(&joined, &used, &capacity, "\n\n");
    }
    if (part->route_count > 0) {
        ok = ok && append_try_routes(&joined, &used, &capacity, part, japanese);
    } else {
        ok = ok && append_text(&joined, &used, &capacity, "```lhat\n");
        if (part->line != NULL) {
            ok = ok && append_text(&joined, &used, &capacity, part->line);
        }
        if (part->type != NULL) {
            // The separator only where there is a line above it to separate
            // from; a member has the type alone.
            ok = ok &&
                 append_text(&joined, &used, &capacity,
                             part->line != NULL ? "\n: " : ": ") &&
                 append_text(&joined, &used, &capacity, part->type);
        }
        ok = ok && append_text(&joined, &used, &capacity, "\n```");
    }
    const char *documentation = part->documentation;
    if (part->fits_evaluation != LSP_HOVER_NOT_FITS) {
        documentation = part->fits_evaluation == LSP_HOVER_FITS_RUNTIME
            ? (japanese ? "実行時に判定します。" : "Runtime: tests the value at runtime.")
            : part->fits_evaluation == LSP_HOVER_FITS_TRUE
            ? (japanese ? "コンパイル時に確定します: `true^`。" : "Compile-time: always `true^`.")
            : (japanese ? "コンパイル時に確定します: `false^`。" : "Compile-time: always `false^`.");
    }
    if (documentation != NULL) {
        ok = ok && append_text(&joined, &used, &capacity, "\n\n") &&
             append_text(&joined, &used, &capacity, documentation);
    }
    if (!ok) {
        free(joined);
        return NULL;
    }

    cJSON *hover = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateObject();
    if (hover == NULL || contents == NULL) {
        cJSON_Delete(hover);
        cJSON_Delete(contents);
        free(joined);
        return NULL;
    }
    cJSON_AddItemToObject(hover, "contents", contents);
    cJSON_AddStringToObject(contents, "kind", "markdown");
    cJSON_AddStringToObject(contents, "value", joined);
    free(joined);

    if (part->has_range) {
        cJSON_AddItemToObject(hover, "range",
                              lsp_range_json(part->from, part->to));
    }
    return hover;
}

void lsp_hover_part_dispose(LspHoverPart *part)
{
    free(part->type);
    callable_dispose(part->callable);
    free(part->abstract_note);
    free(part->definition_path);
    free(part->line);
    free(part->documentation);
    for (size_t i = 0; i < part->route_count; i++) {
        free(part->routes[i].errors);
        free(part->routes[i].target);
    }
    free(part->routes);
    memset(part, 0, sizeof *part);
}

cJSON *lsp_hover_for_unit(const LhatUnit *unit, uint32_t offset)
{
    LspHoverPart part;
    cJSON *hover = lsp_hover_locate(unit, offset, &part)
                       ? lsp_hover_render(&part)
                       : NULL;
    lsp_hover_part_dispose(&part);
    return hover;
}
