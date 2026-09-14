// L^ (lhat) -- LSP server: code switched off with '#[~ ... ]#' (01 の 6.5).

#include "disabled_code.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "position.h"

const char *lsp_disabled_code_statements_field(const LhatNode *node)
{
    switch (node->kind) {
        case LHAT_NODE_BLOCK:
            return "items";
        // 9 章: a clause lists the statements under its marker.
        case LHAT_NODE_LOOP_CLAUSE:
            return "body";
        default:
            return NULL;
    }
}

#if LHAT_WITH_COMMENTS

bool lsp_disabled_code_parse(LspDisabledCode *out, const char *text,
                             const LhatComment *comment)
{
    size_t length = comment->end;
    char *blanked = (char *)malloc(length);
    if (blanked == NULL) {
        return false;
    }
    // The body is what stands between '#[~' and ']#'.
    uint32_t body = comment->offset + 3;
    uint32_t body_end = comment->end - 2;
    for (size_t i = 0; i < length; i++) {
        bool kept = (i >= body && i < body_end) || text[i] == '\n';
        blanked[i] = kept ? text[i] : ' ';
    }
    bool made = lhat_source_init_from_string(&out->source, "<disabled>",
                                             blanked, length);
    free(blanked);
    if (!made) {
        return false;
    }
    lhat_lexer_init(&out->lexer, &out->source);
    lhat_parse(&out->lexer, &out->parsed);
    return true;
}

void lsp_disabled_code_dispose(LspDisabledCode *code)
{
    lhat_parse_result_dispose(&code->parsed);
    lhat_lexer_dispose(&code->lexer);
    lhat_source_dispose(&code->source);
}

// The unit's text is normalised (lhat/source.h), so '\n' is the only break.
static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static uint32_t line_start(const char *text, uint32_t at)
{
    while (at > 0 && text[at - 1] != '\n') {
        at--;
    }
    return at;
}

// Where the line holding `at` ends: its '\n', or the end of the text.
static uint32_t line_end(const char *text, size_t length, uint32_t at)
{
    while (at < length && text[at] != '\n') {
        at++;
    }
    return at;
}

static bool blank_between(const char *text, uint32_t from, uint32_t to)
{
    for (uint32_t at = from; at < to; at++) {
        if (!is_blank(text[at])) {
            return false;
        }
    }
    return true;
}

typedef struct {
    uint32_t from;
    uint32_t to;
} Span;

static void trim(const char *text, Span *s)
{
    while (s->from < s->to && (is_blank(text[s->from]) || text[s->from] == '\n')) {
        s->from++;
    }
    while (s->to > s->from &&
           (is_blank(text[s->to - 1]) || text[s->to - 1] == '\n')) {
        s->to--;
    }
}

static bool add_edit(cJSON *edits, const LhatUnit *unit, uint32_t from,
                     uint32_t to, const char *new_text)
{
    cJSON *edit = cJSON_CreateObject();
    if (edit == NULL) {
        return false;
    }
    cJSON_AddItemToArray(edits, edit);
    cJSON *range = lsp_unit_range_json(unit, from, to);
    if (range == NULL) {
        return false;
    }
    cJSON_AddItemToObject(edit, "range", range);
    return cJSON_AddStringToObject(edit, "newText", new_text) != NULL;
}

static cJSON *refusal(const char *why)
{
    cJSON *out = cJSON_CreateObject();
    if (out != NULL && cJSON_AddStringToObject(out, "refusal", why) == NULL) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

// The innermost disabled code holding the whole selection, looked for among
// `comments` -- lexed from `text` -- and then inside what it holds, since
// block comments nest (6.2).
static bool enclosing_code(const char *text, size_t length,
                           const LhatComment *comments, size_t count,
                           Span s, Span *found)
{
    for (size_t i = 0; i < count && comments[i].offset <= s.from; i++) {
        const LhatComment *c = &comments[i];
        if (s.to > c->end || !lhat_comment_is_disabled_code(c, text, length)) {
            continue;
        }
        found->from = c->offset;
        found->to = c->end;
        LspDisabledCode inner;
        if (lsp_disabled_code_parse(&inner, text, c)) {
            enclosing_code(inner.source.text, inner.source.length,
                           inner.lexer.comments, inner.lexer.comment_count, s,
                           found);
            lsp_disabled_code_dispose(&inner);
        }
        return true;
    }
    return false;
}

// Takes one marker away: its whole line when nothing else is written there,
// otherwise the marker and the space that parts it from the code.
static bool remove_marker(cJSON *edits, const LhatUnit *unit, uint32_t at,
                          uint32_t width, bool opening)
{
    const char *text = unit->source.text;
    size_t length = unit->source.length;
    uint32_t from = at;
    uint32_t to = at + width;
    uint32_t first = line_start(text, from);
    uint32_t last = line_end(text, length, to);
    if (blank_between(text, first, from) && blank_between(text, to, last)) {
        from = first;
        to = last < length ? last + 1 : last;
    } else if (opening && to < length && text[to] == ' ') {
        to++;
    } else if (!opening && from > 0 && text[from - 1] == ' ') {
        from--;
    }
    return add_edit(edits, unit, from, to, "");
}

// 02 の 18.4: an annotation is written above its declaration, whose span
// begins below it. Switching the declaration off without it would leave the
// annotation over whatever statement comes next.
static uint32_t statement_start(const LhatNode *statement)
{
    uint32_t start = lhat_node_span_start(statement);
    const LhatNode *annotations = statement->kind == LHAT_NODE_DEFINE
                                      ? statement->v.binding.annotations
                                      : NULL;
    return annotations != NULL && annotations->offset < start
               ? annotations->offset
               : start;
}

typedef struct {
    Span selection;  // trimmed of the space around it
    Span lines;      // the same, widened to whole lines
    const char *text;
    size_t length;
} Wrap;

typedef struct {
    Span selection;
    const LhatNode *found;
} HolderSearch;

// The outermost statement list below a node that holds the whole selection.
static void find_holder(void *context, const char *field, bool in_list,
                        const LhatNode *child)
{
    (void)field;
    (void)in_list;
    HolderSearch *search = (HolderSearch *)context;
    if (search->found != NULL ||
        lhat_node_span_start(child) > search->selection.from ||
        child->end < search->selection.to) {
        return;
    }
    if (lsp_disabled_code_statements_field(child) != NULL) {
        search->found = child;
        return;
    }
    lhat_node_visit_children(child, find_holder, search);
}

// The first and last statement the selection's lines touch, in the innermost
// statement list that holds the selection. False where it touches none.
static bool find_run(const LhatNode *holder, const Wrap *w,
                     const LhatNode **first, const LhatNode **last)
{
    *first = NULL;
    *last = NULL;
    const LhatNode *statements = holder->kind == LHAT_NODE_BLOCK
                                     ? holder->v.list.items
                                     : holder->v.loop_clause.body;
    for (const LhatNode *n = statements; n != NULL; n = n->next) {
        if (statement_start(n) < w->lines.to && n->end > w->lines.from) {
            if (*first == NULL) {
                *first = n;
            }
            *last = n;
        }
    }

    if (*first == NULL) {
        // 9 章's clauses list statements of their own, after the block's.
        for (const LhatNode *clause =
                 holder->kind == LHAT_NODE_BLOCK ? holder->v.list.extra : NULL;
             clause != NULL; clause = clause->next) {
            if (lhat_node_span_start(clause) <= w->selection.from &&
                w->selection.to <= clause->end) {
                return find_run(clause, w, first, last);
            }
        }
        return false;
    }

    // One statement, with the selection on lines strictly inside it: what is
    // meant is in a body the statement holds rather than the statement. A
    // selection reaching the statement's own first or last line takes it
    // whole -- there is no switching off half of 'if^ c {'.
    if (*first == *last &&
        w->lines.from > line_start(w->text, statement_start(*first)) &&
        w->lines.to < line_end(w->text, w->length, (*first)->end - 1)) {
        HolderSearch search = {w->selection, NULL};
        lhat_node_visit_children(*first, find_holder, &search);
        if (search.found != NULL) {
            return find_run(search.found, w, first, last);
        }
    }
    return true;
}

// 6.2: inside a block comment the lexer counts '#[' and ']#' wherever they
// stand, in a string or a line comment too. Wrapping text in which they do
// not pair up would end the comment somewhere in the middle of it, or not
// where the marker is.
static bool pairs_up(const char *text, uint32_t from, uint32_t to)
{
    int depth = 0;
    for (uint32_t at = from; at + 1 < to; at++) {
        if (text[at] == '#' && text[at + 1] == '[') {
            depth++;
            at++;
        } else if (text[at] == ']' && text[at + 1] == '#') {
            if (--depth < 0) {
                return false;
            }
            at++;
        }
    }
    return depth == 0;
}

// Each marker on a line of its own, indented as the first line is, so the
// code between keeps every column it had.
static bool add_markers(cJSON *edits, const LhatUnit *unit, uint32_t start,
                        uint32_t stop)
{
    const char *text = unit->source.text;
    size_t indent = 0;
    while (start + indent < stop && is_blank(text[start + indent])) {
        indent++;
    }
    char *opening = (char *)malloc(indent + 5);
    char *closing = (char *)malloc(indent + 4);
    bool added = opening != NULL && closing != NULL;
    if (added) {
        memcpy(opening, text + start, indent);
        memcpy(opening + indent, "#[~\n", 5);
        closing[0] = '\n';
        memcpy(closing + 1, text + start, indent);
        memcpy(closing + 1 + indent, "]#", 3);
        added = add_edit(edits, unit, start, start, opening) &&
                add_edit(edits, unit, stop, stop, closing);
    }
    free(opening);
    free(closing);
    return added;
}

cJSON *lsp_disabled_code_toggle(const LhatUnit *unit, uint32_t from,
                                uint32_t to)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return NULL;
    }
    const char *text = unit->source.text;
    size_t length = unit->source.length;
    if (from > to) {
        uint32_t swap = from;
        from = to;
        to = swap;
    }
    if (to > length) {
        to = (uint32_t)length;
    }
    if (from > to) {
        from = to;
    }

    // What was selected, without the space around it. Where that is nothing
    // -- a cursor -- it is the cursor's line.
    Span s = {from, to};
    trim(text, &s);
    if (s.from == s.to) {
        s.from = line_start(text, from);
        s.to = line_end(text, length, from);
        trim(text, &s);
    }

    cJSON *edits = cJSON_CreateArray();
    if (edits == NULL) {
        return NULL;
    }
    bool added = false;

    Span code;
    if (enclosing_code(text, length, unit->lexer.comments,
                       unit->lexer.comment_count, s, &code)) {
        added = remove_marker(edits, unit, code.from, 3, true) &&
                remove_marker(edits, unit, code.to - 2, 2, false);
    } else {
        Wrap w = {s, {0, 0}, text, length};
        w.lines.from = line_start(text, s.from);
        w.lines.to = line_end(text, length, s.to > 0 ? s.to - 1 : 0);
        const LhatNode *first = NULL;
        const LhatNode *last = NULL;
        if (s.from == s.to || !find_run(unit->parsed.root, &w, &first, &last)) {
            cJSON_Delete(edits);
            return refusal("There is no statement here to disable.");
        }

        // A comment line selected along with the statements goes in too.
        uint32_t start = line_start(text, statement_start(first));
        if (w.lines.from < start) {
            start = w.lines.from;
        }
        uint32_t stop = line_end(text, length, last->end - 1);
        if (w.lines.to > stop) {
            stop = w.lines.to;
        }
        if (!pairs_up(text, start, stop)) {
            cJSON_Delete(edits);
            return refusal(
                "A '#[' or ']#' here does not pair up -- in a string or a "
                "line comment, most likely -- and a block comment counts "
                "those too, so disabling this would end it in the wrong place.");
        }
        added = add_markers(edits, unit, start, stop);
    }

    cJSON *out = added ? cJSON_CreateObject() : NULL;
    if (out == NULL) {
        cJSON_Delete(edits);
        return NULL;
    }
    cJSON_AddItemToObject(out, "edits", edits);
    return out;
}

#else  // LHAT_WITH_COMMENTS

// Without comments kept there is no '#[~' to find: the lexer keeps no table.
cJSON *lsp_disabled_code_toggle(const LhatUnit *unit, uint32_t from,
                                uint32_t to)
{
    (void)unit;
    (void)from;
    (void)to;
    return NULL;
}

#endif  // LHAT_WITH_COMMENTS
