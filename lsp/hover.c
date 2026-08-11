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

// 07 の module^ says what the unit is, so a comment written against it is
// the unit's own description. It declares rather than uses a name, so the
// checker records no resolution for it and it is found by position instead.
static const LhatNode *module_at(const LhatNode *root, uint32_t offset)
{
    for (const LhatNode *s = root->v.list.items; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_MODULE &&
            offset >= lhat_node_span_start(s) && offset < s->end) {
            return s;
        }
    }
    return NULL;
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

#ifdef LHAT_WITH_COMMENTS
// 01 の 6.4: the comment block written above a definition is what it says
// about itself. The markers are stripped so the text reads as prose.
static void append_comments(cJSON *lines, const LhatUnit *unit,
                            const LhatNode *node)
{
    for (const LhatComment *c = node->comments; c != NULL;
         c = c->next_for_node) {
        uint32_t start = c->offset;
        uint32_t end = c->end;
        if (end > unit->source.length || start >= end) {
            continue;
        }
        const char *from = unit->source.text + start;
        size_t span = end - start;
        // '#[' … ']#' or '#' … end of line.
        if (span >= 2 && from[0] == '#' && from[1] == '[') {
            from += 2;
            span -= (span >= 4 ? 4 : 2);
        } else if (span >= 1 && from[0] == '#') {
            from += 1;
            span -= 1;
        }
        while (span > 0 && (*from == ' ' || *from == '\t')) {
            from++;
            span--;
        }
        while (span > 0 && (from[span - 1] == ' ' || from[span - 1] == '\r' ||
                            from[span - 1] == '\n')) {
            span--;
        }
        if (span == 0) {
            continue;
        }
        char *copy = (char *)malloc(span + 1);
        if (copy == NULL) {
            return;
        }
        memcpy(copy, from, span);
        copy[span] = '\0';
        cJSON_AddItemToArray(lines, cJSON_CreateString(copy));
        free(copy);
    }
}
#endif

cJSON *lsp_hover_for_unit(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return NULL;
    }
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);

    const LhatNode *definition = NULL;
    if (resolved != NULL) {
        DefinitionSearch state = {resolved->definition, NULL};
        search(unit->parsed.root, &state);
        definition = state.best;
    } else {
        // Not a name that was used. The one declaration worth showing on its
        // own is module^.
        definition = module_at(unit->parsed.root, offset);
    }
    if (definition == NULL) {
        return NULL;
    }

    const char *line = NULL;
    size_t line_length = 0;
    first_line(unit, definition, &line, &line_length);
    if (line_length == 0) {
        return NULL;
    }

    // A fenced block for the definition, then whatever was written about it.
    // Markdown rather than plain text so the definition keeps the editor's
    // code styling.
    cJSON *parts = cJSON_CreateArray();
    if (parts == NULL) {
        return NULL;
    }

    // 07 の the type the checker settled on, under the line as written.
    // The two say different things -- one is what the writer put down, the
    // other what was inferred from it -- and a definition with no annotation
    // has only the second.
    char inferred[LHAT_HOVER_TYPE_BUFFER];
    size_t inferred_length = 0;
    if (resolved != NULL && resolved->type != NULL) {
        inferred_length =
            lhat_type_write(resolved->type, inferred, sizeof inferred);
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
    memcpy(fenced + used, line, line_length);
    used += line_length;
    if (inferred_length > 0) {
        memcpy(fenced + used, "\n: ", 3);
        used += 3;
        memcpy(fenced + used, inferred, inferred_length);
        used += inferred_length;
    }
    memcpy(fenced + used, "\n```", 5);
    cJSON_AddItemToArray(parts, cJSON_CreateString(fenced));
    free(fenced);

#ifdef LHAT_WITH_COMMENTS
    append_comments(parts, unit, definition);
#endif

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
    // about rather than guessing a word boundary. A module^ declaration was
    // found by position and has no such range, so it goes without one and
    // the editor falls back to its own guess.
    cJSON *range = resolved != NULL ? cJSON_CreateObject() : NULL;
    if (range != NULL) {
        cJSON_AddItemToObject(hover, "range", range);
        LspPosition from = lsp_position_at(unit->source.text,
                                           unit->source.length, resolved->use);
        LspPosition to = lsp_position_at(unit->source.text,
                                         unit->source.length, resolved->use_end);
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
