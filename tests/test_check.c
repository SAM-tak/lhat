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
    return lhat_test_report("test_check");
}
