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

#include "fixture.h"

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
    // 05 の 3 章: what the provider declared, which 5.5 binds it under.
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
    memset(&require, 0, sizeof require);
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
        "var^ secret = 1\n"
        "public^ let^ dist = f^ a:number^, b:number^ -> number^ { return^ a }\n";

    // 05 の 4 章: what a unit publishes is read from its declarations, so a
    // require^ of it yields exactly the public^ names.
    LHAT_TEST("public^ names cross and private ones do not");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ g = require^ \"lib/geometry.lh\"\n"
                  "var^ d : number^ = g.dist(1, 2)\n");
    LHAT_CHECK(lib.asked, "the resolver was asked");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    // 05 の 3.1: the path module^ declared is kept, which is what 5.5
    // reads to decide where the short form binds.
    LHAT_TEST("the path module^ declared is recorded");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider, "var^ g = require^ \"lib/geometry.lh\"\n");
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
                  "var^ g = require^ \"lib/plain.lh\"\n");
    LHAT_CHECK(lib.provider.checked.module_name == NULL, "3.2 allows none");
    check_against_dispose(&u, &lib);

    // 05 の 8.6: what require^ answers is the machine's record of what
    // the unit published, not a table the requiring unit adds to.
    LHAT_TEST("a module table is the machine's own");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/plain.lh";
    check_against(&u, &lib, "public^ let^ thing = 1\n",
                  "var^ g = require^ \"lib/plain.lh\"\n"
                  "g.thing := 2\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_TABLE_IS_SEALED);
    check_against_dispose(&u, &lib);

    // 4.2 publishes names. A table one of them holds is as mutable as it was,
    // which is what leaves room for deciding that separately later.
    LHAT_TEST("but what it published stays as writable as it was");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/plain.lh";
    check_against(&u, &lib, "public^ let^ registry = { x := 0 }\n",
                  "var^ g = require^ \"lib/plain.lh\"\n"
                  "g.registry.x := 2\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    // 05 の 5.5: without a var^ the unit goes under the path it declared.
    // 8.8 makes the tables on the way, so only the root is a new name.
    LHAT_TEST("the short form binds under the declared path");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib,
                  provider,
                  "require^ \"lib/geometry.lh\"\n"
                  "var^ d : number^ = ns.geometry.dist(1, 2)\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    LHAT_TEST("and a private name still does not cross");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "require^ \"lib/geometry.lh\"\n"
                  "var^ s = ns.geometry.secret\n");
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
                  "var^ g = require^ \"lib/geometry.lh\"\n"
                  "var^ s = g.secret\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_NO_MEMBER);
    check_against_dispose(&u, &lib);

    // 05 の 6.1: a qualified name works as a type because 04 の 14.4 already
    // made one writable, so the form built for error kinds carries over.
    LHAT_TEST("a required definition is writable as a type");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ g = require^ \"lib/geometry.lh\"\n"
                  "var^ p : g.Point = g.Point.new()\n"
                  "var^ n : number^ = p.x\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    LHAT_TEST("a required error kind is writable as a type");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ g = require^ \"lib/geometry.lh\"\n"
                  "var^ e : g.Bad = error^g.Bad.Degenerate{ }\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);

    // 05 の 6.1: the arguments of a required procedure are checked like any
    // other, which is the point of following the import at all.
    LHAT_TEST("a required procedure checks its arguments");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ g = require^ \"lib/geometry.lh\"\n"
                  "var^ d = g.dist(1, \"text\")\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
    check_against_dispose(&u, &lib);

    // 6.3: a unit that could not be had is reported where it was required.
    LHAT_TEST("a unit that cannot be had is reported");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ g = require^ \"nowhere.lh\"\n");
    CHECK_REPORTS(&u, LHAT_CHECK_ERR_REQUIRE_FAILED);
    check_against_dispose(&u, &lib);

    // 05 の 5.4: require^ binds one name and the importer picks it, so two
    // units of the same shape sit side by side without colliding.
    LHAT_TEST("the importer chooses the name");
    memset(&lib, 0, sizeof lib);
    lib.expected_path = "lib/geometry.lh";
    check_against(&u, &lib, provider,
                  "var^ theirs = require^ \"lib/geometry.lh\"\n"
                  "var^ d : number^ = theirs.dist(1, 2)\n");
    CHECK_CLEAN(&u);
    check_against_dispose(&u, &lib);
}

// 03 の 4.3: a REPL checks many inputs as one running program, so a name one
// input bound keeps its type in the next.
static void test_session(void)
{
    Unit u;

    LHAT_TEST("a name keeps its type into the next input");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 40\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        check_next_text(&u, s, "var^ n : number^ = x\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("and the wrong type is caught across inputs");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 40\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ t : string^ = x\n");
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
        check_asked_text(&u, s, "var^ count = p^ { yield^ 1 }\n");
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
        check_asked_text(&u, s, "var^ count = p^ { yield^ 1 }\n");
        unit_dispose(&u);
        check_asked_text(&u, s, "count()\nvar^ k = 1\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_COROUTINE_DROPPED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("a name never bound is still not in scope");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ n : number^ = nowhere\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_UNDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 03 の 4.3: at the top level of a session a name written again is the
    // same place written again, not the clash 8.7 makes of two var^ in one
    // scope. A prompt is for writing a line again.
    LHAT_TEST("a name written again is a redefinition, not a clash");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ x : string^ = \"now\"\n");
        CHECK_CLEAN(&u);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    LHAT_TEST("and the newer type is the one it has");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ x : string^ = \"now\"\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ n : number^ = x\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 8.7 still holds within one input.
    LHAT_TEST("but twice in one input is still a clash");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x = 1\nvar^ x = 2\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // Including when the first of the two came from an earlier input: the
    // mark saying "bound elsewhere" is spent by the first var^ that writes
    // the name again.
    LHAT_TEST("and so is one redefinition too many in a single input");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ x = 2\nvar^ x = 3\n");
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_REDEFINED);
        unit_dispose(&u);
        lhat_check_session_dispose(s);
    }

    // 8.7 keeps a name visible before its var^ runs, so a redefinition may
    // read what the name already holds.
    LHAT_TEST("a redefinition may read what is already there");
    {
        LhatCheckSession *s = lhat_check_session_new();
        check_next_text(&u, s, "var^ x : number^ = 1\n");
        unit_dispose(&u);
        check_next_text(&u, s, "var^ x = x + 10\n");
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
    check_text(&u, "var^ x = nowhere\n");
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
    check_text(&u, "var^ t = { p = 1 }\nvar^ v = t.missing\n");
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
    check_text(&u, "var^ dup = 1\nvar^ dup = 2\n");
    if (u.checked.diagnostic_count > 0) {
        const LhatCheckDiagnostic *d = &u.checked.diagnostics[0];
        LHAT_CHECK_EQ_INT(d->code, LHAT_CHECK_ERR_REDEFINED);
        LHAT_CHECK(d->name != NULL && d->name_length == 3, "dup");
    }
    unit_dispose(&u);

    // The name is borrowed from the source, so it has to still say the same
    // thing once the diagnostic has been carried around a little.
    LHAT_TEST("and the borrowed text is the name itself");
    check_text(&u, "var^ x = elsewhere\n");
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
    check_text(&u, "var^ x : string^ = 1\n");
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
    test_modules();
    test_session();
    test_named_diagnostics();
    return lhat_test_report("test_check_modules");
}
