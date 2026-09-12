// L^ (lhat) -- what may stand where the cursor is.
//
// 07 の 4 章. Four questions wear one request, told apart by what is written
// to the left of the cursor:
//
//   foo.            the members of whatever foo answers
//   le              the words of the language, and the names in scope
//   import^ std.    the modules the host registered under std
//   require^ "li    the units of this workspace
//
// The first two ask the checker and work nothing out for themselves. The
// receiver of a dot is read off the record the checker left as it settled it;
// the names in scope are read off the record each scope left as it closed.
// Reading 02 の 14.10's lookup or 8 章's scoping a second time outside the
// checker would disagree with it exactly where the rules are hard.
//
// Public because a host cannot compute it, which is lhat/semantic.h's reason
// word for word: what crosses is the answer and not the machinery. An editor
// embedded in an engine wants completion as much as a language server does,
// and neither can reach LhatMemberSite or LhatBindingSite.
//
// The other two are the host's own. A module list is what the host
// registered and a unit list is what its filesystem holds; this says only
// WHICH question the cursor is asking and where what is being typed began,
// so that a host offering its own candidates offers them in the right place.
//
// Not reached by lhat.h, which is the header for running a program.

#ifndef LHAT_COMPLETION_H
#define LHAT_COMPLETION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/program.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which of the four the cursor is asking.
typedef enum {
    // A comment, a string that is not a require^'s, or a place no name may
    // stand. Nothing is offered rather than everything.
    LHAT_COMPLETION_NOTHING = 0,
    // A '.' stands before the cursor: what may follow is the receiver's to
    // say and nothing else's.
    LHAT_COMPLETION_MEMBER,
    // A word is being written -- a name in scope, or a word of the language.
    LHAT_COMPLETION_WORD,
    // Inside an import^ path. The host offers its registered modules.
    LHAT_COMPLETION_MODULE,
    // Inside the string of a require^. The host offers its units.
    LHAT_COMPLETION_UNIT
} LhatCompletionAsk;

// Coarse the way LSP's own CompletionItemKind is, and for the same reason as
// lhat/semantic.h's list: these are the distinctions an editor draws.
typedef enum {
    LHAT_COMPLETION_VARIABLE = 0,
    LHAT_COMPLETION_FIELD,
    LHAT_COMPLETION_METHOD,    // 02 の 14.10: reached through a value
    LHAT_COMPLETION_FUNCTION,  // reached through the definition
    LHAT_COMPLETION_CLASS,     // 14.1: what a def^ made, and a registered type
    LHAT_COMPLETION_MODULE_NAME,
    LHAT_COMPLETION_WORD_OF_LANGUAGE,
    LHAT_COMPLETION_CONSTANT
} LhatCompletionKind;

// A name is a name: 01 の 2.3's hats are part of it and a qualified path is
// never offered whole, so this is the widest a single segment plus its marks
// can be. A type written out is cut with an ellipsis past its room, as
// lhat_type_write does everywhere else -- past that a reader is helped by
// opening the definition instead.
#define LHAT_COMPLETION_LABEL 128
#define LHAT_COMPLETION_DETAIL 256

typedef struct {
    LhatCompletionKind kind;
    char label[LHAT_COMPLETION_LABEL];
    // The type the name holds, written out. Empty where there is none to
    // write -- a word of the language has no type.
    char detail[LHAT_COMPLETION_DETAIL];
} LhatCompletionItem;

// Which question stands at `offset`, and where what is being typed began.
//
// `from` is written for every answer but NOTHING, and is the cursor itself
// where none of the word has been typed yet. An item that replaces a whole
// path replaces from there. It may be NULL.
//
// Reads the text alone, which is why it answers while a buffer is half
// typed: a module path that is still being written resolves to nothing and
// an unterminated string is one error token running to the end of the file,
// so neither has a tree worth asking. MEMBER is the exception and does ask
// the checker, since only the checker knows a dot was an access rather than
// a decimal point.
LhatCompletionAsk lhat_unit_completion_ask(const LhatUnit *unit,
                                           uint32_t offset, uint32_t *from);

// The same off the text alone, for a caller that has no checked unit yet.
//
// Never answers MEMBER: only the checker knows a dot was an access rather
// than the point of a number, so a caller wanting that answer checks first
// and asks the call above. What this is for is the other three -- a module
// path and a require^ string are worth answering before a check is spent,
// since neither has a tree worth asking either way.
LhatCompletionAsk lhat_completion_ask_text(const char *text, size_t length,
                                           uint32_t offset, uint32_t *from);

// What may stand at `offset`, for the two questions the checker owns.
//
// Fills `into` and answers how many there are, as lhat_unit_semantic_names
// does: a count larger than `capacity` says the array was filled as far as it
// went and the rest were counted, so measuring is a call with (NULL, 0).
//
// MEMBER answers the members of the receiver -- every one the checker would
// accept and no other, from the type's own members (so shadowing and
// 14.7改2's delegation are the one search a member access uses) and from
// 14.19's built-ins, which no list holds and which are asked of the checker
// one spelling at a time.
//
// WORD answers the names in scope innermost first, then the words of the
// language. A name that shadows another comes before the one it shadows, and
// a word whose spelling a name already took is left out: what the writer can
// reach is what they meant.
//
// MODULE and UNIT answer 0 -- those lists are the host's. NOTHING answers 0.
//
// Answers 0 for a unit that did not check, and 0 in a build without
// LHAT_WITH_RESOLUTIONS: what a name means is what the checker recorded, and
// that build records nothing.
size_t lhat_unit_completion_items(const LhatUnit *unit, uint32_t offset,
                                  LhatCompletionItem *into, size_t capacity);

// The words of the language on their own, filled and counted the same way.
//
// A buffer with nothing in it yet has no unit to ask and still has these to
// offer, which is why they are reachable without one. They are also the one
// part of an answer that no list anywhere else holds: 01 の 2.1 keeps no
// keyword table -- every hatted word is the one token kind and the parser
// decides which of them each is -- so a host writing its own would be
// writing the first.
//
// Candidates, not an authority. Nothing is checked against this and nothing
// is refused by it: a word missing is one suggestion that does not appear,
// and a word here the language no longer takes is one the checker reports
// the moment it is written.
size_t lhat_completion_words(LhatCompletionItem *into, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_COMPLETION_H
