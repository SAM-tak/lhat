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

#include "lhat/program.h"

bool lhatstdlib_lton_register(LhatProgram *program);

#endif  // LHATSTDLIB_LTON_H
