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

// The entry for `code`, or NULL when the table holds none. `table` has to be
// the array itself rather than a pointer to it, since the length is read off
// it. A negative code converts to a size past any table, so it is none too.
#define LHAT_MESSAGE_AT(table, code)                                          \
    ((size_t)(code) < sizeof(table) / sizeof((table)[0]) &&                   \
             (table)[(size_t)(code)].id != NULL                               \
         ? &(table)[(size_t)(code)]                                           \
         : NULL)

// The IDs for the four tables whose message functions are public (lexer.h,
// module.h, vm.h, program.h) -- declared here rather than beside them, since
// what an ID is to a host is still 10 §7.3's. NULL for a code the table does
// not hold.
const char *lhat_lexer_error_id(LhatErrorCode code);
const char *lhat_compile_status_id(LhatCompileStatus status);
const char *lhat_run_status_id(LhatRunStatus status);
const char *lhat_program_error_id(LhatProgramErrorCode code);

// The ID of the text lhat_compile_message_write draws from.
const char *lhat_compile_message_id(const LhatCompileResult *result);

#endif  // LHAT_MESSAGE_H
