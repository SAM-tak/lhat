// L^ (lhat) -- the source formatter ([lsp.format]).
//
// The input is cut into units -- a token, a whole interpolated string, a
// comment -- each written back byte for byte. What the formatter decides is
// only what goes between two units: how many line breaks, how many spaces,
// and how deep a line starts. Line breaks are the writer's, except where a
// rule moves a brace or a line is too long; blank lines shrink to one.
//
// Depth comes from regions: bracket pairs, and the span of every node of the
// syntax tree. A line sits one deeper than the line the innermost region
// around it opened on, so constructs that open on one line count once. Three
// kinds of line start somewhere else: a closer goes back to its opener's
// line, a block's '{' on a line of its own stays at its head's depth, and a
// clause label ([lsp.format.labels]) goes back to the construct it belongs to.

#include "format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "lhat/port.h"
#include "lhat/source.h"
#include "ast.h"
#include "parser.h"

void lhat_format_defaults(LhatFormatOptions *options)
{
    options->indent_with_tabs = false;
    options->indent_size = 4;
    options->tab_width = 4;
    options->max_line_length = 100;
    options->end_of_line = LHAT_EOL_KEEP;
    options->final_newline = true;
    options->brace_next_line = false;
    options->space_after_hat = false;
}

// ---------------------------------------------------------------------------
// Growable arrays
// ---------------------------------------------------------------------------

static bool grow(void **items, size_t *capacity, size_t count, size_t size)
{
    if (count < *capacity) {
        return true;
    }
    size_t wanted = *capacity < 16 ? 16 : *capacity * 2;
    void *grown = lhat_realloc(*items, wanted * size);
    if (grown == NULL) {
        return false;
    }
    *items = grown;
    *capacity = wanted;
    return true;
}

typedef struct {
    char *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} Buffer;

static void put(Buffer *b, const char *bytes, size_t length)
{
    while (!b->failed && b->length + length + 1 > b->capacity) {
        b->failed = !grow((void **)&b->bytes, &b->capacity, b->capacity + 1, 1);
    }
    if (!b->failed) {
        memcpy(b->bytes + b->length, bytes, length);
        b->length += length;
        b->bytes[b->length] = '\0';
    }
}

static void put_repeated(Buffer *b, char c, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        put(b, &c, 1);
    }
}

// ---------------------------------------------------------------------------
// Reading: tokens from one lexer, the tree from another
// ---------------------------------------------------------------------------

typedef struct {
    LhatSource source;
    LhatLexer lexer;        // the token stream
    LhatLexer tree_lexer;   // the one the parser read; outlives the tree
    LhatParseResult parsed;
    LhatToken *tokens;      // up to and excluding EOF
    size_t token_count;
    size_t token_capacity;
    bool ready;
} Reading;

static void reading_dispose(Reading *r)
{
    if (!r->ready) {
        return;
    }
    lhat_free(r->tokens);
    lhat_parse_result_dispose(&r->parsed);
    lhat_lexer_dispose(&r->tree_lexer);
    lhat_lexer_dispose(&r->lexer);
    lhat_source_dispose(&r->source);
}

static LhatFormatStatus reading_init(Reading *r, const char *text, size_t length,
                                     bool lton)
{
    memset(r, 0, sizeof *r);
    if (!lhat_source_init_from_string(&r->source, "<format>", text, length)) {
        return LHAT_FORMAT_OUT_OF_MEMORY;
    }
    lhat_lexer_init(&r->lexer, &r->source);
    lhat_lexer_init(&r->tree_lexer, &r->source);
    r->ready = true;
    for (;;) {
        LhatToken token = lhat_lexer_next(&r->lexer);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
        if (!grow((void **)&r->tokens, &r->token_capacity, r->token_count,
                  sizeof *r->tokens)) {
            return LHAT_FORMAT_OUT_OF_MEMORY;
        }
        r->tokens[r->token_count++] = token;
    }
    if (lton) {
        lhat_parse_lton(&r->tree_lexer, &r->parsed);
    } else {
        lhat_parse(&r->tree_lexer, &r->parsed);
    }
    if (r->lexer.diagnostic_count != 0 || r->parsed.diagnostic_count != 0 ||
        r->parsed.incomplete || r->parsed.root == NULL) {
        return LHAT_FORMAT_SYNTAX_ERROR;
    }
    return LHAT_FORMAT_OK;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

typedef enum { UNIT_TOKEN, UNIT_INTERP, UNIT_LINE_COMMENT, UNIT_BLOCK_COMMENT } UnitKind;

typedef struct {
    UnitKind kind;
    LhatToken token;       // UNIT_TOKEN only
    uint32_t start, end;
    uint32_t breaks;       // line breaks before it: 0, 1 or 2 (a blank line)
    uint32_t written;      // spaces the input had before it on the same line
    uint32_t space;        // spaces to write before it when it is not first
    int32_t match;         // the other bracket of a pair, or -1
    int32_t brace;         // the innermost '{' it stands inside, or -1
    int32_t label_owner;   // [lsp.format.labels]: where its construct starts, or -1
    bool block_open;       // the '{' a block's body opens with
    // Worked out by layout().
    size_t line_start;     // the unit its line begins with
    uint32_t depth;        // line starts only
} Unit;

typedef struct {
    size_t open, close;
    bool indents;  // false: a row of statements, which is not nested in anything
} Region;

typedef struct {
    const LhatFormatOptions *options;
    const char *text;
    Unit *units;
    size_t unit_count, unit_capacity;
    Region *regions;
    size_t region_count, region_capacity;
    size_t *stack;     // the regions open at a unit, innermost last
    uint32_t *bases;   // beside each: the depth its contents are one deeper than
    bool failed;
} Format;

static bool unit_is_op(const Unit *u, LhatOpKind op)
{
    return u->kind == UNIT_TOKEN && u->token.kind == LHAT_TOKEN_OP &&
           u->token.v.op == op;
}

static bool unit_is_comment(const Unit *u)
{
    return u->kind == UNIT_LINE_COMMENT || u->kind == UNIT_BLOCK_COMMENT;
}

static bool unit_opens(const Unit *u)
{
    return unit_is_op(u, LHAT_OP_LPAREN) || unit_is_op(u, LHAT_OP_LBRACKET) ||
           unit_is_op(u, LHAT_OP_LBRACE) || unit_is_op(u, LHAT_OP_NIL_CALL) ||
           unit_is_op(u, LHAT_OP_NIL_INDEX);
}

static bool unit_closes(const Unit *u)
{
    return unit_is_op(u, LHAT_OP_RPAREN) || unit_is_op(u, LHAT_OP_RBRACKET) ||
           unit_is_op(u, LHAT_OP_RBRACE) || unit_is_op(u, LHAT_OP_SEMICOLON);
}

static Unit *add_unit(Format *f, UnitKind kind, uint32_t start, uint32_t end,
                      uint32_t *breaks, uint32_t *written)
{
    if (!grow((void **)&f->units, &f->unit_capacity, f->unit_count,
              sizeof *f->units)) {
        f->failed = true;
        return NULL;
    }
    Unit *u = &f->units[f->unit_count++];
    memset(u, 0, sizeof *u);
    u->kind = kind;
    u->start = start;
    u->end = end;
    u->breaks = *breaks > 2 ? 2 : *breaks;
    u->written = *written;
    u->match = -1;
    u->label_owner = -1;
    *breaks = 0;
    *written = 0;
    return u;
}

// What lies between two tokens is blanks and comments ([lex.comment]); the
// comments become units, and the blanks are counted for the unit after them.
static void scan_gap(Format *f, uint32_t from, uint32_t to, uint32_t *breaks,
                     uint32_t *written)
{
    const char *t = f->text;
    uint32_t at = from;
    while (at < to && !f->failed) {
        if (t[at] == '\n') {
            (*breaks)++;
            *written = 0;
            at++;
        } else if (t[at] != '#') {
            (*written)++;
            at++;
        } else if (at + 1 < to && t[at + 1] == '[') {
            uint32_t start = at;
            int depth = 0;
            do {
                if (t[at] == '#' && t[at + 1] == '[') {
                    depth++;
                    at += 2;
                } else if (t[at] == ']' && t[at + 1] == '#') {
                    depth--;
                    at += 2;
                } else {
                    at++;
                }
            } while (depth > 0 && at < to);
            add_unit(f, UNIT_BLOCK_COMMENT, start, at, breaks, written);
        } else {
            uint32_t start = at;
            while (at < to && t[at] != '\n') {
                at++;
            }
            add_unit(f, UNIT_LINE_COMMENT, start, at, breaks, written);
        }
    }
}

static void cut_units(Format *f, const Reading *r)
{
    uint32_t breaks = 0, written = 0, at = 0;
    for (size_t i = 0; i < r->token_count && !f->failed; i++) {
        const LhatToken *token = &r->tokens[i];
        // [syntax.annot.spelling]: an annotation's span leaves out its '@'.
        uint32_t start = token->offset - (token->kind == LHAT_TOKEN_ANNOTATION);
        scan_gap(f, at, start, &breaks, &written);
        if (token->kind == LHAT_TOKEN_INTERP_BEGIN) {
            // The whole string is one unit: its holes are written as they are.
            size_t depth = 0;
            size_t j = i;
            for (; j < r->token_count; j++) {
                if (r->tokens[j].kind == LHAT_TOKEN_INTERP_BEGIN) depth++;
                if (r->tokens[j].kind == LHAT_TOKEN_INTERP_END && --depth == 0) break;
            }
            const LhatToken *last = &r->tokens[j < r->token_count ? j : j - 1];
            at = last->offset + last->length;
            add_unit(f, UNIT_INTERP, token->offset, at, &breaks, &written);
            i = j;
            continue;
        }
        at = token->offset + token->length;
        Unit *u = add_unit(f, UNIT_TOKEN, start, at, &breaks, &written);
        if (u != NULL) {
            u->token = *token;
        }
    }
    scan_gap(f, at, (uint32_t)r->source.length, &breaks, &written);
    if (f->unit_count != 0) {
        f->units[0].breaks = 0;
    }
}

static void match_brackets(Format *f)
{
    size_t *stack = f->stack;
    size_t depth = 0;
    int32_t brace = -1;
    for (size_t i = 0; i < f->unit_count; i++) {
        Unit *u = &f->units[i];
        u->brace = brace;
        if (unit_is_op(u, LHAT_OP_LBRACE)) {
            brace = (int32_t)i;
        } else if (unit_is_op(u, LHAT_OP_RBRACE) && brace >= 0) {
            brace = f->units[brace].brace;
            u->brace = brace;
        }
        if (unit_opens(u)) {
            stack[depth++] = i;
        } else if (unit_closes(u) && !unit_is_op(u, LHAT_OP_SEMICOLON) && depth > 0) {
            size_t open = stack[--depth];
            f->units[open].match = (int32_t)i;
            u->match = (int32_t)open;
        }
    }
}

// The unit an offset falls in.
static size_t unit_at(const Format *f, uint32_t offset)
{
    size_t low = 0, high = f->unit_count;
    while (high - low > 1) {
        size_t middle = (low + high) / 2;
        if (f->units[middle].start <= offset) low = middle;
        else high = middle;
    }
    return low;
}

static void add_region(Format *f, size_t open, size_t close, bool indents)
{
    if (open >= close) {
        return;
    }
    if (!grow((void **)&f->regions, &f->region_capacity, f->region_count,
              sizeof *f->regions)) {
        f->failed = true;
        return;
    }
    f->regions[f->region_count++] = (Region){open, close, indents};
}

// [lsp.format.labels]: the lists whose members are written as labels of the
// construct holding them -- an if^'s el^ and elif^, a match's when^, a
// block's catch^ arms and loop clauses.
static bool is_clause_list(const LhatNode *parent, const char *field)
{
    if (strcmp(field, "arms") == 0 || strcmp(field, "extra") == 0) {
        return parent->kind == LHAT_NODE_BLOCK || parent->kind == LHAT_NODE_IF_STMT;
    }
    return strcmp(field, "items") == 0 && (parent->kind == LHAT_NODE_IF_STMT ||
                                           parent->kind == LHAT_NODE_IF_EXPR);
}

typedef struct {
    Format *f;
    const LhatNode *parent;
    size_t open;       // where the parent begins
    bool row_at_open;  // a child that begins there is a row
} Walk;

static void walk_node(void *context, const char *field, bool in_list,
                      const LhatNode *node)
{
    (void)in_list;
    Walk *walk = (Walk *)context;
    Format *f = walk->f;
    if (node == NULL || f->failed || node->end == 0) {
        return;
    }
    size_t open = unit_at(f, lhat_node_span_start(node));
    size_t close = unit_at(f, node->end - 1);
    Unit *first = &f->units[open];

    // A body's braces end the construct; an if^ with labels has no node
    // for them of their own ([syntax.if.stmt]).
    const Unit *last = &f->units[close];
    if ((node->kind == LHAT_NODE_BLOCK || node->kind == LHAT_NODE_IF_STMT) &&
        unit_is_op(last, LHAT_OP_RBRACE) && last->match >= (int32_t)open) {
        f->units[last->match].block_open = true;
    }
    // Only inside the braces it opened: in the form a ':' opens, the
    // clauses are what the construct holds, and indent like any contents.
    if (walk->parent != NULL && is_clause_list(walk->parent, field)) {
        size_t owner = unit_at(f, lhat_node_span_start(walk->parent));
        if (owner != open && first->brace >= (int32_t)owner) {
            first->label_owner = (int32_t)owner;
        }
    }

    Walk inner = {f, node, open, false};
    lhat_node_visit_children(node, walk_node, &inner);

    // A row of statements is not nested in anything: a body without braces,
    // and the definitions a for^ scopes to the statement after them
    // ([syntax.for.forms]), which stand in a row -- nor is what begins with
    // one, such as the return^ a body's last expression is read as.
    bool row = node->kind == LHAT_NODE_FOR || inner.row_at_open ||
               (node->kind == LHAT_NODE_BLOCK && !unit_is_op(first, LHAT_OP_LBRACE));
    add_region(f, open, close, !row);
    walk->row_at_open = walk->row_at_open || (row && open == walk->open);
}

static int compare_regions(const void *a, const void *b)
{
    const Region *x = (const Region *)a, *y = (const Region *)b;
    if (x->open != y->open) return x->open < y->open ? -1 : 1;
    if (x->close != y->close) return x->close > y->close ? -1 : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Spacing
// ---------------------------------------------------------------------------

static bool ends_operand(const Unit *u)
{
    if (u->kind == UNIT_INTERP) {
        return true;
    }
    if (u->kind != UNIT_TOKEN) {
        return false;
    }
    switch (u->token.kind) {
        case LHAT_TOKEN_IDENT:
        case LHAT_TOKEN_HAT_IDENT:
        case LHAT_TOKEN_ANNOTATION:  // `@export_range(0, 10)`
        case LHAT_TOKEN_INT:
        case LHAT_TOKEN_FLOAT:
        case LHAT_TOKEN_STRING:
            return true;
        case LHAT_TOKEN_OP:
            return unit_is_op(u, LHAT_OP_RPAREN) || unit_is_op(u, LHAT_OP_RBRACKET) ||
                   unit_is_op(u, LHAT_OP_RBRACE) || unit_is_op(u, LHAT_OP_PRESENT);
        default:
            return false;
    }
}

// [lsp.format.spacing]: the spaces between two units on one line. Where the
// tokens alone cannot tell which reading is meant -- '<' as a comparison or
// a type's argument list, ':' as a label or an annotation -- the writer's
// choice of none or some is kept.
static uint32_t spacing(const Format *f, size_t i)
{
    const Unit *a = &f->units[i - 1];
    const Unit *b = &f->units[i];
    uint32_t kept = b->written != 0 || b->breaks != 0;  // a line break was some

    if (b->kind == UNIT_LINE_COMMENT) {
        return b->written != 0 ? b->written : 1;
    }
    if (unit_is_comment(a) || unit_is_comment(b)) {
        return kept;
    }
    // [lsp.format.hat]: the hat is drawn faint, and a word after it reads as
    // the next word without a space.
    if (a->kind == UNIT_TOKEN && a->token.kind == LHAT_TOKEN_HAT_IDENT &&
        !f->options->space_after_hat) {
        return 0;
    }
    if (a->kind == UNIT_TOKEN && a->token.kind == LHAT_TOKEN_SCOPE) {
        return 0;
    }
    if (unit_is_op(b, LHAT_OP_COMMA) || unit_is_op(b, LHAT_OP_SEMICOLON) ||
        unit_is_op(b, LHAT_OP_RPAREN) || unit_is_op(b, LHAT_OP_RBRACKET) ||
        unit_is_op(b, LHAT_OP_DOT) || unit_is_op(b, LHAT_OP_NIL_DOT) ||
        unit_is_op(b, LHAT_OP_NIL_CALL) || unit_is_op(b, LHAT_OP_NIL_INDEX) ||
        unit_is_op(b, LHAT_OP_ELLIPSIS) || unit_is_op(b, LHAT_OP_PRESENT)) {
        return 0;
    }
    if (unit_is_op(a, LHAT_OP_LPAREN) || unit_is_op(a, LHAT_OP_LBRACKET) ||
        unit_is_op(a, LHAT_OP_DOT) || unit_is_op(a, LHAT_OP_NIL_DOT) ||
        unit_is_op(a, LHAT_OP_NIL_CALL) || unit_is_op(a, LHAT_OP_NIL_INDEX) ||
        unit_is_op(a, LHAT_OP_NOT)) {
        return 0;
    }
    // Inside braces on one line the writer chooses `{ a }` or `{a}`.
    if (unit_is_op(a, LHAT_OP_LBRACE)) {
        return unit_is_op(b, LHAT_OP_RBRACE) ? 0 : kept;
    }
    if (unit_is_op(b, LHAT_OP_RBRACE)) {
        return kept;
    }
    bool after_hat = a->kind == UNIT_TOKEN && a->token.kind == LHAT_TOKEN_HAT_IDENT;
    if (unit_is_op(b, LHAT_OP_LPAREN) || unit_is_op(b, LHAT_OP_LBRACKET)) {
        return after_hat ? kept : !ends_operand(a);
    }
    if (unit_is_op(b, LHAT_OP_LBRACE) &&
        ((a->kind == UNIT_TOKEN && a->token.kind == LHAT_TOKEN_IDENT) ||
         unit_is_op(a, LHAT_OP_RPAREN) || unit_is_op(a, LHAT_OP_RBRACKET))) {
        return kept;  // a constructor's braces may sit against its name
    }
    if (unit_is_op(a, LHAT_OP_COLON) || unit_is_op(b, LHAT_OP_COLON) ||
        unit_is_op(a, LHAT_OP_CONCAT) || unit_is_op(b, LHAT_OP_CONCAT) ||
        unit_is_op(a, LHAT_OP_ELLIPSIS) ||
        unit_is_op(a, LHAT_OP_LT) || unit_is_op(a, LHAT_OP_GT) ||
        unit_is_op(b, LHAT_OP_LT) || unit_is_op(b, LHAT_OP_GT) ||
        unit_is_op(a, LHAT_OP_UNION) || unit_is_op(b, LHAT_OP_UNION) ||
        unit_is_op(a, LHAT_OP_INTERSECT) || unit_is_op(b, LHAT_OP_INTERSECT)) {
        return kept;
    }
    if (unit_is_op(a, LHAT_OP_SUB)) {
        // A minus with no operand before it is a sign. After a hat word it
        // could be either -- `return^ -1`, `self^ - 1` -- so the writer says.
        size_t before = i - 1;
        while (before > 0 && unit_is_comment(&f->units[before - 1])) before--;
        if (before == 0) return 0;
        const Unit *operand = &f->units[before - 1];
        if (operand->kind == UNIT_TOKEN && operand->token.kind == LHAT_TOKEN_HAT_IDENT) {
            return kept;
        }
        return ends_operand(operand) ? 1 : 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void layout(Format *f)
{
    size_t next = 0;     // the next region to push
    size_t depth = 0;    // regions on the stack
    size_t line = 0;
    bool begins = true;  // whether this line begins a construct
    for (size_t i = 0; i < f->unit_count; i++) {
        Unit *u = &f->units[i];
        while (depth > 0 && f->regions[f->stack[depth - 1]].close < i) {
            depth--;
        }
        if (i == 0 || u->breaks != 0) {
            line = i;
            size_t inner = depth;  // the innermost region that indents
            while (inner > 0 && !f->regions[f->stack[inner - 1]].indents) inner--;
            uint32_t outer = inner > 0 ? f->bases[inner - 1] : 0;
            if (unit_closes(u) && depth > 0 &&
                f->regions[f->stack[depth - 1]].close == i) {
                u->depth = f->bases[depth - 1];
            } else if (u->label_owner >= 0) {
                const Unit *owner = &f->units[u->label_owner];
                u->depth = f->units[owner->line_start].depth;
            } else if (u->block_open || inner == 0) {
                u->depth = outer;
            } else {
                u->depth = outer + 1;
            }
            begins = next < f->region_count && f->regions[next].open == i;
        }
        u->line_start = line;
        // A region on a line that begins a construct counts from that line.
        // On one that carries on a construct begun above -- the rest of a
        // long type, say -- it counts from where that began, so a body
        // opened at the end of a long head is one deeper than the head's
        // first line, not than its last.
        while (next < f->region_count && f->regions[next].open == i) {
            f->bases[depth] = begins || depth == 0 ? f->units[line].depth
                                                   : f->bases[depth - 1];
            f->stack[depth++] = next++;
        }
    }
    // Comment lines written above a label are about it, and stand with it.
    size_t label = SIZE_MAX;
    for (size_t i = f->unit_count; i > 0; i--) {
        Unit *u = &f->units[i - 1];
        if (u->line_start != i - 1) {
            continue;
        }
        if (!unit_is_comment(u)) {
            label = u->label_owner >= 0 ? i - 1 : SIZE_MAX;
        } else if (label != SIZE_MAX) {
            u->depth = f->units[label].depth;
        }
    }
}

// Columns a unit takes up to its first line break, and whether it has one.
static size_t unit_width(const Format *f, const Unit *u, bool *broken)
{
    size_t width = 0;
    *broken = false;
    for (uint32_t at = u->start; at < u->end; at++) {
        unsigned char c = (unsigned char)f->text[at];
        if (c == '\n') {
            *broken = true;
            break;
        }
        width += (c & 0xC0) != 0x80;
    }
    return width;
}

static size_t indent_width(const Format *f, uint32_t depth)
{
    const LhatFormatOptions *o = f->options;
    return (size_t)depth * (o->indent_with_tabs ? o->tab_width : o->indent_size);
}

// [lsp.format.length]: a line that is too long is broken inside its widest
// bracket pair -- after the opener, after each comma at the pair's own level,
// and before the closer. Nothing is broken before a '(' or '[', which would
// stop it being a call or an index ([lex.ambig.call-line]). Whether anything
// was broken.
static bool break_long_line(Format *f)
{
    size_t limit = f->options->max_line_length;
    for (size_t start = 0; start < f->unit_count;) {
        size_t end = start + 1;
        while (end < f->unit_count && f->units[end].breaks == 0) end++;
        size_t width = indent_width(f, f->units[start].depth);
        for (size_t i = start; i < end; i++) {
            bool broken;
            width += (i > start ? f->units[i].space : 0) + unit_width(f, &f->units[i], &broken);
            if (broken) break;
        }
        if (width > limit) {
            size_t best = SIZE_MAX, best_span = 0;
            for (size_t i = start; i < end; i++) {
                const Unit *u = &f->units[i];
                if (unit_opens(u) && u->match > (int32_t)(i + 1) &&
                    (size_t)u->match < end && (size_t)u->match - i > best_span) {
                    best = i;
                    best_span = (size_t)u->match - i;
                }
            }
            if (best != SIZE_MAX) {
                size_t close = (size_t)f->units[best].match;
                f->units[best + 1].breaks = 1;
                for (size_t i = best + 1; i < close; i++) {
                    if (f->units[i].match > (int32_t)i && unit_opens(&f->units[i])) {
                        i = (size_t)f->units[i].match;
                    } else if (unit_is_op(&f->units[i], LHAT_OP_COMMA) &&
                               !unit_is_comment(&f->units[i + 1])) {
                        f->units[i + 1].breaks = 1;
                    }
                }
                f->units[close].breaks = 1;
                return true;
            }
        }
        start = end;
    }
    return false;
}

// [lsp.format.braces]: a block's '{' goes where the brace style puts it. A
// block on one line stays on it.
static void place_braces(Format *f)
{
    bool next_line = f->options->brace_next_line;
    for (size_t i = 1; i < f->unit_count; i++) {
        Unit *u = &f->units[i];
        const Unit *previous = &f->units[i - 1];
        if (u->block_open) {
            bool spread = false;
            for (int32_t k = (int32_t)i + 1; k <= u->match; k++) {
                spread = spread || f->units[k].breaks != 0;
            }
            if (next_line && spread) {
                u->breaks = u->breaks != 0 ? u->breaks : 1;
            } else if (!next_line && !unit_is_comment(previous)) {
                u->breaks = 0;
            }
        }
    }
}

static void render(const Format *f, Buffer *out)
{
    const LhatFormatOptions *o = f->options;
    for (size_t i = 0; i < f->unit_count; i++) {
        const Unit *u = &f->units[i];
        if (i > 0 && u->breaks != 0) {
            put_repeated(out, '\n', u->breaks);
        }
        if (u->line_start == i) {
            if (o->indent_with_tabs) put_repeated(out, '\t', u->depth);
            else put_repeated(out, ' ', (size_t)u->depth * o->indent_size);
        } else {
            put_repeated(out, ' ', u->space);
        }
        put(out, f->text + u->start, u->end - u->start);
    }
    if (o->final_newline && f->unit_count != 0) {
        put(out, "\n", 1);
    }
}

// ---------------------------------------------------------------------------
// The same tokens, and the same tree
// ---------------------------------------------------------------------------

typedef struct {
    LhatNodeKind *kinds;
    size_t count, capacity;
    bool failed;
} Kinds;

static void collect_kind(void *context, const char *field, bool in_list,
                         const LhatNode *node)
{
    (void)field;
    (void)in_list;
    Kinds *k = (Kinds *)context;
    if (node == NULL || k->failed) {
        return;
    }
    if (!grow((void **)&k->kinds, &k->capacity, k->count, sizeof *k->kinds)) {
        k->failed = true;
        return;
    }
    k->kinds[k->count++] = node->kind;
    lhat_node_visit_children(node, collect_kind, k);
}

static bool same_reading(const Reading *a, const Reading *b, bool *failed)
{
    if (a->token_count != b->token_count) {
        return false;
    }
    for (size_t i = 0; i < a->token_count; i++) {
        const LhatToken *x = &a->tokens[i], *y = &b->tokens[i];
        if (x->kind != y->kind || x->length != y->length ||
            memcmp(a->source.text + x->offset, b->source.text + y->offset,
                   x->length) != 0) {
            return false;
        }
    }
    // [lex.ambig.call-line]: the one place a line break changes a reading,
    // a '(' or '[' that stops being a call or an index, shows in the tree.
    Kinds ka = {0}, kb = {0};
    collect_kind(&ka, "", false, a->parsed.root);
    collect_kind(&kb, "", false, b->parsed.root);
    *failed = ka.failed || kb.failed;
    bool same = ka.count == kb.count &&
                memcmp(ka.kinds, kb.kinds, ka.count * sizeof *ka.kinds) == 0;
    lhat_free(ka.kinds);
    lhat_free(kb.kinds);
    return same;
}

// ---------------------------------------------------------------------------
// The whole of it
// ---------------------------------------------------------------------------

static LhatEndOfLine end_of_line_of(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\r') {
            return i + 1 < length && text[i + 1] == '\n' ? LHAT_EOL_CRLF : LHAT_EOL_CR;
        }
        if (text[i] == '\n') {
            return LHAT_EOL_LF;
        }
    }
    return LHAT_EOL_LF;
}

LhatFormatStatus lhat_format(const char *text, size_t length, bool lton,
                             const LhatFormatOptions *options, char **out,
                             size_t *out_length)
{
    *out = NULL;
    *out_length = 0;
    Reading input;
    LhatFormatStatus status = reading_init(&input, text, length, lton);
    if (status != LHAT_FORMAT_OK) {
        reading_dispose(&input);
        return status;
    }

    Format f = {options, input.source.text};
    cut_units(&f, &input);
    f.stack = (size_t *)lhat_alloc((f.unit_count + 1) * sizeof *f.stack);
    Buffer formatted = {0};
    if (!f.failed && f.stack != NULL) {
        match_brackets(&f);
        for (size_t i = 0; i < f.unit_count; i++) {
            if (f.units[i].match > (int32_t)i) {
                add_region(&f, i, (size_t)f.units[i].match, true);
            }
        }
        Walk walk = {&f, input.parsed.root, SIZE_MAX, false};
        lhat_node_visit_children(input.parsed.root, walk_node, &walk);
    }
    if (f.failed || f.stack == NULL) {
        status = LHAT_FORMAT_OUT_OF_MEMORY;
    } else {
        lhat_free(f.stack);
        f.stack = (size_t *)lhat_alloc((f.region_count + 1) * sizeof *f.stack);
        f.bases = (uint32_t *)lhat_alloc((f.region_count + 1) * sizeof *f.bases);
        status = f.stack == NULL || f.bases == NULL ? LHAT_FORMAT_OUT_OF_MEMORY
                                                    : LHAT_FORMAT_OK;
    }
    if (status == LHAT_FORMAT_OK) {
        qsort(f.regions, f.region_count, sizeof *f.regions, compare_regions);
        for (size_t i = 1; i < f.unit_count; i++) {
            f.units[i].space = spacing(&f, i);
        }
        place_braces(&f);
        layout(&f);
        while (options->max_line_length != 0 && break_long_line(&f)) {
            layout(&f);
        }
        render(&f, &formatted);
        if (formatted.failed) {
            status = LHAT_FORMAT_OUT_OF_MEMORY;
        }
    }
    lhat_free(f.units);
    lhat_free(f.regions);
    lhat_free(f.stack);
    lhat_free(f.bases);

    if (status == LHAT_FORMAT_OK) {
        Reading output;
        status = reading_init(&output, formatted.bytes != NULL ? formatted.bytes : "",
                              formatted.length, lton);
        bool failed = false;
        if (status == LHAT_FORMAT_SYNTAX_ERROR ||
            (status == LHAT_FORMAT_OK && !same_reading(&input, &output, &failed))) {
            status = LHAT_FORMAT_CHANGED_TOKENS;
        } else if (failed) {
            status = LHAT_FORMAT_OUT_OF_MEMORY;
        }
        reading_dispose(&output);
    }
    reading_dispose(&input);

    LhatEndOfLine eol = options->end_of_line == LHAT_EOL_KEEP
                            ? end_of_line_of(text, length)
                            : options->end_of_line;
    if (status == LHAT_FORMAT_OK) {
        Buffer written = {0};
        put(&written, "", 0);
        for (size_t i = 0; i < formatted.length; i++) {
            char c = formatted.bytes[i];
            if (c != '\n') put(&written, &c, 1);
            else if (eol == LHAT_EOL_CRLF) put(&written, "\r\n", 2);
            else if (eol == LHAT_EOL_CR) put(&written, "\r", 1);
            else put(&written, "\n", 1);
        }
        if (written.failed) {
            lhat_free(written.bytes);
            status = LHAT_FORMAT_OUT_OF_MEMORY;
        } else {
            *out = written.bytes;
            *out_length = written.length;
        }
    }
    lhat_free(formatted.bytes);
    return status;
}

// ---------------------------------------------------------------------------
// [lsp.format.settings]: .editorconfig
// ---------------------------------------------------------------------------

static bool glob_match(const char *p, const char *pe, const char *s, const char *se);

// '{a,b}' and '{1..3}', at p, which is the '{'. Answers through *matched;
// false when the braces do not close and the '{' is an ordinary character.
static bool glob_braces(const char *p, const char *pe, const char *s,
                        const char *se, bool *matched)
{
    const char *close = NULL;
    bool comma = false;
    int depth = 0;
    for (const char *q = p; q < pe; q++) {
        if (*q == '\\' && q + 1 < pe) { q++; continue; }
        if (*q == '{') depth++;
        if (*q == '}' && --depth == 0) { close = q; break; }
        if (*q == ',' && depth == 1) comma = true;
    }
    if (close == NULL) {
        return false;
    }
    const char *rest = close + 1;
    if (!comma) {
        long low, high;
        int used = 0;
        size_t inner = (size_t)(close - p - 1);
        char range[64];
        if (inner >= sizeof range) return false;
        memcpy(range, p + 1, inner);
        range[inner] = '\0';
        if (sscanf(range, "%ld..%ld%n", &low, &high, &used) != 2 ||
            (size_t)used != inner) {
            return false;
        }
        const char *end = s < se && *s == '-' ? s + 1 : s;
        const char *digits = end;
        long value = 0;
        while (end < se && *end >= '0' && *end <= '9') {
            value = value * 10 + (*end++ - '0');
        }
        value = digits > s ? -value : value;
        *matched = end > digits &&
                   value >= (low < high ? low : high) &&
                   value <= (low < high ? high : low) &&
                   glob_match(rest, pe, end, se);
        return true;
    }
    // Each alternative, followed by what comes after the braces.
    *matched = false;
    const char *from = p + 1;
    depth = 0;
    for (const char *q = p + 1; q <= close && !*matched; q++) {
        if (*q == '\\' && q + 1 < close) { q++; continue; }
        if (*q == '{') depth++;
        if (*q == '}' && q != close) depth--;
        if ((*q == ',' && depth == 0) || q == close) {
            size_t alternative = (size_t)(q - from), tail = (size_t)(pe - rest);
            char *joined = (char *)lhat_alloc(alternative + tail + 1);
            if (joined == NULL) return true;
            memcpy(joined, from, alternative);
            memcpy(joined + alternative, rest, tail);
            *matched = glob_match(joined, joined + alternative + tail, s, se);
            lhat_free(joined);
            from = q + 1;
        }
    }
    return true;
}

static bool glob_match(const char *p, const char *pe, const char *s, const char *se)
{
    while (p < pe) {
        char c = *p;
        if (c == '*') {
            bool deep = p + 1 < pe && p[1] == '*';
            p += deep ? 2 : 1;
            for (const char *t = s;; t++) {
                if (glob_match(p, pe, t, se)) return true;
                if (t == se || (!deep && *t == '/')) return false;
            }
        }
        if (c == '{') {
            bool matched;
            if (glob_braces(p, pe, s, se, &matched)) return matched;
        }
        if (c == '[') {
            const char *q = p + 1;
            bool negate = q < pe && *q == '!';
            q += negate;
            const char *close = q;
            while (close < pe && (*close != ']' || close == q)) close++;
            if (close < pe) {
                if (s == se || *s == '/') return false;
                bool in = false;
                for (const char *k = q; k < close; k++) {
                    if (k + 2 < close && k[1] == '-') {
                        in = in || (*s >= k[0] && *s <= k[2]);
                        k += 2;
                    } else {
                        in = in || *s == *k;
                    }
                }
                if (in == negate) return false;
                p = close + 1;
                s++;
                continue;
            }
        }
        if (c == '?') {
            if (s == se || *s == '/') return false;
            p++;
            s++;
            continue;
        }
        if (c == '\\' && p + 1 < pe) {
            c = *++p;
        }
        if (s == se || *s != c) return false;
        p++;
        s++;
    }
    return s == se;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = '\0';
    return s;
}

static void lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static void apply_property(LhatFormatOptions *o, const char *key, const char *value)
{
    LhatFormatOptions defaults;
    lhat_format_defaults(&defaults);
    bool unset = strcmp(value, "unset") == 0;
    unsigned long number = strtoul(value, NULL, 10);
    bool counted = number > 0 && number < 1000;
    if (strcmp(key, "indent_style") == 0) {
        o->indent_with_tabs = unset ? defaults.indent_with_tabs : strcmp(value, "tab") == 0;
    } else if (strcmp(key, "indent_size") == 0) {
        if (unset) o->indent_size = defaults.indent_size;
        else if (strcmp(value, "tab") == 0) o->indent_size = o->tab_width;
        else if (counted) o->indent_size = (uint32_t)number;
    } else if (strcmp(key, "tab_width") == 0) {
        if (unset) o->tab_width = defaults.tab_width;
        else if (counted) o->tab_width = (uint32_t)number;
    } else if (strcmp(key, "max_line_length") == 0) {
        if (unset) o->max_line_length = defaults.max_line_length;
        else if (strcmp(value, "off") == 0) o->max_line_length = 0;
        else if (counted) o->max_line_length = (uint32_t)number;
    } else if (strcmp(key, "end_of_line") == 0) {
        o->end_of_line = strcmp(value, "lf") == 0     ? LHAT_EOL_LF
                         : strcmp(value, "crlf") == 0 ? LHAT_EOL_CRLF
                         : strcmp(value, "cr") == 0   ? LHAT_EOL_CR
                                                      : defaults.end_of_line;
    } else if (strcmp(key, "insert_final_newline") == 0) {
        o->final_newline = unset ? defaults.final_newline : strcmp(value, "false") != 0;
    } else if (strcmp(key, "lhat_brace_style") == 0) {
        o->brace_next_line = strcmp(value, "next_line") == 0;
    } else if (strcmp(key, "lhat_space_after_hat") == 0) {
        o->space_after_hat = strcmp(value, "true") == 0;
    }
}

// One .editorconfig in `directory` (with its trailing '/'), applied to the
// file `relative` names under it. Whether it said root = true.
static bool apply_editorconfig(const char *directory, const char *relative,
                               LhatFormatOptions *o)
{
    size_t length = strlen(directory);
    char *path = (char *)lhat_alloc(length + sizeof ".editorconfig");
    if (path == NULL) return false;
    memcpy(path, directory, length);
    memcpy(path + length, ".editorconfig", sizeof ".editorconfig");
    FILE *file = fopen(path, "rb");
    lhat_free(path);
    if (file == NULL) return false;

    const char *base = strrchr(relative, '/');
    base = base != NULL ? base + 1 : relative;
    bool root = false, preamble = true, applies = false;
    char line[1024];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }
        if (*text == '[') {
            char *close = strrchr(text, ']');
            if (close == NULL) continue;
            *close = '\0';
            const char *glob = text + 1;
            preamble = false;
            // A glob without a '/' names a file in any directory below.
            const char *against = strchr(glob, '/') != NULL ? relative : base;
            if (*glob == '/') glob++;
            applies = glob_match(glob, glob + strlen(glob), against,
                                 against + strlen(against));
            continue;
        }
        char *equals = strchr(text, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        char *key = trim(text), *value = trim(equals + 1);
        lower(key);
        lower(value);
        if (preamble) {
            root = root || (strcmp(key, "root") == 0 && strcmp(value, "true") == 0);
        } else if (applies) {
            apply_property(o, key, value);
        }
    }
    fclose(file);
    return root;
}

void lhat_format_read_editorconfig(const char *path, LhatFormatOptions *options)
{
    size_t length = strlen(path);
    char *full = (char *)lhat_alloc(2 * (length + 1));
    size_t *slashes = (size_t *)lhat_alloc((length + 1) * sizeof *slashes);
    if (full == NULL || slashes == NULL) {
        lhat_free(full);
        lhat_free(slashes);
        return;
    }
    char *directory = full + length + 1;
    size_t count = 0;
    for (size_t i = 0; i <= length; i++) {
        full[i] = path[i] == '\\' ? '/' : path[i];
        if (full[i] == '/') slashes[count++] = i;
    }
    // Nearest first, to find the root; then laid on from the root down, so
    // the nearer file has the last word.
    size_t first = 0;
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t step = 0; step < count; step++) {
            size_t k = pass == 0 ? count - 1 - step : step;
            if (pass == 1 && k < first) continue;
            memcpy(directory, full, slashes[k] + 1);
            directory[slashes[k] + 1] = '\0';
            LhatFormatOptions scratch = *options;
            bool root = apply_editorconfig(directory, full + slashes[k] + 1,
                                           pass == 0 ? &scratch : options);
            if (pass == 0 && root) {
                first = k;
                break;
            }
        }
    }
    lhat_free(full);
    lhat_free(slashes);
}
