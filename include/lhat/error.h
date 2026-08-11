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
size_t lhat_report_write(const LhatReport *report, const LhatSource *source,
                         const char *name, bool rich, char *out,
                         size_t capacity);

#endif  // LHAT_ERROR_H
