// L^ (lhat) -- the fixes a diagnostic carries (07 §6).
//
// The titles a fix is offered under, the slots a stage stores its fixes in,
// and what a unit answers about them. A front-end source: the parser and the
// checker are what work fixes out, so a build without them holds none of
// this -- the same way completion.c answers only where there is a checker.

#include <string.h>

#include "lhat/port.h"
#include "message.h"
#include "program_internal.h"

// 07 §6: the titles a fix is offered under. A title names what the fix
// writes, since that is what a reader is choosing between; `{text}` is what
// the fix's first edit writes, for the one title that stands for any token
// ("write '}'", not "write the token").
static const LhatMessageEntry FIX_MESSAGES[] = {
    [LHAT_FIX_WRITE_TOKEN] = {"fix.write-token", "write '{text}'"},
    [LHAT_FIX_LET_TO_VAR] = {"fix.let-to-var",
        "write var^ where the name is bound"},
    [LHAT_FIX_VAR_TO_LET] = {"fix.var-to-let", "bind with let^"},
    [LHAT_FIX_WRITE_OVERRIDE] = {"fix.write-override", "write override^"},
    [LHAT_FIX_WRITE_OVERLOAD] = {"fix.write-overload", "write overload^"},
    [LHAT_FIX_REMOVE_MARKER] = {"fix.remove-marker", "remove the marker"},
    [LHAT_FIX_TABLE_MEMBERS] = {"fix.table-members", "write 't^{}'"},
    [LHAT_FIX_REMOVE_SCOPE] = {"fix.remove-scope",
        "remove the scope specifier"},
    [LHAT_FIX_REMOVE_ANNOTATION] = {"fix.remove-annotation",
        "remove this annotation"},
    [LHAT_FIX_HAND_BACK] = {"fix.hand-back",
        "write try^ to hand the failure back"},
    [LHAT_FIX_DELEGATE] = {"fix.delegate", "write await^ to delegate"},
    [LHAT_FIX_NEAR_NAME] = {"fix.near-name", "change to '{text}'"},
    [LHAT_FIX_ALL] = {"fix.all", "apply every fix that needs no reading"},
};

LhatFixSlot lhat_fix_slot(LhatFixTitle title, LhatFixConfidence confidence,
                          uint32_t offset, uint32_t length, const char *text)
{
    LhatFixSlot slot;
    memset(&slot, 0, sizeof slot);
    slot.title = lhat_fix_message(title);
    slot.confidence = confidence;
    lhat_fix_slot_add(&slot, offset, length, text);
    return slot;
}

bool lhat_fix_slot_add(LhatFixSlot *slot, uint32_t offset, uint32_t length,
                       const char *text)
{
    if (slot->edit_count >= LHAT_FIX_EDITS) {
        return false;
    }
    LhatFixEdit *edit = &slot->edits[slot->edit_count++];
    edit->offset = offset;
    edit->length = length;
    edit->text = text;
    return true;
}

size_t lhat_fix_slot_count(const LhatFixSlot *slots)
{
    size_t count = 0;
    while (slots != NULL && count < LHAT_FIX_SLOTS &&
           slots[count].title != NULL) {
        count++;
    }
    return count;
}

bool lhat_fix_slot_read(const LhatFixSlot *slots, size_t which, LhatFix *out)
{
    if (out == NULL || which >= lhat_fix_slot_count(slots)) {
        return false;
    }
    out->title_id = slots[which].title->id;
    out->confidence = slots[which].confidence;
    out->edits = slots[which].edits;
    out->edit_count = slots[which].edit_count;
    return true;
}

LHAT_MESSAGE_TABLES(lhat_fix_message_tables,
    {FIX_MESSAGES, LHAT_MESSAGE_COUNT(FIX_MESSAGES)})

const LhatMessageEntry *lhat_fix_message(size_t which)
{
    return LHAT_MESSAGE_AT(FIX_MESSAGES, which);
}

// 07 §6: the fixes the diagnostic at `index` carries, whichever stage made
// it. The lexer works none out -- a byte it cannot read says nothing about
// what belonged there -- so its diagnostics answer none.
static const LhatFixSlot *fix_slots_at(const LhatUnit *unit, size_t index)
{
    LhatStage stage = LHAT_STAGE_LEXER;
    size_t within = 0;
    if (!lhat_unit_stage_of(unit, index, &stage, &within)) {
        return NULL;
    }
    switch (stage) {
        case LHAT_STAGE_PARSER:
            return unit->parsed.diagnostics[within].fixes;
        case LHAT_STAGE_CHECKER:
            return unit->checked.diagnostics[within].fixes;
        case LHAT_STAGE_LEXER:
        default:
            return NULL;
    }
}

size_t lhat_unit_diagnostic_fix_count(const LhatUnit *unit, size_t index)
{
    return lhat_fix_slot_count(fix_slots_at(unit, index));
}

bool lhat_unit_diagnostic_fix(const LhatUnit *unit, size_t index, size_t which,
                              LhatFix *out)
{
    return lhat_fix_slot_read(fix_slots_at(unit, index), which, out);
}

size_t lhat_unit_diagnostic_fix_title(const LhatUnit *unit, size_t index,
                                      size_t which, char *out, size_t capacity)
{
    const LhatFixSlot *slots = fix_slots_at(unit, index);
    LhatFix fix;
    if (!lhat_fix_slot_read(slots, which, &fix)) {
        return lhat_message_render("", NULL, 0, out, capacity);
    }
    const LhatMessageEntry *title = slots[which].title;
    const char *text =
        lhat_program_text(unit->program, title->id, title->text);
    const char *wrote = fix.edits[0].text != NULL ? fix.edits[0].text : "";
    const LhatMessageArg arg = {"text", wrote, strlen(wrote)};
    return lhat_message_render(text, &arg, 1, out, capacity);
}

// 07 §6: two edits meet when what they replace overlaps, or when they start
// at the one place -- two insertions there have no order a reader could
// have meant.
static bool edits_meet(const LhatFixEdit *a, const LhatFixEdit *b)
{
    return a->offset == b->offset ||
           (a->offset < b->offset + b->length &&
            b->offset < a->offset + a->length);
}

size_t lhat_unit_fix_all(const LhatUnit *unit, LhatFixEdit *into,
                         size_t capacity)
{
    LhatFixEdit *taken = NULL;
    size_t count = 0;
    size_t room = 0;
    size_t diagnostics = unit != NULL ? lhat_unit_diagnostic_count(unit) : 0;
    for (size_t i = 0; i < diagnostics; i++) {
        LhatFix fix;
        if (lhat_unit_diagnostic_fix_count(unit, i) != 1 ||
            !lhat_unit_diagnostic_fix(unit, i, 0, &fix) ||
            fix.confidence != LHAT_FIX_MACHINE) {
            continue;
        }
        bool meets = false;
        for (size_t e = 0; e < fix.edit_count && !meets; e++) {
            for (size_t t = 0; t < count && !meets; t++) {
                meets = edits_meet(&fix.edits[e], &taken[t]);
            }
        }
        if (meets) {
            continue;
        }
        // Room for the whole fix before any of it goes in: a fix is taken
        // whole or not at all.
        if (room - count < fix.edit_count) {
            size_t grown = room > 0 ? room * 2 : 8;
            LhatFixEdit *bigger =
                (LhatFixEdit *)lhat_realloc(taken, grown * sizeof *taken);
            if (bigger == NULL) {
                break;
            }
            taken = bigger;
            room = grown;
        }
        for (size_t e = 0; e < fix.edit_count; e++) {
            // In source order as they go in, so the list is sorted without
            // a pass of its own.
            size_t at = count++;
            while (at > 0 && taken[at - 1].offset > fix.edits[e].offset) {
                taken[at] = taken[at - 1];
                at--;
            }
            taken[at] = fix.edits[e];
        }
    }
    for (size_t t = 0; t < count && t < capacity; t++) {
        into[t] = taken[t];
    }
    lhat_free(taken);
    return count;
}

size_t lhat_unit_fix_all_title(const LhatUnit *unit, char *out,
                               size_t capacity)
{
    const LhatMessageEntry *title = lhat_fix_message(LHAT_FIX_ALL);
    return lhat_message_render(
        lhat_program_text(unit != NULL ? unit->program : NULL, title->id,
                          title->text),
        NULL, 0, out, capacity);
}
