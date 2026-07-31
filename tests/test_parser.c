// L^ (lhat) -- tests for the parser.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "01". The cases that matter most are the ones where a decision in the
// specification would be invisible in the token stream but visible here:
// precedence, comparison chaining, the call parenthesis rule, and the
// statement forms that are deliberately not accepted.

#include <string.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult result;
} Parse;

static void parse_text(Parse *p, const char *text)
{
    lhat_source_init_from_string(&p->source, "<test>", text, strlen(text));
    lhat_lexer_init(&p->lexer, &p->source);
    lhat_parse(&p->lexer, &p->result);
}

static void parse_dispose(Parse *p)
{
    lhat_parse_result_dispose(&p->result);
    lhat_lexer_dispose(&p->lexer);
    lhat_source_dispose(&p->source);
}

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

static void test_statements(void)
{
    Parse p;

    // 8.6: let^ is what creates a name; := on its own reassigns.
    LHAT_TEST("definition");
    parse_text(&p, "let^ x = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    LHAT_TEST("let^ also accepts the longer spelling");
    parse_text(&p, "let^ x := 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    LHAT_TEST("let^ with a type annotation");
    parse_text(&p, "let^ x : number^ = 1");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.binding.targets->kind,
                      LHAT_NODE_PARAM);
    parse_dispose(&p);

    LHAT_TEST("multiple definition binds pairwise");
    parse_text(&p, "let^ a, b = 1, 2");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.targets), 2);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.values), 2);
    }
    parse_dispose(&p);

    // 7.3 (Q2) and 8.6: reassignment puts the target first and is written
    // with :=, since let^ took over making names.
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

    // 13.10: the marker sits on the value, not on the binding.
    LHAT_TEST("destructuring binding");
    parse_text(&p, "let^ q, r = unpack^ divmod(7, 2)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_DEFINE);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.binding.targets), 2);
        LHAT_CHECK_EQ_INT(s->v.binding.values->kind, LHAT_NODE_UNPACK);
    }
    parse_dispose(&p);

    // Putting the marker on the value is what makes this work at all.
    LHAT_TEST("destructuring reassignment");
    parse_text(&p, "q, r := unpack^ divmod(7, 2)");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_REASSIGN);
        LHAT_CHECK_EQ_INT(s->v.binding.values->kind, LHAT_NODE_UNPACK);
    }
    parse_dispose(&p);

    LHAT_TEST("destructuring without unpack^ is rejected");
    parse_text(&p, "q, r := divmod(7, 2)");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_DESTRUCTURE_NEEDS_UNPACK);
    parse_dispose(&p);

    LHAT_TEST("unpack^ must be the only value");
    parse_text(&p, "a, b := unpack^ f(), 3");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_UNPACK_NOT_ALONE);
    parse_dispose(&p);

    LHAT_TEST("unpack^ outside a binding is rejected");
    parse_text(&p, "unpack^ f()");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_UNPACK_MISPLACED);
    parse_dispose(&p);

    // The target list is the ordinary one, so a type may be written on it.
    LHAT_TEST("typed destructuring targets");
    parse_text(&p, "let^ q:number^, r:number^ = unpack^ f()");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *target = first_statement(&p)->v.binding.targets;
        LHAT_CHECK_EQ_INT(target->kind, LHAT_NODE_PARAM);
        LHAT_CHECK(target->v.param.type != NULL, "the target should carry a type");
    }
    parse_dispose(&p);

    LHAT_TEST("a single definition may carry a type");
    parse_text(&p, "let^ x:number^ = 1");
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

    // 2.1: juxtaposition is only a call in command mode.
    LHAT_TEST("juxtaposition is rejected with a suggestion");
    parse_text(&p, "foo 1 2 3");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_JUXTAPOSITION);
    parse_dispose(&p);

    // 8.6: the accident this replaced. Without let^, ':=' inside a nested
    // scope reassigns rather than quietly shadowing.
    LHAT_TEST("':=' in a nested scope reassigns");
    parse_text(&p, "let^ i = 0\nif^ foo() { i := 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *inner =
            first_statement(&p)->next->v.list.items->v.clause.body->v.list.items;
        LHAT_CHECK_EQ_INT(inner->kind, LHAT_NODE_REASSIGN);
    }
    parse_dispose(&p);

    LHAT_TEST("shadowing has to be written out");
    parse_text(&p, "let^ i = 0\nif^ foo() { let^ i = 1 }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *inner =
            first_statement(&p)->next->v.list.items->v.clause.body->v.list.items;
        LHAT_CHECK_EQ_INT(inner->kind, LHAT_NODE_DEFINE);
    }
    parse_dispose(&p);

    // 8.7: mutual recursion is handled by scope-wide visibility, so a
    // declaration without a value has no job to do.
    LHAT_TEST("let^ needs a value");
    parse_text(&p, "let^ x : number^");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_LET_NEEDS_VALUE);
    parse_dispose(&p);

    // 8.6: '=' compares, so this reaches the statement level as an
    // expression. The C habit is common enough for its own message.
    LHAT_TEST("'x = 1' names both intentions");
    parse_text(&p, "x = 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_EQUALS_IS_COMPARISON);
    parse_dispose(&p);

    LHAT_TEST("'<<' reports what replaced it");
    parse_text(&p, "counter << 1");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_WITHDRAWN_SHIFT);
    parse_dispose(&p);

    // 8.6: the enclosing construct is the introducer, so ':=' still defines
    // inside for^, with^ and a brace list.
    LHAT_TEST("an introducer keeps ':=' a definition");
    parse_text(&p, "for^ k := 1 to^ 3 { print(k) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_statement(&p)->v.loop.focus->kind, LHAT_NODE_DEFINE);
    parse_dispose(&p);

    // Q2: the postfix form was withdrawn, and the parser says so.
    LHAT_TEST("postfix reassignment reports what replaced it");
    parse_text(&p, "i + 1 -> i");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_WITHDRAWN_ARROW);
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

    // 12.1: with^ takes local definitions and one block.
    LHAT_TEST("with^");
    parse_text(&p, "with^ r := open(\"d\")\nwith^ w := create(\"o\")\n{ copy(r, w) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->kind, LHAT_NODE_WITH);
        LHAT_CHECK_EQ_INT(lhat_node_list_length(s->v.list.items), 2);
        LHAT_CHECK(s->v.list.extra != NULL, "with^ should have a body");
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

    LHAT_TEST("is^ participates in a chain");
    parse_text(&p, "x := a is^ b");
    LHAT_CHECK(is_binary(first_value(&p), LHAT_OP_IS), "expected is^");
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

    // 13.9: three types, uniform across suspension points.
    LHAT_TEST("coroutine type");
    parse_text(&p, "x := y as^ c^{ number^, string^, nil^ }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *t = first_value(&p)->v.ascription.type;
        LHAT_CHECK_EQ_INT(t->kind, LHAT_NODE_TYPE_CORO);
        LHAT_CHECK(t->v.coroutine.receive != NULL, "receive type");
        LHAT_CHECK(t->v.coroutine.produce != NULL, "produce type");
        LHAT_CHECK(t->v.coroutine.result != NULL, "result type");
    }
    parse_dispose(&p);

    // Q9: '::' was withdrawn, and the parser points at '->'.
    LHAT_TEST(":: reports what replaced it");
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
    parse_text(&p, "for^ i := 1 to^ 10 { print(i) }");
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
    parse_text(&p, "for^ i := 10 downto^ 1 step^ 2 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_DOWNTO);
        LHAT_CHECK(s->v.loop.step != NULL, "expected a step");
    }
    parse_dispose(&p);

    // 16.3: from^ was withdrawn, and the parser says what replaced it.
    LHAT_TEST("from^ reports what replaced it");
    parse_text(&p, "for^ i from^ 1 to^ 10 { }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_WITHDRAWN_FROM);
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
    parse_text(&p, "for^ i := 1 while^ i < 10 next^ i := i + 1 { }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *s = first_statement(&p);
        LHAT_CHECK_EQ_INT(s->v.loop.kind, LHAT_FOR_WHILE);
        LHAT_CHECK(s->v.loop.advance != NULL, "expected a next^ statement");
        LHAT_CHECK_EQ_INT(s->v.loop.advance->kind, LHAT_NODE_REASSIGN);
    }
    parse_dispose(&p);

    LHAT_TEST("until^ is the negated form");
    parse_text(&p, "for^ i := 1 until^ i \xE2\x89\xA7 10 next^ i.inc() { }");
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
    parse_text(&p, "for^ i := 1, j := 2 if^ i + j < 10 { print(i) else^: print(j) }");
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
    parse_text(&p, "for^ i := 1 { }");
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
    parse_text(&p, "for^ r := parse(s) { when^ 0: a() }");
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

    // 17.4: a type pattern says so with is^, since a bare name could be
    // either a value or a type.
    LHAT_TEST("a type pattern keeps is^");
    parse_text(&p, "for^ x { when^ is^ number^: a() }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *condition =
            first_statement(&p)->v.loop.body->v.list.items->v.clause.condition;
        LHAT_CHECK(is_binary(condition, LHAT_OP_IS), "expected is^");
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
    parse_text(&p, "let^ r = for^ x: when^ 0: 1 when^ 1 to^ 3: 2 other^: 3 ;");
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
    parse_text(&p, "for^ i:number^ := 1 to^ 3 { print(i) }");
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
    parse_text(&p, "for^ i := 1 { }");
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
               "for^ i := 1 to^ 10 {\n"
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
    parse_text(&p, "for^ i := 1 to^ 10 { print(i) }");
    {
        const LhatNode *body = first_statement(&p)->v.loop.body;
        LHAT_CHECK_EQ_INT(lhat_node_list_length(body->v.list.items), 1);
        LHAT_CHECK(body->v.list.extra == NULL, "no clauses");
    }
    parse_dispose(&p);

    // 9.3: last^ and epilog^ are trailing markers, so the statements before
    // them are unambiguously the body.
    LHAT_TEST("trailing clauses need no main^");
    parse_text(&p, "for^ i := 1 to^ 10 { print(i) last^: log(i) }");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    parse_dispose(&p);

    LHAT_TEST("prolog^ after unlabelled statements needs main^");
    parse_text(&p, "for^ i := 1 to^ 10 { print(i) prolog^: total := 0 }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_MAIN_REQUIRED);
    parse_dispose(&p);

    // 9.2: the order is fixed.
    LHAT_TEST("clauses out of order are rejected");
    parse_text(&p, "for^ i := 1 to^ 10 { epilog^: a() prolog^: b() }");
    LHAT_CHECK(p.result.diagnostic_count > 0, "expected a diagnostic");
    LHAT_CHECK_EQ_INT(p.result.diagnostics[0].code,
                      LHAT_PARSE_ERR_CLAUSE_ORDER);
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

    // 14.11: the same spelling builds an instance inside new^.
    LHAT_TEST("self^{ ... } inside new^");
    parse_text(&p,
               "FooBar := def^{\n"
               "    self^{ value1 := 0, value2 := '' },\n"
               "    new^ := f^v1:number^, v2:string^ {\n"
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
        // The fields reuse the shape of t^{ ... }.
        LHAT_CHECK_EQ_INT(syntax->v.named.members->kind, LHAT_NODE_MEMBER_DECL);
        LHAT_CHECK(syntax->next->v.named.members == NULL, "Eof has no fields");
    }
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

    // 13.11: is^ asks about a type, so a type is what it reads on the right.
    LHAT_TEST("is^ takes a type on the right");
    parse_text(&p, "b := x is^ number^");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    {
        const LhatNode *value = first_value(&p);
        LHAT_CHECK(is_binary(value, LHAT_OP_IS), "expected is^");
        LHAT_CHECK_EQ_INT(value->v.binary.right->kind, LHAT_NODE_TYPE_NAME);
    }
    parse_dispose(&p);

    LHAT_TEST("is^ accepts a structure on the right");
    parse_text(&p, "b := x is^ t^{a : number^}");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK_EQ_INT(first_value(&p)->v.binary.right->kind,
                      LHAT_NODE_TYPE_TABLE);
    parse_dispose(&p);

    // 04 の 14.4: an error kind is reached through the declaration that
    // introduced it, so a type may be a qualified name.
    LHAT_TEST("a type may be a qualified name");
    parse_text(&p,
               "if^ e is^ ParseError.Syntax {\n"
               "    report(e.line)\n"
               "    elseif^ e is^ IOError:\n"
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
    parse_text(&p, "let^ x = 1\na + b\nlet^ y = 2");
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
               "q, r := unpack^ divmod(7, 2)\n"
               "msg := $\"q = {q}, r = {r}\"\n");
    LHAT_CHECK_EQ_INT(error_count(&p), 0);
    LHAT_CHECK(lhat_node_list_length(first_statement(&p)) >= 5,
               "expected several statements");
    parse_dispose(&p);
}

int main(void)
{
    test_statements();
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
    return lhat_test_report("test_parser");
}
