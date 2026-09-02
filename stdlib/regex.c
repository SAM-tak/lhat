// L^ (lhat) -- sample standard library: std.regex (see regex.h).
//
// The Regex hostdata owns its pattern text and its compiled form. A gmatch
// walk compiles a form of its own from that text instead of borrowing the
// object's -- the walk may outlive a dispose^ of the object, and owning the
// compilation outright is cheaper than owning that question.

#include "regex.h"

#include <string.h>

#include "error.h"
#include "regex_engine.h"

typedef struct {
    const LhatHostDataTag *tag;
    const LhatErrorKind *bad_pattern;
    const LhatErrorKind *exhausted;
} RegexModule;

// The hostdata behind std.regex.Regex.
typedef struct {
    LhatRegex *compiled;
    char *pattern;
    size_t pattern_length;
} Regex;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

// 1-based character ordinal of a byte offset -- continuation bytes carry no
// ordinal of their own (02 の 14.19's counting).
static size_t ordinal_at(const char *text, size_t byte_at)
{
    size_t count = 0;
    for (size_t i = 0; i < byte_at; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count + 1;
}

// One code point's width at `at`, for stepping past an empty match.
static size_t step_width(const char *text, size_t length, size_t at)
{
    if (at >= length) {
        return 1;
    }
    size_t width = 1;
    while (at + width < length &&
           ((unsigned char)text[at + width] & 0xC0) == 0x80) {
        width++;
    }
    return width;
}

// ---------------------------------------------------------------------------
// A growable byte buffer for gsub's output
// ---------------------------------------------------------------------------

typedef struct {
    char *bytes;
    size_t count;
    size_t capacity;
    bool failed;
} Buffer;

static void buffer_put(Buffer *b, const char *bytes, size_t count)
{
    if (b->failed || count == 0) {
        return;
    }
    if (b->count + count > b->capacity) {
        size_t wider = b->capacity ? b->capacity * 2 : 64;
        while (wider < b->count + count) {
            wider *= 2;
        }
        char *grown = (char *)lhat_alloc(wider);
        if (grown == NULL) {
            b->failed = true;
            return;
        }
        memcpy(grown, b->bytes, b->count);
        lhat_free(b->bytes);
        b->bytes = grown;
        b->capacity = wider;
    }
    memcpy(b->bytes + b->count, bytes, count);
    b->count += count;
}

// ---------------------------------------------------------------------------
// The searches themselves, shared by the object members and the module forms
// ---------------------------------------------------------------------------

static LhatValue span_string(LhatMachine *machine, const char *text,
                             LhatRegexSpan span)
{
    LhatValue made = lhat_nil();
    if (span.begin == (size_t)-1 ||
        !lhat_machine_make_string(machine, text + span.begin,
                                  span.end - span.begin, &made)) {
        return lhat_nil();
    }
    return made;
}

static LhatValue do_match(LhatMachine *machine, const RegexModule *module,
                          const LhatRegex *compiled, const LhatString *text)
{
    LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS];
    bool blown = false;
    if (!lhat_regex_search(compiled, text->text, text->length, 0, spans,
                           &blown)) {
        return blown ? fail_with(machine, module->exhausted,
                                 "the pattern ran past the matching budget")
                     : lhat_nil();
    }
    return span_string(machine, text->text, spans[0]);
}

static LhatValue do_captures(LhatMachine *machine, const RegexModule *module,
                             const LhatRegex *compiled,
                             const LhatString *text)
{
    LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS];
    bool blown = false;
    if (!lhat_regex_search(compiled, text->text, text->length, 0, spans,
                           &blown)) {
        return blown ? fail_with(machine, module->exhausted,
                                 "the pattern ran past the matching budget")
                     : lhat_nil();
    }
    LhatValue table = lhat_nil();
    if (!lhat_machine_make_table(machine, &table)) {
        return lhat_nil();
    }
    LhatTable *holder = (LhatTable *)lhat_as_object(table);
    size_t groups = lhat_regex_groups(compiled);
    for (size_t i = 0; i <= groups; i++) {
        // 04 の 11.3: a group that took part in no match is simply absent.
        if (spans[i].begin == (size_t)-1) {
            continue;
        }
        LhatValue piece = span_string(machine, text->text, spans[i]);
        if (!lhat_machine_table_set(machine, holder,
                                    lhat_integer((int64_t)i + 1), piece, NULL)) {
            return lhat_nil();
        }
    }
    return table;
}

// The written replacement, $0..$9 and $$ expanded from the spans.
static void expand_replacement(Buffer *out, const LhatString *with,
                               const char *text,
                               const LhatRegexSpan *spans)
{
    for (size_t i = 0; i < with->length; i++) {
        char head = with->text[i];
        if (head != '$' || i + 1 >= with->length) {
            buffer_put(out, &head, 1);
            continue;
        }
        char next = with->text[i + 1];
        if (next == '$') {
            buffer_put(out, "$", 1);
            i++;
            continue;
        }
        if (next >= '0' && next <= '9') {
            size_t group = (size_t)(next - '0');
            if (spans[group].begin != (size_t)-1) {
                buffer_put(out, text + spans[group].begin,
                           spans[group].end - spans[group].begin);
            }
            i++;
            continue;
        }
        buffer_put(out, &head, 1);
    }
}

// One match through the written function: (match, ordinal, captures) in,
// string^ or nil^ out -- nil^ keeps the original (Lua's gsub rule). A fault
// in the function comes back as a non-OK status; answering nil^ then lets
// the machine's own fault propagation end the outer run (05 の 8.7).
static bool call_replacement(LhatMachine *machine, LhatValue fn,
                             const LhatString *text,
                             const LhatRegexSpan *spans, size_t groups,
                             Buffer *out, bool *faulted)
{
    LhatValue arguments[3];
    arguments[0] = span_string(machine, text->text, spans[0]);
    arguments[1] =
        lhat_integer((int64_t)ordinal_at(text->text, spans[0].begin));
    LhatValue table = lhat_nil();
    if (!lhat_machine_make_table(machine, &table)) {
        return false;
    }
    LhatTable *holder = (LhatTable *)lhat_as_object(table);
    for (size_t i = 1; i <= groups; i++) {
        if (spans[i].begin == (size_t)-1) {
            continue;
        }
        if (!lhat_machine_table_set(machine, holder,
                                    lhat_integer((int64_t)i),
                                    span_string(machine, text->text,
                                                spans[i]), NULL)) {
            return false;
        }
    }
    arguments[2] = table;
    LhatRunResult ran = lhat_machine_call(machine, fn, arguments, 3);
    if (ran.status != LHAT_RUN_OK) {
        *faulted = true;
        return false;
    }
    if (lhat_is_object_kind(ran.value, LHAT_OBJECT_STRING)) {
        const LhatString *swapped =
            (const LhatString *)lhat_as_object(ran.value);
        buffer_put(out, swapped->text, swapped->length);
        return true;
    }
    // nil^ -- and anything the checker would have refused -- keeps the
    // original match.
    buffer_put(out, text->text + spans[0].begin,
               spans[0].end - spans[0].begin);
    return true;
}

static LhatValue do_gsub(LhatMachine *machine, const RegexModule *module,
                         const LhatRegex *compiled, const LhatString *text,
                         LhatValue replacement)
{
    bool with_function = !lhat_is_object_kind(replacement, LHAT_OBJECT_STRING);
    const LhatString *written =
        with_function ? NULL
                      : (const LhatString *)lhat_as_object(replacement);
    Buffer out;
    memset(&out, 0, sizeof out);
    size_t from = 0;
    size_t groups = lhat_regex_groups(compiled);
    for (;;) {
        LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS];
        bool blown = false;
        if (!lhat_regex_search(compiled, text->text, text->length, from,
                               spans, &blown)) {
            if (blown) {
                lhat_free(out.bytes);
                return fail_with(machine, module->exhausted,
                                 "the pattern ran past the matching budget");
            }
            break;
        }
        buffer_put(&out, text->text + from, spans[0].begin - from);
        if (with_function) {
            bool faulted = false;
            if (!call_replacement(machine, replacement, text, spans, groups,
                                  &out, &faulted)) {
                lhat_free(out.bytes);
                // A fault's frames are standing; answering anything lets
                // the machine end the run with the fault itself.
                return lhat_nil();
            }
        } else {
            expand_replacement(&out, written, text->text, spans);
        }
        if (spans[0].end == spans[0].begin) {
            // An empty match: keep the character it stood before, and step
            // past it, or the loop would stand still.
            size_t width =
                step_width(text->text, text->length, spans[0].end);
            if (spans[0].end >= text->length) {
                from = spans[0].end;
                break;
            }
            buffer_put(&out, text->text + spans[0].end, width);
            from = spans[0].end + width;
        } else {
            from = spans[0].end;
        }
    }
    buffer_put(&out, text->text + from, text->length - from);
    if (out.failed) {
        lhat_free(out.bytes);
        return lhat_nil();
    }
    LhatValue made = lhat_nil();
    bool ok = lhat_machine_make_string(machine, out.bytes ? out.bytes : "",
                                       out.count, &made);
    lhat_free(out.bytes);
    return ok ? made : lhat_nil();
}

// The pieces between the matches, empties kept -- 02 の 14.19改3's law read
// with a pattern for the separator (Python's re.split).
static LhatValue do_split(LhatMachine *machine, const RegexModule *module,
                          const LhatRegex *compiled, const LhatString *text)
{
    LhatValue table = lhat_nil();
    if (!lhat_machine_make_table(machine, &table)) {
        return lhat_nil();
    }
    LhatTable *pieces = (LhatTable *)lhat_as_object(table);
    size_t from = 0;
    int64_t position = 0;
    for (;;) {
        LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS];
        bool blown = false;
        if (!lhat_regex_search(compiled, text->text, text->length, from,
                               spans, &blown)) {
            if (blown) {
                return fail_with(machine, module->exhausted,
                                 "the pattern ran past the matching budget");
            }
            break;
        }
        if (spans[0].end == spans[0].begin) {
            // An empty separator separates nothing; step past a character
            // or the loop stands still.
            if (spans[0].begin >= text->length) {
                break;
            }
            from = spans[0].end +
                   step_width(text->text, text->length, spans[0].end);
            continue;
        }
        LhatValue piece = lhat_nil();
        if (!lhat_machine_make_string(machine, text->text + from,
                                      spans[0].begin - from, &piece) ||
            !lhat_machine_table_set(machine, pieces,
                                    lhat_integer(++position), piece, NULL)) {
            return lhat_nil();
        }
        from = spans[0].end;
    }
    LhatValue tail = lhat_nil();
    if (!lhat_machine_make_string(machine, text->text + from,
                                  text->length - from, &tail) ||
        !lhat_machine_table_set(machine, pieces, lhat_integer(++position),
                                tail, NULL)) {
        return lhat_nil();
    }
    return table;
}

// ---------------------------------------------------------------------------
// The gmatch walk
// ---------------------------------------------------------------------------

// The walk owns its compiled form outright -- the Regex object it came from
// may be disposed while the walk lives, and owning the compilation is
// cheaper than owning that question. The subject rides the coroutine's held.
typedef struct {
    LhatRegex *compiled;
    const LhatString *subject;  // kept alive by held
    size_t from;
} GmatchWalk;

static bool gmatch_step(LhatMachine *machine, void *context,
                        const LhatValue *sent, size_t sent_count,
                        LhatValue *answers, int *answer_count)
{
    (void)sent;
    (void)sent_count;
    GmatchWalk *walk = (GmatchWalk *)context;
    if (walk->from > walk->subject->length) {
        return false;
    }
    LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS];
    bool blown = false;
    if (!lhat_regex_search(walk->compiled, walk->subject->text,
                           walk->subject->length, walk->from, spans,
                           &blown)) {
        return false;  // done -- a blown budget ends the walk too (regex.h)
    }
    // 02 の 13.8改: a `for^ i, s` walk hands over two answers, written
    // where the machine gave the room.
    answers[0] = lhat_integer(
        (int64_t)ordinal_at(walk->subject->text, spans[0].begin));
    answers[1] = span_string(machine, walk->subject->text, spans[0]);
    if (spans[0].end == spans[0].begin) {
        walk->from = spans[0].end + step_width(walk->subject->text,
                                               walk->subject->length,
                                               spans[0].end);
    } else {
        walk->from = spans[0].end;
    }
    *answer_count = 2;
    return true;
}

static void gmatch_release(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    GmatchWalk *walk = (GmatchWalk *)context;
    lhat_regex_free(walk->compiled);
    lhat_free(walk);
}

// Compiles its own form from the pattern text and hands the walk over.
static LhatValue make_gmatch(LhatMachine *machine, const RegexModule *module,
                             const char *pattern, size_t pattern_length,
                             LhatValue subject_value)
{
    (void)module;
    const char *why = NULL;
    size_t at = 0;
    LhatRegex *compiled =
        lhat_regex_compile(pattern, pattern_length, &why, &at);
    if (compiled == NULL) {
        return lhat_nil();  // the caller compiled once already; see callers
    }
    GmatchWalk *walk = (GmatchWalk *)lhat_alloc(sizeof *walk);
    if (walk == NULL) {
        lhat_regex_free(compiled);
        return lhat_nil();
    }
    walk->compiled = compiled;
    walk->subject = (const LhatString *)lhat_as_object(subject_value);
    walk->from = 0;
    LhatValue made = lhat_nil();
    if (!lhat_machine_make_coroutine(machine, gmatch_step, walk,
                                     gmatch_release, subject_value, &made)) {
        lhat_regex_free(compiled);
        lhat_free(walk);
        return lhat_nil();
    }
    return made;
}

// ---------------------------------------------------------------------------
// The registered functions
// ---------------------------------------------------------------------------

static const LhatString *arg_string(LhatValue value)
{
    return lhat_is_object_kind(value, LHAT_OBJECT_STRING)
               ? (const LhatString *)lhat_as_object(value)
               : NULL;
}

static void regex_new(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    const LhatString *pattern = arg_string(arguments[0]);
    if (pattern == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern, "not a pattern");
        *answer_count = 1;
        return;
    }
    const char *why = NULL;
    size_t at = 0;
    LhatRegex *compiled =
        lhat_regex_compile(pattern->text, pattern->length, &why, &at);
    if (compiled == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern,
                         why != NULL ? why : "out of memory");
        *answer_count = 1;
        return;
    }
    Regex *made = (Regex *)lhat_alloc(sizeof *made);
    char *copy = (char *)lhat_alloc(pattern->length ? pattern->length : 1);
    if (made == NULL || copy == NULL) {
        lhat_free(made);
        lhat_free(copy);
        lhat_regex_free(compiled);
        answers[0] = fail_with(machine, module->bad_pattern, "out of memory");
        *answer_count = 1;
        return;
    }
    memcpy(copy, pattern->text, pattern->length);
    made->compiled = compiled;
    made->pattern = copy;
    made->pattern_length = pattern->length;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->tag, made, &out)) {
        lhat_regex_free(compiled);
        lhat_free(copy);
        lhat_free(made);
        answers[0] = fail_with(machine, module->bad_pattern, "out of memory");
        *answer_count = 1;
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static Regex *self_regex(const RegexModule *module, LhatValue receiver)
{
    return (Regex *)lhat_hostdata_pointer(receiver, module->tag);
}

static void regex_match(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (self == NULL || text == NULL) {
        return;
    }
    answers[0] = do_match(machine, module, self->compiled, text);
    *answer_count = 1;
}

static void regex_captures(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (self == NULL || text == NULL) {
        return;
    }
    answers[0] = do_captures(machine, module, self->compiled, text);
    *answer_count = 1;
}

static void regex_gmatch(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    if (self == NULL || arg_string(arguments[1]) == NULL) {
        return;
    }
    answers[0] = make_gmatch(machine, module, self->pattern, self->pattern_length,
                       arguments[1]);
    *answer_count = 1;
}

static void regex_gsub(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (self == NULL || text == NULL) {
        return;
    }
    answers[0] = do_gsub(machine, module, self->compiled, text, arguments[2]);
    *answer_count = 1;
}

static void regex_split(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (self == NULL || text == NULL) {
        return;
    }
    answers[0] = do_split(machine, module, self->compiled, text);
    *answer_count = 1;
}

static void regex_dispose(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    const RegexModule *module = (const RegexModule *)context;
    Regex *self = self_regex(module, arguments[0]);
    if (self != NULL) {
        lhat_regex_free(self->compiled);
        lhat_free(self->pattern);
        lhat_free(self);
    }
}

// The convenience forms: compile, use, free -- every call. A hot path holds
// a new() of its own, which is the whole reason the object exists.
static void module_match(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    const LhatString *pattern = arg_string(arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (pattern == NULL || text == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern, "not a pattern");
        *answer_count = 1;
        return;
    }
    const char *why = NULL;
    size_t at = 0;
    LhatRegex *compiled =
        lhat_regex_compile(pattern->text, pattern->length, &why, &at);
    if (compiled == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern,
                         why != NULL ? why : "out of memory");
        *answer_count = 1;
        return;
    }
    LhatValue answered = do_match(machine, module, compiled, text);
    lhat_regex_free(compiled);
    answers[0] = answered;
    *answer_count = 1;
}

static void module_gmatch(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    const LhatString *pattern = arg_string(arguments[0]);
    if (pattern == NULL || arg_string(arguments[1]) == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern, "not a pattern");
        *answer_count = 1;
        return;
    }
    // The pattern's own compile errors are reported here, where the walk is
    // made -- make_gmatch compiles again for the walk's own copy.
    const char *why = NULL;
    size_t at = 0;
    LhatRegex *check =
        lhat_regex_compile(pattern->text, pattern->length, &why, &at);
    if (check == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern,
                         why != NULL ? why : "out of memory");
        *answer_count = 1;
        return;
    }
    lhat_regex_free(check);
    answers[0] = make_gmatch(machine, module, pattern->text, pattern->length,
                       arguments[1]);
    *answer_count = 1;
}

static void module_gsub(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const RegexModule *module = (const RegexModule *)context;
    const LhatString *pattern = arg_string(arguments[0]);
    const LhatString *text = arg_string(arguments[1]);
    if (pattern == NULL || text == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern, "not a pattern");
        *answer_count = 1;
        return;
    }
    const char *why = NULL;
    size_t at = 0;
    LhatRegex *compiled =
        lhat_regex_compile(pattern->text, pattern->length, &why, &at);
    if (compiled == NULL) {
        answers[0] = fail_with(machine, module->bad_pattern,
                         why != NULL ? why : "out of memory");
        *answer_count = 1;
        return;
    }
    LhatValue answered =
        do_gsub(machine, module, compiled, text, arguments[2]);
    lhat_regex_free(compiled);
    answers[0] = answered;
    *answer_count = 1;
}

bool lhatstdlib_regex_register(LhatProgram *program)
{
    // 05 の 8.7: every field of this is an identity, and an identity
    // belongs to the process rather than to a program -- one declaration,
    // one tag and one error kind, however many programs declare them. So
    // one of these serves them all, and a second registration writes the
    // same answers back into it.
    static RegexModule shared;
    RegexModule *module = &shared;
    static const char *const variants[] = { "BadPattern", "Exhausted" };
    if (!lhat_register_error_kind(program, "std.regex", "Error", variants, 2,
                                  NULL, NULL)) {
        return false;
    }
    module->bad_pattern =
        lhat_lookup_error_kind(program, "std.regex", "Error", "BadPattern");
    module->exhausted =
        lhat_lookup_error_kind(program, "std.regex", "Error", "Exhausted");

    module->tag = lhat_register_hostdata_type(program, "std.regex", "Regex");
    if (module->tag == NULL) {
        return false;
    }

    // 13.4: registration signatures take no parameter names. The gsub
    // replacement is a union of the written string and the per-match
    // function; the function's own ';' closes it inside the union.
    return lhat_register_func(
               program, "std.regex", "new",
               "f^string^ -> std.regex.Regex|std.regex.Error.BadPattern;",
               regex_new, module) &&
           lhat_register_member(
               program, "std.regex", "Regex", "match",
               "f^self^, string^ -> "
               "string^|nil^|std.regex.Error.Exhausted;",
               regex_match, module) &&
           lhat_register_member(
               program, "std.regex", "Regex", "captures",
               "f^self^, string^ -> "
               "t^{string^[]}|nil^|std.regex.Error.Exhausted;",
               regex_captures, module) &&
           lhat_register_member(
               program, "std.regex", "Regex", "gmatch",
               "f^self^, string^ -> c^{f^ -> number^, string^ -> nil^};",
               regex_gmatch, module) &&
           lhat_register_member(
               program, "std.regex", "Regex", "gsub",
               "f^self^, string^, "
               "string^|f^string^, number^, t^{string^[]} -> string^|nil^; "
               "-> string^|std.regex.Error.Exhausted;",
               regex_gsub, module) &&
           lhat_register_member(
               program, "std.regex", "Regex", "split",
               "f^self^, string^ -> "
               "t^{string^[]}|std.regex.Error.Exhausted;",
               regex_split, module) &&
           lhat_register_member(program, "std.regex", "Regex", "dispose",
                                "p^self^;", regex_dispose, module) &&
           lhat_register_func(
               program, "std.regex", "match",
               "f^string^, string^ -> string^|nil^"
               "|std.regex.Error.BadPattern|std.regex.Error.Exhausted;",
               module_match, module) &&
           lhat_register_func(
               program, "std.regex", "gmatch",
               "f^string^, string^ -> c^{f^ -> number^, string^ -> nil^}"
               "|std.regex.Error.BadPattern;",
               module_gmatch, module) &&
           lhat_register_func(
               program, "std.regex", "gsub",
               "f^string^, string^, "
               "string^|f^string^, number^, t^{string^[]} -> string^|nil^; "
               "-> string^|std.regex.Error.BadPattern"
               "|std.regex.Error.Exhausted;",
               module_gsub, module);
}
