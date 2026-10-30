// L^ (lhat) -- LSP server: where the name at a position was written.

#include "definition.h"

#include <ctype.h>
#include <string.h>

#include "check.h"
#include "lhat/lexer.h"

bool lsp_definition_for_unit(const LhatUnit *unit, uint32_t offset,
                             LspDefinitionSite *out)
{
    if (unit == NULL || out == NULL) {
        return false;
    }
    // A use, and only a use. lsp_resolution_at also answers on a declaration,
    // by reading the record a use of that name left -- which is what a hover
    // wants and the opposite of what this does: standing on the name a let^
    // introduces, the place to go to is already here, and jumping off to some
    // use of it would be going the wrong way.
    const LhatResolution *resolved =
        lhat_check_resolution_at(&unit->checked, offset);
    if (resolved == NULL || !resolved->has_definition) {
        return false;
    }
    // A member carries the unit it was written in whether or not that is
    // this one (type.h), and a reader of this answer wants to know which --
    // so the one case that needs no file named is named by NULL here rather
    // than by every caller comparing paths.
    out->path = resolved->definition_path;
    if (out->path != NULL && unit->path != NULL &&
        strcmp(out->path, unit->path) == 0) {
        out->path = NULL;
    }
    out->offset = resolved->definition;
    return true;
}

// Where the name covering `offset` begins. A declaration is recorded as one
// offset while a cursor may stand anywhere within the name, so a position is
// located with the lexer, including spaces and escaped delimiters.
uint32_t lsp_definition_name_start(const LhatSource *source, uint32_t offset)
{
    LhatLexer lexer;
    lhat_lexer_init(&lexer, source);
    uint32_t at = (uint32_t)source->length;
    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        if (token.kind == LHAT_TOKEN_EOF || token.offset > offset) {
            break;
        }
        if (token.kind == LHAT_TOKEN_IDENT || token.kind == LHAT_TOKEN_HAT_IDENT) {
            if (offset < token.offset + token.length) {
                at = token.offset;
                break;
            }
            if (offset == token.offset + token.length) at = token.offset;
        }
    }
    lhat_lexer_dispose(&lexer);
    return at;
}

uint32_t lsp_definition_name_end(const char *text, size_t length,
                                 uint32_t offset)
{
    // 01 の 3.1: the lexer takes any non-ASCII that is neither space nor a
    // reserved mark as part of a name, so this does too -- the same walk
    // resolution.c makes backwards, made forwards.
    uint32_t at = offset <= length ? offset : (uint32_t)length;
    if (at < length && text[at] == '`') {
        at++;
        while (at < length && text[at] != '\n' && text[at] != '\r') {
            if (text[at++] == '`') {
                if (at < length && text[at] == '`') {
                    at++;
                } else {
                    return at;
                }
            }
        }
        return offset;
    }
    while (at < length) {
        unsigned char c = (unsigned char)text[at];
        if (!(isalnum(c) || c == '_' || c >= 0x80)) {
            break;
        }
        at++;
    }
    return at;
}
