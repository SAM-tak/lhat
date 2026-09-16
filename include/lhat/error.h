// L^ (lhat) -- one shape for what every stage has to say.
//
// 03 の 1.1 has four stages and each keeps its own diagnostics: the lexer's
// LhatDiagnostic, the parser's LhatParseDiagnostic, the checker's
// LhatCheckDiagnostic. That is right -- a stage's codes are its own -- but
// what a reader sees should not depend on which stage spoke.
//
// So the codes stay where they are and only the rendering is shared. A
// caller turns whatever it has into an LhatReport and writes it out.
//
// Nothing here touches stdio. 05 の 8.9 keeps the language away from its
// surroundings, so this fills a buffer and the caller decides where it goes
// -- the same arrangement lhat_value_write uses.

#ifndef LHAT_ERROR_H
#define LHAT_ERROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

// 10 §7.1: a message is written in the language of the program it is about.
// lhat/program.h is where that is declared; naming it here asks for nothing
// else.
struct LhatProgram;

typedef enum {
    LHAT_REPORT_ERROR,
    LHAT_REPORT_NOTE
} LhatReportKind;

// One thing to say about one place. `message` is borrowed -- every stage's
// message function answers a literal -- and so is everything else here.
typedef struct {
    LhatReportKind kind;
    const char *message;

    // Where. `line` and `column` are one-based and are what the stage
    // recorded; `offset` indexes the source text and is what the line is
    // found by. `length` is in bytes, and zero marks one character rather
    // than a span.
    //
    // The column shown is a count of characters, since that is what an editor
    // is told to go to. Where the mark is *put* is a count of terminal cells,
    // which is not the same number -- see 03 の 1.3.
    uint32_t offset;
    uint32_t line;
    uint32_t column;
    uint32_t length;
} LhatReport;

// The plain form, which is one line and needs no source:
//
//     main.lh:1:67: error: a ';' closes what a ':' opened
//
// The rich form adds the line it happened on and points at it:
//
//     let^ f = f^ n { if^ n < 2: 1 else^: n * this^(n - 1) }
//                                                          ~
//     (main.lh)1:67: error: a ';' closes what a ':' opened
//
// The mark is '~' and not '^' because a hat is a letter here (01 の 2.2): a
// column of them under a line full of them reads as more of the same.
//
// A line too wide to fit is shown through a window around the mark, with
// '...' at whichever end was cut:
//
//     ...n < 2: 1 else^: n * this^(n - 1) }...
//                                        ~
//
// `source` may be NULL, and then the rich form falls back to the plain one --
// there is nothing to quote. `name` overrides the source's own, for a caller
// that knows the unit by a path the source does not carry; NULL keeps it.
//
// Follows lhat_value_write: answers how many bytes the whole thing wants,
// not counting the terminating NUL, and fills up to `capacity` including it.
// So measuring is a call with (NULL, 0).
// `program` is the one the report is about, whose language it is written in
// (10 §7.1); NULL writes the English. Its declaration is lhat/program.h's,
// which this header does not otherwise need.
size_t lhat_report_write(const struct LhatProgram *program,
                         const LhatReport *report, const LhatSource *source,
                         const char *name, bool rich, char *out,
                         size_t capacity);

// 10 §4: a message's stable ID and its English text. The ID is what a
// translation is written against and never changes meaning; the English is
// the reference every other language translates (10 §2.2).
typedef struct {
    const char *id;
    const char *text;
} LhatMessageEntry;

// 10 §5.1: one argument of a message -- the hole it fills, by name, and the
// text that fills it, exactly as it is to be written.
typedef struct {
    const char *name;
    const char *value;
    size_t length;
} LhatMessageArg;

// The sentence `text` makes with `args` in its holes. A hole is `{`, a name
// -- a lower-case ASCII letter, then letters, digits and '-' -- and `}`;
// every other brace is a brace, and `\{`, `\}` and `\\` write a brace or a
// backslash. What fills a hole goes in as it is and is not read for holes
// again, and a hole with no argument of its name is written as it stands. A
// phrase (10 §5.2) is looked up by whoever hands it over.
//
// Follows lhat_report_write: answers how many bytes the whole sentence
// wants, not counting the terminating NUL, and fills up to `capacity`
// including it. So measuring is a call with (NULL, 0).
size_t lhat_message_render(const char *text, const LhatMessageArg *args,
                           size_t count, char *out, size_t capacity);

// 10 §4.2: the sources this build holds messages for, in the order 10 §3.1
// lists them; NULL one past the last. A build without the front end holds no
// `check`, `parse` or `lex` (10 §6.2).
const char *lhat_messages_source(size_t index);

// 10 §6.3: the English of one source, written as a catalog with every entry
// commented out -- what a translation is made from. A source this build does
// not hold writes nothing and answers 0.
//
// Follows lhat_report_write: answers how many bytes the whole catalog wants,
// not counting the terminating NUL, and fills up to `capacity` including it.
size_t lhat_messages_write_english(const char *source, char *out,
                                   size_t capacity);

// The same for a table a tool holds itself -- the cli's `cli`, the debug
// adapter's `dap`, a host's own. The format lives here and nowhere else, so
// every catalog is written the one way. An entry whose ID does not start with
// `source` and a '.' is left out.
size_t lhat_messages_write_catalog(const char *source,
                                   const LhatMessageEntry *entries,
                                   size_t count, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_ERROR_H
