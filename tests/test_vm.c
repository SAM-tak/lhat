// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <string.h>

#include "code.h"
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
    LhatRunResult ran;
} Run;

static void run_text(Run *r, const char *text)
{
    lhat_source_init_from_string(&r->source, "<test>", text, strlen(text));
    lhat_lexer_init(&r->lexer, &r->source);
    lhat_parse(&r->lexer, &r->parsed);
    r->compiled = lhat_compile(r->parsed.root, &r->lexer, &r->proto);
    memset(&r->ran, 0, sizeof r->ran);
    if (r->compiled == LHAT_COMPILE_OK) {
        r->ran = lhat_run(r->proto);
    }
}

static void run_dispose(Run *r)
{
    lhat_proto_free(r->proto);
    lhat_parse_result_dispose(&r->parsed);
    lhat_lexer_dispose(&r->lexer);
    lhat_source_dispose(&r->source);
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

    LHAT_TEST("'//' by zero fails");
    run_text(&r, "return^ 1 // 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DIVIDE_BY_ZERO);
    run_dispose(&r);

    LHAT_TEST("'%' by zero fails");
    run_text(&r, "return^ 1 % 0\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_DIVIDE_BY_ZERO);
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
    LHAT_TEST("':=' inside a block reaches out");
    run_text(&r, "let^ x = 1\ndo^{ x := 9 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("let^ inside a block does not");
    run_text(&r, "let^ x = 1\ndo^{ let^ x = 9 }\nreturn^ x\n");
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

int main(void)
{
    test_encoding();
    test_arithmetic();
    test_names();
    test_control();
    test_calls();
    test_closures();
    return lhat_test_report("test_vm");
}
