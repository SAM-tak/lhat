// L^ (lhat) -- a message's stable ID and its English text (10 §4).
//
// Every table of messages the library writes is one of these, indexed by the
// code it answers for. The ID is what a translation is written against and
// never changes meaning (10 §4.2); the English is the reference every other
// language translates (10 §2.2). A code the table does not hold has an entry
// of NULLs, which LHAT_MESSAGE_AT answers as no entry at all.
//
// Internal. What an ID crosses the public API as is 10 §7.3's to settle.

#ifndef LHAT_MESSAGE_H
#define LHAT_MESSAGE_H

#include <stddef.h>

#include "lhat/error.h"
#include "lhat/lexer.h"
#include "lhat/module.h"
#include "lhat/program.h"
#include "lhat/vm.h"

// How many rows a table of entries holds.
#define LHAT_MESSAGE_COUNT(table) (sizeof(table) / sizeof((table)[0]))

// One of a source's tables (10 §6.3): a source hands over every table it
// holds, and the catalog is written out of them in order. A row of NULLs is
// a code the table does not hold and is left out.
typedef struct {
    const LhatMessageEntry *entries;
    size_t count;
} LhatMessageTable;

// The function each source answers its tables with. `count` is how many
// tables; the tables themselves outlive every caller.
#define LHAT_MESSAGE_TABLES(function, ...)                                    \
    const LhatMessageTable *function(size_t *count)                           \
    {                                                                         \
        static const LhatMessageTable tables[] = {__VA_ARGS__};               \
        *count = LHAT_MESSAGE_COUNT(tables);                                  \
        return tables;                                                        \
    }

const LhatMessageTable *lhat_check_message_tables(size_t *count);
const LhatMessageTable *lhat_parse_message_tables(size_t *count);
const LhatMessageTable *lhat_lexer_message_tables(size_t *count);
const LhatMessageTable *lhat_compile_message_tables(size_t *count);
const LhatMessageTable *lhat_run_message_tables(size_t *count);
const LhatMessageTable *lhat_program_message_tables(size_t *count);
const LhatMessageTable *lhat_source_message_tables(size_t *count);
const LhatMessageTable *lhat_report_message_tables(size_t *count);
const LhatMessageTable *lhat_trace_message_tables(size_t *count);
const LhatMessageTable *lhat_fix_message_tables(size_t *count);

// 07 §6: which title a fix is offered under. The kinds are the library's own,
// since a fix is worked out where a stage refused.
typedef enum {
    LHAT_FIX_WRITE_TOKEN,
    LHAT_FIX_LET_TO_VAR,
    LHAT_FIX_VAR_TO_LET,
    LHAT_FIX_WRITE_OVERRIDE,
    LHAT_FIX_WRITE_OVERLOAD,
    LHAT_FIX_REMOVE_MARKER,
    LHAT_FIX_TABLE_MEMBERS,
    LHAT_FIX_REMOVE_SCOPE,
    LHAT_FIX_REMOVE_ANNOTATION,
    LHAT_FIX_HAND_BACK,
    LHAT_FIX_DELEGATE,
    LHAT_FIX_NEAR_NAME
} LhatFixTitle;

const LhatMessageEntry *lhat_fix_message(size_t which);

// The entry for `code`, or NULL when the table holds none. `table` has to be
// the array itself rather than a pointer to it, since the length is read off
// it. A negative code converts to a size past any table, so it is none too.
#define LHAT_MESSAGE_AT(table, code)                                          \
    ((size_t)(code) < sizeof(table) / sizeof((table)[0]) &&                   \
             (table)[(size_t)(code)].id != NULL                               \
         ? &(table)[(size_t)(code)]                                           \
         : NULL)

// The ID of the text lhat_compile_message_write draws from.
const char *lhat_compile_message_id(const LhatCompileResult *result);

// 10 §6: what one language has for one source -- the entries read out of the
// bytes a host handed over (10 §6.2). Owns its strings, and holds whole IDs,
// since that is what a render site asks by. Zeroed is empty.
typedef struct {
    char *tag;
    char *source;
    LhatMessageEntry *entries;
    size_t count;
    size_t capacity;
} LhatCatalog;

// 10 §6.4: reads `text` into `catalog`, replacing whatever it held, and
// answers how many entries it ends up holding -- a name written twice is one
// entry, the later text. `english` is the table to check the names and the
// holes against, or NULL to check against the source this build holds
// itself.
//
// Never fails. A name the source does not hold, a text whose holes are not
// the English's, an entry with no text, and a line that is none of these are
// left out; what is left stands.
size_t lhat_catalog_load(LhatCatalog *catalog, const char *tag,
                         const char *source, const LhatMessageEntry *english,
                         size_t english_count, const char *text,
                         size_t length);
void lhat_catalog_dispose(LhatCatalog *catalog);
// The text this catalog has for `id`, or NULL when it has none.
const char *lhat_catalog_text(const LhatCatalog *catalog, const char *id);

#endif  // LHAT_MESSAGE_H
