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

#include "hover.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "position.h"
#include "resolution.h"
#include "type.h"

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
// A use is answered from the checker instead (lsp_hover_for_unit), and this
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

// 01 の 6.4: the comment block written above a definition is what it says
// about itself. There is one reading of it (ast.c), so what a hover shows and
// what a host reads through lhat_unit_documentation cannot drift apart.
static void append_documentation(cJSON *parts, const LhatUnit *unit,
                                 const LhatNode *node)
{
    size_t needed = lhat_node_documentation(node, unit->source.text,
                                            unit->source.length, NULL, 0);
    if (needed == 0) {
        return;
    }
    char *said = (char *)malloc(needed + 1);
    if (said == NULL) {
        return;
    }
    lhat_node_documentation(node, unit->source.text, unit->source.length, said,
                            needed + 1);
    cJSON_AddItemToArray(parts, cJSON_CreateString(said));
    free(said);
}

// 14.15: the member that is still a hole, in italics above the block. The
// name is the one 14.11's refusal would name (LHAT_CHECK_ERR_STILL_ABSTRACT
// reports this same member), so a reader who goes on to write new() meets the
// same word rather than a second account of the same fact.
//
// Asterisks rather than underscores: a name may begin with '_' (01 の 3.1),
// and '_(abstract: _want)_' is not the emphasis it looks like.
static void append_abstract_note(cJSON *parts, const LhatTypeMember *unfilled)
{
    static const char opening[] = "*(abstract: ";
    static const char closing[] = ")*";
    size_t room = sizeof opening - 1 + unfilled->name_length + sizeof closing;
    char *note = (char *)malloc(room);
    if (note == NULL) {
        return;
    }
    size_t used = sizeof opening - 1;
    memcpy(note, opening, used);
    memcpy(note + used, unfilled->name, unfilled->name_length);
    used += unfilled->name_length;
    memcpy(note + used, closing, sizeof closing);  // the NUL comes with it
    cJSON_AddItemToArray(parts, cJSON_CreateString(note));
    free(note);
}

cJSON *lsp_hover_for_unit(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return NULL;
    }
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);

    const LhatNode *definition = NULL;
    // Where the answer is about, when it did not come from a resolution --
    // so that the editor still underlines the name rather than guessing.
    const LhatNode *declared_name = NULL;
    if (resolved != NULL && resolved->has_definition) {
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
    // below and the line above are about.
    const LhatResolution *typed = resolved != NULL && resolved->type != NULL
                                      ? resolved
                                      : lsp_resolution_at(unit, offset);

    // A member resolves in a type rather than in a scope (14.10), so there
    // is no place in this source to show -- and for a type from another unit
    // or a host registration there is none anywhere. What it is is still
    // known, and that is the answer.
    if (definition == NULL && (typed == NULL || typed->type == NULL)) {
        return NULL;
    }

    const char *line = NULL;
    size_t line_length = 0;
    if (definition != NULL) {
        first_line(unit, definition, &line, &line_length);
    }

    // A fenced block for the definition, then whatever was written about it.
    // Markdown rather than plain text so the definition keeps the editor's
    // code styling.
    cJSON *parts = cJSON_CreateArray();
    if (parts == NULL) {
        return NULL;
    }

    // 14.15 with 14.11: a definition still holding a member nothing has
    // provided is one to compose onto, not one to make anything of -- and
    // 14.11 refuses its new. The written form does not say so anywhere the
    // reader is looking: 'Node' and 'Sprite2D' are the same three words until
    // the self^{ } section is read line by line. Above the block rather than
    // in it, since what is in it is L^ and this is about it.
    const LhatTypeMember *unfilled =
        typed != NULL ? lhat_check_unimplemented_member(typed->type) : NULL;
    if (unfilled != NULL) {
        append_abstract_note(parts, unfilled);
    }

    // 07 の the type the checker settled on, under the line as written.
    // The two say different things -- one is what the writer put down, the
    // other what was inferred from it -- and a definition with no annotation
    // has only the second.
    char inferred[LHAT_HOVER_TYPE_BUFFER];
    size_t inferred_length = 0;
    if (typed != NULL && typed->type != NULL) {
        // What it answers is how much the whole type wanted, which is more
        // than this buffer holds for a big one -- and what is *in* the
        // buffer then is the cut form ending in an ellipsis. A hover shows
        // the cut form (07 の 4 章: a shorter answer says more here), so
        // what is read back out is what fits.
        inferred_length =
            lhat_type_write(typed->type, inferred, sizeof inferred);
        if (inferred_length > sizeof inferred - 1) {
            inferred_length = strlen(inferred);
        }
    }

    size_t room = line_length + inferred_length + 32;
    char *fenced = (char *)malloc(room);
    if (fenced == NULL) {
        cJSON_Delete(parts);
        return NULL;
    }
    size_t used = 0;
    memcpy(fenced + used, "```lhat\n", 8);
    used += 8;
    if (line_length > 0) {
        memcpy(fenced + used, line, line_length);
        used += line_length;
    }
    if (inferred_length > 0) {
        // The separator only where there is a line above it to separate
        // from; a member has the type alone.
        if (line_length > 0) {
            memcpy(fenced + used, "\n", 1);
            used += 1;
        }
        memcpy(fenced + used, ": ", 2);
        used += 2;
        memcpy(fenced + used, inferred, inferred_length);
        used += inferred_length;
    }
    memcpy(fenced + used, "\n```", 5);
    cJSON_AddItemToArray(parts, cJSON_CreateString(fenced));
    free(fenced);

    if (definition != NULL) {
        append_documentation(parts, unit, definition);
    }

    // Joined with blank lines, which is how Markdown keeps them as paragraphs.
    size_t total = 0;
    int count = cJSON_GetArraySize(parts);
    for (int i = 0; i < count; i++) {
        total += strlen(cJSON_GetArrayItem(parts, i)->valuestring) + 2;
    }
    char *joined = (char *)malloc(total + 1);
    if (joined == NULL) {
        cJSON_Delete(parts);
        return NULL;
    }
    joined[0] = '\0';
    size_t filled = 0;
    for (int i = 0; i < count; i++) {
        const char *part = cJSON_GetArrayItem(parts, i)->valuestring;
        size_t length = strlen(part);
        if (i > 0) {
            memcpy(joined + filled, "\n\n", 2);
            filled += 2;
        }
        memcpy(joined + filled, part, length);
        filled += length;
    }
    joined[filled] = '\0';
    cJSON_Delete(parts);

    cJSON *hover = cJSON_CreateObject();
    if (hover == NULL) {
        free(joined);
        return NULL;
    }
    cJSON *contents = cJSON_CreateObject();
    if (contents == NULL) {
        free(joined);
        cJSON_Delete(hover);
        return NULL;
    }
    cJSON_AddItemToObject(hover, "contents", contents);
    cJSON_AddStringToObject(contents, "kind", "markdown");
    cJSON_AddStringToObject(contents, "value", joined);
    free(joined);

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
    cJSON *range = to_offset > from_offset ? cJSON_CreateObject() : NULL;
    if (range != NULL) {
        cJSON_AddItemToObject(hover, "range", range);
        LspPosition from = lsp_position_at(unit->source.text,
                                           unit->source.length, from_offset);
        LspPosition to = lsp_position_at(unit->source.text,
                                         unit->source.length, to_offset);
        cJSON *start = cJSON_CreateObject();
        cJSON *end = cJSON_CreateObject();
        cJSON_AddItemToObject(range, "start", start);
        cJSON_AddItemToObject(range, "end", end);
        cJSON_AddNumberToObject(start, "line", from.line);
        cJSON_AddNumberToObject(start, "character", from.character);
        cJSON_AddNumberToObject(end, "line", to.line);
        cJSON_AddNumberToObject(end, "character", to.character);
    }
    return hover;
}
