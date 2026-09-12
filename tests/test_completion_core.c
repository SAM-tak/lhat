// L^ (lhat) -- what may stand where the cursor is (include/lhat/completion.h).
//
// 07 の 4 章. The two questions the checker owns are pinned here, against the
// one rule each has: what a member access offers is what lhat_type_find_member
// would answer with, and what a word offers is what 8 章's lookup would have
// found. A second list in this file would be a second reading of those, which
// is the thing the header exists to refuse.

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/completion.h"
#include "lhat/lexer.h"
#include "lhat/value.h"
#include "lhat/source.h"
#include "parser.h"
#include "program_internal.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

// A standalone unit -- lhat_check takes a lexer and a tree directly, so no
// program.h graph is needed to ask either question.
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

// Just past `needle`, which is written where the cursor should stand.
static uint32_t after(const Checked *c, const char *needle)
{
    const char *at = strstr(c->unit.source.text, needle);
    return at == NULL ? 0
                      : (uint32_t)(at - c->unit.source.text + strlen(needle));
}

// Every item at `offset`, measured first and then filled -- which is also
// what pins the header's bargain about a count larger than the capacity.
static size_t items_at(const Checked *c, uint32_t offset,
                       LhatCompletionItem **into)
{
    size_t wanted = lhat_unit_completion_items(&c->unit, offset, NULL, 0);
    *into = wanted > 0
                ? (LhatCompletionItem *)calloc(wanted, sizeof **into)
                : NULL;
    if (*into == NULL) {
        return wanted;
    }
    size_t filled = lhat_unit_completion_items(&c->unit, offset, *into, wanted);
    LHAT_CHECK_EQ_INT(filled, wanted);
    return filled;
}

static bool offers(const LhatCompletionItem *items, size_t count,
                   const char *label)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].label, label) == 0) {
            return true;
        }
    }
    return false;
}

static const LhatCompletionItem *find(const LhatCompletionItem *items,
                                      size_t count, const char *label)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].label, label) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Which of the four the cursor is asking

static void test_which_question(void)
{
    Checked c;

    LHAT_TEST("a dot before the cursor is the member question");
    {
        check_text(&c, "let^ t = { x = 1 }\nlet^ n = t.\n");
        uint32_t from = 0;
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit, after(&c, "t."),
                                                   &from),
                          LHAT_COMPLETION_MEMBER);
        check_dispose(&c);
    }

    LHAT_TEST("a word being written is the word question, and says where");
    {
        check_text(&c, "let^ answer = 1\nlet^ n = ans\n");
        uint32_t from = 0;
        uint32_t at = after(&c, "= ans");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit, at, &from),
                          LHAT_COMPLETION_WORD);
        // Where the word began, so an item replacing it replaces all of it.
        LHAT_CHECK_EQ_INT(from, at - 3);
        check_dispose(&c);
    }

    LHAT_TEST("an import^ path is the module question");
    {
        check_text(&c, "import^ std.\n");
        uint32_t from = 0;
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit,
                                                   after(&c, "std."), &from),
                          LHAT_COMPLETION_MODULE);
        check_dispose(&c);
    }

    // 01 の 2.3: the hat is part of a name, so a path written straight after
    // the word is one run of name bytes -- which is how this repository
    // writes it, and how it read as a name being typed.
    LHAT_TEST("import^ takes the path with no space between");
    {
        check_text(&c, "import^godot\n");
        uint32_t from = 0;
        uint32_t at = after(&c, "import^godot");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit, at, &from),
                          LHAT_COMPLETION_MODULE);
        LHAT_CHECK_EQ_INT(from, at - 5);  // just past the word
        check_dispose(&c);
    }

    LHAT_TEST("and with a dot after it, still the module question");
    {
        check_text(&c, "import^godot.\n");
        uint32_t from = 0;
        uint32_t at = after(&c, "import^godot.");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit, at, &from),
                          LHAT_COMPLETION_MODULE);
        LHAT_CHECK_EQ_INT(from, at - 6);
        check_dispose(&c);
    }

    LHAT_TEST("the word alone, with nothing typed after it");
    {
        check_text(&c, "import^\n");
        uint32_t from = 0;
        uint32_t at = after(&c, "import^");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit, at, &from),
                          LHAT_COMPLETION_MODULE);
        LHAT_CHECK_EQ_INT(from, at);
        check_dispose(&c);
    }

    // A name that merely ends the same way is a name.
    LHAT_TEST("a word that is not import^ is not the module question");
    {
        check_text(&c, "let^ notimport^x = 1\n"
                   "let^ n = notimport^x\n");
        LHAT_CHECK(lhat_unit_completion_ask(&c.unit,
                                            after(&c, "= notimport^x"), NULL)
                       != LHAT_COMPLETION_MODULE,
                   "not the module question");
        check_dispose(&c);
    }

    LHAT_TEST("a require^ string is the unit question");
    {
        check_text(&c, "require^ \"lib/\n");
        uint32_t from = 0;
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit,
                                                   after(&c, "lib/"), &from),
                          LHAT_COMPLETION_UNIT);
        check_dispose(&c);
    }

    LHAT_TEST("a comment asks nothing");
    {
        check_text(&c, "# a note about ans\nlet^ answer = 1\n");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit,
                                                   after(&c, "about ans"),
                                                   NULL),
                          LHAT_COMPLETION_NOTHING);
        check_dispose(&c);
    }

    LHAT_TEST("a string asks nothing");
    {
        check_text(&c, "let^ s = \"ans\"\n");
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(&c.unit,
                                                   after(&c, "\"ans"), NULL),
                          LHAT_COMPLETION_NOTHING);
        check_dispose(&c);
    }

    LHAT_TEST("no unit asks nothing, and says the cursor is where it was");
    {
        uint32_t from = 7;
        LHAT_CHECK_EQ_INT(lhat_unit_completion_ask(NULL, 3, &from),
                          LHAT_COMPLETION_NOTHING);
        LHAT_CHECK_EQ_INT(from, 3);
    }
}

// ---------------------------------------------------------------------------
// The members of a receiver

static void test_members(void)
{
    Checked c;
    LhatCompletionItem *items = NULL;

    LHAT_TEST("a table offers what it holds, with the type written out");
    {
        check_text(&c, "let^ t = { x = 1, name = \"a\" }\nlet^ n = t.\n");
        size_t count = items_at(&c, after(&c, "t."), &items);
        LHAT_CHECK(offers(items, count, "x"), "the number field");
        LHAT_CHECK(offers(items, count, "name"), "the string field");
        const LhatCompletionItem *x = find(items, count, "x");
        LHAT_CHECK(x != NULL && strstr(x->detail, "number") != NULL,
                   "the type came with it: %s",
                   x != NULL ? x->detail : "(nothing)");
        LHAT_CHECK_EQ_INT(x != NULL ? (int)x->kind : -1,
                          LHAT_COMPLETION_FIELD);
        free(items);
        check_dispose(&c);
    }

    // 02 の 14.19: no list holds these; the checker answers them, and the
    // header's whole argument is that a host cannot ask any other way.
    LHAT_TEST("a string offers the built-ins the checker answers");
    {
        check_text(&c, "let^ s = \"ab\"\nlet^ n = s.\n");
        size_t count = items_at(&c, after(&c, "s."), &items);
        LHAT_CHECK(offers(items, count, "length"), "length is one of them");
        LHAT_CHECK(offers(items, count, "toupper"), "and so is toupper");
        free(items);
        check_dispose(&c);
    }

    // 14.10 with 13.4: what takes a receiver is written as a call on the
    // value, and an editor draws those apart from fields.
    LHAT_TEST("a member that takes a receiver reads as a method");
    {
        check_text(&c,
                   "let^ D = def^{ self^{ x : number^ },\n"
                   "               get = f^self^ -> number^ { return^ 1 } }\n"
                   "let^ d = D.new()\nlet^ n = d.\n");
        size_t count = items_at(&c, after(&c, "d."), &items);
        const LhatCompletionItem *got = find(items, count, "get");
        LHAT_CHECK(got != NULL && got->kind == LHAT_COMPLETION_METHOD,
                   "get is a method");
        free(items);
        check_dispose(&c);
    }

    // Where no dot stands the question is the other one, so what comes back
    // is the words and the names rather than a receiver's members.
    LHAT_TEST("without a dot the answer is the word question's");
    {
        check_text(&c, "let^ t = { x = 1 }\n");
        size_t count = items_at(&c, 0, &items);
        LHAT_CHECK(count > 0, "the words are there");
        LHAT_CHECK(!offers(items, count, "x"), "and no member of t is");
        free(items);
        check_dispose(&c);
    }
}

// ---------------------------------------------------------------------------
// The names in scope, and the words

static void test_words(void)
{
    Checked c;
    LhatCompletionItem *items = NULL;

    LHAT_TEST("a name in scope is offered, with what it holds");
    {
        check_text(&c, "let^ answer = 42\nlet^ n = a\n");
        size_t count = items_at(&c, after(&c, "= a"), &items);
        const LhatCompletionItem *a = find(items, count, "answer");
        LHAT_CHECK(a != NULL, "the binding is there");
        LHAT_CHECK(a != NULL && strstr(a->detail, "number") != NULL,
                   "with its type: %s", a != NULL ? a->detail : "(nothing)");
        free(items);
        check_dispose(&c);
    }

    LHAT_TEST("the words of the language come with it");
    {
        check_text(&c, "let^ answer = 42\nlet^ n = a\n");
        size_t count = items_at(&c, after(&c, "= a"), &items);
        LHAT_CHECK(offers(items, count, "if^"), "a word that binds nothing");
        LHAT_CHECK(offers(items, count, "number^"), "a type needing no name");
        LHAT_CHECK(offers(items, count, "nil^"), "a value no binding holds");
        free(items);
        check_dispose(&c);
    }

    // 8 章: the first of a spelling is what the lookup would have found, and
    // one list holds one of each.
    LHAT_TEST("a name written twice is offered once");
    {
        check_text(&c,
                   "let^ same = 1\n"
                   "do^{ let^ same = \"two\"\n     let^ n = s\n}\n");
        size_t count = items_at(&c, after(&c, "= s"), &items);
        size_t seen = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(items[i].label, "same") == 0) {
                seen++;
            }
        }
        LHAT_CHECK_EQ_INT(seen, 1);
        // The inner one shadows, so it is the one that answered.
        const LhatCompletionItem *s = find(items, count, "same");
        LHAT_CHECK(s != NULL && strstr(s->detail, "string") != NULL,
                   "the innermost won: %s", s != NULL ? s->detail : "(none)");
        free(items);
        check_dispose(&c);
    }

    // The bargain the header states, and the one thing a host has to get
    // right: a count larger than the capacity says to come back with room.
    LHAT_TEST("a small array is filled as far as it goes and the rest counted");
    {
        check_text(&c, "let^ answer = 42\nlet^ n = a\n");
        uint32_t at = after(&c, "= a");
        size_t whole = lhat_unit_completion_items(&c.unit, at, NULL, 0);
        LHAT_CHECK(whole > 3, "there is more than room for: %zu", whole);
        LhatCompletionItem few[3];
        memset(few, 0, sizeof few);
        size_t said = lhat_unit_completion_items(&c.unit, at, few, 3);
        LHAT_CHECK_EQ_INT(said, whole);
        LHAT_CHECK(few[0].label[0] != '\0', "and the room was used");
        check_dispose(&c);
    }
}


// ---------------------------------------------------------------------------
// What a registered receiver answers, remembered (07 の 4 章)

typedef struct {
    const char *path;
    const char *text;
} File;

static char *disk_load(void *context, const char *path, size_t *length)
{
    const File *file = (const File *)context;
    if (strcmp(file->path, path) != 0) {
        return NULL;
    }
    size_t size = strlen(file->text);
    char *copy = (char *)malloc(size + 1);
    if (copy != NULL) {
        memcpy(copy, file->text, size + 1);
        *length = size;
    }
    return copy;
}

// Never called: nothing here runs, it only checks.
static void stub(struct LhatMachine *machine, void *context,
                 const LhatValue *arguments, size_t count, LhatValue *answers,
                 int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    (void)answers;
    *answer_count = 0;
}

static void register_k(LhatProgram *program)
{
    lhat_register_hostdata_type(program, "k", "T");
    lhat_register_member(program, "k", "T", "alpha", "f^self^ -> number^;",
                         stub, NULL);
    lhat_register_member(program, "k", "T", "beta", "f^self^ -> number^;",
                         stub, NULL);
    lhat_register_func(program, "k", "make", "f^ -> k.T;", stub, NULL);
}

// Just past `needle` in a unit the program checked.
static uint32_t after_in(const LhatUnit *unit, const char *needle)
{
    const char *at = strstr(unit->source.text, needle);
    return at == NULL ? 0 : (uint32_t)(at - unit->source.text + strlen(needle));
}

static size_t items_of(const LhatUnit *unit, uint32_t offset,
                       LhatCompletionItem **into)
{
    size_t wanted = lhat_unit_completion_items(unit, offset, NULL, 0);
    *into = wanted > 0 ? (LhatCompletionItem *)calloc(wanted, sizeof **into)
                       : NULL;
    if (*into == NULL) {
        return wanted;
    }
    return lhat_unit_completion_items(unit, offset, *into, wanted);
}

static void test_remembered(void)
{
    LHAT_TEST("a receiver's members are answered out of what was kept");
    {
        File file = {"main.lh",
                     "import^ k\n"
                     "let^ t = k.make()\n"
                     "let^ n = t.\n"};
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &file);
        register_k(&program);
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL, "the unit checked");

        uint32_t where = after_in(unit, "t.");
        LhatCompletionItem *items = NULL;
        size_t count = items_of(unit, where, &items);
        LHAT_CHECK(offers(items, count, "alpha"), "alpha is offered");
        LHAT_CHECK(offers(items, count, "beta"), "beta is offered");
        // items_of asks twice, to measure and to fill; the second is the
        // first one's answer given back.
        LHAT_CHECK_EQ_INT(program.completion_hits, 1);

        LhatCompletionItem *again = NULL;
        size_t twice = items_of(unit, where, &again);
        LHAT_CHECK_EQ_INT(twice, count);
        LHAT_CHECK(offers(again, twice, "alpha"), "alpha again");
        LHAT_CHECK(offers(again, twice, "beta"), "beta again");
        LHAT_CHECK_EQ_INT(program.completion_hits, 3);

        // A registration is the one thing that writes into a type already
        // answered for, so what was kept goes and the next ask builds again.
        lhat_register_func(&program, "k", "other", "f^ -> number^;", stub,
                           NULL);
        free(again);
        again = NULL;
        twice = items_of(unit, where, &again);
        LHAT_CHECK_EQ_INT(twice, count);
        LHAT_CHECK_EQ_INT(program.completion_hits, 4);

        free(items);
        free(again);
        lhat_program_dispose(&program);
    }

    // The hazard the address key has to survive: a def^'s type is built again
    // by every recheck, so an answer kept under the old one must not be given
    // for the new. It is not, because the program's arena never hands the
    // same address out twice while it lives.
    LHAT_TEST("a recheck that changed a def^ is answered afresh");
    {
        File file = {"main.lh",
                     "let^ D = def^{ self^{ alef : number^ } }\n"
                     "let^ d = D.new()\n"
                     "let^ n = d.\n"};
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &file);
        const LhatUnit *unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL, "the unit checked");

        LhatCompletionItem *items = NULL;
        size_t count = items_of(unit, after_in(unit, "d."), &items);
        LHAT_CHECK(offers(items, count, "alef"), "alef is offered");
        free(items);
        items = NULL;

        file.text =
            "let^ D = def^{ self^{ bet : number^ } }\n"
            "let^ d = D.new()\n"
            "let^ n = d.\n";
        LHAT_CHECK(lhat_program_invalidate(&program, "main.lh") > 0,
                   "the unit was invalidated");
        unit = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(unit != NULL, "the unit checked again");

        count = items_of(unit, after_in(unit, "d."), &items);
        LHAT_CHECK(offers(items, count, "bet"), "bet is offered");
        LHAT_CHECK(!offers(items, count, "alef"), "alef is not");

        free(items);
        lhat_program_dispose(&program);
    }
}

int main(void)
{
    test_which_question();
    test_members();
    test_words();
    test_remembered();
    return lhat_test_report("test_completion_core");
}
