// L^ (lhat) -- parser.

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "lhatconfig.h"

typedef struct {
    LhatLexer *lexer;
    LhatToken current;
    LhatToken ahead;
    LhatParseResult *result;
    bool panicking;   // suppress the cascade after a reported error
    bool saw_yield;   // 15.2: set while parsing a body that contains yield^

    // 02 の 8.2: a bare expression is a statement at the top level of
    // interactive input and nowhere else. `depth` counts the statement lists
    // being parsed, so the unit's own is 1 and anything nested is more.
    bool interactive;
    size_t depth;

    // 15.12: the depth of the statement list a subroutine body owns, where a
    // bare expression is read and the last one becomes the answer. Zero
    // outside any body -- 8.2's rule holds everywhere else, nested blocks of
    // the body included.
    size_t bare_depth;
} Parser;

// Levels of 11.6, weakest first. A larger value binds tighter.
enum {
    PREC_NONE = 0,
    PREC_OR,
    PREC_AND,
    PREC_COMPARE,
    PREC_CONCAT,
    PREC_ADD,
    PREC_MUL
};

static LhatNode *parse_expression(Parser *p);
static LhatNode *parse_type(Parser *p);
static LhatNode *parse_statement(Parser *p);
static LhatNode *parse_block_body(Parser *p, const LhatToken *at);
static LhatNode *parse_clause_body(Parser *p, const LhatToken *at, bool in_loop,
                                   bool walks);
static LhatNode *access_node(Parser *p, LhatNodeKind kind, const LhatToken *at,
                             LhatNode *target, LhatNode *argument, bool nil_safe);
static LhatNode *simple_node(Parser *p);
static LhatNode *parse_error_fields(Parser *p);
static LhatNode *parse_module(Parser *p);
static LhatNode *parse_public(Parser *p);
static LhatNode *parse_for(Parser *p);
static LhatNode *parse_binding(Parser *p, LhatNodeKind kind,
                               const LhatToken *at, LhatNode *targets);
static bool is_binary_op(const LhatNode *node, LhatOpKind op);
static bool is_call_statement(const LhatNode *node);  // 8.2
static bool starts_expression(const LhatToken *token);
static bool is_statement_keyword(const Parser *p);

// ---------------------------------------------------------------------------
// Token access
// ---------------------------------------------------------------------------

static void advance(Parser *p)
{
    p->current = p->ahead;
    p->ahead = lhat_lexer_next(p->lexer);
}

static bool at_eof(const Parser *p)
{
    return p->current.kind == LHAT_TOKEN_EOF;
}

static bool is_op(const LhatToken *token, LhatOpKind op)
{
    return token->kind == LHAT_TOKEN_OP && token->v.op == op;
}

static bool check_op(const Parser *p, LhatOpKind op)
{
    return is_op(&p->current, op);
}

static bool match_op(Parser *p, LhatOpKind op)
{
    if (!check_op(p, op)) {
        return false;
    }
    advance(p);
    return true;
}

// Compares a hat identifier against a word, ignoring the trailing carets.
static bool token_is_hat(const Parser *p, const LhatToken *token, const char *word)
{
    if (token->kind != LHAT_TOKEN_HAT_IDENT) {
        return false;
    }
    size_t length = (size_t)token->length - token->v.hats;
    size_t wanted = strlen(word);
    if (length != wanted) {
        return false;
    }
    return memcmp(p->lexer->source->text + token->offset, word, wanted) == 0;
}

static bool check_hat(const Parser *p, const char *word)
{
    return token_is_hat(p, &p->current, word);
}

static bool match_hat(Parser *p, const char *word)
{
    if (!check_hat(p, word)) {
        return false;
    }
    advance(p);
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static void report(Parser *p, const LhatToken *at, LhatParseErrorCode code)
{
    // 02 の 3.1: running out of input is not the same as a syntax error.
    if (at->kind == LHAT_TOKEN_EOF) {
        p->result->incomplete = true;
    }

    if (p->panicking) {
        return;
    }
    p->panicking = true;

    LhatParseResult *r = p->result;
    if (r->diagnostic_count == r->diagnostic_capacity) {
        size_t grown = r->diagnostic_capacity ? r->diagnostic_capacity * 2 : 8;
        LhatParseDiagnostic *bigger =
            (LhatParseDiagnostic *)lhat_realloc(r->diagnostics, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;
        }
        r->diagnostics = bigger;
        r->diagnostic_capacity = grown;
    }

    LhatParseDiagnostic *d = &r->diagnostics[r->diagnostic_count++];
    d->code = code;
    d->offset = at->offset;
    d->line = at->line;
    d->column = at->column;
    d->has_expected = false;
    d->expected = LHAT_OP_LPAREN;  // read only when has_expected says so
    d->found = at->kind;
    d->found_op = at->kind == LHAT_TOKEN_OP ? at->v.op : LHAT_OP_LPAREN;
    d->length = at->length;
}

// The same, naming the token that was wanted. Every expect_op knows it, and
// saying which turns "a different token" into something a reader can act on.
static void report_expected(Parser *p, const LhatToken *at, LhatOpKind op)
{
    bool was_panicking = p->panicking;
    report(p, at, LHAT_PARSE_ERR_EXPECTED_TOKEN);
    if (was_panicking || p->result->diagnostic_count == 0) {
        return;  // report kept quiet, so there is nothing to say it on
    }
    LhatParseDiagnostic *d =
        &p->result->diagnostics[p->result->diagnostic_count - 1];
    d->has_expected = true;
    d->expected = op;
}

static LhatNode *make(Parser *p, LhatNodeKind kind, const LhatToken *at)
{
    return lhat_node_new(&p->result->arena, kind, at);
}

static LhatNode *error_node(Parser *p, LhatParseErrorCode code)
{
    report(p, &p->current, code);
    return make(p, LHAT_NODE_ERROR, &p->current);
}

static bool expect_op(Parser *p, LhatOpKind op)
{
    if (match_op(p, op)) {
        return true;
    }
    report_expected(p, &p->current, op);
    return false;
}

// Skips forward until something that can begin a statement, so one mistake
// does not turn the rest of the file into noise.
static void synchronize(Parser *p)
{
    p->panicking = false;
    while (!at_eof(p)) {
        if (check_op(p, LHAT_OP_RBRACE)) {
            return;
        }
        if (p->current.kind == LHAT_TOKEN_HAT_IDENT ||
            p->current.kind == LHAT_TOKEN_IDENT) {
            return;
        }
        advance(p);
    }
}

// ---------------------------------------------------------------------------
// Types (13 章)
// ---------------------------------------------------------------------------

// A parameter inside a type carries no name (13.4); only '...' may appear
// where a name would be (13.7).
static LhatNode *parse_type_params(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_ARROW) &&
           !check_op(p, LHAT_OP_SEMICOLON)) {
        LhatNode *param = make(p, LHAT_NODE_PARAM, &p->current);
        if (param == NULL) {
            return head;
        }

        if (match_op(p, LHAT_OP_ELLIPSIS)) {
            param->v.param.variadic = true;
            if (match_op(p, LHAT_OP_COLON)) {
                param->v.param.type = parse_type(p);
            }
        } else {
            param->v.param.type = parse_type(p);
        }

        lhat_node_append(&head, &tail, param);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

static LhatNode *parse_type_function(Parser *p, bool is_function)
{
    LhatToken start = p->current;
    advance(p);  // f^ or p^

    LhatNode *node = make(p, LHAT_NODE_TYPE_FUNC, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.func.is_function = is_function;
    node->v.func.params = parse_type_params(p);

    if (check_op(p, LHAT_OP_COLONCOLON)) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON);
        advance(p);
        node->v.func.return_type = parse_type(p);
    } else if (match_op(p, LHAT_OP_ARROW)) {
        // 13.2: '->' is present only when something is returned.
        node->v.func.return_type = parse_type(p);
    }

    expect_op(p, LHAT_OP_SEMICOLON);
    return node;
}

static LhatNode *parse_type_coroutine(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // c^

    LhatNode *node = make(p, LHAT_NODE_TYPE_CORO, &start);
    if (node == NULL) {
        return NULL;
    }

    expect_op(p, LHAT_OP_LBRACE);
    node->v.coroutine.receive = parse_type(p);
    expect_op(p, LHAT_OP_COMMA);
    node->v.coroutine.produce = parse_type(p);
    expect_op(p, LHAT_OP_COMMA);
    node->v.coroutine.result = parse_type(p);
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

// 'name : type, ...' up to the closing brace. Shared by t^{ ... } and by the
// fields a kind declares (04 の 2.2), which is the same shape.
static LhatNode *parse_member_decls(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *member = make(p, LHAT_NODE_MEMBER_DECL, &p->current);
        if (member == NULL) {
            break;
        }

        // 13.7, 14.10改: the sequence half may end in a variadic tail, the
        // same '...' a parameter list ends in -- unbounded, one type for
        // every position beyond the fixed ones.
        if (check_op(p, LHAT_OP_ELLIPSIS)) {
            advance(p);
            member->v.entry.variadic = true;
            if (match_op(p, LHAT_OP_COLON)) {
                member->v.entry.value = parse_type(p);
            }
            lhat_node_append(&head, &tail, member);
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        // 14.10改: a member is written 'name : type'. Anything else in the
        // list is a type on its own, and takes the next position -- 14 章
        // makes a table a sequence as well as a mapping, and the sequence
        // half is described by writing its types in order. One token of
        // lookahead separates them: only a name followed by ':' is a member.
        bool named = (p->current.kind == LHAT_TOKEN_IDENT ||
                      p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
                      p->current.kind == LHAT_TOKEN_HAT_IDENT) &&
                     is_op(&p->ahead, LHAT_OP_COLON);
        if (!named) {
            member->v.entry.key = NULL;
            member->v.entry.value = parse_type(p);
            lhat_node_append(&head, &tail, member);
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        if (p->current.kind == LHAT_TOKEN_IDENT ||
            p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
            p->current.kind == LHAT_TOKEN_HAT_IDENT) {
            LhatNodeKind kind = p->current.kind == LHAT_TOKEN_NAME_LITERAL
                                    ? LHAT_NODE_NAME
                                    : (p->current.kind == LHAT_TOKEN_HAT_IDENT
                                           ? LHAT_NODE_HAT_IDENT
                                           : LHAT_NODE_IDENT);
            LhatNode *name = make(p, kind, &p->current);
            if (name != NULL) {
                if (kind == LHAT_NODE_NAME) {
                    name->v.string.offset = p->current.v.string.offset;
                    name->v.string.length = p->current.v.string.length;
                } else {
                    name->v.name.offset = p->current.offset;
                    name->v.name.length = p->current.length;
                    name->v.name.hats = kind == LHAT_NODE_HAT_IDENT
                                            ? p->current.v.hats
                                            : 0;
                }
            }
            member->v.entry.key = name;
            advance(p);
        } else {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }

        // 13.6 の (1): ':' ties a name to a type.
        expect_op(p, LHAT_OP_COLON);
        member->v.entry.value = parse_type(p);
        lhat_node_append(&head, &tail, member);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

static LhatNode *parse_type_table(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // t^ or table^

    // 14.10: bare, with no members listed, t^ asks for nothing in particular
    // -- the top of tables. That is an ordinary type name, so it is written
    // as one and 13.7's judgement reads it without a case of its own.
    if (!check_op(p, LHAT_OP_LBRACE)) {
        LhatNode *node = make(p, LHAT_NODE_TYPE_NAME, &start);
        if (node != NULL) {
            node->v.name.offset = start.offset;
            node->v.name.length = start.length;
            node->v.name.hats = start.v.hats;
        }
        return node;
    }

    LhatNode *node = make(p, LHAT_NODE_TYPE_TABLE, &start);
    if (node == NULL) {
        return NULL;
    }

    expect_op(p, LHAT_OP_LBRACE);
    node->v.list.items = parse_member_decls(p);
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

static LhatNode *parse_type_primary(Parser *p)
{
    if (check_hat(p, "f")) {
        return parse_type_function(p, true);
    }
    if (check_hat(p, "p")) {
        return parse_type_function(p, false);
    }
    if (check_hat(p, "c")) {
        return parse_type_coroutine(p);
    }
    if (check_hat(p, "t") || check_hat(p, "table")) {
        return parse_type_table(p);
    }

    if (match_op(p, LHAT_OP_LPAREN)) {
        LhatNode *inner = parse_type(p);
        expect_op(p, LHAT_OP_RPAREN);
        return inner;
    }

    if (p->current.kind == LHAT_TOKEN_HAT_IDENT ||
        p->current.kind == LHAT_TOKEN_IDENT) {
        LhatNode *node = make(p, LHAT_NODE_TYPE_NAME, &p->current);
        if (node != NULL) {
            node->v.name.offset = p->current.offset;
            node->v.name.length = p->current.length;
            node->v.name.hats = p->current.kind == LHAT_TOKEN_HAT_IDENT
                                    ? p->current.v.hats
                                    : 0;
        }
        advance(p);

        // 04 の 14.4: an error kind is named through the declaration that
        // introduced it, so a type may be a qualified name.
        while (check_op(p, LHAT_OP_DOT)) {
            LhatToken at = p->current;
            advance(p);
            if (p->current.kind != LHAT_TOKEN_IDENT) {
                report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
                break;
            }
            node = access_node(p, LHAT_NODE_MEMBER, &at, node,
                               simple_node(p), false);
        }
        return node;
    }

    return error_node(p, LHAT_PARSE_ERR_EXPECTED_TYPE);
}

// 13.5 and 14.5: '&' binds tighter than '|', as it does in TypeScript.
static LhatNode *parse_type_intersection(Parser *p)
{
    LhatNode *left = parse_type_primary(p);
    while (check_op(p, LHAT_OP_INTERSECT)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_TYPE_INTERSECT, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.left = left;
        node->v.binary.right = parse_type_primary(p);
        left = node;
    }
    return left;
}

static LhatNode *parse_type(Parser *p)
{
    LhatNode *left = parse_type_intersection(p);
    while (check_op(p, LHAT_OP_UNION)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_TYPE_UNION, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.left = left;
        node->v.binary.right = parse_type_intersection(p);
        left = node;
    }
    return left;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static LhatNode *simple_node(Parser *p)
{
    LhatToken t = p->current;
    LhatNodeKind kind;

    switch (t.kind) {
        case LHAT_TOKEN_INT:          kind = LHAT_NODE_INT; break;
        case LHAT_TOKEN_FLOAT:        kind = LHAT_NODE_FLOAT; break;
        case LHAT_TOKEN_STRING:       kind = LHAT_NODE_STRING; break;
        case LHAT_TOKEN_NAME_LITERAL: kind = LHAT_NODE_NAME; break;
        case LHAT_TOKEN_IDENT:        kind = LHAT_NODE_IDENT; break;
        case LHAT_TOKEN_HAT_IDENT:    kind = LHAT_NODE_HAT_IDENT; break;
        default: return NULL;
    }

    LhatNode *node = make(p, kind, &t);
    if (node == NULL) {
        return NULL;
    }

    switch (kind) {
        case LHAT_NODE_INT:
            node->v.integer.value = t.v.integer.value;
            node->v.integer.base = t.v.integer.base;
            break;
        case LHAT_NODE_FLOAT:
            node->v.real = t.v.real;
            break;
        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
            node->v.string.offset = t.v.string.offset;
            node->v.string.length = t.v.string.length;
            node->v.string.kind = t.v.string.kind;
            break;
        default:
            node->v.name.offset = t.offset;
            node->v.name.length = t.length;
            node->v.name.hats = kind == LHAT_NODE_HAT_IDENT ? t.v.hats : 0;
            break;
    }

    advance(p);
    return node;
}

// 5.4: the lexer hands over an interpolated string as a sequence, so the
// holes are ordinary expressions parsed by the ordinary rules.
static LhatNode *parse_interpolation(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // INTERP_BEGIN

    LhatNode *node = make(p, LHAT_NODE_INTERP, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    for (;;) {
        if (p->current.kind == LHAT_TOKEN_INTERP_TEXT) {
            LhatNode *text = make(p, LHAT_NODE_INTERP_TEXT, &p->current);
            if (text != NULL) {
                text->v.string.offset = p->current.v.string.offset;
                text->v.string.length = p->current.v.string.length;
            }
            lhat_node_append(&head, &tail, text);
            advance(p);
            continue;
        }
        if (p->current.kind == LHAT_TOKEN_INTERP_EXPR_BEGIN) {
            LhatToken at = p->current;
            advance(p);
            LhatNode *hole = make(p, LHAT_NODE_INTERP_HOLE, &at);
            if (hole != NULL) {
                hole->v.hole.value = parse_expression(p);
                if (p->current.kind == LHAT_TOKEN_INTERP_FORMAT) {
                    LhatNode *fmt = make(p, LHAT_NODE_INTERP_TEXT, &p->current);
                    if (fmt != NULL) {
                        fmt->v.string.offset = p->current.v.string.offset;
                        fmt->v.string.length = p->current.v.string.length;
                    }
                    hole->v.hole.format = fmt;
                    advance(p);
                }
            }
            if (p->current.kind == LHAT_TOKEN_INTERP_EXPR_END) {
                advance(p);
            } else {
                // 5.4: what closes a hole, spelled as the brace it is.
                report_expected(p, &p->current, LHAT_OP_RBRACE);
            }
            lhat_node_append(&head, &tail, hole);
            continue;
        }
        if (p->current.kind == LHAT_TOKEN_INTERP_END) {
            advance(p);
            break;
        }
        report(p, &p->current, LHAT_PARSE_ERR_UNEXPECTED);
        break;
    }

    node->v.list.items = head;
    return node;
}

// Entries of a braced list, shared by the table literal and by the field
// template of 14.6. A table may hold positional values; a template names
// every field, so `require_key` reports the difference here rather than
// leaving a nameless field for a later stage to puzzle over.
static LhatNode *parse_brace_entries(Parser *p, bool require_key)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *entry = make(p, LHAT_NODE_TABLE_ENTRY, &p->current);
        if (entry == NULL) {
            break;
        }

        // 14.15: a field the composition has to provide, written with its
        // type. Only a template has one -- a table literal makes a value,
        // and there is nothing there for a declaration to wait for.
        if (require_key && check_hat(p, "abstract")) {
            LhatToken at_marker = p->current;
            advance(p);
            entry->v.entry.declared = true;
            if (p->current.kind != LHAT_TOKEN_IDENT &&
                p->current.kind != LHAT_TOKEN_NAME_LITERAL &&
                p->current.kind != LHAT_TOKEN_HAT_IDENT) {
                report(p, &at_marker, LHAT_PARSE_ERR_FIELD_NEEDS_NAME);
                break;
            }
            entry->v.entry.key = simple_node(p);
            if (expect_op(p, LHAT_OP_COLON)) {
                entry->v.entry.value = parse_type(p);
            }
            lhat_node_append(&head, &tail, entry);
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        // 14.6改: an entry names a member that was not there, and 8.6 spells
        // that '='. ':=' is the older spelling and still reads.
        //
        // What '=' costs here is that 'a = 0' can no longer be a comparison
        // standing as a positional entry. Wrapping it says so -- '(' does not
        // begin a name, so the brackets take it out of this test entirely --
        // and wanting a list of comparisons is rarer than wanting members.
        bool keyed = (p->current.kind == LHAT_TOKEN_IDENT ||
                      p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
                      p->current.kind == LHAT_TOKEN_HAT_IDENT) &&
                     (is_op(&p->ahead, LHAT_OP_DEFINE) ||
                      is_op(&p->ahead, LHAT_OP_EQ));

        // 14.6改: '[ ... ] =' gives the key as an expression, for the keys
        // 01 の 6 章 leaves unwritable as names. Nothing else can begin with
        // '[' here -- it does not start an expression -- so no lookahead is
        // needed to tell this from a positional entry.
        if (check_op(p, LHAT_OP_LBRACKET)) {
            LhatToken at_bracket = p->current;
            advance(p);
            entry->v.entry.key = parse_expression(p);
            entry->v.entry.computed = true;
            expect_op(p, LHAT_OP_RBRACKET);
            if (!match_op(p, LHAT_OP_EQ) && !match_op(p, LHAT_OP_DEFINE)) {
                report_expected(p, &p->current, LHAT_OP_EQ);
            }
            // 14.6: a field template names every field, so a computed key is
            // not one of them.
            if (require_key) {
                report(p, &at_bracket, LHAT_PARSE_ERR_FIELD_NEEDS_NAME);
            }
        } else if (keyed) {
            entry->v.entry.key = simple_node(p);
            advance(p);  // '=' or ':='
        } else if (require_key) {
            report(p, &p->current, LHAT_PARSE_ERR_FIELD_NEEDS_NAME);
        }
        entry->v.entry.value = parse_expression(p);

        lhat_node_append(&head, &tail, entry);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

// 10.7: a bare '{ ... }' is a table literal.
static LhatNode *parse_table(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // {

    LhatNode *node = make(p, LHAT_NODE_TABLE, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.list.items = parse_brace_entries(p, false);
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

// 14.6 and 14.11: one spelling, two readings decided by where it stands. In
// the body of a def^ it declares the fields an instance gets; inside new^ it
// fills them in. Both name the fields of an instance, so the parser keeps
// them as one node and lets the position speak.
static LhatNode *parse_self_table(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // self^

    LhatNode *node = make(p, LHAT_NODE_SELF_TABLE, &start);
    if (node == NULL) {
        return NULL;
    }
    advance(p);  // {
    node->v.list.items = parse_brace_entries(p, true);
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

// A dotted path, as in IOError.NotFound. Kept as MEMBER nodes so it reads
// like any other qualified name in the tree.
static LhatNode *parse_qualified_name(Parser *p)
{
    if (p->current.kind != LHAT_TOKEN_IDENT &&
        p->current.kind != LHAT_TOKEN_NAME_LITERAL) {
        return error_node(p, LHAT_PARSE_ERR_EXPECTED_NAME);
    }

    LhatNode *node = simple_node(p);
    while (check_op(p, LHAT_OP_DOT)) {
        LhatToken at = p->current;
        advance(p);
        if (p->current.kind != LHAT_TOKEN_IDENT &&
            p->current.kind != LHAT_TOKEN_NAME_LITERAL) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }
        node = access_node(p, LHAT_NODE_MEMBER, &at, node, simple_node(p), false);
    }
    return node;
}

// 04 の 2.5. The kind is written into the construction rather than left to a
// member, since 2.3 makes the kind the type. The leading error^ is what lets
// this be recognised without knowing any types: a qualified name followed by
// '{' would otherwise read as a name and a table literal (10.7).
static LhatNode *parse_error_new(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // error^

    LhatNode *node = make(p, LHAT_NODE_ERROR_NEW, &start);
    if (node == NULL) {
        return NULL;
    }

    if (check_op(p, LHAT_OP_LBRACE)) {
        // 2.2 requires a kind, and 2.5 puts it in the syntax, so an error^
        // with nothing in front of the brace has lost it rather than defaulted.
        report(p, &p->current, LHAT_PARSE_ERR_ERROR_NEEDS_KIND);
    } else {
        node->v.named.name = parse_qualified_name(p);
    }

    if (expect_op(p, LHAT_OP_LBRACE)) {
        node->v.named.members = parse_brace_entries(p, true);
        expect_op(p, LHAT_OP_RBRACE);
    }
    return node;
}

// 14 章. def^ stays an expression (14.9), so the name of a definition comes
// from whatever it is bound to and composition reads as an ordinary '..'.
static LhatNode *parse_def(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // def^

    LhatNode *node = make(p, LHAT_NODE_DEF, &start);
    if (node == NULL) {
        return NULL;
    }
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return node;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    bool seen_template = false;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatToken at = p->current;
        LhatDefModifier modifier = LHAT_DEF_PLAIN;

        // 14.12. The marker leads the member it applies to.
        if (match_hat(p, "override")) {
            modifier = LHAT_DEF_OVERRIDE;
        } else if (match_hat(p, "overload")) {
            modifier = LHAT_DEF_OVERLOAD;
        } else if (match_hat(p, "abstract")) {
            // 14.15: a third marker in the same place, since what it says is
            // about the member as a whole the way the other two are.
            modifier = LHAT_DEF_ABSTRACT;
        }

        LhatNode *entry = make(p, LHAT_NODE_TABLE_ENTRY, &at);
        if (entry == NULL) {
            break;
        }
        entry->v.entry.modifier = modifier;

        if (check_hat(p, "self") && is_op(&p->ahead, LHAT_OP_LBRACE)) {
            // 14.3: the template is the one entry that is not a member, so it
            // carries no key and no marker of 14.12 can apply to it.
            if (modifier != LHAT_DEF_PLAIN) {
                report(p, &at, LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE);
            }
            if (seen_template) {
                report(p, &p->current, LHAT_PARSE_ERR_DUPLICATE_TEMPLATE);
            }
            seen_template = true;
            entry->v.entry.value = parse_self_table(p);
        } else if (check_hat(p, "op")) {
            // 11.1: an operator is a function, and a definition carries it as
            // an ordinary member. The name is the operator itself, which
            // 01 の 6 章 keeps a program from writing by hand -- so nothing
            // of the writer's can collide with it.
            advance(p);
            LhatToken symbol = p->current;
            // 11.8: '..' and the arithmetic of 11.4. and^, or^ and '!' stay
            // built in, and 11.5's comparisons decide by disjointness rather
            // than by asking a type -- neither takes an op^.
            bool definable = symbol.kind == LHAT_TOKEN_OP &&
                             (symbol.v.op == LHAT_OP_CONCAT ||
                              symbol.v.op == LHAT_OP_ADD ||
                              symbol.v.op == LHAT_OP_SUB ||
                              symbol.v.op == LHAT_OP_MUL ||
                              symbol.v.op == LHAT_OP_DIV ||
                              symbol.v.op == LHAT_OP_FLOORDIV ||
                              symbol.v.op == LHAT_OP_MOD ||
                              symbol.v.op == LHAT_OP_POW);
            if (!definable) {
                report(p, &symbol, LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE);
                break;
            }
            LhatNode *name = make(p, LHAT_NODE_IDENT, &symbol);
            if (name != NULL) {
                name->v.name.offset = symbol.offset;
                name->v.name.length = symbol.length;
                name->v.name.hats = 0;
            }
            entry->v.entry.key = name;
            advance(p);
            if (expect_op(p, LHAT_OP_DEFINE)) {
                entry->v.entry.value = parse_expression(p);
            }
        } else if (p->current.kind == LHAT_TOKEN_IDENT ||
                   p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
                   p->current.kind == LHAT_TOKEN_HAT_IDENT) {
            entry->v.entry.key = simple_node(p);
            // 14.15: an abstract^ member is written with its type, since
            // there is no value to read it from. 14.10 spells a member's
            // type the same way, so the two agree.
            if (modifier == LHAT_DEF_ABSTRACT) {
                entry->v.entry.declared = true;
                if (expect_op(p, LHAT_OP_COLON)) {
                    entry->v.entry.value = parse_type(p);
                }
            } else if (expect_op(p, LHAT_OP_DEFINE)) {
                entry->v.entry.value = parse_expression(p);
            }
        } else {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_MEMBER);
            break;
        }

        lhat_node_append(&head, &tail, entry);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }

    node->v.list.items = head;
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

// A parameter in a definition may be named, typed and given a default.
static LhatNode *parse_params(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_LBRACE) &&
           !check_op(p, LHAT_OP_ARROW) && !check_op(p, LHAT_OP_COLONCOLON)) {
        LhatNode *param = make(p, LHAT_NODE_PARAM, &p->current);
        if (param == NULL) {
            break;
        }

        if (match_op(p, LHAT_OP_ELLIPSIS)) {
            param->v.param.variadic = true;
            if (match_op(p, LHAT_OP_COLON)) {
                param->v.param.type = parse_type(p);
            }
        } else if (p->current.kind == LHAT_TOKEN_IDENT ||
                   p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
                   p->current.kind == LHAT_TOKEN_HAT_IDENT) {
            param->v.param.name = simple_node(p);
            if (match_op(p, LHAT_OP_COLON)) {
                param->v.param.type = parse_type(p);
            }
            if (match_op(p, LHAT_OP_EQ)) {
                param->v.param.fallback = parse_expression(p);
            }
        } else {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }

        lhat_node_append(&head, &tail, param);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

// 15.12: a function whose body is one expression answers with it, without a
// return^ written. Whether the body is one statement shows only once the list
// has ended, so a bare expression is read as an expression statement while
// parsing and sorted out here.
//
// The narrow form on purpose. Anywhere it does not reach, 8.2 holds as it
// did, and a call standing alone is untouched -- it was already a statement
// and already meant "run this and drop what it answers". So nothing that
// compiles today changes what it does.
static void answer_with_body(Parser *p, LhatNode *body)
{
    if (body == NULL || body->kind != LHAT_NODE_BLOCK) {
        return;
    }

    LhatNode *only = body->v.list.items;
    if (only != NULL && only->next == NULL) {
        // Whatever it computed is what the function answers with -- a call
        // included. Telling a call that answers a value from one that does
        // not is the checker's to do, and it does: 13.2 already refuses a f^
        // whose body is one call, so this can only make a refused body work.
        // Where the call answers nothing, 'return^ foo()' is refused in its
        // turn, and says so better than falling out did.
        if (only->kind == LHAT_NODE_CALL_STMT) {
            only->kind = LHAT_NODE_RETURN;
        }
        return;
    }

    // More than one statement, so 8.2 holds and a bare expression was never
    // allowed here. A call standing alone still is.
    for (LhatNode *s = body->v.list.items; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_CALL_STMT ||
            is_call_statement(s->v.jump.value)) {
            continue;
        }
        LhatToken at;
        memset(&at, 0, sizeof at);
        // Anything but EOF: 3.1 reads that as input that stopped early, and
        // this input did not.
        at.kind = LHAT_TOKEN_IDENT;
        at.offset = s->offset;
        at.line = s->line;
        at.column = s->column;
        p->panicking = false;
        report(p, &at, LHAT_PARSE_ERR_BARE_EXPRESSION);
    }
}

static LhatNode *parse_function(Parser *p, bool is_function)
{
    LhatToken start = p->current;
    advance(p);  // f^ or p^

    LhatNode *node = make(p, LHAT_NODE_FUNC, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.func.is_function = is_function;
    node->v.func.params = parse_params(p);

    if (check_op(p, LHAT_OP_COLONCOLON)) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON);
        advance(p);
        node->v.func.return_type = parse_type(p);
    } else if (match_op(p, LHAT_OP_ARROW)) {
        node->v.func.return_type = parse_type(p);
    }

    // 15.2: yield^ in the body is what makes a procedure yieldable, so the
    // flag is collected while parsing rather than by walking the tree
    // afterwards. A nested definition gets its own scope for the same reason
    // its yield^ belongs to it and not to this one.
    bool enclosing = p->saw_yield;
    p->saw_yield = false;

    LhatToken brace = p->current;
    if (expect_op(p, LHAT_OP_LBRACE)) {
        // 15.12: only a function, and only in this list -- not in a block
        // nested inside it. 13.2 makes a f^ that falls out an error already,
        // so this can only make a refused body work and never change one.
        size_t enclosing_bare = p->bare_depth;
        p->bare_depth = is_function ? p->depth + 1 : 0;

        // A subroutine body is not a loop, so 9.10's walk-only rules do not
        // apply to it either.
        node->v.func.body = parse_clause_body(p, &brace, false, false);

        p->bare_depth = enclosing_bare;
        if (is_function) {
            answer_with_body(p, node->v.func.body);
        }
        expect_op(p, LHAT_OP_RBRACE);
    }

    node->v.func.yields = p->saw_yield;
    p->saw_yield = enclosing;
    return node;
}

// 5.1 and 5.3: one marker, 'el^', covers both else-if and else, and a
// condition before the ':' is what tells them apart. The longer spellings are
// accepted too.
static bool is_else_marker(const Parser *p)
{
    return check_hat(p, "el") || check_hat(p, "else") || check_hat(p, "ei") ||
           check_hat(p, "elseif") || check_hat(p, "elsif") || check_hat(p, "elif");
}

// Words that begin a statement. The lexer keeps no keyword table (01 の 2.1),
// so this knowledge lives here. It is needed wherever a construct may be
// followed by an optional expression: without it, 'break^' would swallow the
// 'yield^ 1' that follows it as though it were its operand.
static bool is_statement_keyword(const Parser *p)
{
    static const char *const words[] = {
        "if", "do", "let", "with", "return", "break", "panic", "yield", "_yield",
        "for", "repeat", "while", "until", "when", "other", "errordef",
        "prolog", "prologue", "pre", "premain", "first", "main", "last",
        "epilog", "epilogue", "finally"
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (check_hat(p, words[i])) {
            return true;
        }
    }
    return is_else_marker(p);
}

// Whether what stands here ends the if^ expression rather than continuing a
// clause. 6 章 closes it with ';', and an input that stopped early closes it
// by running out -- 3.1 reads on from there rather than reporting.
static bool closes_if_expression(const Parser *p)
{
    return at_eof(p) || check_op(p, LHAT_OP_SEMICOLON) ||
           check_op(p, LHAT_OP_RBRACE) || check_op(p, LHAT_OP_RPAREN);
}

// 5.1: the condition is already read by the time the ':' says which form
// this is, so the two callers hand it in rather than reading it again.
static LhatNode *parse_if_expression_from(Parser *p, LhatToken start,
                                          LhatNode *condition)
{
    LhatNode *node = make(p, LHAT_NODE_IF_EXPR, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    LhatNode *first = make(p, LHAT_NODE_IF_CLAUSE, &start);
    if (first == NULL) {
        return node;
    }
    first->v.clause.condition = condition;
    expect_op(p, LHAT_OP_COLON);
    first->v.clause.body = parse_expression(p);
    lhat_node_append(&head, &tail, first);

    while (is_else_marker(p)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *clause = make(p, LHAT_NODE_IF_CLAUSE, &at);
        if (clause == NULL) {
            break;
        }
        if (!check_op(p, LHAT_OP_COLON)) {
            clause->v.clause.condition = parse_expression(p);
        }
        // 5.1: 'el^' takes a condition or nothing, and which one it was is
        // known only once the ':' turns up. When what turns up instead is the
        // ';' that closes the construct (6 章), there was no condition -- what
        // was read is the value, and the ':' the writer left out belongs
        // right after the marker.
        //
        // Reported there rather than at the ';', which is where the parser
        // noticed but not where the writer has to look. Taking the expression
        // as the value also keeps the rest of the construct readable.
        if (!check_op(p, LHAT_OP_COLON) && closes_if_expression(p)) {
            report(p, &at, LHAT_PARSE_ERR_ELSE_NEEDS_COLON);
            clause->v.clause.body = clause->v.clause.condition;
            clause->v.clause.condition = NULL;
            lhat_node_append(&head, &tail, clause);
            break;
        }
        expect_op(p, LHAT_OP_COLON);
        clause->v.clause.body = parse_expression(p);
        lhat_node_append(&head, &tail, clause);
    }

    // 6 章: a ':' opens the construct and a ';' closes it.
    expect_op(p, LHAT_OP_SEMICOLON);
    node->v.list.items = head;
    return node;
}

static LhatNode *parse_if_expression(Parser *p, LhatToken start)
{
    return parse_if_expression_from(p, start, parse_expression(p));
}

static LhatNode *parse_primary(Parser *p)
{
    LhatToken t = p->current;

    switch (t.kind) {
        case LHAT_TOKEN_INT:
        case LHAT_TOKEN_FLOAT:
        case LHAT_TOKEN_STRING:
        case LHAT_TOKEN_NAME_LITERAL:
        case LHAT_TOKEN_IDENT:
            return simple_node(p);

        case LHAT_TOKEN_SCOPE: {
            LhatNode *node = make(p, LHAT_NODE_SCOPE, &t);
            advance(p);
            if (node != NULL) {
                node->v.scope.kind = t.v.scope.kind;
                node->v.scope.depth = t.v.scope.depth;
                // 01 の 8 章: the name is glued to the sigil, so it is part
                // of this node rather than a separate expression.
                node->v.scope.name = simple_node(p);
                if (node->v.scope.name == NULL) {
                    report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
                }
            }
            return node;
        }

        case LHAT_TOKEN_INTERP_BEGIN:
            return parse_interpolation(p);

        case LHAT_TOKEN_HAT_IDENT:
            if (check_hat(p, "f")) {
                return parse_function(p, true);
            }
            if (check_hat(p, "p")) {
                return parse_function(p, false);
            }
            if (check_hat(p, "if")) {
                advance(p);
                return parse_if_expression(p, t);
            }
            // 17.2: the expression form of a match. parse_for sorts out which
            // form it is, since both start the same way.
            if (check_hat(p, "for")) {
                return parse_for(p);
            }
            if (check_hat(p, "def")) {
                return parse_def(p);
            }
            // 02 の 14.16: always parenthesized, and always exactly one
            // operand -- a primary in its own right rather than a prefix
            // operator with a precedence of its own.
            if (check_hat(p, "typeof")) {
                LhatToken at = p->current;
                advance(p);
                LhatNode *node = make(p, LHAT_NODE_TYPEOF, &at);
                if (node == NULL) {
                    return NULL;
                }
                if (!expect_op(p, LHAT_OP_LPAREN)) {
                    return node;
                }
                node->v.jump.value = parse_expression(p);
                expect_op(p, LHAT_OP_RPAREN);
                return node;
            }
            // 'self^' on its own is an ordinary value, as in self^.value1, so
            // only a '{' immediately after it makes the form of 14.6. The
            // parameter list of 14.4 never reaches here: it takes the name
            // directly, and its '{' opens the body.
            if (check_hat(p, "self") && is_op(&p->ahead, LHAT_OP_LBRACE)) {
                return parse_self_table(p);
            }
            // 04 の 14.5: 'error^' followed by a name constructs; on its own
            // it is the supertype, which only a type position may ask for.
            if (check_hat(p, "error") &&
                (p->ahead.kind == LHAT_TOKEN_IDENT ||
                 p->ahead.kind == LHAT_TOKEN_NAME_LITERAL ||
                 is_op(&p->ahead, LHAT_OP_LBRACE))) {
                return parse_error_new(p);
            }
            return simple_node(p);

        case LHAT_TOKEN_OP:
            if (t.v.op == LHAT_OP_LPAREN) {
                advance(p);
                LhatNode *inner = parse_expression(p);
                expect_op(p, LHAT_OP_RPAREN);
                return inner;
            }
            if (t.v.op == LHAT_OP_LBRACE) {
                return parse_table(p);
            }
            // 13.7: inside a body '...' names the collected arguments.
            if (t.v.op == LHAT_OP_ELLIPSIS) {
                LhatNode *node = make(p, LHAT_NODE_HAT_IDENT, &t);
                if (node != NULL) {
                    node->v.name.offset = t.offset;
                    node->v.name.length = t.length;
                }
                advance(p);
                return node;
            }
            break;

        default:
            break;
    }

    return error_node(p, LHAT_PARSE_ERR_EXPECTED_EXPRESSION);
}

static LhatNode *access_node(Parser *p, LhatNodeKind kind, const LhatToken *at,
                             LhatNode *target, LhatNode *argument, bool nil_safe)
{
    LhatNode *node = make(p, kind, at);
    if (node == NULL) {
        return target;
    }
    node->v.access.target = target;
    node->v.access.argument = argument;
    node->v.access.nil_safe = nil_safe;
    return node;
}

static LhatNode *parse_arguments(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RPAREN)) {
        LhatNode *argument;
        // 13.7: '...' bare, written as an argument, forwards the collected
        // tail into this call rather than naming it as a value in its own
        // right -- there is no other reading a table has here, since a
        // variadic slot never takes one table as a single element.
        if (check_op(p, LHAT_OP_ELLIPSIS)) {
            LhatToken at = p->current;
            LhatNode *collected = make(p, LHAT_NODE_HAT_IDENT, &at);
            if (collected != NULL) {
                collected->v.name.offset = at.offset;
                collected->v.name.length = at.length;
            }
            advance(p);
            LhatNode *spread = make(p, LHAT_NODE_SPREAD, &at);
            if (spread != NULL) {
                spread->v.jump.value = collected;
            }
            argument = spread;
            lhat_node_append(&head, &tail, argument);
            // 13.7: nothing can follow the whole tail it forwards.
            if (check_op(p, LHAT_OP_COMMA)) {
                report(p, &p->current, LHAT_PARSE_ERR_SPREAD_NOT_LAST);
            }
            break;
        }
        argument = parse_expression(p);
        lhat_node_append(&head, &tail, argument);
        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    expect_op(p, LHAT_OP_RPAREN);
    return head;
}

static LhatNode *parse_postfix(Parser *p)
{
    LhatNode *node = parse_primary(p);

    for (;;) {
        LhatToken at = p->current;

        if (check_op(p, LHAT_OP_DOT) || check_op(p, LHAT_OP_NIL_DOT)) {
            bool nil_safe = at.v.op == LHAT_OP_NIL_DOT;
            advance(p);
            // 10.1: digits after a '.' are an integer key.
            LhatNode *name = simple_node(p);
            if (name == NULL) {
                report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
                return node;
            }
            node = access_node(p, LHAT_NODE_MEMBER, &at, node, name, nil_safe);
            continue;
        }

        if (check_op(p, LHAT_OP_LBRACKET) || check_op(p, LHAT_OP_NIL_INDEX)) {
            bool nil_safe = at.v.op == LHAT_OP_NIL_INDEX;
            advance(p);
            LhatNode *index = parse_expression(p);
            expect_op(p, LHAT_OP_RBRACKET);
            node = access_node(p, LHAT_NODE_INDEX, &at, node, index, nil_safe);
            continue;
        }

        // 10.9: a '(' on a line of its own starts a statement rather than
        // continuing this expression.
        if (check_op(p, LHAT_OP_LPAREN) && !at.preceded_by_newline) {
            advance(p);
            node = access_node(p, LHAT_NODE_CALL, &at, node,
                               parse_arguments(p), false);
            continue;
        }
        if (check_op(p, LHAT_OP_NIL_CALL)) {
            advance(p);
            node = access_node(p, LHAT_NODE_CALL, &at, node,
                               parse_arguments(p), true);
            continue;
        }

        break;
    }
    return node;
}

static LhatNode *parse_unary(Parser *p);

// 11.5 の (2): '**' binds tighter than a unary minus, and its right operand
// may itself be unary, so '-2 ** 2' is -4 while '2 ** -1' still parses.
static LhatNode *parse_power(Parser *p)
{
    LhatNode *left = parse_postfix(p);
    if (check_op(p, LHAT_OP_POW)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_BINARY, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.op = LHAT_OP_POW;
        node->v.binary.left = left;
        node->v.binary.right = parse_unary(p);
        return node;
    }
    return left;
}

static LhatNode *parse_unary(Parser *p)
{
    // 05 の 5 章. Unary like try^, so '(require^ "m").f' needs the brackets
    // that say what is being reached into.
    if (check_hat(p, "require")) {
        LhatToken at = p->current;
        advance(p);

        LhatNode *node = make(p, LHAT_NODE_REQUIRE, &at);
        if (node == NULL) {
            return NULL;
        }
        // 05 の 5.2: the checker follows this, so the path has to be settled
        // by the time it looks. A computed one is a different mechanism (M3).
        if (p->current.kind != LHAT_TOKEN_STRING) {
            report(p, &p->current, LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL);
            return node;
        }
        node->v.jump.value = simple_node(p);
        return node;
    }

    // 05 の 8.7: import^ names a module rather than a file, so its operand is
    // the path 3 章 spells rather than a string. Only what the host
    // registered answers to it -- see 8.7 for why not what require^ brought.
    if (check_hat(p, "import")) {
        LhatToken at = p->current;
        advance(p);

        LhatNode *node = make(p, LHAT_NODE_IMPORT, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = parse_qualified_name(p);
        return node;
    }

    // 02 の 15.8: delegation. A word of its own rather than a reading of
    // yield^, since the two have different types -- yield^ answers what the
    // resume sent, this answers the inner coroutine's return value.
    if (check_hat(p, "yieldall")) {
        LhatToken at = p->current;
        p->saw_yield = true;  // 15.2: delegating is suspending
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_YIELD_ALL, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = parse_unary(p);
        return node;
    }

    // 02 の 15.4: yield^ is an expression, since its value is what the resume
    // sent. It takes everything to its right, so 'yield^ a + 1' sends the sum
    // -- there is nothing for a tighter reading to do with the remainder.
    //
    // 15.11: '_yield^' is written the same way and says the same thing about
    // the three types. It is the same node with a flag rather than one of
    // its own, so that no part of the checker can tell the two apart.
    bool phantom_yield = check_hat(p, "_yield");
    if (phantom_yield || check_hat(p, "yield")) {
        LhatToken at = p->current;
        p->saw_yield = true;  // 15.2: this is what makes the body yieldable
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_YIELD, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.phantom = phantom_yield;
        // 01 の 10.9 again: what it sends has to be on its own line.
        if (!p->current.preceded_by_newline && starts_expression(&p->current) &&
            !is_statement_keyword(p)) {
            node->v.jump.value = parse_expression(p);
        }
        return node;
    }

    // 04 の 5 章: try^ sits at the unary level, so 'try^ f() + 1' adds to the
    // unwrapped value rather than trying to unwrap the sum.
    if (check_hat(p, "try")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_TRY, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = parse_unary(p);
        return node;
    }

    if (check_op(p, LHAT_OP_NOT) || check_op(p, LHAT_OP_SUB)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_UNARY, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.unary.op = at.v.op;
        node->v.unary.operand = parse_unary(p);
        return node;
    }
    return parse_power(p);
}

// 04 の 4 章 and 11.7. Both drop one arm of a union and put a value in its
// place; only the arm differs, so they share a level, a shape and a side of
// the tree. Binding tighter than the binary operators is what makes
// 'base + t[k] ?? 0' default around t[k] rather than around the sum.
static LhatNode *parse_fallback(Parser *p)
{
    LhatNode *left = parse_unary(p);

    for (;;) {
        bool catching = check_hat(p, "catch");
        if (!catching && !check_op(p, LHAT_OP_NIL_ELSE)) {
            break;
        }

        LhatToken at = p->current;
        advance(p);

        LhatNode *node = make(p, LHAT_NODE_BINARY, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.op = catching ? LHAT_OP_CATCH : LHAT_OP_NIL_ELSE;
        node->v.binary.left = left;
        node->v.binary.right = parse_unary(p);
        left = node;
    }
    return left;
}

static LhatNode *parse_ascription(Parser *p)
{
    LhatNode *left = parse_fallback(p);
    while (check_hat(p, "as")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_AS, &at);
        if (node == NULL) {
            return left;
        }
        node->v.ascription.value = left;
        node->v.ascription.type = parse_type(p);
        left = node;
    }
    return left;
}

static bool binary_info(const Parser *p, LhatOpKind *op, int *precedence,
                        bool *right_associative)
{
    *right_associative = false;

    if (p->current.kind == LHAT_TOKEN_HAT_IDENT) {
        if (check_hat(p, "or")) {
            *op = LHAT_OP_OR;
            *precedence = PREC_OR;
            return true;
        }
        if (check_hat(p, "and")) {
            *op = LHAT_OP_AND;
            *precedence = PREC_AND;
            return true;
        }
        return false;
    }

    if (p->current.kind != LHAT_TOKEN_OP) {
        return false;
    }

    switch (p->current.v.op) {
        case LHAT_OP_CONCAT:
            *op = LHAT_OP_CONCAT;
            *precedence = PREC_CONCAT;
            *right_associative = true;  // 11.5 の (1)
            return true;
        case LHAT_OP_ADD:
        case LHAT_OP_SUB:
            *op = p->current.v.op;
            *precedence = PREC_ADD;
            return true;
        case LHAT_OP_MUL:
        case LHAT_OP_DIV:
        case LHAT_OP_FLOORDIV:
        case LHAT_OP_MOD:
            *op = p->current.v.op;
            *precedence = PREC_MUL;
            return true;
        default:
            return false;
    }
}

static bool is_comparison(const Parser *p, LhatOpKind *op)
{
    // 11.6改: 'is^' asks identity now; 'isa^' took over the type-fit
    // question is^ used to ask. Checking 'isa' first would not matter
    // either way -- the lexer already reads 'isa' as one identifier, never
    // 'is' followed by a stray 'a' -- but it reads the same order the two
    // words are introduced in (13.11 の後の 11.6改).
    if (check_hat(p, "isa")) {
        *op = LHAT_OP_ISA;
        return true;
    }
    if (check_hat(p, "is")) {
        *op = LHAT_OP_IS;
        return true;
    }
    if (p->current.kind != LHAT_TOKEN_OP) {
        return false;
    }
    switch (p->current.v.op) {
        case LHAT_OP_EQ:
        case LHAT_OP_NE:
        case LHAT_OP_LT:
        case LHAT_OP_GT:
        case LHAT_OP_LE:
        case LHAT_OP_GE:
            *op = p->current.v.op;
            return true;
        default:
            return false;
    }
}

static LhatNode *parse_binary(Parser *p, int min_precedence)
{
    LhatNode *left = parse_ascription(p);

    for (;;) {
        LhatOpKind op;
        int precedence;
        bool right_associative;
        if (!binary_info(p, &op, &precedence, &right_associative) ||
            precedence < min_precedence) {
            break;
        }

        LhatToken at = p->current;
        advance(p);

        LhatNode *right = parse_binary(
            p, right_associative ? precedence : precedence + 1);

        LhatNode *node = make(p, LHAT_NODE_BINARY, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.op = op;
        node->v.binary.left = left;
        node->v.binary.right = right;
        left = node;
    }
    return left;
}

// 11.5 の (5): comparisons chain, and an operand shared by two links is
// evaluated once, which is why the chain is kept as one node.
static LhatNode *parse_comparison(Parser *p)
{
    LhatNode *first = parse_binary(p, PREC_CONCAT);

    LhatOpKind op;
    if (!is_comparison(p, &op)) {
        return first;
    }

    LhatNode *operands = first;
    LhatNode *operand_tail = first;
    LhatNode *operators = NULL;
    LhatNode *operator_tail = NULL;
    size_t links = 0;

    while (is_comparison(p, &op)) {
        LhatToken at = p->current;
        advance(p);

        LhatNode *marker = make(p, LHAT_NODE_UNARY, &at);
        if (marker == NULL) {
            break;
        }
        marker->v.unary.op = op;
        lhat_node_append(&operators, &operator_tail, marker);

        // 13.11: isa^ asks whether the left side may stand where the right
        // side is written, so what it takes on the right is a type. is^
        // asks identity, an ordinary value on both sides.
        lhat_node_append(&operands, &operand_tail,
                         op == LHAT_OP_ISA ? parse_type(p)
                                           : parse_binary(p, PREC_CONCAT));
        links++;
    }

    if (links == 1) {
        LhatNode *node = make(p, LHAT_NODE_BINARY, NULL);
        if (node == NULL) {
            return first;
        }
        node->offset = operators->offset;
        node->line = operators->line;
        node->column = operators->column;
        node->v.binary.op = operators->v.unary.op;
        node->v.binary.left = operands;
        node->v.binary.right = operands->next;
        operands->next = NULL;
        return node;
    }

    LhatNode *node = make(p, LHAT_NODE_COMPARE_CHAIN, NULL);
    if (node == NULL) {
        return first;
    }
    node->offset = operands->offset;
    node->line = operands->line;
    node->column = operands->column;
    node->v.chain.operands = operands;
    node->v.chain.operators = operators;
    return node;
}

static LhatNode *parse_logical(Parser *p, int min_precedence)
{
    LhatNode *left = parse_comparison(p);

    for (;;) {
        LhatOpKind op;
        int precedence;
        bool right_associative;
        if (!binary_info(p, &op, &precedence, &right_associative) ||
            precedence > PREC_AND || precedence < min_precedence) {
            break;
        }

        LhatToken at = p->current;
        advance(p);

        LhatNode *right = parse_logical(p, precedence + 1);
        LhatNode *node = make(p, LHAT_NODE_BINARY, &at);
        if (node == NULL) {
            return left;
        }
        node->v.binary.op = op;
        node->v.binary.left = left;
        node->v.binary.right = right;
        left = node;
    }
    return left;
}

static LhatNode *parse_expression(Parser *p)
{
    return parse_logical(p, PREC_OR);
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static bool starts_expression(const LhatToken *token)
{
    switch (token->kind) {
        case LHAT_TOKEN_INT:
        case LHAT_TOKEN_FLOAT:
        case LHAT_TOKEN_STRING:
        case LHAT_TOKEN_NAME_LITERAL:
        case LHAT_TOKEN_IDENT:
        case LHAT_TOKEN_HAT_IDENT:
        case LHAT_TOKEN_SCOPE:
        case LHAT_TOKEN_INTERP_BEGIN:
            return true;
        default:
            return false;
    }
}

// 2.3: whether this token could carry on the expression that came before it.
// The same classification 01 の 10.9 uses, which is why the command form
// needs no notion of its own -- 'x - 1' stays a subtraction because '-' can
// continue, while 'print "done"' is a call because a string cannot.
static bool continues_expression(const Parser *p, const LhatToken *token)
{
    switch (token->kind) {
        case LHAT_TOKEN_OP:
            switch (token->v.op) {
                // Anything that could not start a fresh expression on its own
                // is a continuation. '!' and '-' differ: '!' is prefix only
                // (Q3), '-' can be either, and 2.3 gives '-' to subtraction.
                case LHAT_OP_NOT:
                case LHAT_OP_LBRACE:
                    return false;
                default:
                    return true;
            }
        case LHAT_TOKEN_HAT_IDENT:
            // The binary ones (11.6). Everything else with a hat starts
            // something and so may be an argument.
            return token_is_hat(p, token, "and") || token_is_hat(p, token, "or") ||
                   token_is_hat(p, token, "is") || token_is_hat(p, token, "as") ||
                   token_is_hat(p, token, "catch") ||
                   token_is_hat(p, token, "to");
        default:
            return false;
    }
}

// 8.3: the forms a statement may begin with. Anything else appearing after a
// finished statement is a continuation that does not belong there.
static bool can_begin_statement(const Parser *p)
{
    switch (p->current.kind) {
        case LHAT_TOKEN_IDENT:
        case LHAT_TOKEN_NAME_LITERAL:
        case LHAT_TOKEN_SCOPE:
            return true;
        case LHAT_TOKEN_HAT_IDENT:
            // Some hat identifiers name a value rather than syntax -- the
            // machine (05 の 8.6), a coroutine's own handle (15.10), the
            // receiver (14.4) and the focus (16.2). A statement can start
            // with any of them, the way one can start with any other name
            // (8.3), so 2.1 must not read them as a further argument to the
            // call on the line above.
            return is_statement_keyword(p) || check_hat(p, "L") ||
                   check_hat(p, "this") || check_hat(p, "self") ||
                   check_hat(p, "it") || check_hat(p, "super");
        case LHAT_TOKEN_OP:
            // 01 の 10.9: a '(' on a fresh line opens a statement.
            return p->current.v.op == LHAT_OP_LPAREN;
        default:
            return false;
    }
}

// 9.2: the clause markers, in the order they must appear. Returns -1 when
// the current token is not one of them.
static int clause_index(const Parser *p)
{
    if (check_hat(p, "prolog") || check_hat(p, "prologue")) {
        return LHAT_CLAUSE_PROLOG;
    }
    // 9.10: pre^ runs before the condition is tested, which is what gives a
    // loop the shape C spells do ... while. premain^ is the same word (9.6).
    if (check_hat(p, "pre") || check_hat(p, "premain")) {
        return LHAT_CLAUSE_PRE;
    }
    if (check_hat(p, "first")) {
        return LHAT_CLAUSE_FIRST;
    }
    if (check_hat(p, "main")) {
        return LHAT_CLAUSE_MAIN;
    }
    if (check_hat(p, "last")) {
        return LHAT_CLAUSE_LAST;
    }
    if (check_hat(p, "epilog") || check_hat(p, "epilogue")) {
        return LHAT_CLAUSE_EPILOG;
    }
    if (check_hat(p, "finally")) {
        return LHAT_CLAUSE_FINALLY;
    }
    return -1;
}

static LhatNode *parse_statement_list(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    p->depth++;

    // Stops at an else marker and at a clause marker as well as at '}',
    // because 5.2 and 9.2 both put those inside the braces.
    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE) && !is_else_marker(p) &&
           clause_index(p) < 0) {
        uint32_t before = p->current.offset;

        LhatNode *statement = parse_statement(p);
        if (statement == NULL) {
            break;
        }
        lhat_node_append(&head, &tail, statement);

        if (p->panicking) {
            synchronize(p);
        }

        // Guarantees progress: a statement that consumed nothing would
        // otherwise spin here for ever.
        if (p->current.offset == before && !at_eof(p)) {
            advance(p);
        }
    }
    p->depth--;
    return head;
}

static LhatNode *parse_block_body(Parser *p, const LhatToken *at)
{
    LhatNode *block = make(p, LHAT_NODE_BLOCK, at);
    if (block == NULL) {
        return NULL;
    }
    block->v.list.items = parse_statement_list(p);
    return block;
}

// A body that may carry the clauses of 9 章. Outside a loop only finally^ is
// allowed (10.1), since the others describe how an iteration proceeds.
// `walks` marks a for^ ... in^, which 9.10 keeps pre^ out of.
static LhatNode *parse_clause_body(Parser *p, const LhatToken *at, bool in_loop,
                                   bool walks)
{
    LhatNode *block = make(p, LHAT_NODE_BLOCK, at);
    if (block == NULL) {
        return NULL;
    }

    block->v.list.items = parse_statement_list(p);
    bool unlabelled = block->v.list.items != NULL;

    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    int previous = -1;
    // 9.3改: an unheaded statement list is an implicit main^, so it counts.
    bool has_body = unlabelled;
    bool saw_clause = false;

    for (int index; (index = clause_index(p)) >= 0;) {
        LhatToken at_clause = p->current;

        if (!in_loop && index != LHAT_CLAUSE_FINALLY) {
            report(p, &at_clause, LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP);
        } else if (index <= previous) {
            report(p, &at_clause, LHAT_PARSE_ERR_CLAUSE_ORDER);
        } else if (unlabelled && index <= LHAT_CLAUSE_MAIN) {
            // 9.3: prolog^, pre^ and first^ lead, so statements written before
            // them would be swallowed; main^ must be named to keep them apart.
            report(p, &at_clause, LHAT_PARSE_ERR_MAIN_REQUIRED);
        } else if (walks && index == LHAT_CLAUSE_PRE) {
            // 9.10: what a walk binds is bound after the coroutine answers,
            // so a clause running before that would read the turn before.
            report(p, &at_clause, LHAT_PARSE_ERR_PRE_IN_WALK);
        }
        saw_clause = true;
        if (LHAT_CLAUSE_IS_BODY(index)) {
            has_body = true;
        }
        previous = index;

        advance(p);
        expect_op(p, LHAT_OP_COLON);

        LhatNode *statements = parse_statement_list(p);
        if (index == LHAT_CLAUSE_MAIN) {
            block->v.list.items = statements;
            continue;
        }

        LhatNode *clause = make(p, LHAT_NODE_LOOP_CLAUSE, &at_clause);
        if (clause == NULL) {
            break;
        }
        clause->v.loop_clause.kind = (LhatClauseKind)index;
        clause->v.loop_clause.body = statements;
        lhat_node_append(&head, &tail, clause);
    }

    // 9.3改: first^, pre^, main^ and last^ are the body. Once the braces are
    // carved into clauses, one of them has to be it -- otherwise the loop
    // iterates over nothing, which is what a prolog^ followed by unheaded
    // statements silently becomes, since those statements join the prolog^.
    //
    // Braces with no clause heading at all are an implicit main^ and need no
    // saying so, empty ones included.
    if (in_loop && saw_clause && !has_body) {
        report(p, at, LHAT_PARSE_ERR_NO_BODY_CLAUSE);
    }

    block->v.list.extra = head;
    return block;
}

static LhatNode *parse_braced_block(Parser *p, bool in_loop, bool walks)
{
    LhatToken brace = p->current;
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return NULL;
    }
    LhatNode *block = parse_clause_body(p, &brace, in_loop, walks);
    expect_op(p, LHAT_OP_RBRACE);
    return block;
}

// A target may carry a type, as an ordinary definition may (Memo.md L36).
// Reassignment targets must not, since the variable already exists.
static LhatNode *parse_target(Parser *p)
{
    LhatToken start = p->current;
    LhatNode *value = parse_expression(p);

    if (!check_op(p, LHAT_OP_COLON)) {
        return value;
    }

    advance(p);
    LhatNode *target = make(p, LHAT_NODE_PARAM, &start);
    if (target == NULL) {
        return value;
    }
    target->v.param.name = value;
    target->v.param.type = parse_type(p);
    return target;
}

// 13.10: 'unpack^ expr' marks the value that is taken apart. Putting the
// marker on the value rather than on the binding is what lets a
// reassignment destructure too.
static LhatNode *parse_value(Parser *p)
{
    if (!check_hat(p, "unpack")) {
        return parse_expression(p);
    }

    LhatToken start = p->current;
    advance(p);

    LhatNode *node = make(p, LHAT_NODE_UNPACK, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.jump.value = parse_expression(p);
    return node;
}

// The value list of a binding, plus the arity checks the two forms share.
static LhatNode *parse_binding(Parser *p, LhatNodeKind kind,
                               const LhatToken *at, LhatNode *targets)
{
    LhatNode *values = NULL;
    LhatNode *values_tail = NULL;
    lhat_node_append(&values, &values_tail, parse_value(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&values, &values_tail, parse_value(p));
    }

    LhatNode *node = make(p, kind, at);
    if (node == NULL) {
        return NULL;
    }
    node->v.binding.targets = targets;
    node->v.binding.values = values;

    size_t target_count = lhat_node_list_length(targets);
    size_t source_count = lhat_node_list_length(values);

    bool unpacking = false;
    for (LhatNode *value = values; value != NULL; value = value->next) {
        if (value->kind == LHAT_NODE_UNPACK) {
            unpacking = true;
        }
    }

    if (unpacking) {
        // 13.10: the marked value has to be the only one, since it is spread
        // across every target.
        if (source_count != 1) {
            report(p, at, LHAT_PARSE_ERR_UNPACK_NOT_ALONE);
        }
    } else if (target_count != source_count) {
        report(p, at,
               source_count == 1 ? LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_UNPACK
                                 : LHAT_PARSE_ERR_BINDING_ARITY);
    }
    return node;
}

// 05 の 3 章. Names the unit, independently of where its file sits, so that
// moving a file does not change the label a type shows (7.1).
static LhatNode *parse_module(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // module^

    LhatNode *node = make(p, LHAT_NODE_MODULE, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.named.name = parse_qualified_name(p);
    return node;
}

// 05 の 4 章. A mark on the declaration rather than a list at the end of the
// file, which is what lets the exports be known from the text alone -- 6 章
// needs that, since the checker follows an import without running anything.
static LhatNode *parse_public(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // public^

    LhatNode *declaration = parse_statement(p);
    if (declaration == NULL) {
        return NULL;
    }

    switch (declaration->kind) {
        case LHAT_NODE_DEFINE:
            declaration->v.binding.exported = true;
            break;
        case LHAT_NODE_ERRORDEF:
            declaration->v.named.exported = true;
            break;
        default:
            // 4 章 puts it on a declaration. Anything else has no name to
            // publish.
            report(p, &start, LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION);
            break;
    }
    return declaration;
}

// 8.6. let^ is what creates a name; without it ':=' reassigns. Making the
// dangerous spelling the longer one is the whole point -- writing ':=' where
// a definition was meant used to shadow silently, which is the accident Go
// is known for.
//
// 8.6 also allows '=' here, and only here, because no expression can follow
// 'let^ name': the same reason 13.4 already writes a default with '='.
// What let^ binds is a name, never an arbitrary expression, so this does not
// go through parse_target: reading the target as an expression would swallow
// the '=' as a comparison before the binding ever saw it.
static LhatNode *parse_let_target(Parser *p)
{
    LhatToken start = p->current;
    // 05 の 8.6: L^ is a place a path may start from, and the only hat
    // identifier that is. The '.' is what tells it from 'let^ true^ = 1',
    // which names nothing; the checker refuses any other spelling.
    bool hatted_root = p->current.kind == LHAT_TOKEN_HAT_IDENT &&
                       p->ahead.kind == LHAT_TOKEN_OP &&
                       p->ahead.v.op == LHAT_OP_DOT;
    if (!hatted_root && p->current.kind != LHAT_TOKEN_IDENT &&
        p->current.kind != LHAT_TOKEN_NAME_LITERAL &&
        p->current.kind != LHAT_TOKEN_SCOPE) {
        return error_node(p, LHAT_PARSE_ERR_EXPECTED_NAME);
    }

    LhatNode *name = parse_primary(p);

    // 8.8: a target may name a member rather than a place of its own. Only
    // the plain '.' form -- '?.' asks whether something is there, which is
    // not a question a name being introduced can answer.
    while (check_op(p, LHAT_OP_DOT)) {
        LhatToken at = p->current;
        advance(p);
        name = access_node(p, LHAT_NODE_MEMBER, &at, name, simple_node(p),
                           false);
    }

    if (!check_op(p, LHAT_OP_COLON)) {
        return name;
    }

    advance(p);
    LhatNode *target = make(p, LHAT_NODE_PARAM, &start);
    if (target == NULL) {
        return name;
    }
    target->v.param.name = name;
    target->v.param.type = parse_type(p);
    return target;
}

// 12.1: with^ always introduces a fresh local of its own, never a path into
// an existing table, so its target skips the '.member' chain 8.8 lets an
// ordinary definition take -- a name, optionally typed, and nothing more.
static LhatNode *parse_with_target(Parser *p)
{
    if (p->current.kind != LHAT_TOKEN_IDENT &&
        p->current.kind != LHAT_TOKEN_NAME_LITERAL) {
        return error_node(p, LHAT_PARSE_ERR_EXPECTED_NAME);
    }

    LhatToken start = p->current;
    LhatNode *name = parse_primary(p);

    if (!check_op(p, LHAT_OP_COLON)) {
        return name;
    }

    advance(p);
    LhatNode *target = make(p, LHAT_NODE_PARAM, &start);
    if (target == NULL) {
        return name;
    }
    target->v.param.name = name;
    target->v.param.type = parse_type(p);
    return target;
}

static LhatNode *parse_let(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // let^

    LhatNode *targets = NULL;
    LhatNode *tail = NULL;
    lhat_node_append(&targets, &tail, parse_let_target(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&targets, &tail, parse_let_target(p));
    }

    LhatToken at = p->current;
    // 8.8改: which of the two was written matters for a path target, so it
    // is recorded rather than just accepted like 8.6 treats it for a name.
    bool via_reassign_op = match_op(p, LHAT_OP_DEFINE);
    if (!via_reassign_op && !match_op(p, LHAT_OP_EQ)) {
        // 8.7: a declaration without a value is not a form. Mutual recursion
        // is handled by the whole scope seeing the name, so nothing needs one.
        report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
        return make(p, LHAT_NODE_ERROR, &start);
    }
    LhatNode *node = parse_binding(p, LHAT_NODE_DEFINE, &at, targets);
    if (node != NULL) {
        node->v.binding.via_reassign_op = via_reassign_op;
    }
    return node;
}

static LhatNode *parse_if_body(Parser *p, LhatToken start, LhatNode *condition)
{
    LhatNode *node = make(p, LHAT_NODE_IF_STMT, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatToken brace = p->current;
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return node;
    }

    // 5.2: the clauses live inside the braces.
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    LhatNode *first = make(p, LHAT_NODE_IF_CLAUSE, &brace);
    if (first == NULL) {
        return node;
    }
    first->v.clause.condition = condition;
    first->v.clause.body = parse_block_body(p, &brace);
    lhat_node_append(&head, &tail, first);

    while (is_else_marker(p)) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *clause = make(p, LHAT_NODE_IF_CLAUSE, &at);
        if (clause == NULL) {
            break;
        }
        if (!check_op(p, LHAT_OP_COLON)) {
            clause->v.clause.condition = parse_expression(p);
        }
        expect_op(p, LHAT_OP_COLON);
        clause->v.clause.body = parse_block_body(p, &at);
        lhat_node_append(&head, &tail, clause);
    }

    expect_op(p, LHAT_OP_RBRACE);
    node->v.list.items = head;
    return node;
}

static LhatNode *parse_if_statement(Parser *p, LhatToken start)
{
    return parse_if_body(p, start, parse_expression(p));
}

// Where a bare expression is read at all: the top of interactive input (8.2,
// where 03 の 4.3 makes it the answer) and the body of a function (15.12,
// where answer_with_body makes it the return^). Not in a block nested inside
// either -- 8.1's reason holds there, and `bare_depth` is the list itself
// rather than anything below it.
static bool may_stand_alone(const Parser *p)
{
    return (p->interactive && p->depth == 1) ||
           (p->bare_depth != 0 && p->depth == p->bare_depth);
}

// 8.2: an expression that has arrived where a statement belongs. A call may
// stand alone anywhere; anything else only where the above says so. Reading
// it as a return^ here instead would stop what follows and would refuse a
// call of a procedure that answers nothing -- 15.12 turns the one that is
// the whole of a function's body into one, once the body has ended.
static LhatNode *expression_as_statement(Parser *p, LhatToken start,
                                         LhatNode *value)
{
    if (value != NULL && value->kind == LHAT_NODE_ERROR) {
        return value;
    }
    if (is_call_statement(value) || may_stand_alone(p)) {
        LhatNode *node = make(p, LHAT_NODE_CALL_STMT, &start);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = value;
        return node;
    }
    report(p, &start, LHAT_PARSE_ERR_BARE_EXPRESSION);
    return make(p, LHAT_NODE_ERROR, &start);
}

// 16.3. The focus is a list of bindings, of destructuring targets, or one
// expression when it is unnamed and reached through it^ (16.2).
// True when the parser stands where a type annotation could begin but the
// ':' really opens the clauses of 17.2.
static bool opens_when_clauses(const Parser *p)
{
    if (!check_op(p, LHAT_OP_COLON) || p->ahead.kind != LHAT_TOKEN_HAT_IDENT) {
        return false;
    }
    return token_is_hat(p, &p->ahead, "when") ||
           token_is_hat(p, &p->ahead, "other") ||
           token_is_hat(p, &p->ahead, "el") ||
           token_is_hat(p, &p->ahead, "else");
}

static LhatNode *parse_for_focus(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    for (;;) {
        LhatToken at = p->current;
        // 8.6改: for^ is no longer an introducer of its own. let^ asks for a
        // fresh, loop-scoped name exactly as it would anywhere else; without
        // it, a bare ':=' reaches an existing name instead (8.6's own rule
        // for a bare ':=', applied here rather than for^ making every focus
        // a definition regardless of what was written).
        bool has_let = match_hat(p, "let");

        LhatNode *target;
        // 8.6: set the moment a bare '=' is found meaning comparison rather
        // than assignment, so the one diagnostic it earns is not followed by
        // whatever nonsense checking the comparison as a focus would add.
        bool equals_error = false;
        if (has_let) {
            // 8.6: let^ always introduces a name, never an arbitrary
            // expression, so the target is read the same restricted way
            // parse_let_target reads one (name, optional '.member' chain,
            // optional ':type'). parse_expression would otherwise read
            // 'i = 0' whole, as a comparison, before '=' had a chance to
            // mean definition here -- ':=' never had this problem, since it
            // is not a token an expression continues through.
            target = parse_let_target(p);
        } else {
            // 17.2's expression form puts a ':' straight after the subject,
            // which is the shape of the type annotation of 16.3. What
            // follows the ':' tells them apart, and one token of lookahead
            // is enough.
            target = parse_expression(p);
            // 8.6: with no introducer, 'i = 0' is read whole, as a
            // comparison, before a bare ':=' ever gets a look-in -- caught
            // the same way a top-level 'x = 1' statement already is (8.2),
            // and refused with the same message rather than accepted as an
            // unnamed focus that happens to be a boolean.
            if (is_binary_op(target, LHAT_OP_EQ)) {
                report(p, &at, LHAT_PARSE_ERR_EQUALS_IS_COMPARISON);
                equals_error = true;
            } else if (check_op(p, LHAT_OP_COLON) && !opens_when_clauses(p)) {
                advance(p);
                LhatNode *annotated = make(p, LHAT_NODE_PARAM, &at);
                if (annotated == NULL) {
                    break;
                }
                annotated->v.param.name = target;
                annotated->v.param.type = parse_type(p);
                target = annotated;
            }
        }

        if (equals_error) {
            target = make(p, LHAT_NODE_ERROR, &at);
        } else if (has_let) {
            // 8.6: let^ accepts either spelling, both meaning define.
            if (!match_op(p, LHAT_OP_DEFINE) && !match_op(p, LHAT_OP_EQ)) {
                report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
            } else {
                LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
                if (binding == NULL) {
                    break;
                }
                binding->v.binding.targets = target;
                binding->v.binding.values = parse_expression(p);
                target = binding;
            }
        } else if (check_op(p, LHAT_OP_DEFINE)) {
            // 8.6: no introducer, so ':=' reassigns an existing name.
            advance(p);
            LhatNode *binding = make(p, LHAT_NODE_REASSIGN, &at);
            if (binding == NULL) {
                break;
            }
            binding->v.binding.targets = target;
            binding->v.binding.values = parse_expression(p);
            target = binding;
        } else if (check_op(p, LHAT_OP_EQ)) {
            // 8.6: '=' with no introducer reads as a comparison -- the same
            // ambiguity for^'s old blanket-introducer rule existed to avoid,
            // now refused with the same message 8.2 already gives a bare
            // 'x = 1' statement rather than silently deciding for the writer.
            // Reached only for an annotated target ('i:number^ = 0'), which
            // parse_expression above stopped short of on its own.
            report(p, &p->current, LHAT_PARSE_ERR_EQUALS_IS_COMPARISON);
            advance(p);
            parse_expression(p);
            target = make(p, LHAT_NODE_ERROR, &at);
        } else if (head == NULL && !check_op(p, LHAT_OP_COMMA)) {
            // 16.2: a focus with no name written is still a focus, and it^ is
            // what names it. Binding it here rather than leaving a bare
            // expression means every form of for^ introduces a name, which is
            // what 16.1 says for^ is for.
            LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
            LhatNode *name = make(p, LHAT_NODE_FOCUS, &at);
            if (binding == NULL || name == NULL) {
                break;
            }
            binding->v.binding.targets = name;
            binding->v.binding.values = target;
            target = binding;
        }

        lhat_node_append(&head, &tail, target);
        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

// 17.9: a pattern becomes the condition of an if-clause, which is what makes
// narrowing and exhaustiveness apply without anything new. The subject is
// referred to by the name for^ gave it, copied so each condition holds its
// own node.
static LhatNode *subject_reference(Parser *p, const LhatNode *focus,
                                   const LhatToken *at)
{
    const LhatNode *name = focus;
    // 8.6改: when^ を分岐させる書き方が for^ の焦点なので、焦点が既存の外側名を
    // 使い回す LHAT_NODE_REASSIGN のときも、DEFINE と同じく targets から名前を
    // 取り出す。ここを素通りさせると、再代入文そのものがコピーされて式の位置に
    // 置かれ、コンパイラが式として扱えず失敗する。
    if (name != NULL && (name->kind == LHAT_NODE_DEFINE ||
                          name->kind == LHAT_NODE_REASSIGN)) {
        name = name->v.binding.targets;
    }
    if (name != NULL && name->kind == LHAT_NODE_PARAM) {
        name = name->v.param.name;
    }
    if (name == NULL) {
        return NULL;
    }

    LhatNode *copy = make(p, name->kind, at);
    if (copy != NULL && name->kind != LHAT_NODE_FOCUS) {
        copy->v = name->v;
        copy->next = NULL;
    }
    return copy;
}

static LhatNode *binary_node(Parser *p, const LhatToken *at, LhatOpKind op,
                             LhatNode *left, LhatNode *right)
{
    LhatNode *node = make(p, LHAT_NODE_BINARY, at);
    if (node == NULL) {
        return left;
    }
    node->v.binary.op = op;
    node->v.binary.left = left;
    node->v.binary.right = right;
    return node;
}

// One pattern of 17.3, lowered against the subject.
static LhatNode *parse_pattern(Parser *p, const LhatNode *focus)
{
    LhatToken at = p->current;

    // 17.4: a type has to say so, since a bare name cannot be told from a
    // value. isa^ already means exactly this question (13.11).
    if (match_hat(p, "isa")) {
        return binary_node(p, &at, LHAT_OP_ISA, subject_reference(p, focus, &at),
                           parse_type(p));
    }

    LhatNode *low = parse_expression(p);
    if (!check_hat(p, "to")) {
        return binary_node(p, &at, LHAT_OP_EQ, subject_reference(p, focus, &at),
                           low);
    }

    // 17.3: both ends are included, as in 16.4.
    advance(p);
    LhatNode *high = parse_expression(p);
    return binary_node(
        p, &at, LHAT_OP_AND,
        binary_node(p, &at, LHAT_OP_GE, subject_reference(p, focus, &at), low),
        binary_node(p, &at, LHAT_OP_LE, subject_reference(p, focus, &at), high));
}

// 17.3: several patterns on one when^ mean any of them, which is an or^.
static LhatNode *parse_patterns(Parser *p, const LhatNode *focus)
{
    LhatToken at = p->current;
    LhatNode *condition = parse_pattern(p, focus);
    while (match_op(p, LHAT_OP_COMMA)) {
        condition = binary_node(p, &at, LHAT_OP_OR, condition,
                                parse_pattern(p, focus));
    }
    return condition;
}

static bool is_when_marker(const Parser *p)
{
    return check_hat(p, "when") || check_hat(p, "other") || is_else_marker(p);
}

// 17.2. The clauses become an if-chain, so 17.9's expansion is the tree
// itself rather than something a later stage has to perform.
//
// 17.6: only the ':' after the subject opens, exactly as only the if^ of an
// if expression does, so the expression form is closed by one ';'.
static LhatNode *parse_when_clauses(Parser *p, const LhatToken *start,
                                    const LhatNode *focus, bool as_expression)
{
    LhatNode *node = make(p, as_expression ? LHAT_NODE_IF_EXPR
                                           : LHAT_NODE_IF_STMT, start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (is_when_marker(p)) {
        LhatToken at = p->current;
        bool defaulting = !check_hat(p, "when");
        advance(p);

        LhatNode *clause = make(p, LHAT_NODE_IF_CLAUSE, &at);
        if (clause == NULL) {
            break;
        }
        if (!defaulting) {
            clause->v.clause.condition = parse_patterns(p, focus);
        }

        // 17.5: the ':' is not optional. Memo.md L195 had left that open.
        expect_op(p, LHAT_OP_COLON);

        if (as_expression) {
            clause->v.clause.body = parse_expression(p);
        } else {
            LhatNode *block = make(p, LHAT_NODE_BLOCK, &at);
            if (block == NULL) {
                break;
            }
            LhatNode *statements = NULL;
            LhatNode *statements_tail = NULL;
            while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE) &&
                   !is_when_marker(p)) {
                uint32_t before = p->current.offset;
                lhat_node_append(&statements, &statements_tail,
                                 parse_statement(p));
                if (p->panicking) {
                    synchronize(p);
                }
                if (p->current.offset == before && !at_eof(p)) {
                    advance(p);
                }
            }
            block->v.list.items = statements;
            clause->v.clause.body = block;
        }

        lhat_node_append(&head, &tail, clause);
        if (defaulting) {
            break;  // 17.5: the default is the last thing there can be
        }
    }

    node->v.list.items = head;
    return node;
}

// next^ takes one or more statements, separated by commas (Memo.md L532).
static LhatNode *parse_advance(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    do {
        lhat_node_append(&head, &tail, parse_statement(p));
    } while (match_op(p, LHAT_OP_COMMA));

    return head;
}

static LhatNode *parse_for(Parser *p)
{
    LhatToken start = p->current;
    advance(p);

    LhatNode *node = make(p, LHAT_NODE_FOR, &start);
    if (node == NULL) {
        return NULL;
    }

    node->v.loop.focus = parse_for_focus(p);

    // 16.3: from^ was replaced by ':=' so that every binding with an initial
    // value is written the same way. The initial value is read and discarded
    // so that the rest of the header still parses.
    if (check_hat(p, "from")) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_FROM);
        advance(p);
        parse_expression(p);
    }

    bool is_loop = true;
    if (match_hat(p, "to")) {
        node->v.loop.kind = LHAT_FOR_TO;
        node->v.loop.bound = parse_expression(p);
        if (match_hat(p, "step")) {
            node->v.loop.step = parse_expression(p);
        }
    } else if (match_hat(p, "downto")) {
        node->v.loop.kind = LHAT_FOR_DOWNTO;
        node->v.loop.bound = parse_expression(p);
        if (match_hat(p, "step")) {
            node->v.loop.step = parse_expression(p);
        }
    } else if (match_hat(p, "in")) {
        node->v.loop.kind = LHAT_FOR_IN;
        node->v.loop.bound = parse_expression(p);
    } else if (check_hat(p, "while") || check_hat(p, "until")) {
        node->v.loop.kind = check_hat(p, "while") ? LHAT_FOR_WHILE : LHAT_FOR_UNTIL;
        advance(p);
        node->v.loop.bound = parse_expression(p);
        if (match_hat(p, "next")) {
            node->v.loop.advance = parse_advance(p);
        }
    } else if (check_hat(p, "if")) {
        // 16.3: this one does not iterate. It scopes the definitions to a
        // condition without another level of nesting.
        LhatToken at = p->current;
        advance(p);
        node->v.loop.kind = LHAT_FOR_IF;
        node->v.loop.bound = parse_expression(p);
        // 5.2改: '{' opens the statement form and ':' the expression one,
        // here as anywhere. The focus still does not leave -- only the value
        // built from it does, which is what an expression is.
        node->v.loop.is_expression = check_op(p, LHAT_OP_COLON);
        node->v.loop.body =
            node->v.loop.is_expression
                ? parse_if_expression_from(p, at, node->v.loop.bound)
                : parse_if_body(p, at, node->v.loop.bound);
        return node;
    } else if (check_op(p, LHAT_OP_LBRACE) || check_op(p, LHAT_OP_COLON)) {
        // 17 章: no driving clause at all, so what follows dispatches on the
        // subject rather than iterating over it.
        node->v.loop.kind = LHAT_FOR_WHEN;
        bool as_expression = check_op(p, LHAT_OP_COLON);
        node->v.loop.is_expression = as_expression;
        LhatToken at = p->current;
        advance(p);

        node->v.loop.body =
            parse_when_clauses(p, &at, node->v.loop.focus, as_expression);

        // A brace with no when^ inside dispatches on nothing and iterates
        // over nothing, so it is the missing clause of 16.3 rather than an
        // empty match.
        if (node->v.loop.body != NULL &&
            node->v.loop.body->v.list.items == NULL) {
            report(p, &at, LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE);
        }

        // 17.6: one ';' closes the expression form, since only the ':' after
        // the subject opened anything.
        expect_op(p, as_expression ? LHAT_OP_SEMICOLON : LHAT_OP_RBRACE);
        return node;
    } else {
        report(p, &p->current, LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE);
        return node;
    }

    // 16.3: next^ is the update of a conditional loop, and only that. 16.4
    // makes to^ and downto^ sugar for a while^ that already carries one, and
    // 16.3 has a walk advanced by the coroutine it is stepping -- so in every
    // other form there is no focus for next^ to move. Consumed either way, so
    // that the body still parses and this is the only diagnostic.
    if (is_loop && node->v.loop.kind != LHAT_FOR_WHILE &&
        node->v.loop.kind != LHAT_FOR_UNTIL && check_hat(p, "next")) {
        report(p, &p->current, LHAT_PARSE_ERR_NEXT_NOT_HERE);
        advance(p);
        parse_advance(p);
    }

    node->v.loop.body =
        parse_braced_block(p, is_loop, node->v.loop.kind == LHAT_FOR_IN);
    return node;
}

// 16.5: repeat^ introduces no focus, and takes no next^.
static LhatNode *parse_repeat(Parser *p)
{
    LhatToken start = p->current;
    advance(p);

    LhatNode *node = make(p, LHAT_NODE_REPEAT, &start);
    if (node == NULL) {
        return NULL;
    }

    if (match_hat(p, "while")) {
        node->v.repeat.kind = LHAT_REPEAT_WHILE;
        node->v.repeat.bound = parse_expression(p);
    } else if (match_hat(p, "until")) {
        node->v.repeat.kind = LHAT_REPEAT_UNTIL;
        node->v.repeat.bound = parse_expression(p);
    } else if (check_op(p, LHAT_OP_LBRACE)) {
        node->v.repeat.kind = LHAT_REPEAT_FOREVER;
    } else {
        node->v.repeat.kind = LHAT_REPEAT_COUNT;
        node->v.repeat.bound = parse_expression(p);
    }

    if (check_hat(p, "next")) {
        report(p, &p->current, LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT);
        advance(p);
        parse_advance(p);
    }

    node->v.repeat.body = parse_braced_block(p, true, false);
    return node;
}

// 04 の 2.2. A field of an error kind, which may carry a type, a default, or
// both. The default is written with ':=' because 14.6's template already
// writes a named field with an initial value that way, and 8.6 keeps '=' to
// the two positions where no expression could stand.
//
// PARAM rather than MEMBER_DECL, since it is the shape that already holds a
// name, a type and a value together.
static LhatNode *parse_error_fields(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        if (p->current.kind != LHAT_TOKEN_IDENT &&
            p->current.kind != LHAT_TOKEN_NAME_LITERAL) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }

        LhatNode *field = make(p, LHAT_NODE_PARAM, &p->current);
        if (field == NULL) {
            break;
        }
        field->v.param.name = simple_node(p);

        if (match_op(p, LHAT_OP_COLON)) {
            field->v.param.type = parse_type(p);
        }
        if (match_op(p, LHAT_OP_DEFINE)) {
            field->v.param.fallback = parse_expression(p);
        }
        if (field->v.param.type == NULL && field->v.param.fallback == NULL) {
            report(p, &p->current, LHAT_PARSE_ERR_FIELD_NEEDS_TYPE);
        }

        lhat_node_append(&head, &tail, field);
        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }
    return head;
}

// 04 の 2.2. A declaration rather than an expression, because 2.4 makes the
// name the identity: a name that is only a label can be taken from a binding
// the way def^ does (14.9), but one the type is made of belongs in the
// declaration. So there is no way to write an anonymous one.
static LhatNode *parse_errordef(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // errordef^

    LhatNode *node = make(p, LHAT_NODE_ERRORDEF, &start);
    if (node == NULL) {
        return NULL;
    }

    if (p->current.kind == LHAT_TOKEN_IDENT ||
        p->current.kind == LHAT_TOKEN_NAME_LITERAL) {
        node->v.named.name = simple_node(p);
    } else {
        report(p, &p->current, LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME);
    }

    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return node;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *kind = make(p, LHAT_NODE_ERROR_KIND, &p->current);
        if (kind == NULL) {
            break;
        }

        if (p->current.kind != LHAT_TOKEN_IDENT &&
            p->current.kind != LHAT_TOKEN_NAME_LITERAL) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }
        kind->v.named.name = simple_node(p);

        // A kind may declare fields, which narrowing then makes visible
        // (04 の 6.1). Unlike the members of t^{ ... } they may carry a
        // default, so they are read here rather than shared with the type.
        if (match_op(p, LHAT_OP_LBRACE)) {
            kind->v.named.members = parse_error_fields(p);
            expect_op(p, LHAT_OP_RBRACE);
        }

        lhat_node_append(&head, &tail, kind);

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }

    node->v.named.members = head;
    expect_op(p, LHAT_OP_RBRACE);
    return node;
}

static LhatNode *parse_with(Parser *p)
{
    LhatToken start = p->current;
    LhatNode *node = make(p, LHAT_NODE_WITH, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    // 12.1: only local definitions may appear, and several with^ clauses may
    // precede one block. with^ is its own introducer (8.6), so a name gets
    // an optional ':type' the same way let^'s target does, and either '='
    // or ':=' means define -- but never a '.member' path, since with^ always
    // introduces a fresh local rather than reaching into an existing table.
    while (check_hat(p, "with")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
        if (binding == NULL) {
            break;
        }
        binding->v.binding.targets = parse_with_target(p);
        bool via_reassign_op = match_op(p, LHAT_OP_DEFINE);
        if (!via_reassign_op && !match_op(p, LHAT_OP_EQ)) {
            report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
        } else {
            binding->v.binding.values = parse_expression(p);
        }
        binding->v.binding.via_reassign_op = via_reassign_op;
        lhat_node_append(&head, &tail, binding);
    }

    node->v.list.items = head;
    node->v.list.extra = parse_braced_block(p, false, false);
    return node;
}

static LhatNode *parse_jump(Parser *p, LhatNodeKind kind)
{
    LhatToken start = p->current;
    advance(p);
    LhatNode *node = make(p, kind, &start);
    if (node == NULL) {
        return NULL;
    }

    // Memo.md L500 writes a multi-level break as 'break^[name]', so the
    // operand is bracketed and never a bare expression.
    if (kind == LHAT_NODE_BREAK) {
        if (match_op(p, LHAT_OP_LBRACKET)) {
            node->v.jump.value = parse_expression(p);
            expect_op(p, LHAT_OP_RBRACKET);
        }
        return node;
    }

    // With no statement terminator, a word that begins a statement must not
    // be mistaken for the operand of the one before it.
    //
    // 01 の 10.9 settles the case a keyword cannot: the operand has to be on
    // the same line. Without that, a bare `yield^` or `return^` swallows
    // whatever statement comes next, since an ordinary name begins an
    // expression just as well as it begins a statement.
    bool operand_follows =
        !p->current.preceded_by_newline &&
        ((starts_expression(&p->current) && !is_statement_keyword(p)) ||
         check_op(p, LHAT_OP_LPAREN) || check_op(p, LHAT_OP_SUB) ||
         check_op(p, LHAT_OP_NOT) || check_op(p, LHAT_OP_LBRACE));

    if (operand_follows) {
        node->v.jump.value = parse_expression(p);
    }
    return node;
}

// 04 の 11.6改: unlike return^/break^/yield^, the value is not optional --
// panic^ answers no value of its own, so a bare panic^ would say nothing.
static LhatNode *parse_panic(Parser *p)
{
    LhatToken start = p->current;
    advance(p);
    LhatNode *node = make(p, LHAT_NODE_PANIC, &start);
    if (node == NULL) {
        return NULL;
    }
    node->v.jump.value = parse_expression(p);
    return node;
}

// 8.2 lets a call stand alone. try^ (04 の 5.1) and catch^ (04 の 4.4) wrap
// one without changing that a call is being made -- they only say what to do
// with a failure -- so `try^ save(x)` and `save(x) catch^ nil^` are
// statements too. `??` is included on the same footing as catch^ (11.7).
//
// 15.8: yieldall^ stands alone as well. Its value may be wanted or not, and
// what it does -- running the inner coroutine to its end -- happens either
// way. A plain call of a yieldable procedure is what does nothing.
static bool is_binary_op(const LhatNode *node, LhatOpKind op)
{
    return node->kind == LHAT_NODE_BINARY && node->v.binary.op == op;
}

static bool is_call_statement(const LhatNode *node)
{
    while (node != NULL) {
        if (node->kind == LHAT_NODE_CALL) {
            return true;
        }
        if (node->kind == LHAT_NODE_YIELD_ALL) {
            return true;
        }
        if (node->kind == LHAT_NODE_TRY) {
            node = node->v.jump.value;
            continue;
        }
        if (node->kind == LHAT_NODE_BINARY &&
            (node->v.binary.op == LHAT_OP_CATCH ||
             node->v.binary.op == LHAT_OP_NIL_ELSE)) {
            node = node->v.binary.left;
            continue;
        }
        return false;
    }
    return false;
}

static LhatNode *parse_statement(Parser *p)
{
    LhatToken start = p->current;

    if (start.kind == LHAT_TOKEN_HAT_IDENT) {
        if (check_hat(p, "do")) {
            advance(p);
            return parse_braced_block(p, false, false);
        }
        if (check_hat(p, "with")) {
            return parse_with(p);
        }
        // 16.1: which form a for^ is shows only after its driving clause, so
        // it is parsed and then asked. One written with ':' answers a value,
        // and 8.2 decides whether a value may stand here.
        if (check_hat(p, "for")) {
            LhatNode *node = parse_for(p);
            if (node != NULL && node->kind == LHAT_NODE_FOR &&
                node->v.loop.is_expression) {
                return expression_as_statement(p, start, node);
            }
            return node;
        }
        if (check_hat(p, "repeat")) {
            return parse_repeat(p);
        }
        if (check_hat(p, "return")) {
            return parse_jump(p, LHAT_NODE_RETURN);
        }
        if (check_hat(p, "break")) {
            return parse_jump(p, LHAT_NODE_BREAK);
        }
        if (check_hat(p, "panic")) {
            return parse_panic(p);
        }
        // 05 の 5.4改: a require^ standing alone binds the unit under the
        // path it declared, rather than under a name the reader picks. It is
        // a statement of its own so that 8.2 keeps holding -- a bare
        // expression is still not a statement.
        if (check_hat(p, "require") || check_hat(p, "import")) {
            bool importing = check_hat(p, "import");  // 05 の 8.7
            LhatToken at = p->current;
            LhatNode *inner = parse_unary(p);
            LhatNodeKind wanted =
                importing ? LHAT_NODE_IMPORT : LHAT_NODE_REQUIRE;
            if (inner == NULL || inner->kind != wanted) {
                return inner;
            }
            LhatNode *node = make(p, importing ? LHAT_NODE_IMPORT_STMT
                                               : LHAT_NODE_REQUIRE_STMT, &at);
            if (node == NULL) {
                return NULL;
            }
            node->v.jump.value = inner->v.jump.value;
            return node;
        }
        if (check_hat(p, "yield") || check_hat(p, "_yield")) {
            bool phantom = check_hat(p, "_yield");  // 15.11
            p->saw_yield = true;
            LhatNode *node = parse_jump(p, LHAT_NODE_YIELD);
            if (node != NULL) {
                node->v.jump.phantom = phantom;
            }
            return node;
        }
        // 5.1: '{' opens the statement form and ':' the expression one, and
        // which it is shows only once the condition has been read. So the
        // condition is read here and the token after it decides -- an
        // expression standing at a statement then answers to 8.2 like any
        // other, rather than being refused for wanting a brace.
        if (check_hat(p, "if")) {
            advance(p);
            LhatNode *condition = parse_expression(p);
            if (check_op(p, LHAT_OP_COLON)) {
                return expression_as_statement(
                    p, start, parse_if_expression_from(p, start, condition));
            }
            return parse_if_body(p, start, condition);
        }
        if (check_hat(p, "errordef")) {
            return parse_errordef(p);
        }
        if (check_hat(p, "let")) {
            return parse_let(p);
        }
        if (check_hat(p, "module")) {
            return parse_module(p);
        }
        if (check_hat(p, "public")) {
            return parse_public(p);
        }
    }

    // 13.10: unpack^ belongs to the value of a binding and nowhere else.
    if (check_hat(p, "unpack")) {
        report(p, &p->current, LHAT_PARSE_ERR_UNPACK_MISPLACED);
        advance(p);
        parse_expression(p);
        return make(p, LHAT_NODE_ERROR, &start);
    }

    // Otherwise a reassignment or a call. A definition needs let^ (8.6), so
    // nothing here can create a name.
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    lhat_node_append(&head, &tail, parse_target(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&head, &tail, parse_target(p));
    }

    // 8.6: '<<' was withdrawn once ':=' became the reassignment. It is still
    // lexed so the parser can say what replaced it.
    if (check_op(p, LHAT_OP_REASSIGN)) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_SHIFT);
    }

    if (check_op(p, LHAT_OP_DEFINE) || check_op(p, LHAT_OP_REASSIGN)) {
        LhatToken at = p->current;
        advance(p);
        return parse_binding(p, LHAT_NODE_REASSIGN, &at, head);
    }

    // The withdrawn postfix reassignment of Q2.
    if (check_op(p, LHAT_OP_ARROW)) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_ARROW);
        advance(p);
        parse_expression(p);
        return make(p, LHAT_NODE_ERROR, &start);
    }

    // 2.1: 'foo 1 2 3' is only a call in command mode. What follows here is
    // juxtaposition only if it could not have started a statement of its own,
    // since there is no separator to tell the two apart.
    if (starts_expression(&p->current) && !can_begin_statement(p)) {
        report(p, &p->current, LHAT_PARSE_ERR_JUXTAPOSITION);
        return make(p, LHAT_NODE_ERROR, &start);
    }

    // 8.2: only a call may stand alone as a statement.
    if (head != NULL && head->next == NULL && is_call_statement(head)) {
        LhatNode *node = make(p, LHAT_NODE_CALL_STMT, &start);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = head;
        return node;
    }

    if (head != NULL && head->kind == LHAT_NODE_ERROR) {
        return head;
    }

    // 8.2: at the top level of interactive input, and 15.12 in the body of a
    // function. Being unable to work out 2 + 3 at a prompt is not an option,
    // and a function whose body is one expression answers with it.
    if (head != NULL && head->next == NULL && may_stand_alone(p)) {
        return expression_as_statement(p, start, head);
    }

    // 8.6: 'x = 1' is a comparison, so it lands here as a bare expression.
    // The C habit is common enough to deserve its own message rather than a
    // generic one, since the writer meant one of two different things.
    if (head != NULL && head->next == NULL && is_binary_op(head, LHAT_OP_EQ)) {
        report(p, &start, LHAT_PARSE_ERR_EQUALS_IS_COMPARISON);
        return make(p, LHAT_NODE_ERROR, &start);
    }

    report(p, &start, LHAT_PARSE_ERR_BARE_EXPRESSION);
    return make(p, LHAT_NODE_ERROR, &start);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

static void parser_begin(Parser *p, LhatLexer *lexer, LhatParseResult *result)
{
    memset(result, 0, sizeof *result);
    lhat_arena_init(&result->arena);

    p->lexer = lexer;
    p->result = result;
    p->panicking = false;
    p->saw_yield = false;
    p->interactive = false;
    p->depth = 0;
    p->current = lhat_lexer_next(lexer);
    p->ahead = lhat_lexer_next(lexer);
}

static void parser_finish(Parser *p, LhatLexer *lexer, LhatParseResult *result)
{
    if (!at_eof(p)) {
        report(p, &p->current, LHAT_PARSE_ERR_UNEXPECTED);
    }

    // A lexical error means the token stream was already wrong, and two of
    // them mean the input simply stopped early (02 の 3.1).
    for (size_t i = 0; i < lexer->diagnostic_count; i++) {
        LhatErrorCode code = lexer->diagnostics[i].code;
        if (code == LHAT_ERR_UNTERMINATED_STRING ||
            code == LHAT_ERR_UNTERMINATED_BLOCK_COMMENT) {
            result->incomplete = true;
        }
    }
}

static void parse_unit(LhatLexer *lexer, LhatParseResult *result,
                       bool interactive)
{
    Parser parser;
    parser_begin(&parser, lexer, result);
    parser.interactive = interactive;

    LhatToken origin = parser.current;
    result->root = parse_block_body(&parser, &origin);

    // 05 の 3 章: at most one, and at the top. Checked here rather than while
    // parsing, because "the first statement of the unit" is a fact about the
    // finished list rather than about the parser's position.
    if (result->root != NULL) {
        bool first = true;
        for (LhatNode *s = result->root->v.list.items; s != NULL; s = s->next) {
            if (s->kind == LHAT_NODE_MODULE && !first) {
                LhatToken at;
                memset(&at, 0, sizeof at);
                at.kind = LHAT_TOKEN_HAT_IDENT;
                at.offset = s->offset;
                at.line = s->line;
                at.column = s->column;
                parser.panicking = false;
                report(&parser, &at, LHAT_PARSE_ERR_MODULE_MISPLACED);
            }
            first = false;
        }
    }

    parser_finish(&parser, lexer, result);
}

void lhat_parse(LhatLexer *lexer, LhatParseResult *result)
{
    parse_unit(lexer, result, false);
}

void lhat_parse_interactive(LhatLexer *lexer, LhatParseResult *result)
{
    parse_unit(lexer, result, true);
}

// 14.10 asks that a signature print in a form that parses back as a type
// annotation, and 05 の 8.7 has a host write one as text. Both need the type
// grammar of 13 章 reachable on its own, which is all this is.
void lhat_parse_type_only(LhatLexer *lexer, LhatParseResult *result)
{
    Parser parser;
    parser_begin(&parser, lexer, result);
    result->root = parse_type(&parser);

    // Anything after the type is not part of it, and quietly ignoring it
    // would let a typo through as a shorter type than was meant.
    if (parser.current.kind != LHAT_TOKEN_EOF) {
        report(&parser, &parser.current, LHAT_PARSE_ERR_EXPECTED_TOKEN);
    }
}

// 2.3: the command form applies when the input opens with a name and the
// token after it could not carry an expression on. That second half is what
// keeps arithmetic working at a prompt -- 'x - 1' is a subtraction, not a
// call of x.
bool lhat_parse_is_command(const LhatLexer *lexer)
{
    LhatLexer probe = *lexer;
    LhatToken first = lhat_lexer_next(&probe);
    if (first.kind != LHAT_TOKEN_IDENT) {
        return false;
    }

    Parser p;
    p.lexer = &probe;
    LhatToken second = lhat_lexer_next(&probe);
    return !continues_expression(&p, &second);
}

void lhat_parse_command(LhatLexer *lexer, LhatParseResult *result)
{
    // 3.2: the command form is tried first and the normal form is what the
    // input falls back to, so a host can hand every line to one entry point.
    if (!lhat_parse_is_command(lexer)) {
        lhat_parse(lexer, result);
        return;
    }

    Parser parser;
    parser_begin(&parser, lexer, result);

    LhatToken origin = parser.current;
    LhatNode *callee = simple_node(&parser);

    // 2.4: a call parenthesis binds tighter than juxtaposition, so 'foo(1)'
    // reaches the same tree in both forms rather than becoming foo applied to
    // a parenthesised list.
    LhatNode *arguments = NULL;
    LhatNode *tail = NULL;
    while (!at_eof(&parser)) {
        uint32_t before = parser.current.offset;
        lhat_node_append(&arguments, &tail, parse_expression(&parser));
        if (parser.current.offset == before) {
            break;
        }
    }

    LhatNode *call = access_node(&parser, LHAT_NODE_CALL, &origin, callee,
                                 arguments, false);
    LhatNode *statement = make(&parser, LHAT_NODE_CALL_STMT, &origin);
    LhatNode *block = make(&parser, LHAT_NODE_BLOCK, &origin);
    if (statement != NULL && block != NULL) {
        statement->v.jump.value = call;
        block->v.list.items = statement;
    }
    result->root = block;

    parser_finish(&parser, lexer, result);
}

void lhat_parse_result_dispose(LhatParseResult *result)
{
    lhat_arena_dispose(&result->arena);
    lhat_free(result->diagnostics);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->diagnostic_capacity = 0;
    result->root = NULL;
}

const char *lhat_parse_error_message(LhatParseErrorCode code)
{
    switch (code) {
        case LHAT_PARSE_ERR_NONE:
            return "no error";
        case LHAT_PARSE_ERR_UNEXPECTED:
            return "unexpected token";
        case LHAT_PARSE_ERR_EXPECTED_EXPRESSION:
            return "expected an expression";
        case LHAT_PARSE_ERR_EXPECTED_TYPE:
            return "expected a type";
        case LHAT_PARSE_ERR_EXPECTED_NAME:
            return "expected a name";
        case LHAT_PARSE_ERR_EXPECTED_TOKEN:
            return "expected a different token here";
        case LHAT_PARSE_ERR_BARE_EXPRESSION:
            return "an expression on its own is not a statement";
        case LHAT_PARSE_ERR_JUXTAPOSITION:
            return "arguments without parentheses are only accepted in command "
                   "mode; did you mean foo(1, 2, 3)?";
        case LHAT_PARSE_ERR_WITHDRAWN_ARROW:
            return "postfix reassignment was withdrawn; write 'target << value'";
        case LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON:
            return "'::' was withdrawn; write '->' before the return type";
        case LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_UNPACK:
            return "taking one value apart needs 'unpack^' before it";
        case LHAT_PARSE_ERR_BINDING_ARITY:
            return "the number of targets and values does not match";
        case LHAT_PARSE_ERR_UNPACK_NOT_ALONE:
            return "'unpack^' must be the only value of the binding";
        case LHAT_PARSE_ERR_UNPACK_MISPLACED:
            return "'unpack^' is only valid as the value of ':=' or '<<'";
        case LHAT_PARSE_ERR_CLAUSE_ORDER:
            return "loop clauses run prolog^, pre^, first^, main^, last^, "
                   "epilog^, finally^ and must be written in that order";
        case LHAT_PARSE_ERR_MAIN_REQUIRED:
            return "statements before prolog^, pre^ or first^ need 'main^:' "
                   "to say they are the body";
        case LHAT_PARSE_ERR_NO_BODY_CLAUSE:
            return "a loop needs a body: one of first^, pre^, main^ or last^, "
                   "or statements with no clause heading at all";
        case LHAT_PARSE_ERR_PRE_IN_WALK:
            return "pre^ runs before the walk answers, so there is nothing "
                   "bound for it to read; use main^";
        case LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP:
            return "only finally^ may appear outside a loop";
        case LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE:
            return "for^ needs one of to^, downto^, in^, while^, until^ or if^";
        case LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT:
            return "next^ belongs to for^; repeat^ declares no focus to advance";
        case LHAT_PARSE_ERR_NEXT_NOT_HERE:
            return "next^ updates the focus of a while^ or until^ loop; this "
                   "form advances its own";
        case LHAT_PARSE_ERR_WITHDRAWN_FROM:
            return "from^ was withdrawn; write 'for^ i := 1 to^ 10'";
        case LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE:
            return "op^ defines '..' and the arithmetic operators; and^, or^, "
                   "'!' and the comparisons are the language's own";
        case LHAT_PARSE_ERR_EXPECTED_MEMBER:
            return "a def^ holds 'name := value' members and one self^{ ... }";
        case LHAT_PARSE_ERR_FIELD_NEEDS_NAME:
            return "every field of self^{ ... } needs a name and a value";
        case LHAT_PARSE_ERR_DUPLICATE_TEMPLATE:
            return "a def^ declares its fields once; write one self^{ ... }";
        case LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE:
            return "override^ and overload^ mark a member, not the fields";
        case LHAT_PARSE_ERR_MODULE_MISPLACED:
            return "module^ goes first, and only once in a file";
        case LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION:
            return "public^ marks a let^ or an errordef^";
        case LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL:
            return "require^ takes a written path, since the checker follows it";
        case LHAT_PARSE_ERR_ELSE_NEEDS_COLON:
            return "this needs a ':' after it; what follows was read as the "
                   "condition of a further test, and no ':' came";
        case LHAT_PARSE_ERR_SPREAD_NOT_LAST:
            return "'...' forwards the whole collected tail, so nothing can "
                   "follow it here";
        case LHAT_PARSE_ERR_FIELD_NEEDS_TYPE:
            return "a field needs a type, a default, or both";
        case LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME:
            return "errordef^ needs a name; an error kind has no anonymous form";
        case LHAT_PARSE_ERR_ERROR_NEEDS_KIND:
            return "write the kind, as in error^IOError.NotFound{ ... }";
        case LHAT_PARSE_ERR_WITHDRAWN_SHIFT:
            return "'<<' was withdrawn; reassignment is written 'x := 1'";
        case LHAT_PARSE_ERR_LET_NEEDS_VALUE:
            return "let^ needs a value; write 'let^ x = 0'";
        case LHAT_PARSE_ERR_EQUALS_IS_COMPARISON:
            return "'=' compares; write 'x := 1' to reassign or 'let^ x = 1' "
                   "to make a new name";
        case LHAT_PARSE_ERR_LEXICAL:
            return "the input could not be tokenised";
    }
    return "unknown error";
}

// What the token that was there is called, for a message to name it by. An
// operator is worth quoting; anything else is a kind, since its spelling is
// already under the mark.
static const char *found_spelling(const LhatParseDiagnostic *d)
{
    switch (d->found) {
        case LHAT_TOKEN_EOF:          return "the end of the input";
        case LHAT_TOKEN_IDENT:        return "a name";
        case LHAT_TOKEN_HAT_IDENT:    return "a word of the language";
        case LHAT_TOKEN_NAME_LITERAL: return "a quoted name";
        case LHAT_TOKEN_INT:
        case LHAT_TOKEN_FLOAT:        return "a number";
        case LHAT_TOKEN_STRING:       return "a string";
        case LHAT_TOKEN_SCOPE:        return "a scope specifier";
        case LHAT_TOKEN_OP:           break;
        default:                      return "something else";
    }
    // Quoted, since an operator is short enough to read inside a sentence.
    static char quoted[16];
    snprintf(quoted, sizeof quoted, "'%s'", lhat_op_name(d->found_op));
    return quoted;
}

// The codes that are about the token they met, rather than about something
// larger the token happened to be inside.
static bool names_a_token(LhatParseErrorCode code)
{
    return code == LHAT_PARSE_ERR_UNEXPECTED ||
           code == LHAT_PARSE_ERR_EXPECTED_EXPRESSION ||
           code == LHAT_PARSE_ERR_EXPECTED_TYPE ||
           code == LHAT_PARSE_ERR_EXPECTED_NAME;
}

size_t lhat_parse_message_write(const LhatParseDiagnostic *diagnostic,
                                char *out, size_t capacity)
{
    const char *plain = diagnostic != NULL
                            ? lhat_parse_error_message(diagnostic->code)
                            : "unknown error";

    // Only where the diagnostic knows something its code does not.
    int written;
    if (diagnostic != NULL && diagnostic->has_expected) {
        written = snprintf(out, out != NULL ? capacity : 0,
                           "a '%s' was expected here, and this is %s",
                           lhat_op_name(diagnostic->expected),
                           found_spelling(diagnostic));
    } else if (diagnostic != NULL && names_a_token(diagnostic->code)) {
        written = snprintf(out, out != NULL ? capacity : 0, "%s, and this is %s",
                           plain, found_spelling(diagnostic));
    } else {
        written = snprintf(out, out != NULL ? capacity : 0, "%s", plain);
    }

    if (written < 0) {
        if (out != NULL && capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return (size_t)written;
}
