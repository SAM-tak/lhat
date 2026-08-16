// L^ (lhat) -- LSP server: what the checker settled on at a position.
//
// 07 の 4 章 records a resolution for every name that was *used*, and a
// declaration binds a name rather than resolving one -- so a position on the
// name a let^ introduces finds nothing, and that is exactly where a reader
// stands to ask what it is. What is recorded is every use, each saying where
// the name it reached was bound, so the answer is there under another key.
//
// Both answers a reader gets about a position -- the hover and the signature
// -- have to agree about what stands there, so the rule is here rather than
// in either of them.

#ifndef LSP_RESOLUTION_H
#define LSP_RESOLUTION_H

#include <stdint.h>

#include "check.h"
#include "program_internal.h"

// The resolution covering `offset`: the one recorded at a use, or, where the
// position is on a declaration, the one a use of that name points back at.
// NULL when nothing there resolves.
const LhatResolution *lsp_resolution_at(const LhatUnit *unit, uint32_t offset);

#endif  // LSP_RESOLUTION_H
