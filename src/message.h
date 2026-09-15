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

// 10 §5.1: one argument of a message -- the hole it fills, by name, and the
// text that fills it, exactly as it is to be written. A phrase (10 §5.2) has
// already been looked up by whoever hands it over.
typedef struct {
    const char *name;
    const char *value;
    size_t length;
} LhatMessageArg;

// The sentence `text` makes with `args` in its holes (src/message.c says what
// a hole is). lhat_report_write's convention: answers the bytes the whole
// sentence needs, not counting the terminating NUL, and fills up to
// `capacity` including it -- so (NULL, 0) measures.
size_t lhat_message_render(const char *text, const LhatMessageArg *args,
                           size_t count, char *out, size_t capacity);

// The IDs for the four tables whose message functions are public (lexer.h,
// module.h, vm.h, program.h) -- declared here rather than beside them, since
// what an ID is to a host is still 10 §7.3's. NULL for a code the table does
// not hold.
const char *lhat_lexer_error_id(LhatErrorCode code);
const char *lhat_compile_status_id(LhatCompileStatus status);
const char *lhat_run_status_id(LhatRunStatus status);
const char *lhat_program_error_id(LhatProgramErrorCode code);

#endif  // LHAT_MESSAGE_H
