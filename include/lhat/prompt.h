// L^ (lhat) -- a prompt: one session of each stage answering many inputs.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. 4 章 is the read-eval-print loop: an input that stopped early is
// carried until it is whole (02 の 3.1), the names one input binds are there
// for the next (4.3), a bare expression is a statement here and nowhere else
// (02 の 8.2), and an input that is refused leaves the session as it was.
//
// All of that is the library's. What is the host's is everything around it:
// where the lines come from, what is printed before each one, where answers
// and reports go, and which program and machine the inputs run on -- a game
// hands in the ones it is running, so what it registered is what the prompt
// reaches.
//
//     LhatPrompt *prompt = lhat_prompt_new(program, machine, "console");
//     for (each line the host reads) {
//         LhatPromptResult r = lhat_prompt_feed(prompt, line, length);
//         // r.status == LHAT_PROMPT_MORE: show a continuation prompt
//         // lhat_prompt_report: what the input was told, if anything
//         // r.status == LHAT_PROMPT_RAN: r.value is the answer
//     }
//     lhat_prompt_dispose(prompt);
//
// The front end's: a build without LHAT_WITH_FRONTEND has none of this, the
// way it has no lhat/completion.h. Not reached by lhat.h.

#ifndef LHAT_PROMPT_H
#define LHAT_PROMPT_H

#include <stdbool.h>
#include <stddef.h>

#include "lhat/program.h"
#include "lhat/value.h"
#include "lhat/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LhatPrompt LhatPrompt;

// A prompt whose inputs are checked against what `program` registered, in
// its language and with its strictness, and run on `machine` -- which the
// program has to have been installed on (lhat_program_install). Both are
// the host's and have to outlive the prompt. `name` is what a report calls
// the input ("stdin", say); it is copied. NULL when there is no room.
//
// The inputs run on `machine` between whatever else the host runs there, so
// a host feeding a prompt does it where nothing else is running on it.
LhatPrompt *lhat_prompt_new(LhatProgram *program, LhatMachine *machine,
                            const char *name);
void lhat_prompt_dispose(LhatPrompt *prompt);

typedef enum {
    LHAT_PROMPT_RAN,      // it ran; `value` is what it answered (nil^ for none)
    LHAT_PROMPT_MORE,     // it stopped early and is carried for the next feed
    LHAT_PROMPT_REFUSED,  // the reading, the check or the compile refused it,
                          // and the session is as it was before
    LHAT_PROMPT_FAILED    // it ran and stopped on a run error
} LhatPromptStatus;

typedef struct {
    LhatPromptStatus status;
    LhatValue value;  // meaningful for LHAT_PROMPT_RAN alone
} LhatPromptResult;

// Adds `text` -- a line, or any piece of one -- to what is being carried,
// and answers once that is a whole input. The value belongs to the machine.
LhatPromptResult lhat_prompt_feed(LhatPrompt *prompt, const char *text,
                                  size_t length);

// Throws away what is being carried -- for a host to bind to whatever it
// lets a reader leave an input they will not finish with. Answers whether
// there was anything to throw away.
bool lhat_prompt_drop(LhatPrompt *prompt);

// What the last feed was told, in the program's language (10 §7.1): every
// diagnostic its input met, each with the line it points into, and a run
// error with its traceback (lhat_run_error_write). Empty when it was told
// nothing, as a feed answering LHAT_PROMPT_MORE always is -- warnings under
// relaxed are told about an input that still ran. Follows lhat_report_write:
// answers the length the whole text needs, and (NULL, 0) measures.
size_t lhat_prompt_report(const LhatPrompt *prompt, char *out,
                          size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_PROMPT_H
