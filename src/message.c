// L^ (lhat) -- a message's text and its arguments, made into the sentence.
//
// 10 §5.1: a sentence is never built by concatenation; its arguments go into
// named holes. A hole is `{`, then a name -- a lower-case ASCII letter, then
// letters, digits and '-' -- then `}`. Every other brace is a brace, which is
// what lets the English already written stand unchanged: its braces are `{}`,
// `{ ... }` and `'{'`, none of them the shape of a hole. `\{`, `\}` and `\\`
// write a brace or a backslash where one would otherwise read as a hole or an
// escape (10 §6.1); no English text holds a backslash, so none of it changes.
//
// What fills a hole goes in as it is. A phrase (10 §5.2) is looked up by the
// caller, in whatever language it is drawing the sentence in, before it is
// handed over -- this knows nothing of languages.
//
// A hole with no argument of its name is written as it stands, braces and
// all, so a sentence that was not given something says so instead of closing
// up around the gap.

#include "message.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lhat/port.h"
#include "lhat/version.h"

typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} Sink;

static Sink sink(char *out, size_t capacity)
{
    Sink s;
    s.out = capacity > 0 ? out : NULL;
    s.capacity = capacity;
    s.used = 0;
    return s;
}

static void put(Sink *s, const char *text, size_t length)
{
    if (s->out != NULL && s->used < s->capacity - 1) {
        size_t room = s->capacity - 1 - s->used;
        memcpy(s->out + s->used, text, length < room ? length : room);
    }
    s->used += length;
}

static void put_text(Sink *s, const char *text)
{
    put(s, text, strlen(text));
}

// lhat_report_write's convention: how many bytes the whole thing wanted,
// with the buffer closed at whichever came first.
static size_t sealed(Sink *s)
{
    if (s->out != NULL) {
        s->out[s->used < s->capacity - 1 ? s->used : s->capacity - 1] = '\0';
    }
    return s->used;
}

static bool name_start(char c)
{
    return c >= 'a' && c <= 'z';
}

static bool name_byte(char c)
{
    return name_start(c) || (c >= '0' && c <= '9') || c == '-';
}

static const LhatMessageArg *argument_named(const LhatMessageArg *args,
                                            size_t count, const char *name,
                                            size_t length)
{
    for (size_t i = 0; i < count; i++) {
        if (args[i].name != NULL && strlen(args[i].name) == length &&
            memcmp(args[i].name, name, length) == 0) {
            return &args[i];
        }
    }
    return NULL;
}

size_t lhat_message_render(const char *text, const LhatMessageArg *args,
                           size_t count, char *out, size_t capacity)
{
    Sink s = sink(out, capacity);
    if (text == NULL) {
        text = "";
    }

    const char *run = text;  // the literal text not yet put
    const char *p = text;
    while (*p != '\0') {
        if (p[0] == '\\' && (p[1] == '{' || p[1] == '}' || p[1] == '\\')) {
            put(&s, run, (size_t)(p - run));
            put(&s, p + 1, 1);
            p += 2;
            run = p;
            continue;
        }
        if (p[0] == '{' && name_start(p[1])) {
            const char *end = p + 2;
            while (name_byte(*end)) {
                end++;
            }
            const LhatMessageArg *arg =
                *end == '}' ? argument_named(args, count, p + 1,
                                             (size_t)(end - (p + 1)))
                            : NULL;
            if (arg != NULL) {
                put(&s, run, (size_t)(p - run));
                if (arg->value != NULL) {
                    put(&s, arg->value, arg->length);
                }
                p = end + 1;
                run = p;
                continue;
            }
        }
        p++;
    }
    put(&s, run, (size_t)(p - run));
    return sealed(&s);
}

// ---------------------------------------------------------------------------
// 10 §6.3: the English, written out as a catalog -- one file per source, its
// entries all commented out, for a translation to be made from.

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
};

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
    out->edits = &slots[which].edit;
    out->edit_count = 1;
    return true;
}

LHAT_MESSAGE_TABLES(lhat_fix_message_tables,
    {FIX_MESSAGES, LHAT_MESSAGE_COUNT(FIX_MESSAGES)})

const LhatMessageEntry *lhat_fix_message(size_t which)
{
    return LHAT_MESSAGE_AT(FIX_MESSAGES, which);
}

typedef const LhatMessageTable *(*Tables)(size_t *count);

// The sources this build holds, in the order 10 §3.1 lists them. A build
// without the front end holds no check, parse or lex to write (10 §6.2).
static const struct {
    const char *name;
    Tables tables;
} SOURCES[] = {
#if LHAT_WITH_FRONTEND
    {"check", lhat_check_message_tables},
    {"parse", lhat_parse_message_tables},
    {"lex", lhat_lexer_message_tables},
#endif
    {"compile", lhat_compile_message_tables},
    {"run", lhat_run_message_tables},
    {"program", lhat_program_message_tables},
    {"source", lhat_source_message_tables},
    {"report", lhat_report_message_tables},
    {"trace", lhat_trace_message_tables},
    {"fix", lhat_fix_message_tables},
};

const char *lhat_messages_source(size_t index)
{
    return index < LHAT_MESSAGE_COUNT(SOURCES) ? SOURCES[index].name : NULL;
}

// What a file says before its entries: which source it holds, which version's
// English, and what a translator does with an entry.
static void put_header(Sink *s, const char *source)
{
    put_text(s, "# ");
    put_text(s, source);
    put_text(s, " -- L^ " LHAT_VERSION ", every entry commented out.\n");
    put_text(s, "# To translate an entry, write it again below with the same "
                "name and the new text.\n\n");
}

// One entry, commented out. The name is the ID without its source, since the
// file is named for the source (10 §6.1), and a newline in the text becomes a
// continuation line. An entry of another source is not this file's.
static void put_entry(Sink *s, const char *source,
                      const LhatMessageEntry *entry)
{
    size_t length = strlen(source);
    if (entry->id == NULL || strncmp(entry->id, source, length) != 0 ||
        entry->id[length] != '.') {
        return;
    }
    put_text(s, "# ");
    put_text(s, entry->id + length + 1);
    put_text(s, " = ");
    for (const char *p = entry->text; *p != '\0'; p++) {
        if (*p == '\n') {
            // A continuation line is one space, then the text's own line --
            // the reader takes that one space back off (10 §6.1), so what
            // was written comes back unchanged.
            put_text(s, "\n#  ");
        } else if (*p == '\\') {
            put_text(s, "\\\\");
        } else {
            put(s, p, 1);
        }
    }
    put(s, "\n", 1);
}

size_t lhat_messages_write_catalog(const char *source,
                                   const LhatMessageEntry *entries,
                                   size_t count, char *out, size_t capacity)
{
    Sink s = sink(out, capacity);
    if (source == NULL) {
        return sealed(&s);
    }
    put_header(&s, source);
    for (size_t i = 0; entries != NULL && i < count; i++) {
        put_entry(&s, source, &entries[i]);
    }
    return sealed(&s);
}

size_t lhat_messages_write_english(const char *source, char *out,
                                   size_t capacity)
{
    Sink s = sink(out, capacity);
    for (size_t i = 0; source != NULL && i < LHAT_MESSAGE_COUNT(SOURCES);
         i++) {
        if (strcmp(SOURCES[i].name, source) != 0) {
            continue;
        }
        put_header(&s, source);
        size_t count = 0;
        const LhatMessageTable *tables = SOURCES[i].tables(&count);
        for (size_t t = 0; t < count; t++) {
            for (size_t e = 0; e < tables[t].count; e++) {
                put_entry(&s, source, &tables[t].entries[e]);
            }
        }
        break;
    }
    return sealed(&s);
}

// ---------------------------------------------------------------------------
// 10 §6.4: reading a catalog -- bytes in 10 §6.1's format, for one language
// and one source, into the entries this build knows the names and the holes
// of. Nothing here fails: a line it cannot use is left out and the rest
// stands, so a catalog written for another version still reads.

// The hole after `p`, if there is one: `name` and `length` are its name, and
// what comes back is where to look from next. The walk the renderer makes,
// so an escaped brace is not a hole here either.
static const char *hole_after(const char *p, const char **name, size_t *length)
{
    while (*p != '\0') {
        if (p[0] == '\\' && (p[1] == '{' || p[1] == '}' || p[1] == '\\')) {
            p += 2;
            continue;
        }
        if (p[0] == '{' && name_start(p[1])) {
            const char *end = p + 2;
            while (name_byte(*end)) {
                end++;
            }
            if (*end == '}') {
                *name = p + 1;
                *length = (size_t)(end - (p + 1));
                return end + 1;
            }
        }
        p++;
    }
    return NULL;
}

static bool holds_hole(const char *text, const char *name, size_t length)
{
    const char *at = text;
    const char *found = NULL;
    size_t found_length = 0;
    while ((at = hole_after(at, &found, &found_length)) != NULL) {
        if (found_length == length && memcmp(found, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// A translation is taken only when its holes are the English's -- not more,
// not fewer. Walked from both sides rather than gathered into a set, since a
// text holds a handful of holes at most.
static bool same_holes(const char *english, const char *said)
{
    const char *name = NULL;
    size_t length = 0;
    for (const char *at = english;
         (at = hole_after(at, &name, &length)) != NULL;) {
        if (!holds_hole(said, name, length)) {
            return false;
        }
    }
    for (const char *at = said; (at = hole_after(at, &name, &length)) != NULL;) {
        if (!holds_hole(english, name, length)) {
            return false;
        }
    }
    return true;
}

// 10 §4.2's spelling, over the name a line gives: lower-case letters and
// digits, joined by '-' or '.', with neither doubled nor at either end.
static bool well_spelled(const char *name, size_t length)
{
    bool last_alnum = false;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            last_alnum = true;
        } else if ((c == '-' || c == '.') && last_alnum) {
            last_alnum = false;
        } else {
            return false;
        }
    }
    return last_alnum;
}

// The English of `source`'s `name`: from the table the caller gave, or from
// what this build holds when it gave none.
static const char *english_of(const char *source,
                              const LhatMessageEntry *english, size_t count,
                              const char *name, size_t length)
{
    const LhatMessageTable one = {english, count};
    const LhatMessageTable *tables = english != NULL ? &one : NULL;
    size_t table_count = english != NULL ? 1 : 0;
    for (size_t i = 0; english == NULL && i < LHAT_MESSAGE_COUNT(SOURCES);
         i++) {
        if (strcmp(SOURCES[i].name, source) == 0) {
            tables = SOURCES[i].tables(&table_count);
            break;
        }
    }
    size_t prefix = strlen(source);
    for (size_t t = 0; tables != NULL && t < table_count; t++) {
        for (size_t e = 0; e < tables[t].count; e++) {
            const LhatMessageEntry *entry = &tables[t].entries[e];
            if (entry->id != NULL && strncmp(entry->id, source, prefix) == 0 &&
                entry->id[prefix] == '.' &&
                strncmp(entry->id + prefix + 1, name, length) == 0 &&
                entry->id[prefix + 1 + length] == '\0') {
                return entry->text;
            }
        }
    }
    return NULL;
}

// The text of the entry being read, which its continuation lines grow. Out
// of memory it stops growing and says so, and the entry is left out.
typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    bool short_of_room;
} Held;

static void held_add(Held *h, const char *text, size_t length)
{
    if (h->length + length + 1 > h->capacity) {
        size_t wanted = (h->length + length + 1) * 2;
        char *bigger = (char *)lhat_realloc(h->text, wanted);
        if (bigger == NULL) {
            h->short_of_room = true;
            return;
        }
        h->text = bigger;
        h->capacity = wanted;
    }
    memcpy(h->text + h->length, text, length);
    h->length += length;
    h->text[h->length] = '\0';
}

static void held_drop(Held *h)
{
    lhat_free(h->text);
    h->text = NULL;
    h->length = 0;
    h->capacity = 0;
    h->short_of_room = false;
}

// Keeps `text` under `source`.`name`, taking it over. An ID already held is
// written again, since a catalog's later entry is the one that counts.
static bool catalog_put(LhatCatalog *catalog, const char *name, size_t length,
                        char *text)
{
    size_t room = strlen(catalog->source) + 1 + length + 1;
    char *id = (char *)lhat_alloc(room);
    if (id == NULL) {
        return false;
    }
    snprintf(id, room, "%s.%.*s", catalog->source, (int)length, name);
    for (size_t i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->entries[i].id, id) == 0) {
            lhat_free((void *)catalog->entries[i].text);
            catalog->entries[i].text = text;
            lhat_free(id);
            return true;
        }
    }
    if (catalog->count == catalog->capacity) {
        size_t grown = catalog->capacity != 0 ? catalog->capacity * 2 : 16;
        LhatMessageEntry *bigger = (LhatMessageEntry *)lhat_realloc(
            catalog->entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            lhat_free(id);
            return false;
        }
        catalog->entries = bigger;
        catalog->capacity = grown;
    }
    catalog->entries[catalog->count].id = id;
    catalog->entries[catalog->count].text = text;
    catalog->count++;
    return true;
}

// The entry that was being read, taken into the catalog or left out.
static void close_entry(LhatCatalog *catalog, const LhatMessageEntry *english,
                        size_t english_count, const char *name, size_t length,
                        Held *said)
{
    const char *reference =
        name != NULL && !said->short_of_room
            ? english_of(catalog->source, english, english_count, name, length)
            : NULL;
    if (reference != NULL && said->length > 0 &&
        same_holes(reference, said->text) &&
        catalog_put(catalog, name, length, said->text)) {
        said->text = NULL;  // the catalog holds it now
    }
    held_drop(said);
}

static char *owned(const char *text)
{
    size_t room = strlen(text) + 1;
    char *copy = (char *)lhat_alloc(room);
    if (copy != NULL) {
        memcpy(copy, text, room);
    }
    return copy;
}

void lhat_catalog_dispose(LhatCatalog *catalog)
{
    if (catalog == NULL) {
        return;
    }
    for (size_t i = 0; i < catalog->count; i++) {
        lhat_free((void *)catalog->entries[i].id);
        lhat_free((void *)catalog->entries[i].text);
    }
    lhat_free(catalog->entries);
    lhat_free(catalog->tag);
    lhat_free(catalog->source);
    memset(catalog, 0, sizeof *catalog);
}

const char *lhat_catalog_text(const LhatCatalog *catalog, const char *id)
{
    for (size_t i = 0; catalog != NULL && id != NULL && i < catalog->count;
         i++) {
        if (strcmp(catalog->entries[i].id, id) == 0) {
            return catalog->entries[i].text;
        }
    }
    return NULL;
}

size_t lhat_catalog_load(LhatCatalog *catalog, const char *tag,
                         const char *source, const LhatMessageEntry *english,
                         size_t english_count, const char *text, size_t length)
{
    if (catalog == NULL || tag == NULL || source == NULL || text == NULL) {
        return 0;
    }
    lhat_catalog_dispose(catalog);
    catalog->tag = owned(tag);
    catalog->source = owned(source);
    if (catalog->tag == NULL || catalog->source == NULL) {
        lhat_catalog_dispose(catalog);
        return 0;
    }

    // The entry being read: the name its line gave, and the text its
    // continuation lines are adding to. A blank line is the text's only when
    // a continuation line follows it (10 §6.1).
    const char *name = NULL;
    size_t name_length = 0;
    Held said = {NULL, 0, 0, false};
    size_t blanks = 0;

    size_t at = 0;
    if (length >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        at = 3;  // a BOM some editor wrote, as 01 §1 drops it from a source
    }
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n' && text[end] != '\r') {
            end++;
        }
        size_t next = end < length ? end + 1 : length;
        if (end + 1 < length && text[end] == '\r' && text[end + 1] == '\n') {
            next = end + 2;  // every line ending is one line ending
        }
        const char *line = text + at;
        size_t line_length = end - at;
        while (line_length > 0 && (line[line_length - 1] == ' ' ||
                                   line[line_length - 1] == '\t')) {
            line_length--;  // what is at the end of a line is not in the text
        }
        at = next;

        if (line_length == 0) {
            blanks++;
            continue;
        }
        if (line[0] == '#') {
            close_entry(catalog, english, english_count, name, name_length,
                        &said);
            name = NULL;
            blanks = 0;
            continue;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            if (name == NULL) {
                blanks = 0;  // the continuation of nothing
                continue;
            }
            for (size_t i = 0; i <= blanks; i++) {
                held_add(&said, "\n", 1);
            }
            blanks = 0;
            held_add(&said, line + 1, line_length - 1);
            continue;
        }

        // A line of its own closes the entry before it, whatever it says.
        close_entry(catalog, english, english_count, name, name_length,
                    &said);
        name = NULL;
        blanks = 0;
        const char *equals = (const char *)memchr(line, '=', line_length);
        if (equals == NULL) {
            continue;
        }
        size_t written = (size_t)(equals - line);
        while (written > 0 &&
               (line[written - 1] == ' ' || line[written - 1] == '\t')) {
            written--;
        }
        if (!well_spelled(line, written)) {
            continue;
        }
        name = line;
        name_length = written;
        const char *rest = equals + 1;
        size_t rest_length = line_length - (size_t)(rest - line);
        while (rest_length > 0 && (*rest == ' ' || *rest == '\t')) {
            rest++;
            rest_length--;
        }
        held_add(&said, rest, rest_length);
    }
    close_entry(catalog, english, english_count, name, name_length, &said);
    return catalog->count;
}
