// L^ (lhat) -- the regular-expression engine behind std.regex.
//
// A backtracking matcher over a compiled pattern tree, self-contained: the
// only thing it takes from the rest of the tree is port.h's allocator. The
// dialect is a deliberate subset -- what the world's regex knowledge already
// covers, nothing exotic:
//
//   literals   a  あ           (UTF-8, matched by bytes)
//   any        .               (one code point, never a newline's worth more)
//   classes    [a-z] [^0-9]    (code-point ranges, so [あ-ん] works)
//   escapes    \d \w \s and their negations, \n \t \r, \\ and every
//              \<punct> as the literal
//   repeats    * + ? {n} {n,} {n,m}   (greedy; a trailing ? makes it lazy)
//   groups     (...) capturing, up to 15, and (?:...) grouping alone
//   choice     a|b
//   anchors    ^ $             (the text's ends; no multiline mode)
//
// No backreferences and no lookaround -- both are what turns backtracking
// pathological. A blowup that still happens (nested repeats over a long
// text) hits an explicit depth cap and is reported as such rather than
// eating the C stack.

#ifndef LHATSTDLIB_REGEX_ENGINE_H
#define LHATSTDLIB_REGEX_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct LhatRegex LhatRegex;

// Group 0 is the whole match; 1..15 are written groups in order of their
// opening parenthesis. A group that took part in no match has begin ==
// end == (size_t)-1.
#define LHAT_REGEX_MAX_GROUPS 16

typedef struct {
    size_t begin;  // byte offset into the text
    size_t end;    // byte offset just past the match
} LhatRegexSpan;

// Compiles a pattern. Answers NULL on a malformed one, with *error aimed at
// a static message and *error_at at the byte offset the trouble was seen.
LhatRegex *lhat_regex_compile(const char *pattern, size_t length,
                              const char **error, size_t *error_at);

void lhat_regex_free(LhatRegex *regex);

// How many written groups the pattern holds (not counting group 0).
size_t lhat_regex_groups(const LhatRegex *regex);

// Searches for the leftmost match at or after `from` (a byte offset).
// Answers true with the spans filled; false when nothing matches. A
// backtracking blowup answers false with *depth_exceeded set -- the caller
// tells the two apart because a blowup is the pattern's own error.
bool lhat_regex_search(const LhatRegex *regex, const char *text,
                       size_t length, size_t from,
                       LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS],
                       bool *depth_exceeded);

#endif  // LHATSTDLIB_REGEX_ENGINE_H
