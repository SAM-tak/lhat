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

typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} Sink;

static void put(Sink *s, const char *text, size_t length)
{
    if (s->out != NULL && s->used < s->capacity - 1) {
        size_t room = s->capacity - 1 - s->used;
        memcpy(s->out + s->used, text, length < room ? length : room);
    }
    s->used += length;
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
    Sink s;
    s.out = capacity > 0 ? out : NULL;
    s.capacity = capacity;
    s.used = 0;
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

    if (s.out != NULL) {
        size_t last = s.used < capacity - 1 ? s.used : capacity - 1;
        s.out[last] = '\0';
    }
    return s.used;
}
