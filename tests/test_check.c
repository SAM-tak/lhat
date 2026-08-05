// L^ (lhat) -- tests for the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". The cases pinned here are the ones a decision in the
// specification produces and that would otherwise be invisible: the scope
// rule of 8.7, the result inference of 03 の 3.4, and the way catch^, ?? and
// try^ each drop one arm of a union.

#include <stdio.h>
#include <stdlib.h>
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

// The same, as the next input of a session (03 の 4.3).
static void check_next_text(Unit *u, LhatCheckSession *s, const char *text)
{
    lhat_source_init_from_string(&u->source, "<test>", text, strlen(text));
    lhat_lexer_init(&u->lexer, &u->source);
    lhat_parse(&u->lexer, &u->parsed);
    lhat_check_next(s, u->parsed.root, &u->lexer, true, &u->checked);
}

// The same again, read the way a prompt reads it (02 の 8.2).
static void check_asked_text(Unit *u, LhatCheckSession *s, const char *text)
{
    lhat_source_init_from_string(&u->source, "<test>", text, strlen(text));
    lhat_lexer_init(&u->lexer, &u->source);
    lhat_parse_interactive(&u->lexer, &u->parsed);
    lhat_check_next(s, u->parsed.root, &u->lexer, true, &u->checked);
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

    // 03 の 7 章、P6: neither half is annotated, so each infers its result
    // from its own body alone. 'a' is checked first, and its call to the
    // not-yet-checked 'b' is unknown^ -- so 'a' infers bool^|unknown^, not
    // plain bool^. Only that leftover unknown^ arm is what makes the
    // mismatch below fire; before the fix, append_arms mistook it for a
    // duplicate of the bool^ arm already there and silently dropped it,
    // leaving 'a' looking like a clean bool^ when it was not one.
    LHAT_TEST("mutual recursion still needs its own annotation to be checked");
    check_text(&u,
               "let^ a = f^ n:number^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n"
               "let^ x : bool^ = a(4)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // The same pair, both sides telling the truth about what they return --
    // 'b' really is number^|bool^, and saying so is what makes 'a' check.
    LHAT_TEST("and a correct annotation on both sides checks cleanly");
    check_text(&u,
               "let^ a = f^ n:number^ -> bool^ {\n"
               "  if^ n <= 0 { return^ true^ }\n"
               "  return^ b(n - 1)\n"
               "}\n"
               "let^ b = f^ n:number^ -> number^|bool^ {\n"
               "  if^ n <= 0 { return^ 999 }\n"
               "  return^ a(n - 1)\n"
               "}\n"
               "let^ x : bool^ = a(4)\n"
               "let^ y : number^|bool^ = b(4)\n");
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
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
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

    // 7.4改: 'x += 1' checks the same way 'x := x + 1' would -- the operator
    // it stands for has to accept the right side, and the result has to fit
    // the name.
    LHAT_TEST("compound assignment checks like the operator it stands for");
    check_text(&u, "let^ x : number^ = 1\nx += 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses what the operator would refuse");
    check_text(&u, "let^ s = \"hi\"\ns += 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
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
               "    repeat^ { for^ let^ x := 1 if^ true^ { break^ } }\n"
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

    // 11.6改: is^ is a comparison like '=', so the same disjointness check
    // applies and it answers bool^ either way.
    LHAT_TEST("is^ answers bool^");
    check_text(&u, "let^ x : bool^ = 1 is^ 2\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("is^ on types that can never meet is reported");
    check_text(&u, "return^ 1 is^ \"text\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_INCOMPARABLE);
    unit_dispose(&u);

    // 13.11: isa^ reads a type, so an unknown one is reported there too.
    LHAT_TEST("isa^ resolves its right side as a type");
    check_text(&u, "let^ x = 1\nlet^ b : bool^ = x isa^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: any^ holds of every value, so the question is empty whatever is
    // on the left. 13.11 decides this from the right side alone -- it never
    // reads the left's inferred type against the right.
    LHAT_TEST("asking isa^ any^ asks nothing and is reported");
    check_text(&u, "let^ x : number^ = 1\nlet^ b = x isa^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    LHAT_TEST("whatever the left happens to be");
    check_text(&u, "let^ x : any^ = 1\nlet^ b = x isa^ any^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.5 collapses a union with any^ to any^, so the written form does not
    // let it through.
    LHAT_TEST("and however the any^ is spelled");
    check_text(&u, "let^ x : number^ = 1\nlet^ b = x isa^ any^|nil^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ISA_ALWAYS_TRUE);
    unit_dispose(&u);

    // 13.11: an answer the left's inferred type fixes is not refused. isa^ is
    // there to be asked at run time, and the checker narrowing that from what
    // it thinks it knows is what 13.7 introduced any^ to avoid.
    LHAT_TEST("but an answer fixed by the left is left alone");
    check_text(&u,
               "let^ x : number^ = 1\n"
               "let^ a = x isa^ string^\n"
               "let^ b = x isa^ number^\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("which is what makes any^ usable at all");
    check_text(&u, "let^ f = p^ x:any^ { let^ b = x isa^ string^ }\n");
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
               "if^ r isa^ number^ { let^ n : number^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the false branch keeps the rest");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r isa^ number^ {\n"
               "    else^:\n"
               "        let^ s : string^ = r\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("narrowing does not reach past what was tested");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r isa^ number^ { let^ s : string^ = r }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 04 の 6.1: narrowing to a kind is what makes its declared field visible.
    LHAT_TEST("a narrowed error kind shows its fields");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "let^ r = parse()\n"
               "if^ r isa^ ParseError.Syntax { let^ n : number^ = r.line }\n");
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
               "    if^ r isa^ ParseError.Syntax { return^ 0 }\n"
               "    if^ r isa^ ParseError.Eof { return^ 0 }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("a branch that falls through leaves nothing behind");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ g = f^ -> number^ {\n"
               "    let^ r = f()\n"
               "    if^ r isa^ string^ { }\n"
               "    return^ r + 1\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 04 の 7 章: handling every kind is ordinary narrowing, so the success
    // type is what is left in the last clause.
    LHAT_TEST("an exhausted union leaves the success type");
    check_text(&u,
               "errordef^ IOError { NotFound, Denied }\n"
               "let^ open = f^ -> number^|IOError { return^ 0 }\n"
               "let^ use = f^ -> number^ {\n"
               "    let^ r = open()\n"
               "    if^ r isa^ IOError.NotFound {\n"
               "        return^ 0\n"
               "        elseif^ r isa^ IOError.Denied:\n"
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
               "    if^ r isa^ IOError.NotFound {\n"
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
               "if^ !(r isa^ number^) { let^ s : string^ = r }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: and^ tells us both held; or^ says nothing when it is true.
    LHAT_TEST("and^ narrows both sides");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ g = f^ -> number^|string^ { return^ 0 }\n"
               "let^ a = f()\n"
               "let^ b = g()\n"
               "if^ a isa^ number^ and^ b isa^ number^ {\n"
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
               "    if^ f() isa^ number^ { return^ f() + 1 }\n"
               "    return^ 0\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a dot path is narrowed");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ t = { a := f() }\n"
               "if^ t.a isa^ number^ { let^ n : number^ = t.a }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.11: reassigning ends it, since the claim was about what was examined.
    LHAT_TEST("a reassignment ends the narrowing");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "if^ r isa^ number^ {\n"
               "    r := f()\n"
               "    let^ n : number^ = r\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("narrowing works in the if expression too");
    check_text(&u,
               "let^ f = f^ -> number^|string^ { return^ 0 }\n"
               "let^ r = f()\n"
               "let^ n : number^ = if^ r isa^ number^: r el^: 0 ;\n");
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

    // 14.4: taking the method out from under the dot is what spells the
    // receiver out, and then it is counted like any other argument.
    LHAT_TEST("a method taken as a value is passed the receiver");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "let^ bump = C.bump\n"
               "bump(c, 1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.4: the form is what decides, and a parenthesis does not change it.
    // JavaScript reads '(obj.m)()' the same way.
    LHAT_TEST("parentheses do not take the receiver off");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ c = C.new^()\n"
               "(C.bump)(c, 1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // The other side of the dot is unaffected -- there the parenthesis holds
    // the receiver, and what is called is still a member access.
    LHAT_TEST("a parenthesised receiver is still a method call");
    check_text(&u,
               "let^ C = def^{\n"
               "    self^{ v := 0 },\n"
               "    bump := p^self^, step:number^ { },\n"
               "}\n"
               "let^ x = C.new^()\n"
               "let^ y = C.new^()\n"
               "(if^ true^: x el^: y;).bump(1)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 12.5 with 12.7: with^ wants a dispose(), and one that returns nothing,
    // because 12.7 made cleanup unable to fail.
    LHAT_TEST("with^ accepts a value that has dispose()");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 }, dispose := p^self^ { } }\n"
               "with^ c = C.new^() { }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("with^ refuses a value without one");
    check_text(&u,
               "let^ C = def^{ self^{ v := 1 } }\n"
               "with^ c = C.new^() { }\n");
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

    // 14.15改: an override^ over nothing is not a mistake -- it says the
    // composition has to bring what it replaces. What it costs is that the
    // definition can no longer be instantiated on its own.
    LHAT_TEST("a marker with nothing under it waits for a composition");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{}, override^ nope := p^self^ { } }\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and stands in the way of new^ until one comes");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{}, override^ nope := p^self^ { } }\n"
                 "let^ o = Bar.new^()\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.12: overload^ has no such reading. Adding a way to call something
    // that is not there says nothing.
    LHAT_TEST("overload^ over nothing is still reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{ self^{}, overload^ nope := p^self^ { } }\n");
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

    // 14.12: once a name is overloaded it carries an intersection, and an
    // override^ replaces the one arm it overlaps. Comparing the replacement
    // against the whole intersection would refuse it, since no single
    // signature is usable where all of them were.
    LHAT_TEST("override^ replaces the one overloaded arm it overlaps");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, n:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The arms that were not overridden are untouched, so the name goes on
    // being callable every way it was before.
    LHAT_TEST("the arms an override^ left alone stay callable");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ o = Baz.new^()\n"
                 "o.bar()\n"
                 "o.bar(1)\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12: override^ may widen its arguments, and a widening can reach two
    // of the arms at once. Which one was meant is then not decidable.
    LHAT_TEST("an override^ over two overloaded arms is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, ...:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // Overlapping is what picks the arm, so a signature that overlaps none of
    // them has nothing to replace -- the same as a marker over a name that
    // was never there.
    LHAT_TEST("an override^ that overlaps no arm is reported");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", base,
                 "let^ Bar = Foo .. def^{\n"
                 "    self^{},\n"
                 "    overload^ bar := p^self^, n:number^ { },\n"
                 "}\n"
                 "let^ Baz = Bar .. def^{\n"
                 "    self^{},\n"
                 "    override^ bar := p^self^, a:number^, b:number^ { },\n"
                 "}\n");
        check_text(&u, text);
    }
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
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

    // 14.5: the right side of '..' may be a name rather than a def^ literal.
    // The compiler flattens that chain through its own registry (14.2), so
    // the type has to carry both sides -- it used to answer with the right
    // one alone and lose everything the left brought.
    static const char *const parts =
        "let^ A = def^{ self^{ x := 0 }, a := f^ -> number^ { return^ 1 } }\n"
        "let^ B = def^{ self^{ y := \"s\" }, b := f^ -> number^ { return^ 2 } }\n";

    LHAT_TEST("composition by name carries both sides");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "let^ D = A .. B\n"
                 "let^ p : number^ = D.a()\n"
                 "let^ q : number^ = D.b()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an instance of it holds both templates");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "let^ D = A .. B\n"
                 "let^ o = D.new^()\n"
                 "let^ p : number^ = o.x\n"
                 "let^ q : string^ = o.y\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A chain mixing both forms is what 14.2 lets the compiler settle, so the
    // checker has to reach the same answer.
    LHAT_TEST("a chain of names ending in a literal carries all of them");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts,
                 "let^ D = A .. B .. def^{ self^{ z := true^ },\n"
                 "                         c := f^ -> number^ { return^ 3 } }\n"
                 "let^ o = D.new^()\n"
                 "let^ p : number^ = o.x\n"
                 "let^ q : string^ = o.y\n"
                 "let^ r : bool^ = o.z\n"
                 "let^ s : number^ = o.c()\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5改: neither side of a composition by name was written against the
    // other, so neither is the answer under a name they share. The
    // composition stands -- what it costs is that the name is no longer
    // reachable through it.
    LHAT_TEST("a member both sides carry does not stop the composition");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "let^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
               "let^ D = A .. B\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and reading it through the composition is reported");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "let^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
               "let^ D = A .. B\n"
               "let^ r : number^ = D.new^().m()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER);
    unit_dispose(&u);

    // 14.4 is the way out, and it wanted nothing added: a method taken from
    // the side that wrote it applies to whatever fits it structurally.
    LHAT_TEST("but naming the side reaches either one");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^self^ -> number^ { return^ 1 } }\n"
               "let^ B = def^{ self^{}, m := f^self^ -> number^ { return^ 2 } }\n"
               "let^ D = A .. B\n"
               "let^ fromA = A.m\n"
               "let^ fromB = B.m\n"
               "let^ o = D.new^()\n"
               "let^ x : number^ = fromA(o)\n"
               "let^ y : number^ = fromB(o)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the names only one side carries are untouched");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 },\n"
               "  only := f^ -> number^ { return^ 9 } }\n"
               "let^ B = def^{ self^{}, m := f^ -> number^ { return^ 2 } }\n"
               "let^ D = A .. B\n"
               "let^ r : number^ = D.new^().only()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5改: a field is the one that stays an error at the '..'. A method is
    // shared, so either side's is still reachable through it; a field is
    // per-instance and the flattened table holds one, so there is no
    // qualified form to fall back to.
    LHAT_TEST("a field both templates carry is still reported at the '..'");
    check_text(&u,
               "let^ A = def^{ self^{ v := 0 } }\n"
               "let^ B = def^{ self^{ v := \"x\" } }\n"
               "let^ D = A .. B\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COMPOSE_COLLIDES);
    unit_dispose(&u);

    // 14.15 is the way to write a mixin that does not own the field it uses,
    // and two of them compose without ever colliding.
    LHAT_TEST("and two mixins that declare rather than own do not collide");
    check_text(&u,
               "let^ A = def^{ self^{ abstract^ v : number^ },\n"
               "  a := f^self^ -> number^ { return^ self^.v } }\n"
               "let^ B = def^{ self^{ abstract^ v : number^ },\n"
               "  b := f^self^ -> number^ { return^ self^.v } }\n"
               "let^ Host = def^{ self^{ v := 3 } }\n"
               "let^ D = Host .. A .. B\n"
               "let^ r : number^ = D.new^().a()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12改: super^ names what an override^ is writing over, so it is a
    // name only there. Nothing else in a def^ hid anything.
    LHAT_TEST("super^ is a name inside an override^");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "let^ D = A .. def^{ self^{},\n"
               "  override^ m := f^ -> number^ { return^ super^() + 1 } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and not in a member with no marker");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ super^() } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SUPER_OUTSIDE);
    unit_dispose(&u);

    LHAT_TEST("and not outside a def^ at all");
    check_text(&u, "let^ x = super^\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_SUPER_OUTSIDE);
    unit_dispose(&u);

    // 14.4: super^(…) is the bound form, so the receiver is not written; taken
    // as a value it is spelled out, the way a method taken from under the dot
    // is.
    LHAT_TEST("super^ called directly takes the receiver on its own");
    check_text(&u,
               "let^ A = def^{ self^{ n := 0 },\n"
               "  get := f^self^ -> number^ { return^ self^.n } }\n"
               "let^ D = A .. def^{ self^{},\n"
               "  override^ get := f^self^ -> number^ { return^ super^() } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and wants it written once taken as a value");
    check_text(&u,
               "let^ A = def^{ self^{ n := 0 },\n"
               "  get := f^self^ -> number^ { return^ self^.n } }\n"
               "let^ D = A .. def^{ self^{},\n"
               "  override^ get := f^self^ -> number^ {\n"
               "    let^ old = super^\n"
               "    return^ old(self^)\n"
               "  } }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.15: abstract^ declares a member and leaves it for the composition,
    // which is what lets a mixin call something it does not itself provide.
    LHAT_TEST("abstract^ is filled in by a composition");
    check_text(&u,
               "let^ Counting = def^{\n"
               "    self^{ count := 0 },\n"
               "    abstract^ step : f^ -> number^;,\n"
               "    bump := p^self^ { self^.count := self^.count + class^.step() },\n"
               "}\n"
               "let^ Fast = Counting .. def^{\n"
               "    self^{},\n"
               "    step := f^ -> number^ { return^ 10 },\n"
               "}\n"
               "let^ o = Fast.new^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.11: an instance carries a value under every name, so one still only
    // declared has nothing to make.
    LHAT_TEST("and a definition still holding one cannot be instantiated");
    check_text(&u,
               "let^ Counting = def^{ self^{}, abstract^ step : f^ -> number^; }\n"
               "let^ o = Counting.new^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // A declaration is not a definition of the member, so filling it in is
    // not 14.12's collision and wants no marker.
    LHAT_TEST("filling an abstract^ in needs no marker");
    check_text(&u,
               "let^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "let^ B = A .. def^{ self^{},\n"
               "  m := f^ -> number^ { return^ 1 } }\n"
               "let^ o = B.new^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and refuses one, since nothing was replaced");
    check_text(&u,
               "let^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "let^ B = A .. def^{ self^{},\n"
               "  override^ m := f^ -> number^ { return^ 1 } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE);
    unit_dispose(&u);

    // What the declaration does ask is that the value fit the type it wrote.
    LHAT_TEST("what fills an abstract^ has to fit the type it declared");
    check_text(&u,
               "let^ A = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "let^ B = A .. def^{ self^{},\n"
               "  m := f^ -> string^ { return^ \"x\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    LHAT_TEST("declaring what is already provided is reported");
    check_text(&u,
               "let^ A = def^{ self^{}, m := f^ -> number^ { return^ 1 } }\n"
               "let^ B = A .. def^{ self^{}, abstract^ m : f^ -> number^; }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ALREADY_PROVIDED);
    unit_dispose(&u);

    // 14.6: the template takes one too, which is what lets a mixin reach a
    // field through self^ without owning it.
    LHAT_TEST("a template field may be abstract^ as well");
    check_text(&u,
               "let^ Greet = def^{\n"
               "    self^{ abstract^ n : number^ },\n"
               "    hello := f^self^ -> number^ { return^ self^.n + 1 },\n"
               "}\n"
               "let^ Thing = Greet .. def^{ self^{ n := 10 } }\n"
               "let^ r : number^ = Thing.new^().hello()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and one left unfilled stops the construction too");
    check_text(&u,
               "let^ Greet = def^{ self^{ abstract^ n : number^ } }\n"
               "let^ o = Greet.new^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.5: the two sides of a composition by name pair up the same way --
    // one declaring what the other provides is the point, not a collision.
    LHAT_TEST("a declaration and a definition compose by name");
    check_text(&u,
               "let^ Need = def^{ self^{}, abstract^ m : f^ -> number^; }\n"
               "let^ Give = def^{ self^{}, m := f^ -> number^ { return^ 7 } }\n"
               "let^ D = Need .. Give\n"
               "let^ r : number^ = D.new^().m()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.15改: with the two together, a mixin written against no base at all
    // can replace a member and call what it replaced. abstract^ says what the
    // host must hold, the pending override^ says what it must provide.
    LHAT_TEST("a mixin written against no base composes onto one");
    check_text(&u,
               "let^ Base = def^{ self^{ n := 0 },\n"
               "  run := p^self^ { self^.n := self^.n + 1 } }\n"
               "let^ Logged = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { self^.n := self^.n + 10\n"
               "                             super^() } }\n"
               "let^ App = Base .. Logged\n"
               "let^ o = App.new^()\n"
               "o.run()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and two of them stack");
    check_text(&u,
               "let^ Base = def^{ self^{ n := 0 },\n"
               "  run := p^self^ { self^.n := self^.n + 1 } }\n"
               "let^ A = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { super^() } }\n"
               "let^ B = def^{ self^{ abstract^ n : number^ },\n"
               "  override^ run := p^self^ { super^() } }\n"
               "let^ App = Base .. A .. B\n"
               "let^ o = App.new^()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Stacking two settles neither -- the chain still wants something under
    // them both, so new^ stays out of reach.
    LHAT_TEST("but stacking two does not settle either");
    check_text(&u,
               "let^ A = def^{ self^{}, override^ run := p^self^ { super^() } }\n"
               "let^ B = def^{ self^{}, override^ run := p^self^ { super^() } }\n"
               "let^ X = A .. B\n"
               "let^ o = X.new^()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_STILL_ABSTRACT);
    unit_dispose(&u);

    // 14.12's check is the one the def^ would have had. It runs where the
    // two finally meet instead.
    LHAT_TEST("substitutability is checked where the two meet");
    check_text(&u,
               "let^ Base = def^{ self^{}, run := f^ -> number^ { return^ 1 } }\n"
               "let^ Bad = def^{ self^{},\n"
               "  override^ run := f^ -> string^ { return^ \"x\" } }\n"
               "let^ App = Base .. Bad\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 14.11 gives every definition a new^ whether or not one was written, so
    // the two always carry that name. It is rebuilt rather than collided.
    LHAT_TEST("the synthesised new^ is not a collision");
    {
        char text[1024];
        snprintf(text, sizeof text, "%s%s", parts, "let^ D = A .. B\n");
        check_text(&u, text);
    }
    CHECK_CLEAN(&u);
    unit_dispose(&u);
}

// 02 の 14.12改: typeof^'s own static type is a fixed nominal carrier
// (TypeInfo) regardless of the operand -- the descriptive payload is a
// runtime concern (03 の 4.2), so the checker's job is only to give
// '.signature' somewhere to resolve and to still check the operand.
static void test_typeof(void)
{
    Unit u;

    LHAT_TEST("typeof^(x).signature is a string^");
    check_text(&u, "let^ s : string^ = typeof^(5).signature\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("an unknown member is refused");
    check_text(&u, "let^ x = typeof^(5).bogus\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("the operand is still checked");
    check_text(&u, "let^ x = typeof^(nowhere)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 02 の 2811: typeof^(x) = typeof^(y) has to type-check at all, which
    // needs both sides to resolve to the very same TypeInfo type.
    LHAT_TEST("two typeof^ results compare with '='");
    check_text(&u, "let^ b : bool^ = typeof^(5) = typeof^(\"x\")\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Session-scoped the way L^'s environment is (05 の 8.6), so a typeof^
    // bound in one input is still the same nominal type in the next.
    LHAT_TEST("the TypeInfo type is the same across a session's inputs");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ t = typeof^(5)\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_next_text(&u, s, "let^ b : bool^ = t = typeof^(\"x\")\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }
}

// 02 の 13.7: the variadic collector. '...' inside the body names it, typed
// as 14.10改's unbounded tail -- a table whose sequence half is one element
// type repeated, nothing fixed.
static void test_variadic(void)
{
    Unit u;

    LHAT_TEST("'...' inside the body is a table of the element type");
    check_text(&u,
               "let^ f = f^ ...:number^ -> number^ {\n"
               "  let^ t : t^{ ...:number^ } = ...\n"
               "  return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.8 has no tuples, so a single name over an in^ walk takes the whole
    // pair (14 章), the same as walking any other table -- 13.7 does not
    // special-case this.
    LHAT_TEST("for^ i, x in^ ... gives x the element type");
    check_text(&u,
               "let^ f = f^ ...:number^ -> number^ {\n"
               "  let^ total = 0\n"
               "  for^ i, x in^ ... {\n"
               "    let^ n : number^ = x\n"
               "    let^ p : number^ = i\n"
               "    total := total + x\n"
               "  }\n"
               "  return^ total\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 04 の 11.3: a dynamic key may be out of range, and this one really can
    // be -- the count is unbounded, unlike a table with listed members.
    LHAT_TEST("a dynamic index into ... may be nil^");
    check_text(&u,
               "let^ f = f^ ...:number^ -> number^ {\n"
               "  let^ i = 1\n"
               "  let^ v : number^|nil^ = ...[i]\n"
               "  return^ 0\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 13.7: at least the fixed count, any number beyond it.
    LHAT_TEST("fewer than the fixed count is refused");
    check_text(&u,
               "let^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
               "  return^ a + b\n"
               "}\n"
               "f(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    LHAT_TEST("exactly the fixed count, with none variadic, is fine");
    check_text(&u,
               "let^ f = f^ a:number^, b:number^, ...:number^ -> number^ {\n"
               "  return^ a + b\n"
               "}\n"
               "f(1, 2)\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a mismatched variadic argument is still caught");
    check_text(&u,
               "let^ f = f^ ...:number^ -> number^ { return^ 0 }\n"
               "f(1, \"x\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 13.7: 'expr...' spreads a collected table back into a variadic tail.
    LHAT_TEST("'...' forwards into another variadic call");
    check_text(&u,
               "let^ inner = f^ ...:number^ -> number^ { return^ 0 }\n"
               "let^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(...)\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("forwarding into a non-variadic callee is refused");
    check_text(&u,
               "let^ inner = f^ -> number^ { return^ 0 }\n"
               "let^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(...)\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_VARIADIC);
    unit_dispose(&u);

    LHAT_TEST("fixed arguments may lead a forwarded spread");
    check_text(&u,
               "let^ inner = f^ base:number^, ...:number^ -> number^ {\n"
               "  return^ base\n"
               "}\n"
               "let^ outer = f^ ...:number^ -> number^ {\n"
               "  return^ inner(100, ...)\n"
               "}\n");
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
               "for^ let^ r := f() { when^ 0: let^ n : number^ = r other^: }\n");
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
               "for^ let^ r := parse() {\n"
               "    when^ isa^ ParseError.Syntax:\n"
               "        let^ n : number^ = r.line\n"
               "    other^:\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("the field is not visible in another clause");
    check_text(&u,
               "errordef^ ParseError { Syntax { line : number^ }, Eof }\n"
               "let^ parse = f^ -> number^|ParseError { return^ 0 }\n"
               "for^ let^ r := parse() {\n"
               "    when^ isa^ ParseError.Eof:\n"
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
               "    for^ let^ r := open() {\n"
               "        when^ isa^ IOError.NotFound: return^ 0\n"
               "        when^ isa^ IOError.Denied: return^ 0\n"
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
               "    for^ let^ r := open() {\n"
               "        when^ isa^ IOError.NotFound: return^ 0\n"
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

static LhatType *library_resolve(void *context, const char *path, size_t length,
                                 const char **module_name)
{
    Library *lib = (Library *)context;
    lib->asked = true;
    if (strlen(lib->expected_path) != length ||
        memcmp(lib->expected_path, path, length) != 0) {
        return NULL;  // 6.3 reports a unit that could not be had
    }
    // 05 の 3 章: what the provider declared, which 5.4改 binds it under.
    if (module_name != NULL) {
        *module_name = lib->provider.checked.module_name;
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

    // 05 の 3.1改: the path module^ declared is kept, which is what 5.4改
    // reads to decide where the short form binds.
    LHAT_TEST("the path module^ declared is recorded");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider, "let^ g = require^ \"lib/geometry.lh\"\n");
    LHAT_CHECK(lib.provider.checked.module_name != NULL,
               "the provider declared a path");
    if (lib.provider.checked.module_name != NULL) {
        LHAT_CHECK(strcmp(lib.provider.checked.module_name, "ns.geometry") == 0,
                   "the path is written out with its dots");
    }
    check_against_dispose(&u, &lib);

    LHAT_TEST("and a unit that declares none records nothing");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/plain.lh";
    check_against(&u, &lib, "public^ let^ thing = 1\n",
                  "let^ g = require^ \"lib/plain.lh\"\n");
    LHAT_CHECK(lib.provider.checked.module_name == NULL, "3.2 allows none");
    check_against_dispose(&u, &lib);

    // 05 の 5.4改: without a let^ the unit goes under the path it declared.
    // 8.8 makes the tables on the way, so only the root is a new name.
    LHAT_TEST("the short form binds under the declared path");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib,
                  provider,
                  "require^ \"lib/geometry.lh\"\n"
                  "let^ d : number^ = ns.geometry.dist(1, 2)\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    LHAT_TEST("and a private name still does not cross");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "require^ \"lib/geometry.lh\"\n"
                  "let^ s = ns.geometry.secret\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    check_against_dispose(&u, &lib);

    // 8.7 on the last segment: two units may not claim one path, and one
    // written twice is the same clash.
    LHAT_TEST("and claiming one path twice is a redefinition");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "require^ \"lib/geometry.lh\"\n"
                  "require^ \"lib/geometry.lh\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    check_against_dispose(&u, &lib);

    // 3.2 lets a unit declare no path, and then there is nothing to bind it
    // under.
    LHAT_TEST("and a unit with no module^ cannot be bound this way");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/plain.lh";
    check_against(&u, &lib, "public^ let^ thing = 1\n",
                  "require^ \"lib/plain.lh\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MODULE_UNNAMED);
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

    // 05 の 8.6: L^ is there without being imported, and is not a name -- so
    // 8.1's "nothing is visible" is untouched.
    LHAT_TEST("L^ carries the collector and the registry");
    check_text(&u,
               "L^.collectgarbage()\n"
               "let^ m = L^.modules\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and nothing else");
    check_text(&u, "let^ x = L^.nowhere\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    unit_dispose(&u);

    LHAT_TEST("and collectgarbage takes no argument");
    check_text(&u, "L^.collectgarbage(1)\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_ARITY);
    unit_dispose(&u);

    // 8.6: the registry a unit is loaded into once, grown the way 8.8 grows
    // any table.
    LHAT_TEST("and L^ may be the root of a path");
    check_text(&u,
               "let^ L^.modules.ns1.mod1 = { greet := 1 }\n"
               "let^ L^.modules.ns1.mod2 = { x := 2 }\n"
               "let^ n : number^ = L^.modules.ns1.mod1.greet\n"
               "let^ k : number^ = L^.modules.ns1.mod2.x\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the same path twice is a redefinition");
    check_text(&u,
               "let^ L^.modules.ns1.mod1 = { }\n"
               "let^ L^.modules.ns1.mod1 = { }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    // Only that one spelling is a place; the rest name nothing.
    LHAT_TEST("but another hat identifier is not a root");
    check_text(&u, "let^ true^.x = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 8.8: let^ introduces a member, the way ':=' reassigns one. The tables on
    // the way are made where the path does not reach one yet.
    LHAT_TEST("let^ introduces a member along a path");
    check_text(&u,
               "let^ a.b.c = 1\n"
               "let^ n : number^ = a.b.c\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and two paths through one table meet");
    check_text(&u,
               "let^ a.b.c = 1\n"
               "let^ a.b.d = 2\n"
               "let^ a.z = 3\n"
               "let^ n : number^ = a.b.c + a.b.d + a.z\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an existing table gains the member");
    check_text(&u,
               "let^ t = { p := 1 }\n"
               "let^ t.q = 2\n"
               "let^ n : number^ = t.p + t.q\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // The last segment is the name being introduced, so 8.7 holds for it.
    LHAT_TEST("but writing the last segment twice is a redefinition");
    check_text(&u,
               "let^ a.b = 1\n"
               "let^ a.b = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
    unit_dispose(&u);

    LHAT_TEST("and a segment on the way has to be a table");
    check_text(&u,
               "let^ a = 1\n"
               "let^ a.b = 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_NOT_TABLE);
    unit_dispose(&u);

    // 14 章 fixes what an instance of a def^ carries, which is the one kind
    // of table this cannot add to.
    LHAT_TEST("and a def^ instance takes no new member");
    check_text(&u,
               "let^ P = def^{ self^{ x := 0 } }\n"
               "let^ p = P.new^()\n"
               "let^ p.y = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
    unit_dispose(&u);

    LHAT_TEST("nor does the definition itself");
    check_text(&u,
               "let^ P = def^{ self^{ x := 0 } }\n"
               "let^ P.y = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_PATH_IS_DEFINITION);
    unit_dispose(&u);

    // The root says where the member goes and nothing about itself, so an
    // enclosing binding is reached rather than shadowed.
    LHAT_TEST("and the root is not shadowed");
    check_text(&u,
               "let^ outer = { }\n"
               "let^ add = p^ { let^ outer.k = 7 }\n"
               "add()\n"
               "let^ n : number^ = outer.k\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and an annotation on the last segment is checked");
    check_text(&u,
               "let^ a.b : string^ = 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 15.11: a body that must be a coroutine but has nothing to suspend for.
    // _yield^ says the same thing about the three types as a yield^ does, so
    // the two are the same type -- which is the whole point of it, since
    // 13.9's三つ has to match what the consumer was written against.
    LHAT_TEST("_yield^ gives the same coroutine type a yield^ does");
    check_text(&u,
               "let^ real = p^ -> number^ {\n"
               "  let^ got : string^ = yield^ 1\n"
               "  return^ 9\n"
               "}\n"
               "let^ fake = p^ -> number^ {\n"
               "  let^ got : string^ = _yield^ 1\n"
               "  return^ 9\n"
               "}\n"
               "let^ a : c^{string^, number^, number^} = real()\n"
               "let^ b : c^{string^, number^, number^} = fake()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and a body with only _yield^ is still a coroutine");
    check_text(&u,
               "let^ fake = p^ { _yield^ 1 }\n"
               "let^ c : c^{nil^, number^, nil^} = fake()\n"
               "let^ d : bool^ = c.done()\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // Same node, same rule: 15.8 does not care which of the two made it.
    LHAT_TEST("and dropping what it answers is reported the same way");
    check_text(&u,
               "let^ fake = p^ { _yield^ 1 }\n"
               "fake()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
    unit_dispose(&u);

    LHAT_TEST("and the wrong type is caught the same way");
    check_text(&u,
               "let^ fake = p^ { let^ got : string^ = _yield^ 1 }\n"
               "let^ c : c^{number^, number^, nil^} = fake()\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
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

    // 14.6改: '[ ... ] :=' writes a key that 01 の 6 章 leaves unwritable as
    // a name. A literal one still tells the type what it named.
    LHAT_TEST("an integer key reaches the sequence half");
    check_text(&u,
               "let^ t = { [0] := 7 }\n"
               "let^ s : string^ = t[0]\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // Lua's rule, which 14 章 follows: t.a and t["a"] are one key.
    LHAT_TEST("and a string key is the member of that name");
    check_text(&u,
               "let^ t = { [\"a\"] := 7 }\n"
               "let^ s : string^ = t.a\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 14.10 lets a table carry more than its type lists, so a key only known
    // when it runs leaves the type saying nothing about it.
    LHAT_TEST("a computed key leaves the type quiet");
    check_text(&u,
               "let^ k = 3\n"
               "let^ t = { [k] := 7 }\n"
               "let^ s : string^ = t[3]\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("but the key itself is checked");
    check_text(&u, "let^ t = { [nowhere] := 7 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
    unit_dispose(&u);

    // 04 の 11.3: nil^ spells absence, so it cannot also be a key. A key that
    // can only ever be one is decided here rather than left to the machine.
    LHAT_TEST("a key that can only be nil^ is reported");
    check_text(&u, "let^ t = { [nil^] := 7 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_KEY);
    unit_dispose(&u);

    LHAT_TEST("while one that merely might be is left to run time");
    check_text(&u,
               "let^ x : number^|nil^ = 1\n"
               "let^ t = { [x] := 7 }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // A keyed entry takes no place in the sequence, exactly as a named one
    // does not.
    LHAT_TEST("a computed key takes no position");
    check_text(&u,
               "let^ t = { 10, [\"k\"] := 20, 30 }\n"
               "let^ s : string^ = t[2]\n");
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
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
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

    // 11.2: '..' joins what answers it, and 11.3 settles that here. The
    // machine already refused these; the checker had been silent.
    LHAT_TEST("a number does not answer '..'");
    check_text(&u, "let^ v = 1 .. \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("on either side of it");
    check_text(&u, "let^ v = \"a\" .. 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("nor does a bool^ or a nil^");
    check_text(&u, "let^ v = true^ .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // Every arm has to answer, since the value may be any of them.
    LHAT_TEST("and a union answers only when all of it does");
    check_text(&u,
               "let^ x : string^|nil^ = \"a\"\n"
               "let^ v = x .. \"b\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a string answers, which is 11.2's first example");
    check_text(&u, "let^ v : string^ = \"a\" .. \"b\" .. \"c\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.5: between two definitions '..' composes, and never calls an op^..
    // either of them carries. 14.7 gives a definition and an instance the
    // same members, so what tells them apart is what made them.
    LHAT_TEST("and between two definitions it composes");
    check_text(&u,
               "let^ Foo = def^{ self^{} }\n"
               "let^ Baz = def^{ self^{} }\n"
               "let^ A = Foo .. Baz\n"
               "let^ B = Foo .. def^{ self^{} }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.1: an operator is a function, carried by the type as a member whose
    // name is the operator. 01 の 6 章 keeps that name unwritable by hand.
    LHAT_TEST("an instance answers with the op^ its definition wrote");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ s : string^ = v .. \"a\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and the operator's result is what the join answers");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ n : number^ = v .. \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    unit_dispose(&u);

    // 11.1 again: the right operand is the operator's argument, so what may
    // stand there is what its parameter admits.
    LHAT_TEST("and its parameter is what may stand on the right");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{ x := 0 },\n"
               "  op^.. := f^self^, other:string^ -> string^ { return^ other },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ s = v .. 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("a structure with no op^ does not answer at all");
    check_text(&u,
               "let^ t = { a := 1 }\n"
               "let^ v = t .. t\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.4改: the arithmetic operators ask the same question, so a written
    // op^ answers them too.
    LHAT_TEST("a definition answers arithmetic with its own op^");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, o:number^ -> string^ { return^ \"x\" },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ s : string^ = v + 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("with its parameter deciding the right side");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{ n := 0 },\n"
               "  op^+ := f^self^, o:number^ -> number^ { return^ o },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ n = v + \"a\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 14.8 keeps number^ answering all seven, so ordinary arithmetic is
    // exactly what it was.
    LHAT_TEST("and a number still answers all of them itself");
    check_text(&u,
               "let^ n : number^ = 1 + 2 - 3 * 4 / 5\n"
               "let^ m : number^ = 7 // 2 + 7 % 2 + 2 ** 3\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.8: bool^ carries none, and and^/or^/'!' are the language's own.
    LHAT_TEST("a bool^ answers no operator");
    check_text(&u, "let^ v = true^ + 1\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_OPERATOR);
    unit_dispose(&u);

    // 11.8: an op^ is an f^ taking self^ and one argument. Nothing later
    // checks it -- the call site reads the signature and believes it, and the
    // machine hands over a receiver and one argument whatever was declared --
    // so a shape that is not an operator has to be refused here.
    LHAT_TEST("an op^ without a self^ is not an operator");
    check_text(&u,
               "let^ V = def^{ self^{},\n"
               "  op^.. := f^ o:string^ -> string^ { return^ o } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 11.1: an operator is a function, so a p^ could carry side effects into
    // one.
    LHAT_TEST("nor is a p^");
    check_text(&u,
               "let^ V = def^{ self^{}, op^.. := p^self^, o:string^ { } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("nor anything that is not a subroutine");
    check_text(&u, "let^ V = def^{ self^{}, op^.. := 5 }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.4 leaves exactly one parameter beside the self^: the right operand.
    LHAT_TEST("and it takes the right operand, no more and no less");
    check_text(&u,
               "let^ V = def^{ self^{},\n"
               "  op^.. := f^self^ -> string^ { return^ \"z\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    LHAT_TEST("two arguments is not an operator either");
    check_text(&u,
               "let^ V = def^{ self^{},\n"
               "  op^.. := f^self^, a:string^, b:string^ -> string^ { return^ a } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.12: an overloaded operator is every arm at once, so each has to be
    // an operator on its own.
    LHAT_TEST("every arm of an overloaded operator is checked");
    check_text(&u,
               "let^ V = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o },\n"
               "  overload^ op^.. := f^self^ -> string^ { return^ \"z\" },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_OPERATOR);
    unit_dispose(&u);

    // 14.5: what a composition brings in keeps working, and 14.12's markers
    // reach an operator like any other member.
    LHAT_TEST("a composition carries an operator in");
    check_text(&u,
               "let^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o } }\n"
               "let^ D = B .. def^{ self^{} }\n"
               "let^ s : string^ = D.new^() .. \"x\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("override^ may widen what an operator takes");
    check_text(&u,
               "let^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o } }\n"
               "let^ D = B .. def^{ self^{},\n"
               "  override^ op^.. := f^self^, o:string^|number^ -> string^ {\n"
               "    return^ \"d\" } }\n"
               "let^ s : string^ = D.new^() .. 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and not narrow it");
    check_text(&u,
               "let^ B = def^{ self^{},\n"
               "  op^.. := f^self^, o:string^|number^ -> string^ {\n"
               "    return^ \"b\" } }\n"
               "let^ D = B .. def^{ self^{},\n"
               "  override^ op^.. := f^self^, o:string^ -> string^ {\n"
               "    return^ \"d\" } }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
    unit_dispose(&u);

    // 11.3 判定 is structural, so a '..' member put there by 14.14's computed
    // key answers exactly as a written op^ does.
    LHAT_TEST("a '..' reached by a computed key answers too");
    check_text(&u,
               "let^ t = { [\"..\"] := f^self^, o:string^ -> string^ {\n"
               "  return^ o } }\n"
               "let^ s : string^ = t .. \"x\"\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 11.8: an operator is a member, so 14.12's overload^ is what lets one
    // type answer several right-hand types.
    LHAT_TEST("overload^ gives one operator several right-hand types");
    check_text(&u,
               "let^ Vec = def^{\n"
               "  self^{},\n"
               "  op^.. := f^self^, o:string^ -> string^ { return^ o },\n"
               "  overload^ op^.. := f^self^, o:number^ -> number^ { return^ o },\n"
               "}\n"
               "let^ v = Vec.new^()\n"
               "let^ s : string^ = v .. \"a\"\n"
               "let^ n : number^ = v .. 1\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    // 14.12 asks for a marker on two members of one name whether they came
    // from a base or from the same def^. Only the base had been looked at.
    LHAT_TEST("two members of one name in one def^ need the marker too");
    check_text(&u,
               "let^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  overload^ show := p^self^, x:number^ { },\n"
               "}\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and without one it is still the ordinary mistake");
    check_text(&u,
               "let^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  show := p^self^, x:number^ { },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MEMBER_EXISTS);
    unit_dispose(&u);

    LHAT_TEST("with the overlap rule applying inside one def^ as well");
    check_text(&u,
               "let^ V = def^{\n"
               "  self^{},\n"
               "  show := p^self^, x:string^ { },\n"
               "  overload^ show := p^self^, x:string^ { },\n"
               "}\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
    unit_dispose(&u);

    // 03 の 3.5 turns what is not known into the machine's business.
    LHAT_TEST("a gap in inference says nothing either way");
    check_text(&u, "let^ f = f^ x -> string^ { return^ x .. \"b\" }\n");
    CHECK_CLEAN(&u);
    unit_dispose(&u);

    LHAT_TEST("and 13.7's any^ is every value at once");
    check_text(&u,
               "let^ x : any^ = \"a\"\n"
               "let^ v = x .. \"b\"\n");
    CHECK_CLEAN(&u);
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

// 03 の 4.3: a REPL checks many inputs as one running program, so a name one
// input bound keeps its type in the next.
static void test_session(void)
{
    Unit u;

    LHAT_TEST("a name keeps its type into the next input");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 40\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_next_text(&u, s, "let^ n : number^ = x\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("and the wrong type is caught across inputs");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 40\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ t : string^ = x\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 15.8 refuses a call that makes a coroutine and does nothing with it,
    // because the statement provably has no effect. 03 の 4.3 makes the last
    // statement of an input the answer, and showing the answer is an effect.
    LHAT_TEST("the statement an input answers with may make a coroutine");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_asked_text(&u, s, "let^ count = p^ { yield^ 1 }\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_asked_text(&u, s, "count()\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("but one before the last still drops it");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_asked_text(&u, s, "let^ count = p^ { yield^ 1 }\n");
        unit_dispose(&u);
        check_asked_text(&u, s, "count()\nlet^ k = 1\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("a name never bound is still not in scope");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ n : number^ = nowhere\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 03 の 4.3: at the top level of a session a name written again is the
    // same place written again, not the clash 8.7 makes of two let^ in one
    // scope. A prompt is for writing a line again.
    LHAT_TEST("a name written again is a redefinition, not a clash");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ x : string^ = \"now\"\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("and the newer type is the one it has");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ x : string^ = \"now\"\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ n : number^ = x\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 8.7 still holds within one input.
    LHAT_TEST("but twice in one input is still a clash");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x = 1\nlet^ x = 2\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // Including when the first of the two came from an earlier input: the
    // mark saying "bound elsewhere" is spent by the first let^ that writes
    // the name again.
    LHAT_TEST("and so is one redefinition too many in a single input");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ x = 2\nlet^ x = 3\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 8.7 keeps a name visible before its let^ runs, so a redefinition may
    // read what the name already holds.
    LHAT_TEST("a redefinition may read what is already there");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "let^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "let^ x = x + 10\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }
}

// 03 の 1.3: a code that is about a name says which. The codes stay as they
// are and the diagnostic carries what they cannot.
static void test_named_diagnostics(void)
{
    Unit u;

    LHAT_TEST("a name that is not in scope is named");
    check_text(&u, "let^ x = nowhere\n");
    {
        LHAT_CHECK(u.checked.diagnostic_count > 0, "expected a diagnostic");
        if (u.checked.diagnostic_count > 0) {
            const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
            LHAT_CHECK_EQ_INT(d->code, LHAT_CHECK_ERR_UNDEFINED);
            LHAT_CHECK(d->name != NULL, "and it says which");
            LHAT_CHECK_EQ_INT(d->name_length, 7);

            char message[128];
            size_t needed = lhat_check_message_write(d, message, sizeof message);
            LHAT_CHECK(needed < sizeof message, "it fits");
            LHAT_CHECK(strcmp(message, "no such name in scope: nowhere") == 0,
                       "the message names it");
        }
    }
    unit_dispose(&u);

    LHAT_TEST("and a member that is not there is too");
    check_text(&u, "let^ t = { p = 1 }\nlet^ v = t.missing\n");
    if (u.checked.diagnostic_count > 0) {
        const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
        LHAT_CHECK_EQ_INT(d->code, LHAT_CHECK_ERR_NO_MEMBER);
        char message[128];
        lhat_check_message_write(d, message, sizeof message);
        LHAT_CHECK(strcmp(message,
                          "this value has no such member: missing") == 0,
                   "the member is named");
    }
    unit_dispose(&u);

    LHAT_TEST("and so is one defined twice");
    check_text(&u, "let^ dup = 1\nlet^ dup = 2\n");
    if (u.checked.diagnostic_count > 0) {
        const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
        LHAT_CHECK_EQ_INT(d->code, LHAT_CHECK_ERR_REDEFINED);
        LHAT_CHECK(d->name != NULL && d->name_length == 3, "dup");
    }
    unit_dispose(&u);

    // The name is borrowed from the source, so it has to still say the same
    // thing once the diagnostic has been carried around a little.
    LHAT_TEST("and the borrowed text is the name itself");
    check_text(&u, "let^ x = elsewhere\n");
    if (u.checked.diagnostic_count > 0) {
        const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
        LHAT_CHECK(d->name != NULL, "there is one");
        if (d->name != NULL) {
            LHAT_CHECK(strncmp(d->name, "elsewhere", d->name_length) == 0,
                       "and it points at the word in the source");
        }
    }
    unit_dispose(&u);

    // A code that knows nothing besides itself answers what it always did.
    LHAT_TEST("but one that names nothing keeps its own message");
    check_text(&u, "let^ x : string^ = 1\n");
    if (u.checked.diagnostic_count > 0) {
        const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
        LHAT_CHECK(d->name == NULL, "nothing to name");
        char message[128];
        lhat_check_message_write(d, message, sizeof message);
        LHAT_CHECK(strcmp(message, lhat_check_error_message(d->code)) == 0,
                   "the code's own message");
    }
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
    test_typeof();
    test_variadic();
    test_patterns();
    test_modules();
    test_coroutines();
    test_walking();
    test_positions();
    test_no_value();
    test_session();
    test_named_diagnostics();
    return lhat_test_report("test_check");
}
