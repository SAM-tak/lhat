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
#include <string.h>

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
