// L^ (lhat) -- built-in value operations.

#include "vm_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "number.h"
#include "lhat/port.h"

// 02 の 14.21: the whole number `toward` picks -- floor, ceil or nearbyint.
//
// 14.8改: an integer while it can be one. Past what an int64 names, the real
// is already whole (every double that large is), so it answers as itself --
// and an infinity or a NaN falls out of the same test rather than wanting one
// of its own, since neither is inside the range.
//
// nearbyint reads the rounding mode, whose default is to nearest with a half
// going to the even side. printf's "%.0f" reads the same mode, which is why
// 14.21's round and 14.17's format agree on a half -- one setting, not two
// implementations that happen to match.
static LhatValue whole_of(LhatValue value, double (*toward)(double))
{
    if (lhat_is_integer(value)) {
        return value;  // already the whole number it names
    }
    double whole = toward(lhat_as_real(value));
    if (whole >= -9223372036854775808.0 && whole < 9223372036854775808.0) {
        return lhat_integer((int64_t)whole);
    }
    return lhat_real(whole);
}

// 02 の 14.19: an ordinal as written. 14.8 makes number^ one type of two
// representations, so 3 and 3.0 name the same character -- and a value that
// came out of a division is rounded rather than refused, since a real is the
// ordinary answer there.
//
// The rounding is floor(x + 0.5) and not the round of arithmetic. A negative
// ordinal is resolved by adding an integer, and only a rounding that
// commutes with that gives one answer whichever order the two happen in;
// rounding away from zero does not.
bool vm_ordinal_of(LhatValue value, int64_t *out)
{
    if (lhat_is_integer(value)) {
        *out = lhat_as_integer(value);
        return true;
    }
    if (!lhat_is_number(value)) {
        return false;
    }
    double real = lhat_number_as_real(value);
    if (!(real > -9.0e15 && real < 9.0e15)) {
        return false;  // past what an ordinal could name either way
    }
    *out = (int64_t)floor(real + 0.5);
    return true;
}

// 14.19: a written ordinal as a position counting from 1. A negative one
// counts from the end, so -1 is the last character.
int64_t vm_resolve_ordinal(int64_t written, size_t count)
{
    if (written < 0) {
        return (int64_t)count + 1 + written;
    }
    return written;  // 0 stays 0, which no position is, and the caller refuses
}
// ---------------------------------------------------------------------------
// 02 の 14.19改3: the plain string searches
// ---------------------------------------------------------------------------

// The needle's first stand at or after `from`, byte positions both ways.
// A well-formed UTF-8 needle begins with a lead byte and lead bytes never
// continue anything, so a hit always lands on a character boundary.
static bool find_bytes(const char *text, size_t length, size_t from,
                       const char *needle, size_t needle_length, size_t *at)
{
    if (needle_length == 0) {
        *at = from <= length ? from : length;
        return from <= length;
    }
    for (size_t i = from; i + needle_length <= length; i++) {
        if (text[i] == needle[0] &&
            memcmp(text + i, needle, needle_length) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

// How many characters begin inside [0, until) -- continuation bytes carry
// no ordinal of their own.
static size_t characters_before(const char *text, size_t until)
{
    size_t count = 0;
    for (size_t i = 0; i < until; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

// The walk findall answers: every non-overlapping stand of the needle, as
// 1-based character ordinals. The haystack rides the coroutine's `held`;
// this state owns its copy of the needle.
typedef struct {
    const LhatString *subject;  // kept alive by `held`
    size_t next_byte;
    size_t chars_before;        // characters before next_byte
    size_t needle_length;
    char needle[];
} FindWalk;

static bool findall_step(struct LhatMachine *machine, void *context,
                         const LhatValue *sent, size_t sent_count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)sent;
    (void)sent_count;
    FindWalk *walk = (FindWalk *)context;
    if (walk->needle_length == 0) {
        return false;  // nowhere to stand; an empty needle walks nothing
    }
    size_t at = 0;
    if (!find_bytes(walk->subject->text, walk->subject->length,
                    walk->next_byte, walk->needle, walk->needle_length,
                    &at)) {
        return false;
    }
    size_t ordinal = walk->chars_before +
                     characters_before(walk->subject->text + walk->next_byte,
                                       at - walk->next_byte) +
                     1;
    // The next search starts past this stand, and the ordinal count moves
    // with it -- counted over the span walked, never from the top again.
    size_t past = at + walk->needle_length;
    walk->chars_before =
        ordinal - 1 + characters_before(walk->subject->text + at, past - at);
    walk->next_byte = past;
    answers[0] = lhat_integer((int64_t)ordinal);
    *answer_count = 1;
    return true;
}

static void findall_release(struct LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    lhat_free(context);
}

LhatRunStatus vm_call_native(Machine *m, const LhatNative *native,
                             size_t into, size_t first, uint8_t b,
                             unsigned prepared)
{
    LhatValue sent = b > 0 ? lhat_slots_get(m->slots, first) : lhat_nil();

    // 05 の 8.6: the one thing a program cannot arrange for
    // itself. It takes nothing and answers nothing.
    if (native->kind == LHAT_NATIVE_COLLECTGARBAGE) {
        if (b != 0) {
            return LHAT_RUN_ARITY;
        }
        lhat_gc_collect(m);
        lhat_slots_set(m->slots, into, lhat_nil());
        return LHAT_RUN_OK;
    }

    // 05 の 8.9: the box's two members. get answers the value
    // whole -- the box's run is head-shaped, so the placement
    // a host's answer takes lays it down unchanged. set
    // writes a value of the same tag over the bytes.
    // 02 の 14.22: the table operations, through one door.
    // The arguments are copied out of the window first -- a
    // sort's comparator runs nested L^ code over this frame
    // -- and the answer lands over R(a) afterwards.
    if (native->kind >= LHAT_NATIVE_JOIN &&
        native->kind <= LHAT_NATIVE_CLEAR) {
        LhatValue held[4];
        if (b > 4) {
            return LHAT_RUN_ARITY;
        }
        for (size_t i = 0; i < (size_t)b; i++) {
            held[i] = lhat_slots_get(m->slots, first + i);
        }
        LhatValue answered = lhat_nil();
        LhatRunStatus asked = vm_table_native(m, native, held,
                                           (size_t)b,
                                           &answered);
        if (asked != LHAT_RUN_OK) {
            return asked;
        }
        lhat_slots_set(m->slots, into, answered);
        return LHAT_RUN_OK;
    }

    if (native->kind == LHAT_NATIVE_BOX_GET ||
        native->kind == LHAT_NATIVE_BOX_SET) {
        LhatHostValueBox *box =
            (LhatHostValueBox *)lhat_as_object(native->bound);
        const LhatHostValueTag *box_tag =
            lhat_hostvalue_box_tag(box);
        if (native->kind == LHAT_NATIVE_BOX_GET) {
            if (b != 0) {
                return LHAT_RUN_ARITY;
            }
            LhatValue answered;
            answered.tag = LHAT_VALUE_HOSTVALUE;
            answered.as.hostvalue_run = box->run;
            // As at a host call's answer: the site has to
            // have reserved the width.
            if (prepared < box_tag->width ||
                !vm_place_hostvalue_answer(m, into,
                                        answered)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            return LHAT_RUN_OK;
        }
        if (b != 1) {
            return LHAT_RUN_ARITY;
        }
        if (!lhat_is_hostvalue(sent) ||
            lhat_as_hostvalue_tag(sent) != box_tag) {
            return LHAT_RUN_TYPE_ERROR;
        }
        // 14.11: a prototype's box takes no writes.
        if (box->sealed) {
            return LHAT_RUN_SEALED;
        }
        for (size_t i = 1; i < box_tag->width; i++) {
            box->run[i] = m->slots.values[first + i];
        }
        lhat_slots_set(m->slots, into, lhat_nil());
        return LHAT_RUN_OK;
    }

    // 02 の 14.17: the value written down. Takes nothing, or
    // a format when what it is bound to is a number^ -- the
    // two signatures 14.12 makes an intersection of, told
    // apart here by how many arguments arrived.
    if (native->kind == LHAT_NATIVE_TOSTRING) {
        if (b > 1) {
            return LHAT_RUN_ARITY;
        }
        size_t needed;
        char *text;
        if (b == 1) {
            if (!lhat_is_number(native->bound)) {
                // Only a number^ carries the second
                // signature, so this call was never one of
                // the two ways of writing this value.
                return LHAT_RUN_ARITY;
            }
            LhatValue fmt = sent;
            if (!lhat_is_object_kind(fmt, LHAT_OBJECT_STRING)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            const LhatString *spelt =
                (const LhatString *)lhat_as_object(fmt);
            if (!lhat_number_format(native->bound, spelt->text,
                                    spelt->length, NULL, 0,
                                    &needed)) {
                return LHAT_RUN_BAD_FORMAT;
            }
            text = (char *)lhat_alloc(needed + 1);
            if (text == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            lhat_number_format(native->bound, spelt->text,
                               spelt->length, text, needed + 1,
                               &needed);
        } else {
            // 05 の 8.9: a bound host value is the head
            // alone -- the tag, no bytes -- so the name is
            // all there is to write. lhat_value_text reads
            // the pointer form (a host's argument) and must
            // not see this one.
            if (lhat_is_hostvalue(native->bound)) {
                const LhatHostValueTag *bound_tag =
                    lhat_as_hostvalue_tag(native->bound);
                size_t module_len = strlen(bound_tag->module);
                size_t name_len = strlen(bound_tag->name);
                needed = module_len + name_len + 3;
                text = (char *)lhat_alloc(needed + 1);
                if (text == NULL) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
                snprintf(text, needed + 1, "<%s.%s>",
                         bound_tag->module, bound_tag->name);
                LhatString *spelt_name = lhat_string_new(
                    &m->objects, text, needed);
                lhat_free(text);
                if (spelt_name == NULL) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
                lhat_slots_set(m->slots, into, lhat_object((LhatObject *)spelt_name));
                return LHAT_RUN_OK;
            }
            needed = lhat_value_text(native->bound, NULL, 0);
            text = (char *)lhat_alloc(needed + 1);
            if (text == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            lhat_value_text(native->bound, text, needed + 1);
        }
        LhatString *written =
            lhat_string_new(&m->objects, text, needed);
        lhat_free(text);
        if (written == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)written));
        return LHAT_RUN_OK;
    }

    // 02 の 14.20: the comparison '=' makes, with the error
    // term written down instead of taken from 14.8. The same
    // predicate either way, so what a writer chooses is the
    // width of the band and never a different question.
    if (native->kind == LHAT_NATIVE_EQ) {
        if (b != 2) {
            return LHAT_RUN_ARITY;
        }
        LhatValue against = sent;
        LhatValue width = lhat_slots_get(m->slots, first + 1);
        if (!lhat_is_number(against) ||
            !lhat_is_number(width)) {
            return LHAT_RUN_TYPE_ERROR;
        }
        // Written as a distance rather than as 14.8's factor:
        // a writer asking for one says how far apart two may
        // be, which is the number they have in hand. Scaled
        // the same way all the same, so the answer does not
        // change with where on the line the two sit.
        double allowed = lhat_number_as_real(width);
        lhat_slots_set(m->slots, into, lhat_bool(lhat_value_close(
                     native->bound, against, allowed)));
        return LHAT_RUN_OK;
    }

    // 02 の 14.21: the whole number below, above or nearest.
    // Nothing to take: which of the three it is was settled
    // by the name the member was reached through.
    if (native->kind == LHAT_NATIVE_FLOOR ||
        native->kind == LHAT_NATIVE_CEIL ||
        native->kind == LHAT_NATIVE_ROUND) {
        if (b != 0) {
            return LHAT_RUN_ARITY;
        }
        lhat_slots_set(m->slots, into, whole_of(native->bound,
                          native->kind == LHAT_NATIVE_FLOOR
                              ? floor
                          : native->kind == LHAT_NATIVE_CEIL
                              ? ceil
                              : nearbyint));
        return LHAT_RUN_OK;
    }

    // 02 の 14.21改: abs and sign take nothing; clamp takes
    // its two bounds. An integer stays an integer (14.8改)
    // -- only the one integer with no negative widens.
    if (native->kind == LHAT_NATIVE_ABS ||
        native->kind == LHAT_NATIVE_SIGN) {
        if (b != 0) {
            return LHAT_RUN_ARITY;
        }
        LhatValue self = native->bound;
        if (native->kind == LHAT_NATIVE_SIGN) {
            double d = lhat_number_as_real(self);
            lhat_slots_set(m->slots, into, lhat_integer(d > 0 ? 1 : d < 0 ? -1 : 0));
        } else if (lhat_is_integer(self)) {
            int64_t i = lhat_as_integer(self);
            lhat_slots_set(m->slots, into, i == INT64_MIN
                         ? lhat_real(-(double)i)
                         : lhat_integer(i < 0 ? -i : i));
        } else {
            lhat_slots_set(m->slots, into, lhat_real(fabs(lhat_as_real(self))));
        }
        return LHAT_RUN_OK;
    }
    if (native->kind == LHAT_NATIVE_CLAMP) {
        if (b != 2) {
            return LHAT_RUN_ARITY;
        }
        LhatValue low = sent;
        LhatValue high = lhat_slots_get(m->slots, first + 1);
        if (!lhat_is_number(low) || !lhat_is_number(high)) {
            return LHAT_RUN_TYPE_ERROR;
        }
        // The answer is one of the three as handed over, so
        // an integer bound keeps its representation.
        LhatValue self = native->bound;
        double d = lhat_number_as_real(self);
        lhat_slots_set(m->slots, into, d < lhat_number_as_real(low)    ? low
                 : d > lhat_number_as_real(high) ? high
                                                 : self);
        return LHAT_RUN_OK;
    }

    // 02 の 14.17改2: the number^ a string^ names, or nil^
    // where it names none. Takes nothing, or a format -- the
    // same two signatures 14.17's takes, told apart the same
    // way, by how many arguments arrived.
    if (native->kind == LHAT_NATIVE_TONUMBER) {
        if (b > 1) {
            return LHAT_RUN_ARITY;
        }
        // builtin_member only ever binds this to a string^.
        const LhatString *subject =
            (const LhatString *)lhat_as_object(native->bound);
        if (b == 0) {
            // 01 の 10 章's own grammar, so what tonumber
            // reads and what L^ reads cannot drift apart.
            bool is_real = false;
            int64_t whole = 0;
            double real = 0.0;
            if (!lhat_number_read(subject->text,
                                  subject->length, &is_real,
                                  &whole, &real)) {
                lhat_slots_set(m->slots, into, lhat_nil());
                return LHAT_RUN_OK;
            }
            lhat_slots_set(m->slots, into, is_real ? lhat_real(real)
                             : lhat_integer(whole));
            return LHAT_RUN_OK;
        }
        if (!lhat_is_object_kind(sent, LHAT_OBJECT_STRING)) {
            return LHAT_RUN_TYPE_ERROR;
        }
        const LhatString *spelt =
            (const LhatString *)lhat_as_object(sent);
        LhatValue number = lhat_nil();
        bool got = false;
        if (!lhat_number_scan(subject->text, subject->length,
                              spelt->text, spelt->length,
                              &number, &got)) {
            // 14.17 draws the line in the same place: the
            // format is the writer's and a bad one is an
            // error, where the text is data and a text that
            // does not match is simply not a number^.
            return LHAT_RUN_BAD_FORMAT;
        }
        lhat_slots_set(m->slots, into, got ? number : lhat_nil());
        return LHAT_RUN_OK;
    }

    // 02 の 14.19: a run of the subject's characters, named
    // by ordinals that start at 1 and count from the end
    // when negative. A range that does not stand answers the
    // empty string -- what is not there is not an error, the
    // way 04 の 11.3 has a missing key answer nil^.
    //
    // 14.19改: at(i) is that run with both ends at i, so it
    // comes through here -- the ordinal is resolved, rounded
    // and refused in exactly the same places.
    if (native->kind == LHAT_NATIVE_SUBSTRING ||
        native->kind == LHAT_NATIVE_AT) {
        bool single = native->kind == LHAT_NATIVE_AT;
        if (b < 1 || b > (single ? 1 : 2)) {
            return LHAT_RUN_ARITY;
        }
        const LhatString *subject =
            (const LhatString *)lhat_as_object(native->bound);
        int64_t from = 0;
        int64_t to = 0;
        if (!vm_ordinal_of(sent, &from) ||
            (b == 2 && !vm_ordinal_of(lhat_slots_get(m->slots, first + 1), &to))) {
            // Handing over something that is not a number is
            // the writer's mistake, not a range that came out
            // empty -- 14.17改2 draws the same line.
            return LHAT_RUN_TYPE_ERROR;
        }
        size_t count = subject->characters;
        // The one ordinal ends where it starts for at, and at
        // the end of the string for substring.
        int64_t last = single  ? from
                       : b == 2 ? to
                                : (int64_t)count;
        int64_t start = vm_resolve_ordinal(from, count);
        int64_t end = vm_resolve_ordinal(last, count);
        if (start < 1 || end < start || end > (int64_t)count) {
            LhatString *empty =
                lhat_string_new(&m->objects, "", 0);
            if (empty == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            lhat_slots_set(m->slots, into, lhat_object((LhatObject *)empty));
            return LHAT_RUN_OK;
        }
        // The whole of it is the string itself: nothing about
        // a string changes, so a copy would be a second name
        // for the same bytes and nothing more.
        if (start == 1 && end == (int64_t)count) {
            lhat_slots_set(m->slots, into, native->bound);
            return LHAT_RUN_OK;
        }
        size_t at_byte =
            lhat_string_byte_at(subject, (size_t)start - 1);
        size_t end_byte =
            lhat_string_byte_at(subject, (size_t)end);
        LhatString *cut = lhat_string_new(
            &m->objects, subject->text + at_byte,
            end_byte - at_byte);
        if (cut == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)cut));
        return LHAT_RUN_OK;
    }

    // 02 の 14.19改3: the ASCII case swaps. The rest of the
    // bytes pass through as they are -- no Unicode case
    // tables -- and a string nothing changed in answers
    // itself, substring's whole-of-it economy.
    if (native->kind == LHAT_NATIVE_TOUPPER ||
        native->kind == LHAT_NATIVE_TOLOWER) {
        if (b != 0) {
            return LHAT_RUN_ARITY;
        }
        const LhatString *subject =
            (const LhatString *)lhat_as_object(native->bound);
        bool up = native->kind == LHAT_NATIVE_TOUPPER;
        size_t changed = 0;
        for (size_t i = 0; i < subject->length; i++) {
            char head = subject->text[i];
            if (up ? (head >= 'a' && head <= 'z')
                   : (head >= 'A' && head <= 'Z')) {
                changed++;
            }
        }
        if (changed == 0) {
            lhat_slots_set(m->slots, into, native->bound);
            return LHAT_RUN_OK;
        }
        char *text = (char *)lhat_alloc(subject->length + 1);
        if (text == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < subject->length; i++) {
            char head = subject->text[i];
            if (up && head >= 'a' && head <= 'z') {
                head = (char)(head - 'a' + 'A');
            } else if (!up && head >= 'A' && head <= 'Z') {
                head = (char)(head - 'A' + 'a');
            }
            text[i] = head;
        }
        LhatString *swapped = lhat_string_new(
            &m->objects, text, subject->length);
        lhat_free(text);
        if (swapped == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)swapped));
        return LHAT_RUN_OK;
    }

    // 02 の 14.19改3: join^'s inverse, so the law decides
    // the details -- s.split(sep).join^(sep) = s, which is
    // what keeps every empty piece. The 0-argument form is
    // the other reading, "the words": runs of whitespace
    // split it and nothing empty is kept.
    if (native->kind == LHAT_NATIVE_SPLIT) {
        if (b > 1 ||
            (b == 1 &&
             !lhat_is_object_kind(sent, LHAT_OBJECT_STRING))) {
            return b > 1 ? LHAT_RUN_ARITY
                                : LHAT_RUN_TYPE_ERROR;
        }
        const LhatString *subject =
            (const LhatString *)lhat_as_object(native->bound);
        LhatTable *pieces = lhat_table_new(&m->objects);
        if (pieces == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        // The table under construction is a root while the
        // strings are made (14.22's chain).
        LhatNativeHold hold;
        hold.held = lhat_object((LhatObject *)pieces);
        hold.outer = m->native_hold;
        m->native_hold = &hold;
        bool refused = false;
        bool ok = true;
        int64_t position = 0;
        if (b == 0) {
            // The words: whitespace runs split, empties drop.
            size_t i = 0;
            while (ok && i < subject->length) {
                while (i < subject->length &&
                       ((unsigned char)subject->text[i] <=
                        ' ')) {
                    i++;
                }
                size_t begin = i;
                while (i < subject->length &&
                       ((unsigned char)subject->text[i] >
                        ' ')) {
                    i++;
                }
                if (i == begin) {
                    break;
                }
                LhatString *piece = lhat_string_new(
                    &m->objects, subject->text + begin,
                    i - begin);
                ok = piece != NULL &&
                     vm_set_key(m, pieces,
                             lhat_integer(++position),
                             lhat_object((LhatObject *)piece),
                             &refused);
            }
        } else {
            const LhatString *sep =
                (const LhatString *)lhat_as_object(sent);
            size_t from = 0;
            while (ok) {
                size_t found = 0;
                bool hit = false;
                size_t end;
                if (sep->length == 0) {
                    // One character per piece -- and the
                    // round trip with join^("") holds.
                    if (from >= subject->length) {
                        break;
                    }
                    end = from + 1;
                    while (end < subject->length &&
                           ((unsigned char)subject
                                    ->text[end] &
                            0xC0) == 0x80) {
                        end++;
                    }
                } else {
                    hit = find_bytes(subject->text,
                                     subject->length, from,
                                     sep->text, sep->length,
                                     &found);
                    end = hit ? found : subject->length;
                }
                LhatString *piece = lhat_string_new(
                    &m->objects, subject->text + from,
                    end - from);
                ok = piece != NULL &&
                     vm_set_key(m, pieces,
                             lhat_integer(++position),
                             lhat_object((LhatObject *)piece),
                             &refused);
                if (!ok) {
                    break;
                }
                if (sep->length == 0) {
                    from = end;
                } else if (!hit) {
                    break;  // the tail piece went in
                } else {
                    from = found + sep->length;
                }
            }
        }
        m->native_hold = hold.outer;
        if (!ok) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)pieces));
        return LHAT_RUN_OK;
    }

    // 02 の 14.19改3: the plain searches. The needle is a
    // literal string -- what a pattern would ask for lives
    // in std.regex -- and what is not there answers nil^
    // (04 の 11.3), never a sentinel.
    if (native->kind == LHAT_NATIVE_FIND ||
        native->kind == LHAT_NATIVE_FINDALL ||
        native->kind == LHAT_NATIVE_REPLACE) {
        const LhatString *subject =
            (const LhatString *)lhat_as_object(native->bound);
        if (b < 1 ||
            !lhat_is_object_kind(sent, LHAT_OBJECT_STRING)) {
            return b < 1 ? LHAT_RUN_ARITY
                                : LHAT_RUN_TYPE_ERROR;
        }
        const LhatString *needle =
            (const LhatString *)lhat_as_object(sent);

        if (native->kind == LHAT_NATIVE_FIND) {
            if (b > 2) {
                return LHAT_RUN_ARITY;
            }
            // The optional second ordinal reads as
            // substring's does -- 1-based, negative from
            // the end.
            int64_t from = 1;
            if (b == 2 && !vm_ordinal_of(lhat_slots_get(m->slots, first + 1), &from)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            from = vm_resolve_ordinal(from, subject->characters);
            if (from < 1) {
                from = 1;
            }
            size_t start_byte =
                (size_t)from - 1 <= subject->characters
                    ? lhat_string_byte_at(subject,
                                          (size_t)from - 1)
                    : subject->length;
            size_t found = 0;
            if (!find_bytes(subject->text, subject->length,
                            start_byte, needle->text,
                            needle->length, &found)) {
                lhat_slots_set(m->slots, into, lhat_nil());
                return LHAT_RUN_OK;
            }
            lhat_slots_set(m->slots, into, lhat_integer(
                          (int64_t)characters_before(
                              subject->text, found) +
                          1));
            return LHAT_RUN_OK;
        }

        if (native->kind == LHAT_NATIVE_FINDALL) {
            if (b != 1) {
                return LHAT_RUN_ARITY;
            }
            FindWalk *walk = (FindWalk *)lhat_alloc(
                sizeof *walk + needle->length);
            if (walk == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            walk->subject = subject;
            walk->next_byte = 0;
            walk->chars_before = 0;
            walk->needle_length = needle->length;
            memcpy(walk->needle, needle->text,
                   needle->length);
            LhatCoroutine *made = lhat_host_iterator(
                &m->objects, findall_step, walk,
                findall_release, native->bound);
            if (made == NULL) {
                lhat_free(walk);
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            lhat_slots_set(m->slots, into, lhat_object((LhatObject *)made));
            return LHAT_RUN_OK;
        }

        // REPLACE: every stand, non-overlapping, no
        // callback -- that vocabulary is std.regex's.
        if (b != 2 ||
            !lhat_is_object_kind(lhat_slots_get(m->slots, first + 1),
                                 LHAT_OBJECT_STRING)) {
            return b != 2 ? LHAT_RUN_ARITY
                                 : LHAT_RUN_TYPE_ERROR;
        }
        const LhatString *with =
            (const LhatString *)lhat_as_object(lhat_slots_get(m->slots, first + 1));
        if (needle->length == 0) {
            lhat_slots_set(m->slots, into, native->bound);  // nowhere to stand
            return LHAT_RUN_OK;
        }
        size_t stands = 0;
        for (size_t i = 0;
             find_bytes(subject->text, subject->length, i,
                        needle->text, needle->length, &i);
             i += needle->length) {
            stands++;
        }
        if (stands == 0) {
            lhat_slots_set(m->slots, into, native->bound);
            return LHAT_RUN_OK;
        }
        size_t total = subject->length +
                       stands * with->length -
                       stands * needle->length;
        char *text = (char *)lhat_alloc(total + 1);
        if (text == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        size_t out_at = 0;
        size_t in_at = 0;
        size_t found = 0;
        while (find_bytes(subject->text, subject->length,
                          in_at, needle->text,
                          needle->length, &found)) {
            memcpy(text + out_at, subject->text + in_at,
                   found - in_at);
            out_at += found - in_at;
            memcpy(text + out_at, with->text, with->length);
            out_at += with->length;
            in_at = found + needle->length;
        }
        memcpy(text + out_at, subject->text + in_at,
               subject->length - in_at);
        out_at += subject->length - in_at;
        LhatString *swapped =
            lhat_string_new(&m->objects, text, out_at);
        lhat_free(text);
        if (swapped == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)swapped));
        return LHAT_RUN_OK;
    }

    // 16.3: what `in^` walks. A table answers with a walk of
    // its keys; a coroutine is already one.
    // 16.3改2: the projections are made the same way and at
    // the same moment -- one call, one walk -- which is what
    // the parentheses are saying.
    if (native->kind == LHAT_NATIVE_ITERATE ||
        native->kind == LHAT_NATIVE_KEYS ||
        native->kind == LHAT_NATIVE_VALUES) {
        if (native->kind == LHAT_NATIVE_ITERATE &&
            lhat_is_object_kind(native->bound,
                                LHAT_OBJECT_COROUTINE)) {
            lhat_slots_set(m->slots, into, native->bound);
            return LHAT_RUN_OK;
        }
        const LhatTable *over = vm_table_of(native->bound);
        if (over == NULL) {
            return LHAT_RUN_NOT_CALLABLE;
        }
        LhatWalkPart part =
            native->kind == LHAT_NATIVE_KEYS ? LHAT_WALK_KEYS
            : native->kind == LHAT_NATIVE_VALUES
                ? LHAT_WALK_VALUES
                : LHAT_WALK_PAIR;
        LhatCoroutine *walk =
            lhat_table_iterator(&m->objects, over, part);
        if (walk == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)walk));
        return LHAT_RUN_OK;
    }
    return LHAT_RUN_NOT_CALLABLE;
}
