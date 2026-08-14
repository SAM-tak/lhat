// L^ (lhat) -- LSP server: byte offset -> LSP Position.
//
// lhat's line/column count Unicode code points (src/lexer.c). LSP's
// Position.character counts UTF-16 code units. 03-compilation-pipeline.md's
// 1.3節 already keeps two counts of a column apart -- what a diagnostic
// names (code points) and where a terminal's mark lands (cells, UAX #11).
// This is the third, shared by diagnostics.c and semantic_tokens.c since
// both turn a byte offset into a client-visible position.

#ifndef LSP_POSITION_H
#define LSP_POSITION_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int line;       // 0-based
    int character;  // 0-based, UTF-16 code units
} LspPosition;

// `byte_offset` is clamped to `text_length` rather than read out of bounds.
LspPosition lsp_position_at(const char *text, size_t text_length,
                            uint32_t byte_offset);

// The other way: LSP gives a line and a UTF-16 character, and lhat counts
// bytes. A position past the end of its line stops at the line's end rather
// than running into the next.
uint32_t lsp_offset_at(const char *text, size_t text_length, int line,
                       int character);

#endif  // LSP_POSITION_H
