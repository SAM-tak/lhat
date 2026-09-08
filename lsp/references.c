// L^ (lhat) -- LSP server: every place one name was written.

#include "references.h"

#include <ctype.h>
#include <string.h>

#include "check.h"

#include "definition.h"

// 01 の 3.1: what a name is made of. The lexer takes any non-ASCII that is
// neither space nor a reserved mark, so this does too -- the same rule
// resolution.c and definition.c already walk by.
static bool is_name_byte(unsigned char c)
{
    return isalnum(c) || c == '_' || c >= 0x80;
}

// Where the name covering `offset` begins. A declaration is recorded as one
// offset while a cursor may stand anywhere within the name, so a position is
// walked back to the start of what it is inside.
static uint32_t name_start_at(const char *text, size_t length, uint32_t offset)
{
    uint32_t at = offset <= length ? offset : (uint32_t)length;
    while (at > 0 && is_name_byte((unsigned char)text[at - 1])) {
        at--;
    }
    return at;
}

// Which unit a record's declaration lives in. NULL on the record means this
// same unit (definition.c says the same thing the other way round).
static const char *owner_of(const LhatUnit *unit, const LhatResolution *entry)
{
    return entry->definition_path != NULL ? entry->definition_path
                                          : unit->path;
}

static bool same_place(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

// Whether the name written at `offset` in `unit` is the target's, which is
// what keeps a record pointing at some other name out of the answer. A
// require^'s record points at offset 0 of a whole file -- where that file's
// own first declaration also stands -- and this is what tells the two apart
// without a list of the kinds of record that do it.
static bool name_here_is(const LhatUnit *unit, uint32_t offset,
                         const LspReferenceTarget *target)
{
    if (unit->source.text == NULL || offset > unit->source.length) {
        return false;
    }
    uint32_t end = lsp_definition_name_end(unit->source.text,
                                           unit->source.length, offset);
    return end - offset == target->name_length &&
           memcmp(unit->source.text + offset, target->name,
                  target->name_length) == 0;
}

bool lsp_references_target(const LhatUnit *unit, uint32_t offset,
                           LspReferenceTarget *out)
{
    if (unit == NULL || out == NULL || unit->source.text == NULL) {
        return false;
    }
    // Where the name under the cursor begins and ends. Asked first, and asked
    // of the position rather than of a record, because a cursor sitting just
    // past a name is still on it as far as a reader is concerned -- and a
    // record spans the name itself, so looking one byte past would miss.
    uint32_t from = name_start_at(unit->source.text, unit->source.length,
                                  offset);
    uint32_t to = lsp_definition_name_end(unit->source.text,
                                          unit->source.length, from);
    if (to <= from) {
        return false;  // not standing on a name at all
    }
    // 01 の 2.3: the hat is part of the name, and a hatted word is the
    // language's own -- f^, let^, self^. None of them was declared anywhere
    // a rename could edit.
    if (to < unit->source.length && unit->source.text[to] == '^') {
        return false;
    }
    // And what is not a name is not one to rename either: a number sits in
    // the same bytes a name is made of but for the digit it begins with.
    if (!lsp_references_is_name(unit->source.text + from, to - from)) {
        return false;
    }

    // A record here means the cursor is on a *use*, and the record says what
    // it reached. No record means the cursor is on something no use points
    // at from within this unit -- a declaration, whose uses may all be in
    // other files or may not exist yet -- and the position is then the whole
    // of what is known about it.
    //
    // The two are told apart rather than merged, because a use that reached
    // something declared nowhere here (a host name, a built-in, a member of
    // a plain table literal) must answer nothing at all. Taking it for a
    // declaration of its own would let a rename edit that one place and
    // leave every other one behind.
    const LhatResolution *use = lhat_check_resolution_at(&unit->checked, from);
    const char *path = NULL;
    uint32_t at = 0;
    if (use != NULL) {
        if (!use->has_definition) {
            return false;
        }
        path = owner_of(unit, use);
        at = use->definition;
    } else {
        path = unit->path;
        at = from;
    }
    if (path == NULL) {
        return false;
    }

    out->path = path;
    out->offset = at;
    out->name = unit->source.text + from;
    out->name_length = to - from;

    // The declaration in this very unit has to hold that name. Across units
    // the check waits until the other one is walked (references_declaration_in
    // makes it there), since its text is not in hand here.
    if (same_place(path, unit->path) && !name_here_is(unit, at, out)) {
        return false;
    }
    return true;
}

void lsp_references_in_unit(const LhatUnit *unit,
                            const LspReferenceTarget *target,
                            LspReferenceSink sink, void *context)
{
    if (unit == NULL || target == NULL || sink == NULL) {
        return;
    }
    const LhatCheckResult *checked = &unit->checked;
    // The records are in use order (chk_settle_resolutions), which is source
    // order -- so what is handed on is already what a reader expects to read.
    for (size_t i = 0; i < checked->resolution_count; i++) {
        const LhatResolution *entry = &checked->resolutions[i];
        if (!entry->has_definition || entry->definition != target->offset ||
            !same_place(owner_of(unit, entry), target->path)) {
            continue;
        }
        // 01 の 2.3: the hat is part of a name, and a use written with one
        // is a different name from the same word without. Comparing the
        // spelling keeps them apart, and keeps a record that points at some
        // other name out of the answer at the same time.
        if (!name_here_is(unit, entry->use, target)) {
            continue;
        }
        sink(context, unit, entry->use, entry->use_end);
    }
}

bool lsp_references_declaration_in(const LhatUnit *unit,
                                   const LspReferenceTarget *target,
                                   uint32_t *from, uint32_t *to)
{
    if (unit == NULL || target == NULL ||
        !same_place(unit->path, target->path) ||
        !name_here_is(unit, target->offset, target)) {
        return false;
    }
    *from = target->offset;
    *to = target->offset + target->name_length;
    return true;
}

bool lsp_references_is_name(const char *spelling, size_t length)
{
    // 01 の 3.1: a name does not begin with a digit, and every byte of it is
    // one a name is made of. The hat is left out on purpose -- 2.3 makes a
    // hatted word the language's own, and none of them is a name a writer
    // bound (07 の 4 章's WORDS is the list of them).
    if (spelling == NULL || length == 0 ||
        isdigit((unsigned char)spelling[0])) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!is_name_byte((unsigned char)spelling[i])) {
            return false;
        }
    }
    return true;
}
