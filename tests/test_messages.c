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

int main(void)
{
    test_ids();
    return lhat_test_report("test_messages");
}
