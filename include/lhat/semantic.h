// L^ (lhat) -- what each name in a checked unit turned out to mean.
//
// [lsp.hover]. Where lhat/lexer.h says where a word begins and ends, this says
// what the word was: a class the writer declared, a module reached through, a
// parameter, a call. None of it is reachable from the text -- [binary.containers.bytes] reserves no
// word and `Godot.Sprite2D` is spelled exactly as `self^.ticks` is, so what
// tells them apart is what the checker resolved each name to.
//
// An editor colouring L^ wants both layers, and wants them separately: the
// lexical one answers while a buffer is half-typed, and this one answers once
// the unit checks. VSCode already reads them as two (a TextMate grammar under
// the language server's tokens); this is the second, for any host.
//
// Public because a host cannot compute it. The classification reads what the
// checker recorded and what a type turned out to be -- neither of which
// leaves src/ -- so what crosses is the answer and not the machinery.
//
// Not reached by lhat.h, which is the header for running a program.

#ifndef LHAT_SEMANTIC_H
#define LHAT_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/program.h"

#ifdef __cplusplus
extern "C" {
#endif

// Coarse on purpose, and coarse the same way LSP's own legend is: these are
// the distinctions an editor draws, not the ones the checker makes. A host
// with fewer colours than this folds them; one with more has nothing here to
// split them by.
typedef enum {
    LHAT_SEMANTIC_NAMESPACE,  // [modules.host.machine]: a table names are reached through
    LHAT_SEMANTIC_TYPE,       // number^, and a type a host registered
    LHAT_SEMANTIC_CLASS,      // [syntax.obj.def]: what a def^ made
    LHAT_SEMANTIC_PARAMETER,
    LHAT_SEMANTIC_VARIABLE,
    LHAT_SEMANTIC_FUNCTION,
    LHAT_SEMANTIC_PROPERTY
} LhatSemanticKind;

typedef struct {
    LhatSemanticKind kind;
    // Into the unit's normalised text, and without the hats: [lex.hat.repeat] makes
    // them part of the name, and an editor drawing the word apart from its
    // mark wants the word. lhat/lexer.h's span is where the whole of it is.
    uint32_t offset;
    uint32_t length;  // bytes
    // Where the name was introduced rather than used.
    bool declaration;
    bool readonly;  // [syntax.stmt.let]: bound by a let^ rather than a var^
    // [syntax.obj.substring], [syntax.obj.tostring-hat], [syntax.sub.coroutine-members]: the language answered this name itself --
    // `length` on a string, `resume` on a coroutine, `message` on an error.
    // Nothing in the source wrote it and no host registered it, so an editor
    // drawing it draws the library's rather than the program's. Both
    // spellings answer alike: [syntax.obj.tostring-hat] has the hatted one always reach the
    // built-in, and the bare one reach it everywhere but a plain table.
    bool builtin;
} LhatSemanticName;

// Fills `into` in offset order and answers how many the unit has. A count
// larger than `capacity` says the array was filled as far as it went and the
// rest were counted -- ask again with room, or colour what came back. So
// measuring is a call with (NULL, 0), as lhat_program_dump_host_api is.
//
// One name written once answers once, even where the tree holds it twice:
// [lex.op.compound] expands 'a[i] += 1' into the reassignment and the 'a[i] + 1' it
// stands for, both carrying the same spans, while saying the target is read
// exactly once.
//
// Answers 0 for a unit that did not check, and 0 in a build without
// LHAT_WITH_RESOLUTIONS -- what a name means is what the checker recorded,
// and that build records nothing.
size_t lhat_unit_semantic_names(const LhatUnit *unit, LhatSemanticName *into,
                                size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_SEMANTIC_H
