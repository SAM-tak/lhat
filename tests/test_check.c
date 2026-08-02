// L^ (lhat) -- tests for the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". The cases pinned here are the ones a decision in the
// specification produces and that would otherwise be invisible: the scope
// rule of 8.7, the result inference of 03 の 3.4, and the way catch^, ?? and
// try^ each drop one arm of a union.

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
} Unit;

static void check_text(Unit *u, const char *text)
{
    lhat_source_init_from_string(&u->source, "<test>", text, strlen(text));
    lhat_lexer_init(&u->lexer, &u->source);
    lhat_parse(&u->lexer, &u->parsed);
    lhat_check(u->parsed.root, &u->lexer, true, &u->checked);
}

static void unit_dispose(Unit *u)
{
    lhat_check_result_dispose(&u->checked);
    lhat_parse_result_dispose(&u->parsed);
    lhat_lexer_dispose(&u->lexer);
    lhat_source_dispose(&u->source);
}

static size_t syntax_errors(const Unit *u)
{
    return u->parsed.diagnostic_count + u->lexer.diagnostic_count;
}

static bool has_error(const Unit *u, LhatCheckErrorCode code)
{
    for (size_t i = 0; i < u->checked.diagnostic_count; i++) {
        if (u->checked.diagnostics[i].code == code) {
            return true;
        }
    }
    return false;
}

// Every case here should parse; a syntax error would make the check
// meaningless, so it is asserted separately.
#define CHECK_CLEAN(u)                                                        \
    do {                                                                      \
        LHAT_CHECK_EQ_INT(syntax_errors(u), 0);                               \
        LHAT_CHECK_EQ_INT((u)->checked.diagnostic_count, 0);                  \
    } while (0)

#define CHECK_REPORTS(u, code)                                                \
    do {                                                                      \
        LHAT_CHECK_EQ_INT(syntax_errors(u), 0);                               \
        LHAT_CHECK(has_error(u, code), "expected " #code);                    \
    } while (0)

static void test_names(void)
{
    Unit u;

    LHAT_TEST("a definition binds and a reassignment finds it");
    check_text(&u, "let^ x = 1\nx := 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an unknown name is reported");
    check_text(&u, "y := 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 8.6: the accident let^ was introduced to remove. The inner statement
    // reassigns the outer name rather than making a second one.
    LHAT_TEST("':=' in a nested scope reaches the outer name");
    check_text(&u, "let^ i = 0\ndo^{ i := 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("shadowing gets its own binding");
    check_text(&u, "let^ i = 0\ndo^{ let^ i = \"text\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.7: twice in one scope is an error; the nested case above is not.
    LHAT_TEST("the same scope may not define a name twice");
    check_text(&u, "let^ x = 1\nlet^ x = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    // 8.7: visible throughout the scope, but not readable before its let^
    // has run.
    LHAT_TEST("reading before the let^ has run is an error");
    check_text(&u, "let^ x = y\nlet^ y = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_USED_BEFORE_DEFINED);
    unit_dispose(&u);

    // The same visibility is what lets two subroutines call each other with
    // no forward declaration, since a body does not run where it is written.
    LHAT_TEST("mutual recursion needs no forward declaration");
    check_text(&u,
               "let^ isEven = f^ n:number^ -> bool^ { return^ isOdd(n) }\n"
               "let^ isOdd = f^ n:number^ -> bool^ { return^ isEven(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a parameter is in scope in the body");
    check_text(&u, "let^ f = f^ n:number^ -> number^ { return^ n + 1 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

static void test_expressions(void)
{
    Unit u;

    LHAT_TEST("arithmetic needs numbers");
    check_text(&u, "let^ s = \"a\"\nlet^ n = s + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);

    LHAT_TEST("a comparison is a bool");
    check_text(&u, "let^ b : bool^ = 1 < 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and^ takes bools");
    check_text(&u, "let^ b = 1 and^ 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);

    LHAT_TEST("an annotation has to be satisfied");
    check_text(&u, "let^ x : number^ = \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a reassignment keeps the type of the name");
    check_text(&u, "let^ x = 1\nx := \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a call checks its arguments and arity");
    check_text(&u,
               "let^ f = f^ n:number^ -> number^ { return^ n }\n"
               "let^ a = f(1)\n"
               "let^ b = f(\"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("too few arguments is reported");
    check_text(&u,
               "let^ f = f^ n:number^ -> number^ { return^ n }\n"
               "let^ a = f()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("calling something that is not a subroutine");
    check_text(&u, "let^ x = 1\nlet^ y = x(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_CALLABLE);
    unit_dispose(&u);

    // 14.10: a structure asks for at least its members, so reading one it
    // does not have is where the report belongs.
    LHAT_TEST("a member has to exist");
    check_text(&u, "let^ t = { a := 1 }\nlet^ x = t.a\nlet^ y = t.b\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("a table literal keeps its members' types");
    check_text(&u, "let^ t = { a := 1 }\nlet^ n : number^ = t.a\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: any^ admits every value, so passing one on is fine.
    LHAT_TEST("any^ accepts anything");
    check_text(&u,
               "let^ log = p^ x:any^ { }\n"
               "log(1)\n"
               "log(\"text\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 03 の 3.4.
static void test_results(void)
{
    Unit u;

    LHAT_TEST("the result type is inferred from return^");
    check_text(&u,
               "let^ f = f^ { return^ 0 }\n"
               "let^ n : number^ = f()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("several return^ make a union");
    check_text(&u,
               "let^ f = f^ b:bool^ {\n"
               "    if^ b { return^ 0 }\n"
               "    return^ nil^\n"
               "}\n"
               "let^ n : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a declared result is checked against return^");
    check_text(&u, "let^ f = f^ -> number^ { return^ \"text\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: the exits that do not go through the subroutine itself settle the
    // result between them, so recursion needs nothing written.
    LHAT_TEST("recursion is inferred from the exits that are not recursive");
    check_text(&u,
               "let^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * fact(n - 1)\n"
               "}\n"
               "let^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the recursive exit does not widen it");
    check_text(&u,
               "let^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ fact(n - 1)\n"
               "}\n"
               "let^ s : string^ = fact(5)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 3.4: a recursive exit is dropped only when its type is the one being
    // worked out. Here the call sits inside something whose answer does not
    // depend on it, so the arm it contributes has to be kept.
    LHAT_TEST("a recursive call inside a larger expression still counts");
    check_text(&u,
               "let^ tag = f^ v:any^ -> string^ { return^ \"t\" }\n"
               "let^ f = f^ x:number^ {\n"
               "    if^ x > 1 { return^ tag(f(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "let^ v : number^|string^ = f(3)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the union really is both arms");
    check_text(&u,
               "let^ tag = f^ v:any^ -> string^ { return^ \"t\" }\n"
               "let^ f = f^ x:number^ {\n"
               "    if^ x > 1 { return^ tag(f(x - 1)) }\n"
               "    return^ 1\n"
               "}\n"
               "let^ v : number^ = f(3)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 12.8 and 03 の 5.6 leave no other way out, so a body every exit of
    // which calls itself can never produce a value.
    LHAT_TEST("a body whose every exit is recursive is reported");
    check_text(&u, "let^ f = f^ n:number^ { return^ f(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEVER_RETURNS);
    unit_dispose(&u);

    LHAT_TEST("falling out of the body is an exit, so this one is not that");
    check_text(&u,
               "let^ f = p^ n:number^ {\n"
               "    if^ n > 0 { return^ f(n - 1) }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2 keeps "returns nothing" apart from "returns nil^". A body that
    // returns a value on one path and falls out on another does produce a
    // value, so the missing one is nil^ (04 の 11.3).
    LHAT_TEST("falling out of the body puts nil^ in the result");
    check_text(&u,
               "let^ f = p^ b:bool^ { if^ b { return^ 1 } }\n"
               "let^ n : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and the union is what the caller has to handle");
    check_text(&u,
               "let^ f = p^ b:bool^ { if^ b { return^ 1 } }\n"
               "let^ n : number^ = f(true^) ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.2: a function answers on every path, so an f^ that can reach its
    // end has one with nothing to answer with. No result type fixes that.
    LHAT_TEST("a function that can reach its end is reported");
    check_text(&u, "let^ f = f^ b:bool^ { if^ b { return^ true^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and writing the result does not excuse it");
    check_text(&u,
               "let^ f = f^ b:bool^ -> bool^|nil^ { if^ b { return^ true^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("covering every path is what it takes");
    check_text(&u,
               "let^ f = f^ b:bool^ {\n"
               "    if^ b { return^ true^ }\n"
               "    return^ false^\n"
               "}\n"
               "let^ x : bool^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an empty function body is reported too");
    check_text(&u, "let^ f = f^ { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // The same exit under a p^ that wrote a result which does not admit it.
    LHAT_TEST("a written result has to admit the value-less exit");
    check_text(&u,
               "let^ f = p^ b:bool^ -> number^ { if^ b { return^ 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    unit_dispose(&u);

    LHAT_TEST("and admitting it is enough");
    check_text(&u,
               "let^ f = p^ b:bool^ -> number^|nil^ { if^ b { return^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4 counts every exit, and a bare return^ is one that produces
    // no value -- the same exit reaching the end of the body is, and it
    // leaves the same nil^ behind at run time.
    LHAT_TEST("a bare return^ is an exit that produces no value");
    check_text(&u,
               "let^ f = p^ b:bool^ { if^ b { return^ 1 } return^ }\n"
               "let^ v : number^ = f(true^)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("so nil^ joins the result beside the value the other exit makes");
    check_text(&u,
               "let^ f = p^ b:bool^ { if^ b { return^ 1 } return^ }\n"
               "let^ v : number^|nil^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an f^ may not take that exit either");
    check_text(&u, "let^ f = f^ -> number^ { return^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and a written result has to admit it");
    check_text(&u, "let^ f = p^ -> number^ { return^ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT);
    unit_dispose(&u);

    // 16.5: a repeat^ with no bound and no break^ of its own never ends, so
    // the end of the body is not somewhere control can arrive.
    LHAT_TEST("an endless repeat^ is not a way to the end of a body");
    check_text(&u, "let^ f = f^ -> number^ { repeat^ { } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("nor when every turn of it returns");
    check_text(&u, "let^ f = f^ -> number^ { repeat^ { return^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a p^ with a written result is not made to admit nil^");
    check_text(&u, "let^ g = p^ -> number^ { repeat^ { yield^ 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 9.8: break^ leaves the loop, so the end is reachable again.
    LHAT_TEST("a break^ puts the end of the body back within reach");
    check_text(&u, "let^ f = f^ -> number^ { repeat^ { break^ } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("wherever the break^ is written inside the loop");
    check_text(&u,
               "let^ f = f^ -> number^ { repeat^ { if^ true^ { break^ } } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // A break^ of an inner loop leaves that one, not this one.
    LHAT_TEST("but a nested loop keeps its own break^");
    check_text(&u,
               "let^ f = f^ -> number^ { repeat^ { repeat^ { break^ } } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: the if^ form of for^ does not iterate, so a break^ in it is the
    // outer loop's.
    LHAT_TEST("and the if^ form of for^ is not a loop to break out of");
    check_text(&u,
               "let^ f = f^ -> number^ {\n"
               "    repeat^ { for^ x := 1 if^ true^ { break^ } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 17 章: nor is the when^ form.
    LHAT_TEST("nor is the when^ form");
    check_text(&u,
               "let^ f = f^ -> number^ {\n"
               "    repeat^ { for^ 1 { when^ 1: break^ other^: } }\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // Only the endless form. The others can run out on their own.
    LHAT_TEST("a bounded repeat^ still reaches the end of the body");
    check_text(&u, "let^ f = f^ -> number^ { repeat^ 3 { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    LHAT_TEST("and so does a conditional one");
    check_text(&u, "let^ f = f^ -> number^ { repeat^ while^ true^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_FUNCTION_FALLS_OUT);
    unit_dispose(&u);

    // 13.11: a branch that ends in an endless loop is a branch that leaves.
    LHAT_TEST("a branch ending in one covers its path");
    check_text(&u,
               "let^ f = f^ -> number^ {\n"
               "    if^ true^ { repeat^ { } el^: return^ 1 }\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A body with no return^ at all asked for no value. 13.2 has a form for
    // it, and nil^ is not it.
    LHAT_TEST("a body that returns nothing stays returning nothing");
    check_text(&u,
               "let^ log = p^ n:number^ { let^ x = n }\n"
               "log(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("every path returning means no nil^ joins in");
    check_text(&u,
               "let^ f = p^ b:bool^ { if^ b { return^ 1 else^: return^ 2 } }\n"
               "let^ n : number^ = f(true^)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.10: this^ is a self-call like a call by name, so 3.4 counts it the
    // same way and a body with no name can recurse.
    LHAT_TEST("this^ recursion is inferred the same way");
    check_text(&u,
               "let^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "let^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a body whose every exit is this^ is reported");
    check_text(&u, "let^ f = f^ n:number^ { return^ this^(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NEVER_RETURNS);
    unit_dispose(&u);

    // 15.10: this^ has the signature of the body it is in, so the arguments
    // are checked -- which a call by name cannot do while the name is still
    // being bound.
    LHAT_TEST("this^ checks its arguments");
    check_text(&u,
               "let^ f = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ this^(\"text\")\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("this^ outside any body is reported");
    check_text(&u, "let^ x = this^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_THIS_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("with the result written, recursion is fine");
    check_text(&u, "let^ f = f^ n:number^ -> number^ { return^ f(n) }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 04.
static void test_errors(void)
{
    Unit u;

    LHAT_TEST("a kind and its set become types");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ e : IOError = error^IOError.NotFound{ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 8.1: the whole detection mechanism for an unhandled error is
    // ordinary conformance.
    LHAT_TEST("an unhandled error cannot be used as the value");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "let^ f = f^ -> number^|IOError { return^ 0 }\n"
               "let^ n : number^ = f()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("catch^ drops the error arm");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "let^ f = f^ -> number^|IOError { return^ 0 }\n"
               "let^ n : number^ = f() catch^ 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 4.1: a catch^ that can never fire is an error, since it says
    // something about a failure that cannot happen.
    LHAT_TEST("catch^ on what cannot fail is reported");
    check_text(&u, "let^ n = 1 catch^ 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_FAIL);
    unit_dispose(&u);

    LHAT_TEST("?? drops the nil arm");
    check_text(&u,
               "let^ f = f^ -> number^|nil^ { return^ 0 }\n"
               "let^ n : number^ = f() ?? 0\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("?? on what cannot be nil is reported");
    check_text(&u, "let^ n = 1 ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_BE_NIL);
    unit_dispose(&u);

    LHAT_TEST("try^ unwraps and the result is the success type");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ read = f^ -> number^|IOError { return^ try^ open() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 5.3: reported where try^ is written, not at the caller.
    LHAT_TEST("try^ may not let out an error the result excludes");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ read = f^ -> number^|IOError.NotFound {\n"
               "    return^ try^ open()\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TRY_OUTSIDE);
    unit_dispose(&u);

    // 04 の 2.4: identity is the declaration, so two identical ones differ.
    LHAT_TEST("kinds from different declarations do not mix");
    check_text(&u,
               "errordef^ IOError { NotFound }\n"
               "errordef^ UserError { NotFound }\n"
               "let^ e : IOError = error^UserError.NotFound{ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 2.2: a field may carry a default, written with ':=' as 14.6's
    // template already writes a named field with an initial value.
    LHAT_TEST("a default stands in for the type");
    check_text(&u,
               "errordef^ ParseError { Syntax { line := 0 } }\n"
               "let^ e = error^ParseError.Syntax{ }\n"
               "let^ n : number^ = e.line\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a field may carry both a type and a default");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ := 0 } }\n"
               "let^ e = error^ParseError.Syntax{ }\n"
               "let^ n : number^ = e.line\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a default has to fit the declared type");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ := \"text\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 2.5: without a default there is nothing to fall back to.
    LHAT_TEST("a field with no default has to be written");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ } }\n"
               "let^ e = error^ParseError.Syntax{ }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISSING_FIELD);
    unit_dispose(&u);

    LHAT_TEST("a default may still be overridden at the construction");
    check_text(&u,
               "errordef^ ParseError { Syntax { line := 0 } }\n"
               "let^ e = error^ParseError.Syntax{ line := 3 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 6.1: a declared field is reached through the kind.
    LHAT_TEST("a kind's declared field is visible");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ } }\n"
               "let^ e = error^ParseError.Syntax{ line := 3 }\n"
               "let^ n : number^ = e.line\n"
               "let^ m : string^ = e.message\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

static void test_annotations(void)
{
    Unit u;

    LHAT_TEST("an unknown type name is reported");
    check_text(&u, "let^ x : Nonesuch = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    unit_dispose(&u);

    // 14.10: at least the listed members.
    LHAT_TEST("a structural annotation asks for at least its members");
    check_text(&u,
               "let^ t : t^{ a : number^ } = { a := 1, b := 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a missing member fails the annotation");
    check_text(&u, "let^ t : t^{ a : number^ } = { b := 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10: bare, with nothing listed, it asks for nothing in particular --
    // the top of tables, which 13.7 notes is not the top of every value.
    LHAT_TEST("a bare table type takes any table");
    check_text(&u,
               "let^ x : table^ = { a := 1 }\n"
               "let^ y : t^ = { 1, 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing that is not one");
    check_text(&u, "let^ x : table^ = 5\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a bare one joins a union like any other type");
    check_text(&u,
               "let^ x : table^|nil^ = { a := 1 }\n"
               "let^ y : nil^|t^ = nil^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union annotation accepts either arm");
    check_text(&u,
               "let^ x : number^|string^ = 1\n"
               "let^ y : number^|string^ = \"text\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union annotation refuses a third type");
    check_text(&u, "let^ x : number^|string^ = true^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.11: is^ reads a type, so an unknown one is reported there too.
    LHAT_TEST("is^ resolves its right side as a type");
    check_text(&u, "let^ x = 1\nlet^ b : bool^ = x is^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: any^ holds of every value, so the question is empty whatever is
    // on the left. 13.11 decides this from the right side alone -- it never
    // reads the left's inferred type against the right.
    LHAT_TEST("asking is^ any^ asks nothing and is reported");
    check_text(&u, "let^ x : number^ = 1\nlet^ b = x is^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_IS_ALWAYS_TRUE);
    unit_dispose(&u);

    LHAT_TEST("whatever the left happens to be");
    check_text(&u, "let^ x : any^ = 1\nlet^ b = x is^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_IS_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.5 collapses a union with any^ to any^, so the written form does not
    // let it through.
    LHAT_TEST("and however the any^ is spelled");
    check_text(&u, "let^ x : number^ = 1\nlet^ b = x is^ any^|nil^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_IS_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.11: an answer the left's inferred type fixes is not refused. is^ is
    // there to be asked at run time, and the checker narrowing that from what
    // it thinks it knows is what 13.7 introduced any^ to avoid.
    LHAT_TEST("but an answer fixed by the left is left alone");
    check_text(&u,
               "let^ x : number^ = 1\n"
               "let^ a = x is^ string^\n"
               "let^ b = x is^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("which is what makes any^ usable at all");
    check_text(&u, "let^ f = p^ x:any^ { let^ b = x is^ string^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 13.11, and 04 の 7 章 which rests entirely on it.
static void test_narrowing(void)
{
    Unit u;

    LHAT_TEST("the true branch keeps only the arms that fit");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r is^ number^ { let^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the false branch keeps the rest");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r is^ number^ {\n"
               "    else^:\n"
               "        let^ s : string^ = r\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("narrowing does not reach past what was tested");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r is^ number^ { let^ s : string^ = r }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 6.1: narrowing to a kind is what makes its declared field visible.
    LHAT_TEST("a narrowed error kind shows its fields");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "let^ r = parse()\n"
               "if^ r is^ ParseError.Syntax { let^ n : number^ = r.line }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the field is not visible without narrowing");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "let^ r = parse()\n"
               "let^ n = r.line\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 04 の 6.1 is written in the early-return style, so a branch that never
    // falls through has to leave its narrowing behind.
    LHAT_TEST("an exiting branch narrows what follows it");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    let^ r = parse()\n"
               "    if^ r is^ ParseError.Syntax { return^ 0 }\n"
               "    if^ r is^ ParseError.Eof { return^ 0 }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a branch that falls through leaves nothing behind");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ g = f^ -> number^ {\n"
               "    let^ r = f()\n"
               "    if^ r is^ string^ { }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);

    // 04 の 7 章: handling every kind is ordinary narrowing, so the success
    // type is what is left in the last clause.
    LHAT_TEST("an exhausted union leaves the success type");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    let^ r = open()\n"
               "    if^ r is^ IOError.NotFound {\n"
               "        return^ 0\n"
               "        elseif^ r is^ IOError.Denied:\n"
               "            return^ 0\n"
               "        else^:\n"
               "            return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union not exhausted still carries its errors");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    let^ r = open()\n"
               "    if^ r is^ IOError.NotFound {\n"
               "        return^ 0\n"
               "        else^:\n"
               "            return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("'!' turns the branches around");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ !(r is^ number^) { let^ s : string^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: and^ tells us both held; or^ says nothing when it is true.
    LHAT_TEST("and^ narrows both sides");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ g = f^ -> number^|string^ { return^ 0 }\n"
               "let^ a = f()\n"
               "let^ b = g()\n"
               "if^ a is^ number^ and^ b is^ number^ {\n"
               "    let^ n : number^ = a + b\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: only a name or a dot path from one, since a call may give a
    // different value the second time.
    LHAT_TEST("a call is not narrowed");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ g = f^ -> number^ {\n"
               "    if^ f() is^ number^ { return^ f() + 1 }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);

    LHAT_TEST("a dot path is narrowed");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ t = { a := f() }\n"
               "if^ t.a is^ number^ { let^ n : number^ = t.a }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: reassigning ends it, since the claim was about what was examined.
    LHAT_TEST("a reassignment ends the narrowing");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r is^ number^ {\n"
               "    r := f()\n"
               "    let^ n : number^ = r\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("narrowing works in the if expression too");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "let^ n : number^ = if^ r is^ number^: r el^: 0 ;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 14 章 and 12.5.
static void test_definitions(void)
{
    Unit u;

    // 14.7: an instance reaches the definition's members, so its type holds
    // both those and the template's fields.
    LHAT_TEST("an instance has the fields and the members");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ value := 0, label := \"\" },\n"
               "    show := p^self^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "let^ n : number^ = c.value\n"
               "let^ s : string^ = c.label\n"
               "c.show()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11: a definition without one still offers a new^ taking nothing.
    LHAT_TEST("new^ exists without being written");
    check_text(&u, "let^ C = def^{ self^{ v := 1 } }\nlet^ c = C.new^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a field the definition does not declare is reported");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 } }\n"
               "let^ c = C.new^()\n"
               "let^ x = c.missing\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 14.4: self^ reaches the instance from inside a method.
    LHAT_TEST("self^ reaches the fields");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ {\n"
               "        self^.v := self^.v + step\n"
               "    },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("class^ reaches the definition's members");
    check_text(&u,
               "let^ print = p^ x:any^ { }\n"
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    origin := 7,\n"
               "    show := p^ { print(class^.origin) },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: the receiver is not written at the call, so it is not counted.
    LHAT_TEST("a method call does not pass the receiver");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "c.bump(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a method call still checks its arguments");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "c.bump(\"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a method call with too few arguments is reported");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "c.bump()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 12.5 with 12.7: with^ wants a dispose(), and one that returns nothing,
    // because 12.7 made cleanup unable to fail.
    LHAT_TEST("with^ accepts a value that has dispose()");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "with^ c := C.new^() { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("with^ refuses a value without one");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 } }\n"
               "with^ c := C.new^() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_DISPOSABLE);
    unit_dispose(&u);

    LHAT_TEST("with^ refuses a dispose() that returns something");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 1 },\n"
               "    dispose := f^self^ -> number^ { return^ 0 },\n"
               "}\n"
               "with^ c := C.new^() { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_DISPOSABLE);
    unit_dispose(&u);

    // 14.10: the structural form is what 12.5 is really asking for, so it
    // has to resolve on its own.
    LHAT_TEST("the structural form of a disposable resolves");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "let^ c = C.new^()\n"
               "let^ d : t^{ dispose : p^self^; } = c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 05 の 2.2: one environment for names, which is what 14.9 needs -- it
    // says a definition takes its name from its binding, so that name has to
    // be reachable where a type is written.
    LHAT_TEST("a definition's name works as a type");
    check_text(&u,
               "let^ Foo = def^{ self^{ v := 1 } }\n"
               "let^ x : Foo = Foo.new^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.7: writing the name asks for the whole structure, and that is what
    // an instance carries -- so as a type the name means an instance.
    LHAT_TEST("as a type the name means an instance");
    check_text(&u,
               "let^ Foo = def^{ self^{ v := 1 } }\n"
               "let^ x : Foo = Foo.new^()\n"
               "let^ n : number^ = x.v\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the definition itself is not an instance");
    check_text(&u,
               "let^ Foo = def^{ self^{ v := 1 } }\n"
               "let^ x : Foo = Foo\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 05 の 2.2: an errordef^ name lives in the same place, so a value of the
    // same name collides rather than shadowing quietly.
    LHAT_TEST("a type and a value of one name collide");
    check_text(&u,
               "errordef^ E { A }\n"
               "let^ E = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    // 14.9: the name is a label, so two definitions of the same shape are one
    // type and nothing has to be shared for that to hold.
    LHAT_TEST("identity stays structural");
    check_text(&u,
               "let^ A = def^{ self^{ v := 0 } }\n"
               "let^ B = def^{ self^{ v := 0 } }\n"
               "let^ take = p^ x:t^{ v : number^ } { }\n"
               "take(A.new^())\n"
               "take(B.new^())\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 14.5 and 14.12.
static void test_composition(void)
{
    Unit u;

    static const char *const base =
        "let^ Foo = def^{\n"
        "    self^{ a := 0 },\n"
        "    foo := p^self^, x:string^ { },\n"
        "    bar := p^self^ { },\n"
        "}\n";

    // 14.5: the derived definition carries both sides, and 14.7 means an
    // instance of it reaches the base's fields as well as its own.
    LHAT_TEST("composition carries the base's fields and members");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{ b := 0 }, extra := p^self^ { } }\n"
                 "let^ o = Bar.new^()\n"
                 "let^ x : number^ = o.a\n"
                 "let^ y : number^ = o.b\n"
                 "o.bar()\n"
                 "o.extra()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: without a marker, a name the base already uses is a mistake.
    LHAT_TEST("a same-named member needs a marker");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{}, foo := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    // 14.12: arguments may widen, which is what makes the replacement usable
    // wherever the original was.
    LHAT_TEST("override^ may widen its arguments");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    override^ foo := p^self^, x:string^|number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("override^ may not narrow them");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    override^ foo := p^self^, x:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 14.12: overloading is only allowed where no call could fit both.
    LHAT_TEST("overload^ needs a signature that stays apart");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an overlapping overload^ is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ foo := p^self^, y:string^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    LHAT_TEST("a marker with nothing under it is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{}, override^ nope := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
    unit_dispose(&u);

    // 14.12: an overloaded member is callable both ways, which is what '&'
    // means, so the intersection is what the member ends up being.
    LHAT_TEST("an overloaded member accepts either signature");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ o = Bar.new^()\n"
                 "o.bar()\n"
                 "o.bar(1)\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5 composes to make something new, so a constructor inherited from
    // the base still has to build the derived instance.
    LHAT_TEST("new^ builds the derived instance");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{ b := \"\" } }\n"
                 "let^ s : string^ = Bar.new^().b\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 17 章. Nothing here is checked by machinery of its own -- 17.9 lowers a
// pattern to a condition, so what runs is 13.11's narrowing.
static void test_patterns(void)
{
    Unit u;

    // 16.2: the focus with no name written is called it^, and it is bound.
    LHAT_TEST("the subject is in scope as it^");
    check_text(&u,
               "let^ f = f^ -> number^ { return^ 0 }\n"
               "for^ f() { when^ 0: let^ n : number^ = it^ other^: }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a named subject is in scope under its name");
    check_text(&u,
               "let^ f = f^ -> number^ { return^ 0 }\n"
               "for^ r := f() { when^ 0: let^ n : number^ = r other^: }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.2 applies to every form of for^, not only to a match.
    LHAT_TEST("it^ is bound in a loop too");
    check_text(&u, "for^ 1 to^ 3 { let^ n : number^ = it^ }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 17.9: narrowing is what makes a declared field visible, exactly as in
    // an if-chain.
    LHAT_TEST("a type pattern narrows the subject");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "for^ r := parse() {\n"
               "    when^ is^ ParseError.Syntax:\n"
               "        let^ n : number^ = r.line\n"
               "    other^:\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the field is not visible in another clause");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "for^ r := parse() {\n"
               "    when^ is^ ParseError.Eof:\n"
               "        let^ n = r.line\n"
               "    other^:\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 04 の 7 章: exhaustiveness needed no mechanism of its own, and it keeps
    // working through the sugar.
    LHAT_TEST("an exhausted union leaves the success type");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    for^ r := open() {\n"
               "        when^ is^ IOError.NotFound: return^ 0\n"
               "        when^ is^ IOError.Denied: return^ 0\n"
               "        other^: return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a union not exhausted still carries its errors");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    for^ r := open() {\n"
               "        when^ is^ IOError.NotFound: return^ 0\n"
               "        other^: return^ r\n"
               "    }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 17.3: a value pattern is a comparison, so the subject and the value
    // have to be comparable at all.
    LHAT_TEST("a pattern the subject can never match is reported");
    check_text(&u, "for^ \"text\" { when^ 1 to^ 3: other^: }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    LHAT_TEST("a value pattern of the wrong type is reported");
    check_text(&u, "for^ 1 { when^ \"text\": other^: }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    // 17.2: the expression form has a type, taken from its clauses.
    LHAT_TEST("the expression form yields the union of its clauses");
    check_text(&u,
               "let^ n = 1\n"
               "let^ s : string^ = for^ n: when^ 0: \"zero\" other^: \"more\" ;\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a clause of the wrong type is reported");
    check_text(&u,
               "let^ n = 1\n"
               "let^ s : string^ = for^ n: when^ 0: \"zero\" other^: 1 ;\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 05-modules.md. A unit is checked against a resolver that answers imports;
// here one unit stands in for the file system, which is enough to pin what
// the checker does with the answer.
typedef struct {
    Unit provider;
    const char *expected_path;
    bool asked;
} Library;

static LhatType *library_resolve(void *context, const char *path, size_t length)
{
    Library *lib = (Library *)context;
    lib->asked = true;
    if (strlen(lib->expected_path) != length ||
        memcmp(lib->expected_path, path, length) != 0) {
        return NULL;  // 6.3 reports a unit that could not be had
    }
    return lib->provider.checked.exports;
}

static void check_against(Unit *u, Library *lib, const char *provider,
                          const char *text)
{
    // The provider is checked first and into the same arena, since 6 章 has
    // the units requiring it hold on to the types it publishes.
    lhat_source_init_from_string(&lib->provider.source, "<lib>", provider,
                                 strlen(provider));
    lhat_lexer_init(&lib->provider.lexer, &lib->provider.source);
    lhat_parse(&lib->provider.lexer, &lib->provider.parsed);
    lhat_check(lib->provider.parsed.root, &lib->provider.lexer, true,
               &lib->provider.checked);

    LhatRequire require;
    require.resolve = library_resolve;
    require.context = lib;

    lhat_source_init_from_string(&u->source, "<test>", text, strlen(text));
    lhat_lexer_init(&u->lexer, &u->source);
    lhat_parse(&u->lexer, &u->parsed);
    lhat_check_unit(u->parsed.root, &u->lexer, true,
                    lib->provider.checked.types, &require, &u->checked);
}

static void check_against_dispose(Unit *u, Library *lib)
{
    unit_dispose(u);
    unit_dispose(&lib->provider);
}

static void test_modules(void)
{
    Unit u;
    Library lib;

    static const char *const provider =
        "module^ ns.geometry\n"
        "public^ let^ Point = def^{ self^{ x := 0, y := 0 } }\n"
        "public^ errordef^ Bad { Degenerate }\n"
        "let^ secret = 1\n"
        "public^ let^ dist = f^ a:number^, b:number^ -> number^ { return^ a }\n";

    // 05 の 4 章: what a unit publishes is read from its declarations, so a
    // require^ of it yields exactly the public^ names.
    LHAT_TEST("public^ names cross and private ones do not");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"lib/geometry.lh\"\n"
                  "let^ d : number^ = g.dist(1, 2)\n");
    LHAT_CHECK(lib.asked, "the resolver was asked");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    LHAT_TEST("a name without public^ does not cross");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"lib/geometry.lh\"\n"
                  "let^ s = g.secret\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    check_against_dispose(&u, &lib);

    // 05 の 6.1: a qualified name works as a type because 04 の 14.4 already
    // made one writable, so the form built for error kinds carries over.
    LHAT_TEST("a required definition is writable as a type");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"lib/geometry.lh\"\n"
                  "let^ p : g.Point = g.Point.new^()\n"
                  "let^ n : number^ = p.x\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    LHAT_TEST("a required error kind is writable as a type");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"lib/geometry.lh\"\n"
                  "let^ e : g.Bad = error^g.Bad.Degenerate{ }\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    // 05 の 6.1: the arguments of a required procedure are checked like any
    // other, which is the point of following the import at all.
    LHAT_TEST("a required procedure checks its arguments");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"lib/geometry.lh\"\n"
                  "let^ d = g.dist(1, \"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    check_against_dispose(&u, &lib);

    // 6.3: a unit that could not be had is reported where it was required.
    LHAT_TEST("a unit that cannot be had is reported");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ g = require^ \"nowhere.lh\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REQUIRE_FAILED);
    check_against_dispose(&u, &lib);

    // 05 の 5.4: require^ binds one name and the importer picks it, so two
    // units of the same shape sit side by side without colliding.
    LHAT_TEST("the importer chooses the name");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "let^ theirs = require^ \"lib/geometry.lh\"\n"
                  "let^ d : number^ = theirs.dist(1, 2)\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);
}

// 02 の 15.5 and 15.8: what a call of a yieldable procedure answers, and the
// mistake the answer makes catchable.
static void test_coroutines(void)
{
    Unit u;

    LHAT_TEST("a call of a yieldable procedure answers a coroutine");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.8: 15.5 makes such a call run no part of the body, so the statement
    // has no effect at all. C#'s IEnumerator allows this silently.
    LHAT_TEST("dropping the coroutine is reported");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "gen()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
    unit_dispose(&u);

    LHAT_TEST("delegating to it is not");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ outer = p^ { yieldall^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("binding it is not either");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "let^ d = c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A procedure that does not yield is unaffected: its effect has already
    // happened when the call returns.
    LHAT_TEST("an ordinary call still stands alone");
    check_text(&u,
               "let^ go = p^ { let^ x = 1 }\n"
               "go()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 05 の 8.5: a coroutine carries these without anything being imported.
    LHAT_TEST("a coroutine carries start, resume, dispose and iterate");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "let^ v = c.start()\n"
               "let^ w = c.resume(nil^)\n"
               "c.dispose()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.6改: and the two questions, which answer bool^ whatever the three
    // types of 13.9 turn out to be.
    LHAT_TEST("a coroutine carries done and started");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "let^ d : bool^ = c.done()\n"
               "let^ s : bool^ = c.started()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The case that made them necessary: Y is nil^ and the body ends without
    // a value, so the union a resume answers is nil^ alone and carries no
    // sign of which of the two happened.
    LHAT_TEST("they answer even where the resume union says nothing");
    check_text(&u,
               "let^ gen = p^ { yield^ }\n"
               "let^ c = gen()\n"
               "let^ v : nil^ = c.start()\n"
               "let^ d : bool^ = c.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("neither takes an argument");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "c.done(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("and nothing else");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "let^ v = c.nowhere\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    // 16.3: a table answers iterate() with a walk over its keys, without
    // anything being written. The machine has always read it that way; the
    // type of it was missing here.
    LHAT_TEST("a table carries the built-in iterate");
    check_text(&u,
               "let^ t = { 1, 2 }\n"
               "let^ w = t.iterate()\n"
               "let^ d : bool^ = w.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8 has no tuples, so what a walk yields is a table of the pair.
    LHAT_TEST("and the walk yields a table");
    check_text(&u,
               "let^ t = { 1, 2 }\n"
               "let^ pair : t^{} |nil^ = t.iterate().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it is not the values the table holds");
    check_text(&u,
               "let^ t = { 1, 2 }\n"
               "let^ pair : number^|nil^ = t.iterate().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3 lets a written iterate win, which is why the built-in is only
    // reached once the search for a written member has failed.
    LHAT_TEST("a written iterate wins over the built-in one");
    check_text(&u,
               "let^ t = { iterate := f^ { return^ p^ { yield^ 9 }() } }\n"
               "let^ v : number^|nil^ = t.iterate().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.5: the arguments belong to the call, which binds the parameters.
    // 15.4's resume carries the one value a yield^ answers with, which 13.9
    // types as the coroutine's receive type -- a different thing.
    LHAT_TEST("the arguments belong to the call");
    check_text(&u,
               "let^ p = p^ x:number^, y:string^ { let^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "let^ c = p(1, \"b\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the call still checks them");
    check_text(&u,
               "let^ p = p^ x:number^, y:string^ { yield^ 10 }\n"
               "let^ c = p(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.9: what a resume answers is the union of the yield type and the
    // return type, which the consumer tells apart. 15.2改: both are now
    // inferred from the body's yield^/return^ sites.
    LHAT_TEST("what a resume answers includes both the yield and the return type");
    check_text(&u,
               "let^ p = p^ -> string^ { let^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "let^ c = p()\n"
               "let^ s : number^|string^ = c.resume(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 15.6改: the return type is what the last resume receives, so a body
    // that reaches its end without one puts nil^ there -- that is the value
    // the machine really hands back, not 03's "returns nothing" leaking in.
    LHAT_TEST("a body with no return^ answers nil^ at the end");
    check_text(&u,
               "let^ p = p^ { yield^ 1 }\n"
               "let^ v : number^|nil^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the yield type alone does not cover it");
    check_text(&u,
               "let^ p = p^ { yield^ 1 }\n"
               "let^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // And where every exit produces one, nothing spurious joins the union.
    LHAT_TEST("a body that always returns a value brings no nil^");
    check_text(&u,
               "let^ p = p^ { yield^ 1 return^ 9 }\n"
               "let^ v : number^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9改: nor does one that cannot end at all. 16.5's endless repeat^ has
    // no last resume, so there is no value for the third type to be the type
    // of -- and a nil^ there would make every consumer narrow away something
    // that never arrives.
    LHAT_TEST("a coroutine that cannot end answers only what it yields");
    check_text(&u,
               "let^ p = p^ { repeat^ { yield^ 1 } }\n"
               "let^ v : number^ = p().start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and its walk binds a focus with no nil^ in it either");
    check_text(&u,
               "let^ p = p^ { repeat^ { yield^ 1 } }\n"
               "for^ x in^ p() { let^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A bare return^ ends it, so nil^ is back -- the resume that takes that
    // exit really receives one.
    LHAT_TEST("but a bare return^ is an end, and brings nil^ with it");
    check_text(&u,
               "let^ p = p^ { yield^ 1 return^ }\n"
               "let^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and so does a break^ that lets the loop finish");
    check_text(&u,
               "let^ p = p^ { repeat^ { yield^ 1 break^ } }\n"
               "let^ v : number^ = p().start()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // done() is what tells the two apart, and it is there either way.
    LHAT_TEST("done() is carried whether or not the body can end");
    check_text(&u,
               "let^ p = p^ { repeat^ { yield^ 1 } }\n"
               "let^ d : bool^ = p().done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("yieldall^ needs a coroutine");
    check_text(&u,
               "let^ plain = f^ -> number^ { return^ 1 }\n"
               "let^ outer = p^ { yieldall^ plain() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_COROUTINE);
    unit_dispose(&u);

    // 15.8: the value of a delegation is the inner one's return value.
    LHAT_TEST("the value of yieldall^ is the inner return type");
    check_text(&u,
               "let^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "let^ outer = p^ { let^ n : number^ = yieldall^ gen() }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it is not the type it yields");
    check_text(&u,
               "let^ gen = p^ -> number^ { yield^ 1 return^ 2 }\n"
               "let^ outer = p^ { let^ s : string^ = yieldall^ gen() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.2改: start() takes nothing and answers the same union resume does --
    // it is what runs a fresh coroutine from the top.
    LHAT_TEST("start() answers the same union as resume()");
    check_text(&u,
               "let^ p = p^ -> string^ { let^ a:number^ = yield^ 10 return^ \"a\" }\n"
               "let^ c = p()\n"
               "let^ s : number^|string^ = c.start()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("start() takes no arguments");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "c.start(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 15.2改: R is one fixed type now, so resume takes exactly one argument.
    LHAT_TEST("resume needs exactly one argument, not zero");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "c.resume()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("resume needs exactly one argument, not two");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "c.resume(1, 2)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 13.9改: every yield^ in a body has to agree on what it sends (Y) and
    // what it answers (R) -- no more folding differing yields into a union.
    LHAT_TEST("two bare yield^ that agree on Y stay clean");
    check_text(&u,
               "let^ p = p^ { yield^ 1 yield^ 2 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("two bare yield^ that disagree on Y are reported");
    check_text(&u,
               "let^ p = p^ { yield^ 1 yield^ \"a\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("two bound yield^ that disagree on R are reported");
    check_text(&u,
               "let^ p = p^ {\n"
               "    let^ a:number^ = yield^ 1\n"
               "    let^ b:string^ = yield^ 2\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);

    // 15.2改: a yield^ that a let^ binds directly is the only place R can be
    // written, so leaving the annotation off is reported rather than left
    // to infer to UNKNOWN.
    LHAT_TEST("a bound yield^ needs a written type");
    check_text(&u,
               "let^ p = p^ { let^ a = yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    LHAT_TEST("a reassigned yield^ has nowhere to write the annotation");
    check_text(&u,
               "let^ p = p^ {\n"
               "    let^ a:number^ = 0\n"
               "    a := yield^ 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    LHAT_TEST("a yield^ buried in an expression has nowhere to write it either");
    check_text(&u,
               "let^ p = p^ -> number^ { return^ 1 + yield^ 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION);
    unit_dispose(&u);

    // 15.8: yieldall^ passes the inner coroutine's Y/R through as this
    // body's own, exactly as if a yield^ had been written here directly.
    LHAT_TEST("yieldall^ can disagree with this body's own yield^ on R");
    check_text(&u,
               "let^ inner = p^ { let^ a:number^ = yield^ 1 }\n"
               "let^ outer = p^ {\n"
               "    let^ b:string^ = yield^ \"x\"\n"
               "    yieldall^ inner()\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH);
    unit_dispose(&u);
}

// 02 の 16.3. The focus of an in^ loop is bound from what the walk yields,
// which is the one place a for^ header defines names rather than reading
// them. Until this, they were read -- and found nothing.
static void test_walking(void)
{
    Unit u;

    LHAT_TEST("the focus of an in^ loop is in scope inside the body");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { let^ n = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and carries what the coroutine yields");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { let^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type is caught in the body");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { let^ s : string^ = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.7: the focus belongs to the loop, which is a scope of its own.
    LHAT_TEST("and it does not outlive the loop");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x in^ gen() { }\n"
               "let^ n = x\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 16.3: the annotation form of the focus.
    LHAT_TEST("a written focus type is what the name gets");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x:number^ in^ gen() { let^ n = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and it has to admit what the walk yields");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "for^ x:string^ in^ gen() { let^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3: a coroutine answers iterate() with itself, so one held in a name
    // walks without being called again.
    LHAT_TEST("a coroutine walks as itself");
    check_text(&u,
               "let^ gen = p^ { yield^ 1 }\n"
               "let^ c = gen()\n"
               "for^ x in^ c { let^ n : number^ = x }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8 has no tuples, so a table's built-in walk yields the pair as a
    // table -- not the values the table holds.
    LHAT_TEST("a table walks as pairs");
    check_text(&u,
               "let^ t = { 1, 2 }\n"
               "for^ pair in^ t { let^ n : number^ = pair }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.10: several names take the value apart by position, and 14 章 makes
    // position 1 the key and position 2 the value of the pair.
    LHAT_TEST("several names take the pair apart by position");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "for^ k, v in^ t { let^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type is caught for a position too");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "for^ k, v in^ t { let^ s : string^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14 章: a table is a sequence and a mapping at once, so what a key can
    // be depends on which halves the table has.
    LHAT_TEST("the key of a walk over the dense part is a number^");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "for^ k, v in^ t { let^ n : number^ = k }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and of one over written members a string^");
    check_text(&u,
               "let^ t = { a := \"x\" }\n"
               "for^ k, v in^ t { let^ s : string^ = k }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a table with both halves yields either");
    check_text(&u,
               "let^ t = { 10, a := \"x\" }\n"
               "for^ k, v in^ t { let^ x : number^|string^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and neither half alone covers it");
    check_text(&u,
               "let^ t = { 10, a := \"x\" }\n"
               "for^ k, v in^ t { let^ n : number^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 16.3: a written focus type is checked against the position it takes.
    LHAT_TEST("a focus annotation is checked against its position");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "for^ k, v:string^ in^ t { let^ n = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a written iterate is what the walk comes from");
    check_text(&u,
               "let^ t = { iterate := f^ { return^ p^ { yield^ 9 }() } }\n"
               "for^ v in^ t { let^ s : string^ = v }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and a definition may answer with one");
    check_text(&u,
               "let^ Range = def^{\n"
               "  self^{ upto := 0 },\n"
               "  new^ := f^ n { return^ self^{ upto := n } },\n"
               "  iterate := f^self^ {\n"
               "    return^ p^ { yield^ 1 }()\n"
               "  },\n"
               "}\n"
               "for^ v in^ Range.new^(4) { let^ n : number^ = v }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 16.3: what in^ walks has to answer with a coroutine, the same demand
    // 15.8 makes of yieldall^.
    LHAT_TEST("walking something with no iterate is reported");
    check_text(&u, "for^ x in^ 5 { let^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_COROUTINE);
    unit_dispose(&u);

    LHAT_TEST("and so is an iterate that answers something else");
    check_text(&u,
               "let^ t = { iterate := f^ -> number^ { return^ 1 } }\n"
               "for^ x in^ t { let^ n = x }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_COROUTINE);
    unit_dispose(&u);
}

// 02 の 14 章 makes a table a sequence as well as a mapping. The keyed half
// was described by name from the start; the sequence half was dropped on the
// floor, so nothing downstream of it -- t[1], unpack^, a walk's pair -- had
// anything to read.
static void test_positions(void)
{
    Unit u;

    LHAT_TEST("a positional entry carries its type");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "let^ n : number^ = t[1]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("so the wrong type at a position is caught");
    check_text(&u,
               "let^ t = { 10, 20 }\n"
               "let^ s : string^ = t[1]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and each position keeps its own");
    check_text(&u,
               "let^ t = { 10, \"a\" }\n"
               "let^ s : string^ = t[2]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The machine numbers the dense part in written order, skipping the
    // entries that carry a key -- so the types have to be counted the same.
    LHAT_TEST("a keyed entry takes no position from the ones after it");
    check_text(&u,
               "let^ t = { 10, a := \"x\", 20 }\n"
               "let^ n : number^ = t[2]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.10 lets a table carry more than its type lists, so a position it
    // says nothing about is unknown rather than absent.
    LHAT_TEST("a position the type does not mention says nothing");
    check_text(&u,
               "let^ t = { 10 }\n"
               "let^ s : string^ = t[9]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a written key reaches the member of that name");
    check_text(&u,
               "let^ t = { a := 1 }\n"
               "let^ s : string^ = t[\"a\"]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10改: the sequence half can be written down as well as inferred --
    // types listed with no name in front of them, in order.
    LHAT_TEST("a written type may list its positions");
    check_text(&u, "let^ t : t^{ number^, string^ } = { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and each one is checked");
    check_text(&u, "let^ t : t^{ number^, string^ } = { 1, 2 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a position the value does not fill is missing");
    check_text(&u, "let^ t : t^{ number^, string^ } = { 1 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10 asks for at least what is listed, positions included.
    LHAT_TEST("but more positions than were asked for are fine");
    check_text(&u, "let^ t : t^{ number^ } = { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("names and positions mix in one type");
    check_text(&u,
               "let^ t : t^{ number^, a : string^ } = { 1, a := \"x\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A named entry takes no place in the sequence, in the type as in the
    // literal -- so the count does not depend on where it was written.
    LHAT_TEST("and a named one takes no position, wherever it stands");
    check_text(&u,
               "let^ t : t^{ a : string^, number^ } = { 1, a := \"x\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // What it is for: the positions reach unpack^ and an index from an
    // annotation, not only from a literal the checker watched being built.
    LHAT_TEST("a written position reaches unpack^");
    check_text(&u,
               "let^ f = f^ -> t^{ number^, string^ } { return^ { 1, \"a\" } }\n"
               "let^ q, r = unpack^ f()\n"
               "let^ n : number^ = q\n"
               "let^ s : string^ = r\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and is not interchangeable there either");
    check_text(&u,
               "let^ f = f^ -> t^{ number^, string^ } { return^ { 1, \"a\" } }\n"
               "let^ q, r = unpack^ f()\n"
               "let^ s : string^ = q\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and an index reaches it too");
    check_text(&u,
               "let^ f = p^ x : t^{ number^, string^ } {\n"
               "    let^ s : string^ = x[1]\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.10: unpack^ takes one value apart by position. The mark is on the
    // right, which is what tells this from a multiple definition.
    LHAT_TEST("unpack^ gives each name the position it takes");
    check_text(&u,
               "let^ q, r = unpack^ { 1, \"a\" }\n"
               "let^ n : number^ = q\n"
               "let^ s : string^ = r\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the positions are not interchangeable");
    check_text(&u,
               "let^ q, r = unpack^ { 1, \"a\" }\n"
               "let^ s : string^ = q\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("an annotation on a destructured name is checked");
    check_text(&u, "let^ q:string^, r:string^ = unpack^ { 1, \"a\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and one that agrees is not");
    check_text(&u, "let^ q:number^, r:string^ = unpack^ { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 8.6: the same form reassigning names that already exist.
    LHAT_TEST("a destructuring reassignment is checked the same way");
    check_text(&u,
               "let^ q = 0\n"
               "let^ r = \"\"\n"
               "q, r := unpack^ { 1, \"a\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a position that does not fit the name is reported");
    check_text(&u,
               "let^ q = \"\"\n"
               "let^ r = \"\"\n"
               "q, r := unpack^ { 1, \"a\" }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Without the mark the right side is one value per target (13.10), and
    // that path is untouched.
    LHAT_TEST("a multiple definition still pairs the two sides off");
    check_text(&u,
               "let^ a, b = 1, \"x\"\n"
               "let^ n : number^ = a\n"
               "let^ s : string^ = b\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and still catches a pair that does not fit");
    check_text(&u,
               "let^ a, b = 1, \"x\"\n"
               "let^ s : string^ = a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);
}

// 02 の 13.2 gives a signature a form for "no result", and 03 の 3.4 keeps
// that apart from nil^ on the grounds that otherwise 11.7's `??` would apply
// to a meaningless expression. That reasoning only holds if calling such a
// subroutine produces something no value fits -- which it did not: the result
// was a NULL, and a NULL is how "not inferred" is spelled, so every use of it
// was waved through.
static void test_no_value(void)
{
    Unit u;

    // The case 03 names outright.
    LHAT_TEST("?? cannot apply to a call that produces nothing");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"a\") ?? 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_BE_NIL);
    unit_dispose(&u);

    // 04 の 4.1 puts catch^ on the same footing.
    LHAT_TEST("nor can catch^");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"a\") catch^ 0\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_CANNOT_FAIL);
    unit_dispose(&u);

    LHAT_TEST("a name cannot be bound to it");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("nor reassigned to one");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = 1\n"
               "v := log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("an annotation does not admit it either");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v : number^ = log(\"a\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and neither does an argument");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(log(\"a\"))\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("arithmetic needs a number, which this is not");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"a\") + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_NUMBER);
    unit_dispose(&u);

    LHAT_TEST("and a condition a bool^");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "if^ log(\"a\") { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_BOOL);
    unit_dispose(&u);

    // Reported where it is written rather than let into the result, where it
    // would reach every caller as a type nothing inhabits.
    LHAT_TEST("a return^ cannot carry it");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ f = p^ { return^ log(\"a\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("with a written result saying so too");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ f = p^ -> number^ { return^ log(\"a\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 8.2: a call is the one expression that stands alone as a statement, and
    // that is where a subroutine with no result belongs.
    LHAT_TEST("but calling it as a statement is the point of it");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "log(\"a\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 03 の 3.4 reads a NULL result as "still being worked out", so a body
    // calling itself has to keep getting one.
    LHAT_TEST("and a self-call is still a result being inferred");
    check_text(&u,
               "let^ fact = f^ n:number^ {\n"
               "    if^ n <= 1 { return^ 1 }\n"
               "    return^ n * this^(n - 1)\n"
               "}\n"
               "let^ n : number^ = fact(5)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.9改: a coroutine that cannot end never produces a return value, so
    // delegating to one produces nothing either.
    LHAT_TEST("delegating to a coroutine that cannot end produces nothing");
    check_text(&u,
               "let^ g = p^ { repeat^ { yield^ 1 } }\n"
               "let^ o = p^ { let^ v = yieldall^ g() }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Every position that wants a value says so. A statement is the one that
    // does not (8.2), and it is checked above.
    LHAT_TEST("a table entry wants a value");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ t = { a := log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and so does a positional one");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ t = { log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("an index key wants one");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ t = { 1 }\n"
               "let^ v = t[log(\"x\")]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("as^ wants one to ascribe");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"x\") as^ number^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("unpack^ wants one to take apart");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ q, r = unpack^ log(\"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.3 leaves what '..' means to op^, but it cannot mean anything at all
    // when handed nothing.
    LHAT_TEST("'..' wants one on either side");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ v = log(\"x\") .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("a yield^ wants one to send out");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ g = p^ { yield^ log(\"x\") }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 01 の 5.4: a hole is an ordinary expression, and was not being checked
    // at all -- not for this and not for anything else.
    LHAT_TEST("an interpolation hole wants one");
    check_text(&u,
               "let^ log = p^ m:string^ { let^ y = m }\n"
               "let^ s = $\"v={log(\"x\")}\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("and is checked like any other expression");
    check_text(&u, "let^ s = $\"v={nowhere}\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    LHAT_TEST("while a sound one stays sound");
    check_text(&u,
               "let^ n = 1\n"
               "let^ s = $\"v={n} and {n + 1}\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

int main(void)
{
    test_names();
    test_expressions();
    test_results();
    test_errors();
    test_annotations();
    test_narrowing();
    test_definitions();
    test_composition();
    test_patterns();
    test_modules();
    test_coroutines();
    test_walking();
    test_positions();
    test_no_value();
    return lhat_test_report("test_check");
}
