// L^ (lhat) -- LSP server: textDocument/semanticTokens/full.
//
// TextMate (syntaxes/lhat.tmLanguage.json) already colours everything a
// regular expression over the raw text can tell apart -- keywords, strings,
// numbers, the punctuation of 01-lexical-structure.md 2章's '^'-suffixed
// words. What it cannot do is tell a declaration from a use, a variable
// from a function call, or a type name from an ordinary one, because that
// needs the syntax tree and what the checker made of it.
//
// That reading is the language's own now (include/lhat/semantic.h), since a
// second editor wanted it. What is left here is the encoding LSP asks for.

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
