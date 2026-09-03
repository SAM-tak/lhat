// L^ (lhat) -- LSP server: textDocument/hover (07 の 4 章).
//
// Shows the definition a name reaches. The checker recorded where each name
// it resolved was bound (check.h's LhatResolution), so this reads that answer
// rather than working 8 章's scoping out again -- a second implementation
// would be one more thing to keep in step, and would disagree exactly where
// the rules are hard.
//
// What is shown is the source that introduced the name, cut to its first
// line, then the type the checker settled on, then the comments written
// against it. The line and the type say different things -- one is what the
// writer put down, the other what was made of it -- and a definition with no
// annotation has only the second.
//
// The line and the comments are cut from the unit that holds the definition,
// which 05 の 6.1 lets be another unit than the one the cursor is in. So the
// answer is made in two halves (hover.h): what this unit knows, then what the
// defining one shows -- and the search for the declaring form only ever runs
// over the tree the offset belongs to.

#include "hover.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "resolution.h"
#include "type.h"
#include "util.h"

// A type written out for one hover. Past this a reader is not helped by more,
// and lhat_type_write cuts with an ellipsis rather than refusing.
#define LHAT_HOVER_TYPE_BUFFER 512

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
    // 8.6 with 13.1: 'let^ x:number^ = 1' spells its target as a PARAM, but
    // the form that declares x is the let^ -- its first line is what the
    // reader wants, and 01 の 6.4's comment above it is attached to it, not
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

// A hat identifier is not among these. 01 の 2.1 makes it the spelling every
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

// What the defining unit shows of a definition: the first line of the form
// that declared it, and -- 01 の 6.4 -- the comment block written above it,
// which is what it says about itself. There is one reading of that block
// (ast.c), so what a hover shows and what a host reads through
// lhat_unit_documentation cannot drift apart. Both copied out, since the
// unit is only safe to read while it is held (hover.h).
static void describe_node(const LhatUnit *unit, const LhatNode *node,
                          LspHoverPart *part)
{
    const char *line = NULL;
    size_t line_length = 0;
    first_line(unit, node, &line, &line_length);
    free(part->line);
    part->line = lsp_strndup(line, line_length);

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

// 14.15: the member that is still a hole, in italics above the block. The
// name is the one 14.11's refusal would name (LHAT_CHECK_ERR_STILL_ABSTRACT
// reports this same member), so a reader who goes on to write new() meets the
// same word rather than a second account of the same fact.
//
// Asterisks rather than underscores: a name may begin with '_' (01 の 3.1),
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

bool lsp_hover_locate(const LhatUnit *unit, uint32_t offset, LspHoverPart *out)
{
    memset(out, 0, sizeof *out);
    if (unit == NULL || unit->parsed.root == NULL) {
        return false;
    }
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);

    // 05 の 6.1: a definition in another unit is an offset into that unit's
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

    // A member resolves in a type rather than in a scope (14.10), so there
    // may be no place in this source to show -- and for a host registration
    // there is none anywhere. What it is is still known, and that is the
    // answer.
    if (definition == NULL && !elsewhere &&
        (typed == NULL || typed->type == NULL)) {
        return false;
    }

    if (typed != NULL && typed->type != NULL) {
        // What it answers is how much the whole type wanted, which is more
        // than this buffer holds for a big one -- and what is *in* the
        // buffer then is the cut form ending in an ellipsis. A hover shows
        // the cut form (07 の 4 章: a shorter answer says more here), so
        // what is read back out is what fits.
        char inferred[LHAT_HOVER_TYPE_BUFFER];
        size_t length = lhat_type_write(typed->type, inferred, sizeof inferred);
        if (length > sizeof inferred - 1) {
            length = strlen(inferred);
        }
        out->type = lsp_strndup(inferred, length);
        // 14.15 with 14.11: a definition still holding a member nothing has
        // provided is one to compose onto, not one to make anything of --
        // and 14.11 refuses its new. The written form does not say so
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
        out->definition = lhat_node_span_start(definition);
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
    // it declares one, whose comment is the unit's own description (07 の 4
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

cJSON *lsp_hover_render(const LspHoverPart *part)
{
    if (part->line == NULL && part->type == NULL) {
        return NULL;
    }

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
    if (part->documentation != NULL) {
        ok = ok && append_text(&joined, &used, &capacity, "\n\n") &&
             append_text(&joined, &used, &capacity, part->documentation);
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
        cJSON *range = cJSON_CreateObject();
        cJSON *start = cJSON_CreateObject();
        cJSON *end = cJSON_CreateObject();
        cJSON_AddItemToObject(hover, "range", range);
        cJSON_AddItemToObject(range, "start", start);
        cJSON_AddItemToObject(range, "end", end);
        cJSON_AddNumberToObject(start, "line", part->from.line);
        cJSON_AddNumberToObject(start, "character", part->from.character);
        cJSON_AddNumberToObject(end, "line", part->to.line);
        cJSON_AddNumberToObject(end, "character", part->to.character);
    }
    return hover;
}

void lsp_hover_part_dispose(LspHoverPart *part)
{
    free(part->type);
    free(part->abstract_note);
    free(part->definition_path);
    free(part->line);
    free(part->documentation);
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
