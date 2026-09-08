// L^ (lhat) -- LSP server: every place one name was written.
//
// 07 の 5 章. The checker records, for every name it resolved, where the
// meaning it reached was declared (check.h's LhatResolution). Going to a
// definition reads one such record; finding the references reads them all
// and keeps the ones pointing at the same place. The walk is the checker's
// either way, and 8 章's scoping is not read a second time.
//
// A name is therefore identified by **where it was declared** rather than by
// how it is spelt: two `count`s in two scopes are two names, and one name
// reached from three files is one. That is what makes a rename safe without
// the server knowing anything about scopes.
//
// What has no declared place answers nothing -- a name the host registered
// was declared in C (05 の 8.7), and a built-in of a string or a coroutine
// (14.19, 15.6改) was declared by the language. Neither can be renamed, and
// saying so is the right answer rather than a missing one.

#ifndef LSP_REFERENCES_H
#define LSP_REFERENCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "program_internal.h"

// The one declaration a question is about: the unit it was written in and
// where its name starts there. `path` is borrowed and outlives the answer
// (a unit's own path, or the one a member carried in type.h's declared_in).
typedef struct {
    const char *path;
    uint32_t offset;
    // What is written there, so every edit replaces the same spelling. A
    // record that points at a place holding some other name is not about
    // this name at all -- a require^'s does exactly that, pointing at the
    // start of a whole file -- and comparing catches it without this having
    // to know which kinds of record do it.
    const char *name;  // borrowed from the unit's source
    uint32_t name_length;
} LspReferenceTarget;

// What the name at `offset` in `unit` was declared as. False when nothing
// there resolves, when what it resolved to has no written place, or when
// what stands at that place is not the name that was asked about.
//
// Both ways round: a use answers with what it reached, and a declaration
// answers with itself.
bool lsp_references_target(const LhatUnit *unit, uint32_t offset,
                           LspReferenceTarget *out);

// Every use of `target` written in `unit`, in source order. `sink` is handed
// the byte range of each -- the name alone, not the construct around it, so
// a rename replaces exactly what it must.
typedef void (*LspReferenceSink)(void *context, const LhatUnit *unit,
                                 uint32_t from, uint32_t to);
void lsp_references_in_unit(const LhatUnit *unit,
                            const LspReferenceTarget *target,
                            LspReferenceSink sink, void *context);

// The declaration itself, when it stands in `unit`. Told apart from the uses
// because LSP lets a client ask for the references without it, and because a
// rename always wants it.
bool lsp_references_declaration_in(const LhatUnit *unit,
                                   const LspReferenceTarget *target,
                                   uint32_t *from, uint32_t *to);

// Whether `spelling` is a name a writer could have written (01 の 3.1), so a
// rename refuses a new name the lexer would not read back as one. Says
// nothing about whether the name is free -- 8.7's collision is the checker's
// to report, once, where it happens.
bool lsp_references_is_name(const char *spelling, size_t length);

#endif  // LSP_REFERENCES_H
