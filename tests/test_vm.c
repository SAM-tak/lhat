// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

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
    lhat_run_result_dispose(&r->ran);
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

#define CHECK_STRING(r, expected)                                             \
    do {                                                                      \
        LHAT_CHECK_EQ_INT((r)->compiled, LHAT_COMPILE_OK);                    \
        LHAT_CHECK_EQ_INT((r)->ran.status, LHAT_RUN_OK);                      \
        LHAT_CHECK(lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING),   \
                   "expected a string");                                      \
        if (lhat_is_object_kind((r)->ran.value, LHAT_OBJECT_STRING)) {        \
            const LhatString *s =                                             \
                (const LhatString *)lhat_as_object((r)->ran.value);           \
            LHAT_CHECK(strcmp(s->text, (expected)) == 0, (expected));         \
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

    LHAT_TEST("repeat^ on its own needs break^ to end");
    run_text(&r,
             "let^ i = 0\n"
             "repeat^ { i := i + 1 if^ i = 3 { break^ } }\n"
             "return^ i\n");
    CHECK_INTEGER(&r, 3);
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

    LHAT_TEST("break^ outside a loop does not compile");
    run_text(&r, "break^\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNSUPPORTED);
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
             "for^ i := 1 to^ 4 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // 16.4: the direction is not inferred, so this is empty rather than
    // counting down.
    LHAT_TEST("a limit below the start runs none");
    run_text(&r,
             "let^ x = 0\n"
             "for^ i := 1 to^ 0 { x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("downto^ counts down");
    run_text(&r,
             "let^ seen = 0\n"
             "for^ i := 3 downto^ 1 { seen := seen * 10 + i }\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 321);
    run_dispose(&r);

    // 16.4: step^ is a positive amount either way; the sign belongs to the
    // clause.
    LHAT_TEST("step^ is a positive amount for both directions");
    run_text(&r,
             "let^ up = 0\n"
             "for^ i := 1 to^ 9 step^ 3 { up := up * 10 + i }\n"
             "return^ up\n");
    CHECK_INTEGER(&r, 147);
    run_dispose(&r);

    run_text(&r,
             "let^ down = 0\n"
             "for^ i := 9 downto^ 1 step^ 3 { down := down * 10 + i }\n"
             "return^ down\n");
    CHECK_INTEGER(&r, 963);
    run_dispose(&r);

    // 16.4: the bound says how far the loop goes, so it is read before the
    // loop starts and cannot move while it runs.
    LHAT_TEST("the bound is read once");
    run_text(&r,
             "let^ n = 3\n"
             "let^ x = 0\n"
             "for^ i := 1 to^ n { n := 100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and downto^ reads its bound once too");
    run_text(&r,
             "let^ n = 1\n"
             "let^ x = 0\n"
             "for^ i := 3 downto^ n { n := -100 x := x + 1 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // step^ is the amount each advance moves by rather than a limit fixed
    // before the loop, so it is read every time round.
    LHAT_TEST("step^ is read every time round");
    run_text(&r,
             "let^ s = 1\n"
             "let^ n = 0\n"
             "for^ i := 1 to^ 9 step^ s { n := n + 1 s := 3 }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 3);  // i is 1, 4, 7. Reading s once would give nine
    run_dispose(&r);

    LHAT_TEST("10 to^ 1 step^ 2 is empty rather than confusing");
    run_text(&r,
             "let^ x = 0\n"
             "for^ i := 10 to^ 1 step^ 2 { x := x + 1 }\n"
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
             "for^ i := 1 while^ i ≦ 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("until^ is the same the other way round");
    run_text(&r,
             "let^ total = 0\n"
             "for^ i := 1 until^ i > 4 next^ i := i + 1 { total := total + i }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("the focus is gone after the loop");
    run_text(&r, "for^ i := 1 to^ 2 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    LHAT_TEST("loops nest");
    run_text(&r,
             "let^ n = 0\n"
             "for^ i := 1 to^ 3 {\n"
             "  for^ j := 1 to^ 4 { n := n + 1 }\n"
             "}\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 16.3 and 16.1: this one does not repeat. It is the do^ block written
    // without the extra nesting.
    LHAT_TEST("if^ uses the focus once and does not repeat");
    run_text(&r,
             "let^ x = 0\n"
             "for^ i := 1, j := 2 if^ i + j < 10 { x := i + j }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and its focus does not escape either");
    run_text(&r, "for^ i := 1 if^ i > 0 { }\nreturn^ i\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 14 章 の table with 16: what a loop is mostly for.
    LHAT_TEST("a loop fills a table");
    run_text(&r,
             "let^ t = { }\n"
             "for^ i := 1 to^ 4 { t[i] := i * i }\n"
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
             "for^ i := 1 to^ 4 {\n"
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
             "for^ i := 1 to^ 10 {\n"
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
             "for^ i := 1 to^ 3 {\n"
             "  get := f^ { return^ i }\n"
             "}\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 4);
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
    test_strings();
    test_tables();
    test_repeat();
    test_for();
    test_loop_clauses();
    return lhat_test_report("test_vm");
}
