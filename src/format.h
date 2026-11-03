// L^ (lhat) -- the source formatter ([lsp.format]).
//
// One engine, two callers: the cli's --format and the language server's
// textDocument/formatting. What it reads is the token stream, written back
// with the spacing, indentation and line breaks the rules give; the syntax
// tree only says where constructs begin and end. Every token is written as
// it was spelt, so the formatted text reads as the same tokens -- and it is
// read again before it is handed back, so a rule that got that wrong is
// caught here rather than in the file.

#ifndef LHAT_FORMAT_H
#define LHAT_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LHAT_EOL_KEEP,  // whichever the input used first
    LHAT_EOL_LF,
    LHAT_EOL_CRLF,
    LHAT_EOL_CR
} LhatEndOfLine;

// [lsp.format.settings]: EditorConfig's own properties, and the two of
// L^'s that it does not have.
typedef struct {
    bool indent_with_tabs;     // indent_style
    uint32_t indent_size;      // indent_size, in columns
    uint32_t tab_width;        // tab_width: what a tab counts for in a line's length
    uint32_t max_line_length;  // max_line_length; 0 is "off"
    LhatEndOfLine end_of_line; // end_of_line
    bool final_newline;        // insert_final_newline
    bool brace_next_line;      // lhat_brace_style = next_line
    bool space_after_hat;      // lhat_space_after_hat = true
} LhatFormatOptions;

typedef enum {
    LHAT_FORMAT_OK,
    LHAT_FORMAT_SYNTAX_ERROR,   // the input does not read; nothing is changed
    LHAT_FORMAT_CHANGED_TOKENS, // the result read differently: a formatter bug
    LHAT_FORMAT_OUT_OF_MEMORY
} LhatFormatStatus;

void lhat_format_defaults(LhatFormatOptions *options);

// Lays the .editorconfig files that govern `path` over `options`, the
// farthest first, stopping at one that says root = true. Properties none of
// them sets keep the value `options` came with.
void lhat_format_read_editorconfig(const char *path, LhatFormatOptions *options);

// The formatted text of `text`, read as a unit -- or as LTON when `lton`
// ([lton]). `*out` is the caller's to lhat_free.
LhatFormatStatus lhat_format(const char *text, size_t length, bool lton,
                             const LhatFormatOptions *options, char **out,
                             size_t *out_length);

#endif  // LHAT_FORMAT_H
