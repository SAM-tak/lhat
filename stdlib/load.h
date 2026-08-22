// L^ (lhat) -- sample standard library: std.load.
//
// 05 の 5.6: a script brought in while the program runs -- a stage's script
// read after the loading screen, a piece of text a tool made. What
// require^ could not do (5.2: its path is a literal, and what it brings in
// is loaded once and kept) this does on purpose: the path is a value, the
// load compiles every time, and what it brings in goes with the closure it
// answered -- the body is the machine's heap's, collected with the last
// closure of it.
//
//   std.load.file(path)         -> p^... -> any^; | std.load.Error
//   std.load.text(source, name) -> the same, from text; `name` is what a
//                                  diagnostic calls it and what a require^
//                                  inside it is relative to
//
// What the closure answers is the unit's: a script (no module^) takes '...'
// (3.2) and answers its return^; a module^ unit takes nothing and answers
// the table of its public^ names -- built and sealed as require^ would,
// but entering no registry, so import^ never sees it and a second load is
// a second table. What the script itself require^s joins the program as
// any unit does: checked and compiled then and there, registered once it
// first runs, kept.
//
// Error.CannotRead is a path the program's loader has nothing for (or a
// program given no loader); Error.Rejected is what the checker or the
// compiler said, every diagnostic on a line of its own, in message.
//
// A load writes the program, so it belongs to one machine at a time: not
// from a std.thread worker while another machine may be loading.
//
// Lives here rather than on L^ (8.6) because the machine cannot do this on
// its own -- it takes the program and its loader -- and because a host
// that registers nothing here has no run-time code loading at all (8.9's
// line: nothing reaches a file system without being told to).

#ifndef LHATSTDLIB_LOAD_H
#define LHATSTDLIB_LOAD_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_load_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_LOAD_H
