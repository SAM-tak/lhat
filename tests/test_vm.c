// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <string.h>

#include "code.h"
#include "object.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "testutil.h"
#include "vm.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatProto *proto;
    LhatCompileStatus compiled;
    // 03 の 4.3: the machine owns what the program allocates, so the answer
    // is only good while it is. One test, one machine.
    LhatMachine *machine;
    LhatRunResult ran;
} Run;

// Everything up to the run, for a test driving a machine of its own.
static void compile_text(Run *r, const char *text)
{
    lhat_source_init_from_string(&r->source, "<test>", text, strlen(text));
    lhat_lexer_init(&r->lexer, &r->source);
    lhat_parse(&r->lexer, &r->parsed);
    r->compiled = lhat_compile(r->parsed.root, &r->lexer, &r->proto);
    r->machine = NULL;
    memset(&r->ran, 0, sizeof r->ran);
}

// The same, as the next input of a session (03 の 4.3).
static void compile_next_text(Run *r, LhatCompileSession *s, const char *text)
{
    lhat_source_init_from_string(&r->source, "<test>", text, strlen(text));
    lhat_lexer_init(&r->lexer, &r->source);
    lhat_parse(&r->lexer, &r->parsed);
    r->compiled = lhat_compile_next(s, r->parsed.root, &r->lexer, &r->proto);
    r->machine = NULL;
    memset(&r->ran, 0, sizeof r->ran);
}

// The same again, read the way a prompt reads it (02 の 8.2), so a bare
// expression is a statement and the last one is the answer (03 の 4.3).
static void compile_asked_text(Run *r, LhatCompileSession *s, const char *text)
{
    lhat_source_init_from_string(&r->source, "<test>", text, strlen(text));
    lhat_lexer_init(&r->lexer, &r->source);
    lhat_parse_interactive(&r->lexer, &r->parsed);
    r->compiled = lhat_compile_next(s, r->parsed.root, &r->lexer, &r->proto);
    r->machine = NULL;
    memset(&r->ran, 0, sizeof r->ran);
}

static void compiled_dispose(Run *r)
{
    lhat_proto_free(r->proto);
    lhat_parse_result_dispose(&r->parsed);
    lhat_lexer_dispose(&r->lexer);
    lhat_source_dispose(&r->source);
}

static void run_text(Run *r, const char *text)
{
    compile_text(r, text);
    if (r->compiled != LHAT_COMPILE_OK) {
        return;
    }
    r->machine = lhat_machine_new();
    if (r->machine != NULL) {
        r->ran = lhat_run(r->machine, r->proto);
    }
}

static void run_dispose(Run *r)
{
    lhat_machine_dispose(r->machine);
    r->machine = NULL;
    compiled_dispose(r);
}

// The value a unit's return^ produced, asserted as an exact integer.
#define CHECK_INTEGER(r, expected)                                            \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_integer((r)->ran.value), "expected an integer");   \
        LHAT_CHECK_EQ_INT(lhat_as_integer((r)->ran.value), (expected));       \
    } while (0)

#define CHECK_REAL(r, expected)                                               \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_real((r)->ran.value), "expected a real");          \
        LHAT_CHECK(lhat_as_real((r)->ran.value) == (expected), "value");      \
    } while (0)

#define CHECK_BOOL(r, expected)                                               \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_bool((r)->ran.value), "expected a bool");          \
        LHAT_CHECK_EQ_INT(lhat_as_bool((r)->ran.value), (expected));          \
    } while (0)

// 5.2: the encoding is a shift and a mask, so what goes in comes back out.
static void test_encoding(void)
{
    LHAT_TEST("an instruction round trips");
    {
        LhatInstruction i = lhat_encode_abc(LHAT_BC_ADD, 3, 200, 255);
        LHAT_CHECK_EQ_INT(lhat_op(i), LHAT_BC_ADD);
        LHAT_CHECK_EQ_INT(lhat_a(i), 3);
        LHAT_CHECK_EQ_INT(lhat_b(i), 200);
        LHAT_CHECK_EQ_INT(lhat_c(i), 255);
    }

    LHAT_TEST("Bx carries the whole constant range");
    {
        LhatInstruction i = lhat_encode_abx(LHAT_BC_LOADK, 7, 65535);
        LHAT_CHECK_EQ_INT(lhat_op(i), LHAT_BC_LOADK);
        LHAT_CHECK_EQ_INT(lhat_a(i), 7);
        LHAT_CHECK_EQ_INT(lhat_bx(i), 65535);
    }

    LHAT_TEST("a jump reaches both ways");
    {
        LHAT_CHECK_EQ_INT(lhat_jump_offset(lhat_encode_jump(LHAT_BC_JUMP, 0, 1)), 1);
        LHAT_CHECK_EQ_INT(lhat_jump_offset(lhat_encode_jump(LHAT_BC_JUMP, 0, -1)), -1);
        LHAT_CHECK_EQ_INT(
            lhat_jump_offset(lhat_encode_jump(LHAT_BC_JUMP, 0, 32767)), 32767);
        LHAT_CHECK_EQ_INT(
            lhat_jump_offset(lhat_encode_jump(LHAT_BC_JUMP, 0, -32768)), -32768);
    }

    LHAT_TEST("an equal constant is stored once");
    {
        LhatChunk chunk;
        lhat_chunk_init(&chunk);
        LHAT_CHECK_EQ_INT(lhat_chunk_constant(&chunk, lhat_integer(7)), 0);
        LHAT_CHECK_EQ_INT(lhat_chunk_constant(&chunk, lhat_integer(7)), 0);
        LHAT_CHECK_EQ_INT(lhat_chunk_constant(&chunk, lhat_integer(8)), 1);
        LHAT_CHECK_EQ_INT(chunk.constant_count, 2);
        lhat_chunk_dispose(&chunk);
    }
}

static void test_arithmetic(void)
{
    Run r;

    LHAT_TEST("integers stay integers");
    run_text(&r, "return^ 1 + 2 * 3\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("precedence and grouping hold through the compiler");
    run_text(&r, "return^ (1 + 2) * 3\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 02 の 14.8: number^ is one type, so an operation with a real in it
    // widens rather than refusing.
    LHAT_TEST("a real widens the operation");
    run_text(&r, "return^ 1 + 0.5\n");
    CHECK_REAL(&r, 1.5);
    run_dispose(&r);

    // 14.8改: the integers hold the answer or they do not, and when they do
    // not the operation widens the same way a real operand widens it. 04 の
    // 11.2 rules out failing here -- every '+' would return a union.
    LHAT_TEST("an integer sum too large to hold widens");
    run_text(&r, "return^ 9223372036854775807 + 1\n");
    CHECK_REAL(&r, 9223372036854775808.0);
    run_dispose(&r);

    LHAT_TEST("and so does a difference");
    run_text(&r, "return^ -9223372036854775807 - 2\n");
    CHECK_REAL(&r, -9223372036854775809.0);
    run_dispose(&r);

    LHAT_TEST("and a product");
    run_text(&r, "return^ 4294967296 * 4294967296\n");
    CHECK_REAL(&r, 18446744073709551616.0);
    run_dispose(&r);

    // The one integer whose negation is not an integer.
    LHAT_TEST("and the negation that does not fit");
    run_text(&r, "return^ -(-9223372036854775807 - 1)\n");
    CHECK_REAL(&r, 9223372036854775808.0);
    run_dispose(&r);

    // What fits is untouched -- 20! is the largest that does.
    LHAT_TEST("what the integers do hold stays exact");
    run_text(&r,
             "let^ f = f^ n:number^ -> number^ {\n"
             "  if^ n < 2 { return^ 1 }\n"
             "  return^ n * this^(n - 1)\n"
             "}\n"
             "return^ f(20)\n");
    CHECK_INTEGER(&r, 2432902008176640000);
    run_dispose(&r);

    // A zero operand cannot overflow, and the check must not send it wide.
    LHAT_TEST("a zero product stays an integer");
    run_text(&r, "return^ 0 * 9223372036854775807\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 04 の 11.2: '/' is real division, which is what keeps ordinary
    // arithmetic out of the unions.
    LHAT_TEST("'/' is real division");
    run_text(&r, "return^ 7 / 2\n");
    CHECK_REAL(&r, 3.5);
    run_dispose(&r);

    LHAT_TEST("'//' floors");
    run_text(&r, "return^ 7 // 2\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("'//' floors towards minus infinity");
    run_text(&r, "return^ -7 // 2\n");
    CHECK_INTEGER(&r, -4);
    run_dispose(&r);

    LHAT_TEST("'%' agrees in sign with the divisor");
    run_text(&r, "return^ -7 % 2\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 04 の 11.2 again: only these two can fail.
    LHAT_TEST("'/' by zero does not fail");
    run_text(&r, "return^ 1 / 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    run_dispose(&r);

    // 04 の 11.2改: like an overflow (14.8改), a zero divisor widens to
    // real arithmetic instead of failing -- '//' and '%' answer inf/nan
    // the same way '/' already does.
    LHAT_TEST("'//' by zero widens to real and answers inf");
    run_text(&r, "return^ 1 // 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_real(r.ran.value), "expected a real");
    LHAT_CHECK(isinf(lhat_as_real(r.ran.value)), "expected inf");
    run_dispose(&r);

    LHAT_TEST("'%' by zero widens to real and answers nan");
    run_text(&r, "return^ 1 % 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_real(r.ran.value), "expected a real");
    LHAT_CHECK(isnan(lhat_as_real(r.ran.value)), "expected nan");
    run_dispose(&r);

    // 02 の 11.5 の (2): '**' binds tighter than a unary minus.
    LHAT_TEST("'-2 ** 2' is minus four");
    run_text(&r, "return^ -2 ** 2\n");
    CHECK_REAL(&r, -4.0);
    run_dispose(&r);

    // 5.1: the generic instruction checks what it was given.
    LHAT_TEST("arithmetic on a bool is refused at run time");
    run_text(&r, "return^ true^ + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);
}

static void test_names(void)
{
    Run r;

    // 02 の 8.6: let^ makes the name, ':=' reaches it.
    LHAT_TEST("a name holds its value");
    run_text(&r, "let^ x = 2\nlet^ y = 3\nreturn^ x * y\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("':=' reassigns rather than shadowing");
    run_text(&r, "let^ x = 1\nx := x + 41\nreturn^ x\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.6's whole point: the inner statement reaches the outer name.
    // 7.4改: 'target op= value' means 'target := target op value'.
    LHAT_TEST("every compound assignment operator");
    run_text(&r, "let^ x = 10\nx += 5\nreturn^ x\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    run_text(&r, "let^ x = 10\nx -= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    run_text(&r, "let^ x = 10\nx *= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    run_text(&r, "let^ x = 10\nx /= 4\nreturn^ x\n");
    CHECK_REAL(&r, 2.5);
    run_dispose(&r);

    run_text(&r, "let^ x = 10\nx %= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    run_text(&r, "let^ x = 10\nx //= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    run_text(&r, "let^ x = 2\nx **= 5\nreturn^ x\n");
    CHECK_REAL(&r, 32.0);
    run_dispose(&r);

    run_text(&r, "let^ x = \"ab\"\nx ..= \"cd\"\nreturn^ x = \"abcd\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 7.4改's whole reason to exist: the target is read once, not twice.
    // A path target proves it, since only there does re-evaluating the
    // target run something with a side effect.
    LHAT_TEST("a path target is evaluated once, not twice");
    run_text(&r,
             "let^ t = { 100, 200 }\n"
             "let^ calls = 0\n"
             "let^ idx = p^ { calls := calls + 1\nreturn^ 1 }\n"
             "t[idx()] += 5\n"
             "return^ t.1 = 105 and^ calls = 1\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and the same holds for a member target");
    run_text(&r,
             "let^ t = { x := 10 }\n"
             "t.x += 7\n"
             "return^ t.x\n");
    CHECK_INTEGER(&r, 17);
    run_dispose(&r);

    LHAT_TEST("':=' inside a block reaches out");
    run_text(&r, "let^ x = 1\ndo^{ x := 9 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("let^ inside a block does not");
    run_text(&r, "let^ x = 1\ndo^{ let^ x = 9 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 8.6: let^ present means define regardless of which spelling follows --
    // ':=' after let^ is accepted as a convenience, not a different meaning.
    LHAT_TEST("let^ also defines with ':='");
    run_text(&r, "let^ x := 1\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("an unknown name does not compile");
    run_text(&r, "return^ nowhere\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);
}

static void test_control(void)
{
    Run r;

    LHAT_TEST("an if statement takes the true branch");
    run_text(&r,
             "let^ x = 0\n"
             "if^ 1 < 2 { x := 10 else^: x := 20 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("and the false one");
    run_text(&r,
             "let^ x = 0\n"
             "if^ 1 > 2 { x := 10 else^: x := 20 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("an elseif chain picks one arm");
    run_text(&r,
             "let^ x = 0\n"
             "if^ 1 > 2 {\n"
             "    x := 1\n"
             "    elseif^ 2 > 1:\n"
             "        x := 2\n"
             "    else^:\n"
             "        x := 3\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 02 の 5.1: the expression form yields a value.
    LHAT_TEST("the if expression yields a value");
    run_text(&r, "let^ x = if^ 1 < 2: 7 el^: 8 ;\nreturn^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("comparisons produce bools");
    run_text(&r, "return^ 1 < 2\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 02 の 11.6: and^ and or^ settle without the right side when they can.
    LHAT_TEST("or^ leaves the right side alone once it is true");
    run_text(&r, "return^ true^ or^ nowhere\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("and^ is false when the left is");
    run_text(&r, "return^ false^ and^ true^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("or^ is true when the left is");
    run_text(&r, "return^ true^ or^ false^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("or^ falls through to the right");
    run_text(&r, "return^ false^ or^ true^\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // A condition has to be a bool; nothing else is a truth value.
    LHAT_TEST("a condition that is not a bool is refused");
    run_text(&r, "let^ x = 0\nif^ 1 { x := 1 }\nreturn^ x\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("a unit with no return^ yields nil^");
    run_text(&r, "let^ x = 1\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);
}

// 5.3: the callee and its arguments are contiguous, and exactly one value
// comes back -- 02 の 13.8 having removed multiple returns is why nothing has
// to be reconciled here.
static void test_calls(void)
{
    Run r;

    LHAT_TEST("a subroutine is called and answers");
    run_text(&r, "let^ twice = f^n { return^ n * 2 }\nreturn^ twice(21)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("arguments arrive in order");
    run_text(&r,
             "let^ less = f^a, b { return^ a - b }\n"
             "return^ less(10, 3)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 02 の 15.12: a function whose body is one expression answers with it,
    // with no return^ written. The parser turns it into one, so nothing here
    // or in the checker had to change.
    LHAT_TEST("a function whose body is one expression answers with it");
    run_text(&r,
             "let^ square = f^ n:number^ -> number^ { n * n }\n"
             "return^ square(7)\n");
    CHECK_INTEGER(&r, 49);
    run_dispose(&r);

    LHAT_TEST("and the expression may be one of 5.1");
    run_text(&r,
             "let^ sign = f^ n:number^ -> number^ { if^ n < 0: 0 el^: 1 ; }\n"
             "return^ sign(0 - 3) * 10 + sign(3)\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // A call too. Whether it answers anything is the checker's question, and
    // 13.2 already refused a f^ whose body was one call -- so this only makes
    // a refused body work.
    LHAT_TEST("and a call that is the whole body is the answer");
    run_text(&r,
             "let^ inner = f^ -> number^ { return^ 42 }\n"
             "let^ wrap = f^ -> number^ { inner() }\n"
             "return^ wrap()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.2 again: beside another statement it is a call standing alone, which
    // always meant "run this and drop what it answers".
    LHAT_TEST("but beside another statement it drops what it answers");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ bump = f^ -> number^ { log.n := log.n + 1  return^ 9 }\n"
             "let^ wrap = f^ -> number^ { bump()  return^ log.n }\n"
             "return^ wrap()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a call is an expression like any other");
    run_text(&r,
             "let^ one = f^ { return^ 1 }\n"
             "return^ one() + one() * 3\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 02 の 8.7: a name is visible across its whole scope, so a body may call
    // itself without anything declared ahead of it.
    LHAT_TEST("a subroutine reaches its own name");
    run_text(&r,
             "let^ fact = f^n {\n"
             "  if^ n <= 1 { return^ 1 }\n"
             "  return^ n * fact(n - 1)\n"
             "}\n"
             "return^ fact(5)\n");
    CHECK_INTEGER(&r, 120);
    run_dispose(&r);

    // 02 の 15.10: a body with no name still has one way to reach itself.
    LHAT_TEST("this^ reaches the subroutine running");
    run_text(&r,
             "let^ fact = f^n {\n"
             "  if^ n <= 1 { return^ 1 }\n"
             "  return^ n * this^(n - 1)\n"
             "}\n"
             "return^ fact(5)\n");
    CHECK_INTEGER(&r, 120);
    run_dispose(&r);

    LHAT_TEST("and it works where there is no name to use");
    run_text(&r,
             "let^ apply = f^ g, n { return^ g(n) }\n"
             "return^ apply(f^n { if^ n <= 1 { return^ 1 } "
             "return^ n + this^(n - 1) }, 4)\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // The innermost one, so an inner body does not reach the outer one.
    LHAT_TEST("this^ is the innermost subroutine");
    run_text(&r,
             "let^ outer = f^ {\n"
             "  let^ inner = f^n { if^ n <= 0 { return^ 0 } "
             "return^ 1 + this^(n - 1) }\n"
             "  return^ inner(3)\n"
             "}\n"
             "return^ outer()\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("this^ outside any subroutine does not compile");
    run_text(&r, "return^ this^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("a p^ with no return^ answers nil^");
    run_text(&r, "let^ nothing = p^ { }\nreturn^ nothing()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("calling something that is not a subroutine is a fault");
    run_text(&r, "let^ x = 1\nreturn^ x()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NOT_CALLABLE);
    run_dispose(&r);

    LHAT_TEST("the wrong number of arguments is a fault");
    run_text(&r, "let^ f = f^a, b { return^ a }\nreturn^ f(1)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // Nothing bounds the recursion, so the frames run out. 5.3 wants that
    // reported rather than reached by walking off the array.
    LHAT_TEST("frames that go too deep are reported, not walked off");
    run_text(&r, "let^ f = f^ { return^ f() }\nreturn^ f()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_STACK_OVERFLOW);
    run_dispose(&r);
}

// 5.4: a capture is a place, not a copy. 02 の 8.6 is what forces it -- ':='
// inside a nested body reassigns the outer binding, and a copy would lose the
// change.
static void test_closures(void)
{
    Run r;

    LHAT_TEST("a body reads a name from around it");
    run_text(&r,
             "let^ base = 10\n"
             "let^ add = f^n { return^ base + n }\n"
             "return^ add(5)\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("a ':=' inside a body reaches the outer binding");
    run_text(&r,
             "let^ count = 0\n"
             "let^ bump = p^ { count := count + 1 }\n"
             "bump()\n"
             "bump()\n"
             "return^ count\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("two bodies capturing one name share it");
    run_text(&r,
             "let^ n = 1\n"
             "let^ set = p^ { n := 9 }\n"
             "let^ get = f^ { return^ n }\n"
             "set()\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // The place outlives the frame it started in, which is what closing an
    // upvalue is for.
    LHAT_TEST("a captured place outlives the frame that held it");
    run_text(&r,
             "let^ counter = f^ {\n"
             "  let^ n = 0\n"
             "  return^ p^ { n := n + 1 return^ n }\n"
             "}\n"
             "let^ next = counter()\n"
             "next()\n"
             "next()\n"
             "return^ next()\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("two closures from one body get separate places");
    run_text(&r,
             "let^ counter = f^ {\n"
             "  let^ n = 0\n"
             "  return^ p^ { n := n + 1 return^ n }\n"
             "}\n"
             "let^ a = counter()\n"
             "let^ b = counter()\n"
             "a()\n"
             "a()\n"
             "return^ b()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 5.4's second case: the name is not in the immediate parent either, so
    // each level on the way down has to carry it.
    LHAT_TEST("a name is carried down through more than one level");
    run_text(&r,
             "let^ outer = f^ {\n"
             "  let^ n = 7\n"
             "  return^ f^ { return^ f^ { return^ n }() }\n"
             "}\n"
             "return^ outer()()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // A block's slots go back to the pool at its end, so a closure that
    // outlives the block has to have stopped sharing them by then. Without
    // that it would read whatever the block after it put there.
    LHAT_TEST("a place captured in a block survives the block");
    run_text(&r,
             "let^ get = f^ { return^ 0 }\n"
             "do^{\n"
             "  let^ n = 5\n"
             "  get := f^ { return^ n }\n"
             "}\n"
             "do^{\n"
             "  let^ other = 99\n"
             "  other := other\n"
             "}\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a parameter is captured like anything else");
    run_text(&r,
             "let^ adder = f^by { return^ f^n { return^ n + by } }\n"
             "let^ add3 = adder(3)\n"
             "return^ add3(4)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);
}

#define CHECK_STRING(r, expected)                                             \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING),   \
                   "expected a string");                                      \
        if (lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING)) {        \
            const LhatString *s =                                             \
                (const LhatString *)lhat_as_object((r)->ran.value);           \
            LHAT_CHECK(strcmp(s->text, (expected)) == 0,                      \
                       "got \"%s\", want \"%s\"", s->text, (expected));       \
        }                                                                     \
    } while (0)

static void test_strings(void)
{
    Run r;

    LHAT_TEST("a string literal is a value");
    run_text(&r, "return^ \"hello\"\n");
    CHECK_STRING(&r, "hello");
    run_dispose(&r);

    // 01 の 5 章: the escapes are resolved before the compiler sees them.
    LHAT_TEST("the escapes are already resolved");
    run_text(&r, "return^ \"a\\tb\"\n");
    CHECK_STRING(&r, "a\tb");
    run_dispose(&r);

    // 02 の 11.6: a string is what it says.
    LHAT_TEST("two strings spelling the same thing are equal");
    run_text(&r, "return^ \"foo\" = \"foo\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and different ones are not");
    run_text(&r, "return^ \"foo\" = \"bar\"\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 11.6改: is^ asks whether the two are the same object, not whether
    // they spell the same thing -- unlike '=', a string built at runtime is
    // never the same instance as one built separately even when it reads
    // the same.
    LHAT_TEST("is^ asks identity rather than spelling, even for strings");
    run_text(&r,
             "let^ a = \"f\" .. \"oo\"\n"
             "let^ b = \"f\" .. \"oo\"\n"
             "return^ a is^ b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("but a name for the same string is the same instance");
    run_text(&r, "let^ a = \"f\" .. \"oo\"\nlet^ b = a\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 02 の 11.2: '..' is concatenation in general; strings are the case
    // that is settled, and 14.5's composition of definitions is the other.
    LHAT_TEST("'..' joins two strings");
    run_text(&r, "return^ \"ab\" .. \"cd\"\n");
    CHECK_STRING(&r, "abcd");
    run_dispose(&r);

    LHAT_TEST("and joins a run of them right to left");
    run_text(&r, "return^ \"a\" .. \"b\" .. \"c\"\n");
    CHECK_STRING(&r, "abc");
    run_dispose(&r);

    LHAT_TEST("an empty string joins to nothing");
    run_text(&r, "return^ \"\" .. \"x\" .. \"\"\n");
    CHECK_STRING(&r, "x");
    run_dispose(&r);

    LHAT_TEST("the result is a string like any other");
    run_text(&r, "return^ (\"a\" .. \"b\") = \"ab\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("neither operand is changed");
    run_text(&r,
             "let^ a = \"one\"\n"
             "let^ b = a .. \"two\"\n"
             "return^ a\n");
    CHECK_STRING(&r, "one");
    run_dispose(&r);

    // 11.1: an operator is a function, and 11.3 asks the left operand for
    // it. A string answers built in; anything else answers with a member,
    // which 14.4 hands the left operand as self^.
    LHAT_TEST("a definition answers '..' with its own op^");
    run_text(&r,
             "let^ Vec = def^{\n"
             "  self^{ tag := \"v\" },\n"
             "  op^.. := f^self^, other:string^ -> string^ {\n"
             "    return^ self^.tag .. other\n"
             "  },\n"
             "}\n"
             "let^ v = Vec.new^()\n"
             "return^ v .. \"!\"\n");
    CHECK_STRING(&r, "v!");
    run_dispose(&r);

    LHAT_TEST("and what it answers is an ordinary value");
    run_text(&r,
             "let^ Vec = def^{\n"
             "  self^{ tag := \"v\" },\n"
             "  op^.. := f^self^, other:string^ -> string^ {\n"
             "    return^ self^.tag .. other\n"
             "  },\n"
             "}\n"
             "let^ v = Vec.new^()\n"
             "return^ (v .. \"!\") .. \"?\"\n");
    CHECK_STRING(&r, "v!?");
    run_dispose(&r);

    // 11.4改: the arithmetic operators ask the same question '..' does.
    LHAT_TEST("a definition answers arithmetic with its own op^");
    run_text(&r,
             "let^ Vec = def^{\n"
             "  self^{ n := 10 },\n"
             "  op^+ := f^self^, o:number^ -> number^ { return^ self^.n + o },\n"
             "  op^* := f^self^, o:number^ -> number^ { return^ self^.n * o },\n"
             "}\n"
             "let^ v = Vec.new^()\n"
             "return^ (v + 5) * 100 + (v * 3)\n");
    CHECK_INTEGER(&r, 1530);  // 15, then 30
    run_dispose(&r);

    // 14.8's number^ carries all seven built in, so ordinary arithmetic keeps
    // the instructions it had and pays nothing for the lookup.
    LHAT_TEST("numbers keep their own instructions");
    run_text(&r, "return^ 1 + 2 * 3 - 4\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("a structure with no op^ for it is refused");
    run_text(&r,
             "let^ t = { a := 1 }\n"
             "return^ t + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("a structure with no '..' cannot answer");
    run_text(&r,
             "let^ t = { a := 1 }\n"
             "let^ u = { b := 2 }\n"
             "return^ t .. u\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 11.3 leaves the rest to the operator's own definition, which needs
    // op^. 5.1 has the instruction refuse what it cannot do.
    LHAT_TEST("joining a number to a string is refused at run time");
    run_text(&r, "return^ \"a\" .. 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("arithmetic on a string is refused at run time");
    run_text(&r, "return^ \"a\" + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);
}

// 02 の 14 章: the one data structure.
static void test_tables(void)
{
    Run r;

    LHAT_TEST("an empty table is a table");
    run_text(&r, "let^ t = { }\nreturn^ t\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    run_dispose(&r);

    LHAT_TEST("a named member reads back");
    run_text(&r, "let^ t = { a := 1, b := 2 }\nreturn^ t.b\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 04 の 11.3: t.foo and t[k] differ in what the checker knows, not in
    // where the machine looks.
    LHAT_TEST("the two spellings reach one place");
    run_text(&r, "let^ t = { a := 1 }\nreturn^ t[\"a\"]\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a positional entry counts from one");
    run_text(&r, "let^ t = { 10, 20, 30 }\nreturn^ t.1 + t.3\n");
    CHECK_INTEGER(&r, 40);
    run_dispose(&r);

    LHAT_TEST("keyed and positional entries mix");
    run_text(&r, "let^ t = { 10, a := 1, 20 }\nreturn^ t.2 + t.a\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    LHAT_TEST("a value may be any expression");
    run_text(&r, "let^ n = 3\nlet^ t = { a := n * 2 }\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("tables nest");
    run_text(&r, "let^ t = { a := { b := 5 } }\nreturn^ t.a.b\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 14.14改: an entry introduces a member, which 8.6 spells '='. What that
    // costs is that a comparison standing on its own has to say so.
    LHAT_TEST("an entry written with '=' is a member, not a comparison");
    run_text(&r,
             "let^ a = 1\n"
             "let^ t = { a = 0, b = 2 }\n"
             "return^ t.a * 10 + t.b\n");
    CHECK_INTEGER(&r, 2);  // t.a is 0, not the comparison's false^
    run_dispose(&r);

    LHAT_TEST("and one in brackets is the comparison");
    run_text(&r,
             "let^ a = 1\n"
             "let^ t = { (a = 1), (a = 2) }\n"
             "if^ t[1] {\n"
             "  if^ t[2] { return^ 0 }\n"
             "  return^ 1\n"
             "}\n"
             "return^ 2\n");
    CHECK_INTEGER(&r, 1);  // true^ then false^
    run_dispose(&r);

    // 11.3: a table is a mapping, so there is no out of range -- only a key
    // that is there and one that is not.
    LHAT_TEST("a missing key answers nil^ rather than failing");
    run_text(&r, "let^ t = { }\nreturn^ t[\"nowhere\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("a member is a place that ':=' reaches");
    run_text(&r, "let^ t = { a := 1 }\nt.a := 9\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("':=' may make a member that was not there");
    run_text(&r, "let^ t = { }\nt.a := 7\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("an index is a place too");
    run_text(&r, "let^ t = { }\nlet^ k = \"key\"\nt[k] := 4\nreturn^ t[\"key\"]\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    LHAT_TEST("storing nil^ removes the key");
    run_text(&r, "let^ t = { a := 1 }\nt.a := nil^\nreturn^ t[\"a\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("nil^ cannot be a key");
    run_text(&r, "let^ t = { }\nlet^ k = nil^\nt[k] := 1\nreturn^ t\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_KEY);
    run_dispose(&r);

    LHAT_TEST("indexing something that is not a table is refused");
    run_text(&r, "let^ n = 1\nreturn^ n[\"a\"]\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 14.2 makes a table's identity what it is, so two literals of the same
    // shape are two tables.
    LHAT_TEST("a table is equal only to itself");
    run_text(&r, "let^ a = { x := 1 }\nlet^ b = { x := 1 }\nreturn^ a = b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and a name for one is the same one");
    run_text(&r, "let^ a = { x := 1 }\nlet^ b = a\nreturn^ a = b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 11.6改: '=' and 'is^' agree here since 14.2 already makes a table's
    // '=' an identity comparison -- 'is^' means to keep meaning that once
    // '=' moves to comparing a table's contents instead (03 の 7 章、P7).
    LHAT_TEST("'is^' agrees with '=' on tables for now");
    run_text(&r, "let^ a = { x := 1 }\nlet^ b = { x := 1 }\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and a name for one is the same instance under 'is^' too");
    run_text(&r, "let^ a = { x := 1 }\nlet^ b = a\nreturn^ a is^ b\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // A table is a reference, so the two names reach one table (Memo.md の
    // 「シンボルはすべて参照」).
    LHAT_TEST("a table is shared rather than copied");
    run_text(&r, "let^ a = { x := 1 }\nlet^ b = a\nb.x := 5\nreturn^ a.x\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a table passes into a subroutine as itself");
    run_text(&r,
             "let^ bump = p^t { t.n := t.n + 1 }\n"
             "let^ t = { n := 0 }\n"
             "bump(t)\n"
             "bump(t)\n"
             "return^ t.n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a subroutine may be a member");
    run_text(&r,
             "let^ t = { twice := f^n { return^ n * 2 } }\n"
             "return^ t.twice(4)\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);
}

// 16.5: repeat^ is the one that carries no focus.
static void test_repeat(void)
{
    Run r;

    LHAT_TEST("repeat^ n runs n times");
    run_text(&r, "let^ x = 0\nrepeat^ 5 { x := x + 1 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("repeat^ 0 runs none");
    run_text(&r, "let^ x = 0\nrepeat^ 0 { x := x + 1 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // The count says how many times, so changing it inside cannot move the
    // finishing line.
    LHAT_TEST("the count is read once");
    run_text(&r,
             "let^ n = 3\n"
             "let^ x = 0\n"
             "repeat^ n { n := 100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("repeat^ while^ tests before the body");
    run_text(&r,
             "let^ i = 0\n"
             "repeat^ while^ i < 4 { i := i + 1 }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    LHAT_TEST("a condition that is false at the start runs nothing");
    run_text(&r,
             "let^ x = 0\n"
             "repeat^ while^ false^ { x := 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 16.5: until^ is while^ negated, tested in the same place.
    LHAT_TEST("repeat^ until^ is while^ negated");
    run_text(&r,
             "let^ i = 0\n"
             "repeat^ until^ i ≧ 4 { i := i + 1 }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.6改: '[ ... ] :=' builds an entry under a key that is not a name.
    LHAT_TEST("a computed key lands where it says");
    run_text(&r, "let^ t = { [0] := 7 }\nreturn^ t[0]\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and the key is an ordinary expression");
    run_text(&r,
             "let^ k = 3\n"
             "let^ t = { [k + 1] := 5 }\n"
             "return^ t[4]\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 14 章 follows Lua here: t.a and t["a"] are one key.
    LHAT_TEST("a string key and a name are the same entry");
    run_text(&r, "let^ t = { [\"a\"] := 1 }\nreturn^ t.a\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and it reads back the other way round");
    run_text(&r, "let^ t = { a := 2 }\nreturn^ t[\"a\"]\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // A keyed entry takes no place in the sequence, so the positional ones
    // carry on counting past it.
    LHAT_TEST("a keyed entry takes no position from the sequence");
    run_text(&r,
             "let^ t = { 10, [\"k\"] := 20, 30 }\n"
             "return^ t[2]\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    LHAT_TEST("and is still reachable by its key");
    run_text(&r,
             "let^ t = { 10, [\"k\"] := 20, 30 }\n"
             "return^ t[\"k\"]\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    // 04 の 11.3: what the checker cannot see coming, the machine refuses.
    LHAT_TEST("a key that turns out to be nil^ is a fault");
    run_text(&r,
             "let^ x : number^|nil^ = nil^\n"
             "let^ t = { [x] := 1 }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_KEY);
    run_dispose(&r);

    LHAT_TEST("repeat^ on its own needs break^ to end");
    run_text(&r,
             "let^ i = 0\n"
             "repeat^ { i := i + 1 if^ i = 3 { break^ } }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // Or a return^, which leaves the body and the loop with it. The checker
    // reads this as a body whose end is unreachable (03 の 3.4), so what it
    // blesses has to run.
    LHAT_TEST("a return^ leaves an endless repeat^ too");
    run_text(&r,
             "let^ f = f^ -> number^ { repeat^ { return^ 7 } }\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("break^ leaves only the loop it is in");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 9.8: one loop to leave and none around it is the same mistake as
    // naming more than are there, so it is the same diagnostic -- not the
    // "does not compile yet" a form still waiting on the compiler gets.
    LHAT_TEST("break^ outside a loop is a break^ reaching too far");
    run_text(&r, "break^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    // 02 の 9.8: the hats count the loops to leave, so break^ is one and
    // break^^ is two. Written this way the level is read off the word
    // itself, which is why a plain break^ needs nothing added to it.
    LHAT_TEST("break^^ leaves two loops");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^^ }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 9.8: Memo.md L500's bracketed form says the same number, so the two
    // spellings are one thing and neither is the primary one.
    LHAT_TEST("and break^[2] says the same");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 { n := n + 1 break^[2] }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("three loops and break^^^ leaves all of them");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 3 {\n"
             "  repeat^ 3 {\n"
             "    repeat^ 3 { n := 7 break^^^ }\n"
             "    n := n + 100\n"
             "  }\n"
             "  n := n + 10\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // Asking for more loops than stand here is written down wrong rather
    // than quietly clamped to the outermost one.
    LHAT_TEST("a level past the loops that are there is refused by name");
    run_text(&r, "repeat^ 3 { break^^^ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("and one loop too many is enough to be too far");
    run_text(&r, "repeat^ 3 { repeat^ 3 { repeat^ 3 { break^^^^ } } }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("the bracketed spelling is refused the same way");
    run_text(&r, "repeat^ 3 { repeat^ 3 { break^[3] } }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_BREAK_TOO_FAR);
    run_dispose(&r);

    // 9.8: the loop the level names ends the way it would have ended on its
    // own, so its epilog^ runs. The ones passed through are left rather than
    // ended, and theirs do not.
    LHAT_TEST("the loop named ends normally, the ones passed through do not");
    run_text(&r,
             "let^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^^\n"
             "  epilog^:\n"
             "    log.s := log.s .. \"in,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "out,");
    run_dispose(&r);

    LHAT_TEST("and one level runs both, being the inner loop's own end");
    run_text(&r,
             "let^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^\n"
             "  epilog^:\n"
             "    log.s := log.s .. \"in,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "in,out,");
    run_dispose(&r);

    // 9.8 with 10.7: what a loop is only passed through still has to give
    // back what it took, so its finally^ runs where its epilog^ does not.
    LHAT_TEST("a finally^ passed through still runs");
    run_text(&r,
             "let^ log = { s := \"\" }\n"
             "repeat^ 1 {\n"
             "  repeat^ 1 {\n"
             "    break^^\n"
             "  finally^:\n"
             "    log.s := log.s .. \"fin,\"\n"
             "  }\n"
             "epilog^:\n"
             "  log.s := log.s .. \"out,\"\n"
             "}\n"
             "return^ log.s\n");
    CHECK_STRING(&r, "fin,out,");
    run_dispose(&r);

    // 16.5: an endless repeat^ ends the statements around it unless
    // something leaves it, and a break^ from inside a nested loop is one --
    // which is what the level is for.
    LHAT_TEST("a level reaching out of an endless repeat^ is a way out");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ {\n"
             "  repeat^ 3 { n := 5 break^^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);
}

// 16.1: for^ is where the value being looked at is defined. Whether it
// repeats is up to the clause that follows.
static void test_for(void)
{
    Run r;

    LHAT_TEST("to^ counts up and includes the limit");
    run_text(&r,
             "let^ total = 0\n"
             "for^ let^ i := 1 to^ 4 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // 16.4: the direction is not inferred, so this is empty rather than
    // counting down.
    LHAT_TEST("a limit below the start runs none");
    run_text(&r,
             "let^ x = 0\n"
             "for^ let^ i := 1 to^ 0 { x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("downto^ counts down");
    run_text(&r,
             "let^ seen = 0\n"
             "for^ let^ i := 3 downto^ 1 { seen := seen * 10 + i }\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 321);
    run_dispose(&r);

    // 16.4: step^ is a positive amount either way; the sign belongs to the
    // clause.
    LHAT_TEST("step^ is a positive amount for both directions");
    run_text(&r,
             "let^ up = 0\n"
             "for^ let^ i := 1 to^ 9 step^ 3 { up := up * 10 + i }\n"
             "return^ up\n");
    CHECK_INTEGER(&r, 147);
    run_dispose(&r);

    run_text(&r,
             "let^ down = 0\n"
             "for^ let^ i := 9 downto^ 1 step^ 3 { down := down * 10 + i }\n"
             "return^ down\n");
    CHECK_INTEGER(&r, 963);
    run_dispose(&r);

    // 16.4: the bound says how far the loop goes, so it is read before the
    // loop starts and cannot move while it runs.
    LHAT_TEST("the bound is read once");
    run_text(&r,
             "let^ n = 3\n"
             "let^ x = 0\n"
             "for^ let^ i := 1 to^ n { n := 100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and downto^ reads its bound once too");
    run_text(&r,
             "let^ n = 1\n"
             "let^ x = 0\n"
             "for^ let^ i := 3 downto^ n { n := -100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and step^ is read once as well");
    run_text(&r,
             "let^ s = 1\n"
             "let^ n = 0\n"
             "for^ let^ i := 1 to^ 9 step^ s { n := n + 1 s := 3 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 9);  // reading s each time round would give three
    run_dispose(&r);

    LHAT_TEST("10 to^ 1 step^ 2 is empty rather than confusing");
    run_text(&r,
             "let^ x = 0\n"
             "for^ let^ i := 10 to^ 1 step^ 2 { x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 16.2: a focus with no name written is still a focus.
    LHAT_TEST("an unnamed focus is reached through it^");
    run_text(&r,
             "let^ total = 0\n"
             "for^ 1 to^ 4 { total := total + it^ }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("while^ tests before the body and next^ runs after it");
    run_text(&r,
             "let^ total = 0\n"
             "for^ let^ i := 1 while^ i ≦ 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("until^ is the same the other way round");
    run_text(&r,
             "let^ total = 0\n"
             "for^ let^ i := 1 until^ i > 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("the focus is gone after the loop");
    run_text(&r, "for^ let^ i := 1 to^ 2 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("loops nest");
    run_text(&r,
             "let^ n = 0\n"
             "for^ let^ i := 1 to^ 3 {\n"
             "  for^ let^ j := 1 to^ 4 { n := n + 1 }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 16.3: `in^ e` asks e for the coroutine to walk. A table answers with
    // one over its keys, so the dense part comes back in index order.
    LHAT_TEST("in^ walks a table's dense part in order");
    run_text(&r,
             "let^ t = { 10, 20, 30 }\n"
             "let^ seen = 0\n"
             "for^ k, v in^ t { seen := seen * 100 + k * 10 + v // 10 }\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 112233);
    run_dispose(&r);

    LHAT_TEST("and reaches the keyed part too");
    run_text(&r,
             "let^ t = { a := 5, b := 7 }\n"
             "let^ total = 0\n"
             "for^ k, v in^ t { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("an empty table walks no turns");
    run_text(&r,
             "let^ t = { }\n"
             "let^ n = 0\n"
             "for^ k, v in^ t { n := n + 1 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 13.10: one name takes the value whole, several take it apart by
    // position. in^ is the marker, so no unpack^ is written.
    LHAT_TEST("one name takes what was yielded whole");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "let^ total = 0\n"
             "for^ v in^ gen() { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("a coroutine answers iterate with itself");
    run_text(&r,
             "let^ gen = p^ { yield^ 4 yield^ 5 }\n"
             "let^ c = gen()\n"
             "let^ total = 0\n"
             "for^ v in^ c { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // Anything with an iterate answers, which is what makes the rule a
    // convention rather than a special case for tables.
    LHAT_TEST("a definition answers by writing iterate");
    run_text(&r,
             "let^ Range = def^{\n"
             "  self^{ upto := 0 },\n"
             "  new^ := f^ n { return^ self^{ upto := n } },\n"
             "  iterate := f^self^ {\n"
             "    let^ limit = self^.upto\n"
             "    return^ p^ { for^ let^ i := 1 to^ limit { yield^ i } }()\n"
             "  },\n"
             "}\n"
             "let^ total = 0\n"
             "for^ v in^ Range.new^(4) { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("a written iterate wins over the built-in one");
    run_text(&r,
             "let^ t = { 1, 2, 3, iterate := f^ { return^ p^ { yield^ 9 }() } }\n"
             "let^ total = 0\n"
             "for^ v in^ t { total := total + v }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 16.3 puts a table's iterate() on the same footing as any other
    // coroutine, so what it answers has to be drivable by hand and not only
    // by the loop -- which emits BC_RESUME rather than going through the
    // members. Before this, start() built a frame out of a walk that has no
    // closure and the machine read through a null pointer.
    LHAT_TEST("a walk taken by hand starts like any other coroutine");
    run_text(&r,
             "let^ t = { 10, 20 }\n"
             "let^ w = t.iterate()\n"
             "let^ pair = w.start()\n"
             "return^ pair[1] * 100 + pair[2]\n");
    CHECK_INTEGER(&r, 110);  // key 1, value 10
    run_dispose(&r);

    LHAT_TEST("and resumes to the pairs after it");
    run_text(&r,
             "let^ t = { 10, 20 }\n"
             "let^ w = t.iterate()\n"
             "w.start()\n"
             "let^ pair = w.resume(nil^)\n"
             "return^ pair[1] * 100 + pair[2]\n");
    CHECK_INTEGER(&r, 220);  // key 2, value 20
    run_dispose(&r);

    LHAT_TEST("and finishes when the table runs out");
    run_text(&r,
             "let^ t = { 10 }\n"
             "let^ w = t.iterate()\n"
             "w.start()\n"
             "let^ last = w.resume(nil^)\n"
             "let^ n = last ?? 0\n"
             "if^ w.done() { n := n + 1 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);  // nil^, and done
    run_dispose(&r);

    // 15.2改 applies to a walk unchanged: nothing about having no body makes
    // the first resume mean something on its own.
    LHAT_TEST("resuming a walk that has not started is a fault");
    run_text(&r,
             "let^ t = { 10 }\n"
             "let^ w = t.iterate()\n"
             "w.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_NOT_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting a walk twice is a fault");
    run_text(&r,
             "let^ t = { 10, 20 }\n"
             "let^ w = t.iterate()\n"
             "w.start()\n"
             "w.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    // 10.7: a walk has nothing pending, so disposal is only the state.
    LHAT_TEST("a walk in progress can be disposed");
    run_text(&r,
             "let^ t = { 10, 20, 30 }\n"
             "let^ w = t.iterate()\n"
             "w.start()\n"
             "w.dispose()\n"
             "return^ w.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("break^ leaves a walk like any other loop");
    run_text(&r,
             "let^ t = { 1, 2, 3, 4 }\n"
             "let^ n = 0\n"
             "for^ k, v in^ t { n := n + 1 if^ n = 2 { break^ } }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("the clauses of 9 章 apply to a walk too");
    run_text(&r,
             "let^ t = { 1, 2, 3 }\n"
             "let^ log = { s := 0 }\n"
             "for^ k, v in^ t {\n"
             "  main^:\n"
             "    log.s := log.s + v\n"
             "  epilog^:\n"
             "    log.s := log.s * 10\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 60);
    run_dispose(&r);

    LHAT_TEST("the focus is gone after the walk");
    run_text(&r, "let^ t = { 1 }\nfor^ k, v in^ t { }\nreturn^ v\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("walking something with no iterate is refused at run time");
    run_text(&r, "let^ n = 1\nfor^ v in^ n { }\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    // 16.3 and 16.1: this one does not repeat. It is the do^ block written
    // without the extra nesting.
    LHAT_TEST("if^ uses the focus once and does not repeat");
    run_text(&r,
             "let^ x = 0\n"
             "for^ let^ i := 1, let^ j := 2 if^ i + j < 10 { x := i + j }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and its focus does not escape either");
    run_text(&r, "for^ let^ i := 1 if^ i > 0 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.2改: '{' opens the statement form and ':' the expression one, here as
    // anywhere -- 16.1 has for^ take the form its clause does.
    LHAT_TEST("and it answers a value when written with ':'");
    run_text(&r,
             "let^ x = for^ let^ i := 1, let^ j := 2 if^ i + j < 10: i + j el^: 0 ;\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 16.7 is not against it: the name still does not leave, only the value
    // built from it, which is what an expression is.
    LHAT_TEST("and the focus still does not escape");
    run_text(&r,
             "let^ x = for^ let^ i := 1 if^ i > 0: i el^: 0 ;\n"
             "return^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 14 章 の table with 16: what a loop is mostly for.
    LHAT_TEST("a loop fills a table");
    run_text(&r,
             "let^ t = { }\n"
             "for^ let^ i := 1 to^ 4 { t[i] := i * i }\n"
             "return^ t.3\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);
}

// 9 章: the five clauses, and 9.8's three strengths of exit.
static void test_loop_clauses(void)
{
    Run r;

    // 9.4: what prolog^ declares lives as long as the loop, without leaking
    // out of it.
    LHAT_TEST("prolog^ runs once and its names last the whole loop");
    run_text(&r,
             "let^ out = 0\n"
             "repeat^ 4 {\n"
             "  prolog^:\n"
             "    let^ total = 0\n"
             "  main^:\n"
             "    total := total + 1\n"
             "  epilog^:\n"
             "    out := total\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 9.1: prolog^ runs whether the condition ever holds or not.
    LHAT_TEST("prolog^ and epilog^ run even when the body does not");
    run_text(&r,
             "let^ seen = 0\n"
             "repeat^ 0 {\n"
             "  prolog^:\n"
             "    seen := seen + 1\n"
             "  main^:\n"
             "    seen := seen + 100\n"
             "  epilog^:\n"
             "    seen := seen + 10\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 9.10: pre^ runs at the head of every turn, before the condition is
    // tested -- so the body runs once however the condition comes out. This
    // is the shape C spells do ... while.
    LHAT_TEST("pre^ runs even when the condition never holds");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("where main^ under the same condition never does");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // With both, the test sits between them: pre^ takes one more turn than
    // main^, since the turn whose test failed still ran its pre^.
    LHAT_TEST("pre^ and main^ straddle the condition");
    run_text(&r,
             "let^ n = 0\n"
             "let^ i = 0\n"
             "repeat^ while^ i < 3 {\n"
             "  pre^:\n"
             "    n := n + 10\n"
             "    i := i + 1\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 32);  // three pre^, two main^
    run_dispose(&r);

    LHAT_TEST("premain^ is the same clause");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  premain^:\n"
             "    n := n + 7\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 9.1 is unchanged: first^ and last^ belong to the turns the condition
    // accepted, and pre^ does not make one of those.
    LHAT_TEST("a turn that only ran pre^ is not one first^ or last^ counts");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ while^ false^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "  first^:\n"
             "    n := n + 100\n"
             "  last^:\n"
             "    n := n + 1000\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 9.8: break^ leaves from inside pre^ like from anywhere else.
    LHAT_TEST("break^ leaves a loop from inside pre^");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ {\n"
             "  pre^:\n"
             "    n := n + 1\n"
             "    if^ n = 3 { break^ }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // The declaration goes in prolog^, which is what makes reading before the
    // test writable at all -- 9.4 gives prolog^ names the whole loop.
    LHAT_TEST("prolog^ and pre^ together are the loop-and-a-half");
    run_text(&r,
             "let^ out = 0\n"
             "let^ i = 0\n"
             "repeat^ while^ i < 3 {\n"
             "  prolog^:\n"
             "    let^ seen = 0\n"
             "  pre^:\n"
             "    i := i + 1\n"
             "  main^:\n"
             "    seen := seen + i\n"
             "  epilog^:\n"
             "    out := seen\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 3);  // 1 + 2; the turn that read 3 failed the test
    run_dispose(&r);

    LHAT_TEST("first^ runs at the head of the first iteration only");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 3 {\n"
             "  first^:\n"
             "    n := n + 100\n"
             "  main^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 103);
    run_dispose(&r);

    // 9.1: first^ and last^ do not run when the condition never holds.
    LHAT_TEST("first^ and last^ stay away when nothing ran");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 0 {\n"
             "  first^:\n"
             "    n := n + 1\n"
             "  main^:\n"
             "    n := n + 1\n"
             "  last^:\n"
             "    n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 9.7: the whole reason last^ keeps a copy. Naively it would see 5.
    LHAT_TEST("last^ sees the value the condition last accepted");
    run_text(&r,
             "let^ seen = 0\n"
             "for^ let^ i := 1 to^ 4 {\n"
             "  last^:\n"
             "    seen := i\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 9.8: break^ is a normal end, so last^ and epilog^ both run, and the
    // copy taken at the head of the iteration is the right one.
    LHAT_TEST("break^ leaves last^ looking at the current iteration");
    run_text(&r,
             "let^ seen = 0\n"
             "for^ let^ i := 1 to^ 10 {\n"
             "  main^:\n"
             "    if^ i = 5 { break^ }\n"
             "  last^:\n"
             "    seen := i\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("break^ runs epilog^ too");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 10 {\n"
             "  main^:\n"
             "    break^\n"
             "  epilog^:\n"
             "    n := 7\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 9.8: return^ is not a normal end for the loop, so neither runs.
    LHAT_TEST("return^ runs neither last^ nor epilog^");
    run_text(&r,
             "let^ out = { n := 0 }\n"
             "let^ go = f^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      return^ 1\n"
             "    last^:\n"
             "      out.n := out.n + 10\n"
             "    epilog^:\n"
             "      out.n := out.n + 100\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ out.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 9.4: a name made in main^ lives one iteration, so the next one starts
    // over rather than seeing the last.
    LHAT_TEST("what main^ declares lasts one iteration");
    run_text(&r,
             "let^ out = 0\n"
             "repeat^ 3 {\n"
             "  let^ each = 0\n"
             "  each := each + 1\n"
             "  out := each\n"
             "}\n"
             "return^ out\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("the clauses of a loop do not leak out of it");
    run_text(&r,
             "repeat^ 1 {\n"
             "  prolog^:\n"
             "    let^ total = 0\n"
             "  main^:\n"
             "    total := 1\n"
             "}\n"
             "return^ total\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.4 with 9.4: the focus is one place per loop, not per iteration, so a
    // closure made inside sees where it ended up.
    LHAT_TEST("a closure made in a loop captures the place, not the moment");
    run_text(&r,
             "let^ get = f^ { return^ 0 }\n"
             "for^ let^ i := 1 to^ 3 {\n"
             "  get := f^ { return^ i }\n"
             "}\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);
}

// 04: errors are values. 5.6 wants no unwinding for any of this, and none of
// it needs any.
static void test_errors(void)
{
    Run r;

    LHAT_TEST("an error is a value like any other");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "let^ e = error^IOError.NotFound{ message := \"no such file\" }\n"
             "return^ e.message\n");
    CHECK_STRING(&r, "no such file");
    run_dispose(&r);

    // 2.3: every kind gets message and cause without declaring either.
    LHAT_TEST("message defaults to the empty string");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError.NotFound{ }.message\n");
    CHECK_STRING(&r, "");
    run_dispose(&r);

    LHAT_TEST("cause defaults to nil^");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError.NotFound{ }.cause\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("a declared field comes back");
    run_text(&r,
             "errordef^ ParseError { Syntax { line : number^, column : number^ }, Eof }\n"
             "let^ e = error^ParseError.Syntax{ line := 3, column := 12 }\n"
             "return^ e.line * 100 + e.column\n");
    CHECK_INTEGER(&r, 312);
    run_dispose(&r);

    // 2.2: a default is an expression evaluated at each construction, not a
    // value stored once.
    LHAT_TEST("a field left out takes its default");
    run_text(&r,
             "errordef^ ParseError { Syntax { line := 0, column := 0 } }\n"
             "let^ e = error^ParseError.Syntax{ line := 7 }\n"
             "return^ e.line * 100 + e.column\n");
    CHECK_INTEGER(&r, 700);
    run_dispose(&r);

    LHAT_TEST("the default is evaluated at each construction");
    run_text(&r,
             "let^ n = 0\n"
             "let^ next = f^ { n := n + 1 return^ n }\n"
             "errordef^ E { K { seq := next() } }\n"
             "let^ a = error^E.K{ }\n"
             "let^ b = error^E.K{ }\n"
             "return^ b.seq\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 2.4: two declarations may spell a kind the same and still be different
    // kinds. This is the whole reason identity is the declaration site.
    LHAT_TEST("the same spelling in two declarations is two kinds");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "errordef^ UserError { NotFound }\n"
             "let^ e = error^IOError.NotFound{ }\n"
             "return^ e isa^ UserError.NotFound\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("and it is the kind it was made from");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "errordef^ UserError { NotFound }\n"
             "let^ e = error^IOError.NotFound{ }\n"
             "return^ e isa^ IOError.NotFound\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 2.3: naming the declaration asks about the union of its kinds.
    LHAT_TEST("naming the declaration asks about all of its kinds");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "let^ e = error^IOError.Denied{ }\n"
             "return^ e isa^ IOError\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a sibling kind is not the one it is");
    run_text(&r,
             "errordef^ IOError { NotFound, Denied }\n"
             "let^ e = error^IOError.Denied{ }\n"
             "return^ e isa^ IOError.NotFound\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("something that is not an error is not a kind either");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ 1 isa^ IOError\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a kind that was never declared does not compile");
    run_text(&r, "return^ error^Nowhere.Missing{ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 2.3: the declaration is the union, not a type to construct.
    LHAT_TEST("the declaration itself cannot be constructed");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "return^ error^IOError{ }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);
}

// 04 の 4 章 and 5 章, and 02 の 11.7.
static void test_catch_and_try(void)
{
    Run r;

    LHAT_TEST("catch^ replaces an error with its right side");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ fail = f^ { return^ error^E.Bad{ } }\n"
             "return^ fail() catch^ 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("and leaves a value that is not an error alone");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ ok = f^ { return^ 7 }\n"
             "return^ ok() catch^ 0\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 4.2: the error is it^ inside the right side, the same word 02 の 16.2
    // uses for a focus.
    LHAT_TEST("the caught error is it^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ fail = f^ { return^ error^E.Bad{ message := \"oh\" } }\n"
             "return^ fail() catch^ it^.message\n");
    CHECK_STRING(&r, "oh");
    run_dispose(&r);

    LHAT_TEST("it^ tells the kinds apart");
    run_text(&r,
             "errordef^ E { A, B }\n"
             "let^ fail = f^ { return^ error^E.B{ } }\n"
             "return^ fail() catch^ if^ it^ isa^ E.A: 1 el^: 2 ;\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 4.3: catch^ binds tighter than the arithmetic around it, so what fails
    // is the call and not the sum.
    LHAT_TEST("catch^ attaches to the operation, not the sum");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ fail = f^ { return^ error^E.Bad{ } }\n"
             "return^ 10 + fail() catch^ 5\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("it^ is gone again after the catch^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ ok = f^ { return^ 1 }\n"
             "let^ x = ok() catch^ 0\n"
             "return^ it^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 5.1: try^ hands the error to the caller and carries on otherwise.
    LHAT_TEST("try^ returns the error from the procedure that wrote it");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ inner = f^ { return^ error^E.Bad{ message := \"deep\" } }\n"
             "let^ outer = f^ {\n"
             "  let^ v = try^ inner()\n"
             "  return^ v + 1\n"
             "}\n"
             "return^ outer().message\n");
    CHECK_STRING(&r, "deep");
    run_dispose(&r);

    LHAT_TEST("and gets out of the way when there is no error");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ inner = f^ { return^ 41 }\n"
             "let^ outer = f^ { return^ try^ inner() + 1 }\n"
             "return^ outer()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 5.2: try^ leaves the way return^ does, so a loop's last^ and epilog^
    // do not run.
    LHAT_TEST("try^ leaving a loop runs neither last^ nor epilog^");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ out = { n := 0 }\n"
             "let^ fail = f^ { return^ error^E.Bad{ } }\n"
             "let^ go = f^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      let^ v = try^ fail()\n"
             "    last^:\n"
             "      out.n := out.n + 10\n"
             "    epilog^:\n"
             "      out.n := out.n + 100\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ out.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("try^ stands as a statement when the value is not wanted");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ fail = f^ { return^ error^E.Bad{ message := \"gone\" } }\n"
             "let^ go = f^ { try^ fail() return^ 0 }\n"
             "return^ go().message\n");
    CHECK_STRING(&r, "gone");
    run_dispose(&r);

    // 02 の 11.7: '??' is the same shape, asking about nil^.
    // The escape keeps "??'" from being read as a trigraph for '^'.
    LHAT_TEST("'?\?' supplies a value for a missing key");
    run_text(&r, "let^ t = { }\nreturn^ t[\"nowhere\"] ?? 5\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("and leaves a key that is there alone");
    run_text(&r, "let^ t = { a := 1 }\nreturn^ t[\"a\"] ?? 5\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 11.7 asks about nil^ and nothing else, so false^ is a value that is
    // there rather than one that is missing.
    LHAT_TEST("'?\?' asks about nil^, not about being false");
    run_text(&r, "let^ t = { a := false^ }\nreturn^ t[\"a\"] ?? true^\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);
}

// 02 の 10 章 and 12 章: the cleanups, which 5.5 makes one mechanism.
static void test_cleanups(void)
{
    Run r;

    LHAT_TEST("finally^ runs when the block ends");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "do^{\n"
             "  log.n := 1\n"
             "finally^:\n"
             "  log.n := log.n + 10\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 10.2: however the block is left.
    LHAT_TEST("finally^ runs when return^ leaves through it");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ go = p^ {\n"
             "  do^{\n"
             "    return^ 1\n"
             "  finally^:\n"
             "    log.n := 7\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and when break^ leaves through it");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "repeat^ 5 {\n"
             "  do^{\n"
             "    log.n := log.n + 1\n"
             "    break^\n"
             "  finally^:\n"
             "    log.n := log.n + 10\n"
             "  }\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 04 の 5.2: try^ leaves the way return^ does, so finally^ still runs.
    LHAT_TEST("and when try^ hands an error to the caller");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "let^ log = { n := 0 }\n"
             "let^ fail = f^ { return^ error^E.Bad{ } }\n"
             "let^ go = p^ {\n"
             "  do^{\n"
             "    let^ v = try^ fail()\n"
             "  finally^:\n"
             "    log.n := 3\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.1: a p^ body is a block like any other.
    LHAT_TEST("a procedure body may carry a finally^");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ go = p^ {\n"
             "  return^ 1\n"
             "finally^:\n"
             "  log.n := 5\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.4: innermost first, which is the order the frame drains in.
    LHAT_TEST("nested finally^ run from the inside out");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ go = p^ {\n"
             "  do^{\n"
             "    do^{\n"
             "      return^ 1\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 1\n"
             "    }\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("a finally^ that was already run is not run again");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ go = p^ {\n"
             "  do^{\n"
             "    log.n := log.n + 1\n"
             "  finally^:\n"
             "    log.n := log.n + 10\n"
             "  }\n"
             "  return^ 0\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 10.5: Java lets a finally return and silently replace the answer. C#
    // refuses; so does this.
    LHAT_TEST("return^ inside a finally^ does not compile");
    run_text(&r, "do^{\n  let^ x = 1\nfinally^:\n  return^ 2\n}\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
    run_dispose(&r);

    // 9.2 and 10.9: finally^ comes after the loop's own clauses.
    LHAT_TEST("a loop's finally^ runs after its epilog^");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "repeat^ 1 {\n"
             "  main^:\n"
             "    log.s := log.s * 10 + 1\n"
             "  epilog^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 3\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    // 9.8: a return^ out of a loop runs neither epilog^ nor last^, but 10.2
    // still runs the finally^.
    LHAT_TEST("a return^ out of a loop runs the finally^ and nothing else");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ go = p^ {\n"
             "  repeat^ 3 {\n"
             "    main^:\n"
             "      return^ 1\n"
             "    epilog^:\n"
             "      log.s := log.s * 10 + 2\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 3\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 12.2: dispose() at the end of the block.
    LHAT_TEST("with^ disposes at the end of the block");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ open = f^ { return^ { dispose := p^ { log.n := 1 } } }\n"
             "with^ h = open()\n"
             "{\n"
             "  log.n := 0\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 12.2: the reverse of the order they were defined in, so a resource that
    // depends on an earlier one goes first.
    LHAT_TEST("several with^ dispose in reverse order");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ res = f^n { return^ { dispose := p^ { log.s := log.s * 10 + n } } }\n"
             "with^ a = res(1)\n"
             "with^ b := res(2)\n"
             "{\n"
             "  log.s := 0\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    // 12.3: dispose() has the same strength as finally^.
    LHAT_TEST("with^ disposes when return^ leaves through it");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ open = f^ { return^ { dispose := p^ { log.n := 4 } } }\n"
             "let^ go = p^ {\n"
             "  with^ h = open()\n"
             "  {\n"
             "    return^ 1\n"
             "  }\n"
             "}\n"
             "go()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 12.4: finally^ first, dispose() after, because with^ wraps the block
    // from outside and finally^ is its last clause from inside.
    LHAT_TEST("finally^ runs before the dispose that surrounds it");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ open = f^ { return^ { dispose := p^ { log.s := log.s * 10 + 2 } } }\n"
             "with^ h = open()\n"
             "{\n"
             "  log.s := 0\n"
             "finally^:\n"
             "  log.s := log.s * 10 + 1\n"
             "}\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("the resource is in scope inside the block and not after it");
    run_text(&r,
             "let^ open = f^ { return^ { dispose := p^ { }, n := 6 } }\n"
             "let^ seen = 0\n"
             "with^ h = open()\n"
             "{\n"
             "  seen := h.n\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    run_text(&r,
             "let^ open = f^ { return^ { dispose := p^ { } } }\n"
             "with^ h = open()\n"
             "{\n"
             "}\n"
             "return^ h\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);
}

// 02 の 14 章: def^ is the one mechanism for a user-defined type.
static void test_definitions(void)
{
    Run r;

    // 14.11: without a new^ of its own, a definition gets one taking no
    // arguments that answers what the template says.
    LHAT_TEST("the default new^ builds an instance from the template");
    run_text(&r,
             "let^ Foo = def^{ self^{ a := 1, b := 2 } }\n"
             "let^ f = Foo.new^()\n"
             "return^ f.a * 10 + f.b\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 14.3: a method belongs to the definition and is shared; a field belongs
    // to the instance and is copied.
    LHAT_TEST("two instances have their own fields");
    run_text(&r,
             "let^ Foo = def^{ self^{ n := 0 } }\n"
             "let^ a = Foo.new^()\n"
             "let^ b = Foo.new^()\n"
             "a.n := 5\n"
             "return^ b.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.11: an initialiser is an expression evaluated at each construction,
    // so a mutable initial value is not shared. Python's mutable default
    // argument has no counterpart here.
    LHAT_TEST("a mutable initial value is not shared between instances");
    run_text(&r,
             "let^ Foo = def^{ self^{ items := { } } }\n"
             "let^ a = Foo.new^()\n"
             "let^ b = Foo.new^()\n"
             "a.items[1] := 9\n"
             "return^ b.items[1] ?? 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.4: the shape of the signature says it is a method. No modifier does.
    LHAT_TEST("a method gets the receiver as its self^");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ n := 7 },\n"
             "  get := f^self^ { return^ self^.n },\n"
             "}\n"
             "return^ Foo.new^().get()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("a method takes arguments after the receiver");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ n := 1 },\n"
             "  add := f^self^, x { return^ self^.n + x },\n"
             "}\n"
             "return^ Foo.new^().add(4)\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a method may change the instance");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ n := 0 },\n"
             "  bump := p^self^ { self^.n := self^.n + 1 },\n"
             "}\n"
             "let^ f = Foo.new^()\n"
             "f.bump()\n"
             "f.bump()\n"
             "return^ f.n\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.4: taking the method out and passing the receiver by hand is the
    // same call written differently.
    LHAT_TEST("a method taken out is called with the receiver by hand");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ n := 3 },\n"
             "  get := f^self^ { return^ self^.n },\n"
             "}\n"
             "let^ f = Foo.new^()\n"
             "let^ g = f.get\n"
             "return^ g(f)\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 14.4: a member with no self^ is a static one, so the receiver is not
    // passed to it.
    LHAT_TEST("a member without self^ is static");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, make := f^x { return^ x * 2 } }\n"
             "return^ Foo.make(21)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 14.7: an instance sees the definition's members too.
    LHAT_TEST("an instance reaches a static member");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, tag := f^ { return^ 9 } }\n"
             "return^ Foo.new^().tag()\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 14.11: new^ may fill some fields and leave the rest to the template.
    LHAT_TEST("new^ fills what it names and the template the rest");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ a := 1, b := 2 },\n"
             "  new^ := f^v { return^ self^{ a := v } },\n"
             "}\n"
             "let^ f = Foo.new^(8)\n"
             "return^ f.a * 10 + f.b\n");
    CHECK_INTEGER(&r, 82);
    run_dispose(&r);

    // 14.11: producing a value only to overwrite it is not something an
    // initialiser should be made to do.
    LHAT_TEST("the initialiser of a field new^ named is not evaluated");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ side = f^ { log.n := log.n + 1 return^ 0 }\n"
             "let^ Foo = def^{\n"
             "  self^{ a := side() },\n"
             "  new^ := f^ { return^ self^{ a := 5 } },\n"
             "}\n"
             "let^ f = Foo.new^()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.11: an initialiser cannot see self^, which does not exist yet, but
    // can see class^, which does.
    LHAT_TEST("an initialiser sees class^");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ a := class^.base() },\n"
             "  base := f^ { return^ 6 },\n"
             "}\n"
             "return^ Foo.new^().a\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("a method sees class^ too");
    run_text(&r,
             "let^ Foo = def^{\n"
             "  self^{ },\n"
             "  base := f^ { return^ 4 },\n"
             "  get := f^self^ { return^ class^.base() },\n"
             "}\n"
             "return^ Foo.new^().get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.5: composition is '..' and the order matters.
    LHAT_TEST("composition brings the base's members along");
    run_text(&r,
             "let^ Foo = def^{ self^{ a := 1 }, one := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{ self^{ b := 2 }, two := f^ { return^ 2 } }\n"
             "let^ x = Bar.new^()\n"
             "return^ x.a * 1000 + x.b * 100 + x.one() * 10 + x.two()\n");
    CHECK_INTEGER(&r, 1212);
    run_dispose(&r);

    // 14.12: override^ replaces, and the later part is what wins.
    LHAT_TEST("override^ replaces the member it names");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, tag := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^\n"
             "  tag := f^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new^().tag()\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("and the base keeps its own");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, tag := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^\n"
             "  tag := f^ { return^ 2 },\n"
             "}\n"
             "return^ Foo.new^().tag()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.12改: super^ is what the override^ wrote over. The parts are walked
    // in order, so it is read out of the table before the write.
    LHAT_TEST("super^ reaches the definition an override^ replaced");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, tag := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  override^ tag := f^ { return^ super^() + 100 },\n"
             "}\n"
             "return^ Bar.new^().tag()\n");
    CHECK_INTEGER(&r, 101);
    run_dispose(&r);

    // Each link sees the one before it, since each is written over what the
    // parts up to that point had built.
    LHAT_TEST("and each link of a chain reaches its own");
    run_text(&r,
             "let^ A = def^{ self^{ }, m := f^ { return^ 1 } }\n"
             "let^ B = A .. def^{ self^{ },\n"
             "  override^ m := f^ { return^ super^() + 10 } }\n"
             "let^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^ { return^ super^() + 100 } }\n"
             "return^ C.new^().m()\n");
    CHECK_INTEGER(&r, 111);
    run_dispose(&r);

    // 14.4: the receiver of super^(…) is the one the body already holds, so
    // it is not written. This one reads a field through it.
    LHAT_TEST("super^ takes the receiver without it being written");
    run_text(&r,
             "let^ Foo = def^{ self^{ n := 3 },\n"
             "  get := f^self^ -> number^ { return^ self^.n } }\n"
             "let^ Bar = Foo .. def^{ self^{ },\n"
             "  override^ get := f^self^ -> number^ { return^ super^() * 10 } }\n"
             "return^ Bar.new^().get()\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    // 14.4 again: named on its own it is a value like any other, and then
    // the receiver is spelled out.
    LHAT_TEST("and spelled out once super^ is taken as a value");
    run_text(&r,
             "let^ Foo = def^{ self^{ n := 3 },\n"
             "  get := f^self^ -> number^ { return^ self^.n } }\n"
             "let^ Bar = Foo .. def^{ self^{ },\n"
             "  override^ get := f^self^ -> number^ {\n"
             "    let^ old = super^\n"
             "    return^ old(self^) + 1\n"
             "  } }\n"
             "return^ Bar.new^().get()\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 14.12: an override^ over an overloaded name replaces the one arm it
    // overlaps, so a plain write would take the whole group with it.
    LHAT_TEST("override^ over an overload^ keeps the other arms");
    run_text(&r,
             "let^ A = def^{ self^{ }, m := f^ { return^ 1 } }\n"
             "let^ B = A .. def^{ self^{ },\n"
             "  overload^ m := f^ x:number^ { return^ x } }\n"
             "let^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^ { return^ super^() + 100 } }\n"
             "return^ C.new^().m(7)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and super^ there is the arm that was replaced");
    run_text(&r,
             "let^ A = def^{ self^{ }, m := f^ { return^ 1 } }\n"
             "let^ B = A .. def^{ self^{ },\n"
             "  overload^ m := f^ x:number^ { return^ x } }\n"
             "let^ C = B .. def^{ self^{ },\n"
             "  override^ m := f^ { return^ super^() + 100 } }\n"
             "return^ C.new^().m()\n");
    CHECK_INTEGER(&r, 101);
    run_dispose(&r);

    // 14.15: a declaration puts nothing in the table, and what a composition
    // provides goes under the same name. The mixin's own body reaches it.
    LHAT_TEST("what fills an abstract^ is what the mixin calls");
    run_text(&r,
             "let^ Counting = def^{\n"
             "  self^{ count := 0 },\n"
             "  abstract^ step : f^ -> number^;,\n"
             "  bump := p^self^ { self^.count := self^.count + class^.step() },\n"
             "}\n"
             "let^ Fast = Counting .. def^{\n"
             "  self^{},\n"
             "  step := f^ -> number^ { return^ 10 },\n"
             "}\n"
             "let^ o = Fast.new^()\n"
             "o.bump()\n"
             "return^ o.count\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // 14.6: a declared field has no initialiser to run, so the one that fills
    // it is the only one the construction sees.
    LHAT_TEST("an abstract^ field is initialised by what fills it");
    run_text(&r,
             "let^ Greet = def^{\n"
             "  self^{ abstract^ n : number^ },\n"
             "  hello := f^self^ -> number^ { return^ self^.n + 1 },\n"
             "}\n"
             "let^ Thing = Greet .. def^{ self^{ n := 10 } }\n"
             "return^ Thing.new^().hello()\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    // 14.15改: a mixin written against no base, composed onto one. What its
    // super^ reaches is settled by 14.2's chain the same as any other.
    LHAT_TEST("a mixin written against no base reaches the base it gets");
    run_text(&r,
             "let^ Base = def^{ self^{ n := 0 },\n"
             "  run := p^self^ { self^.n := self^.n + 1 } }\n"
             "let^ Logged = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 10\n"
             "                             super^() } }\n"
             "let^ App = Base .. Logged\n"
             "let^ o = App.new^()\n"
             "o.run()\n"
             "return^ o.n\n");
    CHECK_INTEGER(&r, 11);
    run_dispose(&r);

    LHAT_TEST("and two of them stack in the order they are written");
    run_text(&r,
             "let^ Base = def^{ self^{ n := 0 },\n"
             "  run := p^self^ { self^.n := self^.n + 1 } }\n"
             "let^ A = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 10\n"
             "                             super^() } }\n"
             "let^ B = def^{ self^{ abstract^ n : number^ },\n"
             "  override^ run := p^self^ { self^.n := self^.n + 100\n"
             "                             super^() } }\n"
             "let^ App = Base .. A .. B\n"
             "let^ o = App.new^()\n"
             "o.run()\n"
             "return^ o.n\n");
    CHECK_INTEGER(&r, 111);
    run_dispose(&r);

    // 14.5改: nothing is written under a name both sides carry, since the
    // checker will not read it through the composition -- but the method each
    // side wrote is still a value, and 14.4 applies it to whatever fits.
    LHAT_TEST("either side of an ambiguous name is still reachable");
    run_text(&r,
             "let^ A = def^{ self^{},\n"
             "  m := f^self^ -> number^ { return^ 1 },\n"
             "  only := f^ -> number^ { return^ 9 } }\n"
             "let^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
             "let^ D = A .. B\n"
             "let^ fromA = A.m\n"
             "let^ fromB = B.m\n"
             "let^ o = D.new^()\n"
             "return^ fromA(o) * 100 + fromB(o) * 10 + D.new^().only()\n");
    CHECK_INTEGER(&r, 129);
    run_dispose(&r);

    // 14.2: the chain is settled at the definition, so an instance made
    // before a later definition is unaffected by it.
    LHAT_TEST("two definitions of the same shape stay separate");
    run_text(&r,
             "let^ Foo = def^{ self^{ n := 1 } }\n"
             "let^ Bar = def^{ self^{ n := 2 } }\n"
             "return^ Foo.new^().n * 10 + Bar.new^().n\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("a composed definition's new^ fills the base's fields too");
    run_text(&r,
             "let^ Foo = def^{ self^{ a := 1, b := 2 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ c := 3 },\n"
             "  new^ := f^v { return^ self^{ b := v } },\n"
             "}\n"
             "let^ x = Bar.new^(9)\n"
             "return^ x.a * 100 + x.b * 10 + x.c\n");
    CHECK_INTEGER(&r, 193);
    run_dispose(&r);

    // 14.12: overload^ adds a way to call without losing the one that was
    // there, and 14.12's ban on overlapping signatures means the call finds
    // at most one candidate -- a search rather than a choice.
    LHAT_TEST("overload^ keeps the way that was already there");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, m := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^ x:string^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new^().m()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and answers the added one when that is what fits");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, m := f^ { return^ 1 } }\n"
             "let^ Bar = Foo .. def^{\n"
             "  self^{ },\n"
             "  overload^\n"
             "  m := f^ x:string^ { return^ 2 },\n"
             "}\n"
             "return^ Bar.new^().m(\"s\")\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.4: a candidate that takes self^ is reached as a method, so what was
    // given starts past the receiver. The search had been comparing the
    // receiver against the first parameter instead, and no candidate taking
    // self^ ever fitted.
    LHAT_TEST("a candidate taking self^ is found");
    run_text(&r,
             "let^ V = def^{ self^{},\n"
             "  m := f^self^, o:number^ -> number^ { return^ o },\n"
             "  overload^ m := f^self^, o:string^ -> number^ { return^ 9 },\n"
             "}\n"
             "let^ v = V.new^()\n"
             "return^ v.m(7) * 10 + v.m(\"x\")\n");
    CHECK_INTEGER(&r, 79);
    run_dispose(&r);

    LHAT_TEST("across a composition as well");
    run_text(&r,
             "let^ Foo = def^{ self^{},\n"
             "  m := f^self^, o:number^ -> number^ { return^ o } }\n"
             "let^ Bar = Foo .. def^{ self^{},\n"
             "  overload^ m := f^self^, o:string^ -> number^ { return^ 9 } }\n"
             "let^ v = Bar.new^()\n"
             "return^ v.m(7) * 10 + v.m(\"x\")\n");
    CHECK_INTEGER(&r, 79);
    run_dispose(&r);

    // 14.5 and 14.12 reach an operator the way they reach any other member.
    LHAT_TEST("a composition carries an operator in");
    run_text(&r,
             "let^ B = def^{ self^{ t := \"b\" },\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ self^.t .. o } }\n"
             "let^ D = B .. def^{ self^{} }\n"
             "return^ D.new^() .. \"x\"\n");
    CHECK_STRING(&r, "bx");
    run_dispose(&r);

    LHAT_TEST("and an override^ of one replaces it");
    run_text(&r,
             "let^ B = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ \"base\" } }\n"
             "let^ D = B .. def^{ self^{},\n"
             "  override^ op^.. := f^self^, o:string^ -> string^ {\n"
             "    return^ \"derived\" } }\n"
             "return^ D.new^() .. \"x\"\n");
    CHECK_STRING(&r, "derived");
    run_dispose(&r);

    LHAT_TEST("while an overload^ of one adds to it");
    run_text(&r,
             "let^ B = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> string^ { return^ \"s\" } }\n"
             "let^ D = B .. def^{ self^{},\n"
             "  overload^ op^.. := f^self^, o:number^ -> string^ {\n"
             "    return^ \"n\" } }\n"
             "let^ d = D.new^()\n"
             "return^ (d .. \"x\") .. (d .. 7)\n");
    CHECK_STRING(&r, "sn");
    run_dispose(&r);

    // 11.3 judges structurally, so a '..' put on a plain table by 14.14's
    // computed key answers exactly as a written op^ does.
    LHAT_TEST("a '..' reached by a computed key answers as well");
    run_text(&r,
             "let^ t = { [\"..\"] := f^self^, o:string^ -> string^ {\n"
             "  return^ o } }\n"
             "return^ t .. \"x\"\n");
    CHECK_STRING(&r, "x");
    run_dispose(&r);

    LHAT_TEST("and an operator may answer with a structure");
    run_text(&r,
             "let^ V = def^{ self^{ n := 3 },\n"
             "  op^+ := f^self^, o:number^ -> t^{ n : number^ } {\n"
             "    return^ self^ } }\n"
             "return^ (V.new^() + 1).n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 11.8 makes an operator a member, so 14.12's overload^ reaches it too --
    // and 14.4 makes the left operand the receiver, which is the shape the
    // search has to ask in.
    LHAT_TEST("an operator may be overloaded like any member");
    run_text(&r,
             "let^ V = def^{ self^{},\n"
             "  op^.. := f^self^, o:string^ -> number^ { return^ 1 },\n"
             "  overload^ op^.. := f^self^, o:number^ -> number^ { return^ 2 },\n"
             "}\n"
             "let^ v = V.new^()\n"
             "return^ (v .. \"s\") * 10 + (v .. 7)\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // The candidates may differ in the type rather than the count, which is
    // the case a count alone could not tell apart.
    LHAT_TEST("candidates of one arity are told apart by type");
    run_text(&r,
             "let^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^ x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^ x:number^ { return^ 2 },\n"
             "}\n"
             "let^ s = Show.new^()\n"
             "return^ s.show(\"t\") * 10 + s.show(7)\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 14.10: the judgement is structural, so a candidate taking a shape is
    // asked whether the value has those members.
    LHAT_TEST("a structural parameter is judged by its members");
    run_text(&r,
             "let^ Draw = def^{\n"
             "  self^{ },\n"
             "  draw := f^ s:t^{ radius : number^ } { return^ 1 },\n"
             "  overload^\n"
             "  draw := f^ s:t^{ width : number^, height : number^ } { return^ 2 },\n"
             "}\n"
             "let^ d = Draw.new^()\n"
             "return^ d.draw({ radius := 1 }) * 10 + d.draw({ width := 1, height := 2 })\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("no candidate taking these arguments is a fault");
    run_text(&r,
             "let^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^ x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^ x:number^ { return^ 2 },\n"
             "}\n"
             "return^ Show.new^().show(true^)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NO_CANDIDATE);
    run_dispose(&r);

    // 14.8: number^ is one type, so either representation answers to it.
    LHAT_TEST("either representation of a number answers to number^");
    run_text(&r,
             "let^ Show = def^{\n"
             "  self^{ },\n"
             "  show := f^ x:string^ { return^ 1 },\n"
             "  overload^\n"
             "  show := f^ x:number^ { return^ 2 },\n"
             "}\n"
             "return^ Show.new^().show(0.5)\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("an ordinary member is untouched by any of this");
    run_text(&r,
             "let^ Foo = def^{ self^{ }, m := f^ x { return^ x + 1 } }\n"
             "return^ Foo.new^().m(1)\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 14.13: self^{ … } outside a definition has no fields to name.
    LHAT_TEST("self^{ } outside a definition does not compile");
    run_text(&r, "let^ x = self^{ a := 1 }\nreturn^ x\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
    run_dispose(&r);
}

// 02 の 14.12改: typeof^ reflects the value itself rather than anything the
// checker inferred (03 の 4.2), so this is testing reflect_type's walk of the
// actual runtime object graph.
static void test_typeof(void)
{
    Run r;

    LHAT_TEST("a number's signature");
    run_text(&r, "return^ typeof^(5).signature\n");
    CHECK_STRING(&r, "number^");
    run_dispose(&r);

    LHAT_TEST("and a real's is the same -- 14.8 is one type");
    run_text(&r, "return^ typeof^(5.5).signature\n");
    CHECK_STRING(&r, "number^");
    run_dispose(&r);

    LHAT_TEST("a string's signature");
    run_text(&r, "return^ typeof^(\"x\").signature\n");
    CHECK_STRING(&r, "string^");
    run_dispose(&r);

    LHAT_TEST("a bool's signature");
    run_text(&r, "return^ typeof^(true^).signature\n");
    CHECK_STRING(&r, "bool^");
    run_dispose(&r);

    LHAT_TEST("nil^'s signature");
    run_text(&r, "return^ typeof^(nil^).signature\n");
    CHECK_STRING(&r, "nil^");
    run_dispose(&r);

    // 14.7: an instance's members are split between it (fields, set at
    // construction, 14.11) and its definition (methods, shared, 14.2) --
    // reflect_type walks both and folds them into one structure.
    LHAT_TEST("an instance's signature carries fields and methods");
    run_text(&r,
             "let^ Point = def^{ self^{ x := 0, y := 0 },\n"
             "  sum := f^self^ -> number^ { return^ self^.x + self^.y } }\n"
             "return^ typeof^(Point.new^()).signature\n");
    CHECK_STRING(&r, "t^{ new : f^;, sum : f^ -> number^;, x : number^, "
                     "y : number^ }");
    run_dispose(&r);

    // 14.4: self^ is the receiver, not a parameter of the signature.
    LHAT_TEST("a function's signature carries its parameters and result");
    run_text(&r,
             "let^ f = f^ x:number^, y:string^ -> bool^ { return^ true^ }\n"
             "return^ typeof^(f).signature\n");
    CHECK_STRING(&r, "f^number^, string^ -> bool^;");
    run_dispose(&r);

    LHAT_TEST("and a procedure with no result says nothing after it");
    run_text(&r, "let^ p = p^ x:number^ { }\nreturn^ typeof^(p).signature\n");
    CHECK_STRING(&r, "p^number^;");
    run_dispose(&r);

    // 04 の 2.4: identity is the declaration, so typeof^ answers with the
    // kind's own qualified name -- the example 04-errors.md 136 gives.
    LHAT_TEST("an error's signature is its qualified kind name");
    run_text(&r,
             "errordef^ IOError { NotFound }\n"
             "let^ e = error^IOError.NotFound{ }\n"
             "return^ typeof^(e).signature\n");
    CHECK_STRING(&r, "IOError.NotFound");
    run_dispose(&r);

    // 14.10改: the sequence half is written as bare types, in order, with no
    // names -- not as members keyed by their position.
    LHAT_TEST("a table literal's positional part has no names");
    run_text(&r, "return^ typeof^({1, \"x\"}).signature\n");
    CHECK_STRING(&r, "t^{ number^, string^ }");
    run_dispose(&r);

    // 02 の 2811: typeof^(x) = typeof^(y) is 11.3's structural identity, not
    // the identity of the two LhatRuntimeType objects reflect_type built --
    // two separate calls never share one.
    LHAT_TEST("typeof^(x) = typeof^(y) compares structurally");
    run_text(&r, "return^ typeof^(5) = typeof^(6)\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and answers false across different shapes");
    run_text(&r, "return^ typeof^(5) = typeof^(\"x\")\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.9: a name never takes part in identity -- two independently written
    // definitions of the same shape are the same type.
    LHAT_TEST("two unrelated definitions of the same shape are equal");
    run_text(&r,
             "let^ A = def^{ self^{ n := 0 } }\n"
             "let^ B = def^{ self^{ n := 5 } }\n"
             "return^ typeof^(A.new^()) = typeof^(B.new^())\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and unequal once the shapes differ");
    run_text(&r,
             "let^ A = def^{ self^{ n := 0 } }\n"
             "let^ B = def^{ self^{ n := 0, extra := 1 } }\n"
             "return^ typeof^(A.new^()) = typeof^(B.new^())\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    // 14.10's round trip: what typeof^(x).signature answers with has to be
    // usable as a type annotation.
    LHAT_TEST("the signature parses back as an annotation");
    run_text(&r,
             "let^ Point = def^{ self^{ x := 0, y := 0 } }\n"
             "let^ p : t^{ x : number^, y : number^ } = Point.new^()\n"
             "return^ p.x + p.y\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 14.5, 14.12: a multi-dispatched member is callable every way its arms
    // list, which is what '&' means -- matching 14.12's own example.
    LHAT_TEST("an overload^ed member's signature is an intersection");
    run_text(&r,
             "let^ Foo = def^{ self^{}, foo := p^ { } }\n"
             "let^ Bar = Foo .. def^{ self^{},\n"
             "  overload^ foo := p^ x:string^ { } }\n"
             "return^ typeof^(Bar.new^()).signature\n");
    CHECK_STRING(&r, "t^{ foo : p^; & p^string^;, new : f^; }");
    run_dispose(&r);

    // 02 の 14.16: 8.8 lets a written annotation widen what a path introduces
    // past the value's own type ('let^ a.next : t^{} = b' does not narrow to
    // b), which is enough to make two tables hold each other. Measured to
    // crash with a stack overflow before the walk tracked its own path.
    LHAT_TEST("a cycle between two tables does not recurse forever");
    run_text(&r,
             "let^ a = { }\n"
             "let^ b = { }\n"
             "let^ a.next : t^{} = b\n"
             "let^ b.next : t^{} = a\n"
             "return^ typeof^(a).signature\n");
    CHECK_STRING(&r, "t^{ next : t^{ next : table^ } }");
    run_dispose(&r);

    // The same through two def^ instances -- 'next := { }' defaults the
    // field to the unstructured top of tables (13.7), which anything at all
    // conforms to, including another instance of the same definition.
    LHAT_TEST("and between two instances of the same definition");
    run_text(&r,
             "let^ Node = def^{ self^{ next := { } } }\n"
             "let^ a = Node.new^()\n"
             "let^ b = Node.new^()\n"
             "a.next := b\n"
             "b.next := a\n"
             "return^ typeof^(a).signature\n");
    CHECK_STRING(&r, "t^{ new : f^;, next : t^{ new : f^;, next : table^ } }");
    run_dispose(&r);

    // A value reached twice without a cycle -- shared, not circular -- is not
    // cut short. Only revisiting something still on the current path is.
    LHAT_TEST("a value reached two ways (no cycle) is not cut short");
    run_text(&r,
             "let^ shared = { v := 1 }\n"
             "let^ t = { a := shared, b := shared }\n"
             "return^ typeof^(t).signature\n");
    CHECK_STRING(&r, "t^{ a : t^{ v : number^ }, b : t^{ v : number^ } }");
    run_dispose(&r);
}

// 02 の 14.17: every value carries tostring. 14.16 above turns a value's type
// into text; this turns the value itself into text.
static void test_tostring(void)
{
    Run r;

    // The three that name themselves as literals read as words rather than
    // as the spelling a prompt would show, which is the whole of what
    // separates this from lhat_value_write.
    LHAT_TEST("nil^ is the word");
    run_text(&r, "return^ nil^.tostring()\n");
    CHECK_STRING(&r, "nil");
    run_dispose(&r);

    LHAT_TEST("and so are the two bools");
    run_text(&r, "return^ true^.tostring() .. false^.tostring()\n");
    CHECK_STRING(&r, "truefalse");
    run_dispose(&r);

    LHAT_TEST("a string^ is its own text, unquoted");
    run_text(&r, "return^ \"hi\".tostring()\n");
    CHECK_STRING(&r, "hi");
    run_dispose(&r);

    // 14.8: one type, two representations, and tostring keeps them apart the
    // same way a literal does -- 3 and 3.0 are not the same text.
    LHAT_TEST("a number^ says which representation it has");
    run_text(&r, "return^ (3).tostring() .. \" \" .. (3.0).tostring()\n");
    CHECK_STRING(&r, "3 3.0");
    run_dispose(&r);

    LHAT_TEST("a table is written the way 14 章 writes one");
    run_text(&r, "return^ { 1, \"a\" }.tostring()\n");
    CHECK_STRING(&r, "{ 1, \"a\" }");
    run_dispose(&r);

    // The value handed over is read as text; one inside a table is written
    // as the table spells it, which is the table's spelling and not that
    // value's.
    LHAT_TEST("but what is inside it keeps the table's spelling");
    run_text(&r, "return^ { \"a\", true^ }.tostring()\n");
    CHECK_STRING(&r, "{ \"a\", true^ }");
    run_dispose(&r);

    LHAT_TEST("a format writes the number through it");
    run_text(&r, "return^ (255).tostring(\"%x\")\n");
    CHECK_STRING(&r, "ff");
    run_dispose(&r);

    LHAT_TEST("and carries whatever text was written around it");
    run_text(&r, "return^ (42).tostring(\"n = %d units\")\n");
    CHECK_STRING(&r, "n = 42 units");
    run_dispose(&r);

    // 14.8: the conversion decides the reading, so either representation
    // answers either family.
    LHAT_TEST("the conversion decides how the number is read");
    run_text(&r,
             "return^ (3.9).tostring(\"%d\") .. \" \" .. (3).tostring(\"%.2f\")\n");
    CHECK_STRING(&r, "3 3.00");
    run_dispose(&r);

    LHAT_TEST("a per cent sign the writer wanted survives");
    run_text(&r, "return^ (50).tostring(\"%d%%\")\n");
    CHECK_STRING(&r, "50%");
    run_dispose(&r);

    // Handing snprintf a program's format unchecked would read the number as
    // a pointer, so the conversion is checked before anything is written.
    LHAT_TEST("a conversion a number^ cannot answer is refused");
    run_text(&r, "return^ (1).tostring(\"%s\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    LHAT_TEST("and so is a second conversion, with one number to write");
    run_text(&r, "return^ (1).tostring(\"%d %d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    LHAT_TEST("and a format asking for no number at all");
    run_text(&r, "return^ (1).tostring(\"plain\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // The length is the machine's to supply -- a program spelling one would
    // be naming a width it cannot know.
    LHAT_TEST("and a length the writer spelled");
    run_text(&r, "return^ (1).tostring(\"%ld\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // '*' reads its width from a further argument, and there is only ever
    // the one number here.
    LHAT_TEST("and a width taken from an argument that is not there");
    run_text(&r, "return^ (1).tostring(\"%*d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // 16.3's rule for iterate, which 14.17 follows: the built-in answers
    // only where nothing was written under that name.
    LHAT_TEST("a written tostring wins over the built-in");
    run_text(&r,
             "let^ t = { tostring := f^self^ -> string^ { return^ \"mine\" } }\n"
             "return^ t.tostring()\n");
    CHECK_STRING(&r, "mine");
    run_dispose(&r);

    LHAT_TEST("and one written into a def^ answers for its instances");
    run_text(&r,
             "let^ P = def^{ self^{ x := 7 },\n"
             "  tostring := f^self^ -> string^ { return^ \"P\" } }\n"
             "return^ P.new^().tostring()\n");
    CHECK_STRING(&r, "P");
    run_dispose(&r);

    // Only a number^ carries the second signature, so this call is neither
    // of the two ways of writing the value down.
    LHAT_TEST("nothing but a number^ takes a format");
    run_text(&r, "return^ \"x\".tostring(\"%d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("and a third argument is not one of them either");
    run_text(&r, "return^ (1).tostring(\"%d\", \"%d\")\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("a coroutine carries it beside its own operations");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "return^ gen().tostring()\n");
    CHECK_STRING(&r, "c^");
    run_dispose(&r);
}

// 01 の 8 章: a scope specifier starts the search further out. What the
// compiler counts as one scope has to be what the checker counts, or the
// name a program was checked against is not the one it reaches -- so these
// pin the count from the running side as well.
static void test_scope_specifiers(void)
{
    Run r;

    LHAT_TEST("'$^' reads the binding one scope out");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  return^ $^x\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("'$^^' one further, and '$^' still the nearer one");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  do^{ let^ x = 3\n"
             "    return^ $^^x * 10 + $^x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // Memo.md L676-695 is this table: the same three scopes named from
    // either end, and the two numberings meeting in the middle.
    LHAT_TEST("'$' reads the unit's own top level");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  do^{ let^ x = 3\n"
             "    return^ $x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and repeating the sigil counts inwards from it");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  do^{ let^ x = 3\n"
             "    return^ $$x * 10 + $$$x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 23);
    run_dispose(&r);

    LHAT_TEST("naming a scope further in than this one is refused");
    run_text(&r, "let^ x = 1\nreturn^ $$x\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);

    // A subroutine body is one scope like any other, so it takes its place
    // in the absolute numbering too.
    LHAT_TEST("a body counts in the absolute numbering as well");
    run_text(&r,
             "let^ x = 1\n"
             "let^ f = f^ -> number^ { let^ x = 2\n"
             "  return^ $$x\n"
             "}\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a write through the absolute form lands there too");
    run_text(&r,
             "let^ x = 1\n"
             "let^ seen = 0\n"
             "do^{ let^ x = 2\n"
             "  do^{ let^ x = 3\n"
             "    $$x := 9\n"
             "  }\n"
             "  seen := x\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 5.4: reaching out of a body is a capture, so the specifier has to
    // count the body as the one scope its parameters are already in.
    LHAT_TEST("and reaches out of a subroutine as a captured place");
    run_text(&r,
             "let^ x = 1\n"
             "let^ f = f^ -> number^ { let^ x = 2\n"
             "  return^ $^x\n"
             "}\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a loop body is one scope like any other");
    run_text(&r,
             "let^ x = 1\n"
             "let^ seen = 0\n"
             "repeat^ 1 { let^ x = 2\n"
             "  seen := $^x\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 8.6 with 01 の 8 章: writing through a specifier reaches the binding a
    // read of the same words would, and leaves the nearer one alone.
    LHAT_TEST("':=' through '$^' writes the outer binding");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  $^x := 9\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("and leaves the one it was written beside");
    run_text(&r,
             "let^ x = 1\n"
             "let^ seen = 0\n"
             "do^{ let^ x = 2\n"
             "  $^x := 9\n"
             "  seen := x\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a write out of a subroutine reaches the captured place");
    run_text(&r,
             "let^ x = 1\n"
             "let^ f = p^ { let^ x = 2\n"
             "  $^x := 9\n"
             "}\n"
             "f()\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("reading back through the same words sees the write");
    run_text(&r,
             "let^ x = 1\n"
             "do^{ let^ x = 2\n"
             "  $^x := 42\n"
             "  return^ $^x\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 14.4 binds class^ into the scope a def^'s '{' opens, so a specifier
    // written in a method counts that scope -- the same one the checker
    // pushes there.
    LHAT_TEST("a def^ body is one scope on this side too");
    run_text(&r,
             "let^ class = 1\n"
             "let^ D = def^{ self^{ v := 7 },\n"
             "  m := f^self^ -> string^ { return^ typeof^($^class).signature }\n"
             "}\n"
             "return^ D.new^().m()\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_STRING),
               "expected a string");
    if (lhat_is_object_kind(r.ran.value, LHAT_OBJECT_STRING)) {
        const LhatString *s = (const LhatString *)lhat_as_object(r.ran.value);
        // The definition, not the number^ named `class` outside it.
        LHAT_CHECK(strncmp(s->text, "t^{", 3) == 0,
                   "'$^class' reached the definition");
    }
    run_dispose(&r);

    LHAT_TEST("counting past the scopes that are open is refused");
    run_text(&r, "do^{ return^ $^^^^x }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);
}

// 01 の 5.4: the lexer and the parser have read $"..." from the start; this
// is what it compiles to. A hole is 02 の 14.17's tostring and the pieces
// are joined with 11.2's '..', so interpolation adds no way of building a
// string that writing it out by hand would not reach.
static void test_interpolation(void)
{
    Run r;

    LHAT_TEST("text with no hole is just the text");
    run_text(&r, "return^ $\"plain\"\n");
    CHECK_STRING(&r, "plain");
    run_dispose(&r);

    LHAT_TEST("and nothing at all is the empty string");
    run_text(&r, "return^ $\"\"\n");
    CHECK_STRING(&r, "");
    run_dispose(&r);

    LHAT_TEST("a hole is its value written down");
    run_text(&r, "let^ a = 5\nreturn^ $\"n = {a}\"\n");
    CHECK_STRING(&r, "n = 5");
    run_dispose(&r);

    // 5.4: no empty text run is made between two holes that meet.
    LHAT_TEST("two holes may meet");
    run_text(&r, "let^ a = 5\nreturn^ $\"{a}{a}\"\n");
    CHECK_STRING(&r, "55");
    run_dispose(&r);

    LHAT_TEST("a hole holds an expression, not just a name");
    run_text(&r, "return^ $\"{ 1 + 2 }\"\n");
    CHECK_STRING(&r, "3");
    run_dispose(&r);

    // 14.17: nil^ and bool^ are the bare words there, so they are here.
    LHAT_TEST("what a hole writes is what tostring answers");
    run_text(&r, "return^ $\"{nil^} {true^} {\"s\"} {3.0}\"\n");
    CHECK_STRING(&r, "nil true s 3.0");
    run_dispose(&r);

    // 5.4: the text after ':' is a format, and 14.17 hands it to a number^.
    LHAT_TEST("a format after ':' reaches tostring");
    run_text(&r, "let^ a = 5\nreturn^ $\"{a:%04d}\"\n");
    CHECK_STRING(&r, "0005");
    run_dispose(&r);

    LHAT_TEST("a bad format is the same runtime error as anywhere");
    run_text(&r, "return^ $\"{ 1 :%s}\"\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_BAD_FORMAT);
    run_dispose(&r);

    // 5.4: '{{' and '}}' spell the braces themselves.
    LHAT_TEST("doubled braces are the braces");
    run_text(&r, "return^ $\"{{literal}}\"\n");
    CHECK_STRING(&r, "{literal}");
    run_dispose(&r);

    // 5.4: a hole is scanned by the ordinary rules, so what is in it may be
    // a string literal, a table, or another interpolation.
    LHAT_TEST("a hole may hold a string of its own");
    run_text(&r, "return^ $\"{ \"in\" }\"\n");
    CHECK_STRING(&r, "in");
    run_dispose(&r);

    LHAT_TEST("and another interpolated string");
    run_text(&r, "let^ a = 5\nreturn^ $\"[{ $\"{a}\" }]\"\n");
    CHECK_STRING(&r, "[5]");
    run_dispose(&r);

    // 14.17: reached the way a program would reach it, so a written one is
    // what answers.
    LHAT_TEST("a written tostring is what a hole uses");
    run_text(&r,
             "let^ p = { tostring := f^self^ -> string^ { return^ \"P\" } }\n"
             "return^ $\"got {p}\"\n");
    CHECK_STRING(&r, "got P");
    run_dispose(&r);

    LHAT_TEST("a hole runs where it stands, not where the string was made");
    run_text(&r,
             "let^ n = 1\n"
             "let^ show = f^ -> string^ { return^ $\"n is {n}\" }\n"
             "n := 2\n"
             "return^ show()\n");
    CHECK_STRING(&r, "n is 2");
    run_dispose(&r);
}

// 02 の 13.7: the variadic collector, made to actually compile and run.
// compile_subroutine used to refuse any variadic parameter outright.
static void test_variadic(void)
{
    Run r;

    LHAT_TEST("a variadic sum over what was actually passed");
    run_text(&r,
             "let^ sum = f^ ...:number^ -> number^ {\n"
             "  let^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "return^ sum(1, 2, 3, 4)\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("zero variadic arguments collects an empty table");
    run_text(&r,
             "let^ sum = f^ ...:number^ -> number^ {\n"
             "  let^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "return^ sum()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("fixed parameters lead the variadic tail");
    run_text(&r,
             "let^ f = f^ label:string^, ...:number^ -> number^ {\n"
             "  let^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "return^ f(\"x\", 10, 20)\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    // 13.7's own arity: at least the fixed count, checked at run time when
    // nothing statically caught it -- run_text compiles straight past the
    // checker, so this is the runtime path answering on its own.
    LHAT_TEST("fewer than the fixed count fails at run time");
    run_text(&r,
             "let^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
             "  return^ a + b\n"
             "}\n"
             "return^ f(1)\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // 13.7: 'expr...' forwards the collected tail into another call.
    LHAT_TEST("'...' forwards into another variadic call");
    run_text(&r,
             "let^ sum = f^ ...:number^ -> number^ {\n"
             "  let^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "let^ logged = f^ ...:number^ -> number^ { return^ sum(...) }\n"
             "return^ logged(1, 2, 3, 4, 5)\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("a fixed argument may lead the forwarded spread");
    run_text(&r,
             "let^ sum3 = f^ base:number^, ...:number^ -> number^ {\n"
             "  let^ total = base\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "let^ wrap = f^ ...:number^ -> number^ {\n"
             "  return^ sum3(100, ...)\n"
             "}\n"
             "return^ wrap(1, 2, 3)\n");
    CHECK_INTEGER(&r, 106);
    run_dispose(&r);

    // 02 の 14.16: typeof^ reconstructs the signature, including the tail.
    LHAT_TEST("typeof^ reflects a variadic signature");
    run_text(&r,
             "let^ sum = f^ ...:number^ -> number^ { return^ 0 }\n"
             "return^ typeof^(sum).signature\n");
    CHECK_STRING(&r, "f^...:number^ -> number^;");
    run_dispose(&r);

    LHAT_TEST("fixed and variadic together in the signature");
    run_text(&r,
             "let^ f = f^ label:string^, ...:number^ -> number^ {\n"
             "  return^ 0\n"
             "}\n"
             "return^ typeof^(f).signature\n");
    CHECK_STRING(&r, "f^string^, ...:number^ -> number^;");
    run_dispose(&r);

    // 14.10's round trip: the printed signature has to parse back.
    LHAT_TEST("the variadic signature parses back as an annotation");
    run_text(&r,
             "let^ sum = f^ ...:number^ -> number^ { return^ 0 }\n"
             "let^ typed : f^...:number^ -> number^; = sum\n"
             "return^ typed(1, 2, 3)\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

// 02 の 15 章 and 5.11: a coroutine is one suspended frame, which is all
// 15.5 leaves room for.
static void test_coroutines(void)
{
    Run r;

    // 15.5: the call answers a coroutine and the body has not started.
    LHAT_TEST("calling a yieldable procedure runs nothing");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ { log.n := 1 yield^ 0 }\n"
             "let^ c = gen()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 15.5: not "up to the first yield^" -- the body starts at its top when
    // start() comes, so each side of a yield^ runs on its own turn.
    LHAT_TEST("the body starts at its top, not after the first yield^");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ p = p^ {\n"
             "  log.s := log.s * 10 + 1\n"
             "  yield^\n"
             "  log.s := log.s * 10 + 2\n"
             "}\n"
             "let^ c = p()\n"
             "let^ made = log.s\n"
             "c.start()\n"
             "let^ first = log.s\n"
             "c.resume(nil^)\n"
             "return^ made * 10000 + first * 100 + log.s\n");
    CHECK_INTEGER(&r, 112);  // nothing, then 1, then 12
    run_dispose(&r);

    LHAT_TEST("starting runs the body up to the yield^");
    run_text(&r,
             "let^ gen = p^ { yield^ 7 }\n"
             "let^ c = gen()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("each resume carries on from where it stopped");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "let^ c = gen()\n"
             "let^ a = c.start()\n"
             "let^ b = c.resume(nil^)\n"
             "return^ a * 10 + b\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    LHAT_TEST("the arguments of the call reach the body");
    run_text(&r,
             "let^ gen = p^n { yield^ n * 2 }\n"
             "let^ c = gen(21)\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 15.4: bidirectional. Without this there is no await^ to build (15.4).
    LHAT_TEST("yield^ answers what the resume sent");
    run_text(&r,
             "let^ gen = p^ {\n"
             "  let^ got = yield^ 0\n"
             "  yield^ got + 1\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(41)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("the body keeps its own names between resumes");
    run_text(&r,
             "let^ counter = p^ {\n"
             "  let^ n = 0\n"
             "  repeat^ {\n"
             "    n := n + 1\n"
             "    yield^ n\n"
             "  }\n"
             "}\n"
             "let^ c = counter()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("the last resume answers what the body returned");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 return^ 9 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 15.6改: and nil^ when the body has no return^ to answer with. That is
    // a value the resume really receives, not a stand-in for 03's "returns
    // nothing" -- 15.5 keeps the two apart by never letting the second one
    // reach a caller.
    LHAT_TEST("and nil^ when the body reached its end without one");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("resuming one that has finished is a fault");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DEAD_COROUTINE);
    run_dispose(&r);

    // 8.8 の S23: the checker walks a body whether or not it runs, so a let^
    // on a path that is never taken puts a member in the type that the table
    // does not have. Written down as a test because the hole is known and
    // left open -- 03 の 5.1's checks are what catches it, at the use.
    LHAT_TEST("a path let^ that never runs leaves the member absent");
    run_text(&r,
             "let^ t = { }\n"
             "let^ never = p^ { let^ t.b = 1 }\n"
             "return^ t.b + 1\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("and running it is what puts the member there");
    run_text(&r,
             "let^ t = { }\n"
             "let^ fill = p^ { let^ t.b = 1 }\n"
             "fill()\n"
             "return^ t.b + 1\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 05 の 8.6: one table per machine, made with it and rooted by it.
    LHAT_TEST("L^ answers the machine's own table");
    run_text(&r,
             "let^ L^.modules.ns1.mod1 = { greet := 7 }\n"
             "let^ L^.modules.ns1.mod2 = { x := 2 }\n"
             "return^ L^.modules.ns1.mod1.greet * 10 + L^.modules.ns1.mod2.x\n");
    CHECK_INTEGER(&r, 72);
    run_dispose(&r);

    // 8.6: what the registry holds is reachable from the machine, so a
    // collection run by hand does not take it.
    LHAT_TEST("and what it holds survives a collection");
    run_text(&r,
             "let^ L^.modules.ns1.mod1 = { greet := 7 }\n"
             "L^.collectgarbage()\n"
             "return^ L^.modules.ns1.mod1.greet\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and two machines do not share one");
    {
        Run one;
        compile_text(&one, "let^ L^.modules.a = { n := 1 }\nreturn^ 0\n");
        LhatMachine *first = lhat_machine_new();
        LhatMachine *second = lhat_machine_new();
        lhat_run(first, one.proto);
        Run two;
        compile_text(&two, "return^ L^.modules.a.n\n");
        LhatRunResult missing = lhat_run(second, two.proto);
        // 04 の 11.3 makes a key that is not there nil^, and reading through
        // it is where the machine says so.
        LHAT_CHECK_EQ_INT(missing.status, LHAT_RUN_TYPE_ERROR);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(first, two.proto).value), 1);
        lhat_machine_dispose(first);
        lhat_machine_dispose(second);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 8.8: the tables on the way are made where the path does not reach one
    // yet, and left alone where it does.
    LHAT_TEST("let^ along a path makes the tables it needs");
    run_text(&r,
             "let^ a.b.c = 1\n"
             "return^ a.b.c\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and a second path through one table does not replace it");
    run_text(&r,
             "let^ a.b.c = 1\n"
             "let^ a.b.d = 2\n"
             "let^ a.z = 3\n"
             "return^ a.b.c * 100 + a.b.d * 10 + a.z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    LHAT_TEST("and a table already there keeps what it had");
    run_text(&r,
             "let^ t = { p := 4 }\n"
             "let^ t.q = 5\n"
             "return^ t.p * 10 + t.q\n");
    CHECK_INTEGER(&r, 45);
    run_dispose(&r);

    // 5.4: the root is reached, not shadowed, so a body writing one reaches
    // the place the enclosing frame holds.
    LHAT_TEST("and a nested body reaches the root it does not own");
    run_text(&r,
             "let^ outer = { }\n"
             "let^ add = p^ { let^ outer.k = 7 }\n"
             "add()\n"
             "return^ outer.k\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 15.11: _yield^ makes the body a coroutine's without suspending it, so
    // start() runs the whole thing and finishes.
    LHAT_TEST("_yield^ does not suspend");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ fake = p^ -> number^ {\n"
             "  log.s := 1\n"
             "  _yield^ 7\n"
             "  log.s := 2\n"
             "  return^ 9\n"
             "}\n"
             "let^ c = fake()\n"
             "let^ made = log.s\n"
             "let^ ended = c.start()\n"
             "return^ made * 1000 + log.s * 100 + ended\n");
    CHECK_INTEGER(&r, 209);  // nothing ran until start(), then all of it
    run_dispose(&r);

    LHAT_TEST("and the coroutine it answers is finished after start()");
    run_text(&r,
             "let^ fake = p^ { _yield^ 1 }\n"
             "let^ c = fake()\n"
             "let^ before = c.done()\n"
             "c.start()\n"
             "if^ before { return^ 0 }\n"
             "if^ c.done() { return^ 1 }\n"
             "return^ 2\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // What it would have sent is still worked out -- only the suspending is
    // dropped, so an expression written there still does what it does.
    LHAT_TEST("what it would have sent is still evaluated");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ bump = f^ -> number^ { log.n := log.n + 1  return^ log.n }\n"
             "let^ fake = p^ { _yield^ bump() }\n"
             "let^ c = fake()\n"
             "c.start()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // Nobody resumes it, so nothing comes back. The declared type says
    // otherwise, and 03 の 5.1's checks are what catches the writer whose
    // "this never runs" turned out to be wrong.
    LHAT_TEST("but nothing comes back from it");
    run_text(&r,
             "let^ fake = p^ -> string^ {\n"
             "  let^ got : string^ = _yield^ 1\n"
             "  return^ got .. \"!\"\n"
             "}\n"
             "let^ c = fake()\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("two coroutines from one procedure are separate");
    run_text(&r,
             "let^ counter = p^ {\n"
             "  let^ n = 0\n"
             "  repeat^ { n := n + 1 yield^ n }\n"
             "}\n"
             "let^ a = counter()\n"
             "let^ b = counter()\n"
             "a.start()\n"
             "a.resume(nil^)\n"
             "return^ b.start()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 15.5 again: the caller of a yieldable procedure need not be yieldable,
    // so nothing has to be marked on the way up.
    LHAT_TEST("an ordinary procedure may drive a coroutine");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "let^ sum = p^ {\n"
             "  let^ c = gen()\n"
             "  return^ c.start() + c.resume(nil^)\n"
             "}\n"
             "return^ sum()\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 02 の 10.7: a coroutine dropped while suspended still runs what is
    // pending, and 12.6 says dispose() is how that is asked for.
    LHAT_TEST("disposing runs the finally^ the body was inside");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.7: the same finally^ is never run twice.
    LHAT_TEST("a finally^ already run is not run again at disposal");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "  yield^ 2\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("disposing one that never started has nothing to run");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "let^ c = gen()\n"
             "c.dispose()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("a disposed coroutine cannot be resumed");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DEAD_COROUTINE);
    run_dispose(&r);

    // 12.6: dispose() is what with^ calls, so a coroutine goes into a with^
    // like any other resource.
    LHAT_TEST("with^ disposes a coroutine at the end of the block");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 3\n"
             "  }\n"
             "}\n"
             "with^ c = gen()\n"
             "{\n"
             "  c.start()\n"
             "}\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.7: there is nothing waiting to resume a coroutine being disposed.
    LHAT_TEST("yield^ during disposal is a fault");
    run_text(&r,
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    yield^ 2\n"
             "  }\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_YIELD_OUTSIDE);
    run_dispose(&r);

    // 02 の 10.7: dispose() is how a program asks; the collector is what
    // happens when it never does. Dropping a suspended coroutine has to run
    // what is pending just the same, or a finally^ becomes a promise the
    // language keeps only when it is watched.
    LHAT_TEST("a coroutine dropped while suspended has its finally^ run");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // The cleanup is L^ code, so it cannot run inside the collection -- it
    // runs at the first instruction boundary after it. That boundary is
    // before the next statement, which is the part worth pinning: the delay
    // is real in the implementation and invisible to the program.
    LHAT_TEST("and has run by the next statement");
    run_text(&r,
             "let^ log = { n := 0, seen := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "log.seen := log.n\n"
             "return^ log.seen\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 10.7 again: never twice. What the program already disposed of has
    // nothing left pending, so becoming unreachable afterwards is quiet.
    LHAT_TEST("one already disposed of is not run again by the collector");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "  c.dispose()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 10.7: and the collector does not run one twice either. Nothing marks
    // a coroutine as already done -- what does the work is that its count
    // of pending cleanups is 0 afterwards, so every later collection walks
    // past it. Asked with two collections after the drop.
    LHAT_TEST("the collector does not run the same one on a later cycle");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // One that ran its body to the end has nothing pending either, and one
    // that never started never pushed anything.
    LHAT_TEST("one that finished on its own is quiet at the collection");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "  c.resume(nil^)\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("one dropped before it started has nothing to run");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := 5\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ { let^ c = gen() }\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // The queue takes one per instruction boundary, so several dropped at
    // once all run -- innermost cleanup of each, in the order they come off
    // the list. Only the count is asserted; the order between coroutines is
    // the collector's business and 10.7 promises nothing about it.
    LHAT_TEST("several dropped at once all run");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log.n := log.n + 1\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ a = gen()  a.start()\n"
             "  let^ b = gen()  b.start()\n"
             "  let^ c = gen()  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 10.7: the cleanups run innermost first here as anywhere else -- the
    // collector picks when, not what.
    LHAT_TEST("a dropped one's nested cleanups run innermost first");
    run_text(&r,
             "let^ log = { s := 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    do^{\n"
             "      yield^ 1\n"
             "    finally^:\n"
             "      log.s := log.s * 10 + 1\n"
             "    }\n"
             "  finally^:\n"
             "    log.s := log.s * 10 + 2\n"
             "  }\n"
             "}\n"
             "let^ drop = p^ {\n"
             "  let^ c = gen()\n"
             "  c.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 10.7: dropped too late for any collection to have noticed -- nothing
    // here fills the heap, so without a collection at the end of the run
    // this finally^ would never run at all. No statement can watch it
    // happen, since the answer was settled before it ran, so the table the
    // run answers with is read from here.
    LHAT_TEST("one dropped too late for a collection still runs at the end");
    run_text(&r,
             "let^ log = { 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log[1] := 5\n"
             "  }\n"
             "}\n"
             "let^ outer = p^ {\n"
             "  let^ drop = p^ {\n"
             "    let^ c = gen()\n"
             "    c.start()\n"
             "  }\n"
             "  drop()\n"
             "}\n"
             "outer()\n"
             "return^ log\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    LHAT_CHECK_EQ_INT(
        lhat_as_integer(lhat_table_get(
            (const LhatTable *)lhat_as_object(r.ran.value), lhat_integer(1))),
        5);
    run_dispose(&r);

    // The other side of the same rule: the run ending is not the machine
    // ending, so what the program is still holding is not garbage and its
    // cleanups do not run. 10.7 promises a dropped coroutine, not a live one.
    LHAT_TEST("one still held when the run ends is left alone");
    run_text(&r,
             "let^ log = { 0 }\n"
             "let^ gen = p^ {\n"
             "  do^{\n"
             "    yield^ 1\n"
             "  finally^:\n"
             "    log[1] := 5\n"
             "  }\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "return^ log\n");
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_TABLE), "a table");
    LHAT_CHECK_EQ_INT(
        lhat_as_integer(lhat_table_get(
            (const LhatTable *)lhat_as_object(r.ran.value), lhat_integer(1))),
        0);
    run_dispose(&r);

    // 16.3: a walk has no body to have left anything pending in, and holds
    // neither closure nor registers -- the collector has to leave it alone.
    LHAT_TEST("a dropped walk is collected without any of this");
    run_text(&r,
             "let^ drop = p^ {\n"
             "  let^ t = { 10, 20, 30 }\n"
             "  let^ w = t.iterate()\n"
             "  w.start()\n"
             "}\n"
             "drop()\n"
             "L^.collectgarbage()\n"
             "return^ 7\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 02 の 15.8: what a plain call does not do, yieldall^ does. The inner
    // one's yields reach the outer one's resumer.
    LHAT_TEST("yieldall^ forwards the inner one's yields");
    run_text(&r,
             "let^ a = p^ { yield^ 1 yield^ 2 }\n"
             "let^ b = p^ { yieldall^ a() yield^ 3 }\n"
             "let^ c = b()\n"
             "let^ x = c.start()\n"
             "let^ y = c.resume(nil^)\n"
             "let^ z = c.resume(nil^)\n"
             "return^ x * 100 + y * 10 + z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    // 15.8: the value of the delegation is the inner one's return value, the
    // shape PEP 380 gave a generator's return.
    LHAT_TEST("the value of yieldall^ is the inner return");
    run_text(&r,
             "let^ a = p^ { yield^ 1 return^ 9 }\n"
             "let^ b = p^ { let^ r = yieldall^ a() yield^ r }\n"
             "let^ c = b()\n"
             "c.start()\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // 15.4 through a delegation: what the resume sends reaches the inner one.
    LHAT_TEST("what the resume sends reaches the inner coroutine");
    run_text(&r,
             "let^ a = p^ {\n"
             "  let^ got = yield^ 0\n"
             "  return^ got + 1\n"
             "}\n"
             "let^ b = p^ { let^ r = yieldall^ a() yield^ r }\n"
             "let^ c = b()\n"
             "c.start()\n"
             "return^ c.resume(41)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("delegations nest");
    run_text(&r,
             "let^ a = p^ { yield^ 1 }\n"
             "let^ b = p^ { yieldall^ a() yield^ 2 }\n"
             "let^ d = p^ { yieldall^ b() yield^ 3 }\n"
             "let^ c = d()\n"
             "let^ x = c.start()\n"
             "let^ y = c.resume(nil^)\n"
             "let^ z = c.resume(nil^)\n"
             "return^ x * 100 + y * 10 + z\n");
    CHECK_INTEGER(&r, 123);
    run_dispose(&r);

    LHAT_TEST("a body that only delegates is still yieldable");
    run_text(&r,
             "let^ a = p^ { yield^ 5 }\n"
             "let^ b = p^ { yieldall^ a() }\n"
             "let^ c = b()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    // 15.8 の 15.5: the plain call is what does nothing, which is the whole
    // reason the delegation had to be written.
    LHAT_TEST("a plain call still only makes a coroutine");
    run_text(&r,
             "let^ log = { n := 0 }\n"
             "let^ a = p^ { log.n := 1 yield^ 0 }\n"
             "let^ b = p^ { a() yield^ 9 }\n"
             "let^ c = b()\n"
             "c.start()\n"
             "return^ log.n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("yield^ outside a coroutine is a fault");
    run_text(&r, "yield^ 1\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_YIELD_OUTSIDE);
    run_dispose(&r);

    // 15.2改: start and resume now split what the first resume used to do
    // silently -- the machine holds that split itself (vm.h's opening
    // comment), so each has to be called on the state that makes it mean
    // something.
    LHAT_TEST("resuming one that has never started is a fault");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.resume(nil^)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_NOT_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting one that is already suspended is a fault");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    LHAT_TEST("starting one that has already finished is a fault");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "c.start()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_COROUTINE_ALREADY_STARTED);
    run_dispose(&r);

    LHAT_TEST("start() takes no arguments");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("resume() needs exactly one argument, not zero");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume()\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("resume() needs exactly one argument, not two");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(1, 2)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // 15.6改: the two questions, which is how a consumer picks an operation
    // rather than finding out by faulting.
    LHAT_TEST("a fresh coroutine has neither started nor finished");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "return^ c.started() or^ c.done()\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);

    LHAT_TEST("a suspended one has started and has not finished");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "return^ c.started() and^ !c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a finished one answers both");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.started() and^ c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // The reason both exist: what a resume answers is Y|Ret (13.9), and a
    // body that yields nil^ and ends without a value answers nil^ either
    // way. Nothing in the value says which one it was.
    LHAT_TEST("done() tells the end from a yield^ of the same value");
    run_text(&r,
             "let^ gen = p^ { yield^ }\n"
             "let^ c = gen()\n"
             "let^ log = { s := 0 }\n"
             "c.start()\n"
             "if^ c.done() { log.s := log.s + 1 }\n"
             "c.resume(nil^)\n"
             "if^ c.done() { log.s := log.s + 10 }\n"
             "return^ log.s\n");
    CHECK_INTEGER(&r, 10);  // suspended, then finished
    run_dispose(&r);

    // 10.7: disposal ends the coroutine without the body reaching its end.
    LHAT_TEST("disposal finishes it too");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.dispose()\n"
             "return^ c.done()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // Neither runs the body, so neither is refused on a coroutine that every
    // other operation would fault on.
    LHAT_TEST("both still answer after the coroutine is dead");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "c.resume(nil^)\n"
             "return^ c.done() and^ c.started()\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("done() takes no arguments");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.done(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    LHAT_TEST("started() takes no arguments");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 }\n"
             "let^ c = gen()\n"
             "c.started(1)\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // The pair is enough to drive a coroutine that was handed over rather
    // than made here -- which done() alone could not do, since a fresh one
    // and a suspended one both answer false.
    LHAT_TEST("the two together drive one of unknown state");
    run_text(&r,
             "let^ gen = p^ { yield^ 1 yield^ 2 yield^ 3 }\n"
             "let^ drain = p^ c {\n"
             "  let^ n = 0\n"
             "  if^ !c.started() { c.start() n := n + 1 }\n"
             "  repeat^ while^ !c.done() { c.resume(nil^) n := n + 1 }\n"
             "  return^ n\n"
             "}\n"
             "return^ drain(gen())\n");
    CHECK_INTEGER(&r, 4);  // three yields, then the end
    run_dispose(&r);

    // yieldall^ drives a freshly made coroutine on its own, with no start()
    // written anywhere -- 15.8's whole point is that the delegation handles
    // this by itself.
    LHAT_TEST("yieldall^ still starts a fresh coroutine on its own");
    run_text(&r,
             "let^ a = p^ { yield^ 1 }\n"
             "let^ b = p^ { yieldall^ a() }\n"
             "let^ c = b()\n"
             "return^ c.start()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);
}

// 02 の 17 章. 17.9 makes a match sugar over an if-chain, so what is worth
// pinning is that the chain comes out right -- not the instructions.
static void test_patterns(void)
{
    Run r;

    LHAT_TEST("a value pattern picks its arm");
    run_text(&r,
             "let^ n = 2\n"
             "let^ x = 0\n"
             "for^ n { when^ 1: x := 10 when^ 2: x := 20 other^: x := 30 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("other^ takes what is left");
    run_text(&r,
             "let^ n = 9\n"
             "let^ x = 0\n"
             "for^ n { when^ 1: x := 10 other^: x := 30 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    // 17.3: to^ includes both ends, as 16.4 has it.
    LHAT_TEST("a range pattern includes both ends");
    run_text(&r,
             "let^ seen = 0\n"
             "for^ let^ i := 1 to^ 5 {\n"
             "  for^ i { when^ 2 to^ 4: seen := seen + 1 other^: }\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 17.3: several on one arm, which 17.9 makes an or^.
    LHAT_TEST("patterns separated by commas share an arm");
    run_text(&r,
             "let^ hits = 0\n"
             "for^ let^ i := 1 to^ 6 {\n"
             "  for^ i { when^ 2, 3, 5: hits := hits + 1 other^: }\n"
             "}\n"
             "return^ hits\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 17.2: the subject is evaluated once, which is the point of for^ being
    // where a focus is defined (16.1).
    LHAT_TEST("the subject is evaluated once");
    run_text(&r,
             "let^ calls = { n := 0 }\n"
             "let^ get = f^ { calls.n := calls.n + 1 return^ 2 }\n"
             "for^ get() { when^ 1: when^ 2: when^ 3: other^: }\n"
             "return^ calls.n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 16.2 and 17.2: unnamed, the subject is it^; named, it is the name.
    LHAT_TEST("the subject is reached through it^");
    run_text(&r,
             "let^ x = 0\n"
             "for^ 7 { when^ 7: x := it^ other^: }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("and through a name when one is written");
    run_text(&r,
             "let^ x = 0\n"
             "for^ let^ n := 7 { when^ 7: x := n other^: }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    LHAT_TEST("the subject is gone after the match");
    run_text(&r, "for^ let^ n := 1 { when^ 1: other^: }\nreturn^ n\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 8.6改・16.3改: without let^, the subject's name reaches an existing
    // outer one instead of a fresh one -- the pattern itself still has to
    // compile against it, not just the body.
    LHAT_TEST("a bare name reuses an outer subject, patterns included");
    run_text(&r, "let^ n = 7\nfor^ n := 7 { when^ 7: n := 100 other^: n := 200 }\nreturn^ n\n");
    CHECK_INTEGER(&r, 100);
    run_dispose(&r);

    // 17.4: a type pattern writes isa^, since a bare name could be either.
    LHAT_TEST("a type pattern uses isa^");
    run_text(&r,
             "errordef^ E { A, B }\n"
             "let^ x = 0\n"
             "for^ error^E.B{ } {\n"
             "  when^ isa^ E.A: x := 1\n"
             "  when^ isa^ E.B: x := 2\n"
             "  other^: x := 3\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    // 17.2 の 式形. 17.6: only the ':' of for^ opens, and ';' closes it all.
    LHAT_TEST("the expression form answers a value");
    run_text(&r,
             "let^ n = 2\n"
             "let^ r = for^ n: when^ 1: 10 when^ 2: 20 other^: 30 ;\n"
             "return^ r\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("and reaches its subject the same way");
    run_text(&r,
             "let^ r = for^ 3: when^ 1 to^ 2: 0 other^: it^ * 100 ;\n"
             "return^ r\n");
    CHECK_INTEGER(&r, 300);
    run_dispose(&r);

    // 17.8: no guards. The arm's body is where a further test goes.
    LHAT_TEST("a further test goes inside the arm");
    run_text(&r,
             "let^ x = 0\n"
             "for^ let^ n := 5 {\n"
             "  when^ 1 to^ 9:\n"
             "    if^ n > 3 { x := 1 else^: x := 2 }\n"
             "  other^:\n"
             "    x := 3\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);
}

// 03 の 1.2 keeps Lua's incremental collector as something to borrow later.
// What is pinned here is that the working form reclaims what a program lets
// go of and keeps what it does not.
static void test_collection(void)
{
    Run r;

    // Every turn makes a table and drops it. Without a collector these all
    // pile up until the run ends.
    LHAT_TEST("what the program lets go of is reclaimed");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 2000 { let^ t = { a := 1, b := 2 } n := n + 1 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2000);
    LHAT_CHECK(r.ran.collected > 1000, "the collector ran and freed");
    LHAT_CHECK(r.ran.live < 500, "little is left at the end");
    run_dispose(&r);

    // The same loop, holding on to every table. Nothing may be freed.
    LHAT_TEST("what the program holds is kept");
    run_text(&r,
             "let^ kept = { }\n"
             "for^ let^ i := 1 to^ 2000 { kept[i] := { a := i } }\n"
             "return^ kept[1500].a\n");
    CHECK_INTEGER(&r, 1500);
    LHAT_CHECK(r.ran.live > 2000, "every table held is still there");
    run_dispose(&r);

    // 5.4: a closure keeps the place it captured, so what that place holds
    // has to survive the collection too.
    LHAT_TEST("a captured place keeps its value alive");
    run_text(&r,
             "let^ get = f^ { return^ 0 }\n"
             "do^{\n"
             "  let^ held = { n := 42 }\n"
             "  get := f^ { return^ held.n }\n"
             "}\n"
             "repeat^ 2000 { let^ waste = { a := 1 } }\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // A suspended coroutine holds its registers, and they are roots through
    // it rather than through any frame.
    LHAT_TEST("a suspended coroutine keeps what its registers hold");
    run_text(&r,
             "let^ gen = p^ {\n"
             "  let^ mine = { n := 7 }\n"
             "  yield^ 0\n"
             "  yield^ mine.n\n"
             "}\n"
             "let^ c = gen()\n"
             "c.start()\n"
             "repeat^ 2000 { let^ waste = { a := 1 } }\n"
             "return^ c.resume(nil^)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // A string the answer points at has to outlive every collection between
    // it being made and the run ending.
    LHAT_TEST("the answer survives");
    run_text(&r,
             "let^ s = \"kept\"\n"
             "repeat^ 2000 { let^ waste = { a := 1 } }\n"
             "return^ s .. \"!\"\n");
    CHECK_STRING(&r, "kept!");
    run_dispose(&r);

    // A cycle is unreachable but points at itself, so reference counting
    // would keep it. Marking from the roots does not.
    LHAT_TEST("a cycle the program dropped is reclaimed");
    run_text(&r,
             "let^ n = 0\n"
             "repeat^ 2000 {\n"
             "  let^ a = { }\n"
             "  let^ b = { }\n"
             "  a.other := b\n"
             "  b.other := a\n"
             "  n := n + 1\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 2000);
    LHAT_CHECK(r.ran.live < 500, "the cycles went");
    run_dispose(&r);

    // 14.2's link from an instance to its definition is a reference like any
    // other, and the definition outlives the instances.
    LHAT_TEST("a definition outlives the instances that read it");
    run_text(&r,
             "let^ Foo = def^{ self^{ n := 0 }, get := f^self^ { return^ self^.n } }\n"
             "let^ last = Foo.new^()\n"
             "repeat^ 2000 { let^ f = Foo.new^() }\n"
             "return^ last.get()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

// 03 の 4 章: a REPL is one machine answering many inputs, so the machine has
// to be an object a caller keeps rather than something a run owns.
static void test_machine(void)
{
    LHAT_TEST("a machine answers more than one input");
    {
        LhatMachine *m = lhat_machine_new();
        LHAT_CHECK(m != NULL, "a machine");
        Run first, second;
        compile_text(&first, "return^ 6 * 7\n");
        compile_text(&second, "return^ \"and\" .. \" again\"\n");
        LhatRunResult a = lhat_run(m, first.proto);
        LhatRunResult b = lhat_run(m, second.proto);
        LHAT_CHECK_EQ_INT(a.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 42);
        LHAT_CHECK_EQ_INT(b.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(b.value, LHAT_OBJECT_STRING),
                   "a string came back");
        // What each run allocated is still on the machine when the next one
        // starts, which is what a REPL needs of it.
        LHAT_CHECK(b.live >= a.live, "the heap carried over");
        lhat_machine_dispose(m);
        compiled_dispose(&first);
        compiled_dispose(&second);
    }

    // The static one could serve only one caller and never nest.
    LHAT_TEST("and two machines stand side by side");
    {
        LhatMachine *one = lhat_machine_new();
        LhatMachine *two = lhat_machine_new();
        Run text;
        compile_text(&text, "return^ 5\n");
        LhatRunResult a = lhat_run(one, text.proto);
        LhatRunResult b = lhat_run(two, text.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 5);
        LHAT_CHECK_EQ_INT(lhat_as_integer(b.value), 5);
        lhat_machine_dispose(one);
        lhat_machine_dispose(two);
        compiled_dispose(&text);
    }

    // 03 の 4.3: the top-level names of one input are still there for the
    // next, which is what a session is for.
    LHAT_TEST("a session carries top-level names between inputs");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s, "let^ x = 40\nreturn^ x\n");
        compile_next_text(&two, s, "let^ y = 2\nreturn^ x + y\n");
        compile_next_text(&three, s, "x := x + y\nreturn^ x\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 40);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 42);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 42);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    LHAT_TEST("and a subroutine one input made is callable in the next");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(
            &one, s,
            "let^ greet = f^ n:string^ -> string^ { return^ \"hi \" .. n }\n");
        compile_next_text(&two, s, "return^ greet(\"there\")\n");
        lhat_run(m, one.proto);
        LhatRunResult r = lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(r.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_object_kind(r.value, LHAT_OBJECT_STRING),
                   "the subroutine survived");
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 03 の 4.3: a name written again keeps the slot it had, so a prompt does
    // not run out of registers however many times a line is rewritten.
    LHAT_TEST("a name written again keeps its slot");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run turns[300];
        size_t taken = 0;
        bool all_compiled = true;
        for (size_t i = 0; i < 300; i++) {
            char text[64];
            snprintf(text, sizeof text, "let^ x = %zu\nreturn^ x\n", i);
            compile_next_text(&turns[taken], s, text);
            if (turns[taken].compiled != LHAT_COMPILE_OK) {
                all_compiled = false;
                compiled_dispose(&turns[taken]);
                break;
            }
            taken++;
        }
        LHAT_CHECK(all_compiled, "300 rewrites of one name compiled");
        if (taken > 0) {
            LHAT_CHECK_EQ_INT(
                lhat_as_integer(lhat_run(m, turns[taken - 1].proto).value),
                (int64_t)(taken - 1));
        }
        for (size_t i = 0; i < taken; i++) {
            compiled_dispose(&turns[i]);
        }
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
    }

    // 8.7 keeps a name visible before its let^ runs, and the slot still holds
    // what the last input put there.
    LHAT_TEST("and a redefinition reads what is already in it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(&one, s, "let^ x = 1\n");
        compile_next_text(&two, s, "let^ x = x + 10\nreturn^ x\n");
        lhat_run(m, one.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 11);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    // 03 の 4.3: an input answers with the value of its last statement, when
    // that statement is an expression -- so a prompt shows a call's result
    // without a return^ written by hand.
    LHAT_TEST("an input answers with its last expression");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_asked_text(&one, s, "2 + 3\n");
        compile_asked_text(
            &two, s,
            "let^ twice = f^ n:number^ -> number^ { return^ n * 2 }\n");
        compile_asked_text(&three, s, "twice(21)\n");
        // Only the last one answers; the statements before it still run.
        compile_asked_text(&four, s, "let^ k = 7\nk + 1\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 5);
        LhatRunResult made = lhat_run(m, two.proto);
        LHAT_CHECK(lhat_is_nil(made.value), "a let^ answers with nothing");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 42);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 8);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // The value is the answer, not a return^ -- a call answering nothing is
    // still a statement, and what follows it still runs.
    LHAT_TEST("and a call answering nothing does not stop the input");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one;
        compile_asked_text(&one, s,
                           "let^ n = 0\n"
                           "let^ bump = p^ { n := n + 1 }\n"
                           "bump()\n"
                           "bump()\n"
                           "n\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, one.proto).value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
    }

    // 02 の 14.2 and 04 の 2.4: what a def^ composes onto and what an
    // errordef^ declared are worked out while compiling and never reach the
    // machine, so an input that needs them needs what an earlier one found.
    LHAT_TEST("a def^ from an earlier input can be composed onto");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s, "let^ A = def^{ self^{ x = 1 } }\n");
        compile_next_text(&two, s,
                          "let^ B = A .. def^{ bar := f^self^ -> number^ "
                          "{ return^ 2 } }\n");
        compile_next_text(&three, s,
                          "let^ b = B.new^()\n"
                          "return^ b.x * 10 + b.bar()\n");
        LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(three.compiled, LHAT_COMPILE_OK);
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        // 12 is the field from the first input and the method from the
        // second: a node read through the wrong input's lexer would name a
        // different field and answer nil^.
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 12);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    LHAT_TEST("and an errordef^ from an earlier input is still declared");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two;
        compile_next_text(&one, s, "errordef^ E { Bad, Worse }\n");
        compile_next_text(&two, s,
                          "let^ e = error^ E.Worse { }\n"
                          "if^ e isa^ E.Worse { return^ 1 }\n"
                          "return^ 0\n");
        LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
        lhat_run(m, one.proto);
        // The second kind, so a lookup reading the declaration through this
        // input's lexer would not find it by name at all.
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, two.proto).value), 1);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
    }

    LHAT_TEST("and an overload^ added in a later input is callable");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three;
        compile_next_text(&one, s,
                          "let^ Foo = def^{ self^{}, "
                          "foo := f^self^ -> number^ { return^ 1 } }\n");
        compile_next_text(&two, s,
                          "let^ Bar = Foo .. def^{ overload^ "
                          "foo := f^self^, s:string^ -> number^ "
                          "{ return^ 2 } }\n");
        compile_next_text(&three, s,
                          "let^ b = Bar.new^()\n"
                          "return^ b.foo() * 10 + b.foo(\"x\")\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 12);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
    }

    // 03 の 4.3 with 5.4: the session's top level goes on holding its slots
    // between inputs, so a place a closure captured in one input is the very
    // place a later one reads and writes. Which is what makes a prompt agree
    // with a file: the same three lines in one unit answer 2 as well.
    LHAT_TEST("a ':=' inside a closure reaches a later input");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "let^ a = 1\n");
        compile_next_text(&two, s,
                          "let^ f = f^ -> number^ { a := 2 return^ 1 }\n");
        compile_next_text(&three, s, "return^ f()\n");
        compile_next_text(&four, s, "return^ a\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 1);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // Severing one name's sharing is severing that name's, not the slot's
    // neighbours' -- every let^ of a name at a session's top level goes back
    // in the one slot, so a CLOSE over the whole frame would take the
    // bindings above it with it.
    LHAT_TEST("and writing another name over does not sever it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "let^ a = 1\nlet^ b = 2\n");
        compile_next_text(&two, s,
                          "let^ g = f^ -> number^ { b := 9 return^ 0 }\n");
        compile_next_text(&three, s, "let^ a = 5\nreturn^ g()\n");
        compile_next_text(&four, s, "return^ b\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        lhat_run(m, three.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 9);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // 5.4's sharing is of one binding, not of the slot it happens to sit in.
    // A let^ writing the name again is a new binding -- it reuses the slot,
    // so what captured the earlier one has to stop sharing it there. That is
    // the difference from the ':=' above, which writes the same binding.
    LHAT_TEST("a closure keeps what it captured when its input ended");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "let^ x = 1\n");
        compile_next_text(&two, s,
                          "let^ show = f^ -> number^ { return^ x }\n");
        compile_next_text(&three, s, "let^ x = 2\nreturn^ x\n");
        compile_next_text(&four, s, "return^ show()\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, three.proto).value), 2);
        LHAT_CHECK_EQ_INT(lhat_as_integer(lhat_run(m, four.proto).value), 1);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // Which is what keeps a redefinition to another type from making an
    // earlier closure's result type a lie.
    LHAT_TEST("so redefining to another type does not reach it");
    {
        LhatMachine *m = lhat_machine_new();
        LhatCompileSession *s = lhat_compile_session_new();
        Run one, two, three, four;
        compile_next_text(&one, s, "let^ x = 1\n");
        compile_next_text(&two, s,
                          "let^ show = f^ -> number^ { return^ x }\n");
        compile_next_text(&three, s, "let^ x = \"now\"\n");
        compile_next_text(&four, s, "return^ show() + 1\n");
        lhat_run(m, one.proto);
        lhat_run(m, two.proto);
        lhat_run(m, three.proto);
        LhatRunResult r = lhat_run(m, four.proto);
        LHAT_CHECK_EQ_INT(r.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 2);
        lhat_machine_dispose(m);
        lhat_compile_session_dispose(s);
        compiled_dispose(&one);
        compiled_dispose(&two);
        compiled_dispose(&three);
        compiled_dispose(&four);
    }

    // One proto, several runs: 5.2 and 5.4 make the registers and the frames
    // the run's, so nothing of one is left over in the next.
    LHAT_TEST("and one proto may be run again");
    {
        LhatMachine *m = lhat_machine_new();
        Run text;
        compile_text(&text, "let^ n = 0\nn := n + 1\nreturn^ n\n");
        LhatRunResult a = lhat_run(m, text.proto);
        LhatRunResult b = lhat_run(m, text.proto);
        LHAT_CHECK_EQ_INT(lhat_as_integer(a.value), 1);
        LHAT_CHECK_EQ_INT(lhat_as_integer(b.value), 1);
        lhat_machine_dispose(m);
        compiled_dispose(&text);
    }
}

int main(void)
{
    test_encoding();
    test_arithmetic();
    test_names();
    test_control();
    test_calls();
    test_closures();
    test_strings();
    test_tables();
    test_repeat();
    test_for();
    test_loop_clauses();
    test_errors();
    test_catch_and_try();
    test_cleanups();
    test_definitions();
    test_typeof();
    test_tostring();
    test_scope_specifiers();
    test_interpolation();
    test_variadic();
    test_coroutines();
    test_patterns();
    test_collection();
    test_machine();
    return lhat_test_report("test_vm");
}
