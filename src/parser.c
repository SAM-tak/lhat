// L^ (lhat) -- parser.

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grow.h"
#include "lhat/port.h"
#include "lhat/config.h"

typedef struct {
    LhatLexer *lexer;
    // The token last consumed. Only `finish` reads it, to learn where the
    // construct being left ended.
    LhatToken previous;
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

    // 04 の 4.5: the depth of the statement list a try^{ } owns, where a
    // catch^ opens an arm rather than standing between two expressions. The
    // two readings are both legal L^ otherwise -- 'f() catch^ 0' is a value
    // and 'f()' followed by 'catch^ E:' is a statement and an arm -- and no
    // amount of lookahead separates them, so the block's own list is where
    // the word is spoken for. Parenthesise a fallback written there.
    // Zero outside any try^{ }.
    size_t catch_depth;

    // How many expressions are open (parse_expression). A bracket, a call's
    // arguments, a table or a hole opens one, and so does a body written
    // inside an expression -- so the level a statement of the try^{ }'s own
    // list runs at is recorded rather than assumed, and 4.5's word belongs
    // to the block only at or above it.
    size_t expr_depth;
    size_t catch_expr_depth;
} Parser;

// Levels of 11.6, weakest first. A larger value binds tighter.
enum {
    PREC_NONE = 0,
    PREC_OR,
    PREC_AND,
    PREC_COMPARE,
    // 11.9: tighter than the comparisons it is read for, so
    // 'a <=> b < 0' asks about the answer rather than chaining. C++ places
    // its own at the same remove from the relational operators.
    PREC_SPACESHIP,
    PREC_CONCAT,
    PREC_ADD,
    PREC_MUL
};

static LhatNode *parse_expression(Parser *p);
static LhatNode *parse_type(Parser *p);
static LhatNode *parse_statement(Parser *p);
// 13.8改: '(a, b)' written as a jump's value becomes the same node the
// comma-separated spelling makes. Used by parse_jump and by 15.12's
// answer_with_body, which is what lets 'f^ { (0, 1) }' work.
static void fold_tuple_answer(LhatNode *jump);
static LhatNode *parse_block_body(Parser *p, const LhatToken *at);
static LhatNode *parse_clause_body(Parser *p, const LhatToken *at, bool in_loop,
                                   bool walks);
static LhatNode *access_node(Parser *p, LhatNodeKind kind, const LhatToken *at,
                             LhatNode *target, LhatNode *argument, bool nil_safe);
static LhatNode *simple_node(Parser *p);
static void fill_name(LhatNode *node, const LhatToken *t, LhatNodeKind kind);
static LhatNode *parse_error_fields(Parser *p);
static LhatNode *parse_module(Parser *p);
static LhatNode *parse_public(Parser *p);
static LhatNode *parse_for(Parser *p);
static LhatNode *parse_try_block(Parser *p);
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
    p->previous = p->current;
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

// Compares a hat identifier's word, whatever its hat count. Only for the
// spellings that stack (01 の 2.3): break^^^ reads its count as how many
// loops to leave, so its keyword check must not insist on one hat.
static bool token_is_hat_stacked(const Parser *p, const LhatToken *token,
                                 const char *word)
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

// Compares a hat identifier against a word. 01 の 2.3: one hat is the
// identifier itself and further hats count levels, so a keyword -- which
// counts nothing -- is only itself with exactly one. 'if^^' is not a
// misspelt if^; it falls through to being read as a name, where the count
// is refused with a message of its own (parse_name_hats).
static bool token_is_hat(const Parser *p, const LhatToken *token, const char *word)
{
    return token_is_hat_stacked(p, token, word) && token->v.hats == 1;
}

// 13.12: '_^' stands where a name would and binds nothing. Every place that
// reads a name has to admit it, since the whole point is to be written where
// a name is expected -- the checker is what refuses it everywhere else.
static bool at_discard(const Parser *p)
{
    return token_is_hat(p, &p->current, "_");
}

static void refuse_extra_hats(Parser *p, const LhatToken *token);
static bool compound_assign_op(LhatOpKind token_op, LhatOpKind *base_op);

// 11.9: the comparisons a type answers through '<=>' rather than one by
// one. '=' is not among them any more -- 11.9改 lets a type write that one
// on its own, since knowing what equals what does not mean knowing what
// comes first. '≠' stays derived, from whichever of the two answered.
// is^ and isa^ are not among them either -- one asks identity and the other
// a type, and neither is anything a value's own order decides.
static bool is_derived_comparison(LhatOpKind op)
{
    return op == LHAT_OP_NE || op == LHAT_OP_LT ||
           op == LHAT_OP_GT || op == LHAT_OP_LE || op == LHAT_OP_GE;
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
    LHAT_GROW(r->diagnostics, r->diagnostic_count, r->diagnostic_capacity, 8,
              return);

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

// Widens `node` to end where the last consumed token does. A parse function
// that reads more than one token calls this on the way out, so `make` gives
// the start and this gives the end (ast.h).
//
// It only ever widens. A node whose last token was consumed by something
// parsed after it -- a binary operator's right operand, say, which the
// operator node adopts -- keeps the wider of the two.
static LhatNode *finish(Parser *p, LhatNode *node)
{
    if (node != NULL) {
        uint32_t end = p->previous.offset + p->previous.length;
        if (end > node->end) {
            node->end = end;
        }
    }
    return node;
}

// Same, for a node whose last token is one another node already accounted
// for -- the parser has usually moved on by then, so `finish` would be wrong.
static LhatNode *finish_at(LhatNode *node, const LhatNode *last)
{
    if (node != NULL && last != NULL && last->end > node->end) {
        node->end = last->end;
    }
    return node;
}

// Moves a node's start back to `at`, for the words that lead a construct
// without belonging to any node under it -- 'do^' before its braces, and the
// 'public^' that marks the declaration it precedes. Without this the span
// begins after the word, and a tool showing the source of the node loses it.
static LhatNode *start_at(LhatNode *node, const LhatToken *at)
{
    if (node != NULL && at->offset < node->offset) {
        node->offset = at->offset;
        node->line = at->line;
        node->column = at->column;
    }
    return node;
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

// 14.14改: what a brace introduces takes either spelling, and they mean the
// same thing. '=' is 8.6's word for a definition and the recommended form;
// ':=' is the older one and still reads. Neither is ambiguous here -- a name
// is what stands to the left of both, so there is no expression for '=' to be
// read as a comparison of, which is the one thing 8.6 had to protect.
//
// This is every brace the language has: a table literal and the self^{ … }
// template (14.14改), a def^ member (14.13), and the fields an errordef^
// declares (04 の 2.2). They agreed on nothing before; they agree here.
static bool expect_introduces(Parser *p)
{
    if (match_op(p, LHAT_OP_EQ) || match_op(p, LHAT_OP_REASSIGN)) {
        return true;
    }
    report_expected(p, &p->current, LHAT_OP_EQ);
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

        lhat_node_append(&head, &tail, finish(p, param));

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
    return finish(p, node);
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

    // 13.9 with 15.3改: the front half is written as the signature one resume
    // follows -- 'f^R -> Y;' or 'p^R -> Y;' -- which is where the kind of the
    // body goes, both kinds being possible (15.3改). Read with parse_type so that 13.1's
    // own grammar applies unchanged, including 'f^ -> Y;' for a coroutine
    // nothing is sent to.
    LhatNode *signature = parse_type(p);
    if (signature != NULL && signature->kind == LHAT_NODE_TYPE_FUNC) {
        node->v.coroutine.is_function = signature->v.func.is_function;
        // 15.2: one resume takes exactly one value, so the parameter list
        // holds at most one. An absent one is what a nil^ receive is written
        // as, the same way 13.2 writes an absent result.
        node->v.coroutine.receive =
            signature->v.func.params != NULL
                ? signature->v.func.params->v.param.type
                : NULL;
        node->v.coroutine.produce = signature->v.func.return_type;
        if (signature->v.func.params != NULL &&
            signature->v.func.params->next != NULL) {
            report(p, &start, LHAT_PARSE_ERR_EXPECTED_TYPE);
        }
    } else if (signature != NULL) {
        report(p, &start, LHAT_PARSE_ERR_EXPECTED_TYPE);
    }

    expect_op(p, LHAT_OP_COMMA);
    node->v.coroutine.result = parse_type(p);
    expect_op(p, LHAT_OP_RBRACE);
    return finish(p, node);
}

// 'name : type, ...' up to the closing brace, and the one self^{ ... } section
// a definition's type carries (14.7改).
static LhatNode *parse_member_decls(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    bool has_template = false;

    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE)) {
        LhatNode *member = make(p, LHAT_NODE_MEMBER_DECL, &p->current);
        if (member == NULL) {
            break;
        }

        // 14.7改: a definition says what its instances carry in a self^{ ... }
        // section, written the way the def^ itself writes its template -- it
        // is what 14.16 puts there, and what makes a definition's signature
        // read back as a type (05 の 8.7). Inside are member declarations,
        // not fields with values, so the section holds this same list; the
        // entry that carries it is told apart by what it holds.
        if (check_hat(p, "self") && is_op(&p->ahead, LHAT_OP_LBRACE)) {
            LhatToken start = p->current;
            advance(p);  // self^
            LhatNode *section = make(p, LHAT_NODE_SELF_TABLE, &start);
            advance(p);  // {
            if (section != NULL) {
                section->v.list.items = parse_member_decls(p);
            }
            expect_op(p, LHAT_OP_RBRACE);
            if (has_template) {
                report(p, &start, LHAT_PARSE_ERR_DUPLICATE_TEMPLATE);
            }
            has_template = true;
            member->v.entry.key = NULL;
            member->v.entry.value = finish(p, section);
            lhat_node_append(&head, &tail, finish(p, member));
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        // 13.7, 14.10: the sequence half may end in a variadic tail, the
        // same '...' a parameter list ends in -- unbounded, one type for
        // every position beyond the fixed ones.
        if (check_op(p, LHAT_OP_ELLIPSIS)) {
            advance(p);
            member->v.entry.variadic = true;
            if (match_op(p, LHAT_OP_COLON)) {
                member->v.entry.value = parse_type(p);
            }
            lhat_node_append(&head, &tail, finish(p, member));
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        // 14.10: a member is written 'name : type'. Anything else in the
        // list is a type on its own, and takes the next position -- 14 章
        // makes a table a sequence as well as a mapping, and the sequence
        // half is described by writing its types in order. One token of
        // lookahead separates them: only a name followed by ':' is a member.
        bool named = (p->current.kind == LHAT_TOKEN_IDENT ||
                      p->current.kind == LHAT_TOKEN_HAT_IDENT) &&
                     is_op(&p->ahead, LHAT_OP_COLON);
        if (!named) {
            member->v.entry.key = NULL;
            member->v.entry.value = parse_type(p);
            lhat_node_append(&head, &tail, finish(p, member));
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        if (p->current.kind == LHAT_TOKEN_IDENT ||
            p->current.kind == LHAT_TOKEN_HAT_IDENT) {
            refuse_extra_hats(p, &p->current);
            LhatNodeKind kind = p->current.kind == LHAT_TOKEN_HAT_IDENT
                                    ? LHAT_NODE_HAT_IDENT
                                    : LHAT_NODE_IDENT;
            LhatNode *name = make(p, kind, &p->current);
            if (name != NULL) {
                fill_name(name, &p->current, kind);
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
        lhat_node_append(&head, &tail, finish(p, member));

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
    return finish(p, node);
}

static LhatNode *parse_type_primary(Parser *p)
{
    if (check_hat(p, "f")) {
        return parse_type_function(p, true);
    }
    if (check_hat(p, "p")) {
        return parse_type_function(p, false);
    }
    // 15.13: the same mark a body is written with, in the type that asks for
    // one. What follows has to be a signature.
    if (check_hat(p, "closed")) {
        advance(p);
        if (!check_hat(p, "f") && !check_hat(p, "p")) {
            report(p, &p->current, LHAT_PARSE_ERR_CLOSED_NEEDS_BODY);
            return NULL;
        }
        LhatNode *marked = parse_type_function(p, check_hat(p, "f"));
        if (marked != NULL) {
            marked->v.func.closed = true;
        }
        return marked;
    }
    if (check_hat(p, "c")) {
        return parse_type_coroutine(p);
    }
    if (check_hat(p, "t") || check_hat(p, "table")) {
        return parse_type_table(p);
    }

    if (check_op(p, LHAT_OP_LPAREN)) {
        LhatToken start = p->current;
        advance(p);
        LhatNode *first = parse_type(p);
        if (!check_op(p, LHAT_OP_COMMA)) {
            // Grouping, exactly as before. 13.8改 leaves this reading
            // untouched, and that is what makes a one-position tuple
            // unwritable: '(T)' was already taken, so there is no '(T,)' to
            // invent and no arbitrary choice to make.
            expect_op(p, LHAT_OP_RPAREN);
            return first;
        }

        // 13.8改: two positions or more, in order. A trailing ',' is not
        // allowed -- parse_type reports on the ')' that follows one.
        LhatNode *node = make(p, LHAT_NODE_TYPE_TUPLE, &start);
        if (node == NULL) {
            return NULL;
        }
        LhatNode *head = NULL;
        LhatNode *tail = NULL;
        lhat_node_append(&head, &tail, first);
        while (match_op(p, LHAT_OP_COMMA)) {
            lhat_node_append(&head, &tail, parse_type(p));
        }
        node->v.list.items = head;
        expect_op(p, LHAT_OP_RPAREN);
        return finish(p, node);
    }

    if (p->current.kind == LHAT_TOKEN_HAT_IDENT ||
        p->current.kind == LHAT_TOKEN_IDENT) {
        // A type name never stacks: number^^ counts nothing (01 の 2.3).
        //
        // 13.13 is the one exception, and the fifth word of 01 の 2.3
        // to count levels: Self^ names the type literal enclosing it, so a
        // second hat counts written t^/def^ literals outwards the way it^^
        // counts loops. The word is not self^ -- capital S, a different name
        // (01 の 2.3: a hat identifier is its word plus one hat, compared
        // byte for byte) -- and unlike self^ it is a type rather than a value.
        if (!token_is_hat_stacked(p, &p->current, "Self")) {
            refuse_extra_hats(p, &p->current);
        }
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
        return finish(p, node);
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
        left = finish(p, node);
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
        left = finish(p, node);
    }
    return left;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

// 01 の 2.3: only these words have levels for a second hat to count --
// it^ this^ self^ class^ reach the enclosing focus, subroutine, receiver or
// definition (compiled as stacked reaches -- vm.c). break^ and
// the '$' specifier read their counts on paths of their own. super^^ is
// refused on purpose (14.12改): which implementation an override wraps is
// the composition's business, and skipping layers by count breaks the
// moment a part is inserted -- naming the part (A.a) is that spelling.
static bool word_stacks(const Parser *p, const LhatToken *token)
{
    return token_is_hat_stacked(p, token, "it") ||
           token_is_hat_stacked(p, token, "this") ||
           token_is_hat_stacked(p, token, "self") ||
           token_is_hat_stacked(p, token, "class");
}

// The refusal for a name position that never stacks: a key, a type name, a
// declaration -- extra hats count nothing there, whatever the word.
static void refuse_extra_hats(Parser *p, const LhatToken *token)
{
    if (token->kind == LHAT_TOKEN_HAT_IDENT && token->v.hats > 1) {
        report(p, token, LHAT_PARSE_ERR_HATS_DONT_STACK);
    }
}

// 01 の 3.1: what a name node carries. The span is the source's, which is
// what the language server colours by; a name written with backticks is not
// spelled by that span (the delimiters are out and a doubled backtick is
// one), so the lexer's decoded spelling travels beside it. A hat identifier
// keeps the hat count in the same union member, and never carries a spelling
// of its own -- there is no backticked form of one.
static void fill_name(LhatNode *node, const LhatToken *t, LhatNodeKind kind)
{
    node->v.name.offset = t->offset;
    node->v.name.length = t->length;
    node->v.name.hats = kind == LHAT_NODE_HAT_IDENT ? t->v.hats : 0;
    node->v.name.text_offset =
        kind == LHAT_NODE_HAT_IDENT ? 0 : t->v.string.offset;
    node->v.name.text_length =
        kind == LHAT_NODE_HAT_IDENT ? 0 : t->v.string.length;
}

// `stackable` says whether this position may carry a stacked word at all:
// only a value reference may (parse_primary), and only the words above.
static LhatNode *simple_node_in(Parser *p, bool stackable)
{
    LhatToken t = p->current;
    LhatNodeKind kind;

    switch (t.kind) {
        case LHAT_TOKEN_INT:          kind = LHAT_NODE_INT; break;
        case LHAT_TOKEN_FLOAT:        kind = LHAT_NODE_FLOAT; break;
        case LHAT_TOKEN_STRING:       kind = LHAT_NODE_STRING; break;
        case LHAT_TOKEN_IDENT:        kind = LHAT_NODE_IDENT; break;
        case LHAT_TOKEN_HAT_IDENT:    kind = LHAT_NODE_HAT_IDENT; break;
        default: return NULL;
    }

    if (t.kind == LHAT_TOKEN_HAT_IDENT && t.v.hats > 1 &&
        !(stackable && word_stacks(p, &t))) {
        report(p, &t, LHAT_PARSE_ERR_HATS_DONT_STACK);
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
            node->v.string.offset = t.v.string.offset;
            node->v.string.length = t.v.string.length;
            node->v.string.kind = t.v.string.kind;
            break;
        default:
            fill_name(node, &t, kind);
            break;
    }

    advance(p);
    return node;
}

// Every name position but parse_primary's value reference, where the four
// stacking words are allowed through (simple_node_in above).
static LhatNode *simple_node(Parser *p)
{
    return simple_node_in(p, false);
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
            lhat_node_append(&head, &tail, finish(p, hole));
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
    return finish(p, node);
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
                p->current.kind != LHAT_TOKEN_HAT_IDENT) {
                report(p, &at_marker, LHAT_PARSE_ERR_FIELD_NEEDS_NAME);
                break;
            }
            entry->v.entry.key = simple_node(p);
            if (expect_op(p, LHAT_OP_COLON)) {
                entry->v.entry.value = parse_type(p);
            }
            lhat_node_append(&head, &tail, finish(p, entry));
            if (!match_op(p, LHAT_OP_COMMA)) {
                break;
            }
            continue;
        }

        // 14.14改: an entry names a member that was not there, and 8.6 spells
        // that '='. ':=' is the older spelling and still reads.
        //
        // What '=' costs here is that 'a = 0' cannot be a comparison
        // standing as a positional entry. Wrapping it says so -- '(' does not
        // begin a name, so the brackets take it out of this test entirely --
        // and wanting a list of comparisons is rarer than wanting members.
        bool keyed = (p->current.kind == LHAT_TOKEN_IDENT ||
                      p->current.kind == LHAT_TOKEN_HAT_IDENT) &&
                     (is_op(&p->ahead, LHAT_OP_REASSIGN) ||
                      is_op(&p->ahead, LHAT_OP_EQ));

        // 14.14改: '[ ... ] =' gives the key as an expression, for the keys
        // 01 の 6 章 leaves unwritable as names. Nothing else can begin with
        // '[' here -- it does not start an expression -- so no lookahead is
        // needed to tell this from a positional entry.
        if (check_op(p, LHAT_OP_LBRACKET)) {
            LhatToken at_bracket = p->current;
            advance(p);
            entry->v.entry.key = parse_expression(p);
            entry->v.entry.computed = true;
            expect_op(p, LHAT_OP_RBRACKET);
            expect_introduces(p);
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

        lhat_node_append(&head, &tail, finish(p, entry));

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
    return finish(p, node);
}

// 14.6 and 14.11: one spelling, two readings decided by where it stands. In
// the body of a def^ it declares the fields an instance gets; inside new it
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
    return finish(p, node);
}

// A dotted path, as in IOError.NotFound. Kept as MEMBER nodes so it reads
// like any other qualified name in the tree.
static LhatNode *parse_qualified_name(Parser *p)
{
    if (p->current.kind != LHAT_TOKEN_IDENT) {
        return error_node(p, LHAT_PARSE_ERR_EXPECTED_NAME);
    }

    LhatNode *node = simple_node(p);
    while (check_op(p, LHAT_OP_DOT)) {
        LhatToken at = p->current;
        advance(p);
        if (p->current.kind != LHAT_TOKEN_IDENT) {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_NAME);
            break;
        }
        node = access_node(p, LHAT_NODE_MEMBER, &at, node, simple_node(p), false);
    }
    return finish(p, node);
}

// 01 の 3.3: 'id^name' answers the spelling of `name` as a string. Nothing is
// looked up -- 3.3 is about how the string is written, not about the name
// being there -- so this is a leaf, and the node carries the name the way
// every other name node does.
//
// A hat identifier is a name too (2.3: the hat is part of it), which is what
// lets a member's key be written this way -- 'id^tostring^' is "tostring^".
// A stacked spelling is not a name and is refused where it is everywhere
// else.
static LhatNode *parse_id(Parser *p)
{
    LhatToken at = p->current;
    advance(p);  // id^

    LhatNode *node = make(p, LHAT_NODE_NAME, &at);
    if (node == NULL) {
        return NULL;
    }
    if (p->current.kind != LHAT_TOKEN_IDENT &&
        p->current.kind != LHAT_TOKEN_HAT_IDENT) {
        report(p, &p->current, LHAT_PARSE_ERR_ID_NEEDS_NAME);
        return finish(p, node);
    }
    refuse_extra_hats(p, &p->current);
    fill_name(node, &p->current,
              p->current.kind == LHAT_TOKEN_HAT_IDENT ? LHAT_NODE_HAT_IDENT
                                                      : LHAT_NODE_IDENT);
    advance(p);
    return finish(p, node);
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
    return finish(p, node);
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
            // built in, and 11.5's comparisons are not written one by one --
            // 11.9 has '<=>' answer for the orderings at once.
            //
            // 11.9改: '=' is the exception. A type may know what equals what
            // with no order to put its values in, and writing a '<=>' for it
            // would be answering a question it has no answer to.
            bool definable = symbol.kind == LHAT_TOKEN_OP &&
                             (symbol.v.op == LHAT_OP_SPACESHIP ||
                              symbol.v.op == LHAT_OP_EQ ||
                              symbol.v.op == LHAT_OP_CONCAT ||
                              symbol.v.op == LHAT_OP_ADD ||
                              symbol.v.op == LHAT_OP_SUB ||
                              symbol.v.op == LHAT_OP_MUL ||
                              symbol.v.op == LHAT_OP_DIV ||
                              symbol.v.op == LHAT_OP_FLOORDIV ||
                              symbol.v.op == LHAT_OP_MOD ||
                              symbol.v.op == LHAT_OP_POW);
            if (!definable) {
                // 8.6改: a compound spelling is worth its own answer. It is
                // the one an overload would plausibly be written for, and
                // what it stands for -- 'a := a + b' -- already carries the
                // op^ that decides it, so there is a definition to point at
                // rather than only a rule to state.
                LhatOpKind base;
                LhatParseErrorCode why = LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE;
                if (symbol.kind == LHAT_TOKEN_OP &&
                    compound_assign_op(symbol.v.op, &base)) {
                    why = LHAT_PARSE_ERR_COMPOUND_NOT_DEFINABLE;
                } else if (symbol.kind == LHAT_TOKEN_OP &&
                           is_derived_comparison(symbol.v.op)) {
                    // 11.9: the same shape of answer as the compound
                    // spellings -- there is a definition to point at, and it
                    // is the one that decides all six at once.
                    why = LHAT_PARSE_ERR_COMPARISON_NOT_DEFINABLE;
                }
                report(p, &symbol, why);
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
            if (expect_introduces(p)) {
                entry->v.entry.value = parse_expression(p);
            }
        } else if (p->current.kind == LHAT_TOKEN_IDENT ||
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
            } else if (expect_introduces(p)) {
                entry->v.entry.value = parse_expression(p);
            }
        } else {
            report(p, &p->current, LHAT_PARSE_ERR_EXPECTED_MEMBER);
            break;
        }

        lhat_node_append(&head, &tail, finish(p, entry));

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }

    node->v.list.items = head;
    expect_op(p, LHAT_OP_RBRACE);
    return finish(p, node);
}

// A parameter in a definition may be named, typed and given a default.
//
// 13.4: the default is kept on the node and read by nothing downstream. It is
// what completion and the visual editor put into a call site while the call is
// being written, not a value the callee supplies for a missing argument, so
// neither the checker nor the compiler has anything to do with it.
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

        lhat_node_append(&head, &tail, finish(p, param));

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
            // 13.8改: 'f^ { (0, 1) }' answers a tuple, and folding it here is
            // what makes that the very node 'return^ 0, 1' produces.
            fold_tuple_answer(only);
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
        // As in parse_braced_block: the brace closes the body, not the
        // statement list parse_clause_body stopped at.
        finish(p, node->v.func.body);
    }

    node->v.func.yields = p->saw_yield;
    p->saw_yield = enclosing;
    return finish(p, node);
}

// 5.1 and 5.3: one marker, 'el^', covers both else-if and else, and a
// condition before the ':' is what tells them apart. The longer spellings are
// accepted too.
static bool is_else_marker(const Parser *p)
{
    return check_hat(p, "el") || check_hat(p, "else") || check_hat(p, "ei") ||
           check_hat(p, "elseif") || check_hat(p, "elsif") || check_hat(p, "elif");
}

// Whether a type could begin here. 13 章's forms all start with a name, a
// hat identifier (t^, f^, Self^, …) or the '(' of a tuple -- so a literal or
// a brace standing where an arm's kind belongs is 04 の 4.5's other reading
// rather than a type that failed to parse.
static bool starts_type(const Parser *p)
{
    return p->current.kind == LHAT_TOKEN_IDENT ||
           p->current.kind == LHAT_TOKEN_HAT_IDENT ||
           is_op(&p->current, LHAT_OP_LPAREN);
}

// 04 の 4.5: a catch^ standing in the statement list a try^{ } owns, which is
// where the word opens an arm. Anywhere else it is 4.1's binary operator.
static bool at_catch_arm(const Parser *p)
{
    return p->catch_depth != 0 && p->depth == p->catch_depth &&
           p->expr_depth <= p->catch_expr_depth && check_hat(p, "catch");
}

// Words that begin a statement. The lexer keeps no keyword table (01 の 2.1),
// so this knowledge lives here. It is needed wherever a construct may be
// followed by an optional expression: without it, 'break^' would swallow the
// 'yield^ 1' that follows it as though it were its operand.
static bool is_statement_keyword(const Parser *p)
{
    static const char *const words[] = {
        "if", "do", "var", "let", "with", "return", "break", "panic", "yield",
        "_yield", "yieldall", "await",  // 15.8 and 15.14: both suspend
        // 9.11: and the three spellings of the one that goes on with the loop
        "next", "skip", "continue",
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
    lhat_node_append(&head, &tail, finish(p, first));

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
            lhat_node_append(&head, &tail, finish(p, clause));
            break;
        }
        expect_op(p, LHAT_OP_COLON);
        clause->v.clause.body = parse_expression(p);
        lhat_node_append(&head, &tail, finish(p, clause));
    }

    // 5.1: an expression answers with a value in every case, and the clause
    // written without a condition is the one that answers when no test held.
    // The statement form may leave that case out; this one may not -- every
    // clause writes into the same register, so a chain that ends on a test
    // reads back whatever stood there.
    //
    // Reported where the missing clause goes, which is right before the ';'.
    // Input that ran out stands here as EOF, and report() takes that for 3.1's
    // unfinished input rather than a mistake.
    if (tail == NULL || tail->v.clause.condition != NULL) {
        report(p, &p->current, LHAT_PARSE_ERR_IF_EXPR_NEEDS_ELSE);
    }

    // 6 章: a ':' opens the construct and a ';' closes it.
    expect_op(p, LHAT_OP_SEMICOLON);
    node->v.list.items = head;
    return finish(p, node);
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
            return finish(p, node);
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
            // 15.13: the mark stands before the kind, and marks a body --
            // so what follows has to be one.
            if (check_hat(p, "closed")) {
                advance(p);
                if (!check_hat(p, "f") && !check_hat(p, "p")) {
                    report(p, &p->current, LHAT_PARSE_ERR_CLOSED_NEEDS_BODY);
                    return NULL;
                }
                LhatNode *marked = parse_function(p, check_hat(p, "f"));
                if (marked != NULL) {
                    marked->v.func.closed = true;
                }
                return marked;
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
                return finish(p, node);
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
                 is_op(&p->ahead, LHAT_OP_LBRACE))) {
                return parse_error_new(p);
            }
            // 01 の 3.3: 'id^name' is the name's spelling as a string. The
            // name is not looked up -- what this asks for is the way it is
            // written, which a string literal would say just as well and
            // would not be read as a name by anyone looking at it.
            if (check_hat(p, "id")) {
                return parse_id(p);
            }
            // The one position a stacked word may be written in: a value
            // reference (01 の 2.3), where it^^ means the enclosing focus.
            return simple_node_in(p, true);

        case LHAT_TOKEN_OP:
            if (t.v.op == LHAT_OP_LPAREN) {
                advance(p);
                LhatNode *inner = parse_expression(p);
                if (!check_op(p, LHAT_OP_COMMA)) {
                    // Grouping, as it always was. 13.8改 leaves this reading
                    // untouched, which is what makes a one-position tuple
                    // unwritable on the value side too: '(x)' was taken.
                    expect_op(p, LHAT_OP_RPAREN);
                    return inner;
                }

                // 13.8改: two positions or more -- the same shape and the
                // same one-token lookahead parse_type_primary uses, so the
                // value side and the type side read '(' alike.
                LhatNode *node = make(p, LHAT_NODE_TUPLE, &t);
                if (node == NULL) {
                    return NULL;
                }
                LhatNode *head = NULL;
                LhatNode *tail = NULL;
                lhat_node_append(&head, &tail, inner);
                while (match_op(p, LHAT_OP_COMMA)) {
                    lhat_node_append(&head, &tail, parse_expression(p));
                }
                node->v.list.items = head;
                expect_op(p, LHAT_OP_RPAREN);
                return finish(p, node);
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
            // The reserved shift spellings, met where a value should begin
            // ('x = a >> 2' ends the expression at 'a' and lands here).
            if (t.v.op == LHAT_OP_LSHIFT || t.v.op == LHAT_OP_RSHIFT) {
                return error_node(p, LHAT_PARSE_ERR_RESERVED_SHIFT);
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
    // Every caller has already read the whole of the access -- the member
    // name, the ']' or the ')' -- by the time it gets here.
    return finish(p, node);
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
        // 13.7: the same forwarding written after a value rather than as the
        // bare word -- 'expr...', which the section is named for. The reading
        // is the one the bare form has and for the same reason: a variadic
        // slot never takes one value as a whole, so there is nothing else
        // '...' could mean here.
        //
        // 13.8改: this is what lets a tuple reach a variadic tail. Spreading
        // is written rather than inferred from the callee, which is what
        // keeps 13.7's expansion rule from arising.
        if (check_op(p, LHAT_OP_ELLIPSIS)) {
            LhatToken at = p->current;
            advance(p);
            LhatNode *spread = make(p, LHAT_NODE_SPREAD, &at);
            if (spread != NULL) {
                spread->v.jump.value = argument;
                argument = finish(p, spread);
            }
            lhat_node_append(&head, &tail, argument);
            // 13.7: nothing can follow the whole tail it forwards.
            if (check_op(p, LHAT_OP_COMMA)) {
                report(p, &p->current, LHAT_PARSE_ERR_SPREAD_NOT_LAST);
            }
            break;
        }
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

        // 11.7改2: postfix '?' asks whether the value is not absent --
        // '!(x isa^ nil^)' written short. Here with the other postfix forms
        // rather than in parse_unary, so it binds as tightly as they do
        // (11.6's level 11): 'a.b?' is '(a.b)?'.
        if (check_op(p, LHAT_OP_PRESENT)) {
            advance(p);
            LhatNode *asked = make(p, LHAT_NODE_UNARY, &at);
            if (asked == NULL) {
                break;
            }
            asked->v.unary.op = LHAT_OP_PRESENT;
            asked->v.unary.operand = node;
            node = finish(p, asked);
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
        return finish(p, node);
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
        return finish(p, node);
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
        return finish(p, node);
    }

    // 02 の 15.8: delegation. A word of its own rather than a reading of
    // yield^, since the two have different types -- yield^ answers what the
    // resume sent, this answers the inner coroutine's return value.
    //
    // 15.14: await^ is the same delegation under the word a reader of async
    // code knows. The node is the same one with a flag, the way _yield^ is a
    // yield^ with one -- the type rule is 15.8's and saying it twice is how
    // the two would drift apart.
    if (check_hat(p, "yieldall") || check_hat(p, "await")) {
        LhatToken at = p->current;
        bool awaiting = check_hat(p, "await");
        p->saw_yield = true;  // 15.2: delegating is suspending
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_YIELD_ALL, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.awaiting = awaiting;
        node->v.jump.value = parse_unary(p);
        return finish(p, node);
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
        return finish(p, node);
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
        return finish(p, node);
    }

    // 13.8改: 'pack^ expr' is the one bridge from a tuple to a table. At the
    // unary level, like try^ above -- 'pack^ f().x' would be reading a member
    // of a tuple, which is not a thing, so binding tighter buys nothing.
    if (check_hat(p, "pack")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *node = make(p, LHAT_NODE_PACK, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.jump.value = parse_unary(p);
        return finish(p, node);
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
        return finish(p, node);
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
        // 04 の 4.5: not the one that opens an arm -- there the word belongs
        // to the block, and an expression written just before it has ended.
        bool catching = check_hat(p, "catch") && !at_catch_arm(p);
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
        left = finish(p, node);
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
        left = finish(p, node);
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
        // 11.9: an ordinary binary operator, not one of the chaining
        // comparisons -- its answer is a number^, which nothing chains on.
        case LHAT_OP_SPACESHIP:
            *op = LHAT_OP_SPACESHIP;
            *precedence = PREC_SPACESHIP;
            return true;
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
    // 11.6改: 'is^' asks identity, 'isa^' asks the type fit.
    // Checking 'isa' first would not matter
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
        left = finish(p, node);
    }
    return left;
}

// 11.5 の (5): comparisons chain, and an operand shared by two links is
// evaluated once, which is why the chain is kept as one node.
static LhatNode *parse_comparison(Parser *p)
{
    // 11.9: '<=>' binds tighter than the chain, so each link may be one.
    LhatNode *first = parse_binary(p, PREC_SPACESHIP);

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
                                           : parse_binary(p, PREC_SPACESHIP));
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
        return finish(p, node);
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
    return finish(p, node);
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
        left = finish(p, node);
    }
    return left;
}

static LhatNode *parse_expression(Parser *p)
{
    // 04 の 4.5: how deep in expressions this stands. One is the statement's
    // own -- where a catch^ is the word the block spoke for -- and anything
    // more is inside a bracket, a call's arguments, a table or a hole, where
    // it is 4.1's operator again. That is what makes '(f() catch^ 0)' the
    // way to write the fallback there.
    p->expr_depth++;
    LhatNode *node = parse_logical(p, PREC_OR);
    p->expr_depth--;
    return node;
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
                //, '-' can be either, and 2.3 gives '-' to subtraction.
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
        case LHAT_TOKEN_SCOPE:
            return true;
        case LHAT_TOKEN_HAT_IDENT:
            // Some hat identifiers name a value rather than syntax -- the
            // machine (05 の 8.6), a coroutine's own handle (15.10), the
            // receiver (14.4) and the focus (16.2). A statement can start
            // with any of them, the way one can start with any other name
            // (8.3), so 2.1 must not read them as a further argument to the
            // call on the line above.
            // 04 の 4.5's catch^ ends the statement above it as surely as a
            // '}' would: in the list a try^{ } owns, the word opens an arm.
            return is_statement_keyword(p) || at_catch_arm(p) ||
                   check_hat(p, "L") ||
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
    // because 5.2 and 9.2 both put those inside the braces. 04 の 4.5's
    // catch^ arms sit inside them the same way, in the one list that owns
    // them.
    while (!at_eof(p) && !check_op(p, LHAT_OP_RBRACE) && !is_else_marker(p) &&
           !at_catch_arm(p) && clause_index(p) < 0) {
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
    return finish(p, block);
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
    // 9.3: an unheaded statement list is an implicit main^, so it counts.
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
        lhat_node_append(&head, &tail, finish(p, clause));
    }

    // 9.3: first^, pre^, main^ and last^ are the body. Once the braces are
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
    return finish(p, block);
}

static LhatNode *parse_braced_block(Parser *p, bool in_loop, bool walks)
{
    LhatToken brace = p->current;
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return NULL;
    }
    LhatNode *block = parse_clause_body(p, &brace, in_loop, walks);
    expect_op(p, LHAT_OP_RBRACE);
    // The closing brace belongs to the block, and parse_clause_body ends
    // before reading it.
    return finish(p, block);
}

// A target may carry a type, as an ordinary definition may as an ordinary definition may.
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
    return finish(p, target);
}

// 7.4: 'target op= value' names the plain operator it stands for. Not an
// operator table lookup, since a compound token never reaches parse_binary --
// is_comparison/binary_info never see one, so this is the only place asking.
static bool compound_assign_op(LhatOpKind token_op, LhatOpKind *base_op)
{
    switch (token_op) {
        case LHAT_OP_ADD_ASSIGN:      *base_op = LHAT_OP_ADD;      return true;
        case LHAT_OP_SUB_ASSIGN:      *base_op = LHAT_OP_SUB;      return true;
        case LHAT_OP_MUL_ASSIGN:      *base_op = LHAT_OP_MUL;      return true;
        case LHAT_OP_DIV_ASSIGN:      *base_op = LHAT_OP_DIV;      return true;
        case LHAT_OP_FLOORDIV_ASSIGN: *base_op = LHAT_OP_FLOORDIV; return true;
        case LHAT_OP_MOD_ASSIGN:      *base_op = LHAT_OP_MOD;      return true;
        case LHAT_OP_POW_ASSIGN:      *base_op = LHAT_OP_POW;      return true;
        case LHAT_OP_CONCAT_ASSIGN:   *base_op = LHAT_OP_CONCAT;   return true;
        default: return false;
    }
}

// The value list of a binding, plus the arity checks the two forms share.
static LhatNode *parse_binding(Parser *p, LhatNodeKind kind,
                               const LhatToken *at, LhatNode *targets)
{
    LhatNode *values = NULL;
    LhatNode *values_tail = NULL;
    lhat_node_append(&values, &values_tail, parse_expression(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&values, &values_tail, parse_expression(p));
    }

    LhatNode *node = make(p, kind, at);
    if (node == NULL) {
        return NULL;
    }
    node->v.binding.targets = targets;
    node->v.binding.values = values;

    size_t target_count = lhat_node_list_length(targets);
    size_t source_count = lhat_node_list_length(values);

    if (target_count != source_count && source_count != 1) {
        report(p, at, LHAT_PARSE_ERR_BINDING_ARITY);
    }
    // 13.8改: one value on the right with several names on the left is a
    // tuple being taken apart. Whether it actually is one is a question about
    // the type, which the parser cannot answer -- so it does not ask, and no
    // mark is demanded (13.10). A value that turns out not to be a tuple is
    // the checker's TUPLE_ARITY.
    return finish(p, node);
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
    return finish(p, node);
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
    return start_at(declaration, &start);
}

// 8.6. An introducer is what creates a name; without one ':=' reassigns.
// Making the dangerous spelling the longer one is the whole point: were ':='
// a definition, writing it where a reassignment was meant would shadow
// silently, which is the accident Go is known for.
//
// 8.6 also allows '=' here, and only here, because no expression can follow
// 'var^ name': the same reason 13.4 already writes a default with '='.
// What an introducer binds is a name, never an arbitrary expression, so this
// does not go through parse_target: reading the target as an expression would
// swallow the '=' as a comparison before the binding ever saw it.
//
// 8.9: `allow_path` is false under let^, which binds a name and nothing else.
// 8.8's member introduction stays var^'s, so that a name let^ bound is one
// nothing may reassign -- 'let^ t.a = 1' followed by 't.a := 2' would say
// otherwise, and per-member immutability is 05 の M7's question, not this
// one's.
static LhatNode *parse_let_target(Parser *p, bool allow_path)
{
    LhatToken start = p->current;
    // 05 の 8.6: L^ is a place a path may start from, and the only hat
    // identifier that is. The '.' is what tells it from 'let^ true^ = 1',
    // which names nothing; the checker refuses any other spelling.
    bool hatted_root = p->current.kind == LHAT_TOKEN_HAT_IDENT &&
                       p->ahead.kind == LHAT_TOKEN_OP &&
                       p->ahead.v.op == LHAT_OP_DOT;
    // 13.12: and '_^' is the other hat identifier a target may be -- a place
    // to put a value with no name to read it back by.
    if (!hatted_root && !at_discard(p) &&
        p->current.kind != LHAT_TOKEN_IDENT &&
        p->current.kind != LHAT_TOKEN_SCOPE) {
        return error_node(p, LHAT_PARSE_ERR_EXPECTED_NAME);
    }

    LhatNode *name = parse_primary(p);

    // 8.8: a target may name a member rather than a place of its own. Only
    // the plain '.' form -- '?.' asks whether something is there, which is
    // not a question a name being introduced can answer.
    while (check_op(p, LHAT_OP_DOT)) {
        LhatToken at = p->current;
        if (!allow_path) {
            report(p, &at, LHAT_PARSE_ERR_LET_NEEDS_NAME);
            allow_path = true;  // read the rest of the path; say this once
        }
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
    return finish(p, target);
}

// 12.1: with^ always introduces a fresh local of its own, never a path into
// an existing table, so its target skips the '.member' chain 8.8 lets an
// ordinary definition take -- a name, optionally typed, and nothing more.
static LhatNode *parse_with_target(Parser *p)
{
    // 13.12: '_^' is written here too -- 12.6's disposal is what the form is
    // for, and that needs no name to reach the value by.
    if (!at_discard(p) && p->current.kind != LHAT_TOKEN_IDENT) {
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
    return finish(p, target);
}

// 8.9: var^ and let^ read alike and differ only in what they leave behind --
// a name a later ':=' may reach, or one it may not. The word is not otherwise
// part of the grammar, which is why one function reads both.
static LhatNode *parse_let(Parser *p, bool immutable)
{
    LhatToken start = p->current;
    advance(p);  // var^ or let^

    LhatNode *targets = NULL;
    LhatNode *tail = NULL;
    lhat_node_append(&targets, &tail, parse_let_target(p, !immutable));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&targets, &tail, parse_let_target(p, !immutable));
    }

    LhatToken at = p->current;
    // 8.8改: which of the two was written matters for a path target, so it
    // is recorded rather than just accepted like 8.6 treats it for a name.
    bool via_reassign_op = match_op(p, LHAT_OP_REASSIGN);
    if (!via_reassign_op && !match_op(p, LHAT_OP_EQ)) {
        // 8.7: a declaration without a value is not a form. Mutual recursion
        // is handled by the whole scope seeing the name, so nothing needs one.
        report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
        return make(p, LHAT_NODE_ERROR, &start);
    }
    // 8.9: 8.6 made '=' and ':=' the same word after an introducer, and 8.8改
    // gave ':=' a second meaning for a path. Neither survives let^: a name it
    // binds is never reassigned, so the spelling that says "reassign" is not
    // one it accepts.
    if (immutable && via_reassign_op) {
        report(p, &at, LHAT_PARSE_ERR_LET_NEEDS_EQUALS);
    }
    LhatNode *node = parse_binding(p, LHAT_NODE_DEFINE, &at, targets);
    if (node != NULL) {
        node->v.binding.via_reassign_op = via_reassign_op && !immutable;
        node->v.binding.immutable = immutable;
    }
    // parse_binding builds the node at the '=', and the word that introduced
    // the definition is under no node at all -- the same shape 'do^' and
    // 'public^' have.
    return start_at(node, &start);
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
    lhat_node_append(&head, &tail, finish(p, first));

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
        // From after the ':', not from the 'el^'. The marker and the condition
        // belong to the clause; starting the body at the marker would make the
        // two spans identical, and a tool showing a clause could not tell what
        // the clause itself says from what its body does.
        clause->v.clause.body = parse_block_body(p, &p->current);
        lhat_node_append(&head, &tail, finish(p, clause));
    }

    expect_op(p, LHAT_OP_RBRACE);
    node->v.list.items = head;
    return finish(p, node);
}

// 04 の 4.5: try^{ … } and the catch^ arms inside its braces. The shape is
// 5.2's -- the clauses live inside, and a ':' opens the next one -- so this
// reads the way parse_if_body does, with a written type where a condition
// would stand. The first clause is the body and carries none.
static LhatNode *parse_try_block(Parser *p)
{
    LhatToken start = p->current;
    advance(p);  // try^

    LhatNode *node = make(p, LHAT_NODE_TRY_BLOCK, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatToken brace = p->current;
    if (!expect_op(p, LHAT_OP_LBRACE)) {
        return finish(p, node);
    }

    // The list the braces open is the one this block's arms are spoken for
    // in. A try^{ } written inside another owns its own, and the outer one
    // is put back below.
    size_t enclosing_catch = p->catch_depth;
    size_t enclosing_catch_expr = p->catch_expr_depth;
    p->catch_depth = p->depth + 1;
    // A statement of that list opens one expression of its own; anything
    // deeper is a bracket or a body, where the word is the operator again.
    p->catch_expr_depth = p->expr_depth + 1;

    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    LhatNode *body = make(p, LHAT_NODE_IF_CLAUSE, &brace);
    if (body == NULL) {
        p->catch_depth = enclosing_catch;
        p->catch_expr_depth = enclosing_catch_expr;
        return finish(p, node);
    }
    body->v.clause.condition = NULL;
    body->v.clause.body = parse_block_body(p, &brace);
    lhat_node_append(&head, &tail, finish(p, body));

    // Inside these braces every catch^ is an arm, so the word alone is the
    // test here -- at_catch_arm asks a question about the list's own depth,
    // and the list has closed by the time each arm is read.
    bool bare = false;
    while (check_hat(p, "catch")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *arm = make(p, LHAT_NODE_IF_CLAUSE, &at);
        if (arm == NULL) {
            break;
        }
        // The bare arm takes whatever is left, so nothing may follow it --
        // an arm written after one could never be reached.
        if (bare) {
            report(p, &at, LHAT_PARSE_ERR_CATCH_AFTER_BARE);
        }
        // What stands here is a kind and a ':', and what a writer may have
        // meant instead is 4.1's fallback -- which is why the two are told
        // apart before the type is read, rather than by letting the type
        // fail. The message names the parentheses that separate them.
        bool shaped = true;
        if (check_op(p, LHAT_OP_COLON)) {
            bare = true;
        } else if (starts_type(p)) {
            arm->v.clause.condition = parse_type(p);
        } else {
            shaped = false;
        }
        if (!shaped || !check_op(p, LHAT_OP_COLON)) {
            report(p, &p->current, LHAT_PARSE_ERR_CATCH_ARM_NEEDS_TYPE);
            // On to the next arm or the end of the block: what follows was
            // written as arms, and reading it as loose statements would
            // report a second time about the same mistake.
            while (!at_eof(p) && !check_hat(p, "catch") &&
                   !check_op(p, LHAT_OP_RBRACE)) {
                advance(p);
            }
            continue;
        }
        advance(p);  // ':'
        // From after the ':', for the reason parse_if_body gives.
        arm->v.clause.body = parse_block_body(p, &p->current);
        lhat_node_append(&head, &tail, finish(p, arm));
    }

    p->catch_depth = enclosing_catch;
    p->catch_expr_depth = enclosing_catch_expr;
    expect_op(p, LHAT_OP_RBRACE);
    node->v.list.items = head;
    return finish(p, node);
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
        return finish(p, node);
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

// The three flags answer which spelling the focus used, so that
// parse_for can judge it against the form -- which is not known until the
// clause after the focus has been read (16.3改2).
static LhatNode *parse_for_focus(Parser *p, bool *saw_from, bool *saw_word,
                                 bool *saw_reassign)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    *saw_from = false;
    *saw_word = false;
    *saw_reassign = false;

    for (;;) {
        LhatToken at = p->current;
        // 8.6改: for^ is not an introducer of its own. An introducer
        // asks for a fresh, loop-scoped name exactly as it would anywhere
        // else; without one, a bare ':=' reaches an existing name instead
        // (8.6's own rule for a bare ':=', applied here rather than for^
        // making every focus a definition regardless of what was written).
        //
        // 8.9 and 16.3改2: either word may introduce a focus, and nothing here
        // decides between them -- parse_for judges which the form wanted, once
        // the clause after the focus has said what the form is. The while^ and
        // until^ forms of 16.3 ask the writer for a 'next^ i := i + 1', so
        // theirs is a var^; the to^ and downto^ forms advance their own focus
        // and take neither word, spelling the range 'i from^ A to^ B' instead.
        bool immutable = check_hat(p, "let");
        bool has_let = match_hat(p, "var") || match_hat(p, "let");
        if (has_let) {
            *saw_word = true;
        }

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
            //
            // 13.10: a run of names before one '=' takes apart what the value
            // answers with, here as anywhere -- which is what lets a focus be
            // 'let^ x, y = f()'. parse_define reads its targets the same way.
            // The other ',' of 16.3 is the walk's own 'k, v', and that one is
            // written with no introducer, so the two do not meet.
            LhatNode *targets = NULL;
            LhatNode *targets_tail = NULL;
            lhat_node_append(&targets, &targets_tail,
                             parse_let_target(p, !immutable));
            while (match_op(p, LHAT_OP_COMMA)) {
                lhat_node_append(&targets, &targets_tail,
                                 parse_let_target(p, !immutable));
            }
            target = targets;
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
                target = finish(p, annotated);
            }
        }

        if (equals_error) {
            target = make(p, LHAT_NODE_ERROR, &at);
        } else if (has_let) {
            // 8.6: var^ accepts either spelling, both meaning define. 8.9:
            // let^ takes only the one that does not also mean reassign.
            LhatToken op = p->current;
            bool via_reassign_op = check_op(p, LHAT_OP_REASSIGN);
            if (!match_op(p, LHAT_OP_REASSIGN) && !match_op(p, LHAT_OP_EQ)) {
                report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
                // The targets read so far are a list, and one node is what
                // the focus takes -- so the mistake stands as one node too.
                target = make(p, LHAT_NODE_ERROR, &at);
            } else {
                if (immutable && via_reassign_op) {
                    report(p, &op, LHAT_PARSE_ERR_LET_NEEDS_EQUALS);
                }
                LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
                if (binding == NULL) {
                    break;
                }
                binding->v.binding.targets = target;
                binding->v.binding.values = parse_expression(p);
                binding->v.binding.immutable = immutable;
                target = finish(p, binding);
            }
        } else if (check_hat(p, "from")) {
            // 16.3改2: 'i from^ 1 to^ 10'. from^ is the introducer of the
            // counted form, and the only one it takes. A keyword cannot be
            // read as a comparison, so this brings back what 16.3改 had to
            // take away when the spelling was '=' -- for^ introducing its own
            // focus -- without the ambiguity that cost it.
            //
            // 8.9: the name is a let^. Nothing in the source advances it,
            // 16.4's machine does, and writing neither word is what keeps the
            // header from saying something the reader has to un-learn.
            *saw_from = true;
            advance(p);
            LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
            if (binding == NULL) {
                break;
            }
            binding->v.binding.targets = target;
            binding->v.binding.values = parse_expression(p);
            binding->v.binding.immutable = true;
            binding->v.binding.bound_by_form = true;
            target = finish(p, binding);
        } else if (check_op(p, LHAT_OP_REASSIGN)) {
            // 8.6: no introducer, so ':=' reassigns an existing name.
            *saw_reassign = true;
            advance(p);
            LhatNode *binding = make(p, LHAT_NODE_REASSIGN, &at);
            if (binding == NULL) {
                break;
            }
            binding->v.binding.targets = target;
            binding->v.binding.values = parse_expression(p);
            target = finish(p, binding);
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
            target = finish_at(binding, target);
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
    return finish(p, node);
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

        // 17.5: the ':' is not optional.
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
            clause->v.clause.body = finish(p, block);
        }

        lhat_node_append(&head, &tail, finish(p, clause));
        if (defaulting) {
            break;  // 17.5: the default is the last thing there can be
        }
    }

    // 17.5: the statement form does nothing when the subject matches no
    // clause, so it may leave the default out. The expression form has to
    // answer, and there is no way to show that a set of value patterns
    // exhausted the cases -- so it takes other^ whatever the patterns are.
    //
    // No clause at all is the caller's LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE, and
    // that is the one to leave standing.
    if (as_expression && head != NULL &&
        (tail == NULL || tail->v.clause.condition != NULL)) {
        report(p, &p->current, LHAT_PARSE_ERR_MATCH_NEEDS_OTHER);
    }

    node->v.list.items = head;
    return finish(p, node);
}

// next^ takes one or more statements, separated by commas.
static LhatNode *parse_advance(Parser *p)
{
    LhatNode *head = NULL;
    LhatNode *tail = NULL;

    do {
        lhat_node_append(&head, &tail, parse_statement(p));
    } while (match_op(p, LHAT_OP_COMMA));

    return head;
}

// 16.3改2: which spelling the focus took has to agree with the form, and the
// form is not known until the clause after the focus has been read.
//
// A counted loop takes from^ and nothing else. It refuses both introducers
// because it advances its own focus -- let^ would have the source say a name
// changes when nothing in it does, and var^ would invite a write the body may
// not make. Refusing a bare ':=' as well follows from that: if neither word
// may name a focus the machine drives, neither may an outer name be handed to
// it. What 16.3改 wanted there is the conditional form, which says in the
// source what it does to the name.
static void check_focus_form(Parser *p, const LhatNode *node,
                             const LhatToken *at, bool saw_from, bool saw_word,
                             bool saw_reassign)
{
    bool counted = node->v.loop.kind == LHAT_FOR_TO ||
                   node->v.loop.kind == LHAT_FOR_DOWNTO;
    if (counted && (saw_word || saw_reassign)) {
        report(p, at, LHAT_PARSE_ERR_FOCUS_NEEDS_FROM);
    } else if (!counted && saw_from) {
        report(p, at, LHAT_PARSE_ERR_FROM_NOT_HERE);
    }
}

static LhatNode *parse_for(Parser *p)
{
    LhatToken start = p->current;
    advance(p);

    LhatNode *node = make(p, LHAT_NODE_FOR, &start);
    if (node == NULL) {
        return NULL;
    }

    LhatToken focus_at = p->current;
    bool saw_from = false;
    bool saw_word = false;
    bool saw_reassign = false;
    node->v.loop.focus =
        parse_for_focus(p, &saw_from, &saw_word, &saw_reassign);

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
        // 5.2: '{' opens the statement form and ':' the expression one,
        // here as anywhere. The focus still does not leave -- only the value
        // built from it does, which is what an expression is.
        node->v.loop.is_expression = check_op(p, LHAT_OP_COLON);
        check_focus_form(p, node, &focus_at, saw_from, saw_word, saw_reassign);
        node->v.loop.body =
            node->v.loop.is_expression
                ? parse_if_expression_from(p, at, node->v.loop.bound)
                : parse_if_body(p, at, node->v.loop.bound);
        return finish(p, node);
    } else if (check_hat(p, "do")) {
        // 16.3: the focus is made and what follows it is the answer. This is
        // how a tuple reaches another call without a name for each position
        // (13.8改): 'for^ let^ x, y = f() do^: g(x, y);'.
        //
        // There is no statement form. 'do^{ let^ x, y = f() … }' with the
        // definitions written inside is that, and it was always there.
        LhatToken at = p->current;
        advance(p);
        node->v.loop.kind = LHAT_FOR_ONCE;
        node->v.loop.is_expression = true;
        expect_op(p, LHAT_OP_COLON);
        check_focus_form(p, node, &focus_at, saw_from, saw_word, saw_reassign);
        // 17.6: a match is opened by the ':' after its subject, and that is
        // the only spelling it has -- so this is one way of writing it too
        // many. The clauses are still read, since reading them as one
        // expression would earn a second diagnostic for the same mistake.
        if (is_when_marker(p)) {
            report(p, &p->current, LHAT_PARSE_ERR_MATCH_OPENS_AFTER_SUBJECT);
            node->v.loop.kind = LHAT_FOR_WHEN;
            node->v.loop.body =
                parse_when_clauses(p, &at, node->v.loop.focus, true);
        } else {
            node->v.loop.body = parse_expression(p);
        }
        // 6 章: the ':' opened this, so a ';' closes it.
        expect_op(p, LHAT_OP_SEMICOLON);
        return finish(p, node);
    } else if (check_hat(p, "for")) {
        // 16.3: several definitions stand in a row, each scoped to the one
        // after it. The innermost carries the driving clause, so 16.1's rule
        // -- for^ takes the form of the clause that follows it -- reaches
        // this one through the body rather than from anything written here.
        node->v.loop.kind = LHAT_FOR_ONCE;
        check_focus_form(p, node, &focus_at, saw_from, saw_word, saw_reassign);
        node->v.loop.body = parse_for(p);
        node->v.loop.is_expression = node->v.loop.body != NULL &&
                                     node->v.loop.body->kind == LHAT_NODE_FOR &&
                                     node->v.loop.body->v.loop.is_expression;
        return finish(p, node);
    } else if (check_op(p, LHAT_OP_LBRACE) || check_op(p, LHAT_OP_COLON)) {
        // 17 章: no driving clause at all, so what follows dispatches on the
        // subject rather than iterating over it.
        node->v.loop.kind = LHAT_FOR_WHEN;
        check_focus_form(p, node, &focus_at, saw_from, saw_word, saw_reassign);
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
        return finish(p, node);
    } else {
        report(p, &p->current, LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE);
        return finish(p, node);
    }

    check_focus_form(p, node, &focus_at, saw_from, saw_word, saw_reassign);

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
    return finish(p, node);
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
    return finish(p, node);
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
        if (p->current.kind != LHAT_TOKEN_IDENT) {
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
        // 14.14改: a brace introduces, so either spelling reads. Unlike the
        // sites that call expect_introduces, a default is optional here --
        // 04 の 2.2 lets a field carry only a type -- so nothing is reported
        // when neither is there.
        if (match_op(p, LHAT_OP_EQ) || match_op(p, LHAT_OP_REASSIGN)) {
            field->v.param.fallback = parse_expression(p);
        }
        if (field->v.param.type == NULL && field->v.param.fallback == NULL) {
            report(p, &p->current, LHAT_PARSE_ERR_FIELD_NEEDS_TYPE);
        }

        lhat_node_append(&head, &tail, finish(p, field));
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

    if (p->current.kind == LHAT_TOKEN_IDENT) {
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

        if (p->current.kind != LHAT_TOKEN_IDENT) {
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

        lhat_node_append(&head, &tail, finish(p, kind));

        if (!match_op(p, LHAT_OP_COMMA)) {
            break;
        }
    }

    node->v.named.members = head;
    expect_op(p, LHAT_OP_RBRACE);
    return finish(p, node);
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
    // an optional ':type' the same way let^'s target does -- but never a
    // '.member' path, since with^ always introduces a fresh local rather than
    // reaching into an existing table.
    //
    // 8.9: what it introduces is a let^, not a var^. The block holds the
    // resource for exactly as long as the name does, and 12.2 disposes of
    // whatever the name still holds at the end of it; a ':=' in between would
    // leave the block disposing of one object having leaked another. So ':='
    // is refused here for the same reason let^ refuses it.
    while (check_hat(p, "with")) {
        LhatToken at = p->current;
        advance(p);
        LhatNode *binding = make(p, LHAT_NODE_DEFINE, &at);
        if (binding == NULL) {
            break;
        }
        binding->v.binding.targets = parse_with_target(p);
        LhatToken op = p->current;
        bool via_reassign_op = match_op(p, LHAT_OP_REASSIGN);
        if (!via_reassign_op && !match_op(p, LHAT_OP_EQ)) {
            report(p, &p->current, LHAT_PARSE_ERR_LET_NEEDS_VALUE);
        } else {
            if (via_reassign_op) {
                report(p, &op, LHAT_PARSE_ERR_LET_NEEDS_EQUALS);
            }
            binding->v.binding.values = parse_expression(p);
        }
        binding->v.binding.immutable = true;
        binding->v.binding.bound_by_form = true;
        lhat_node_append(&head, &tail, finish(p, binding));
    }

    node->v.list.items = head;
    node->v.list.extra = parse_braced_block(p, false, false);
    return finish(p, node);
}

// 13.8改: 'return^ (a, b)' and 'return^ a, b' answer the same tuple, so they
// become the same node -- the positions hang off `value` with `level` naming
// how many. One shape reaches the checker and the compiler, and neither
// grows a case for the parenthesised spelling.
//
// This is also what makes 15.12's implicit return of a literal work without
// a word of its own: answer_with_body turns the body's bare '(a, b)' into a
// return^ and folds it here, so 'f^ { (0, 1) }' and 'f^ { return^ 0, 1 }'
// compile to the same instructions.
//
// Left alone in expression position: 'f() catch^ (0, 1)' has no jump to fold
// into, and the literal stands there as a value (13.8改's MAKERUN).
static void fold_tuple_answer(LhatNode *jump)
{
    LhatNode *value = jump->v.jump.value;
    if (value == NULL || value->next != NULL ||
        value->kind != LHAT_NODE_TUPLE) {
        return;
    }
    jump->v.jump.value = value->v.list.items;
    jump->v.jump.level = (uint32_t)lhat_node_list_length(value->v.list.items);
}

static LhatNode *parse_jump(Parser *p, LhatNodeKind kind)
{
    LhatToken start = p->current;
    advance(p);
    LhatNode *node = make(p, kind, &start);
    if (node == NULL) {
        return NULL;
    }

    // 02 の 9.8: how many loops to leave, spelt either way. The hats on the
    // word count it -- break^ is one, break^^^ is three -- and the bracketed
    // form says the same number, so 'break^[3]' and 'break^^^' are
    // one thing written twice. The bracket is never a bare expression, which
    // is what keeps it apart from the operand return^ and yield^ take.
    //
    // 9.11: next^ counts the same way, over the loop it goes on with rather
    // than the one it leaves.
    if (kind == LHAT_NODE_BREAK || kind == LHAT_NODE_NEXT) {
        node->v.jump.level = start.v.hats > 0 ? start.v.hats : 1;
        if (match_op(p, LHAT_OP_LBRACKET)) {
            LhatNode *written = parse_expression(p);
            expect_op(p, LHAT_OP_RBRACKET);
            if (written != NULL && written->kind == LHAT_NODE_INT &&
                start.v.hats <= 1) {
                // A count, and the hats did not already give one. Saying it
                // twice is refused below rather than reconciled.
                node->v.jump.level = written->v.integer.value > 0
                                         ? (uint32_t)written->v.integer.value
                                         : 0;
            } else {
                // 9.8's label form, or a count contradicting the hats. Kept
                // for the compiler to refuse by name.
                node->v.jump.value = written;
            }
        }
        return finish(p, node);
    }

    // With no statement terminator, a word that begins a statement must not
    // be mistaken for the operand of the one before it.
    //
    // 01 の 10.9 settles the case a keyword cannot: the operand has to be on
    // the same line. Without that, a bare `yield^` or `return^` swallows
    // whatever statement comes next, since an ordinary name begins an
    // expression just as well as it begins a statement.
    //
    // 5.1 and 17.2: these two begin an expression as readily as a statement,
    // and what stands after a jump is a value -- 15.12 writes the very same
    // if^ expression as a body, so refusing it here made one spelling of an
    // answer unwritable. The statement form written on this line is what
    // gives way; the next line still reads as it always did, which is where
    // a statement after a valueless jump belongs.
    bool begins_expression_form = check_hat(p, "if") || check_hat(p, "for");
    bool operand_follows =
        !p->current.preceded_by_newline &&
        ((starts_expression(&p->current) &&
          (!is_statement_keyword(p) || begins_expression_form)) ||
         check_op(p, LHAT_OP_LPAREN) || check_op(p, LHAT_OP_SUB) ||
         check_op(p, LHAT_OP_NOT) || check_op(p, LHAT_OP_LBRACE));

    if (operand_follows) {
        node->v.jump.value = parse_expression(p);

        // 02 の 13.8改: 'return^ a, b' answers a tuple. The values are a list
        // hanging off `value`, and `level` says how many -- 0 and 1 both mean
        // one, which is every return^ written before tuples existed.
        //
        // A ',' can only be this here: 11 章 has no comma operator, and the
        // ones inside a call or a table were consumed by parse_expression.
        // 'return^ { a, b }' is untouched, and still answers a table -- the
        // two forms are told apart by what is written, which is why nothing
        // has to ask whether a table escapes.
        // yield^ takes the same form as a statement. As an expression it does
        // not: there the ',' would be the value list of the binding around it
        // ('var^ x = yield^ a, b' could be either reading), and that other
        // path is parsed elsewhere. A yield^ answering several values and
        // receiving one is written as a statement.
        if ((kind == LHAT_NODE_RETURN || kind == LHAT_NODE_YIELD) &&
            check_op(p, LHAT_OP_COMMA)) {
            LhatNode *head = node->v.jump.value;
            LhatNode *tail = head;
            while (match_op(p, LHAT_OP_COMMA)) {
                lhat_node_append(&head, &tail, parse_expression(p));
            }
            node->v.jump.value = head;
            node->v.jump.level = (uint32_t)lhat_node_list_length(head);
        }
        fold_tuple_answer(node);
    }
    return finish(p, node);
}

// 04 の 11.6: unlike return^/break^/yield^, the value is not optional --
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
    return finish(p, node);
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
            return start_at(parse_braced_block(p, false, false), &start);
        }
        // 04 の 4.5: a brace after try^ opens the block form, which is a
        // statement. Everywhere else try^ is the unary operator of 5 章, and
        // what follows it is an expression -- a table literal there would be
        // asking a table for an error it cannot hold, so the two do not meet.
        if (check_hat(p, "try") && is_op(&p->ahead, LHAT_OP_LBRACE)) {
            return parse_try_block(p);
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
        // 9.8: break^^^ counts loops with its hats, so the word is matched
        // rather than the spelling. 9.11's next^ counts the same way, and
        // 9.6 gives it two other spellings -- the loop it names is the one
        // it goes on with.
        if (token_is_hat_stacked(p, &p->current, "break")) {
            return parse_jump(p, LHAT_NODE_BREAK);
        }
        if (token_is_hat_stacked(p, &p->current, "next") ||
            token_is_hat_stacked(p, &p->current, "skip") ||
            token_is_hat_stacked(p, &p->current, "continue")) {
            return parse_jump(p, LHAT_NODE_NEXT);
        }
        if (check_hat(p, "panic")) {
            return parse_panic(p);
        }
        // 05 の 5.5: a require^ standing alone binds the unit under the
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
            return finish_at(node, inner);
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
        // 8.9: the two introducers of a name. var^ is 8.6's, unchanged; let^
        // is the same form binding a name nothing may reassign.
        if (check_hat(p, "var")) {
            return parse_let(p, false);
        }
        if (check_hat(p, "let")) {
            return parse_let(p, true);
        }
        if (check_hat(p, "module")) {
            return parse_module(p);
        }
        if (check_hat(p, "public")) {
            return parse_public(p);
        }
    }

    // Otherwise a reassignment or a call. A definition needs let^ (8.6), so
    // nothing here can create a name.
    LhatNode *head = NULL;
    LhatNode *tail = NULL;
    lhat_node_append(&head, &tail, parse_target(p));
    while (match_op(p, LHAT_OP_COMMA)) {
        lhat_node_append(&head, &tail, parse_target(p));
    }

    if (check_op(p, LHAT_OP_REASSIGN)) {
        LhatToken at = p->current;
        advance(p);
        return parse_binding(p, LHAT_NODE_REASSIGN, &at, head);
    }

    // 7.4: 'target op= value' is 'target := target op value' with target
    // read once. 7.4 lets several be written at once, the way a plain ':='
    // already may: each target is paired with the value standing at its own
    // position, and 13.8's "read everything, then write everything" holds
    // here as it does there.
    LhatOpKind compound_base;
    if (p->current.kind == LHAT_TOKEN_OP &&
        compound_assign_op(p->current.v.op, &compound_base)) {
        LhatToken at = p->current;
        advance(p);

        LhatNode *rhs_head = NULL;
        LhatNode *rhs_tail = NULL;
        lhat_node_append(&rhs_head, &rhs_tail, parse_expression(p));
        while (match_op(p, LHAT_OP_COMMA)) {
            lhat_node_append(&rhs_head, &rhs_tail, parse_expression(p));
        }

        LhatNode *node = make(p, LHAT_NODE_REASSIGN, &at);
        if (node == NULL) {
            return NULL;
        }
        node->v.binding.targets = head;

        size_t target_count = lhat_node_list_length(head);
        size_t source_count = lhat_node_list_length(rhs_head);
        if (target_count != source_count) {
            // Nothing spreads one value across several targets here: a
            // compound operator reads the target it writes back to, so a
            // value it never saw cannot stand for one.
            report(p, &at, LHAT_PARSE_ERR_BINDING_ARITY);
        }

        // Each value read is 'target op rhs' -- built around the very same
        // target node the compiler already reads for its address, so
        // checking it costs nothing (infer has no side effect) and compiling
        // it costs a register read, never owner/key evaluated again (below).
        LhatNode *values = NULL;
        LhatNode *values_tail = NULL;
        LhatNode *rhs = rhs_head;
        for (LhatNode *target = head; target != NULL && rhs != NULL;
             target = target->next, rhs = rhs->next) {
            LhatNode *binary = make(p, LHAT_NODE_BINARY, &at);
            if (binary == NULL) {
                break;
            }
            binary->v.binary.op = compound_base;
            binary->v.binary.left = target;
            binary->v.binary.right = rhs;
            lhat_node_append(&values, &values_tail, finish_at(binary, rhs));
        }

        node->v.binding.values = values != NULL ? values : rhs_head;
        node->v.binding.has_compound_op = true;
        node->v.binding.compound_op = compound_base;
        return finish(p, node);
    }

    // 02 の 8.4: the postfix form of reassignment, which the language has not.
    if (check_op(p, LHAT_OP_ARROW)) {
        report(p, &p->current, LHAT_PARSE_ERR_WITHDRAWN_ARROW);
        advance(p);
        parse_expression(p);
        return make(p, LHAT_NODE_ERROR, &start);
    }

    // '<<' and '>>' are reserved. Met after a target they read as an
    // operator that does not exist; 01 の 7 章 makes bit operations
    // functions rather than operators, so the message can say so.
    if (check_op(p, LHAT_OP_LSHIFT) || check_op(p, LHAT_OP_RSHIFT)) {
        report(p, &p->current, LHAT_PARSE_ERR_RESERVED_SHIFT);
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
        return finish(p, node);
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
    // 15.12: no body is open yet. Left unset, this read whatever the stack
    // held, and a value that happened to equal `depth` let 8.2's bare
    // expression through at the top level of a unit.
    p->bare_depth = 0;
    p->catch_depth = 0;  // 04 の 4.5, and left unset for the same reason
    p->expr_depth = 0;
    p->catch_expr_depth = 0;
    // Nothing has been consumed yet, so `finish` before the first advance
    // must not widen anything.
    memset(&p->previous, 0, sizeof p->previous);
    p->current = lhat_lexer_next(lexer);
    p->ahead = lhat_lexer_next(lexer);
}

#if LHAT_WITH_COMMENTS

// 01 の 6.4: tying the kept comments to the nodes they were written against.
//
// One pass, after the whole unit is parsed. Both the comment table and the
// walk are in source order, so a single cursor over the table is enough --
// and doing it here rather than while parsing means the table has stopped
// growing, so the pointers handed to nodes stay valid.
typedef struct {
    const LhatSource *source;
    LhatComment *comments;  // the lexer's table, threaded as it is handed out
    size_t count;
    size_t cursor;
} CommentWalk;

// Whether a comment sits on the same line as the code before it -- a trailing
// comment, which belongs to what it follows rather than to what comes next.
// Asked of the source text rather than of line numbers, since a node records
// only where it starts and a statement may span several lines.
static bool same_line(const CommentWalk *walk, uint32_t after, uint32_t offset)
{
    if (offset <= after || walk->source == NULL) {
        return false;
    }
    const char *from = walk->source->text + after;
    return memchr(from, '\n', (size_t)(offset - after)) == NULL;
}

// Gives `node` every comment from the cursor up to `limit`, appending so that
// a node given comments twice -- the ones above it, then the one left at the
// end of its last line -- keeps them in source order.
static void take_comments(CommentWalk *walk, LhatNode *node, uint32_t limit)
{
    while (walk->cursor < walk->count &&
           walk->comments[walk->cursor].offset < limit) {
        LhatComment *c = &walk->comments[walk->cursor++];
        if (node == NULL) {
            continue;
        }
        c->next_for_node = NULL;
        LhatComment **slot = &node->comments;
        while (*slot != NULL) {
            slot = &(*slot)->next_for_node;
        }
        *slot = c;
    }
}

static void attach_within(CommentWalk *walk, LhatNode *node);

// Hands out every comment before `limit`, choosing for each between what it
// follows and what it precedes: the one before it when it ends that line, and
// otherwise the one it was written above.
static void split_comments(CommentWalk *walk, LhatNode *previous,
                           LhatNode *following, uint32_t limit)
{
    while (walk->cursor < walk->count &&
           walk->comments[walk->cursor].offset < limit) {
        const LhatComment *c = &walk->comments[walk->cursor];
        bool trailing =
            previous != NULL && same_line(walk, previous->end, c->offset);
        // One at a time: whether the next is trailing is its own question.
        take_comments(walk, trailing ? previous : following, c->offset + 1);
    }
}

typedef struct {
    CommentWalk *walk;
    LhatNode *parent;
    LhatNode *previous;  // the child last walked, for a trailing comment
} AttachContext;

static void attach_child(void *context, const char *field, bool in_list,
                         const LhatNode *child)
{
    (void)field;
    (void)in_list;
    AttachContext *state = (AttachContext *)context;
    LhatNode *mutable_child = (LhatNode *)child;
    // Where the child's construct begins, not where its own node does: a
    // comment written before 'a' in 'a + b' belongs to the sum, and the sum's
    // own offset is the '+'.
    split_comments(state->walk, state->previous, mutable_child,
                   lhat_node_span_start(child));

    attach_within(state->walk, mutable_child);
    state->previous = mutable_child;
}

// Everything inside `node`, then whatever is left before it ends. A comment
// after the last child but still within the braces belongs to the node --
// unless it ends the last child's line, which makes it that child's.
static void attach_within(CommentWalk *walk, LhatNode *node)
{
    if (walk->cursor >= walk->count) {
        return;
    }
    AttachContext state = {walk, node, NULL};
    lhat_node_visit_children(node, attach_child, &state);
    split_comments(walk, state.previous, node, node->end);
}

static void attach_comments(LhatLexer *lexer, LhatParseResult *result)
{
    if (result->root == NULL || lexer->comment_count == 0) {
        return;
    }
    CommentWalk walk = {lexer->source, lexer->comments, lexer->comment_count, 0};
    attach_within(&walk, result->root);
    // Anything past the end of the tree -- a comment on the last line of the
    // file, after every statement -- still belongs to the unit.
    take_comments(&walk, result->root, UINT32_MAX);
}

#endif  // LHAT_WITH_COMMENTS

static void parser_finish(Parser *p, LhatLexer *lexer, LhatParseResult *result)
{
    if (!at_eof(p)) {
        report(p, &p->current, LHAT_PARSE_ERR_UNEXPECTED);
    }

#if LHAT_WITH_COMMENTS
    attach_comments(lexer, result);
#endif

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

    // The unit is the whole of the file, not the stretch from its first
    // statement to its last. Without this a comment on the first or last line
    // falls outside the tree entirely, and there is nothing there for it to
    // belong to.
    if (result->root != NULL && lexer->source != NULL) {
        result->root->offset = 0;
        result->root->line = 1;
        result->root->column = 1;
        if ((uint32_t)lexer->source->length > result->root->end) {
            result->root->end = (uint32_t)lexer->source->length;
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
            return "reassignment puts the target first; write 'target := value'";
        case LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON:
            return "'::' is not part of the language; write '->' before the "
                   "return type";
        case LHAT_PARSE_ERR_RESERVED_SHIFT:
            return "'<<' and '>>' are reserved and not part of the language; "
                   "a bit operation is a function";
        case LHAT_PARSE_ERR_ID_NEEDS_NAME:
            return "id^ answers the spelling of a name; write the name after "
                   "it";
        case LHAT_PARSE_ERR_BINDING_ARITY:
            return "the number of targets and values does not match";
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
        case LHAT_PARSE_ERR_FROM_NOT_HERE:
            return "from^ opens the counted range of a to^ or downto^ loop; "
                   "this form takes 'var^ i = 1' or 'let^ i = 1'";
        case LHAT_PARSE_ERR_FOCUS_NEEDS_FROM:
            return "a to^ or downto^ loop advances a focus of its own; write "
                   "'for^ i from^ 1 to^ 10', or a while^ loop to count with a "
                   "name that is already there";
        case LHAT_PARSE_ERR_OPERATOR_NOT_DEFINABLE:
            return "op^ defines '..' and the arithmetic operators; and^, or^, "
                   "'!' and the comparisons are the language's own";
        case LHAT_PARSE_ERR_COMPOUND_NOT_DEFINABLE:
            return "a compound assignment has no definition of its own: "
                   "'a += b' is 'a := a + b', so it is op^+ that decides what "
                   "it does";
        case LHAT_PARSE_ERR_COMPARISON_NOT_DEFINABLE:
            return "the orderings are not written one by one: op^<=> "
                   "answers with a number^, and '<', '>', '\xE2\x89\xA6' and "
                   "'\xE2\x89\xA7' are all read off it. A type that knows what "
                   "equals what but puts its values in no order writes op^= "
                   "instead, which answers a bool^; '\xE2\x89\xA0' is read off "
                   "whichever of the two it has";
        case LHAT_PARSE_ERR_EXPECTED_MEMBER:
            return "a def^ holds 'name := value' members and one self^{ ... }";
        case LHAT_PARSE_ERR_FIELD_NEEDS_NAME:
            return "every field of self^{ ... } needs a name and a value";
        case LHAT_PARSE_ERR_DUPLICATE_TEMPLATE:
            return "a def^ declares its fields once; write one self^{ ... }";
        case LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE:
            return "override^ and overload^ mark a member, not the fields";
        case LHAT_PARSE_ERR_CLOSED_NEEDS_BODY:
            return "closed^ marks a body: write closed^f^ ... or closed^p^ ...";
        case LHAT_PARSE_ERR_CATCH_AFTER_BARE:
            return "a bare catch^: takes whatever is left, so nothing follows "
                   "it -- write the narrower arms first";
        case LHAT_PARSE_ERR_CATCH_ARM_NEEDS_TYPE:
            return "an arm of a try^{ } is written 'catch^ Kind:' or bare as "
                   "'catch^:'. A fallback value is the other reading of the "
                   "word and is written with parentheses here: "
                   "'let^ n = (f() catch^ 0)'";
        case LHAT_PARSE_ERR_MODULE_MISPLACED:
            return "module^ goes first, and only once in a file";
        case LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION:
            return "public^ marks a let^ or an errordef^";
        case LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL:
            return "require^ takes a written path, since the checker follows it";
        case LHAT_PARSE_ERR_ELSE_NEEDS_COLON:
            return "this needs a ':' after it; what follows was read as the "
                   "condition of a further test, and no ':' came";
        case LHAT_PARSE_ERR_IF_EXPR_NEEDS_ELSE:
            return "an if^ written as an expression answers in every case; "
                   "write 'el^: ...' before the ';', or the statement form "
                   "with braces";
        case LHAT_PARSE_ERR_MATCH_NEEDS_OTHER:
            return "a match written as an expression answers in every case; "
                   "write 'other^: ...' before the ';', or the statement form "
                   "with braces";
        case LHAT_PARSE_ERR_MATCH_OPENS_AFTER_SUBJECT:
            return "a match is opened by the ':' after its subject; write "
                   "'for^ e: when^ ...;' -- a do^: answers with the "
                   "expression that follows it";
        case LHAT_PARSE_ERR_SPREAD_NOT_LAST:
            return "'...' forwards the whole collected tail, so nothing can "
                   "follow it here";
        case LHAT_PARSE_ERR_HATS_DONT_STACK:
            return "a second hat counts levels, which this word does not "
                   "take here";
        case LHAT_PARSE_ERR_FIELD_NEEDS_TYPE:
            return "a field needs a type, a default, or both";
        case LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME:
            return "errordef^ needs a name; an error kind has no anonymous form";
        case LHAT_PARSE_ERR_ERROR_NEEDS_KIND:
            return "write the kind, as in error^IOError.NotFound{ ... }";
        case LHAT_PARSE_ERR_LET_NEEDS_VALUE:
            return "a definition needs a value; write 'var^ x = 0'";
        case LHAT_PARSE_ERR_LET_NEEDS_EQUALS:
            return "let^ defines and never reassigns; write 'let^ x = 0', or "
                   "'var^ x = 0' for a name that may be reassigned";
        case LHAT_PARSE_ERR_LET_NEEDS_NAME:
            return "let^ binds a name; write 'var^ t.a = 1' for a member of a "
                   "table";
        case LHAT_PARSE_ERR_EQUALS_IS_COMPARISON:
            return "'=' compares; write 'x := 1' to reassign or 'var^ x = 1' "
                   "to make a new name";
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
