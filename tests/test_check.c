// L^ (lhat) -- tests for the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". The cases pinned here are the ones a decision in the
// specification produces and that would otherwise be invisible: the scope
// rule of 8.7, the result inference of 03 の 3.4, and the way catch^, ?? and
// try^ each drop one arm of a union.

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
    lhat_check(u->parsed.root, &u->source, true, &u->checked);
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

    // 3.4: the answer would depend on itself, so it has to be written.
    LHAT_TEST("a self-calling subroutine needs its result written");
    check_text(&u, "let^ f = f^ n:number^ { return^ f(n) }\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_RECURSION_NEEDS_TYPE);
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

int main(void)
{
    test_names();
    test_expressions();
    test_results();
    test_errors();
    test_annotations();
    return lhat_test_report("test_check");
}
