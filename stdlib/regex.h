// L^ (lhat) -- sample standard library: std.regex.
//
// Compiled regular expressions as hostdata, the Python/Go shape: the plain
// searches live on string^ itself (02 の 14.19改3), and everything a pattern
// asks for lives here, behind an object the writer compiles once and keeps.
//
//   std.regex.new(p)            -> std.regex.Regex | std.regex.Error.BadPattern
//   r.match(s)                  -> string^|nil^ | Error.Exhausted
//   r.captures(s)               -> t^{string^[]}|nil^ | Error.Exhausted
//   r.gmatch(s)                 -> an f^ walk of (ordinal, match) pairs
//   r.gsub(s, "…$1…")           -> string^ | Error.Exhausted
//   r.gsub(s, f^m, at, caps {}) -> the same, each match through the function
//   r.split(s)                  -> the pieces between the matches, empties kept
//   std.regex.match/gmatch/gsub(p, s, …)   -- the convenience forms; they
//       compile every call, so a hot path holds a new() of its own
//
// Error.Exhausted is the backtracking budget giving out -- a pathological
// pattern ((a+)+b against a long text), reported rather than felt. A gmatch
// walk cannot answer an error mid-walk, so there the budget simply ends it.

#ifndef LHATSTDLIB_REGEX_H
#define LHATSTDLIB_REGEX_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_regex_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_REGEX_H
