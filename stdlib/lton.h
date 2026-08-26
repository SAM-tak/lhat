// L^ (lhat) -- sample standard library: std.lton.
//
// LTON is L^ Table Object Notation: a file that is the inside of a table
// literal and nothing else, so that a configuration is written as data
// rather than as a chunk that answers data.
//
//   # conf.lton
//   identity = "lhatove-suite",
//   window = { title = "a window", width = 480, height = 320 },
//
//   let^ conf = try^ std.lton.load("conf.lton")   # => t^{}
//
// The elements are 02 の 14.14's three forms as they stand -- positional,
// `name = value`, `[value] = value` -- separated by `,`, and a trailing one
// is allowed. The spelling is L^'s throughout: the comments, the string
// escapes and the shapes of a number are the language's because the same
// lexer reads them, not because they were made to look alike.
//
// WHAT MAY BE WRITTEN. The text is read as the body of an f^, and 02 の 15.1
// says an f^ may call only an f^. So arithmetic, comparison, concatenation
// and nested tables all go through, and **a p^ call is an error**. Since
// everything with an effect is a p^, an LTON file cannot have one. That is
// the whole of what makes reading one safe, and it is a rule the language
// already had rather than a new check written for this.
//
// 05 の 8.2's initial bindings are not in scope either: what a host bound
// for the units it means to run is no part of what a configuration file may
// name. Nothing else is in scope in this first cut -- letting the caller say
// what to import^ or require^ is DesignDocuments/08-lton.md's T1.
//
// 04 の 11.3: a table does not hold nil^, so `a = nil^` puts no key -- the
// same answer std.json gives, and for the same reason.
//
// See DesignDocuments/08-lton.md.

#ifndef LHATSTDLIB_LTON_H
#define LHATSTDLIB_LTON_H

#include <stdbool.h>
#include <stddef.h>

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_lton_register(LhatProgram *program);

// ---------------------------------------------------------------------------
// Reading one from the host
// ---------------------------------------------------------------------------
//
// The same two readings, named so that C has them too. A configuration is
// data, and a host wanting its own conf.lton should not have to write a unit
// that answers one and run it:
//
//     LhatValue conf;
//     if (lhatstdlib_lton_load(machine, program, "conf.lton", &conf) ==
//         LHAT_LTON_OK) {
//         readConf(machine, conf, settings);   // lhat_table_get, and done
//     }
//
// Registration is not needed. These take the program rather than reaching it
// through a registered module, so a host that only wants to read files never
// puts std.lton where a script can see it.
//
// WHAT COMES BACK IS THE MACHINE'S. vm.h's rule holds here as everywhere: a
// value a host is holding is not a root. Nothing between these calls and the
// reading collects -- the collector advances inside the interpreter's loop
// and in lhat_machine_collectgarbage, and nowhere else, so making a key with
// lhat_machine_make_string and asking lhat_table_get for it is safe as it
// stands. Read what is wanted out of the table before running L^ again;
// a table kept across a run wants somewhere the machine reaches
// (lhat_machine_set_global).
typedef enum {
    LHAT_LTON_OK,
    // The program's loader had nothing for the path (or was never given).
    LHAT_LTON_CANNOT_READ,
    // The checker or the compiler refused it, which is where a text that
    // tried to call a p^ arrives. lhat_program_load_failure(program) is what
    // was said, one diagnostic per line.
    LHAT_LTON_REJECTED,
    // It was read and it ran, and it stopped -- a panic^, a division by
    // zero. lhat_machine_traceback(machine, ...) is where it stopped.
    LHAT_LTON_FAULTED,
    LHAT_LTON_OUT_OF_MEMORY
} LhatLtonStatus;

// A text already in hand. `name` is what diagnostics call it -- NULL for
// "(lton)", which is what the L^ side passes. `length` is the whole of the
// text and is meant: an empty one is an empty table, so 0 cannot stand for
// "measure it yourself".
//
// `out` is the table on OK and nil^ on anything else.
LhatLtonStatus lhatstdlib_lton_parse(LhatMachine *machine,
                                     LhatProgram *program, const char *name,
                                     const char *text, size_t length,
                                     LhatValue *out);

// The text at `path`, read through the program's loader and through nothing
// else (05 の 8.9) -- so a host that handed none over reads nothing here
// either. Diagnostics call it by its path.
LhatLtonStatus lhatstdlib_lton_load(LhatMachine *machine, LhatProgram *program,
                                    const char *path, LhatValue *out);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_LTON_H
