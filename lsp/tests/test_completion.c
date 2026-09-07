// L^ (lhat) -- LSP server tests: what may stand where the cursor is
// (completion.c). 07 の 4 章.
//
// The member half is pinned against the one rule the enumeration has: what
// it offers is what lhat_type_find_member would answer with, so a shadowed
// member appears once and a delegate lends only what takes a receiver. A
// second list here would be a second reading of 14.10.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "completion.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

// A standalone unit -- lhat_check takes a lexer and a tree directly, so no
// program.h graph is needed.
static void check_text(Checked *c, const char *text)
{
    lhat_source_init_from_string(&c->source, "test.lh", text, strlen(text));
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse(&c->lexer, &c->parsed);
    lhat_check(c->parsed.root, &c->lexer, true, &c->checked);

    memset(&c->unit, 0, sizeof c->unit);
    c->unit.path = (char *)"test.lh";
    c->unit.loaded = true;
    c->unit.source = c->source;
    c->unit.lexer = c->lexer;
    c->unit.parsed = c->parsed;
    c->unit.checked = c->checked;
}

static void check_dispose(Checked *c)
{
    lhat_check_result_dispose(&c->checked);
    lhat_parse_result_dispose(&c->parsed);
    lhat_lexer_dispose(&c->lexer);
    lhat_source_dispose(&c->source);
}

// The items offered just past the dot of `needle`, which names the access
// by the text it is written with and ends at the dot itself. The LAST
// occurrence is the one meant -- "Reader.new()" holds an "r." of its own,
// and a helper that took the first would ask about a different access than
// the test says it is asking about.
static cJSON *offered_after(Checked *c, const char *needle)
{
    size_t length = strlen(needle);
    LHAT_CHECK(length > 0 && needle[length - 1] == '.',
               "the needle has to end at the dot: \"%s\"", needle);
    const char *found = NULL;
    for (const char *at = strstr(c->source.text, needle); at != NULL;
         at = strstr(at + 1, needle)) {
        found = at;
    }
    LHAT_CHECK(found != NULL, "expected \"%s\" to be in the source", needle);
    if (found == NULL) {
        return cJSON_CreateArray();
    }
    // One past the dot, which is where the cursor stands when it has only
    // now been typed.
    uint32_t past = (uint32_t)(found - c->source.text) + (uint32_t)length;
    return lsp_completion_members_for_unit(&c->unit, past);
}

static bool offers(const cJSON *items, const char *label)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strcmp(at->valuestring, label) == 0) {
            return true;
        }
    }
    return false;
}

static int times_offered(const cJSON *items, const char *label)
{
    int seen = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strcmp(at->valuestring, label) == 0) {
            seen++;
        }
    }
    return seen;
}

static int kind_of(const cJSON *items, const char *label)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strcmp(at->valuestring, label) == 0) {
            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
            return cJSON_IsNumber(kind) ? kind->valueint : -1;
        }
    }
    return -1;
}

static void expect_offers(const cJSON *items, const char *label, bool expected)
{
    LHAT_CHECK(offers(items, label) == expected, "expected %s to be %s",
               label, expected ? "offered" : "left out");
}

// LSP's CompletionItemKind numbers, the ones used.
enum {
    METHOD = 2, FUNCTION = 3, FIELD = 5, VARIABLE = 6, CLASS = 7, MODULE = 9,
    KEYWORD = 14, UNIT_FILE = 17, CONSTANT = 21,
};

static void test_a_dot_with_nothing_after_it(void)
{
    Checked c;

    // The case the whole design exists for: nothing is written after the
    // dot, so there is no name anything could be looked up by.
    LHAT_TEST("07 の 4 章: a bare dot offers the receiver's members");
    check_text(&c,
               "let^ Reader = def^{\n"
               "    self^{ at = 1, text = \"\" },\n"
               "    peek = f^self^ -> number^ { return^ self^.at },\n"
               "}\n"
               "let^ r = Reader.new()\n"
               "let^ n = r.\n");
    {
        cJSON *items = offered_after(&c, "r.");
        expect_offers(items, "at", true);
        expect_offers(items, "text", true);
        expect_offers(items, "peek", true);
        // 14.11: new belongs to the definition, not to what it makes.
        expect_offers(items, "new", false);
        LHAT_CHECK_EQ_INT(kind_of(items, "peek"), METHOD);
        LHAT_CHECK_EQ_INT(kind_of(items, "at"), FIELD);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("and the definition itself offers what it holds");
    check_text(&c,
               "let^ Reader = def^{\n"
               "    self^{ at = 1 },\n"
               "    peek = f^self^ -> number^ { return^ self^.at },\n"
               "}\n"
               "let^ n = Reader.\n");
    {
        cJSON *items = offered_after(&c, "Reader.");
        expect_offers(items, "new", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("a table literal offers what was written in it");
    check_text(&c,
               "let^ point = { x = 1, y = 2 }\n"
               "let^ n = point.\n");
    {
        cJSON *items = offered_after(&c, "point.");
        expect_offers(items, "x", true);
        expect_offers(items, "y", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // 8.2 refuses this statement outright and the writer will see that, but
    // the run is kept under the refusal and walked for the record
    // (parser.c's `refused`, check_stmt.c's ERROR arm). Without that, a
    // writer one keystroke into a member name -- which is where a request
    // made by hand rather than by the dot arrives -- would be answered with
    // nothing at all.
    LHAT_TEST("and a statement the parser refused still answers");
    check_text(&c,
               "let^ Reader = def^{\n"
               "    self^{ at = 1, text = \"\" },\n"
               "    peek = f^self^ -> number^ { return^ self^.at },\n"
               "}\n"
               "let^ r = Reader.new()\n"
               "r.pe\n");
    {
        cJSON *items = offered_after(&c, "r.");
        expect_offers(items, "peek", true);
        expect_offers(items, "at", true);
        // The built-ins come through the same walk, so they are there too.
        expect_offers(items, "tostring", true);
        cJSON_Delete(items);
    }
    // A cursor at the end of the half-written name finds the same site --
    // LhatMemberSite's end reaches one past the name.
    {
        const char *at = strstr(c.source.text, "r.pe");
        LHAT_CHECK(at != NULL, "expected the refused access");
        if (at != NULL) {
            cJSON *items = lsp_completion_members_for_unit(
                &c.unit, (uint32_t)(at - c.source.text) + 4);
            expect_offers(items, "peek", true);
            cJSON_Delete(items);
        }
    }
    check_dispose(&c);
}

// A receiver that is not a name at all. Reading the resolutions could not
// answer these: the table is keyed by a name's span, and there is no name
// here whose type is the receiver's.
static void test_a_receiver_that_is_not_a_name(void)
{
    Checked c;

    LHAT_TEST("what a call answers is a receiver like any other");
    check_text(&c,
               "let^ make = f^ -> t^{ x : number^ } { return^ { x = 1 } }\n"
               "let^ n = make().\n");
    {
        cJSON *items = offered_after(&c, "make().");
        expect_offers(items, "x", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("and so is what an index answers");
    check_text(&c,
               "let^ rows = { { x = 1 } }\n"
               "let^ n = rows[1].\n");
    {
        cJSON *items = offered_after(&c, "rows[1].");
        expect_offers(items, "x", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // The one that would be silently wrong if the receiver were guessed from
    // the nearest name: 'b' is a number, and the sum is what the dot is on.
    LHAT_TEST("an expression's own type is what is asked, not the last name's");
    check_text(&c,
               "let^ a = \"x\"\n"
               "let^ b = \"y\"\n"
               "let^ n = (a .. b).\n");
    {
        cJSON *items = offered_after(&c, "(a .. b).");
        // 11.2: what the concatenation answers is a string, and a string's
        // members are the checker's own (14.19). The property is that they
        // are the STRING's -- reading the nearest name would have given b's,
        // which is the same type here only by luck. `x` is what a table
        // would have offered, and no table stands anywhere near this dot.
        expect_offers(items, "length", true);
        expect_offers(items, "split", true);
        expect_offers(items, "x", false);
        cJSON_Delete(items);
    }
    check_dispose(&c);
}

// 14.5 composes two definitions and 14.12 marks what replaces what. The
// enumeration asks the lookup which member wins, so an override is one item.
static void test_what_the_lookup_would_answer(void)
{
    Checked c;

    LHAT_TEST("14.12: an overridden member is offered once, as the override");
    check_text(&c,
               "let^ Base = def^{\n"
               "    self^{ n = 0 },\n"
               "    speak = f^self^ -> string^ { return^ \"base\" },\n"
               "}\n"
               "let^ Derived = Base..def^{\n"
               "    override^speak = f^self^ -> string^ { return^ \"derived\" },\n"
               "}\n"
               "let^ d = Derived.new()\n"
               "let^ s = d.\n");
    {
        cJSON *items = offered_after(&c, "d.");
        LHAT_CHECK_EQ_INT(times_offered(items, "speak"), 1);
        // 14.5: what the base wrote and the derived did not is still there.
        expect_offers(items, "n", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // 14.7改2: a delegate lends what an instance may reach through it, which
    // is what takes a receiver -- and nothing else.
    LHAT_TEST("14.7改2: a delegate lends only what takes a receiver");
    check_text(&c,
               "let^ Held = def^{\n"
               "    self^{ n = 0 },\n"
               "    speak = f^self^ -> string^ { return^ \"held\" },\n"
               "}\n"
               "let^ Wrap = def^{\n"
               "    self^{ inner = Held.new() },\n"
               "    delegate^ self^.inner,\n"
               "}\n"
               "let^ w = Wrap.new()\n"
               "let^ s = w.\n");
    {
        cJSON *items = offered_after(&c, "w.");
        expect_offers(items, "speak", true);   // lent: it takes a receiver
        expect_offers(items, "inner", true);   // the wrapper's own field
        cJSON_Delete(items);
    }
    check_dispose(&c);
}

// 14.19 with 15.6改: the members no list holds. The completion asks the
// checker for each spelling it knows of, so which receiver takes which is
// never written down here -- these pin that the answer tracks the cascade.
static void test_the_built_ins(void)
{
    Checked c;

    LHAT_TEST("14.19: a string offers what the checker answers for one");
    check_text(&c, "let^ s = \"text\"\nlet^ n = s.\n");
    {
        cJSON *items = offered_after(&c, "s.");
        expect_offers(items, "length", true);
        expect_offers(items, "at", true);
        expect_offers(items, "split", true);
        expect_offers(items, "replace", true);
        expect_offers(items, "tonumber", true);
        // 14.19: a string is not a value a writer adds names to, so no hat.
        expect_offers(items, "length^", false);
        // and it is not a collection, so the table's counting word is not its
        expect_offers(items, "count", false);
        expect_offers(items, "push^", false);
        LHAT_CHECK_EQ_INT(kind_of(items, "at"), METHOD);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("14.10改: a plain table offers the hatted spellings");
    check_text(&c, "let^ row = { 1, 2 }\nlet^ n = row.\n");
    {
        cJSON *items = offered_after(&c, "row.");
        expect_offers(items, "push^", true);
        expect_offers(items, "length^", true);
        expect_offers(items, "iterate^", true);
        expect_offers(items, "keys^", true);
        // On a plain table the bare spelling stays the writer's.
        expect_offers(items, "push", false);
        expect_offers(items, "iterate", false);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // 02 の 14.8改2: number^ is a word rather than a value, and the
    // constants are the whole of what stands after its dot.
    LHAT_TEST("02 の 14.8改2: number^ offers its constants and nothing else");
    check_text(&c, "let^ n = number^.\n");
    {
        cJSON *items = offered_after(&c, "number^.");
        expect_offers(items, "pi", true);
        expect_offers(items, "tau", true);
        expect_offers(items, "e", true);
        expect_offers(items, "inf", true);
        expect_offers(items, "nan", true);
        // Not the members of a number: that is a different receiver.
        expect_offers(items, "floor", false);
        expect_offers(items, "tostring", false);
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 5);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("15.6改: a coroutine offers what advances it");
    check_text(&c,
               "let^ made = f^ -> c^{ f^ -> number^ } {\n"
               "    return^ c^{ f^ -> number^ } { yield^ 1 }\n"
               "}\n"
               "let^ walk = made()\n"
               "let^ n = walk.\n");
    {
        cJSON *items = offered_after(&c, "walk.");
        expect_offers(items, "resume", true);
        expect_offers(items, "done", true);
        expect_offers(items, "dispose", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // 14.17改: a written member wins, and the two spellings are one member
    // off a plain table -- so the built-in must not show up beside it.
    LHAT_TEST("14.17改: a written member is not doubled by its built-in");
    check_text(&c,
               "let^ Held = def^{\n"
               "    self^{ n = 0 },\n"
               "    tostring = f^self^ -> string^ { return^ \"held\" },\n"
               "}\n"
               "let^ h = Held.new()\n"
               "let^ s = h.\n");
    {
        cJSON *items = offered_after(&c, "h.");
        LHAT_CHECK_EQ_INT(times_offered(items, "tostring"), 1);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // The def^ side of 14.17改: the bare spelling is not the writer's there,
    // so both answer and both are offered.
    LHAT_TEST("but a def^'s instance takes either spelling of iterate");
    check_text(&c,
               "let^ Held = def^{ self^{ n = 0 } }\n"
               "let^ h = Held.new()\n"
               "let^ s = h.\n");
    {
        cJSON *items = offered_after(&c, "h.");
        expect_offers(items, "iterate", true);
        expect_offers(items, "iterate^", true);
        // 14.10改: the sequence operations belong to a plain table alone.
        expect_offers(items, "push^", false);
        cJSON_Delete(items);
    }
    check_dispose(&c);
}

static void test_what_is_never_offered(void)
{
    Checked c;

    // 14.10改: a sequence's members are named by their positions, and 01 の
    // 3.1 spells a name as an identifier -- so "1" is a spelling the
    // language could never accept where a member name goes.
    LHAT_TEST("14.10改: a position is not a name, so it is not offered");
    check_text(&c,
               "let^ row = { 10, 20, 30 }\n"
               "let^ n = row.\n");
    {
        cJSON *items = offered_after(&c, "row.");
        expect_offers(items, "1", false);
        expect_offers(items, "2", false);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    // 03 の 3.1: what nothing decided answers nothing -- and that has to
    // hold for the built-ins too, which would otherwise reach the tail where
    // tostring answers for every type.
    LHAT_TEST("a receiver nothing decided answers an empty list");
    check_text(&c, "let^ n = nowhere.\n");
    {
        cJSON *items = offered_after(&c, "nowhere.");
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 0);
        cJSON_Delete(items);
    }
    check_dispose(&c);

    LHAT_TEST("and a position that is on no dot at all answers nothing");
    check_text(&c, "let^ point = { x = 1 }\n");
    {
        cJSON *items = lsp_completion_members_for_unit(&c.unit, 3);
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 0);
        cJSON_Delete(items);
        items = lsp_completion_members_for_unit(&c.unit, 100000);
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 0);
        cJSON_Delete(items);
    }
    check_dispose(&c);
}

// 11.7改2: '?.' steps past the nil^ arm, so what is offered is what the
// access would accept -- not nothing, which is what the union alone answers.
static void test_a_guarded_dot(void)
{
    Checked c;

    LHAT_TEST("11.7改2: '?.' offers the members past the nil^ arm");
    check_text(&c,
               "let^ hold = f^ v:t^{ x : number^ }|nil^ {\n"
               "    return^ v?.\n"
               "}\n");
    {
        cJSON *items = offered_after(&c, "v?.");
        expect_offers(items, "x", true);
        cJSON_Delete(items);
    }
    check_dispose(&c);
}

// ---------------------------------------------------------------------------
// The two read off the text
// ---------------------------------------------------------------------------

static void expect_import(const char *text, bool expected,
                          const char *prefix_expected)
{
    uint32_t from = 0;
    bool found = lsp_completion_import_prefix(text, strlen(text),
                                              (uint32_t)strlen(text), &from);
    LHAT_CHECK(found == expected, "\"%s\": expected %s", text,
               expected ? "an import path" : "none");
    if (found && expected && prefix_expected != NULL) {
        LHAT_CHECK(strncmp(text + from, prefix_expected,
                           strlen(prefix_expected)) == 0,
                   "\"%s\": expected the path to begin \"%s\", got \"%s\"",
                   text, prefix_expected, text + from);
    }
}

static void test_reading_an_import(void)
{
    LHAT_TEST("05 の 8.7: the cursor is in an import^ path");
    expect_import("import^ std.", true, "std.");
    expect_import("import^ std.math.", true, "std.math.");
    expect_import("import^ ", true, "");
    expect_import("let^ io = import^ std.", true, "std.");

    LHAT_TEST("and is not, where no import^ stands before it");
    expect_import("let^ x = point.", false, NULL);
    expect_import("x.", false, NULL);
    // 01 の 6.1: after a '#' the line is prose.
    expect_import("# import^ std.", false, NULL);
    expect_import("let^ x = 1  # see import^ std.", false, NULL);

    LHAT_TEST("05 の 5 章: and the same for a require^'s string");
    {
        uint32_t from = 0;
        static const char *const line = "let^ u = require^ \"lib/";
        LHAT_CHECK(lsp_completion_require_prefix(line, strlen(line),
                                                 (uint32_t)strlen(line), &from),
                   "expected a require^ path");
        LHAT_CHECK(strcmp(line + from, "lib/") == 0, "got \"%s\"", line + from);

        static const char *const other = "let^ x = \"lib/";
        LHAT_CHECK(!lsp_completion_require_prefix(other, strlen(other),
                                                  (uint32_t)strlen(other),
                                                  &from),
                   "a string that no require^ opened is not a path");
    }
}

// ---------------------------------------------------------------------------
// The words of the language
// ---------------------------------------------------------------------------

// Whether a word may be written at the end of `text`, and what of it stands
// there already.
static void expect_word(const char *text, bool expected, const char *prefix)
{
    uint32_t from = 0;
    bool found = lsp_completion_word_prefix(text, strlen(text),
                                            (uint32_t)strlen(text), &from);
    LHAT_CHECK(found == expected, "\"%s\": expected %s", text,
               expected ? "a word" : "none");
    if (found && expected && prefix != NULL) {
        LHAT_CHECK(strcmp(text + from, prefix) == 0,
                   "\"%s\": expected the word to begin \"%s\", got \"%s\"",
                   text, prefix, text + from);
    }
}

static void test_reading_a_word(void)
{
    LHAT_TEST("a word being written is one, however much of it there is");
    expect_word("le", true, "le");
    expect_word("let^", true, "let^");
    expect_word("let^ x = 1\nre", true, "re");
    expect_word("x1", true, "x1");
    // Nothing typed yet: the whole list stands there, which is what an
    // editor asked outright (rather than by a keystroke) wants.
    expect_word("", true, "");
    expect_word("let^ ", true, "");

    LHAT_TEST("14.10: but a member name is the receiver's to answer");
    expect_word("foo.", false, NULL);
    expect_word("foo.le", false, NULL);
    // 11.7改2: the guarded dot ends in the same byte.
    expect_word("foo?.le", false, NULL);

    LHAT_TEST("01 の 6.1 with 4 章: and neither prose nor text is code");
    expect_word("# le", false, NULL);
    expect_word("let^ x = 1  # wh", false, NULL);
    expect_word("let^ s = \"hello wo", false, NULL);
    // The string closed, so what follows it is code again.
    expect_word("let^ s = \"a\" ; wh", true, "wh");
    // And a quote the string escaped did not close it.
    expect_word("let^ s = \"a\\\" wh", false, NULL);

    LHAT_TEST("01 の 3.1: a number is not a word half written");
    expect_word("let^ x = 1e", false, NULL);
    expect_word("1", false, NULL);
}

static void test_the_words_offered(void)
{
    cJSON *items = lsp_completion_word_items();
    LHAT_CHECK(items != NULL, "expected a list of words");

    LHAT_TEST("the words of each part of the language are offered");
    expect_offers(items, "let^", true);
    expect_offers(items, "if^", true);
    expect_offers(items, "for^", true);
    expect_offers(items, "downto^", true);  // 16.3's clauses, not only its head
    expect_offers(items, "f^", true);
    expect_offers(items, "def^", true);
    expect_offers(items, "errordef^", true);
    expect_offers(items, "await^", true);
    expect_offers(items, "fits^", true);
    expect_offers(items, "override^", true);

    LHAT_TEST("01 の 2.3: with the hat, since the hat is part of the name");
    expect_offers(items, "let", false);
    expect_offers(items, "if", false);

    LHAT_TEST("and a spelling the language dropped is not offered");
    // 14.4改: the word for a definition is def^, and class^ was not kept
    // even as another spelling of it.
    expect_offers(items, "class^", false);
    // 15.6改: the maker is new, so new^ is a member and not a word.
    expect_offers(items, "new^", false);

    LHAT_TEST("each word once, so the editor's filter has one of each");
    LHAT_CHECK_EQ_INT(times_offered(items, "let^"), 1);
    LHAT_CHECK_EQ_INT(times_offered(items, "error^"), 1);
    LHAT_CHECK_EQ_INT(times_offered(items, "finally^"), 1);

    LHAT_TEST("and is drawn as what it is");
    LHAT_CHECK_EQ_INT(kind_of(items, "let^"), KEYWORD);
    LHAT_CHECK_EQ_INT(kind_of(items, "number^"), CLASS);
    LHAT_CHECK_EQ_INT(kind_of(items, "Self^"), CLASS);
    LHAT_CHECK_EQ_INT(kind_of(items, "nil^"), CONSTANT);
    LHAT_CHECK_EQ_INT(kind_of(items, "true^"), CONSTANT);
    // 05 の 8.6 with 8.1: no scope holds these, so nothing else offers them.
    LHAT_CHECK_EQ_INT(kind_of(items, "self^"), VARIABLE);
    LHAT_CHECK_EQ_INT(kind_of(items, "L^"), VARIABLE);
    LHAT_CHECK_EQ_INT(kind_of(items, "_^"), VARIABLE);

    cJSON_Delete(items);
}

static void test_module_items(void)
{
    static const char *const modules[] = {
        "std", "std.io", "std.math", "std.math.vector3", "std.thread", "app",
    };
    size_t count = sizeof modules / sizeof modules[0];

    LHAT_TEST("a module path offers the next segment, once each");
    {
        cJSON *items = lsp_completion_module_items(modules, count, "std.", 4);
        expect_offers(items, "io", true);
        expect_offers(items, "thread", true);
        // One segment at a time: vector3 comes when std.math. is written.
        LHAT_CHECK_EQ_INT(times_offered(items, "math"), 1);
        expect_offers(items, "vector3", false);
        expect_offers(items, "std.io", false);
        LHAT_CHECK_EQ_INT(kind_of(items, "io"), MODULE);
        cJSON_Delete(items);
    }
    {
        cJSON *items = lsp_completion_module_items(modules, count, "", 0);
        expect_offers(items, "std", true);
        expect_offers(items, "app", true);
        LHAT_CHECK_EQ_INT(times_offered(items, "std"), 1);
        cJSON_Delete(items);
    }
    {
        cJSON *items = lsp_completion_module_items(modules, count,
                                                   "std.math.", 9);
        expect_offers(items, "vector3", true);
        cJSON_Delete(items);
    }
}

static void expect_relative(const char *from, const char *target,
                            const char *expected)
{
    char *written = lsp_completion_relative_path(from, target);
    LHAT_CHECK(written != NULL && strcmp(written, expected) == 0,
               "from %s to %s: expected %s, got %s", from, target, expected,
               written != NULL ? written : "(nothing)");
    free(written);
}

static void test_relative_paths(void)
{
    LHAT_TEST("05 の 5 章: a path is written from the requiring unit's own "
              "directory");
    expect_relative("c:/w/main.lh", "c:/w/util.lh", "util.lh");
    expect_relative("c:/w/main.lh", "c:/w/lib/util.lh", "lib/util.lh");
    expect_relative("c:/w/lib/a.lh", "c:/w/main.lh", "../main.lh");
    expect_relative("c:/w/lib/deep/a.lh", "c:/w/main.lh", "../../main.lh");
    expect_relative("c:/w/lib/a.lh", "c:/w/other/b.lh", "../other/b.lh");
    // A shared prefix is only shared up to a whole segment.
    expect_relative("c:/w/lib/a.lh", "c:/w/library/b.lh", "../library/b.lh");

    LHAT_TEST("and a unit is not offered its own path");
    {
        static const char *const candidates[] = {
            "c:/w/main.lh", "c:/w/lib/util.lh",
        };
        cJSON *items = lsp_completion_path_items("c:/w/main.lh", candidates, 2);
        expect_offers(items, "lib/util.lh", true);
        expect_offers(items, "main.lh", false);
        LHAT_CHECK_EQ_INT(kind_of(items, "lib/util.lh"), UNIT_FILE);
        cJSON_Delete(items);
    }
}

int main(void)
{
    test_a_dot_with_nothing_after_it();
    test_a_receiver_that_is_not_a_name();
    test_what_the_lookup_would_answer();
    test_the_built_ins();
    test_what_is_never_offered();
    test_a_guarded_dot();
    test_reading_an_import();
    test_reading_a_word();
    test_the_words_offered();
    test_module_items();
    test_relative_paths();
    return lhat_test_report("test_completion");
}
