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

typedef struct {
    const char *id;
    const char *text;
} LhatMessageEntry;

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

// The IDs of the texts a source has besides the ones its codes index, by
// index from 0; NULL past the last: a traceback's fixed words, a report's
// label, a load's failure, and why a file did not become a source.
const char *lhat_trace_part_id(size_t index);
const char *lhat_report_part_id(size_t index);
const char *lhat_program_part_id(size_t index);
const char *lhat_source_part_id(size_t index);

#endif  // LHAT_MESSAGE_H
