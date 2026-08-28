// L^ (lhat) -- sample standard library: std.json. The rule between a table
// and a JSON text is written out in json.h; this is the two walks that
// carry it.
//
// Written here rather than over vendor/cjson: that library is the language
// server's, scoped to it in CMakeLists for the reason given there, and
// nothing else in stdlib/ reaches past the C standard headers. JSON's
// grammar is small enough that reading it straight into L^'s own tables
// costs less than parsing into a second tree and copying out of it.

#include "error.h"
#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/object.h"
#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

// How deep either walk will follow. A table holding itself reaches this and
// answers TooDeep, which is why neither walk keeps a list of what it has
// already seen: the depth is the whole of what is counted.
#define JSON_MAX_DEPTH 96

typedef struct {
    const LhatErrorKind *bad_text;
    const LhatErrorKind *unsupported;
    const LhatErrorKind *too_deep;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
} JsonModule;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

// The length of the sequence `at` begins, or 0 when it begins none. Refuses
// what a JSON text may not carry: an overlong form, a surrogate spelt as
// three bytes, anything past U+10FFFF.
static size_t utf8_step(const unsigned char *at, size_t left)
{
    unsigned char lead = at[0];
    if (lead < 0x80) {
        return 1;
    }
    size_t length = 0;
    uint32_t code = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07u;
    } else {
        return 0;
    }
    if (left < length) {
        return 0;
    }
    for (size_t i = 1; i < length; i++) {
        if ((at[i] & 0xC0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (uint32_t)(at[i] & 0x3Fu);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
        (length == 4 && code < 0x10000)) {
        return 0;  // an overlong form spells a code point twice
    }
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

// Puts one code point down as UTF-8. `into` has room for four.
static size_t utf8_put(uint32_t code, char *into)
{
    if (code < 0x80) {
        into[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        into[0] = (char)(0xC0 | (code >> 6));
        into[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        into[0] = (char)(0xE0 | (code >> 12));
        into[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        into[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    into[0] = (char)(0xF0 | (code >> 18));
    into[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    into[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    into[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// A growing text. `failed` carries the one refusal out rather than every
// step answering a status of its own -- the writer is a recursion, and
// threading a status through every level of it would say the same thing
// nine times over. A false answer with `failed` still NULL is memory.
typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    const LhatErrorKind *failed;
    const char *why;
} Writer;

static void refuse(Writer *w, const LhatErrorKind *kind, const char *why)
{
    if (w->failed == NULL) {
        w->failed = kind;
        w->why = why;
    }
}

static bool put(Writer *w, const char *bytes, size_t length)
{
    if (w->failed != NULL) {
        return false;
    }
    if (w->length + length > w->capacity) {
        size_t wanted = w->capacity ? w->capacity * 2 : 64;
        while (wanted < w->length + length) {
            wanted *= 2;
        }
        char *bigger = (char *)lhat_realloc(w->text, wanted);
        if (bigger == NULL) {
            return false;  // the caller turns this into OutOfMemory
        }
        w->text = bigger;
        w->capacity = wanted;
    }
    memcpy(w->text + w->length, bytes, length);
    w->length += length;
    return true;
}

static bool put_text(Writer *w, const char *text)
{
    return put(w, text, strlen(text));
}

// 14.18: a JSON string is text, so the bytes have to be UTF-8. What must be
// escaped is the quote, the backslash and everything below a space; the
// rest goes down as it stands, which keeps the text readable.
static bool write_string(Writer *w, const JsonModule *m, const char *text,
                         size_t length)
{
    if (!put(w, "\"", 1)) {
        return false;
    }
    size_t at = 0;
    while (at < length) {
        unsigned char c = (unsigned char)text[at];
        const char *escape = NULL;
        switch (c) {
            case '"':  escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape != NULL) {
            if (!put_text(w, escape)) {
                return false;
            }
            at++;
            continue;
        }
        if (c < 0x20) {
            char spelt[8];
            snprintf(spelt, sizeof spelt, "\\u%04x", c);
            if (!put_text(w, spelt)) {
                return false;
            }
            at++;
            continue;
        }
        size_t step = utf8_step((const unsigned char *)text + at, length - at);
        if (step == 0) {
            refuse(w, m->unsupported, "a string that is not UTF-8");
            return false;
        }
        if (!put(w, text + at, step)) {
            return false;
        }
        at += step;
    }
    return put(w, "\"", 1);
}

// A real written so that reading it back answers the same real. 17 digits
// always do; 15 usually do and read better, so they are tried first and
// kept where they hold. lhat_value_text is no use here -- 14.17 writes a
// number for a reader, with %g, which does not come back the same.
static bool write_real(Writer *w, const JsonModule *m, double real)
{
    if (isnan(real) || isinf(real)) {
        refuse(w, m->unsupported, "a real JSON has no spelling for");
        return false;
    }
    char spelt[40];
    snprintf(spelt, sizeof spelt, "%.15g", real);
    if (strtod(spelt, NULL) != real) {
        snprintf(spelt, sizeof spelt, "%.17g", real);
    }
    return put_text(w, spelt);
}

static bool write_value(Writer *w, const JsonModule *m, LhatValue value,
                        size_t depth);

// The key half of an object. A string goes down as itself; an integer is
// spelt out, which is what makes a mixed table's dense half reachable.
static bool write_key(Writer *w, const JsonModule *m, LhatValue key)
{
    if (lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(key);
        return write_string(w, m, string->text, string->length);
    }
    if (lhat_is_integer(key)) {
        char spelt[24];
        snprintf(spelt, sizeof spelt, "%lld", (long long)lhat_as_integer(key));
        return put(w, "\"", 1) && put_text(w, spelt) && put(w, "\"", 1);
    }
    refuse(w, m->unsupported, "a key that is not a string or an integer");
    return false;
}

// One member of an object, waiting to be put in order. `text` names the key
// as it will be written -- into the string itself where the key is one, and
// into `spelt` where it is an integer, which is the only key that has no
// text of its own to point at.
typedef struct {
    LhatValue key;
    LhatValue value;
    const char *text;
    size_t length;
    char spelt[24];
} Member;

static void name_key(Member *at, LhatValue key, LhatValue value)
{
    at->key = key;
    at->value = value;
    if (lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(key);
        at->text = string->text;
        at->length = string->length;
        return;
    }
    if (lhat_is_integer(key)) {
        int written = snprintf(at->spelt, sizeof at->spelt, "%lld",
                               (long long)lhat_as_integer(key));
        at->text = at->spelt;
        at->length = written > 0 ? (size_t)written : 0;
        return;
    }
    // Nothing else has a key's text. write_key refuses it in a moment; until
    // then it sorts as the empty name.
    at->text = at->spelt;
    at->length = 0;
    at->spelt[0] = '\0';
}

// By the key as it is written, byte for byte -- so "10" stands before "2",
// which is what ordering by text means and is the same rule throughout.
static int by_key(const void *left, const void *right)
{
    const Member *a = (const Member *)left;
    const Member *b = (const Member *)right;
    size_t shorter = a->length < b->length ? a->length : b->length;
    int said = shorter > 0 ? memcmp(a->text, b->text, shorter) : 0;
    if (said != 0) {
        return said;
    }
    if (a->length == b->length) {
        return 0;
    }
    return a->length < b->length ? -1 : 1;
}

static bool write_table(Writer *w, const JsonModule *m, const LhatTable *table,
                        size_t depth)
{
    if (depth >= JSON_MAX_DEPTH) {
        refuse(w, m->too_deep, "nested too deep");
        return false;
    }
    // json.h's rule: only the dense part, and not empty, is an array.
    if (table->entry_count == 0 && table->array_count > 0) {
        if (!put(w, "[", 1)) {
            return false;
        }
        for (size_t i = 0; i < table->array_count; i++) {
            if (i > 0 && !put(w, ",", 1)) {
                return false;
            }
            if (!write_value(w, m, lhat_slots_get(table->array, i),
                             depth + 1)) {
                return false;
            }
        }
        return put(w, "]", 1);
    }

    // 02 の 14.16 answered this question once already, for the same reason:
    // a table's own order is the hash's, which is not the writer's and not
    // stable, so what is written is put in a canonical one instead. Here
    // that makes the text of a table depend on what is in it and nothing
    // else -- two equal tables write the same JSON, a decode and a second
    // encode answer the text they started from, and a diff of two texts is
    // about the data.
    size_t total = table->array_count + table->entry_count;
    Member *members = total > 0
                          ? (Member *)lhat_alloc(total * sizeof *members)
                          : NULL;
    if (total > 0 && members == NULL) {
        return false;  // the caller turns this into OutOfMemory
    }
    size_t filled = 0;
    for (size_t i = 0; i < table->array_count; i++) {
        name_key(&members[filled], lhat_integer((int64_t)i + 1),
                 lhat_slots_get(table->array, i));
        filled++;
    }
    for (size_t i = 0; i < table->entry_capacity; i++) {
        const LhatTableEntry *entry = &table->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;  // a free slot and a tombstone read the same
        }
        name_key(&members[filled], entry->key, entry->value);
        filled++;
    }
    if (filled > 1) {
        qsort(members, filled, sizeof *members, by_key);
    }

    bool ok = put(w, "{", 1);
    for (size_t i = 0; ok && i < filled; i++) {
        if (i > 0 && !put(w, ",", 1)) {
            ok = false;
            break;
        }
        ok = write_key(w, m, members[i].key) && put(w, ":", 1) &&
             write_value(w, m, members[i].value, depth + 1);
    }
    lhat_free(members);
    return ok && put(w, "}", 1);
}

static bool write_value(Writer *w, const JsonModule *m, LhatValue value,
                        size_t depth)
{
    if (lhat_is_nil(value)) {
        return put_text(w, "null");
    }
    if (lhat_is_bool(value)) {
        return put_text(w, lhat_as_bool(value) ? "true" : "false");
    }
    if (lhat_is_integer(value)) {
        char spelt[24];
        snprintf(spelt, sizeof spelt, "%lld",
                 (long long)lhat_as_integer(value));
        return put_text(w, spelt);
    }
    if (lhat_is_real(value)) {
        return write_real(w, m, lhat_as_real(value));
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(value);
        return write_string(w, m, string->text, string->length);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return write_table(w, m, (const LhatTable *)lhat_as_object(value),
                           depth);
    }
    refuse(w, m->unsupported, "a value JSON cannot carry");
    return false;
}

static void json_encode(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const JsonModule *m = (const JsonModule *)context;
    Writer w;
    memset(&w, 0, sizeof w);

    LhatValue answer;
    if (write_value(&w, m, arguments[0], 0)) {
        if (!lhat_machine_make_string(machine, w.text, w.length, &answer)) {
            answer = fail_with(machine, m->out_of_memory, "out of memory");
        }
    } else if (w.failed != NULL) {
        answer = fail_with(machine, w.failed, w.why);
    } else {
        answer = fail_with(machine, m->out_of_memory, "out of memory");
    }
    lhat_free(w.text);
    answers[0] = answer;
    *answer_count = 1;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

typedef struct {
    const char *at;
    const char *end;
    LhatMachine *machine;
    const JsonModule *module;
    const LhatErrorKind *failed;
    const char *why;
} Reader;

// Answers false, so a caller reads `return reject(...)` as "stop, and this
// is why". The first refusal is the one kept: what follows it is a walk
// already going wrong.
static bool reject(Reader *r, const LhatErrorKind *kind, const char *why)
{
    if (r->failed == NULL) {
        r->failed = kind;
        r->why = why;
    }
    return false;
}

static void skip_space(Reader *r)
{
    while (r->at < r->end) {
        char c = *r->at;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return;
        }
        r->at++;
    }
}

static bool read_value(Reader *r, LhatValue *out, size_t depth);

static bool read_hex4(Reader *r, uint32_t *out)
{
    if ((size_t)(r->end - r->at) < 4) {
        return reject(r, r->module->bad_text, "a \\u escape ran out");
    }
    uint32_t code = 0;
    for (int i = 0; i < 4; i++) {
        char c = *r->at++;
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A') + 10;
        } else {
            return reject(r, r->module->bad_text, "a \\u escape is not hex");
        }
        code = (code << 4) | digit;
    }
    *out = code;
    return true;
}

// The text between the quotes, unescaped. `out_text` names a buffer the
// caller frees. A \u escape spells one code point, and a high surrogate
// joins the low one that has to follow it -- a surrogate standing alone is
// not text and is turned away rather than written down as itself.
static bool read_string_text(Reader *r, char **out_text, size_t *out_length)
{
    r->at++;  // the opening quote
    size_t capacity = 32;
    size_t length = 0;
    char *text = (char *)lhat_alloc(capacity);
    if (text == NULL) {
        return reject(r, r->module->out_of_memory, "out of memory");
    }

#define JSON_ROOM(n)                                                          \
    do {                                                                      \
        if (length + (n) > capacity) {                                        \
            size_t wanted = capacity * 2;                                     \
            while (wanted < length + (n)) {                                   \
                wanted *= 2;                                                  \
            }                                                                 \
            char *bigger = (char *)lhat_realloc(text, wanted);                \
            if (bigger == NULL) {                                             \
                lhat_free(text);                                              \
                return reject(r, r->module->out_of_memory, "out of memory");  \
            }                                                                 \
            text = bigger;                                                    \
            capacity = wanted;                                                \
        }                                                                     \
    } while (0)

    while (r->at < r->end && *r->at != '"') {
        unsigned char c = (unsigned char)*r->at;
        if (c < 0x20) {
            lhat_free(text);
            return reject(r, r->module->bad_text,
                          "a control byte inside a string");
        }
        if (c != '\\') {
            size_t step = utf8_step((const unsigned char *)r->at,
                                    (size_t)(r->end - r->at));
            if (step == 0) {
                lhat_free(text);
                return reject(r, r->module->bad_text,
                              "a string that is not UTF-8");
            }
            JSON_ROOM(step);
            memcpy(text + length, r->at, step);
            length += step;
            r->at += step;
            continue;
        }

        r->at++;  // the backslash
        if (r->at >= r->end) {
            lhat_free(text);
            return reject(r, r->module->bad_text, "an escape ran out");
        }
        char escaped = *r->at++;
        char plain = 0;
        switch (escaped) {
            case '"':  plain = '"';  break;
            case '\\': plain = '\\'; break;
            case '/':  plain = '/';  break;
            case 'b':  plain = '\b'; break;
            case 'f':  plain = '\f'; break;
            case 'n':  plain = '\n'; break;
            case 'r':  plain = '\r'; break;
            case 't':  plain = '\t'; break;
            case 'u':  break;
            default:
                lhat_free(text);
                return reject(r, r->module->bad_text,
                              "an escape JSON does not have");
        }
        if (escaped != 'u') {
            JSON_ROOM(1);
            text[length++] = plain;
            continue;
        }

        uint32_t code = 0;
        if (!read_hex4(r, &code)) {
            lhat_free(text);
            return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            uint32_t low = 0;
            if ((size_t)(r->end - r->at) < 2 || r->at[0] != '\\' ||
                r->at[1] != 'u') {
                lhat_free(text);
                return reject(r, r->module->bad_text,
                              "a high surrogate with no low one");
            }
            r->at += 2;
            if (!read_hex4(r, &low)) {
                lhat_free(text);
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                lhat_free(text);
                return reject(r, r->module->bad_text,
                              "a high surrogate with no low one");
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            lhat_free(text);
            return reject(r, r->module->bad_text, "a low surrogate alone");
        }
        char spelt[4];
        size_t step = utf8_put(code, spelt);
        JSON_ROOM(step);
        memcpy(text + length, spelt, step);
        length += step;
    }
#undef JSON_ROOM

    if (r->at >= r->end) {
        lhat_free(text);
        return reject(r, r->module->bad_text, "a string with no end");
    }
    r->at++;  // the closing quote
    *out_text = text;
    *out_length = length;
    return true;
}

static bool read_string(Reader *r, LhatValue *out)
{
    char *text = NULL;
    size_t length = 0;
    if (!read_string_text(r, &text, &length)) {
        return false;
    }
    bool made = lhat_machine_make_string(r->machine, text, length, out);
    lhat_free(text);
    return made ? true : reject(r, r->module->out_of_memory, "out of memory");
}

// 14.8: one number type, two representations. A JSON number written with
// neither a fraction nor an exponent, and small enough to hold, is an
// integer; every other one is a real.
static bool read_number(Reader *r, LhatValue *out)
{
    const char *start = r->at;
    bool integral = true;
    if (r->at < r->end && *r->at == '-') {
        r->at++;
    }
    while (r->at < r->end) {
        char c = *r->at;
        if (c >= '0' && c <= '9') {
            r->at++;
            continue;
        }
        if (c == '.' || c == 'e' || c == 'E') {
            integral = false;
            r->at++;
            continue;
        }
        // A sign is part of the number only inside an exponent, which is the
        // one place one can stand after the first byte.
        if ((c == '+' || c == '-') && r->at > start &&
            (r->at[-1] == 'e' || r->at[-1] == 'E')) {
            r->at++;
            continue;
        }
        break;
    }
    size_t length = (size_t)(r->at - start);
    if (length == 0) {
        return reject(r, r->module->bad_text, "a value JSON has no shape for");
    }

    char spelt[64];
    if (length >= sizeof spelt) {
        return reject(r, r->module->bad_text, "a number too long to read");
    }
    memcpy(spelt, start, length);
    spelt[length] = '\0';

    char *stopped = NULL;
    if (integral) {
        errno = 0;
        long long held = strtoll(spelt, &stopped, 10);
        if (*stopped == '\0' && errno != ERANGE) {
            *out = lhat_integer((int64_t)held);
            return true;
        }
    }
    stopped = NULL;
    errno = 0;
    double real = strtod(spelt, &stopped);
    if (stopped == spelt || *stopped != '\0') {
        return reject(r, r->module->bad_text, "a number JSON has no shape for");
    }
    *out = lhat_real(real);
    return true;
}

// 11.3: null is nothing, and nothing is what an unset key already holds --
// so a null is read and then not put anywhere. In an array that leaves a
// hole; json.h says why that is the honest answer.
static bool keep(Reader *r, LhatTable *table, LhatValue key, LhatValue held)
{
    if (lhat_is_nil(held)) {
        return true;
    }
    bool refused = false;
    if (!lhat_machine_table_set(r->machine, table, key, held, &refused)) {
        return reject(r, r->module->out_of_memory, "out of memory");
    }
    if (refused) {
        return reject(r, r->module->bad_text, "a key a table cannot hold");
    }
    return true;
}

static bool read_array(Reader *r, LhatValue *out, size_t depth)
{
    LhatValue made = lhat_nil();
    if (!lhat_machine_make_table(r->machine, &made)) {
        return reject(r, r->module->out_of_memory, "out of memory");
    }
    LhatTable *table = (LhatTable *)lhat_as_object(made);
    *out = made;  // held from here, so a collection inside the loop reaches it
    r->at++;      // '['
    skip_space(r);
    if (r->at < r->end && *r->at == ']') {
        r->at++;
        return true;
    }
    int64_t at = 0;
    for (;;) {
        LhatValue held = lhat_nil();
        if (!read_value(r, &held, depth + 1)) {
            return false;
        }
        at++;
        if (!keep(r, table, lhat_integer(at), held)) {
            return false;
        }
        skip_space(r);
        if (r->at < r->end && *r->at == ',') {
            r->at++;
            skip_space(r);
            continue;
        }
        if (r->at < r->end && *r->at == ']') {
            r->at++;
            return true;
        }
        return reject(r, r->module->bad_text, "an array with no end");
    }
}

static bool read_object(Reader *r, LhatValue *out, size_t depth)
{
    LhatValue made = lhat_nil();
    if (!lhat_machine_make_table(r->machine, &made)) {
        return reject(r, r->module->out_of_memory, "out of memory");
    }
    LhatTable *table = (LhatTable *)lhat_as_object(made);
    *out = made;
    r->at++;  // '{'
    skip_space(r);
    if (r->at < r->end && *r->at == '}') {
        r->at++;
        return true;
    }
    for (;;) {
        if (r->at >= r->end || *r->at != '"') {
            return reject(r, r->module->bad_text,
                          "a key that is not a string");
        }
        LhatValue key = lhat_nil();
        if (!read_string(r, &key)) {
            return false;
        }
        skip_space(r);
        if (r->at >= r->end || *r->at != ':') {
            return reject(r, r->module->bad_text, "a key with no value");
        }
        r->at++;
        LhatValue held = lhat_nil();
        if (!read_value(r, &held, depth + 1)) {
            return false;
        }
        if (!keep(r, table, key, held)) {
            return false;
        }
        skip_space(r);
        if (r->at < r->end && *r->at == ',') {
            r->at++;
            skip_space(r);
            continue;
        }
        if (r->at < r->end && *r->at == '}') {
            r->at++;
            return true;
        }
        return reject(r, r->module->bad_text, "an object with no end");
    }
}

static bool word_is(Reader *r, const char *word)
{
    size_t length = strlen(word);
    if ((size_t)(r->end - r->at) < length || memcmp(r->at, word, length) != 0) {
        return false;
    }
    r->at += length;
    return true;
}

static bool read_value(Reader *r, LhatValue *out, size_t depth)
{
    if (depth >= JSON_MAX_DEPTH) {
        return reject(r, r->module->too_deep, "nested too deep");
    }
    skip_space(r);
    if (r->at >= r->end) {
        return reject(r, r->module->bad_text, "the text ran out");
    }
    switch (*r->at) {
        case '{': return read_object(r, out, depth);
        case '[': return read_array(r, out, depth);
        case '"': return read_string(r, out);
        default: break;
    }
    if (word_is(r, "true")) {
        *out = lhat_bool(true);
        return true;
    }
    if (word_is(r, "false")) {
        *out = lhat_bool(false);
        return true;
    }
    if (word_is(r, "null")) {
        *out = lhat_nil();
        return true;
    }
    return read_number(r, out);
}

static void json_decode(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const JsonModule *m = (const JsonModule *)context;
    if (!lhat_is_object_kind(arguments[0], LHAT_OBJECT_STRING)) {
        answers[0] = fail_with(machine, m->bad_text, "not a text to read");
        *answer_count = 1;
        return;
    }
    const LhatString *text = (const LhatString *)lhat_as_object(arguments[0]);

    Reader r;
    memset(&r, 0, sizeof r);
    r.at = text->text;
    r.end = text->text + text->length;
    r.machine = machine;
    r.module = m;

    skip_space(&r);
    // json.h: the top of a decode is a table, which is what the signature
    // promises. A scalar standing alone is JSON, but not this call's answer.
    if (r.at >= r.end || (*r.at != '{' && *r.at != '[')) {
        answers[0] = fail_with(machine, m->bad_text,
                         "the top of the text is not an object or an array");
        *answer_count = 1;
        return;
    }
    LhatValue made = lhat_nil();
    if (!read_value(&r, &made, 0)) {
        answers[0] = fail_with(machine, r.failed != NULL ? r.failed : m->bad_text,
                         r.why != NULL ? r.why : "not JSON");
        *answer_count = 1;
        return;
    }
    skip_space(&r);
    if (r.at != r.end) {
        answers[0] = fail_with(machine, m->bad_text, "more text after the value");
        *answer_count = 1;
        return;
    }
    answers[0] = made;
    *answer_count = 1;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool lhatstdlib_json_register(LhatProgram *program)
{
    // 05 の 8.7: registration before checking -- std.error.OutOfMemory has to
    // exist before this module's signatures name it. The call is idempotent.
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    // 05 の 8.7: every field of this is an identity, and an identity belongs
    // to the process rather than to a program -- one declaration, one error
    // kind, however many programs declare it. So one of these serves them
    // all, and a second registration writes the same answers back into it.
    static JsonModule shared;
    JsonModule *module = &shared;
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"BadText", "Unsupported", "TooDeep"};
    const LhatErrorKind *kinds[3];
    if (!lhat_register_error_kind(program, "std.json", "JsonError", variants, 3,
                                  NULL, kinds)) {
        return false;
    }
    module->bad_text = kinds[0];
    module->unsupported = kinds[1];
    module->too_deep = kinds[2];

    return lhat_register_func(
               program, "std.json", "encode",
               "f^t^{} -> string^|std.json.JsonError|std.error.OutOfMemory;",
               json_encode, module) &&
           lhat_register_func(
               program, "std.json", "decode",
               "f^string^ -> t^{}|std.json.JsonError|std.error.OutOfMemory;",
               json_decode, module);
}
