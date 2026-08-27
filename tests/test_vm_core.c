// L^ (lhat) -- tests for the bytecode and the machine.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. Programs are run end to end, since what is worth pinning is the
// answer rather than the instructions chosen to reach it -- 5.1 expects those
// to be replaced by specialised ones later.

#include <math.h>
#include <string.h>

#include "code.h"
#include "fixture.h"

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

    // 5.2: 256 registers, and a call's arguments all live at once. A call
    // written with 300 of them is refused as a whole rather than mis-encoded.
    LHAT_TEST("an expression needing more registers than exist is refused");
    {
        char text[4096];
        size_t at = 0;
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "var^ log = p^... { }\nlog(");
        for (int i = 0; i < 300; i++) {
            at += (size_t)snprintf(text + at, sizeof text - at,
                                   i != 0 ? ",1" : "1");
        }
        snprintf(text + at, sizeof text - at, ")\nreturn^ 1\n");

        Run r;
        compile_text(&r, text);
        LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_TOO_COMPLEX);
        compiled_dispose(&r);
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
             "var^ f = f^ n:number^ -> number^ {\n"
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

    // 04 の 11.2: like an overflow (14.8改), a zero divisor widens to
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
    CHECK_INTEGER(&r, -4);
    run_dispose(&r);

    // 5.1: the generic instruction checks what it was given.
    LHAT_TEST("arithmetic on a bool is refused at run time");
    run_text(&r, "return^ true^ + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);
}

// 02 の 11.6改3: one cast, which answers the value or a failure. What it
// asks is lhat_value_satisfies, the same question 14.12's overload search
// puts to a candidate.
static void test_casts(void)
{
    Run r;

    LHAT_TEST("as^ answers the value where it fits");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ 7 }\n"
                     "return^ f() as^ number^ catch^ 0\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 11.6改3: and a localerror^.CastFailure where it does not, which the
    // catch^ picks up. The run goes on -- what used to stop it is now an
    // answer the writer has to have said something about.
    LHAT_TEST("and a failure where it does not, which catch^ picks up");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "return^ f() as^ number^ catch^ 0\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // 04 の 4.1改: what 'as^ T' was before 11.6改3 -- the assertion. panic^
    // is the one statement catch^ takes on its right, and it answers
    // nothing, so the whole is the left without its error arm: number^ here,
    // which is what lets the sum be written.
    LHAT_TEST("catch^ panic^ it^ is the assertion");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ 7 }\n"
                     "let^ v = f() as^ number^ catch^ panic^ it^\n"
                     "return^ v + 1\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);

    LHAT_TEST("and it stops the run where the cast fails");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "let^ v = f() as^ number^ catch^ panic^ it^\n"
                     "return^ v + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    run_dispose(&r);

    // 4.2: it^ is the error inside the arm, so what panic^ carries may be
    // built out of it rather than only be it.
    LHAT_TEST("it^ is the caught error inside the panic^ arm");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "let^ v = f() as^ number^ catch^ panic^ it^.message\n"
                     "return^ v + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    run_dispose(&r);

    // The longer form, which is what to write when the failure is worth
    // more than stopping -- and which still narrows v to T on the way out.
    LHAT_TEST("the assertion is also writable as a narrowing");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "let^ v = f() as^ number^\n"
                     "if^ v fits^ localerror^.CastFailure { panic^ v }\n"
                     "return^ v + 1\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_PANIC);
    run_dispose(&r);

    LHAT_TEST("and the narrowed value is the one that fitted");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ 7 }\n"
                     "let^ v = f() as^ number^\n"
                     "if^ v fits^ localerror^.CastFailure { panic^ v }\n"
                     "return^ v + 1\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);

    // 11.6: as^ stays stronger than the binary operators, so the sum is
    // around the cast rather than the cast around the sum -- and stronger
    // than catch^, so the alternative is the cast's and not the sum's.
    LHAT_TEST("and as^ still binds tighter than a binary operator");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ 7 }\n"
                     "return^ 1 + f() as^ number^ catch^ 0\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);

    // 04 の 2.6: what comes back is an error like any other, so fits^ tells
    // it apart -- which is how a writer gets at it without a catch^.
    LHAT_TEST("the failure it answers is an error of that kind");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "let^ r = f() as^ number^ catch^ it^\n"
                     "return^ r fits^ localerror^.CastFailure\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 2.7: and it is not an error^, which is the disjointness holding at run
    // time as well as in the checker (03 の 4.2).
    LHAT_TEST("and it is not an error^");
    run_checked_text(&r,
                     "let^ f = f^ -> any^ { return^ \"t\" }\n"
                     "let^ r = f() as^ number^ catch^ it^\n"
                     "return^ (r fits^ localerror^) and^ !(r fits^ error^)\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

static void test_names(void)
{
    Run r;

    // 02 の 8.6: var^ makes the name, ':=' reaches it.
    LHAT_TEST("a name holds its value");
    run_text(&r, "var^ x = 2\nvar^ y = 3\nreturn^ x * y\n");
    CHECK_INTEGER(&r, 6);
    run_dispose(&r);

    LHAT_TEST("':=' reassigns rather than shadowing");
    run_text(&r, "var^ x = 1\nx := x + 41\nreturn^ x\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.6's whole point: the inner statement reaches the outer name.
    // 7.4: 'target op= value' means 'target := target op value'.
    LHAT_TEST("every compound assignment operator");
    run_text(&r, "var^ x = 10\nx += 5\nreturn^ x\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    run_text(&r, "var^ x = 10\nx -= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    run_text(&r, "var^ x = 10\nx *= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 30);
    run_dispose(&r);

    run_text(&r, "var^ x = 10\nx /= 4\nreturn^ x\n");
    CHECK_REAL(&r, 2.5);
    run_dispose(&r);

    run_text(&r, "var^ x = 10\nx %= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    run_text(&r, "var^ x = 10\nx //= 3\nreturn^ x\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    run_text(&r, "var^ x = 2\nx **= 5\nreturn^ x\n");
    CHECK_INTEGER(&r, 32);
    run_dispose(&r);

    run_text(&r, "var^ x = 4\nx **= -1\nreturn^ x\n");
    CHECK_REAL(&r, 0.25);
    run_dispose(&r);

    run_text(&r, "var^ x = \"ab\"\nx ..= \"cd\"\nreturn^ x = \"abcd\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 7.4's whole reason to exist: the target is read once, not twice.
    // A path target proves it, since only there does re-evaluating the
    // target run something with a side effect.
    LHAT_TEST("a path target is evaluated once, not twice");
    run_text(&r,
             "var^ t = { 100, 200 }\n"
             "var^ calls = 0\n"
             "var^ idx = p^ { calls := calls + 1\nreturn^ 1 }\n"
             "t[idx()] += 5\n"
             "return^ t.1 = 105 and^ calls = 1\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("and the same holds for a member target");
    run_text(&r,
             "var^ t = { x := 10 }\n"
             "t.x += 7\n"
             "return^ t.x\n");
    CHECK_INTEGER(&r, 17);
    run_dispose(&r);

    // 8.6改3: 13.8 offers 'a, b := b, a' in place of multiple return values,
    // which it is only if nothing is stored until every value has been read.
    LHAT_TEST("multiple assignment exchanges two values");
    run_text(&r, "var^ a = 1\nvar^ b = 2\na, b := b, a\nreturn^ a * 10 + b\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    LHAT_TEST("and rotates three");
    run_text(&r,
             "var^ p = 1\nvar^ q = 2\nvar^ s = 3\n"
             "p, q, s := s, p, q\n"
             "return^ p * 100 + q * 10 + s\n");
    CHECK_INTEGER(&r, 312);
    run_dispose(&r);

    LHAT_TEST("members exchange the same way");
    run_text(&r,
             "var^ t = { x := 10, y := 20 }\n"
             "t.x, t.y := t.y, t.x\n"
             "return^ t.x * 100 + t.y\n");
    CHECK_INTEGER(&r, 2010);
    run_dispose(&r);

    LHAT_TEST("and so does a mixture of a member and a name");
    run_text(&r,
             "var^ u = { a := 1 }\nvar^ n = 0\n"
             "u.a, n := n, u.a\n"
             "return^ u.a * 10 + n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 8.6改2: several targets, paired by position with the values.
    LHAT_TEST("compound assignment takes several targets");
    run_text(&r,
             "var^ a = 10\nvar^ b = 20\na, b += 1, 2\nreturn^ a * 100 + b\n");
    CHECK_INTEGER(&r, 1122);
    run_dispose(&r);

    LHAT_TEST("and reads every value before writing any of them");
    run_text(&r, "var^ x = 1\nvar^ y = 2\nx, y += y, x\nreturn^ x * 10 + y\n");
    CHECK_INTEGER(&r, 33);
    run_dispose(&r);

    LHAT_TEST("and reaches members too");
    run_text(&r,
             "var^ t = { p := 100, q := 200 }\n"
             "t.p, t.q *= 2, 3\n"
             "return^ t.p + t.q\n");
    CHECK_INTEGER(&r, 800);
    run_dispose(&r);

    LHAT_TEST("':=' inside a block reaches out");
    run_text(&r, "var^ x = 1\ndo^{ x := 9 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("var^ inside a block does not");
    run_text(&r, "var^ x = 1\ndo^{ var^ x = 9 }\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 8.6: var^ present means define regardless of which spelling follows --
    // ':=' after var^ is accepted as a convenience, not a different meaning.
    LHAT_TEST("var^ also defines with ':='");
    run_text(&r, "var^ x := 1\nreturn^ x\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("an unknown name does not compile");
    run_text(&r, "return^ nowhere\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_UNDEFINED);
    run_dispose(&r);

    // 03 の 3.4改: a literal called where it is written takes its parameter
    // types from that call, and a name called anywhere else takes nothing --
    // which is 3.4's line, kept. Asked of the body itself, since what the
    // parameter ended up as is what the body was checked with.
    LHAT_TEST("a call seeds a literal's parameter, and a name's not at all");
    run_checked_text(&r,
                     "var^ g = p^ x { return^ typeof^(x).signature }\n"
                     "return^ p^ y { return^ typeof^(y).signature }(5)\n"
                     "       .. \"|\" .. g(5)\n");
    CHECK_STRING(&r, "number^|UNKNOWN");
    run_dispose(&r);

    // 03 の 4.2 puts the refusals in the checker, so a compile that stops is
    // a hole in it -- and what closes a hole is knowing where to look. The
    // status alone said only that something somewhere would not compile.
    LHAT_TEST("and says where, and which name");
    run_text(&r, "var^ a = 1\nreturn^ nowhere\n");
    LHAT_CHECK_EQ_INT(r.compile_result.status, LHAT_COMPILE_UNDEFINED);
    LHAT_CHECK_EQ_INT(r.compile_result.line, 2);
    LHAT_CHECK_EQ_INT(r.compile_result.column, 9);
    LHAT_CHECK(r.compile_result.name != NULL &&
                   r.compile_result.name_length == 7 &&
                   memcmp(r.compile_result.name, "nowhere", 7) == 0,
               "the name is the one that was written");
    run_dispose(&r);

    // 02 の 9.8: not every status is about a name, and those carry the
    // position alone rather than an empty one.
    LHAT_TEST("a status about no name carries the position alone");
    run_text(&r, "repeat^ 3 {\n  repeat^ 3 { break^^^ }\n}\n");
    LHAT_CHECK_EQ_INT(r.compile_result.status, LHAT_COMPILE_BREAK_TOO_FAR);
    LHAT_CHECK_EQ_INT(r.compile_result.line, 2);
    LHAT_CHECK(r.compile_result.name == NULL, "no name to give");
    run_dispose(&r);

    LHAT_TEST("and a compile that worked answers a clean result");
    run_text(&r, "return^ 1\n");
    LHAT_CHECK_EQ_INT(r.compile_result.status, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.compile_result.line, 0);
    LHAT_CHECK(r.compile_result.name == NULL, "nothing failed");
    run_dispose(&r);

    // 02 の 13.12: '_^' takes its position out of the run and nothing reads
    // it back, so what is pinned is that the names beside it take theirs.
    LHAT_TEST("'_^' takes a position and the names beside it take theirs");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^, number^) {\n"
                     "  return^ 1, 2, 3 }\n"
                     "var^ a, _^, c = f()\n"
                     "return^ a * 10 + c\n");
    CHECK_INTEGER(&r, 13);
    run_dispose(&r);

    LHAT_TEST("and several of them share the one place");
    run_checked_text(&r,
                     "var^ f = f^ -> (number^, number^, number^) {\n"
                     "  return^ 1, 2, 3 }\n"
                     "var^ _^, _^, c = f()\n"
                     "return^ c\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    // 13.12 with 15.8: what is thrown away is the name, not the work. The
    // call still runs, which is why 15.8's "no effect" is unaffected by
    // writing one.
    LHAT_TEST("the value written into a '_^' is still worked out");
    run_checked_text(&r,
                     "var^ n = 0\n"
                     "var^ bump = p^ -> number^ { n := n + 1  return^ 5 }\n"
                     "var^ _^ = bump()\n"
                     "return^ n\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("'_^' stands as a parameter and as a walk's focus");
    run_checked_text(&r,
                     "var^ g = f^ _^:number^, b:number^ -> number^ {\n"
                     "  return^ b }\n"
                     "var^ total = 0\n"
                     "for^ _^, v in^ { 10, 20 } { total := total + v }\n"
                     "return^ g(1, 2) + total\n");
    CHECK_INTEGER(&r, 32);
    run_dispose(&r);
}

static void test_control(void)
{
    Run r;

    LHAT_TEST("an if statement takes the true branch");
    run_text(&r,
             "var^ x = 0\n"
             "if^ 1 < 2 { x := 10 else^: x := 20 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("and the false one");
    run_text(&r,
             "var^ x = 0\n"
             "if^ 1 > 2 { x := 10 else^: x := 20 }\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 20);
    run_dispose(&r);

    LHAT_TEST("an elseif chain picks one arm");
    run_text(&r,
             "var^ x = 0\n"
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
    run_text(&r, "var^ x = if^ 1 < 2: 7 el^: 8 ;\nreturn^ x\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 5.1 with 15.12: what a body may answer with, a return^ may answer
    // with. 15.12 writes this very expression as a body, so the two
    // spellings of one answer have to reach the same value.
    LHAT_TEST("and a return^ answers with one");
    run_text(&r,
             "var^ pick = f^ n:number^ -> number^ {\n"
             "    return^ if^ n < 2: 7 el^: 8 ;\n"
             "}\n"
             "return^ pick(1) * 10 + pick(9)\n");
    CHECK_INTEGER(&r, 78);
    run_dispose(&r);

    LHAT_TEST("a yield^ sends one the same way");
    run_text(&r,
             "var^ gen = p^ n:number^ { yield^ if^ n < 2: 7 el^: 8 ; }\n"
             "return^ gen(9).start()\n");
    CHECK_INTEGER(&r, 8);
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
    run_text(&r, "var^ x = 0\nif^ 1 { x := 1 }\nreturn^ x\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
    run_dispose(&r);

    LHAT_TEST("a unit with no return^ yields nil^");
    run_text(&r, "var^ x = 1\n");
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
    run_text(&r, "var^ twice = f^n { return^ n * 2 }\nreturn^ twice(21)\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    LHAT_TEST("arguments arrive in order");
    run_text(&r,
             "var^ less = f^a, b { return^ a - b }\n"
             "return^ less(10, 3)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 02 の 15.12: a function whose body is one expression answers with it,
    // with no return^ written. The parser turns it into one, so nothing here
    // or in the checker had to change.
    LHAT_TEST("a function whose body is one expression answers with it");
    run_text(&r,
             "var^ square = f^ n:number^ -> number^ { n * n }\n"
             "return^ square(7)\n");
    CHECK_INTEGER(&r, 49);
    run_dispose(&r);

    LHAT_TEST("and the expression may be one of 5.1");
    run_text(&r,
             "var^ sign = f^ n:number^ -> number^ { if^ n < 0: 0 el^: 1 ; }\n"
             "return^ sign(0 - 3) * 10 + sign(3)\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // A call too. Whether it answers anything is the checker's question, and
    // 13.2 already refused a f^ whose body was one call -- so this only makes
    // a refused body work.
    LHAT_TEST("and a call that is the whole body is the answer");
    run_text(&r,
             "var^ inner = f^ -> number^ { return^ 42 }\n"
             "var^ wrap = f^ -> number^ { inner() }\n"
             "return^ wrap()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.2 again: beside another statement it is a call standing alone, which
    // always meant "run this and drop what it answers".
    LHAT_TEST("but beside another statement it drops what it answers");
    run_text(&r,
             "var^ log = { n := 0 }\n"
             "var^ bump = f^ -> number^ { log.n := log.n + 1  return^ 9 }\n"
             "var^ wrap = f^ -> number^ { bump()  return^ log.n }\n"
             "return^ wrap()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a call is an expression like any other");
    run_text(&r,
             "var^ one = f^ { return^ 1 }\n"
             "return^ one() + one() * 3\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    // 02 の 8.7: a name is visible across its whole scope, so a body reaches
    // itself without anything declared ahead of it.
    LHAT_TEST("a subroutine reaches itself through this^");
    run_text(&r,
             "var^ fact = f^n {\n"
             "  if^ n <= 1 { return^ 1 }\n"
             "  return^ n * this^(n - 1)\n"
             "}\n"
             "return^ fact(5)\n");
    CHECK_INTEGER(&r, 120);
    run_dispose(&r);

    // 02 の 8.7改: the right side of a let^ reads the old world -- a shadow
    // starts from what it shadows.
    LHAT_TEST("a shadow's initialiser reads the shadowed value");
    run_text(&r,
             "var^ x = 10\n"
             "var^ got = 0\n"
             "do^{\n"
             "  let^ x = x + 32\n"
             "  got := x\n"
             "}\n"
             "return^ got * 100 + x\n");
    CHECK_INTEGER(&r, 4210);
    run_dispose(&r);

    // 8.7改: and so does a body written there -- the capture is of what the
    // name meant outside, never of the binding being made.
    LHAT_TEST("a body in the initialiser captures the shadowed binding");
    run_text(&r,
             "var^ f = 21\n"
             "var^ got = 0\n"
             "do^{\n"
             "  let^ f = f^ { return^ f * 2 }\n"
             "  got := f()\n"
             "}\n"
             "return^ got\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 8.7改: every name the statement binds reads the old world, the whole
    // right side over -- one value taken apart included.
    LHAT_TEST("several shadowing names all read the old world");
    run_checked_text(&r,
                     "var^ pair = f^ a:number^, b:number^ ->"
                     " (number^, number^) { return^ a, b }\n"
                     "var^ a = 1\n"
                     "var^ b = 2\n"
                     "var^ got = 0\n"
                     "do^{\n"
                     "  let^ a, b = pair(b, a)\n"
                     "  got := a * 10 + b\n"
                     "}\n"
                     "return^ got\n");
    CHECK_INTEGER(&r, 21);
    run_dispose(&r);

    // 02 の 15.10: a body with no name still has one way to reach itself.
    LHAT_TEST("this^ reaches the subroutine running");
    run_text(&r,
             "var^ fact = f^n {\n"
             "  if^ n <= 1 { return^ 1 }\n"
             "  return^ n * this^(n - 1)\n"
             "}\n"
             "return^ fact(5)\n");
    CHECK_INTEGER(&r, 120);
    run_dispose(&r);

    LHAT_TEST("and it works where there is no name to use");
    run_text(&r,
             "var^ apply = f^ g, n { return^ g(n) }\n"
             "return^ apply(f^n { if^ n <= 1 { return^ 1 } "
             "return^ n + this^(n - 1) }, 4)\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    // The innermost one, so an inner body does not reach the outer one.
    LHAT_TEST("this^ is the innermost subroutine");
    run_text(&r,
             "var^ outer = f^ {\n"
             "  var^ inner = f^n { if^ n <= 0 { return^ 0 } "
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
    run_text(&r, "var^ nothing = p^ { }\nreturn^ nothing()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_nil(r.ran.value), "nil^");
    run_dispose(&r);

    LHAT_TEST("calling something that is not a subroutine is a fault");
    run_text(&r, "var^ x = 1\nreturn^ x()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NOT_CALLABLE);
    run_dispose(&r);

    LHAT_TEST("the wrong number of arguments is a fault");
    run_text(&r, "var^ f = f^a, b { return^ a }\nreturn^ f(1)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // Nothing bounds the recursion, so the frames run out. 5.3 wants that
    // reported rather than reached by walking off the array. The call is
    // written inside an expression on purpose: 5.3's tail call would take the
    // frame over instead, and then nothing would run out at all.
    LHAT_TEST("frames that go too deep are reported, not walked off");
    run_text(&r, "var^ f = f^ { return^ this^() + 1 }\nreturn^ f()\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_STACK_OVERFLOW);
    run_dispose(&r);

    // 5.2 fixes a frame's width and gc.c walks all of it, so a slot the frame
    // below left behind is read by the collector -- pointing at something
    // that went away with that frame. Pushing empties the scratch to stop it,
    // and this is the shape that found it: frames pushed and popped often
    // enough that a collection lands between the two.
    //
    // It passes without the emptying too, since the freed value is only read
    // by the walk. What tells the two apart is an asan build, where this
    // reports heap-use-after-free -- run `ctest --preset asan`.
    LHAT_TEST("a frame's scratch does not hold what the last one left");
    run_text(&r,
             "var^ sum = f^ ...:t^{ number^, number^ } -> number^ {\n"
             "  var^ total = 0\n"
             "  for^ t in^ ... { total := total + t[1] + t[2] }\n"
             "  return^ total }\n"
             "var^ total = 0\n"
             "repeat^ 2000 { total := total + sum({ 10, 20 }) }\n"
             "return^ total\n");
    CHECK_INTEGER(&r, 60000);
    LHAT_CHECK(r.ran.collected > 1000, "the loop did collect");
    run_dispose(&r);
}

// 03 の 5.3: a call standing where its frame has nothing left to do runs in
// that frame. Every case here goes deeper than LHAT_MAX_FRAMES, so what is
// pinned is that the depth does not grow -- and the guards, where it must.
static void test_tail_calls(void)
{
    Run r;

    LHAT_TEST("a tail call does not deepen the frames");
    run_text(&r,
             "var^ down = f^ n:number^ -> number^ {\n"
             "    if^ n <= 0 { return^ 0 }\n"
             "    return^ this^(n - 1)\n"
             "}\n"
             "return^ down(50000)\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // Which name is called is not the question -- the frame is free either
    // way, and a ring of two proves the frame is reused rather than a body
    // recognising itself.
    LHAT_TEST("and a ring of two is the same");
    run_text(&r,
             "var^ isEven = f^ n:number^ -> bool^ {\n"
             "    if^ n = 0 { return^ true^ }\n"
             "    return^ isOdd(n - 1)\n"
             "}\n"
             "var^ isOdd = f^ n:number^ -> bool^ {\n"
             "    if^ n = 0 { return^ false^ }\n"
             "    return^ isEven(n - 1)\n"
             "}\n"
             "if^ isEven(50000) and^ !isEven(50001) { return^ 1 }\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 14.4: the receiver is laid out below the arguments, and the window that
    // moves down carries it.
    LHAT_TEST("a method's tail call is one too");
    run_text(&r,
             "var^ C = def^{\n"
             "    self^{ n := 0 },\n"
             "    walk := p^self^, k:number^ -> number^ {\n"
             "        if^ k <= 0 { return^ self^.n }\n"
             "        self^.n := self^.n + 1\n"
             "        return^ self^.walk(k - 1)\n"
             "    },\n"
             "}\n"
             "return^ C.new().walk(50000)\n");
    CHECK_INTEGER(&r, 50000);
    run_dispose(&r);

    // 5.3: a bare call standing last in a body is in tail position as well.
    // What it answers is thrown away, and the body answers the nil^ of
    // falling off its end -- which is what the drop is for.
    LHAT_TEST("a bare call standing last is a tail call, answering nil^");
    run_text(&r,
             "var^ box = { n := 0 }\n"
             "var^ step = p^ k:number^ {\n"
             "    if^ k <= 0 { return^ }\n"
             "    box.n := box.n + 1\n"
             "    this^(k - 1)\n"
             "}\n"
             "var^ answered = step(50000)\n"
             "if^ answered? { return^ 0 }\n"
             "return^ box.n\n");
    CHECK_INTEGER(&r, 50000);
    run_dispose(&r);

    // 5.5: a cleanup runs after the call, so the frame is not free to go.
    // 12.2's dispose has to run, and run once the call is done.
    LHAT_TEST("a call under a with^ is not a tail call");
    run_text(&r,
             "var^ H = def^{ self^{ v := 0 },\n"
             "  dispose := p^self^ { self^.v := 9 } }\n"
             "var^ seen = 0\n"
             "var^ take = f^ h -> number^ { return^ h.v }\n"
             "var^ use = f^ -> number^ {\n"
             "    with^ h = H.new() { return^ take(h) }\n"
             "}\n"
             "return^ use()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    // The same shape deep enough to tell: without the frame, the cleanup
    // would have nowhere to run, so 5.5 keeps the frames and they run out.
    LHAT_TEST("and it still runs out of frames");
    run_text(&r,
             "var^ H = def^{ self^{ v := 0 }, dispose := p^self^ { } }\n"
             "var^ down = f^ n:number^ -> number^ {\n"
             "    if^ n <= 0 { return^ 0 }\n"
             "    with^ h = H.new() { return^ this^(n - 1) }\n"
             "}\n"
             "return^ down(50000)\n");
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_STACK_OVERFLOW);
    run_dispose(&r);

    // 5.11: a coroutine's frame is that coroutine's body. Taking it over
    // would leave the resume with nothing to come back to.
    LHAT_TEST("a coroutine's body keeps its frame");
    run_text(&r,
             "var^ inner = f^ -> number^ { return^ 7 }\n"
             "var^ gen = p^ {\n"
             "    yield^ 1\n"
             "    return^ inner()\n"
             "}\n"
             "var^ co = gen()\n"
             "var^ first = co.start()\n"
             "var^ last = co.resume()\n"
             "return^ first * 10 + last\n");
    CHECK_INTEGER(&r, 17);
    run_dispose(&r);

    // What a host wrote is not an L^ closure, so there is no frame to take
    // over -- the call runs as the plain call it is and the RETURN after it
    // answers.
    LHAT_TEST("a tail call of something else runs as an ordinary call");
    run_text(&r, "var^ f = f^ s:string^ -> number^|nil^ {\n"
                 "    return^ s.tonumber()\n"
                 "}\n"
                 "return^ f(\"41\") ?? 0\n");
    CHECK_INTEGER(&r, 41);
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
             "var^ base = 10\n"
             "var^ add = f^n { return^ base + n }\n"
             "return^ add(5)\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("a ':=' inside a body reaches the outer binding");
    run_text(&r,
             "var^ count = 0\n"
             "var^ bump = p^ { count := count + 1 }\n"
             "bump()\n"
             "bump()\n"
             "return^ count\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("two bodies capturing one name share it");
    run_text(&r,
             "var^ n = 1\n"
             "var^ set = p^ { n := 9 }\n"
             "var^ get = f^ { return^ n }\n"
             "set()\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    // The place outlives the frame it started in, which is what closing an
    // upvalue is for.
    LHAT_TEST("a captured place outlives the frame that held it");
    run_text(&r,
             "var^ counter = f^ {\n"
             "  var^ n = 0\n"
             "  return^ p^ { n := n + 1 return^ n }\n"
             "}\n"
             "var^ next = counter()\n"
             "next()\n"
             "next()\n"
             "return^ next()\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("two closures from one body get separate places");
    run_text(&r,
             "var^ counter = f^ {\n"
             "  var^ n = 0\n"
             "  return^ p^ { n := n + 1 return^ n }\n"
             "}\n"
             "var^ a = counter()\n"
             "var^ b = counter()\n"
             "a()\n"
             "a()\n"
             "return^ b()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 5.4's second case: the name is not in the immediate parent either, so
    // each level on the way down has to carry it.
    LHAT_TEST("a name is carried down through more than one level");
    run_text(&r,
             "var^ outer = f^ {\n"
             "  var^ n = 7\n"
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
             "var^ get = f^ { return^ 0 }\n"
             "do^{\n"
             "  var^ n = 5\n"
             "  get := f^ { return^ n }\n"
             "}\n"
             "do^{\n"
             "  var^ other = 99\n"
             "  other := other\n"
             "}\n"
             "return^ get()\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("a parameter is captured like anything else");
    run_text(&r,
             "var^ adder = f^by { return^ f^n { return^ n + by } }\n"
             "var^ add3 = adder(3)\n"
             "return^ add3(4)\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);
}

// 01 の 2.3: the stacked reach. it^^ is the enclosing loop's focus,
// self^^/def^^ the enclosing def^'s, resolved past the inner binding of
// the same name; this^^ is the enclosing subroutine, captured as its maker's
// own closure since no register ever holds one.
static void test_stacked_hats_compile(void)
{
    Run r;

    LHAT_TEST("it^^ reads the enclosing loop's focus");
    run_text(&r,
             "var^ total = 0\n"
             "for^ 1 to^ 2 {\n"
             "  for^ 10 to^ 11 { total := total + it^^ * 100 + it^ }\n"
             "}\n"
             "return^ total\n");
    // (1,10) (1,11) (2,10) (2,11): 110+111+210+211
    CHECK_INTEGER(&r, 642);
    run_dispose(&r);

    // The count is over bindings of the name, not scopes -- and the chain
    // may cross a body boundary, where the reach becomes a capture.
    LHAT_TEST("and reaches across a body in between");
    run_text(&r,
             "var^ answer = 0\n"
             "for^ 7 to^ 7 {\n"
             "  var^ f = f^ -> number^ {\n"
             "    for^ 1 to^ 1 { return^ it^^ }\n"
             "    return^ 0\n"
             "  }\n"
             "  answer := f()\n"
             "}\n"
             "return^ answer\n");
    CHECK_INTEGER(&r, 7);
    run_dispose(&r);

    // 04 の 4.2: catch^'s it^ is a binding like the focus, so the two stack.
    LHAT_TEST("catch^'s it^ shadows a loop's, and it^^ still reaches it");
    run_text(&r,
             "errordef^ E { Bad }\n"
             "var^ fail = f^ { return^ error^E.Bad{ } }\n"
             "var^ n = 0\n"
             "for^ 5 to^ 5 { n := fail() catch^ it^^ }\n"
             "return^ n\n");
    CHECK_INTEGER(&r, 5);
    run_dispose(&r);

    LHAT_TEST("this^^ is the enclosing subroutine");
    run_text(&r,
             "var^ outer = p^ {\n"
             "  var^ inner = p^ { return^ this^^ }\n"
             "  return^ inner()\n"
             "}\n"
             "return^ outer() is^ outer\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    // 15.10: what the reach is for -- a body with no name of its own
    // calling the enclosing one, which is recursion written without naming
    // anybody. Lexical, not dynamic: the capture is made where the inner
    // body is written, by whichever instance of the outer one makes it.
    LHAT_TEST("this^^ recurses the enclosing subroutine namelessly");
    run_text(&r,
             "var^ outer = f^ n:number^ -> number^ {\n"
             "  var^ inner = f^ -> number^ {\n"
             "    if^ n <= 0 { return^ 0 }\n"
             "    return^ this^^(n - 1) + 1\n"
             "  }\n"
             "  return^ inner()\n"
             "}\n"
             "return^ outer(3)\n");
    CHECK_INTEGER(&r, 3);
    run_dispose(&r);

    LHAT_TEST("and this^^^ one further out");
    run_text(&r,
             "var^ a = p^ {\n"
             "  var^ b = p^ {\n"
             "    var^ d = p^ { return^ this^^^ }\n"
             "    return^ d()\n"
             "  }\n"
             "  return^ b()\n"
             "}\n"
             "return^ a() is^ a\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("self^^ is the enclosing def^'s receiver");
    run_text(&r,
             "var^ Outer = def^{ self^{ x := 7 },\n"
             "  m := f^self^ -> number^ {\n"
             "    var^ Inner = def^{ self^{ y := 1 },\n"
             "      n := f^self^ -> number^ { return^ self^^.x + self^.y }\n"
             "    }\n"
             "    return^ Inner.new().n()\n"
             "  }\n"
             "}\n"
             "return^ Outer.new().m()\n");
    CHECK_INTEGER(&r, 8);
    run_dispose(&r);

    LHAT_TEST("def^^ is the enclosing definition");
    run_text(&r,
             "var^ Outer = def^{ self^{ x := 1 }, tag := 42,\n"
             "  m := f^self^ -> number^ {\n"
             "    var^ Inner = def^{ self^{ y := 2 },\n"
             "      n := f^self^ -> number^ { return^ def^^.tag }\n"
             "    }\n"
             "    return^ Inner.new().n()\n"
             "  }\n"
             "}\n"
             "return^ Outer.new().m()\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // Fewer bindings than the hats count out: refused as a miscount, the
    // same answer '$^' gives past the outermost scope.
    LHAT_TEST("it^^ with one loop is a miscount");
    run_text(&r, "for^ 1 to^ 2 { var^ x = it^^ }\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("this^^ in an unnested body is a miscount");
    run_text(&r, "var^ f = p^ { return^ this^^ }\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);

    LHAT_TEST("self^^ with one receiver is a miscount");
    run_text(&r,
             "var^ P = def^{ self^{ x := 1 },\n"
             "  m := f^self^ -> number^ { return^ self^^.x } }\n"
             "return^ 0\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
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
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  return^ $^x\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("'$^^' one further, and '$^' still the nearer one");
    run_text(&r,
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  do^{ var^ x = 3\n"
             "    return^ $^^x * 10 + $^x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 12);
    run_dispose(&r);

    // 01 の 8: this is that table -- the same three scopes named from
    // either end, and the two numberings meeting in the middle.
    LHAT_TEST("'$' reads the unit's own top level");
    run_text(&r,
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  do^{ var^ x = 3\n"
             "    return^ $x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("and repeating the sigil counts inwards from it");
    run_text(&r,
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  do^{ var^ x = 3\n"
             "    return^ $$x * 10 + $$$x\n"
             "  }\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 23);
    run_dispose(&r);

    LHAT_TEST("naming a scope further in than this one is refused");
    run_text(&r, "var^ x = 1\nreturn^ $$x\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);

    // A subroutine body is one scope like any other, so it takes its place
    // in the absolute numbering too.
    LHAT_TEST("a body counts in the absolute numbering as well");
    run_text(&r,
             "var^ x = 1\n"
             "var^ f = f^ -> number^ { var^ x = 2\n"
             "  return^ $$x\n"
             "}\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a write through the absolute form lands there too");
    run_text(&r,
             "var^ x = 1\n"
             "var^ seen = 0\n"
             "do^{ var^ x = 2\n"
             "  do^{ var^ x = 3\n"
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
             "var^ x = 1\n"
             "var^ f = f^ -> number^ { var^ x = 2\n"
             "  return^ $^x\n"
             "}\n"
             "return^ f()\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    LHAT_TEST("a loop body is one scope like any other");
    run_text(&r,
             "var^ x = 1\n"
             "var^ seen = 0\n"
             "repeat^ 1 { var^ x = 2\n"
             "  seen := $^x\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);

    // 8.6 with 01 の 8 章: writing through a specifier reaches the binding a
    // read of the same words would, and leaves the nearer one alone.
    LHAT_TEST("':=' through '$^' writes the outer binding");
    run_text(&r,
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  $^x := 9\n"
             "}\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("and leaves the one it was written beside");
    run_text(&r,
             "var^ x = 1\n"
             "var^ seen = 0\n"
             "do^{ var^ x = 2\n"
             "  $^x := 9\n"
             "  seen := x\n"
             "}\n"
             "return^ seen\n");
    CHECK_INTEGER(&r, 2);
    run_dispose(&r);

    LHAT_TEST("a write out of a subroutine reaches the captured place");
    run_text(&r,
             "var^ x = 1\n"
             "var^ f = p^ { var^ x = 2\n"
             "  $^x := 9\n"
             "}\n"
             "f()\n"
             "return^ x\n");
    CHECK_INTEGER(&r, 9);
    run_dispose(&r);

    LHAT_TEST("reading back through the same words sees the write");
    run_text(&r,
             "var^ x = 1\n"
             "do^{ var^ x = 2\n"
             "  $^x := 42\n"
             "  return^ $^x\n"
             "}\n"
             "return^ 0\n");
    CHECK_INTEGER(&r, 42);
    run_dispose(&r);

    // 14.4 binds def^ into the scope a def^'s '{' opens, so a specifier
    // written in a method counts that scope -- the same one the checker
    // pushes there. 01 の 2.3: def^ is its own name now, so the outer
    // `class` never collides with it and the specifier spells the hat.
    LHAT_TEST("a def^ body is one scope on this side too");
    run_text(&r,
             "var^ class = 1\n"
             "var^ D = def^{ self^{ v := 7 },\n"
             "  m := f^self^ -> string^ { return^ typeof^($^def^).signature }\n"
             "}\n"
             "return^ D.new().m()\n");
    // 14.16: compiled without checking, typeof^ answers the tag -- so the
    // definition reads "t^" where the outer `class` would read "number^",
    // which is still the whole of what the specifier has to prove here.
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
    LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_STRING),
               "expected a string");
    if (lhat_is_object_kind(r.ran.value, LHAT_OBJECT_STRING)) {
        const LhatString *s = (const LhatString *)lhat_as_object(r.ran.value);
        // The definition, not the number^ named `class` outside it.
        LHAT_CHECK(strncmp(s->text, "t^", 2) == 0,
                   "'$^class' reached the definition");
    }
    run_dispose(&r);

    LHAT_TEST("counting past the scopes that are open is refused");
    run_text(&r, "do^{ return^ $^^^^x }\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_SCOPE_TOO_FAR);
    run_dispose(&r);
}

// 02 の 13.7: the variadic collector, compiled and run.
static void test_variadic(void)
{
    Run r;

    LHAT_TEST("a variadic sum over what was actually passed");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ {\n"
             "  var^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "return^ sum(1, 2, 3, 4)\n");
    CHECK_INTEGER(&r, 10);
    run_dispose(&r);

    LHAT_TEST("zero variadic arguments collects an empty table");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ {\n"
             "  var^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "return^ sum()\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("fixed parameters lead the variadic tail");
    run_text(&r,
             "var^ f = f^ label:string^, ...:number^ -> number^ {\n"
             "  var^ total = 0\n"
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
             "var^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
             "  return^ a + b\n"
             "}\n"
             "return^ f(1)\n");
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
    run_dispose(&r);

    // 13.7: 'expr...' forwards the collected tail into another call.
    LHAT_TEST("'...' forwards into another variadic call");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ {\n"
             "  var^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "var^ logged = f^ ...:number^ -> number^ { return^ sum(...) }\n"
             "return^ logged(1, 2, 3, 4, 5)\n");
    CHECK_INTEGER(&r, 15);
    run_dispose(&r);

    LHAT_TEST("a fixed argument may lead the forwarded spread");
    run_text(&r,
             "var^ sum3 = f^ base:number^, ...:number^ -> number^ {\n"
             "  var^ total = base\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "var^ wrap = f^ ...:number^ -> number^ {\n"
             "  return^ sum3(100, ...)\n"
             "}\n"
             "return^ wrap(1, 2, 3)\n");
    CHECK_INTEGER(&r, 106);
    run_dispose(&r);

    // 13.7: what leads the spread owes the fixed arguments and no more, so
    // the values beyond them join the tail the spread continues. Here the
    // callee has no fixed arguments at all -- print's shape.
    LHAT_TEST("values written into the tail may lead the spread");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ {\n"
             "  var^ total = 0\n"
             "  for^ i, x in^ ... { total := total + x }\n"
             "  return^ total\n"
             "}\n"
             "var^ wrap = f^ ...:number^ -> number^ {\n"
             "  return^ sum(10, 20, ...)\n"
             "}\n"
             "return^ wrap(1, 2, 3)\n");
    CHECK_INTEGER(&r, 36);
    run_dispose(&r);

    // 02 の 14.16: typeof^ reconstructs the signature, including the tail.
    LHAT_TEST("typeof^ reflects a variadic signature");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ { return^ 0 }\n"
             "return^ typeof^(sum).signature\n");
    CHECK_STRING(&r, "f^...:number^ -> number^;");
    run_dispose(&r);

    LHAT_TEST("fixed and variadic together in the signature");
    run_text(&r,
             "var^ f = f^ label:string^, ...:number^ -> number^ {\n"
             "  return^ 0\n"
             "}\n"
             "return^ typeof^(f).signature\n");
    CHECK_STRING(&r, "f^string^, ...:number^ -> number^;");
    run_dispose(&r);

    // 14.10's round trip: the printed signature has to parse back.
    LHAT_TEST("the variadic signature parses back as an annotation");
    run_text(&r,
             "var^ sum = f^ ...:number^ -> number^ { return^ 0 }\n"
             "var^ typed : f^...:number^ -> number^; = sum\n"
             "return^ typed(1, 2, 3)\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

int main(void)
{
    test_encoding();
    test_arithmetic();
    test_casts();
    test_names();
    test_control();
    test_calls();
    test_tail_calls();
    test_closures();
    test_stacked_hats_compile();
    test_scope_specifiers();
    test_variadic();
    return lhat_test_report("test_vm_core");
}
