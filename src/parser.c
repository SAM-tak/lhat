// L^ (lhat) -- parser.

#include "parser.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    LhatLexer *lexer;
    LhatToken current;
    LhatToken ahead;
    LhatParseResult *result;
    bool panicking;   // suppress the cascade after a reported error
    bool saw_yield;   // 15.2: set while parsing a body that contains yield^
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
            (LhatParseDiagnostic *)realloc(r->diagnostics, grown * sizeof *bigger);
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
    report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_TOKEN);
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

static LhatNode *parse_type_table(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // t^ or table^

    LhatNode *node = make(p, LHAT_NODE_TYPE_TABLE, &start);
    if (node == NULL) {
        return NULL;
    }

    expect_op(p, LHAT_OP_LBRACE);

    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *member = make(p, LHAT_NODE_MEMBER_DECL, &p->current);
        if (member == NULL) {
            break;
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

    node->v.list.items = head;
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
                report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_TOKEN);
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

// 10.7: a bare '{ ... }' is a table literal.
static LhatNode *parse_table(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // {

    LhatNode *node = make(p, LHAT_NODE_TABLE, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *entry = make(p, LHAT_NODE_TABLE_ENTRY, &p->current);
        if (entry == NULL) {
            break;
        }

        bool keyed = (p->current.kind == LHAT_TOKEN_IDENT ||
                      p->current.kind == LHAT_TOKEN_NAME_LITERAL ||
                      p->current.kind == LHAT_TOKEN_HAT_IDENT) &&
                     is_op(&p->ahead, LHAT_OP_DEFINE);

        if (keyed) {
            entry->v.entry.key = simple_node(p);
            advance(p);  // :=
        }
        entry->v.entry.value = parse_expression(p);

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
        node->v.func.body = parse_block_body(p, &brace);
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
        "if", "do", "let", "with", "return", "break", "yield",
        "for", "repeat", "while", "until", "when", "other",
        "prolog", "prologue", "first", "main", "last", "epilog", "epilogue",
        "finally"
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (check_hat(p, words[i])) {
            return true;
        }
    }
    return is_else_marker(p);
}

static LhatNode *parse_if_expression(Parser *p, LhatToken start)
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
    first->v.clause.condition = parse_expression(p);
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
        expect_op(p, LHAT_OP_COLON);
        clause->v.clause.body = parse_expression(p);
        lhat_node_append(&head, &tail, clause);
    }

    // 6 章: a ':' opens the construct and a ';' closes it.
    expect_op(p, LHAT_OP_SEMICOLON);
    node->v.list.items = head;
    return node;
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
        lhat_node_append(&head, &tail, parse_expression(p));
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

static LhatNode *parse_ascription(Parser *p)
{
    LhatNode *left = parse_unary(p);
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

        lhat_node_append(&operands, &operand_tail, parse_binary(p, PREC_CONCAT));
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
            return is_statement_keyword(p);
        case LHAT_TOKEN_OP:
            // 01 の 10.9: a '(' on a fresh line opens a statement.
            return p->current.v.op == LHAT_OP_LPAREN;
        default:
            return false;
    }
}

static LhatNode *parse_block_body(Parser *p, const LhatToken *at)
{
    LhatNode *block = make(p, LHAT_NODE_BLOCK, at);
    if (block == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    // Stops at an else marker as well as at '}', because 5.2 puts the clauses
    // of an if statement inside the braces rather than after them.
    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE) && !is_else_marker(p)) {
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

    block->v.list.items = head;
    return block;
}

static LhatNode *parse_braced_block(Parser *p)
{
    LhatToken brace = p->current;
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return NULL;
    }
    LhatNode *block = parse_block_body(p, &brace);
    expect_op(p, LHAT_OP_RBRACE);
    return block;
}

// 13.10: 'let^ a, b := expr' takes one value apart.
static LhatNode *parse_let(Parser *p)
{
    LhatToken start = p->current;
    advance(p);

    LhatNode *node = make(p, LHAT_NODE_LET, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    for (;;) {
        LhatNode *target = make(p, LHAT_NODE_PARAM, &p->current);
        if (target == NULL) {
            break;
        }
        target->v.param.name = simple_node(p);
        if (target->v.param.name == NULL) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }
        if (match_op(p, LHAT_OP_COLON)) {
            target->v.param.type = parse_type(p);
        }
        lhat_node_append(&head, &tail, target);
        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }

    node->v.binding.targets = head;
    if (expect_op(p, LHAT_OP_DEFINE)) {
        node->v.binding.values = parse_expression(p);
    }
    return node;
}

static LhatNode *parse_if_statement(Parser *p, LhatToken start)
{
    LhatNode *node = make(p, LHAT_NODE_IF_STMT, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatNode *condition = parse_expression(p);

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
    // precede one block.
    while (check_hat(p, "with")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
        if (binding == NULL) {
            break;
        }
        binding->v.binding.targets = simple_node(p);
        if (binding->v.binding.targets == NULL) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }
        if (expect_op(p, LHAT_OP_DEFINE)) {
            binding->v.binding.values = parse_expression(p);
        }
        lhat_node_append(&head, &tail, binding);
    }

    node->v.list.items = head;
    node->v.list.extra = parse_braced_block(p);
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
    bool operand_follows =
        (starts_expression(&p->current) && !is_statement_keyword(p)) ||
        check_op(p, LHAT_OP_LPAREN) || check_op(p, LHAT_OP_SUB) ||
        check_op(p, LHAT_OP_NOT) || check_op(p, LHAT_OP_LBRACE);

    if (operand_follows) {
        node->v.jump.value = parse_expression(p);
    }
    return node;
}

static LhatNode *parse_statement(Parser *p)
{
    LhatToken start = p->current;

    if (start.kind == LHAT_TOKEN_HAT_IDENT) {
        if (check_hat(p, "do")) {
            advance(p);
            return parse_braced_block(p);
        }
        if (check_hat(p, "let")) {
            return parse_let(p);
        }
        if (check_hat(p, "with")) {
            return parse_with(p);
        }
        if (check_hat(p, "return")) {
            return parse_jump(p, LHAT_NODE_RETURN);
        }
        if (check_hat(p, "break")) {
            return parse_jump(p, LHAT_NODE_BREAK);
        }
        if (check_hat(p, "yield")) {
            p->saw_yield = true;
            return parse_jump(p, LHAT_NODE_YIELD);
        }
        if (check_hat(p, "if")) {
            advance(p);
            return parse_if_statement(p, start);
        }
    }

    // Otherwise a definition, a reassignment or a call.
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    lhat_node_append(&head, &tail, parse_expression(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&head, &tail, parse_expression(p));
    }

    if (check_op(p, LHAT_OP_DEFINE) || check_op(p, LHAT_OP_REASSIGN)) {
        bool defining = check_op(p, LHAT_OP_DEFINE);
        LhatToken at = p->current;
        advance(p);

        LhatNode *values = NULL;
        LhatNode *values_tail = NULL;
        lhat_node_append(&values, &values_tail, parse_expression(p));
        while (match_op(p, LHAT_OP_COMMA)) {
            lhat_node_append(&values, &values_tail, parse_expression(p));
        }

        LhatNode *node = make(p, defining ? LHAT_NODE_DEFINE : LHAT_NODE_REASSIGN,
                              &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.binding.targets = head;
        node->v.binding.values = values;

        // 13.10: one value against several targets is destructuring, which
        // has to say let^.
        size_t targets = lhat_node_list_length(head);
        size_t sources = lhat_node_list_length(values);
        if (targets != sources) {
            report(p, &at,
                   sources == 1 ? LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_LET
                                : LHAT_PARSE_ERR_BINDING_ARITY);
        }
        return node;
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
    if (head != NULL && head->next == NULL && head->kind == LHAT_NODE_CALL) {
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
    report(p, &start, LHAT_PARSE_ERR_BARE_EXPRESSION);
    return make(p, LHAT_NODE_ERROR, &start);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void lhat_parse(LhatLexer *lexer, LhatParseResult *result)
{
    memset(result, 0, sizeof *result);
    lhat_arena_init(&result->arena);

    Parser parser;
    parser.lexer = lexer;
    parser.result = result;
    parser.panicking = false;
    parser.saw_yield = false;
    parser.current = lhat_lexer_next(lexer);
    parser.ahead = lhat_lexer_next(lexer);

    LhatToken origin = parser.current;
    result->root = parse_block_body(&parser, &origin);

    if (!at_eof(&parser)) {
        report(&parser, &parser.current, LHAT_PARSE_ERR_UNEXPECTED);
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

void lhat_parse_result_dispose(LhatParseResult *result)
{
    lhat_arena_dispose(&result->arena);
    free(result->diagnostics);
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
        case LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_LET:
            return "taking one value apart needs 'let^'";
        case LHAT_PARSE_ERR_BINDING_ARITY:
            return "the number of targets and values does not match";
        case LHAT_PARSE_ERR_LEXICAL:
            return "the input could not be tokenised";
    }
    return "unknown error";
}
