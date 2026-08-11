// L^ (lhat) -- tests for the parser.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "01". The cases that matter most are the ones where a decision in the
// specification would be invisible in the token stream but visible here:
// precedence, comparison chaining, the call parenthesis rule, and the
// statement forms that are deliberately not accepted.

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "fixture.h"

static const LhatNode *first_statement(const Parse *p)
{
    return p->result.root != NULL ? p->result.root->v.list.items : NULL;
}

static size_t error_count(const Parse *p)
{
    return p->result.diagnostic_count + p->lexer.diagnostic_count;
}

static bool is_binary(const LhatNode *node, LhatOpKind op)
{
    return node != NULL && node->kind == LHAT_NODE_BINARY &&
           node->v.binary.op == op;
}

// The value bound by the first statement, whatever the statement form.
static const LhatNode *first_value(const Parse *p)
{
    const LhatNode *statement = first_statement(p);
    if (statement == NULL) {
        return NULL;
    }
    switch (statement->kind) {
        case LHAT_NODE_DEFINE:
        case LHAT_NODE_REASSIGN:
            return statement->v.binding.values;
        case LHAT_NODE_CALL_STMT:
            return statement->v.jump.value;
        default:
            return statement;
    }
}

// ---------------------------------------------------------------------------
// Spans (ast.h's `end`)

typedef struct {
    const LhatNode *parent;
    int failures;
} SpanWalk;

static void span_check_child(void *context, const char *field, bool in_list,
                             const LhatNode *child);

// A parent that ends before one of its children does has forgotten to widen
// itself -- parser.c's finish is what a parse function calls on the way out,
// and a missed call always shows up this way round, never the other.
static void span_walk(const LhatNode *node, SpanWalk *state)
{
    if (node == NULL) {
        return;
    }
    LHAT_CHECK(node->end >= node->offset, "%s: end %u before offset %u",
               lhat_node_kind_name(node->kind), node->end, node->offset);

    SpanWalk inner = {node, 0};
    lhat_node_visit_children(node, span_check_child, &inner);
    state->failures += inner.failures;
}

static void span_check_child(void *context, const char *field, bool in_list,
                             const LhatNode *child)
{
    (void)field;
    (void)in_list;
    SpanWalk *state = (SpanWalk *)context;
    // FOCUS carries no span at all: 16.2's it^ need not appear in the source.
    if (child->kind != LHAT_NODE_FOCUS) {
        LHAT_CHECK(state->parent->end >= child->end,
                   "%s ends at %u but its %s child ends at %u",
                   lhat_node_kind_name(state->parent->kind), state->parent->end,
                   lhat_node_kind_name(child->kind), child->end);
    }
    span_walk(child, state);
}

static void check_spans_enclose(const char *text)
{
    Parse p;
    parse_text(&p, text);
    LHAT_CHECK(error_count(&p) == 0, "%s: %zu unexpected diagnostics", text,
               error_count(&p));
    SpanWalk state = {NULL, 0};
    span_walk(p.result.root, &state);
    parse_dispose(&p);
}

// 06 の 4.2: the source a node covers. The left edge comes from the subtree
// (lhat_node_span_start) because an infix or postfix node is written starting
// at its operator; the right edge is the node's own, since the token that
// closes a construct belongs to no child.
static void check_span_text(const char *text, const LhatNode *(*pick)(const Parse *),
                            const char *expected)
{
    Parse p;
    parse_text(&p, text);
    const LhatNode *node = pick(&p);
    if (node == NULL) {
        LHAT_CHECK(false, "%s: no node", text);
    } else {
        uint32_t start = lhat_node_span_start(node);
        LHAT_CHECK_EQ_STR(p.source.text + start, node->end - start, expected);
    }
    parse_dispose(&p);
}

static void test_spans(void)
{
    // Every construct, walked. The check is structural, so one realistic
    // program per form is worth more than many spellings of the same one.
    LHAT_TEST("a span encloses the spans below it");
    check_spans_enclose("var^ x = 1\n");
    check_spans_enclose("let^ y = f(1, 2) + g.h[3]\n");
    check_spans_enclose("if^ x > 0 { print(\"p\") el^: print(\"n\") }\n");
    check_spans_enclose("let^ v = if^ x > 0: 1 el^: 2 ;\n");
    check_spans_enclose("for^ i from^ 1 to^ 10 { print(i) }\n");
    check_spans_enclose("for^ x in^ xs { print(x) }\n");
    check_spans_enclose("repeat^ 3 { print(1) }\n");
    check_spans_enclose("let^ f = f^a:number^, b:number^ -> number^ { return^ a + b }\n");
    check_spans_enclose("let^ t = { 1, 2, k := 3 }\n");
    check_spans_enclose("let^ d = def^{ self^{ a := 1 }, m := f^ { return^ 1 } }\n");
    check_spans_enclose("errordef^ E { NotFound { path : string^ } }\n");
    check_spans_enclose("let^ s = $\"a{x}b\"\n");
    check_spans_enclose("let^ c = a < b < c\n");
    check_spans_enclose("let^ n = x as^ number^ | string^\n");
    check_spans_enclose("with^ h = open() { read(h) }\n");
    check_spans_enclose("for^ v { when^ 1, 2: print(1) other^: print(2) }\n");
    check_spans_enclose("module^ a.b\nlet^ q = try^ f() catch^ 0\n");
    check_spans_enclose("do^ { var^ i = 0\ni += 1 }\n");
    check_spans_enclose("let^ g = p^ { yield^ 1 }\n");
    check_spans_enclose("let^ ty = x as^ f^number^ -> string^ ;\n");
    check_spans_enclose("let^ co = x as^ c^{ f^number^ -> string^;, nil^ }\n");

    // The whole of a construct, closing token included.
    LHAT_TEST("a span reaches the token that closes the construct");
    check_span_text("if^ x { f() }\n", first_statement, "if^ x { f() }");
    // A word that leads a construct belongs to no node under it -- a binding
    // is built at its '=' -- so the parser moves the node's own start back
    // over it.
    check_span_text("do^ { f() }\n", first_statement, "do^ { f() }");
    check_span_text("public^ let^ x = 1\n", first_statement,
                    "public^ let^ x = 1");
    check_span_text("var^ x = 1\n", first_statement, "var^ x = 1");
    check_span_text("let^ y = f(1)\n", first_statement, "let^ y = f(1)");
    check_span_text("var^ a, b = 1, 2\n", first_statement, "var^ a, b = 1, 2");
    check_span_text("let^ v = { 1, 2 }\n", first_value, "{ 1, 2 }");
    check_span_text("let^ v = f(1, 2)\n", first_value, "f(1, 2)");
    check_span_text("let^ v = a.b[1]\n", first_value, "a.b[1]");
    check_span_text("let^ v = if^ c: 1 el^: 2 ;\n", first_value,
                    "if^ c: 1 el^: 2 ;");
    check_span_text("let^ v = f^ { return^ 1 }\n", first_value,
                    "f^ { return^ 1 }");
    check_span_text("for^ i from^ 1 to^ 3 { f() }\n", first_statement,
                    "for^ i from^ 1 to^ 3 { f() }");
    check_span_text("repeat^ 2 { f() }\n", first_statement, "repeat^ 2 { f() }");

    check_span_text("let^ v = a + b * c\n", first_value, "a + b * c");
    check_span_text("let^ v = a.b.c\n", first_value, "a.b.c");

    // The left edge really does come from the subtree and not from the node:
    // an infix node is written starting at its own operator.
    LHAT_TEST("an infix node's own offset is its operator");
    Parse p;
    parse_text(&p, "let^ v = a + b\n");
    const LhatNode *sum = first_value(&p);
    if (sum == NULL || sum->kind != LHAT_NODE_BINARY) {
        LHAT_CHECK(false, "expected a binary node");
    } else {
        LHAT_CHECK_EQ_STR(p.source.text + sum->offset,
                          sum->end - sum->offset, "+ b");
    }
    parse_dispose(&p);
}

// ---------------------------------------------------------------------------
// Comments (01 の 6.4)

#if LHAT_WITH_COMMENTS

// The comments a node was given, joined so one check reads them all. Ordered
// as they were written, which is what the table's order guarantees.
static void comments_of(const Parse *p, const LhatNode *node, char *out,
                        size_t size)
{
    out[0] = '\0';
    if (node == NULL) {
        return;
    }
    size_t used = 0;
    for (const LhatComment *c = node->comments; c != NULL;
         c = c->next_for_node) {
        size_t length = c->end - c->offset;
        if (used + length + 2 >= size) {
            break;
        }
        if (used > 0) {
            out[used++] = '|';
        }
        memcpy(out + used, p->source.text + c->offset, length);
        used += length;
        out[used] = '\0';
    }
}

static void check_comments(const char *text,
                           const LhatNode *(*pick)(const Parse *),
                           const char *expected)
{
    Parse p;
    parse_text(&p, text);
    char joined[512];
    comments_of(&p, pick(&p), joined, sizeof joined);
    LHAT_CHECK_EQ_STR(joined, strlen(joined), expected);
    parse_dispose(&p);
}

static const LhatNode *root_node(const Parse *p)
{
    return p->result.root;
}

static const LhatNode *second_statement(const Parse *p)
{
    const LhatNode *first = first_statement(p);
    return first != NULL ? first->next : NULL;
}

// Counts what the tree holds, so it can be compared with what the lexer
// scanned. A comment attached twice is counted twice here, and one dropped
// is not counted at all.
static void count_attached(void *context, const char *field, bool in_list,
                           const LhatNode *child);

static void count_attached_in(const LhatNode *node, size_t *total)
{
    for (const LhatComment *c = node->comments; c != NULL;
         c = c->next_for_node) {
        (*total)++;
    }
    lhat_node_visit_children(node, count_attached, total);
}

static void count_attached(void *context, const char *field, bool in_list,
                           const LhatNode *child)
{
    (void)field;
    (void)in_list;
    count_attached_in(child, (size_t *)context);
}

static void check_attached_once(const char *text)
{
    Parse p;
    parse_text(&p, text);
    size_t attached = 0;
    if (p.result.root != NULL) {
        count_attached_in(p.result.root, &attached);
    }
    LHAT_CHECK(attached == p.lexer.comment_count,
               "%zu comments scanned but %zu attached", p.lexer.comment_count,
               attached);
    parse_dispose(&p);
}

static void test_comments(void)
{
    // 01 の 6.4: the lexer keeps them rather than dropping them as trivia.
    LHAT_TEST("comments are kept");
    Parse p;
    parse_text(&p, "# one\nvar^ x = 1  # two\n#[ three ]#\n");
    LHAT_CHECK_EQ_INT(p.lexer.comment_count, 3);
    LHAT_CHECK_EQ_INT(p.lexer.comments[2].block, true);
    LHAT_CHECK_EQ_INT(p.lexer.comments[0].block, false);
    // The span covers the whole of the comment, markers included.
    LHAT_CHECK_EQ_STR(p.source.text + p.lexer.comments[2].offset,
                      p.lexer.comments[2].end - p.lexer.comments[2].offset,
                      "#[ three ]#");
    parse_dispose(&p);

    LHAT_TEST("a comment before a statement belongs to it");
    check_comments("# why\nvar^ x = 1\n", first_statement, "# why");
    check_comments("# a\n# b\nvar^ x = 1\n", first_statement, "# a|# b");
    check_comments("#[ block ]#\nvar^ x = 1\n", first_statement, "#[ block ]#");

    // 6.4: a comment on the same line as the code before it was written
    // against that code, not against whatever follows.
    LHAT_TEST("a trailing comment belongs to the line it ends");
    check_comments("var^ x = 1  # here\nvar^ y = 2\n", first_statement,
                   "# here");
    check_comments("var^ x = 1  # here\nvar^ y = 2\n", second_statement, "");
    check_comments("var^ x = 1\n# next\nvar^ y = 2\n", first_statement, "");
    check_comments("var^ x = 1\n# next\nvar^ y = 2\n", second_statement,
                   "# next");

    LHAT_TEST("a comment inside a block stays inside it");
    {
        Parse q;
        parse_text(&q, "do^ {\n  # inner\n  f()\n}\n");
        const LhatNode *block = first_statement(&q);
        const LhatNode *inner = block != NULL ? block->v.list.items : NULL;
        char joined[512];
        comments_of(&q, inner, joined, sizeof joined);
        LHAT_CHECK_EQ_STR(joined, strlen(joined), "# inner");
        parse_dispose(&q);
    }

    // Nothing follows it inside the braces, so it belongs to the last thing
    // there rather than escaping to the statement after the block.
    LHAT_TEST("a comment after the last statement of a block stays in it");
    check_comments("do^ {\n  f()\n  # last\n}\nvar^ y = 2\n", second_statement,
                   "");

    LHAT_TEST("a comment after every statement belongs to the unit");
    check_comments("var^ x = 1\n# tail\n", root_node, "# tail");

    // Every comment lands on exactly one node: none dropped, none shared.
    // A node may be given comments in two goes -- the ones above it and the
    // one ending its last line -- with another node's comment scanned in
    // between, so a node holding a range of the table would swallow that one.
    LHAT_TEST("every comment is attached exactly once");
    check_attached_once("# lead\n"
                        "module^ a.b  # on the module\n"
                        "#[ before ]#\n"
                        "let^ f = f^n:number^ -> number^ {\n"
                        "  # in the body\n"
                        "  return^ n + 1  # trailing\n"
                        "}\n"
                        "# tail\n");
    // The shape that breaks a range: leading, then one deeper in the subtree,
    // then one trailing the same statement.
    check_attached_once("do^ {\n"
                        "  # a\n"
                        "  f(\n"
                        "    # b\n"
                        "    1\n"
                        "  )  # c\n"
                        "}\n");
    check_attached_once("let^ t = {\n"
                        "  # first\n"
                        "  1,  # one\n"
                        "  # second\n"
                        "  2\n"
                        "}  # after\n");
}

#endif  // LHAT_WITH_COMMENTS

// ---------------------------------------------------------------------------

static void test_statements(void)
{
    Parse p;

    // 8.6: var^ is what creates a name; := on its own reassigns.
    LHAT_TEST("definition");
    parse_text(&p, "var^ x = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    LHAT_TEST("var^ also accepts the longer spelling");
    parse_text(&p, "var^ x := 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    LHAT_TEST("var^ with a type annotation");
    parse_text(&p, "var^ x : number^ = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.binding.targets->kind,
                      LHAT_NODE_PARAM);
    parse_dispose(&p);

    LHAT_TEST("var^ with a type annotation also accepts the longer spelling");
    parse_text(&p, "var^ x : number^ := 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.binding.targets->kind,
                      LHAT_NODE_PARAM);
    parse_dispose(&p);

    LHAT_TEST("multiple definition binds pairwise");
    parse_text(&p, "var^ a, b = 1, 2");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.targets), 2);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.values), 2);
    }
    parse_dispose(&p);

    // 7.3 and 8.6: reassignment puts the target first and is written
    // with :=, since var^ took over making names.
    LHAT_TEST("reassignment");
    parse_text(&p, "i := i + 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_REASSIGN);
    parse_dispose(&p);

    LHAT_TEST("swap is a multiple reassignment");
    parse_text(&p, "a, b := b, a");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_REASSIGN);
    parse_dispose(&p);

    // 7.4: 'a += b' is a reassignment whose value is 'a + b', built around
    // the same target node -- read once for its type/current value, never
    // an expression compiled twice.
    LHAT_TEST("compound assignment reads as a reassignment of a + b");
    parse_text(&p, "a += b");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_REASSIGN);
        LHAT_CHECK(s->v.binding.has_compound_op, "expected has_compound_op");
        LHAT_CHECK_EQ_INT(s->v.binding.compound_op, LHAT_OP_ADD);
        const LhatNode *value = s->v.binding.values;
        LHAT_CHECK_EQ_INT(value->kind, LHAT_NODE_BINARY);
        LHAT_CHECK_EQ_INT(value->v.binary.op, LHAT_OP_ADD);
        LHAT_CHECK(value->v.binary.left == s->v.binding.targets,
                   "expected the value's left to be the very target node");
    }
    parse_dispose(&p);

    LHAT_TEST("every compound spelling maps to its plain operator");
    {
        static const struct {
            const char *text;
            LhatOpKind op;
        } cases[] = {
            { "a += b", LHAT_OP_ADD },
            { "a -= b", LHAT_OP_SUB },
            { "a *= b", LHAT_OP_MUL },
            { "a /= b", LHAT_OP_DIV },
            { "a %= b", LHAT_OP_MOD },
            { "a //= b", LHAT_OP_FLOORDIV },
            { "a **= b", LHAT_OP_POW },
            { "a ..= b", LHAT_OP_CONCAT },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            parse_text(&p, cases[i].text);
            LHAT_CHECK_EQ_INT(error_count(&p), 0);
            LHAT_CHECK_EQ_INT(first_statement(&p)->v.binding.compound_op,
                              cases[i].op);
            parse_dispose(&p);
        }
    }

    // 7.4: several targets, each paired with the value at its own position
    // -- one BINARY per pair, the way a plain ':=' takes one value per target.
    LHAT_TEST("compound assignment takes several targets");
    parse_text(&p, "a, b += 1, 2");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_REASSIGN);
        LHAT_CHECK(s->v.binding.has_compound_op, "compound");
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.targets), 2);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.values), 2);
        LHAT_CHECK_EQ_INT(s->v.binding.values->kind, LHAT_NODE_BINARY);
        LHAT_CHECK_EQ_INT(s->v.binding.values->next->kind, LHAT_NODE_BINARY);
    }
    parse_dispose(&p);

    // Nothing spreads one value across several targets here: a compound
    // operator reads the target it writes back to.
    LHAT_TEST("but the two lists have to be the same length");
    parse_text(&p, "a, b += 1");
    LHAT_CHECK(error_count(&p) > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BINDING_ARITY);
    parse_dispose(&p);

    // 13.8改: several names take the values of one call apart, with no
    // mark at all -- 13.10 marked the table path, and both went together.
    LHAT_TEST("destructuring binding");
    parse_text(&p, "var^ q, r = divmod(7, 2)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.targets), 2);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.values), 1);
    }
    parse_dispose(&p);

    // 13.8改: one value with several names parses. Whether it is a
    // tuple being taken apart is a question about the type, so no mark is
    // demanded here and the checker answers instead.
    LHAT_TEST("one value with several names parses without a mark");
    parse_text(&p, "q, r := divmod(7, 2)");
    LHAT_CHECK_EQ_INT(p.result.diagnostic_count, 0);
    parse_dispose(&p);

    LHAT_TEST("but a count that cannot be either is still refused");
    parse_text(&p, "a, b := 1, 2, 3");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BINDING_ARITY);
    parse_dispose(&p);

    // The target list is the ordinary one, so a type may be written on it.
    LHAT_TEST("typed destructuring targets");
    parse_text(&p, "var^ q:number^, r:number^ = f()");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *target = first_statement(&p)->v.binding.targets;
        LHAT_CHECK_EQ_INT(target->kind, LHAT_NODE_PARAM);
        LHAT_CHECK(target->v.param.type != NULL, "the target should carry a type");
    }
    parse_dispose(&p);

    LHAT_TEST("a single definition may carry a type");
    parse_text(&p, "var^ x:number^ = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.binding.targets->kind,
                      LHAT_NODE_PARAM);
    parse_dispose(&p);

    LHAT_TEST("call statement");
    parse_text(&p, "print(\"hi\")");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    // 8.2: an expression that does nothing is not a statement.
    LHAT_TEST("a bare expression is not a statement");
    parse_text(&p, "a + b");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    // 8.2: except at the top level of interactive input, where working out
    // 2 + 3 has to be possible. It becomes the same expression statement a
    // call is, which 03 の 4.3 makes the value of the input when it is last.
    LHAT_TEST("but at an interactive top level it is one");
    parse_interactive_text(&p, "a + b");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    LHAT_TEST("and a name on its own is one too");
    parse_interactive_text(&p, "x");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    // 5.1: '{' opens the statement form of if^ and ':' the expression one,
    // which shows only after the condition. So the expression form reaches a
    // statement position and answers to 8.2 there like any other expression,
    // rather than being refused for wanting a brace.
    LHAT_TEST("the expression form of if^ stands where a statement does");
    parse_interactive_text(&p, "if^ true^: 1 el^: 2;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(s->v.jump.value->kind, LHAT_NODE_IF_EXPR);
    }
    parse_dispose(&p);

    LHAT_TEST("and the statement form is still the statement form");
    parse_interactive_text(&p, "if^ true^ { x := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_IF_STMT);
    parse_dispose(&p);

    // 16.1: a for^ takes whichever form its clause does, so the if^ of one
    // has both. 16.7 is not against it -- the focus still does not leave;
    // only the value built from it does, which is what an expression is.
    LHAT_TEST("the if^ of a for^ has an expression form too");
    parse_interactive_text(&p, "for^ var^ n = 5 if^ n > 1: n el^: 0 ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(s->v.jump.value->kind, LHAT_NODE_FOR);
        LHAT_CHECK(s->v.jump.value->v.loop.is_expression, "written with ':'");
    }
    parse_dispose(&p);

    LHAT_TEST("and its statement form is unaffected");
    parse_interactive_text(&p, "for^ var^ n = 5 if^ n > 1 { x := n }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_FOR);
    parse_dispose(&p);

    // 17.2's match had the same gap: its expression form parsed but could
    // not stand at a statement, so 8.2 never saw it.
    LHAT_TEST("and a match written as an expression stands there too");
    parse_interactive_text(&p, "for^ 2: when^ 1: \"x\" other^: \"y\" ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    LHAT_TEST("but in a file neither does");
    parse_text(&p, "for^ var^ n = 5 if^ n > 1: n el^: 0 ;");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    // In a file 8.2 holds as it always did: the writer meant braces.
    LHAT_TEST("but in a file the expression form is not a statement");
    parse_text(&p, "if^ true^: 1 el^: 2;");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    // 5.1: 'el^' takes a condition or nothing, so the value that follows one
    // written without its ':' is read as a condition and the ':' is missed
    // only at the ';'. That is where the parser noticed, not where the writer
    // has to look -- the marker is.
    LHAT_TEST("a missing ':' after el^ is reported at the marker");
    parse_text(&p, "var^ a = if^ 1 < 2: 1 el^ 2 ;");
    {
        LHAT_CHECK_EQ_INT(error_count(&p), 1);
        LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
        if (p.result.diagnostic_count > 0) {
            const LhatParseDiagnostic *d = &p.result.diagnostics[0];
            LHAT_CHECK_EQ_INT(d->code, LHAT_PARSE_ERR_ELSE_NEEDS_COLON);
            // 'el^' is at column 23 of the one line.
            LHAT_CHECK_EQ_INT((int)d->column, 23);
        }
    }
    parse_dispose(&p);

    // What was read is taken as the value, so the rest of the construct is
    // still readable and one mistake makes one diagnostic.
    LHAT_TEST("and the expression read is taken as the value");
    parse_text(&p, "var^ a = if^ 1 < 2: 1 el^ 2 ;");
    {
        const LhatNode *value = first_statement(&p)->v.binding.values;
        const LhatNode *clauses = value->v.list.items;
        LHAT_CHECK(clauses != NULL && clauses->next != NULL,
                   "expected two clauses");
        if (clauses != NULL && clauses->next != NULL) {
            const LhatNode *otherwise = clauses->next;
            LHAT_CHECK(otherwise->v.clause.condition == NULL,
                       "the else clause should carry no condition");
            LHAT_CHECK(otherwise->v.clause.body != NULL,
                       "the else clause should carry the value");
        }
    }
    parse_dispose(&p);

    // Only when nothing follows. A value after it means the clause really was
    // a further test, and then the ':' belongs after its condition.
    LHAT_TEST("but a further test still reports where the ':' goes");
    parse_text(&p, "var^ a = if^ 1 < 2: 1 el^ 3 < 4 2 el^: 3 ;");
    {
        LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
        if (p.result.diagnostic_count > 0) {
            LHAT_CHECK(p.result.diagnostics[0].code !=
                           LHAT_PARSE_ERR_ELSE_NEEDS_COLON,
                       "this one is not the missing-marker case");
        }
    }
    parse_dispose(&p);

    LHAT_TEST("and a well-formed chain is untouched");
    parse_text(&p, "var^ a = if^ 1 < 2: 1 el^ 3 < 4: 2 el^: 3 ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // Every expect_op knows the token it wanted, and the diagnostic carries
    // it -- "a different token" says nothing a reader can act on.
    LHAT_TEST("a diagnostic names the token it wanted");
    parse_text(&p, "g := f^ { if^ 1: 2 el^: 3 }");
    {
        LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
        if (p.result.diagnostic_count > 0) {
            const LhatParseDiagnostic *d = &p.result.diagnostics[0];
            LHAT_CHECK_EQ_INT(d->code, LHAT_PARSE_ERR_EXPECTED_TOKEN);
            LHAT_CHECK(d->has_expected, "and says what it was");
            LHAT_CHECK_EQ_INT(d->expected, LHAT_OP_SEMICOLON);

            // The token that was there instead is recorded for every
            // diagnostic, since it is at hand wherever one is made.
            LHAT_CHECK_EQ_INT(d->found, LHAT_TOKEN_OP);
            LHAT_CHECK_EQ_INT(d->found_op, LHAT_OP_RBRACE);
            LHAT_CHECK_EQ_INT(d->length, 1);

            char message[128];
            size_t needed =
                lhat_parse_message_write(d, message, sizeof message);
            LHAT_CHECK(needed < sizeof message, "it fits");
            LHAT_CHECK(strcmp(message,
                              "a ';' was expected here, and this is '}'") == 0,
                       "and the message names both");
        }
    }
    parse_dispose(&p);

    // A code about the token it met names that token, without any site
    // having to hand it over: report already had it.
    LHAT_TEST("and a code about a token names the one it met");
    parse_text(&p, "var^ 1 = 2");
    {
        LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
        if (p.result.diagnostic_count > 0) {
            const LhatParseDiagnostic *d = &p.result.diagnostics[0];
            LHAT_CHECK_EQ_INT(d->code, LHAT_PARSE_ERR_EXPECTED_NAME);
            char message[128];
            lhat_parse_message_write(d, message, sizeof message);
            LHAT_CHECK(strcmp(message, "expected a name, and this is a number")
                           == 0,
                       "it says what was there");
        }
    }
    parse_dispose(&p);

    // A code that knows nothing besides itself answers what it always did.
    LHAT_TEST("and one that names nothing keeps its own message");
    parse_text(&p, "1 + 2");
    {
        LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
        if (p.result.diagnostic_count > 0) {
            const LhatParseDiagnostic *d = &p.result.diagnostics[0];
            LHAT_CHECK(!d->has_expected, "nothing to name");

            char message[128];
            lhat_parse_message_write(d, message, sizeof message);
            LHAT_CHECK(strcmp(message, lhat_parse_error_message(d->code)) == 0,
                       "the code's own message");
        }
    }
    parse_dispose(&p);

    // Measuring and filling have to agree, or a caller sizing a buffer from
    // the first call gets a truncated second one.
    LHAT_TEST("and measuring agrees with filling");
    parse_text(&p, "g := f^ { if^ 1: 2 el^: 3 }");
    if (p.result.diagnostic_count > 0) {
        const LhatParseDiagnostic *d = &p.result.diagnostics[0];
        size_t needed = lhat_parse_message_write(d, NULL, 0);
        char *room = (char *)malloc(needed + 1);
        if (room != NULL) {
            LHAT_CHECK_EQ_INT(lhat_parse_message_write(d, room, needed + 1),
                              needed);
            LHAT_CHECK_EQ_INT(strlen(room), needed);
            free(room);
        }
    }
    parse_dispose(&p);

    // 15.12: a function whose body is one expression answers with it. Read as
    // an expression statement while parsing, since whether the body is one
    // statement shows only once it has ended, and turned into a return^ then.
    LHAT_TEST("a function whose body is one expression answers with it");
    parse_text(&p, "g := f^ -> number^ { 1 + 2 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_value(&p)->v.func.body->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_RETURN);
        LHAT_CHECK(is_binary(body->v.jump.value, LHAT_OP_ADD), "the expression");
    }
    parse_dispose(&p);

    // Narrow on purpose: everywhere it does not reach, 8.2 holds as it did.
    LHAT_TEST("but not when the body is more than one statement");
    parse_text(&p, "g := f^ -> number^ { var^ x = 1  x + 1 }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    LHAT_TEST("nor for a p^, which may answer nothing");
    parse_text(&p, "g := p^ -> number^ { 1 + 2 }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    LHAT_TEST("nor in a block nested inside the body");
    parse_text(&p, "g := f^ -> number^ { do^{ 1 + 2 } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    // A call is the answer too when it is the whole body. Whether it answers
    // anything is the checker's question, and 13.2 already refused a f^ whose
    // body was one call -- so this can only make a refused body work.
    LHAT_TEST("and a call that is the whole body is the answer");
    parse_text(&p, "g := f^ -> number^ { foo() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.func.body->v.list.items->kind,
                      LHAT_NODE_RETURN);
    parse_dispose(&p);

    // With another statement beside it, it is a call standing alone again:
    // that was always a statement and always meant "run this and drop it".
    LHAT_TEST("but with a statement beside it, it is a call statement");
    parse_text(&p, "g := f^ -> number^ { foo()  return^ 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.func.body->v.list.items->kind,
                      LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    LHAT_TEST("and a p^ body of one call still drops what it answers");
    parse_text(&p, "g := p^ { foo() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.func.body->v.list.items->kind,
                      LHAT_NODE_CALL_STMT);
    parse_dispose(&p);

    // 8.2 says the top level and nowhere else, so a block keeps the rule.
    LHAT_TEST("inside a block it is not, even interactively");
    parse_interactive_text(&p, "do^{ a + b }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    LHAT_TEST("nor inside a body");
    parse_interactive_text(&p, "var^ f = p^ { a + b }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    // 8.6's warning about '=' is for a file, where the line can only be a
    // mistake. At a prompt it is a comparison being worked out, which has to
    // stay possible.
    LHAT_TEST("and '=' is a comparison to work out, not a mistake");
    parse_interactive_text(&p, "x = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 2.1: juxtaposition is only a call in command mode.
    LHAT_TEST("juxtaposition is rejected with a suggestion");
    parse_text(&p, "foo 1 2 3");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_JUXTAPOSITION);
    parse_dispose(&p);

    // 2.1 decides what follows a call by asking whether it could begin a
    // statement. A hat identifier that names a value can, so it must not be
    // swallowed as one more argument to the line above.
    LHAT_TEST("self^ begins a statement after a call");
    parse_text(&p, "var^ T = def^{ self^{ n := 0 }, a := p^self^ { },\n"
                   "  s := p^self^ { self^.a()\nself^.n := 1 } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("it^ begins a statement after a call");
    parse_text(&p, "for^ 1 to^ 3 { foo()\nit^.bar() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("L^ begins a statement after a call");
    parse_text(&p, "foo()\nL^.collectgarbage()");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("this^ begins a statement after a call");
    parse_text(&p, "var^ f = p^ { foo()\nthis^() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 8.6: the accident this replaced. Without var^, ':=' inside a nested
    // scope reassigns rather than quietly shadowing.
    LHAT_TEST("':=' in a nested scope reassigns");
    parse_text(&p, "var^ i = 0\nif^ foo() { i := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *inner =
            first_statement(&p)->next->v.list.items->v.clause.body->v.list.items;
        LHAT_CHECK_EQ_INT(inner->kind, LHAT_NODE_REASSIGN);
    }
    parse_dispose(&p);

    LHAT_TEST("shadowing has to be written out");
    parse_text(&p, "var^ i = 0\nif^ foo() { var^ i = 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *inner =
            first_statement(&p)->next->v.list.items->v.clause.body->v.list.items;
        LHAT_CHECK_EQ_INT(inner->kind, LHAT_NODE_DEFINE);
    }
    parse_dispose(&p);

    // 8.7: mutual recursion is handled by scope-wide visibility, so a
    // declaration without a value has no job to do.
    LHAT_TEST("var^ needs a value");
    parse_text(&p, "var^ x : number^");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_LET_NEEDS_VALUE);
    parse_dispose(&p);

    // 14.14改2: every brace introduces, so all five read '=' as well as ':='
    // and the two mean the same thing. Before this they disagreed three ways.
    LHAT_TEST("every brace reads both spellings");
    parse_text(&p,
               "var^ t = { a = 1, [2] = 2 }\n"
               "var^ u = { a := 1, [2] := 2 }\n"
               "var^ P = def^{ self^{ w = 5 }, m = p^ { } }\n"
               "var^ Q = def^{ self^{ w := 5 }, m := p^ { } }\n"
               "errordef^ E { K { line : number^ = 0 } }\n"
               "errordef^ F { K { line : number^ := 0 } }\n"
               "var^ e = error^E.K{ line = 3 }\n"
               "var^ g = error^E.K{ line := 3 }\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 14.14改2: and a name written twice is the later one, in either spelling
    // -- what a mechanically built literal relies on.
    LHAT_TEST("and a repeated key is not an error");
    parse_text(&p, "var^ t = { a = 1, a = 2 }\nvar^ u = { a := 1, a := 2 }\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 8.9: let^ binds the same way var^ does and leaves a name behind that
    // nothing reassigns. The word is the only difference, and it is recorded
    // on the node the two share.
    LHAT_TEST("let^ and var^ both make a definition");
    parse_text(&p, "var^ a = 1\nlet^ b = 2\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *first = first_statement(&p);
        LHAT_CHECK_EQ_INT(first->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(!first->v.binding.immutable, "var^ leaves it writable");
        LHAT_CHECK_EQ_INT(first->next->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(first->next->v.binding.immutable, "let^ does not");
    }
    parse_dispose(&p);

    // 8.9: 8.6 made '=' and ':=' the same word after an introducer. let^ takes
    // only the one that does not also mean reassign.
    LHAT_TEST("let^ refuses ':='");
    parse_text(&p, "let^ x := 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_LET_NEEDS_EQUALS);
    parse_dispose(&p);

    LHAT_TEST("while var^ still takes it");
    parse_text(&p, "var^ x := 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 8.9: 8.8's member introduction stays var^'s, so that a name let^ bound
    // is one nothing may reassign.
    LHAT_TEST("let^ binds a name and not a path");
    parse_text(&p, "let^ t.a = 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_LET_NEEDS_NAME);
    parse_dispose(&p);

    LHAT_TEST("while var^ introduces one");
    parse_text(&p, "var^ t.a = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 12 章: with^ is a let^, so the spelling that also means reassign is not
    // one it accepts either.
    LHAT_TEST("with^ refuses ':=' too");
    parse_text(&p, "with^ r := open(\"d\")\n{ }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_LET_NEEDS_EQUALS);
    parse_dispose(&p);

    // 16.3改 with 8.9: either word introduces a focus, and which one was
    // written is carried through on the binding.
    LHAT_TEST("a for^ focus takes either word");
    parse_text(&p, "for^ i from^ 1 to^ 3 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(focus->v.binding.immutable, "let^ focus");
    }
    parse_dispose(&p);

    LHAT_TEST("and var^ leaves it writable");
    parse_text(&p, "for^ var^ i = 1 while^ i < 3 next^ i := i + 1 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(!focus->v.binding.immutable, "var^ focus");
    }
    parse_dispose(&p);

    // 8.6: '=' compares, so this reaches the statement level as an
    // expression. The C habit is common enough for its own message.
    LHAT_TEST("'x = 1' names both intentions");
    parse_text(&p, "x = 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_EQUALS_IS_COMPARISON);
    parse_dispose(&p);

    // 8.6: the enclosing construct is the introducer, so ':=' still defines
    // inside for^, with^ and a brace list.
    LHAT_TEST("an introducer keeps ':=' a definition");
    parse_text(&p, "for^ k from^ 1 to^ 3 { print(k) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.loop.focus->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    // 01 の 7.3: the postfix form is not the language's, and the parser
    // points at the prefix one.
    LHAT_TEST("postfix reassignment reports what to write instead");
    parse_text(&p, "i + 1 -> i");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_WITHDRAWN_ARROW);
    parse_dispose(&p);

    LHAT_TEST("'<<' after a target reports that it is reserved");
    parse_text(&p, "a << b");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_RESERVED_SHIFT);
    parse_dispose(&p);

    LHAT_TEST("'>>' where a value should begin shares the shift diagnostic");
    parse_text(&p, "var^ x = a >> 2");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_RESERVED_SHIFT);
    parse_dispose(&p);

    // 02 の 11.5: recorded, not supported -- the message says so rather
    // than calling '@' a stray character.
    LHAT_TEST("'@' reports that the notation is not supported yet");
    parse_text(&p, "var^ x = @(1)");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_UNSUPPORTED_AT);
    parse_dispose(&p);

    LHAT_TEST("do^ block");
    parse_text(&p, "do^{ x := 1 y := 2 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_BLOCK);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.list.items), 2);
    }
    parse_dispose(&p);

    LHAT_TEST("return, break and yield");
    parse_text(&p, "do^{ return^ 1 break^ yield^ 2 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_RETURN);
        LHAT_CHECK_EQ_INT(body->next->kind, LHAT_NODE_BREAK);
        LHAT_CHECK_EQ_INT(body->next->next->kind, LHAT_NODE_YIELD);
    }
    parse_dispose(&p);

    // 01 の 10.9: the operand has to be on the same line, the same rule that
    // keeps a '(' at the start of a line from continuing the line above. An
    // ordinary name begins a statement just as well as an expression, so
    // nothing else could tell these apart.
    LHAT_TEST("a jump does not swallow the statement below it");
    parse_text(&p, "do^{ yield^\nx := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_YIELD);
        LHAT_CHECK(body->v.jump.value == NULL, "yield^ sent nothing");
        LHAT_CHECK(body->next != NULL, "the assignment is its own statement");
        if (body->next != NULL) {
            LHAT_CHECK_EQ_INT(body->next->kind, LHAT_NODE_REASSIGN);
        }
    }
    parse_dispose(&p);

    LHAT_TEST("and return^ does not either");
    parse_text(&p, "do^{ return^\nx := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_RETURN);
        LHAT_CHECK(body->v.jump.value == NULL, "return^ answered nothing");
        LHAT_CHECK(body->next != NULL, "the assignment is its own statement");
    }
    parse_dispose(&p);

    LHAT_TEST("an operand on the same line is still taken");
    parse_text(&p, "do^{ return^ x\ny := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_RETURN);
        LHAT_CHECK(body->v.jump.value != NULL, "return^ answered x");
        LHAT_CHECK(body->next != NULL, "and y := 1 stayed separate");
    }
    parse_dispose(&p);

    // 12.1: with^ takes local definitions and one block.
    LHAT_TEST("with^");
    parse_text(&p, "with^ r = open(\"d\")\nwith^ w = create(\"o\")\n{ copy(r, w) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_WITH);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.list.items), 2);
        LHAT_CHECK(s->v.list.extra != NULL, "with^ should have a body");
    }
    parse_dispose(&p);

    // 8.6: with^ is its own introducer, so '=' defines exactly as ':=' does
    // -- ':=' is the accepted spelling, not the only one.
    LHAT_TEST("with^ also accepts '=', and a type annotation");
    parse_text(&p, "with^ h:table^ = open(\"d\")\n{ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_WITH);
        LHAT_CHECK_EQ_INT(s->v.list.items->kind, LHAT_NODE_DEFINE);
    }
    parse_dispose(&p);
}

static void test_precedence(void)
{
    Parse p;

    LHAT_TEST("multiplication binds tighter than addition");
    parse_text(&p, "x := a + b * c");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_ADD), "top should be +");
        LHAT_CHECK(is_binary(e->v.binary.right, LHAT_OP_MUL), "right should be *");
    }
    parse_dispose(&p);

    // 11.5 の (1): '..' sits below '+' and associates to the right.
    LHAT_TEST("concatenation is below addition");
    parse_text(&p, "x := \"n = \" .. a + b");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_CONCAT), "top should be ..");
        LHAT_CHECK(is_binary(e->v.binary.right, LHAT_OP_ADD), "right should be +");
    }
    parse_dispose(&p);

    LHAT_TEST("concatenation is right associative");
    parse_text(&p, "x := a .. b .. c");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_CONCAT), "top should be ..");
        LHAT_CHECK(is_binary(e->v.binary.right, LHAT_OP_CONCAT),
                   "the right operand should be the nested ..");
    }
    parse_dispose(&p);

    LHAT_TEST("addition is left associative");
    parse_text(&p, "x := a + b + c");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_ADD), "top should be +");
        LHAT_CHECK(is_binary(e->v.binary.left, LHAT_OP_ADD),
                   "the left operand should be the nested +");
    }
    parse_dispose(&p);

    // 11.5 の (2): '-2 ** 2' is -(2 ** 2), as in mathematics.
    LHAT_TEST("exponentiation binds tighter than unary minus");
    parse_text(&p, "x := -2 ** 2");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_UNARY);
        LHAT_CHECK(is_binary(e->v.unary.operand, LHAT_OP_POW),
                   "the operand of - should be **");
    }
    parse_dispose(&p);

    LHAT_TEST("the right operand of ** may be unary");
    parse_text(&p, "x := 2 ** -1");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_POW), "top should be **");
        LHAT_CHECK_EQ_INT(e->v.binary.right->kind, LHAT_NODE_UNARY);
    }
    parse_dispose(&p);

    LHAT_TEST("exponentiation is right associative");
    parse_text(&p, "x := 2 ** 3 ** 2");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_POW), "top should be **");
        LHAT_CHECK(is_binary(e->v.binary.right, LHAT_OP_POW),
                   "the right operand should be the nested **");
    }
    parse_dispose(&p);

    LHAT_TEST("and^ binds tighter than or^");
    parse_text(&p, "x := a or^ b and^ c");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_OR), "top should be or^");
        LHAT_CHECK(is_binary(e->v.binary.right, LHAT_OP_AND),
                   "right should be and^");
    }
    parse_dispose(&p);

    LHAT_TEST("comparison is below arithmetic");
    parse_text(&p, "x := a + b < c");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK(is_binary(e, LHAT_OP_LT), "top should be <");
        LHAT_CHECK(is_binary(e->v.binary.left, LHAT_OP_ADD), "left should be +");
    }
    parse_dispose(&p);
}

// 11.5 の (5).
static void test_comparison_chain(void)
{
    Parse p;

    LHAT_TEST("a single comparison stays a binary node");
    parse_text(&p, "x := a < b");
    LHAT_CHECK(is_binary(first_value(&p), LHAT_OP_LT), "expected <");
    parse_dispose(&p);

    LHAT_TEST("1 <= x <= 10 becomes one chain");
    parse_text(&p, "x := 1 \xE2\x89\xA6 y \xE2\x89\xA6 10");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_COMPARE_CHAIN);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.chain.operands), 3);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.chain.operators), 2);
    }
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("mixed comparison operators chain too");
    parse_text(&p, "x := a < b = c");
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_COMPARE_CHAIN);
    parse_dispose(&p);

    LHAT_TEST("isa^ participates in a chain");
    parse_text(&p, "x := a isa^ b");
    LHAT_CHECK(is_binary(first_value(&p), LHAT_OP_ISA), "expected isa^");
    parse_dispose(&p);

    // 11.6改: is^ moved to identity, an ordinary value on both sides -- it
    // reads like '=' rather than isa^'s type on the right.
    LHAT_TEST("is^ takes a value on the right, and chains too");
    parse_text(&p, "x := a is^ b");
    LHAT_CHECK(is_binary(first_value(&p), LHAT_OP_IS), "expected is^");
    parse_dispose(&p);

    LHAT_TEST("is^ chains with other comparisons");
    parse_text(&p, "x := a is^ b = c");
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_COMPARE_CHAIN);
    parse_dispose(&p);
}

static void test_postfix(void)
{
    Parse p;

    LHAT_TEST("member, index and call chain together");
    parse_text(&p, "x := a.b[1].c(2)");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_CALL);
        LHAT_CHECK_EQ_INT(e->v.access.target->kind, LHAT_NODE_MEMBER);
    }
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 01 の 10.1: 'a.1.1' is a chain of integer keys.
    LHAT_TEST("a.1.1 is two integer keys");
    parse_text(&p, "x := a.1.1");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(e->v.access.argument->kind, LHAT_NODE_INT);
        LHAT_CHECK_EQ_INT(e->v.access.target->kind, LHAT_NODE_MEMBER);
    }
    parse_dispose(&p);

    LHAT_TEST("nil propagation");
    parse_text(&p, "x := a?.b?(1)");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_CALL);
        LHAT_CHECK(e->v.access.nil_safe, "the call should be nil safe");
        LHAT_CHECK(e->v.access.target->v.access.nil_safe,
                   "the member access should be nil safe");
    }
    parse_dispose(&p);

    // 01 の 10.9: a '(' after a newline starts a statement.
    LHAT_TEST("a call paren must sit on the same line");
    parse_text(&p, "update()\n(f or^ g)()");
    {
        const LhatNode *statements = first_statement(&p);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(statements), 2);
        LHAT_CHECK_EQ_INT(statements->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(statements->next->kind, LHAT_NODE_CALL_STMT);
    }
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("on one line it is a single call");
    parse_text(&p, "x := update()(f)");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_CALL);
        LHAT_CHECK_EQ_INT(e->v.access.target->kind, LHAT_NODE_CALL);
    }
    parse_dispose(&p);

    // 13.7: '...' bare, as a call argument, forwards the collected tail.
    LHAT_TEST("'...' alone in an argument list is a spread");
    parse_text(&p, "x := sum(...)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *call = first_value(&p);
        LHAT_CHECK_EQ_INT(call->v.access.argument->kind, LHAT_NODE_SPREAD);
    }
    parse_dispose(&p);

    LHAT_TEST("and it may follow fixed arguments");
    parse_text(&p, "x := sum(1, 2, ...)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *first = first_value(&p)->v.access.argument;
        LHAT_CHECK_EQ_INT(lhat_node_list_length(first), 3);
        LHAT_CHECK_EQ_INT(first->next->next->kind, LHAT_NODE_SPREAD);
    }
    parse_dispose(&p);

    LHAT_TEST("but nothing may follow it");
    parse_text(&p, "x := sum(..., 1)");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_SPREAD_NOT_LAST);
    parse_dispose(&p);
}

static void test_literals(void)
{
    Parse p;

    LHAT_TEST("table literal with keys and positional entries");
    parse_text(&p, "t := { a := 1, b := 2, 3 }");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_TABLE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.list.items), 3);
        LHAT_CHECK(e->v.list.items->v.entry.key != NULL, "first entry is keyed");
        LHAT_CHECK(e->v.list.items->next->next->v.entry.key == NULL,
                   "third entry is positional");
    }
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("name literal as a key");
    parse_text(&p, "t := { `a b` := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.list.items->v.entry.key->kind,
                      LHAT_NODE_NAME);
    parse_dispose(&p);

    // 14.14改: an entry introduces a member, and 8.6 spells introducing '='.
    // ':=' is the older spelling and still reads.
    LHAT_TEST("an entry is written with '='");
    parse_text(&p, "t := { a = 1, b = 2, 3 }");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(error_count(&p), 0);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.list.items), 3);
        LHAT_CHECK(e->v.list.items->v.entry.key != NULL, "first entry is keyed");
        LHAT_CHECK(e->v.list.items->next->v.entry.key != NULL,
                   "second entry is keyed");
        LHAT_CHECK(e->v.list.items->next->next->v.entry.key == NULL,
                   "third entry is positional");
    }
    parse_dispose(&p);

    LHAT_TEST("and the two spellings mix");
    parse_text(&p, "t := { a = 1, b := 2 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.list.items->next->v.entry.key != NULL,
               "':=' is still a key");
    parse_dispose(&p);

    // What '=' costs: a comparison standing as a positional entry has to say
    // so. '(' does not begin a name, so it never reaches the test.
    LHAT_TEST("but a comparison written in brackets stays one");
    parse_text(&p, "t := { (a = 1), (b = 2) }");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(error_count(&p), 0);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.list.items), 2);
        LHAT_CHECK(e->v.list.items->v.entry.key == NULL, "positional");
        LHAT_CHECK(is_binary(e->v.list.items->v.entry.value, LHAT_OP_EQ),
                   "and it is the comparison");
    }
    parse_dispose(&p);

    LHAT_TEST("a computed key takes '=' too");
    parse_text(&p, "t := { [k + 1] = 9 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.list.items->v.entry.computed,
               "the key is an expression");
    parse_dispose(&p);

    // 14.6: the field template is the same brace syntax, and declaring a
    // field is introducing one.
    LHAT_TEST("and so does a field of self^");
    parse_text(&p, "P := def^{ self^{ w = 5 } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 5.4: the holes hold ordinary expressions.
    LHAT_TEST("string interpolation");
    parse_text(&p, "s := $\"hi {name}! {n:2.4}\"");
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_INTERP);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.list.items), 4);
    }
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("a format specifier is attached to its hole");
    parse_text(&p, "s := $\"{n:2.4}\"");
    {
        const LhatNode *hole = first_value(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(hole->kind, LHAT_NODE_INTERP_HOLE);
        LHAT_CHECK(hole->v.hole.format != NULL, "expected a format");
    }
    parse_dispose(&p);

    LHAT_TEST("scope specifier");
    parse_text(&p, "x := $Counter");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);
}

static void test_functions(void)
{
    Parse p;

    LHAT_TEST("function with parameters, types and defaults");
    parse_text(&p, "f := f^a:number^, b:number^=2 -> string^ { return^ \"x\" }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *fn = first_value(&p);
        LHAT_CHECK_EQ_INT(fn->kind, LHAT_NODE_FUNC);
        LHAT_CHECK(fn->v.func.is_function, "f^ is a function");
        LHAT_CHECK_EQ_INT(lhat_node_list_length(fn->v.func.params), 2);
        LHAT_CHECK(fn->v.func.return_type != NULL, "expected a return type");
        LHAT_CHECK(fn->v.func.params->next->v.param.fallback != NULL,
                   "the second parameter has a default");
    }
    parse_dispose(&p);

    LHAT_TEST("a procedure returning nothing omits the arrow");
    parse_text(&p, "g := p^x:number^ { print(x) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *fn = first_value(&p);
        LHAT_CHECK(!fn->v.func.is_function, "p^ is a procedure");
        LHAT_CHECK(fn->v.func.return_type == NULL, "no return type");
    }
    parse_dispose(&p);

    // 13.7: '...' goes where a name goes.
    LHAT_TEST("variadic parameter");
    parse_text(&p, "g := p^ ...:number^ { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.func.params->v.param.variadic,
               "the parameter should be variadic");
    parse_dispose(&p);

    LHAT_TEST("an untyped variadic is accepted");
    parse_text(&p, "g := p^ ... { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 15.2: yieldability is inferred from the body, with no separate marker.
    LHAT_TEST("yieldable is inferred");
    parse_text(&p, "g := p^ { yield^ 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.func.yields, "the procedure should yield");
    parse_dispose(&p);

    LHAT_TEST("a nested definition keeps its own yield");
    parse_text(&p, "g := p^ { h := p^ { yield^ 1 } }");
    LHAT_CHECK(!first_value(&p)->v.func.yields,
               "the outer procedure does not yield");
    parse_dispose(&p);

    // 05 の 5.5: a require^ standing alone is a statement of its own, so
    // 8.2's "a bare expression is not a statement" is untouched.
    LHAT_TEST("a require^ on its own is a statement");
    parse_text(&p, "require^ \"lib/m.lh\"");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_REQUIRE_STMT);
    parse_dispose(&p);

    LHAT_TEST("and one with a var^ is still an expression");
    parse_text(&p, "var^ m = require^ \"lib/m.lh\"");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_REQUIRE);
    parse_dispose(&p);

    // 8.8: a var^ target may name a member. The path is the same node an
    // expression uses, so nothing new is read here.
    LHAT_TEST("a var^ target may be a path");
    parse_text(&p, "var^ a.b.c = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *target = first_statement(&p)->v.binding.targets;
        LHAT_CHECK_EQ_INT(target->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(target->v.access.target->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(target->v.access.target->v.access.target->kind,
                          LHAT_NODE_IDENT);
    }
    parse_dispose(&p);

    LHAT_TEST("and it may still be annotated");
    parse_text(&p, "var^ a.b : number^ = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *target = first_statement(&p)->v.binding.targets;
        LHAT_CHECK_EQ_INT(target->kind, LHAT_NODE_PARAM);
        LHAT_CHECK_EQ_INT(target->v.param.name->kind, LHAT_NODE_MEMBER);
    }
    parse_dispose(&p);

    // 15.11: _yield^ is the same node with a flag, so nothing downstream can
    // tell the two apart until the code is written out.
    LHAT_TEST("_yield^ makes a body yieldable too");
    parse_text(&p, "g := p^ { _yield^ 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.func.yields, "the procedure should yield");
    {
        const LhatNode *body = first_value(&p)->v.func.body->v.list.items;
        LHAT_CHECK_EQ_INT(body->kind, LHAT_NODE_YIELD);
        LHAT_CHECK(body->v.jump.phantom, "the yield is marked phantom");
    }
    parse_dispose(&p);

    LHAT_TEST("and it is written the same way where a yield^ takes a value");
    parse_text(&p, "g := p^ { var^ a : string^ = _yield^ 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.func.yields, "the procedure should yield");
    {
        const LhatNode *body = first_value(&p)->v.func.body->v.list.items;
        const LhatNode *sent = body->v.binding.values;
        LHAT_CHECK_EQ_INT(sent->kind, LHAT_NODE_YIELD);
        LHAT_CHECK(sent->v.jump.phantom, "the yield is marked phantom");
    }
    parse_dispose(&p);

    LHAT_TEST("a plain yield^ is not phantom");
    parse_text(&p, "g := p^ { yield^ 1 }");
    LHAT_CHECK(!first_value(&p)->v.func.body->v.list.items->v.jump.phantom,
               "a yield^ suspends");
    parse_dispose(&p);
}

static void test_conditionals(void)
{
    Parse p;

    // 5.2: the clauses live inside the braces.
    LHAT_TEST("if statement with clauses");
    parse_text(&p,
               "if^ x > 0 {\n"
               "    print(1)\n"
               "elseif^ x = 0:\n"
               "    print(2)\n"
               "else^:\n"
               "    print(3)\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_IF_STMT);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.list.items), 3);
        LHAT_CHECK(s->v.list.items->next->next->v.clause.condition == NULL,
                   "the last clause has no condition");
    }
    parse_dispose(&p);

    // 5.1: one marker, el^, tells else-if from else by the condition.
    LHAT_TEST("if expression with el^");
    parse_text(&p, "r := if^ a: 1 el^ b: 2 el^: 3 ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_IF_EXPR);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.list.items), 3);
        LHAT_CHECK(e->v.list.items->next->v.clause.condition != NULL,
                   "the middle clause is an else-if");
        LHAT_CHECK(e->v.list.items->next->next->v.clause.condition == NULL,
                   "the last clause is a plain else");
    }
    parse_dispose(&p);

    // 6.2: a ':' opens the construct and a ';' closes it.
    LHAT_TEST("an if expression must be closed");
    parse_text(&p, "r := if^ a: 1 el^: 2");
    LHAT_CHECK(error_count(&p) > 0, "expected a diagnostic");
    parse_dispose(&p);
}

static void test_types(void)
{
    Parse p;

    LHAT_TEST("function type");
    parse_text(&p, "x := y as^ f^number^, number^ -> string^ ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *as = first_value(&p);
        LHAT_CHECK_EQ_INT(as->kind, LHAT_NODE_AS);
        LHAT_CHECK_EQ_INT(as->v.ascription.type->kind, LHAT_NODE_TYPE_FUNC);
        LHAT_CHECK_EQ_INT(
            lhat_node_list_length(as->v.ascription.type->v.func.params), 2);
    }
    parse_dispose(&p);

    // 13.2: the arrow is dropped when nothing is returned.
    LHAT_TEST("a type with no return value");
    parse_text(&p, "x := y as^ p^number^;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.ascription.type->v.func.return_type == NULL,
               "no return type");
    parse_dispose(&p);

    // 13.3: nesting is why the ';' cannot be dropped.
    LHAT_TEST("nested signature");
    parse_text(&p, "x := y as^ p^number^, p^number^ -> number^ ;, number^ -> number^ ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(
        lhat_node_list_length(first_value(&p)->v.ascription.type->v.func.params), 3);
    parse_dispose(&p);

    // 11.5 の (3): '|' is the union, and it only appears in a type.
    LHAT_TEST("union type");
    parse_text(&p, "x := y as^ number^|nil^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.ascription.type->kind,
                      LHAT_NODE_TYPE_UNION);
    parse_dispose(&p);

    // 14.5: '&' binds tighter than '|', as in TypeScript.
    LHAT_TEST("intersection binds tighter than union");
    parse_text(&p, "x := y as^ a^|b^&c^");
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_UNION);
        LHAT_CHECK_EQ_INT(t->v.binary.right->kind, LHAT_NODE_TYPE_INTERSECT);
    }
    parse_dispose(&p);

    // 14.10: the structural type that with^ needs.
    LHAT_TEST("structural type");
    parse_text(&p, "x := y as^ t^{ dispose : p^self^; }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_TABLE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(t->v.list.items), 1);
    }
    parse_dispose(&p);

    // 14.10: bare, with nothing listed, t^ is the top of tables -- an
    // ordinary type name rather than a structure with no members.
    LHAT_TEST("bare table type");
    parse_text(&p, "x := y as^ t^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.ascription.type->kind,
                      LHAT_NODE_TYPE_NAME);
    parse_dispose(&p);

    LHAT_TEST("and table^ is the same word");
    parse_text(&p, "x := y as^ table^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.ascription.type->kind,
                      LHAT_NODE_TYPE_NAME);
    parse_dispose(&p);

    LHAT_TEST("a bare one takes part in a union like any other name");
    parse_text(&p, "x := y as^ table^|nil^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_UNION);
        LHAT_CHECK_EQ_INT(t->v.binary.left->kind, LHAT_NODE_TYPE_NAME);
    }
    parse_dispose(&p);

    LHAT_TEST("and a listed one is still a structure");
    parse_text(&p, "x := y as^ t^{}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.ascription.type->kind,
                      LHAT_NODE_TYPE_TABLE);
    parse_dispose(&p);

    // 14.10: an entry with no 'name :' in front of it is the type of the
    // next position. One token of lookahead tells the two apart.
    LHAT_TEST("types listed on their own are positions");
    parse_text(&p, "x := y as^ t^{ number^, string^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_TABLE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(t->v.list.items), 2);
        LHAT_CHECK(t->v.list.items->v.entry.key == NULL, "no name on a position");
    }
    parse_dispose(&p);

    LHAT_TEST("and a name with a ':' is still a member");
    parse_text(&p, "x := y as^ t^{ a : number^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_value(&p)->v.ascription.type->v.list.items->v.entry.key
                   != NULL,
               "a member keeps its name");
    parse_dispose(&p);

    LHAT_TEST("and the two mix");
    parse_text(&p, "x := y as^ t^{ number^, a : string^, t^{} }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(
        lhat_node_list_length(
            first_value(&p)->v.ascription.type->v.list.items),
        3);
    parse_dispose(&p);

    // 13.7, 14.10: the sequence half may end in a variadic tail, the same
    // marker a parameter list ends in.
    LHAT_TEST("a table type's tail may be variadic");
    parse_text(&p, "x := y as^ t^{ ...:number^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK(t->v.list.items->v.entry.variadic, "marked variadic");
        LHAT_CHECK(t->v.list.items->v.entry.value != NULL, "has an element type");
    }
    parse_dispose(&p);

    LHAT_TEST("and an untyped one is still accepted");
    parse_text(&p, "x := y as^ t^{ ... }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(
        first_value(&p)->v.ascription.type->v.list.items->v.entry.variadic,
        "marked variadic with no written type");
    parse_dispose(&p);

    LHAT_TEST("and it may follow fixed positions");
    parse_text(&p, "x := y as^ t^{ number^, ...:string^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(
        lhat_node_list_length(
            first_value(&p)->v.ascription.type->v.list.items),
        2);
    parse_dispose(&p);

    // 13.9 with 15.3改: the front half is the signature one resume follows,
    // and it carries the kind of the body.
    LHAT_TEST("coroutine type");
    parse_text(&p, "x := y as^ c^{ p^number^ -> string^;, nil^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_CORO);
        LHAT_CHECK(t->v.coroutine.receive != NULL, "receive type");
        LHAT_CHECK(t->v.coroutine.produce != NULL, "produce type");
        LHAT_CHECK(t->v.coroutine.result != NULL, "result type");
        LHAT_CHECK(!t->v.coroutine.is_function, "a p^ coroutine");
    }
    parse_dispose(&p);

    LHAT_TEST("and an f^ one says so");
    parse_text(&p, "x := y as^ c^{ f^ -> string^;, nil^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_CORO);
        LHAT_CHECK(t->v.coroutine.is_function, "an f^ coroutine");
        // 15.2: nothing is sent to this one, which is written by leaving
        // the parameter list empty rather than by naming nil^.
        LHAT_CHECK(t->v.coroutine.receive == NULL, "no receive type");
        LHAT_CHECK(t->v.coroutine.produce != NULL, "produce type");
    }
    parse_dispose(&p);

    // 01 の 7.6: '::' is not the language's, and the parser points at '->'.
    LHAT_TEST(":: reports what to write instead");
    parse_text(&p, "x := y as^ f^number^ :: string^ ;");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_WITHDRAWN_COLONCOLON);
    parse_dispose(&p);
}

// 16 章. for^ names the focused value; iterating is one thing that may be
// done with it, not the meaning of the word.
static void test_loops(void)
{
    Parse p;

    LHAT_TEST("numeric iteration");
    parse_text(&p, "for^ i from^ 1 to^ 10 { print(i) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_FOR);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_TO);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.loop.focus), 1);
        LHAT_CHECK_EQ_INT(s->v.loop.focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(s->v.loop.step == NULL, "no step given");
    }
    parse_dispose(&p);

    LHAT_TEST("step and downto");
    parse_text(&p, "for^ i from^ 10 downto^ 1 step^ 2 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_DOWNTO);
        LHAT_CHECK(s->v.loop.step != NULL, "expected a step");
    }
    parse_dispose(&p);

    // 16.3改2: from^ opens the counted range, and the name it introduces is a
    // let^ -- nothing in the source advances it.
    LHAT_TEST("from^ introduces the focus of a counted loop");
    parse_text(&p, "for^ i from^ 1 to^ 10 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK(focus->v.binding.immutable, "from^ binds a let^");
        LHAT_CHECK_EQ_INT(first_statement(&p)->v.loop.kind, LHAT_FOR_TO);
    }
    parse_dispose(&p);

    LHAT_TEST("and with downto^ and step^ too");
    parse_text(&p, "for^ i from^ 10 downto^ 1 step^ 2 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_DOWNTO);
        LHAT_CHECK(s->v.loop.step != NULL, "expected a step");
        LHAT_CHECK(s->v.loop.focus->v.binding.immutable, "still a let^");
    }
    parse_dispose(&p);

    // The annotation of 16.3 sits where it always did, before the introducer.
    LHAT_TEST("and takes a type annotation");
    parse_text(&p, "for^ i:number^ from^ 1 to^ 10 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 16.3改2: a counted loop advances its own focus, so the header names no
    // introducer -- writing one would say the source moves it.
    LHAT_TEST("an introducer is refused on a counted loop");
    parse_text(&p, "for^ let^ i = 1 to^ 10 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FOCUS_NEEDS_FROM);
    parse_dispose(&p);

    LHAT_TEST("var^ is refused there as well");
    parse_text(&p, "for^ var^ i = 1 to^ 10 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FOCUS_NEEDS_FROM);
    parse_dispose(&p);

    // 16.3改2: and so is a bare ':='. If neither word may name a focus the
    // machine drives, neither may an outer name be handed to it.
    LHAT_TEST("and a bare ':=' is refused there too");
    parse_text(&p, "for^ i := 1 to^ 10 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FOCUS_NEEDS_FROM);
    parse_dispose(&p);

    // 16.3改: counting an outer name is what the conditional form is for --
    // there the source says what it does to the name.
    LHAT_TEST("while^ is where a bare ':=' counts an existing name");
    parse_text(&p, "for^ i := 1 while^ i < 10 next^ i := i + 1 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_REASSIGN);
    }
    parse_dispose(&p);

    // 16.3改2: and from^ belongs to the counted forms and nowhere else.
    LHAT_TEST("from^ is refused on a conditional loop");
    parse_text(&p, "for^ i from^ 1 while^ i < 3 next^ i := i + 1 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FROM_NOT_HERE);
    parse_dispose(&p);

    LHAT_TEST("iteration through an iterator");
    parse_text(&p, "for^ k, v in^ t { print(k) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_IN);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.loop.focus), 2);
    }
    parse_dispose(&p);

    LHAT_TEST("conditional iteration with next^");
    parse_text(&p, "for^ var^ i = 1 while^ i < 10 next^ i := i + 1 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_WHILE);
        LHAT_CHECK(s->v.loop.advance != NULL, "expected a next^ statement");
        LHAT_CHECK_EQ_INT(s->v.loop.advance->kind, LHAT_NODE_REASSIGN);
    }
    parse_dispose(&p);

    LHAT_TEST("until^ is the negated form");
    parse_text(&p, "for^ var^ i := 1 until^ i \xE2\x89\xA7 10 next^ i.inc() { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.loop.kind, LHAT_FOR_UNTIL);
    parse_dispose(&p);

    // 16.2: the focus need not be named, and it^ is what names it. It is
    // bound like any other focus rather than left as a bare expression, so
    // 16.1's reading of for^ holds for every form.
    LHAT_TEST("an unnamed focus is bound to it^");
    parse_text(&p, "for^ 1 to^ 10 { print(it^) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(focus->v.binding.targets->kind, LHAT_NODE_FOCUS);
        LHAT_CHECK_EQ_INT(focus->v.binding.values->kind, LHAT_NODE_INT);
    }
    parse_dispose(&p);

    // 16.3: this form does not iterate at all.
    LHAT_TEST("for^ ... if^ ... scopes definitions to a condition");
    parse_text(&p, "for^ var^ i = 1, var^ j = 2 if^ i + j < 10 { print(i) else^: print(j) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_IF);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.loop.focus), 2);
        LHAT_CHECK_EQ_INT(s->v.loop.body->kind, LHAT_NODE_IF_STMT);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.loop.body->v.list.items), 2);
    }
    parse_dispose(&p);

    LHAT_TEST("for^ needs a driving clause");
    parse_text(&p, "for^ var^ i = 1 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE);
    parse_dispose(&p);
}

// 17 章.
static void test_patterns(void)
{
    Parse p;

    // 17.9: the clauses become an if-chain, so the expansion is the tree
    // itself rather than something a later stage performs.
    LHAT_TEST("when^ clauses become an if-chain");
    parse_text(&p, "for^ x { when^ 0: a() other^: b() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_WHEN);
        LHAT_CHECK_EQ_INT(s->v.loop.body->kind, LHAT_NODE_IF_STMT);
        const LhatNode *clauses = s->v.loop.body->v.list.items;
        LHAT_CHECK_EQ_INT(lhat_node_list_length(clauses), 2);
        // when^ V: lowers to a comparison against the subject.
        LHAT_CHECK(is_binary(clauses->v.clause.condition, LHAT_OP_EQ),
                   "expected the subject compared to the value");
        // 17.5: the default carries no condition.
        LHAT_CHECK(clauses->next->v.clause.condition == NULL, "other^ is bare");
    }
    parse_dispose(&p);

    // 17.2: the subject is a focus like any other, so it is bound once.
    LHAT_TEST("the subject is bound");
    parse_text(&p, "for^ parse(s) { when^ 0: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *focus = first_statement(&p)->v.loop.focus;
        LHAT_CHECK_EQ_INT(focus->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(focus->v.binding.targets->kind, LHAT_NODE_FOCUS);
    }
    parse_dispose(&p);

    LHAT_TEST("the subject may be named");
    parse_text(&p, "for^ var^ r = parse(s) { when^ 0: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(
        first_statement(&p)->v.loop.focus->v.binding.targets->kind,
        LHAT_NODE_IDENT);
    parse_dispose(&p);

    // 17.3: both ends are included, so the range is a pair of comparisons.
    LHAT_TEST("a range pattern becomes two comparisons");
    parse_text(&p, "for^ x { when^ 1 to^ 3: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *condition =
            first_statement(&p)->v.loop.body->v.list.items->v.clause.condition;
        LHAT_CHECK(is_binary(condition, LHAT_OP_AND), "expected and^");
        LHAT_CHECK(is_binary(condition->v.binary.left, LHAT_OP_GE), "≧ low");
        LHAT_CHECK(is_binary(condition->v.binary.right, LHAT_OP_LE), "≦ high");
    }
    parse_dispose(&p);

    // 17.4: a type pattern says so with isa^, since a bare name could be
    // either a value or a type.
    LHAT_TEST("a type pattern keeps isa^");
    parse_text(&p, "for^ x { when^ isa^ number^: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *condition =
            first_statement(&p)->v.loop.body->v.list.items->v.clause.condition;
        LHAT_CHECK(is_binary(condition, LHAT_OP_ISA), "expected isa^");
        LHAT_CHECK_EQ_INT(condition->v.binary.right->kind, LHAT_NODE_TYPE_NAME);
    }
    parse_dispose(&p);

    LHAT_TEST("several patterns on one when^ are an or^");
    parse_text(&p, "for^ x { when^ 1, 2, 3: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *condition =
            first_statement(&p)->v.loop.body->v.list.items->v.clause.condition;
        LHAT_CHECK(is_binary(condition, LHAT_OP_OR), "expected or^");
    }
    parse_dispose(&p);

    // 17.5: el^ and else^ mean the same as other^.
    LHAT_TEST("the default may be spelled el^ or else^");
    parse_text(&p, "for^ x { when^ 0: a() el^: b() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("the ':' after a pattern is required");
    parse_text(&p, "for^ x { when^ 0 a() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    // 17.6: only the ':' after the subject opens, so one ';' closes it.
    LHAT_TEST("the expression form is closed by one ';'");
    parse_text(&p, "var^ r = for^ x: when^ 0: 1 when^ 1 to^ 3: 2 other^: 3 ;");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK_EQ_INT(value->kind, LHAT_NODE_FOR);
        LHAT_CHECK_EQ_INT(value->v.loop.kind, LHAT_FOR_WHEN);
        LHAT_CHECK_EQ_INT(value->v.loop.body->kind, LHAT_NODE_IF_EXPR);
        LHAT_CHECK_EQ_INT(
            lhat_node_list_length(value->v.loop.body->v.list.items), 3);
    }
    parse_dispose(&p);

    // The ':' of the expression form has the shape of 16.3's annotation, and
    // what follows is what tells them apart.
    LHAT_TEST("a typed focus is still a typed focus");
    parse_text(&p, "for^ i:number^ from^ 1 to^ 3 { print(i) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *target =
            first_statement(&p)->v.loop.focus->v.binding.targets;
        LHAT_CHECK_EQ_INT(target->kind, LHAT_NODE_PARAM);
        LHAT_CHECK(target->v.param.type != NULL, "the annotation survives");
    }
    parse_dispose(&p);

    // A brace with no when^ dispatches on nothing and iterates over nothing.
    LHAT_TEST("a match with no clauses is the missing clause of 16.3");
    parse_text(&p, "for^ var^ i = 1 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FOR_NEEDS_CLAUSE);
    parse_dispose(&p);
}

// 16.5: the loop with no focus.
static void test_repeat(void)
{
    Parse p;

    LHAT_TEST("a count");
    parse_text(&p, "repeat^ 3 { print(1) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_REPEAT);
        LHAT_CHECK_EQ_INT(s->v.repeat.kind, LHAT_REPEAT_COUNT);
    }
    parse_dispose(&p);

    LHAT_TEST("forever");
    parse_text(&p, "repeat^ { break^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.repeat.kind, LHAT_REPEAT_FOREVER);
        LHAT_CHECK(s->v.repeat.bound == NULL, "no bound");
    }
    parse_dispose(&p);

    LHAT_TEST("while and until");
    parse_text(&p, "repeat^ while^ c { c := foo() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.repeat.kind, LHAT_REPEAT_WHILE);
    parse_dispose(&p);

    parse_text(&p, "repeat^ until^ done { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.repeat.kind, LHAT_REPEAT_UNTIL);
    parse_dispose(&p);

    // 16.5: an update clause belongs with the focus that for^ declares.
    LHAT_TEST("repeat^ takes no next^");
    parse_text(&p, "repeat^ while^ c next^ i := i + 1 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_REPEAT_TAKES_NO_NEXT);
    parse_dispose(&p);
}

// 9 章.
static void test_loop_clauses(void)
{
    Parse p;

    LHAT_TEST("all clauses in order");
    parse_text(&p,
               "for^ i from^ 1 to^ 10 {\n"
               "    prolog^: total := 0\n"
               "    first^: log('start')\n"
               "    main^: total := total + i\n"
               "    last^: log(i)\n"
               "    epilog^: log('end')\n"
               "    finally^: cleanup()\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.loop.body;
        // main^ goes into items; the other five are clauses.
        LHAT_CHECK_EQ_INT(lhat_node_list_length(body->v.list.items), 1);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(body->v.list.extra), 5);
        LHAT_CHECK_EQ_INT(body->v.list.extra->v.loop_clause.kind,
                          LHAT_CLAUSE_PROLOG);
    }
    parse_dispose(&p);

    LHAT_TEST("a body with no clause markers");
    parse_text(&p, "for^ i from^ 1 to^ 10 { print(i) }");
    {
        const LhatNode *body = first_statement(&p)->v.loop.body;
        LHAT_CHECK_EQ_INT(lhat_node_list_length(body->v.list.items), 1);
        LHAT_CHECK(body->v.list.extra == NULL, "no clauses");
    }
    parse_dispose(&p);

    // 9.3: last^ and epilog^ are trailing markers, so the statements before
    // them are unambiguously the body.
    LHAT_TEST("trailing clauses need no main^");
    parse_text(&p, "for^ i from^ 1 to^ 10 { print(i) last^: log(i) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("prolog^ after unlabelled statements needs main^");
    parse_text(&p, "for^ i from^ 1 to^ 10 { print(i) prolog^: total := 0 }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_MAIN_REQUIRED);
    parse_dispose(&p);

    // 9.2: the order is fixed.
    LHAT_TEST("clauses out of order are rejected");
    parse_text(&p, "for^ i from^ 1 to^ 10 { epilog^: a() prolog^: b() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_CLAUSE_ORDER);
    parse_dispose(&p);

    // 9.3: once the braces are carved into clauses, one of them has to be
    // the body. Statements written after prolog^ join it instead, which is
    // the shape this catches.
    LHAT_TEST("a loop carved into clauses needs a body among them");
    parse_text(&p, "for^ i from^ 1 to^ 10 { prolog^: total := 0 total := total + i }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_NO_BODY_CLAUSE);
    parse_dispose(&p);

    LHAT_TEST("and a prolog^ on its own is not one");
    parse_text(&p, "for^ i from^ 1 to^ 10 { prolog^: total := 0 }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_NO_BODY_CLAUSE);
    parse_dispose(&p);

    LHAT_TEST("but last^ is a body clause and settles it");
    parse_text(&p, "for^ i from^ 1 to^ 10 { prolog^: total := 0 last^: log(i) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // Braces with no clause heading are an implicit main^, empty ones too.
    LHAT_TEST("and braces with no clause at all are the body");
    parse_text(&p, "for^ i from^ 1 to^ 10 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 11.1: a definition carries an operator as a member whose name is the
    // operator itself.
    LHAT_TEST("op^.. is a member of a definition");
    parse_text(&p,
               "V := def^{ self^{}, op^.. := f^self^, o:string^ -> string^ { "
               "return^ o } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("and the arithmetic operators take one too");
    parse_text(&p,
               "V := def^{ self^{}, op^+ := f^self^, o:number^ -> number^ { "
               "return^ o } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 11.8: and^, or^ and '!' are the language's own logic, and 11.5's
    // comparisons decide by disjointness rather than by asking a type.
    // 11.9: and a comparison is not written one by one -- op^<=> is
    // what answers all six, so that is what a written op^< is pointed at.
    LHAT_TEST("but a comparison is not one to define");
    parse_text(&p,
               "V := def^{ self^{}, op^< := f^self^, o:number^ -> bool^ { "
               "return^ true^ } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    if (p.result.diagnostic_count > 0) {
        LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                          LHAT_PARSE_ERR_COMPARISON_NOT_DEFINABLE);
    }
    parse_dispose(&p);

    LHAT_TEST("and the three-way one is");
    parse_text(&p,
               "V := def^{ self^{}, op^<=> := f^self^, o:V -> number^ { "
               "return^ 0 } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 8.6改: nor is a compound spelling. 'a += b' is 'a := a + b', so what it
    // does is already settled by the op^ of the plain operator -- there is
    // nothing left for a definition of its own to say. Answered separately
    // from the rule above, since the thing to point at is that definition.
    LHAT_TEST("and a compound assignment has no definition of its own");
    parse_text(&p,
               "V := def^{ self^{}, op^+= := f^self^, o:number^ -> number^ { "
               "return^ o } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    if (p.result.diagnostic_count > 0) {
        LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                          LHAT_PARSE_ERR_COMPOUND_NOT_DEFINABLE);
    }
    parse_dispose(&p);

    LHAT_TEST("the concatenating one is refused the same way");
    parse_text(&p,
               "V := def^{ self^{}, op^..= := f^self^, o:string^ -> string^ { "
               "return^ o } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    if (p.result.diagnostic_count > 0) {
        LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                          LHAT_PARSE_ERR_COMPOUND_NOT_DEFINABLE);
    }
    parse_dispose(&p);

    // 9.10: pre^ runs before the condition, so it sits between prolog^ and
    // first^ -- which is where it has to be written.
    LHAT_TEST("pre^ takes its place between prolog^ and first^");
    parse_text(&p,
               "for^ i from^ 1 to^ 10 { prolog^: a() pre^: b() first^: c() "
               "main^: d() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("premain^ is the same clause");
    parse_text(&p, "for^ i from^ 1 to^ 10 { premain^: b() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *body = first_statement(&p)->v.loop.body;
        LHAT_CHECK_EQ_INT(body->v.list.extra->v.loop_clause.kind,
                          LHAT_CLAUSE_PRE);
    }
    parse_dispose(&p);

    LHAT_TEST("pre^ after main^ is out of order");
    parse_text(&p, "for^ i from^ 1 to^ 10 { main^: a() pre^: b() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_CLAUSE_ORDER);
    parse_dispose(&p);

    // 9.10: a walk binds its focus after the coroutine answers, so a clause
    // running before that would read the turn before.
    LHAT_TEST("pre^ is refused in a walk");
    parse_text(&p, "for^ k, v in^ t { pre^: a() main^: b() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_PRE_IN_WALK);
    parse_dispose(&p);

    // 16.3: next^ updates the focus of a conditional loop. A walk is advanced
    // by the coroutine it steps, so there is nothing there for it to move.
    LHAT_TEST("a walk takes no next^");
    parse_text(&p, "for^ k, v in^ t next^ i := i + 1 { main^: a() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_NEXT_NOT_HERE);
    parse_dispose(&p);

    // 16.4 makes to^ and downto^ sugar for a while^ that already carries a
    // next^ of its own, so writing another is the same mistake.
    LHAT_TEST("nor does to^");
    parse_text(&p, "for^ i from^ 1 to^ 10 next^ i := i + 1 { main^: a() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_NEXT_NOT_HERE);
    parse_dispose(&p);

    LHAT_TEST("nor downto^");
    parse_text(&p, "for^ i from^ 10 downto^ 1 next^ i := i - 1 { main^: a() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_NEXT_NOT_HERE);
    parse_dispose(&p);

    LHAT_TEST("but while^ and until^ are what it is for");
    parse_text(&p, "for^ var^ i := 1 while^ i < 3 next^ i := i + 1 { main^: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 10.1: finally^ belongs to blocks in general.
    LHAT_TEST("finally^ on a do^ block");
    parse_text(&p, "do^{ work() finally^: cleanup() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.list.extra), 1);
        LHAT_CHECK_EQ_INT(s->v.list.extra->v.loop_clause.kind,
                          LHAT_CLAUSE_FINALLY);
    }
    parse_dispose(&p);

    LHAT_TEST("finally^ in a procedure body");
    parse_text(&p, "g := p^ { work() finally^: cleanup() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 16.3: the loop clauses describe an iteration, so they need one.
    LHAT_TEST("loop clauses outside a loop are rejected");
    parse_text(&p, "do^{ work() epilog^: cleanup() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_CLAUSE_NOT_IN_LOOP);
    parse_dispose(&p);
}

// 02 の 3.1: a REPL has to tell these two apart.
// 14 章.
static void test_definitions(void)
{
    Parse p;

    LHAT_TEST("a definition with a template and methods");
    parse_text(&p,
               "FooBar := def^{\n"
               "    self^{\n"
               "        value1 := 0,\n"
               "        value2 := '',\n"
               "    },\n"
               "    methodA := p^self^ { print(self^.value1) },\n"
               "    staticProperty := 'x',\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *def = first_value(&p);
        LHAT_CHECK_EQ_INT(def->kind, LHAT_NODE_DEF);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(def->v.list.items), 3);

        // 14.3: the template is the entry without a key.
        const LhatNode *tmpl = def->v.list.items;
        LHAT_CHECK(tmpl->v.entry.key == NULL, "the template has no key");
        LHAT_CHECK_EQ_INT(tmpl->v.entry.value->kind, LHAT_NODE_SELF_TABLE);
        LHAT_CHECK_EQ_INT(
            lhat_node_list_length(tmpl->v.entry.value->v.list.items), 2);

        // 14.4: nothing marks a method; the first parameter does.
        const LhatNode *method = tmpl->next;
        LHAT_CHECK(method->v.entry.key != NULL, "a member is named");
        LHAT_CHECK_EQ_INT(method->v.entry.value->kind, LHAT_NODE_FUNC);
        LHAT_CHECK(method->v.entry.value->v.func.params != NULL,
                   "self^ is a parameter");
    }
    parse_dispose(&p);

    // 14.9: def^ is an expression, which is what makes 14.5 read as an
    // ordinary use of '..'.
    LHAT_TEST("composition is a concatenation");
    parse_text(&p, "FooBar2 := FooBar .. def^{ self^{ value3 := {} } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_CONCAT), "composed with '..'");
        LHAT_CHECK_EQ_INT(value->v.binary.left->kind, LHAT_NODE_IDENT);
        LHAT_CHECK_EQ_INT(value->v.binary.right->kind, LHAT_NODE_DEF);
    }
    parse_dispose(&p);

    LHAT_TEST("override^ and overload^ mark the member that follows");
    parse_text(&p,
               "Bar := Foo .. def^{\n"
               "    override^\n"
               "    foo := p^ { print('b') },\n"
               "    overload^ foo := p^x:string^ { print(x) },\n"
               "    plain := 1,\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *def = first_value(&p)->v.binary.right;
        const LhatNode *entry = def->v.list.items;
        LHAT_CHECK_EQ_INT(entry->v.entry.modifier, LHAT_DEF_OVERRIDE);
        LHAT_CHECK_EQ_INT(entry->next->v.entry.modifier, LHAT_DEF_OVERLOAD);
        LHAT_CHECK_EQ_INT(entry->next->next->v.entry.modifier, LHAT_DEF_PLAIN);
    }
    parse_dispose(&p);

    // 14.11: the same spelling builds an instance inside new.
    LHAT_TEST("self^{ ... } inside new");
    parse_text(&p,
               "FooBar := def^{\n"
               "    self^{ value1 := 0, value2 := '' },\n"
               "    new := f^v1:number^, v2:string^ {\n"
               "        return^ self^{ value1 := v1 }\n"
               "    },\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *entry = first_value(&p)->v.list.items->next;
        const LhatNode *body = entry->v.entry.value->v.func.body;
        const LhatNode *ret = body->v.list.items;
        LHAT_CHECK_EQ_INT(ret->kind, LHAT_NODE_RETURN);
        LHAT_CHECK_EQ_INT(ret->v.jump.value->kind, LHAT_NODE_SELF_TABLE);
    }
    parse_dispose(&p);

    // 14.4: self^ on its own stays a value. Only a '{' after it makes 14.6.
    LHAT_TEST("bare self^ is still a value");
    parse_text(&p, "x := self^.value1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK_EQ_INT(value->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(value->v.access.target->kind, LHAT_NODE_HAT_IDENT);
    }
    parse_dispose(&p);

    // 14.4: a method's parameter list is followed by the body's brace, which
    // must not be read as a template.
    LHAT_TEST("p^self^ { ... } is a parameter and a body");
    parse_text(&p, "m := p^self^ { print(1) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *func = first_value(&p);
        LHAT_CHECK_EQ_INT(func->kind, LHAT_NODE_FUNC);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(func->v.func.params), 1);
        LHAT_CHECK(func->v.func.body != NULL, "the brace opened the body");
    }
    parse_dispose(&p);

    LHAT_TEST("class^ is an ordinary value");
    parse_text(&p, "s := p^ { print(class^.staticProperty) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("a field of the template needs a name");
    parse_text(&p, "F := def^{ self^{ 1, 2 } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FIELD_NEEDS_NAME);
    parse_dispose(&p);

    LHAT_TEST("one template per definition");
    parse_text(&p, "F := def^{ self^{ a := 1 }, self^{ b := 2 } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_DUPLICATE_TEMPLATE);
    parse_dispose(&p);

    LHAT_TEST("override^ cannot mark the template");
    parse_text(&p, "F := def^{ override^ self^{ a := 1 } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_MODIFIER_ON_TEMPLATE);
    parse_dispose(&p);

    LHAT_TEST("a member needs ':='");
    parse_text(&p, "F := def^{ foo }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    // 14.10: the structural form is a type, written with ':' rather than ':='.
    LHAT_TEST("the structural form of a definition");
    parse_text(&p, "d := x as^ t^{ dispose : p^self^; }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *type = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(type->kind, LHAT_NODE_TYPE_TABLE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(type->v.list.items), 1);
    }
    parse_dispose(&p);

    // 14.5: '&' composes the type, '..' composes the definition.
    LHAT_TEST("an intersection of a name and a structure");
    parse_text(&p, "d := x as^ Foo & t^{ dispose : p^self^; }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.ascription.type->kind,
                      LHAT_NODE_TYPE_INTERSECT);
    parse_dispose(&p);
}

// 04-errors.md.
static void test_errors(void)
{
    Parse p;

    LHAT_TEST("errordef^ declares kinds, with and without fields");
    parse_text(&p,
               "errordef^ ParseError {\n"
               "    Syntax { line : number^, column : number^ },\n"
               "    Eof,\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *d = first_statement(&p);
        LHAT_CHECK_EQ_INT(d->kind, LHAT_NODE_ERRORDEF);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(d->v.named.members), 2);

        const LhatNode *syntax = d->v.named.members;
        LHAT_CHECK_EQ_INT(syntax->kind, LHAT_NODE_ERROR_KIND);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(syntax->v.named.members), 2);
        // 04 の 2.2: a field may carry a type, a default or both, so it takes
        // the shape that already holds all three rather than t^{ ... }'s.
        LHAT_CHECK_EQ_INT(syntax->v.named.members->kind, LHAT_NODE_PARAM);
        LHAT_CHECK(syntax->v.named.members->v.param.type != NULL, "typed");
        LHAT_CHECK(syntax->next->v.named.members == NULL, "Eof has no fields");
    }
    parse_dispose(&p);

    // 04 の 2.2: the default is written with ':=', matching 14.6's template.
    LHAT_TEST("a field may carry a default");
    parse_text(&p,
               "errordef^ ParseError {\n"
               "    Syntax { line := 0, column : number^ := 0 },\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *field = first_statement(&p)->v.named.members->v.named.members;
        LHAT_CHECK(field->v.param.type == NULL, "the type may be left out");
        LHAT_CHECK(field->v.param.fallback != NULL, "the default is there");
        LHAT_CHECK(field->next->v.param.type != NULL, "or written alongside");
        LHAT_CHECK(field->next->v.param.fallback != NULL, "with a default too");
    }
    parse_dispose(&p);

    LHAT_TEST("a field needs a type or a default");
    parse_text(&p, "errordef^ ParseError { Syntax { line } }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_FIELD_NEEDS_TYPE);
    parse_dispose(&p);

    // 04 の 2.4: the name is the identity, so there is no anonymous form.
    LHAT_TEST("errordef^ needs a name");
    parse_text(&p, "errordef^ { A, B }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_ERRORDEF_NEEDS_NAME);
    parse_dispose(&p);

    LHAT_TEST("error^Kind{ ... } names the kind in the syntax");
    parse_text(&p,
               "e := error^ParseError.Syntax{ message := 'bad', line := 3 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *e = first_value(&p);
        LHAT_CHECK_EQ_INT(e->kind, LHAT_NODE_ERROR_NEW);
        LHAT_CHECK_EQ_INT(e->v.named.name->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(e->v.named.members), 2);
    }
    parse_dispose(&p);

    // 04 の 2.5: without the kind there is nothing to construct.
    LHAT_TEST("error^ without a kind is rejected");
    parse_text(&p, "e := error^{ message := 'x' }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_ERROR_NEEDS_KIND);
    parse_dispose(&p);

    // 04 の 4.3: the fallback replaces the value of the operation that
    // failed, not of the arithmetic around it.
    LHAT_TEST("catch^ binds tighter than the binary operators");
    parse_text(&p, "total := base + parse(s) catch^ 0");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_ADD), "the sum is outermost");
        LHAT_CHECK(is_binary(value->v.binary.right, LHAT_OP_CATCH),
                   "catch^ is inside the sum");
    }
    parse_dispose(&p);

    LHAT_TEST("the right side of catch^ stops at the unary level");
    parse_text(&p, "n := f() catch^ 0 + 1");
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_ADD), "the sum is outermost");
        LHAT_CHECK(is_binary(value->v.binary.left, LHAT_OP_CATCH),
                   "the fallback is just 0");
    }
    parse_dispose(&p);

    // 11.7: one level, so they chain left to right.
    LHAT_TEST("?? sits beside catch^ and chains with it");
    parse_text(&p, "w := f() catch^ nil^ ?? 0");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_NIL_ELSE), "?? is outermost");
        LHAT_CHECK(is_binary(value->v.binary.left, LHAT_OP_CATCH),
                   "catch^ came first");
    }
    parse_dispose(&p);

    LHAT_TEST("?? defaults around the index, not the sum");
    parse_text(&p, "v := base + t[k] ?? 0");
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_ADD), "the sum is outermost");
        LHAT_CHECK(is_binary(value->v.binary.right, LHAT_OP_NIL_ELSE),
                   "?? is inside the sum");
    }
    parse_dispose(&p);

    // 04 の 5.1: try^ is unary, so it unwraps the call rather than the sum.
    LHAT_TEST("try^ sits at the unary level");
    parse_text(&p, "n := try^ f() + 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_ADD), "the sum is outermost");
        LHAT_CHECK_EQ_INT(value->v.binary.left->kind, LHAT_NODE_TRY);
    }
    parse_dispose(&p);

    // 8.2 with 04 の 5.1 and 4.4: a call is still a call when it is wrapped.
    LHAT_TEST("try^ and catch^ around a call stand alone as statements");
    parse_text(&p, "try^ save(x)\nsave(y) catch^ nil^\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *first = first_statement(&p);
        LHAT_CHECK_EQ_INT(first->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(first->v.jump.value->kind, LHAT_NODE_TRY);
        LHAT_CHECK_EQ_INT(first->next->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK(is_binary(first->next->v.jump.value, LHAT_OP_CATCH),
                   "the catch^ form too");
    }
    parse_dispose(&p);

    // 8.2 still holds: a fallback around something that is not a call is not
    // a statement, since nothing happens.
    LHAT_TEST("a bare fallback is not a statement");
    parse_text(&p, "t[k] ?? 0");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_BARE_EXPRESSION);
    parse_dispose(&p);

    // 13.11: isa^ asks about a type, so a type is what it reads on the right.
    LHAT_TEST("isa^ takes a type on the right");
    parse_text(&p, "b := x isa^ number^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_ISA), "expected isa^");
        LHAT_CHECK_EQ_INT(value->v.binary.right->kind, LHAT_NODE_TYPE_NAME);
    }
    parse_dispose(&p);

    LHAT_TEST("isa^ accepts a structure on the right");
    parse_text(&p, "b := x isa^ t^{a : number^}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.binary.right->kind,
                      LHAT_NODE_TYPE_TABLE);
    parse_dispose(&p);

    // 04 の 14.4: an error kind is reached through the declaration that
    // introduced it, so a type may be a qualified name.
    LHAT_TEST("a type may be a qualified name");
    parse_text(&p,
               "if^ e isa^ ParseError.Syntax {\n"
               "    report(e.line)\n"
               "    elseif^ e isa^ IOError:\n"
               "        log(e)\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *clause = first_statement(&p)->v.list.items;
        LHAT_CHECK_EQ_INT(lhat_node_list_length(clause), 2);
        const LhatNode *type = clause->v.clause.condition->v.binary.right;
        LHAT_CHECK_EQ_INT(type->kind, LHAT_NODE_MEMBER);
        LHAT_CHECK_EQ_INT(type->v.access.target->kind, LHAT_NODE_TYPE_NAME);
    }
    parse_dispose(&p);

    LHAT_TEST("a signature may return a union with an error set");
    parse_text(&p,
               "read := f^ p:string^ -> string^|IOError.NotFound {\n"
               "    return^ try^ open(p)\n"
               "}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *returns = first_value(&p)->v.func.return_type;
        LHAT_CHECK_EQ_INT(returns->kind, LHAT_NODE_TYPE_UNION);
        LHAT_CHECK_EQ_INT(returns->v.binary.right->kind, LHAT_NODE_MEMBER);
    }
    parse_dispose(&p);
}

static void test_incomplete(void)
{
    Parse p;

    LHAT_TEST("an unclosed block is incomplete, not wrong");
    parse_text(&p, "Counter := {\n    value := 0,");
    LHAT_CHECK(p.result.incomplete, "expected the input to be incomplete");
    parse_dispose(&p);

    LHAT_TEST("an unterminated string is incomplete");
    parse_text(&p, "s := \"oops");
    LHAT_CHECK(p.result.incomplete, "expected the input to be incomplete");
    parse_dispose(&p);

    LHAT_TEST("a complete unit is not incomplete");
    parse_text(&p, "x := 1");
    LHAT_CHECK(!p.result.incomplete, "should be complete");
    parse_dispose(&p);

    LHAT_TEST("a genuine syntax error is not incompleteness");
    parse_text(&p, "x := 1\ny + z\nw := 2");
    LHAT_CHECK(!p.result.incomplete, "should not be reported as incomplete");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);
}

static void test_recovery(void)
{
    Parse p;

    LHAT_TEST("parsing continues after an error");
    parse_text(&p, "var^ x = 1\na + b\nvar^ y = 2");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    {
        // The first and last statements should still be recognised.
        const LhatNode *statements = first_statement(&p);
        LHAT_CHECK_EQ_INT(statements->kind, LHAT_NODE_DEFINE);
        const LhatNode *last = statements;
        while (last->next != NULL) {
            last = last->next;
        }
        LHAT_CHECK_EQ_INT(last->kind, LHAT_NODE_DEFINE);
    }
    parse_dispose(&p);

    LHAT_TEST("only the first of a cascade is reported");
    parse_text(&p, "x := ) ) )");
    LHAT_CHECK_EQ_INT(p.result.diagnostic_count, 1);
    parse_dispose(&p);
}

static void test_realistic(void)
{
    Parse p;

    LHAT_TEST("a small program parses without diagnostics");
    parse_text(&p,
               "#!/usr/bin/env lhat\n"
               "# a small sample\n"
               "$Counter := {\n"
               "    value := 0,\n"
               "    bump := p^step:number^ -> number^ {\n"
               "        value := value + step\n"
               "        return^ value\n"
               "    },\n"
               "}\n"
               "c := $Counter\n"
               "if^ c.value \xE2\x89\xA6 10 {\n"
               "    print('done')\n"
               "}\n"
               "q, r := divmod(7, 2)\n"
               "msg := $\"q = {q}, r = {r}\"\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(lhat_node_list_length(first_statement(&p)) >= 5,
               "expected several statements");
    parse_dispose(&p);
}

// 2 章 and 4 章.
static void command_text(Parse *p, const char *text)
{
    lhat_source_init_from_string(&p->source, "<test>", text, strlen(text));
    lhat_lexer_init(&p->lexer, &p->source);
    lhat_parse_command(&p->lexer, &p->result);
}

static bool is_command(const char *text)
{
    LhatSource source;
    LhatLexer lexer;
    lhat_source_init_from_string(&source, "<test>", text, strlen(text));
    lhat_lexer_init(&lexer, &source);
    bool answer = lhat_parse_is_command(&lexer);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return answer;
}

static void test_command_form(void)
{
    Parse p;

    // 2.3's table. What decides it is whether the token after the name could
    // carry an expression on, which is 01 の 10.9's classification reused.
    LHAT_TEST("2.3 decides by what follows the name");
    LHAT_CHECK(is_command("print \"done\""), "a string cannot continue");
    LHAT_CHECK(is_command("ls"), "nothing follows");
    LHAT_CHECK(is_command("foo 1 2 3"), "an integer cannot continue");
    LHAT_CHECK(is_command("foo {a := 1}"), "'{' opens a table literal");
    LHAT_CHECK(is_command("foo !x"), "'!' is prefix only");
    LHAT_CHECK(!is_command("x - 1"), "'-' can be binary");
    LHAT_CHECK(!is_command("x.y"), "'.' continues");
    LHAT_CHECK(!is_command("x := 1"), "':=' continues");
    LHAT_CHECK(!is_command("foo(1)"), "'(' continues");
    LHAT_CHECK(!is_command("var^ x = 1"), "a hat identifier is not a name");

    LHAT_TEST("juxtaposed arguments become a call");
    command_text(&p, "foo 1 2 3");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(s->v.jump.value->kind, LHAT_NODE_CALL);
        LHAT_CHECK_EQ_INT(
            lhat_node_list_length(s->v.jump.value->v.access.argument), 3);
    }
    parse_dispose(&p);

    LHAT_TEST("a bare name is a call with no arguments");
    command_text(&p, "ls");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(first_statement(&p)->v.jump.value->v.access.argument == NULL,
               "no arguments");
    parse_dispose(&p);

    // An argument is a whole expression, so an operator inside one does not
    // split it.
    LHAT_TEST("an operator inside an argument keeps it whole");
    command_text(&p, "foo 1 + 2");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(
        lhat_node_list_length(
            first_statement(&p)->v.jump.value->v.access.argument), 1);
    parse_dispose(&p);

    // 2.3: this is the case the condition exists for.
    LHAT_TEST("arithmetic at a prompt stays arithmetic");
    command_text(&p, "var^ x = 1\nx - 1\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "a bare expression is no statement");
    parse_dispose(&p);

    // 2.4: the call parenthesis binds tighter, so both forms agree.
    LHAT_TEST("a parenthesised call reaches the same tree");
    command_text(&p, "foo(1, 2)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_CALL_STMT);
        LHAT_CHECK_EQ_INT(
            lhat_node_list_length(s->v.jump.value->v.access.argument), 2);
    }
    parse_dispose(&p);

    // 3.2: a fragment that is not the command form falls through, so a host
    // can hand every line to one entry point.
    LHAT_TEST("a non-command fragment is parsed normally");
    command_text(&p, "var^ x = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    // 3.1: an unfinished fragment is not an error, and the command entry
    // point has to say so too.
    LHAT_TEST("an unfinished fragment is reported as incomplete");
    command_text(&p, "var^ t = {");
    LHAT_CHECK(p.result.incomplete, "expected incomplete");
    parse_dispose(&p);

    // 2.1: the normal form still refuses juxtaposition, since a source file
    // gives no place for the argument list to end.
    LHAT_TEST("the normal form still refuses juxtaposition");
    parse_text(&p, "foo 1 2 3");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_JUXTAPOSITION);
    parse_dispose(&p);
}

// 05-modules.md.
static void test_modules(void)
{
    Parse p;

    // 05 の 3 章: a name for the unit, independent of where its file sits.
    LHAT_TEST("module^ names the unit");
    parse_text(&p, "module^ namespace1.module1\nvar^ x = 1\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_MODULE);
        LHAT_CHECK_EQ_INT(s->v.named.name->kind, LHAT_NODE_MEMBER);
    }
    parse_dispose(&p);

    LHAT_TEST("module^ goes first");
    parse_text(&p, "var^ x = 1\nmodule^ m\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_MODULE_MISPLACED);
    parse_dispose(&p);

    LHAT_TEST("module^ appears once");
    parse_text(&p, "module^ a\nmodule^ b\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_MODULE_MISPLACED);
    parse_dispose(&p);

    // 05 の 4 章: a mark on the declaration, not a list at the end of the
    // file, so the exports are known from the text alone.
    LHAT_TEST("public^ marks the declaration");
    parse_text(&p,
               "public^ let^ x = 1\n"
               "var^ y = 2\n"
               "public^ errordef^ E { A }\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK(s->v.binding.exported, "x is public");
        LHAT_CHECK(!s->next->v.binding.exported, "y is not");
        LHAT_CHECK(s->next->next->v.named.exported, "E is public");
    }
    parse_dispose(&p);

    LHAT_TEST("public^ needs something with a name");
    parse_text(&p, "public^ print(1)");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_PUBLIC_NEEDS_DECLARATION);
    parse_dispose(&p);

    LHAT_TEST("require^ takes a path");
    parse_text(&p, "var^ io = require^ \"system/io.lh\"");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK_EQ_INT(value->kind, LHAT_NODE_REQUIRE);
        LHAT_CHECK_EQ_INT(value->v.jump.value->kind, LHAT_NODE_STRING);
    }
    parse_dispose(&p);

    // 05 の 5.2: the checker follows this, so the path cannot be computed.
    LHAT_TEST("require^ refuses a computed path");
    parse_text(&p, "var^ p = \"x\"\nvar^ m = require^ p\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_REQUIRE_NEEDS_LITERAL);
    parse_dispose(&p);

    // 05 の 5.4: it binds one name, chosen here, so reaching in is ordinary
    // member access.
    LHAT_TEST("what require^ yields is reached into normally");
    parse_text(&p,
               "var^ io = require^ \"system/io.lh\"\n"
               "var^ open = io.File.open\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->next->v.binding.values->kind,
                      LHAT_NODE_MEMBER);
    parse_dispose(&p);
}

// 02 の 14.12改: always parenthesized, always exactly one operand -- a
// primary rather than a prefix operator.
static void test_typeof(void)
{
    Parse p;

    LHAT_TEST("typeof^(expr) parses as one node");
    parse_text(&p, "var^ t = typeof^(5)\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_TYPEOF);
    parse_dispose(&p);

    LHAT_TEST("what it names is the operand alone");
    parse_text(&p, "var^ t = typeof^(5)\n");
    LHAT_CHECK_EQ_INT(first_value(&p)->v.jump.value->kind, LHAT_NODE_INT);
    parse_dispose(&p);

    LHAT_TEST("the parentheses are not optional");
    parse_text(&p, "var^ t = typeof^ 5\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    LHAT_TEST("nor is an operand");
    parse_text(&p, "var^ t = typeof^()\n");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    parse_dispose(&p);

    // 14.4: a method reached the ordinary way, exactly as any other member.
    LHAT_TEST("'.signature' reads as a member access");
    parse_text(&p, "var^ s = typeof^(5).signature\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_MEMBER);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.access.target->kind, LHAT_NODE_TYPEOF);
    parse_dispose(&p);
}

// 01 の 2.3: a second hat counts levels, and only it^/this^/self^/
// class^ have levels to count -- everywhere else, and for every other word,
// the extra hats are refused where they are written.
static void test_stacked_hats(void)
{
    Parse p;

    LHAT_TEST("a value word does not stack");
    parse_text(&p, "x := true^^");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);

    // 14.12改: which implementation an override wraps is the composition's
    // business -- skipping layers by count breaks the moment a part is
    // inserted, so naming the part is the spelling for that.
    LHAT_TEST("super does not stack");
    parse_text(&p, "x := super^^.a()");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);

    LHAT_TEST("a keyword does not stack");
    parse_text(&p, "if^^ x { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);

    LHAT_TEST("a type name does not stack");
    parse_text(&p, "var^ n : number^^ = 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);

    LHAT_TEST("a member key does not stack");
    parse_text(&p, "x := t.foo^^");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);

    // The four that do stack pass the parser -- the reach is 01 の 2.3's,
    // and whether it compiles yet is the compiler's answer (test_vm).
    LHAT_TEST("it^^ parses, carrying its count");
    parse_text(&p, "x := it^^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->kind, LHAT_NODE_HAT_IDENT);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.name.hats, 2);
    parse_dispose(&p);

    LHAT_TEST("this^^ parses too");
    parse_text(&p, "x := this^^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 9.8: the one keyword whose hats count -- unchanged.
    LHAT_TEST("break^^^ still counts its loops");
    parse_text(&p,
               "for^ 1 to^ 3 { for^ 1 to^ 3 { for^ 1 to^ 3 { break^^^ } } }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // 13.13: the fifth word, and the one that counts written type
    // literals rather than bindings or loops. A type name still does not
    // stack (above) -- this one is not a name of a type but a reach to one.
    LHAT_TEST("Self^^ stacks where a type is written");
    parse_text(&p, "var^ t : t^{ a : t^{ b : Self^^ } } = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    // It is a type and never a value, so the expression side has no reach for
    // a second hat to count -- refused there like any other word.
    LHAT_TEST("but not where a value is written");
    parse_text(&p, "x := Self^^");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_HATS_DONT_STACK);
    parse_dispose(&p);
}

int main(void)
{
    test_spans();
#if LHAT_WITH_COMMENTS
    test_comments();
#endif
    test_statements();
    test_command_form();
    test_modules();
    test_typeof();
    test_precedence();
    test_comparison_chain();
    test_postfix();
    test_literals();
    test_functions();
    test_conditionals();
    test_types();
    test_loops();
    test_patterns();
    test_repeat();
    test_loop_clauses();
    test_definitions();
    test_errors();
    test_incomplete();
    test_recovery();
    test_realistic();
    test_stacked_hats();
    return lhat_test_report("test_parser");
}
