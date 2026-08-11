// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.
//
// TextMate (syntaxes/lhat.tmLanguage.json) already colours everything a
// regular expression over the raw text can tell apart -- keywords, strings,
// numbers, the punctuation of 01-lexical-structure.md 2章's '^'-suffixed
// words. What it cannot do is tell a declaration from a use, a variable
// from a function call, or a type name from an ordinary one, because that
// needs the syntax tree. This file walks parsed.root (no type checking
// involved -- 03 の 4.2 keeps the tree's shape independent of strict/relaxed,
// and nothing here needs a type, only what kind of name a position is)
// and classifies every identifier by the AST context it sits in.
//
// What this does NOT do: tell a local variable from a captured one, or
// a variable holding a function from an actual function declaration --
// either needs check.c's name resolution, which currently resolves a name
// to a type and discards the binding it came from (infer_name(), check.c)
// rather than writing anything back to the tree. That is a second phase,
// deliberately deferred.

#ifndef LSP_SEMANTIC_TOKENS_H
#define LSP_SEMANTIC_TOKENS_H

#include <stddef.h>

#include "cJSON.h"
#include "program_internal.h"

// The legend initialize.c's capabilities response advertises. Token type
// and modifier indices this file emits are indices into these arrays, so
// the two files have to agree -- semantic_tokens.c is the only place that
// assigns the indices, and initialize.c just echoes these back.
extern const char *const LSP_SEMANTIC_TOKEN_TYPES[];
extern const size_t LSP_SEMANTIC_TOKEN_TYPES_COUNT;
extern const char *const LSP_SEMANTIC_TOKEN_MODIFIERS[];
extern const size_t LSP_SEMANTIC_TOKEN_MODIFIERS_COUNT;

// The LSP SemanticTokens.data array: delta-encoded quintuples
// (deltaLine, deltaStartChar, length, tokenType, tokenModifiers), sorted by
// position as the spec requires. A cJSON array of numbers; NULL only on
// allocation failure.
cJSON *lsp_semantic_tokens_for_unit(const LhatUnit *unit);

#endif  // LSP_SEMANTIC_TOKENS_H
