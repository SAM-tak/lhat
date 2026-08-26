// L^ (lhat) -- LSP server: byte offset -> LSP Position.

#include "position.h"

#include "lton.h"

// One code point from `text[*at..)`, `*at` moved past it. Malformed input is
// read one byte at a time, same as error.c's next_character -- but this
// counts UTF-16 units rather than terminal cells, so it is not the same
// function wearing a different name; the two answer different questions
// from the same kind of walk.
static uint32_t decode_and_advance(const char *text, size_t length, size_t *at)
{
    size_t i = *at;
    unsigned char lead = (unsigned char)text[i];
    size_t follow = 0;
    uint32_t code = lead;

    if (lead >= 0xF0u) {
        follow = 3;
        code = lead & 0x07u;
    } else if (lead >= 0xE0u) {
        follow = 2;
        code = lead & 0x0Fu;
    } else if (lead >= 0xC0u) {
        follow = 1;
        code = lead & 0x1Fu;
    }
    if (i + follow >= length) {
        *at = i + 1;
        return lead;
    }
    for (size_t k = 1; k <= follow; k++) {
        unsigned char byte = (unsigned char)text[i + k];
        if ((byte & 0xC0u) != 0x80u) {
            *at = i + 1;
            return lead;
        }
        code = (code << 6) | (byte & 0x3Fu);
    }
    *at = i + follow + 1;
    return code;
}

uint32_t lsp_offset_at(const char *text, size_t text_length, int line,
                       int character)
{
    size_t i = 0;
    for (int seen = 0; seen < line && i < text_length; i++) {
        if (text[i] == '\n') {
            seen++;
        }
    }
    int units = 0;
    while (i < text_length && text[i] != '\n' && units < character) {
        unsigned char lead = (unsigned char)text[i];
        size_t sequence = 1;
        int width = 1;
        if (lead >= 0xF0) {
            sequence = 4;
            width = 2;  // beyond the BMP: a surrogate pair
        } else if (lead >= 0xE0) {
            sequence = 3;
        } else if (lead >= 0xC0) {
            sequence = 2;
        }
        if (i + sequence > text_length) {
            break;
        }
        i += sequence;
        units += width;
    }
    return (uint32_t)i;
}

LspPosition lsp_position_at(const char *text, size_t text_length,
                            uint32_t byte_offset)
{
    size_t offset = byte_offset <= text_length ? byte_offset : text_length;

    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n') {
        line_start--;
    }
    int line = 0;
    for (size_t i = 0; i < line_start; i++) {
        if (text[i] == '\n') {
            line++;
        }
    }

    int character = 0;
    size_t at = line_start;
    while (at < offset) {
        uint32_t code = decode_and_advance(text, text_length, &at);
        character += code >= 0x10000u ? 2 : 1;
    }

    LspPosition pos;
    pos.line = line;
    pos.character = character;
    return pos;
}

// 08-lton.md: an LTON file is wrapped before the front end reads it, and the
// prologue sits on the line the file's own first line does -- so a line is a
// line either way, and only the first line's columns are shifted by it.
LspPosition lsp_unit_position_at(const LhatUnit *unit, uint32_t byte_offset)
{
    LspPosition at = lsp_position_at(unit->source.text, unit->source.length,
                                     byte_offset);
    if (at.line == 0 && lsp_lton_is_path(unit->path)) {
        uint32_t shift = lsp_lton_prologue_length();
        at.character = at.character > (int)shift ? at.character - (int)shift : 0;
    }
    return at;
}

uint32_t lsp_unit_offset_at(const LhatUnit *unit, int line, int character)
{
    if (line == 0 && lsp_lton_is_path(unit->path)) {
        character += (int)lsp_lton_prologue_length();
    }
    return lsp_offset_at(unit->source.text, unit->source.length, line,
                         character);
}
