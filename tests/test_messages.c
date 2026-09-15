// 10 §4: every coded message has a stable ID. The ID is what a translation is
// written against, so each one is well formed, unique across every table, and
// absent one past the last code -- where the fallback text is answered instead.

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "message.h"
#include "parser.h"
#include "testutil.h"

static const char *check_id(int c)
{
    return lhat_check_error_id((LhatCheckErrorCode)c);
}

static const char *check_message(int c)
{
    return lhat_check_error_message((LhatCheckErrorCode)c);
}

static const char *parse_id(int c)
{
    return lhat_parse_error_id((LhatParseErrorCode)c);
}

static const char *parse_message(int c)
{
    return lhat_parse_error_message((LhatParseErrorCode)c);
}

static const char *lex_id(int c)
{
    return lhat_lexer_error_id((LhatErrorCode)c);
}

static const char *lex_message(int c)
{
    return lhat_lexer_error_message((LhatErrorCode)c);
}

static const char *run_id(int c)
{
    return lhat_run_status_id((LhatRunStatus)c);
}

static const char *run_message(int c)
{
    return lhat_run_status_message((LhatRunStatus)c);
}

static const char *program_id(int c)
{
    return lhat_program_error_id((LhatProgramErrorCode)c);
}

static const char *program_message(int c)
{
    return lhat_program_error_message((LhatProgramErrorCode)c);
}

static const char *compile_id(int c)
{
    return lhat_compile_status_id((LhatCompileStatus)c);
}

static const char *compile_message(int c)
{
    return lhat_compile_status_message((LhatCompileStatus)c);
}

// Each table, counted by the last code its enum declares. A code appended to
// an enum has to be counted here too -- the check one past the end is what
// notices when it was given a row and this was not moved.
typedef struct {
    const char *source;
    int count;
    const char *(*id)(int code);
    const char *(*message)(int code);
    const char *fallback;
} Table;

static const Table TABLES[] = {
    {"check", LHAT_CHECK_ERR_BARE_TABLE_TYPE + 1, check_id, check_message,
     "unknown error"},
    {"parse", LHAT_PARSE_ERR_DUPLICATE_INDEXER + 1, parse_id, parse_message,
     "unknown error"},
    {"lex", LHAT_ERR_INTERPOLATION_TOO_DEEP + 1, lex_id, lex_message,
     "unknown error"},
    {"run", LHAT_RUN_SUSPENDED + 1, run_id, run_message, "unknown"},
    {"program", LHAT_PROGRAM_ERR_NO_FRONTEND + 1, program_id, program_message,
     "unknown error"},
    {"compile", LHAT_COMPILE_NOT_PUBLISHED + 1, compile_id, compile_message,
     "unknown"},
};

// 10 §4.2: `source.name`, lower-case ASCII letters and digits, words joined by
// '-', and nothing doubled or left dangling at either end.
static bool well_formed(const char *id, const char *source)
{
    size_t n = strlen(source);
    if (strncmp(id, source, n) != 0 || id[n] != '.') {
        return false;
    }
    bool last_alnum = false;
    for (const char *p = id + n + 1; *p != '\0'; p++) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (alnum) {
            last_alnum = true;
        } else if ((*p == '-' || *p == '.') && last_alnum) {
            last_alnum = false;
        } else {
            return false;
        }
    }
    return last_alnum;
}

static void test_ids(void)
{
    const char *all[512];
    size_t seen = 0;

    for (size_t t = 0; t < sizeof TABLES / sizeof TABLES[0]; t++) {
        const Table *table = &TABLES[t];
        LHAT_TEST(table->source);
        for (int code = 0; code < table->count; code++) {
            const char *id = table->id(code);
            LHAT_CHECK(id != NULL, "%s code %d has an ID", table->source, code);
            if (id == NULL) {
                continue;
            }
            LHAT_CHECK(well_formed(id, table->source),
                       "%s code %d: '%s' is well formed", table->source, code,
                       id);
            const char *message = table->message(code);
            LHAT_CHECK(message != NULL && message[0] != '\0' &&
                           strcmp(message, table->fallback) != 0,
                       "%s code %d has its own English", table->source, code);
            if (seen < sizeof all / sizeof all[0]) {
                all[seen++] = id;
            }
        }
        // One past the last code there is no row: no ID, and the fallback.
        LHAT_CHECK(table->id(table->count) == NULL,
                   "%s has no ID past its last code", table->source);
        LHAT_CHECK(strcmp(table->message(table->count), table->fallback) == 0,
                   "%s answers its fallback past its last code",
                   table->source);
    }

    LHAT_TEST("every ID is unique across the tables");
    for (size_t i = 0; i < seen; i++) {
        for (size_t j = i + 1; j < seen; j++) {
            LHAT_CHECK(strcmp(all[i], all[j]) != 0, "'%s' is used twice",
                       all[i]);
        }
    }
}

// The sentence lhat_message_render makes, into a buffer wide enough for every
// case here.
static const char *rendered(const char *text, const LhatMessageArg *args,
                            size_t count)
{
    static char room[256];
    lhat_message_render(text, args, count, room, sizeof room);
    return room;
}

#define SAME(actual, expected)                                               \
    LHAT_CHECK(strcmp((actual), (expected)) == 0, "got '%s', want '%s'",     \
               (actual), (expected))

// 10 §5.1 and §6.1: what a hole is, and what is not one.
static void test_render(void)
{
    const LhatMessageArg member[] = {{"member", "ping", 4}};

    LHAT_TEST("a text with no hole comes back as it is");
    SAME(rendered("no such name in scope", NULL, 0), "no such name in scope");

    LHAT_TEST("a hole takes its argument, wherever it stands");
    SAME(rendered("this value has no such member: {member}", member, 1),
         "this value has no such member: ping");
    SAME(rendered("\xE3\x81\x93\xE3\x81\xAE\xE5\x80\xA4\xE3\x81\xAB {member} "
                  "\xE3\x81\xAF\xE7\x84\xA1\xE3\x81\x84",
                  member, 1),
         "\xE3\x81\x93\xE3\x81\xAE\xE5\x80\xA4\xE3\x81\xAB ping "
         "\xE3\x81\xAF\xE7\x84\xA1\xE3\x81\x84");

    LHAT_TEST("the same hole twice takes the argument twice");
    SAME(rendered("{member} and {member}", member, 1), "ping and ping");

    // What the English already holds, which has to stand unchanged.
    LHAT_TEST("braces that are not a hole are braces");
    SAME(rendered("write 't^{}'", member, 1), "write 't^{}'");
    SAME(rendered("self^{ ... } is the notation", member, 1),
         "self^{ ... } is the notation");
    SAME(rendered("a '{' opens it", member, 1), "a '{' opens it");
    SAME(rendered("{Member} {1st} {member", member, 1),
         "{Member} {1st} {member");

    LHAT_TEST("a hole with no argument of its name is written as it stands");
    SAME(rendered("has no {field}", member, 1), "has no {field}");

    LHAT_TEST("escapes write a brace or a backslash");
    SAME(rendered("\\{member\\}", member, 1), "{member}");
    SAME(rendered("a \\\\ b", NULL, 0), "a \\ b");
    SAME(rendered("C:\\path", NULL, 0), "C:\\path");

    LHAT_TEST("what fills a hole is not read for holes again");
    const LhatMessageArg nested[] = {{"a", "{b}", 3}, {"b", "no", 2}};
    SAME(rendered("{a}", nested, 2), "{b}");

    LHAT_TEST("measuring and cutting follow lhat_report_write");
    LHAT_CHECK_EQ_INT(
        lhat_message_render("no such member: {member}", member, 1, NULL, 0),
        strlen("no such member: ping"));
    char small[5];
    size_t wanted =
        lhat_message_render("no such member: {member}", member, 1, small,
                            sizeof small);
    LHAT_CHECK_EQ_INT(wanted, strlen("no such member: ping"));
    SAME(small, "no s");
}

int main(void)
{
    test_ids();
    test_render();
    return lhat_test_report("test_messages");
}
