// 10 §4: every coded message has a stable ID. The ID is what a translation is
// written against, so each one is well formed, unique across every table, and
// absent one past the last code -- where the fallback text is answered instead.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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

// Every source the library holds and the tables it hands over -- the codes'
// texts, and the phrases, fixed words and wrapping sentences beside them.
// The tests build with the front end, so all ten are here (10 §6.2).
typedef const LhatMessageTable *(*Tables)(size_t *count);

static const struct {
    const char *source;
    Tables tables;
} SOURCES[] = {
    {"check", lhat_check_message_tables},
    {"parse", lhat_parse_message_tables},
    {"lex", lhat_lexer_message_tables},
    {"compile", lhat_compile_message_tables},
    {"run", lhat_run_message_tables},
    {"program", lhat_program_message_tables},
    {"source", lhat_source_message_tables},
    {"report", lhat_report_message_tables},
    {"trace", lhat_trace_message_tables},
    {"fix", lhat_fix_message_tables},
};

// Every ID met, for the check that no two entries share one.
static const char *all_ids[512];
static size_t seen;

static void remember(const char *id)
{
    LHAT_CHECK(seen < sizeof all_ids / sizeof all_ids[0], "room for '%s'", id);
    if (seen < sizeof all_ids / sizeof all_ids[0]) {
        all_ids[seen++] = id;
    }
}

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

// How many holes `text` holds, and whether every one is among `allowed`.
static size_t holes_in(const char *text, const char *const *allowed,
                       size_t allowed_count, bool *all_allowed)
{
    size_t count = 0;
    *all_allowed = true;
    for (const char *p = strchr(text, '{'); p != NULL; p = strchr(p + 1, '{')) {
        const char *end = p + 1;
        if (*end < 'a' || *end > 'z') {
            continue;
        }
        while ((*end >= 'a' && *end <= 'z') || (*end >= '0' && *end <= '9') ||
               *end == '-') {
            end++;
        }
        if (*end != '}') {
            continue;
        }
        count++;
        bool known = false;
        for (size_t i = 0; i < allowed_count; i++) {
            size_t n = strlen(allowed[i]);
            known = known || ((size_t)(end + 1 - p) == n &&
                              memcmp(p, allowed[i], n) == 0);
        }
        *all_allowed = *all_allowed && known;
    }
    return count;
}

static void test_ids(void)
{
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
        }
        // One past the last code there is no row: no ID, and the fallback.
        LHAT_CHECK(table->id(table->count) == NULL,
                   "%s has no ID past its last code", table->source);
        LHAT_CHECK(strcmp(table->message(table->count), table->fallback) == 0,
                   "%s answers its fallback past its last code",
                   table->source);
    }

    // Every entry of every table, which is what a catalog is written from.
    for (size_t s = 0; s < LHAT_MESSAGE_COUNT(SOURCES); s++) {
        LHAT_TEST(SOURCES[s].source);
        size_t count = 0;
        const LhatMessageTable *tables = SOURCES[s].tables(&count);
        LHAT_CHECK(count > 0, "%s hands over its tables", SOURCES[s].source);
        size_t held = 0;
        for (size_t t = 0; t < count; t++) {
            for (size_t e = 0; e < tables[t].count; e++) {
                const LhatMessageEntry *entry = &tables[t].entries[e];
                if (entry->id == NULL) {
                    continue;  // a code the table does not hold
                }
                LHAT_CHECK(well_formed(entry->id, SOURCES[s].source),
                           "'%s' is well formed", entry->id);
                LHAT_CHECK(entry->text != NULL && entry->text[0] != '\0',
                           "'%s' has its English", entry->id);
                remember(entry->id);
                held++;
            }
        }
        LHAT_CHECK(held > 0, "%s holds entries", SOURCES[s].source);
    }

    LHAT_TEST("parse: a code's text holds no hole but {found}");
    static const char *const FOUND_HOLE[] = {"{found}"};
    for (int code = 0; code <= LHAT_PARSE_ERR_DUPLICATE_INDEXER; code++) {
        bool all_found;
        size_t n = holes_in(parse_message(code), FOUND_HOLE, 1, &all_found);
        LHAT_CHECK(n <= 1 && all_found, "parse code %d", code);
    }
}

// The holes a checker diagnostic's name is offered under.
static const char *const NAME_HOLES[] = {"{name}", "{member}", "{field}",
                                         "{annotation}", "{kind}"};

// 10 §5.1 over the checker: a diagnostic's name goes into its text's one
// hole. A code with a second text for being reported with a name keeps its
// own free of a hole, and the second text's ID is its own and `.named`.
static void test_named(void)
{
    LHAT_TEST("a checker diagnostic's name goes into one hole");
    int count = LHAT_CHECK_ERR_BARE_TABLE_TYPE + 1;
    for (int code = 0; code <= count; code++) {
        LhatCheckDiagnostic d;
        memset(&d, 0, sizeof d);
        d.code = (LhatCheckErrorCode)code;
        const char *plain = lhat_check_message_id(&d);
        d.name = "Zq9";
        d.name_length = 3;
        const char *named = lhat_check_message_id(&d);
        if (code == count) {
            LHAT_CHECK(plain == NULL && named == NULL,
                       "no ID past the last code");
            continue;
        }
        LHAT_CHECK(plain != NULL && named != NULL &&
                       strcmp(plain, check_id(code)) == 0,
                   "check code %d: both IDs, the plain one its own", code);
        if (plain == NULL || named == NULL) {
            continue;
        }

        bool all_named;
        size_t own = holes_in(check_message(code), NAME_HOLES,
                              sizeof NAME_HOLES / sizeof NAME_HOLES[0],
                              &all_named);
        LHAT_CHECK(own <= 1 && all_named,
                   "'%s' holds at most one hole, and one for a name", plain);
        char message[512];
        lhat_check_message_write(NULL, &d, message, sizeof message);
        if (strcmp(named, plain) == 0) {
            LHAT_CHECK((own == 1) == (strstr(message, "Zq9") != NULL),
                       "'%s' says the name exactly when it has a hole", plain);
            continue;
        }
        size_t n = strlen(plain);
        LHAT_CHECK(strncmp(named, plain, n) == 0 &&
                       strcmp(named + n, ".named") == 0 &&
                       well_formed(named, "check"),
                   "'%s' is the plain ID and .named", named);
        LHAT_CHECK(own == 0, "'%s' has a second text, so no hole", plain);
        LHAT_CHECK(strstr(message, "Zq9") != NULL, "'%s' says the name",
                   named);
    }

    // The compiler's name has no status that is only ever said with one, so
    // every status about a name keeps a plain text and has a second.
    LHAT_TEST("a compile result's name goes into a hole");
    for (int status = 0; status <= LHAT_COMPILE_NOT_PUBLISHED; status++) {
        LhatCompileResult r;
        memset(&r, 0, sizeof r);
        r.status = (LhatCompileStatus)status;
        const char *plain = lhat_compile_message_id(&r);
        r.name = "Zq9";
        r.name_length = 3;
        const char *named = lhat_compile_message_id(&r);
        LHAT_CHECK(plain != NULL && named != NULL &&
                       strcmp(plain, compile_id(status)) == 0,
                   "compile status %d: both IDs, the plain one its own",
                   status);
        bool none;
        LHAT_CHECK(holes_in(compile_message(status), NULL, 0, &none) == 0,
                   "compile status %d: its own text holds no hole", status);
        if (plain == NULL || named == NULL || strcmp(named, plain) == 0) {
            continue;
        }
        size_t n = strlen(plain);
        LHAT_CHECK(strncmp(named, plain, n) == 0 &&
                       strcmp(named + n, ".named") == 0 &&
                       well_formed(named, "compile"),
                   "'%s' is the plain ID and .named", named);
        char message[256];
        lhat_compile_message_write(NULL, &r, message, sizeof message);
        LHAT_CHECK(strstr(message, "Zq9") != NULL, "'%s' says the name",
                   named);
    }
}

// 10 §6.3: the English of one source, written out for a translation to be
// made from -- every entry commented out, under a heading that names the
// source and the version.
static void test_dump(void)
{
    LHAT_TEST("every source this build holds writes its catalog");
    size_t sources = 0;
    for (const char *source; (source = lhat_messages_source(sources)) != NULL;
         sources++) {
        size_t needed = lhat_messages_write_english(source, NULL, 0);
        char *written = (char *)malloc(needed + 1);
        LHAT_CHECK(written != NULL && needed > 0, "'%s' writes something",
                   source);
        if (written == NULL) {
            continue;
        }
        LHAT_CHECK_EQ_INT(lhat_messages_write_english(source, written,
                                                      needed + 1),
                          needed);
        LHAT_CHECK(strlen(written) == needed, "'%s' fills what it measured",
                   source);

        char heading[64];
        snprintf(heading, sizeof heading, "# %s -- L^ ", source);
        LHAT_CHECK(strncmp(written, heading, strlen(heading)) == 0,
                   "'%s' names itself and the version first", source);

        // Every entry is in it, commented out and under its bare name.
        size_t count = 0;
        const LhatMessageTable *tables = NULL;
        for (size_t s = 0; s < LHAT_MESSAGE_COUNT(SOURCES); s++) {
            if (strcmp(SOURCES[s].source, source) == 0) {
                tables = SOURCES[s].tables(&count);
            }
        }
        LHAT_CHECK(tables != NULL, "'%s' is one of the sources", source);
        for (size_t t = 0; tables != NULL && t < count; t++) {
            for (size_t e = 0; e < tables[t].count; e++) {
                const LhatMessageEntry *entry = &tables[t].entries[e];
                if (entry->id == NULL) {
                    continue;
                }
                char wanted[128];
                snprintf(wanted, sizeof wanted, "# %s = ",
                         entry->id + strlen(source) + 1);
                LHAT_CHECK(strstr(written, wanted) != NULL, "'%s' is in it",
                           entry->id);
            }
        }
        free(written);
    }
    LHAT_CHECK_EQ_INT(sources, LHAT_MESSAGE_COUNT(SOURCES));

    LHAT_TEST("a source this build does not hold writes nothing");
    LHAT_CHECK_EQ_INT(lhat_messages_write_english("nowhere", NULL, 0), 0);

    LHAT_TEST("a tool's own table is written the same way");
    const LhatMessageEntry own[] = {
        {"tool.one", "first"},
        {"tool.two", "a line\nand another"},
        {"other.three", "not this file's"},
    };
    size_t needed = lhat_messages_write_catalog("tool", own, 3, NULL, 0);
    char *written = (char *)malloc(needed + 1);
    LHAT_CHECK(written != NULL, "room for it");
    if (written != NULL) {
        lhat_messages_write_catalog("tool", own, 3, written, needed + 1);
        LHAT_CHECK(strstr(written, "# tool -- L^ ") == written,
                   "the heading names the source");
        LHAT_CHECK(strstr(written, "# one = first\n") != NULL, "one entry");
        LHAT_CHECK(strstr(written, "# two = a line\n#  and another\n") != NULL,
                   "a newline becomes a continuation line");
        LHAT_CHECK(strstr(written, "three") == NULL,
                   "another source's entry is not this file's");
        free(written);
    }

    LHAT_TEST("measuring and cutting follow lhat_report_write");
    char small[8];
    size_t wanted = lhat_messages_write_catalog("tool", own, 1, small,
                                                sizeof small);
    LHAT_CHECK(wanted > sizeof small, "it wanted more than it was given");
    LHAT_CHECK(strlen(small) == sizeof small - 1, "and closed what it filled");
}

static void test_unique(void)
{
    LHAT_TEST("every ID is unique across the tables");
    for (size_t i = 0; i < seen; i++) {
        for (size_t j = i + 1; j < seen; j++) {
            LHAT_CHECK(strcmp(all_ids[i], all_ids[j]) != 0,
                       "'%s' is used twice", all_ids[i]);
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
    test_named();
    test_dump();
    test_unique();
    test_render();
    return lhat_test_report("test_messages");
}
