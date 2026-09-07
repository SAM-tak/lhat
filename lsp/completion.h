// L^ (lhat) -- LSP server: what may stand where the cursor is.
//
// 07 の 4 章. Four questions wear one request, and they are told apart by
// what is written to the left of the cursor:
//
//   foo.            the members of whatever foo answers
//   import^ std.    the modules the host registered under std
//   require^ "li    the units of this workspace
//   le              the words of the language
//
// Only the first needs the checker, and it does not work the receiver out
// for itself: the parser leaves a member with nothing under it for a
// trailing dot, the checker records what stands to its left against that
// dot (check.h's LhatMemberSite), and this reads the record. A second
// reading of 14.10's lookup here would disagree with the checker exactly
// where the rules are hard, which is what 4 章 already refused for hover.
//
// The other three are text. A half-written module path resolves to nothing,
// an unterminated string is one error token running to the end of the file
// (lexer.c), and a word being typed is whatever it will be once it is
// finished -- so none of the three has a tree worth asking.

#ifndef LSP_COMPLETION_H
#define LSP_COMPLETION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "program_internal.h"

// The members that may stand after the '.' at `offset` -- a CompletionItem[],
// empty when nothing there is a member access or the receiver answers no
// members. NULL only when there was no room. The caller owns it.
//
// `offset` is a byte offset into the unit's source, standing just past the
// dot: that is where the cursor is when the dot has only now been typed.
cJSON *lsp_completion_members_for_unit(const LhatUnit *unit, uint32_t offset);

// The members of `receiver` on their own, for a caller that already has the
// type. Every member the checker would accept and no other, from two sources:
//
//   what the type holds  -- asked of lhat_type_find_member, so shadowing and
//                           14.7改2's delegation are the one search a member
//                           access uses
//   what the checker answers -- 14.19's built-ins, which no list holds; asked
//                           of lhat_check_builtin_members one spelling at a
//                           time, so which receiver takes which spelling is
//                           never written down twice
//
// `result` is where the built-ins' types are made and may be NULL, which
// leaves them out.
cJSON *lsp_completion_members_of(LhatCheckResult *result, LhatType *receiver);

// Whether the cursor stands in an `import^` path, and if so where the path
// began. Text only: `text` is the whole document, `offset` the cursor.
bool lsp_completion_import_prefix(const char *text, size_t length,
                                  uint32_t offset, uint32_t *from);

// The same for the string of a `require^`. `from` is just past the quote.
bool lsp_completion_require_prefix(const char *text, size_t length,
                                   uint32_t offset, uint32_t *from);

// Whether the cursor stands where a word of the language may be written,
// and if so where that word began -- which is the cursor itself when none of
// it has been typed. Refuses after a '.', where the receiver decides what
// may stand and the member question owns the answer.
bool lsp_completion_word_prefix(const char *text, size_t length,
                                uint32_t offset, uint32_t *from);

// Every word the language has, whatever is written so far: the editor
// filters the list down as more of the word arrives, which is what the
// answer's isIncomplete says it may do.
cJSON *lsp_completion_word_items(void);

// The next segment of every module that begins with `prefix`, once each --
// "std." offers "io" and "math", not "std.io" and "std.math.vector3". The
// modules are the distinct "module" strings the host config carries.
cJSON *lsp_completion_module_items(const char *const *modules, size_t count,
                                   const char *prefix, size_t prefix_length);

// Every candidate written as `unit_path` would have to write it: relative to
// the directory the unit is in, '/'-separated. `unit_path` and the
// candidates are absolute (lsp/uri.h). The unit's own path is left out --
// a unit does not require^ itself.
cJSON *lsp_completion_path_items(const char *unit_path,
                                 const char *const *candidates, size_t count);

// `target` as it would be written from the directory `from` stands in, with
// as many "../" as the shared prefix leaves. Malloc'd; NULL when either is
// NULL. Public for its own test -- it is the one piece here with arithmetic
// in it.
char *lsp_completion_relative_path(const char *from, const char *target);

#endif  // LSP_COMPLETION_H
