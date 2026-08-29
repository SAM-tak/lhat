// L^ (lhat) -- LSP server: textDocument/documentSymbol.
//
// The outline of one unit: what it declares, nested the way it was written.
// 07 の 5 章 said this could be made from the syntax tree alone, and it can --
// a let^, a def^'s entries, an errordef^'s kinds and a module^ all spell
// their names in the source, and none of them waits on the checker. So the
// tree is the whole of what this reads, and a unit that will not check still
// has an outline.
//
// What is listed, and as which of LSP's SymbolKinds:
//
//   let^ x = f^ ...          Function (Method inside a def^), its locals under it
//   let^ X = def^{ ... }     Class: the self^{ } template's fields as Field,
//                            the rest as Property / Method
//   let^ t = { ... }         Object, its named entries under it
//   let^ m = import^ a.b     Module
//   let^ x = ...             Constant; var^ x = ...  Variable
//   errordef^ E { A, B }     Enum, the kinds as EnumMember, their fields as Field
//   module^ a.b.c            Module
//   return^ { ... }          the unit's own answer, when it is a table
//                            literal: its entries at the top -- which is
//                            what a .lton is, once wrapped (lsp/lton.h)
//
// A loop's focus, a for^'s bound and the callbacks an argument list holds
// are walked past on purpose: an outline is the file's shape, not every
// name in it.

#ifndef LSP_DOCUMENT_SYMBOL_H
#define LSP_DOCUMENT_SYMBOL_H

#include "cJSON.h"
#include "program_internal.h"

// The LSP DocumentSymbol[] for `unit` -- the tree as parsed, positions
// through lsp_unit_position_at (position.h). An empty array when the unit
// holds no tree; NULL only on allocation failure. The caller owns it.
cJSON *lsp_document_symbols_for_unit(const LhatUnit *unit);

#endif  // LSP_DOCUMENT_SYMBOL_H
