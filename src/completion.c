// L^ (lhat) -- what may stand where the cursor is.
//
// See include/lhat/completion.h for which of the four questions this answers
// and why they are the ones a host cannot compute. What is here is the two
// that read the checker's own record -- the receiver of a dot, and the names
// in scope -- plus the words of the language, which no list anywhere else
// holds, and the text classification that tells the four apart.
//
// None of it works out a lookup for itself. lhat_type_find_member decides
// which member a receiver answers with, lhat_check_builtin_members decides
// which built-ins it carries, and lhat_check_bindings_at decides what is in
// scope; reading 02 の 14.10 or 8 章 a second time here would disagree with
// the checker exactly where the rules are hard, which is what 07 の 4 章
// already refused for hover.

#include "lhat/completion.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "lhat/port.h"
#include "program_internal.h"
#include "type.h"

#if LHAT_WITH_RESOLUTIONS

// The answer, built whole before any of it is handed over.
//
// It would be smaller to write straight into the caller's array and count
// past the end of it, the way lhat_unit_semantic_names does. It would also
// be wrong here: three of the four sources drop a name another already
// offered (02 の 14.17改 has a written member beat a built-in, 8 章 has an
// inner binding shadow an outer, a word yields to a name of its spelling),
// and a measuring call with no array to read has nothing to compare against.
// Measuring would count the duplicates and filling would drop them, so the
// two calls would disagree -- which is the one thing the caller is promised
// they never do. So the list is built here and copied out.
typedef struct {
    LhatCompletionItem *items;
    size_t count;
    size_t capacity;
    bool failed;  // out of memory; the answer is as far as it got
} Fill;

// One item. `detail` may be NULL.
static void add(Fill *fill, const char *label, size_t length,
                LhatCompletionKind kind, const char *detail)
{
    if (fill->count == fill->capacity) {
        size_t wanted = fill->capacity > 0 ? fill->capacity * 2 : 64;
        LhatCompletionItem *bigger = (LhatCompletionItem *)lhat_realloc(
            fill->items, wanted * sizeof *bigger);
        if (bigger == NULL) {
            fill->failed = true;
            return;
        }
        fill->items = bigger;
        fill->capacity = wanted;
    }
    LhatCompletionItem *item = &fill->items[fill->count++];
    item->kind = kind;
    if (length >= sizeof item->label) {
        length = sizeof item->label - 1;
    }
    memcpy(item->label, label, length);
    item->label[length] = '\0';
    item->detail[0] = '\0';
    if (detail != NULL && *detail != '\0') {
        size_t room = strlen(detail);
        if (room >= sizeof item->detail) {
            room = sizeof item->detail - 1;
        }
        memcpy(item->detail, detail, room);
        item->detail[room] = '\0';
    }
}

// Whether one of these labels is on the list already.
static bool already(const Fill *fill, const char *label, size_t length)
{
    for (size_t i = 0; i < fill->count; i++) {
        if (strlen(fill->items[i].label) == length &&
            memcmp(fill->items[i].label, label, length) == 0) {
            return true;
        }
    }
    return false;
}

// 14.10 with 13.4: a member that takes a receiver is reached through a value
// and is written as a call on it; one that does not is reached through the
// definition. 13.14 names a type instead, and 05 の 8.7's module tables are
// namespaces rather than values.
static LhatCompletionKind kind_of_member(const LhatTypeMember *m)
{
    if (m->names_type) {
        return LHAT_COMPLETION_CLASS;
    }
    if (lhat_type_takes_receiver(m->type)) {
        return LHAT_COMPLETION_METHOD;
    }
    if (m->type != NULL && m->type->kind == LHAT_TYPE_TABLE &&
        m->type->v.table.is_module) {
        return LHAT_COMPLETION_MODULE_NAME;
    }
    if (m->type != NULL && (m->type->kind == LHAT_TYPE_FUNC ||
                            m->type->kind == LHAT_TYPE_INTERSECT)) {
        return LHAT_COMPLETION_FUNCTION;
    }
    return LHAT_COMPLETION_FIELD;
}

// The written half: what the type itself holds, as the checker's walk lists
// it (lhat_check_written_members), so what is offered is what an access
// would accept.
static void offer_written(void *context, const LhatTypeMember *m)
{
    Fill *fill = (Fill *)context;
    char written[LHAT_COMPLETION_DETAIL];
    lhat_type_write(m->type, written, sizeof written);
    // 01 の 2.3: the hat is part of the name, so the label is the spelling
    // the member was written with and nothing is inserted in its place.
    add(fill, m->name, m->name_length, kind_of_member(m), written);
}


// The built-in half (02 の 14.19, 15.6改). Nothing holds these as members:
// they are what the checker answers, so the checker is asked -- once per
// spelling it knows of, with the conditions (which receiver, bare or hatted,
// whether a written member won first) staying where they were written.
static void offer_builtin(void *context, const char *name, size_t length,
                          LhatType *type)
{
    Fill *fill = (Fill *)context;
    // 14.17改: a written member wins, and it has already been added. Adding
    // the built-in of the same spelling would show one member twice.
    if (already(fill, name, length)) {
        return;
    }
    char written[LHAT_COMPLETION_DETAIL];
    lhat_type_write(type, written, sizeof written);
    add(fill, name, length,
        lhat_type_takes_receiver(type)  ? LHAT_COMPLETION_METHOD
        : type != NULL && type->kind == LHAT_TYPE_FUNC
            ? LHAT_COMPLETION_FUNCTION
            : LHAT_COMPLETION_FIELD,
        written);
}

// 13.14 with 05 の 8.7 and 13.4: what the name holds is what says how to draw
// it. A binding that names a type is the one the type cannot say for itself.
static LhatCompletionKind kind_of_binding(const LhatBindingSite *site)
{
    if (site->names_type) {
        return LHAT_COMPLETION_CLASS;
    }
    if (site->type != NULL && site->type->kind == LHAT_TYPE_TABLE &&
        site->type->v.table.is_module) {
        return LHAT_COMPLETION_MODULE_NAME;
    }
    if (site->type != NULL && (site->type->kind == LHAT_TYPE_FUNC ||
                               site->type->kind == LHAT_TYPE_INTERSECT)) {
        return LHAT_COMPLETION_FUNCTION;
    }
    return LHAT_COMPLETION_VARIABLE;
}


// 07 の 4 章: a name some scope around the cursor holds. The checker walks
// the record innermost first, so the first of any spelling is the one 8 章's
// lookup would have found and every later one is shadowed.
static void offer_binding(void *context, const LhatBindingSite *site)
{
    Fill *fill = (Fill *)context;
    if (site->name_length == 0 ||
        already(fill, site->name, site->name_length)) {
        return;
    }
    // 13.7: the collector a script's top level takes. It is a name no one
    // writes as one -- 'p^...' spells it -- so offering it offers nothing.
    if (site->name_length == 3 && memcmp(site->name, "...", 3) == 0) {
        return;
    }
    char written[LHAT_COMPLETION_DETAIL];
    lhat_type_write(site->type, written, sizeof written);
    add(fill, site->name, site->name_length, kind_of_binding(site), written);
}


// ---------------------------------------------------------------------------
// Where the cursor stands
// ---------------------------------------------------------------------------

// 01 の 3.1 with 2.3: what a module path is made of. The hat is here because
// a segment may carry one, and the dot because the path is what is being
// written.
static bool is_path_byte(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '^' || c >= 0x80;
}

// Where the line the cursor is on begins.
static uint32_t line_start(const char *text, uint32_t offset)
{
    uint32_t at = offset;
    while (at > 0 && text[at - 1] != '\n') {
        at--;
    }
    return at;
}

// Whether `to` stands in code, judged from `from` -- which is the start of
// its line. A '#' begins a comment and a '"' a string, and each closes what
// the other would have begun, so the two are read in the one pass.
//
// Only the line matters: a string running over one would take the lexer to
// say, and a request inside one answers nothing worse than a list nobody
// asked for.
static bool in_code(const char *text, uint32_t from, uint32_t to)
{
    bool quoted = false;
    for (uint32_t at = from; at < to; at++) {
        if (quoted && text[at] == '\\') {
            at++;  // whatever it escapes is not the quote that closes
        } else if (text[at] == '"') {
            quoted = !quoted;
        } else if (!quoted && text[at] == '#') {
            return false;
        }
    }
    return !quoted;
}

// Whether `word` stands immediately before `at`, with only spaces between.
static bool word_before(const char *text, uint32_t start, uint32_t at,
                        const char *word, uint32_t *word_at)
{
    size_t length = strlen(word);
    while (at > start && (text[at - 1] == ' ' || text[at - 1] == '\t')) {
        at--;
    }
    if (at < start + length) {
        return false;
    }
    if (memcmp(text + at - length, word, length) != 0) {
        return false;
    }
    *word_at = at - (uint32_t)length;
    return true;
}

static bool import_prefix(const char *text, size_t length,
                          uint32_t offset, uint32_t *from)
{
    if (text == NULL || offset > length) {
        return false;
    }
    static const char WORD[] = "import^";
    const size_t spelt = sizeof WORD - 1;

    uint32_t start = line_start(text, offset);
    uint32_t at = offset;
    while (at > start && is_path_byte((unsigned char)text[at - 1])) {
        at--;
    }
    // 01 の 2.3: the hat is part of a name, so the walk above runs straight
    // through `import^` where nothing separates it from the path --
    // 'import^godot' is one run of name bytes and reads as a name being
    // written. The word is a fixed spelling, so where the run begins with it
    // the path begins just past it. A space puts the word behind the run
    // instead, and then it is looked for there.
    if ((size_t)(offset - at) >= spelt &&
        memcmp(text + at, WORD, spelt) == 0) {
        if (!in_code(text, start, at)) {
            return false;
        }
        *from = at + (uint32_t)spelt;
        return true;
    }
    uint32_t word = 0;
    if (!word_before(text, start, at, WORD, &word) ||
        !in_code(text, start, word)) {
        return false;
    }
    *from = at;
    return true;
}

static bool require_prefix(const char *text, size_t length,
                           uint32_t offset, uint32_t *from)
{
    if (text == NULL || offset > length) {
        return false;
    }
    uint32_t start = line_start(text, offset);
    uint32_t at = offset;
    // Inside the string, so anything but the quote that opened it and the
    // newline that would have ended the line.
    while (at > start && text[at - 1] != '"') {
        at--;
    }
    if (at == start || at == 0) {
        return false;
    }
    uint32_t word = 0;
    if (!word_before(text, start, at - 1, "require^", &word) ||
        !in_code(text, start, word)) {
        return false;
    }
    *from = at;
    return true;
}

// 01 の 3.1 with 2.3: what a word is made of. A '.' is not here -- it ends
// the word and begins a member, which is a different question.
static bool is_word_byte(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '^' || c >= 0x80;
}

static bool word_prefix(const char *text, size_t length,
                        uint32_t offset, uint32_t *from)
{
    if (text == NULL || offset > length) {
        return false;
    }
    uint32_t start = line_start(text, offset);
    uint32_t at = offset;
    while (at > start && is_word_byte((unsigned char)text[at - 1])) {
        at--;
    }
    // A member name is written in a word too, and the receiver decides what
    // may stand there -- so the dot hands the question on rather than
    // answering it. 11.7改2's '?.' ends in the same byte.
    if (at > 0 && text[at - 1] == '.') {
        return false;
    }
    // 01 の 3.1: a name never begins with a digit, so what does is a number
    // being written and no word can follow it.
    if (at < offset && isdigit((unsigned char)text[at])) {
        return false;
    }
    if (!in_code(text, start, at)) {
        return false;
    }
    *from = at;
    return true;
}


// ---------------------------------------------------------------------------
// The words of the language
// ---------------------------------------------------------------------------

// 01 の 2.1: the lexer keeps no keyword table -- every hatted word is the one
// token kind and the parser decides which of them each is (parser.c's
// is_statement_keyword carries its own half of that knowledge for the same
// reason). So there is no list anywhere to read, and this is one.
//
// It is a list of candidates, not an authority. Nothing is checked against
// it and nothing is refused by it: a word missing here is one suggestion
// that does not appear, and a word here that the language no longer takes is
// one the checker reports the moment it is written. When the language gains
// a word, add it -- the same bargain chk_builtin_words[] (src/check_expr.c)
// strikes for members.
typedef struct {
    const char *word;
    LhatCompletionKind kind;
} Word;

static const Word WORDS[] = {
    // 8.9 with 12 章: what binds a name.
    {"let^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"var^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"with^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 6 章 and 9 章: the shapes a body takes. The else marker is written six
    // ways, and which of them a writer likes is not the server's to decide.
    {"if^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"el^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"ei^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"else^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"elif^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"elseif^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"elsif^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"do^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"when^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"other^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"for^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"while^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"repeat^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"until^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 16.3: the clauses of a for^ -- what it walks and how far.
    {"in^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"from^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"to^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"downto^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"step^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 9.4: the parts a loop body divides into.
    {"prolog^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"prologue^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"pre^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"premain^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"first^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"main^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"last^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"epilog^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"epilogue^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 9.11 with 15.8 and 15.14: what leaves, and what suspends.
    {"return^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"break^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"next^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"skip^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"continue^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"panic^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"yield^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"_yield^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"await^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 04 の 2 章: what an error is declared and caught with. 2.7 has two
    // tops, so it has two of the word that declares one.
    {"try^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"catch^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"finally^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"errordef^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"localerrordef^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 13 章 with 14 章: what makes a subroutine and what makes a definition.
    {"f^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"p^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"def^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"enum^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"op^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"id^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"abstract^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"override^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"overload^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"delegate^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"public^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"mutable^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"closed^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"fresh^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"pack^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"box^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"constbox^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 05 の 6.1 and 8.7: what brings another unit or a host's module in.
    {"import^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"require^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"module^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // 4.1 with 13.11: the operators that are words.
    {"and^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"or^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"is^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"as^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"fits^", LHAT_COMPLETION_WORD_OF_LANGUAGE},
    {"typeof^", LHAT_COMPLETION_WORD_OF_LANGUAGE},

    // The types a name is not needed for (check.c's builtin_type), with
    // 13.13's word for the literal being written inside.
    {"number^", LHAT_COMPLETION_CLASS},
    {"int^", LHAT_COMPLETION_CLASS},
    {"float^", LHAT_COMPLETION_CLASS},
    {"string^", LHAT_COMPLETION_CLASS},
    {"bool^", LHAT_COMPLETION_CLASS},
    {"any^", LHAT_COMPLETION_CLASS},
    {"error^", LHAT_COMPLETION_CLASS},
    {"localerror^", LHAT_COMPLETION_CLASS},
    {"t^", LHAT_COMPLETION_CLASS},
    {"c^", LHAT_COMPLETION_CLASS},
    {"Self^", LHAT_COMPLETION_CLASS},

    // The values no binding holds.
    {"nil^", LHAT_COMPLETION_CONSTANT},
    {"true^", LHAT_COMPLETION_CONSTANT},
    {"false^", LHAT_COMPLETION_CONSTANT},

    // 14 章 with 05 の 8.6: the names a construct puts there, and the one the
    // language itself carries. 8.1 keeps them out of what a host can bind,
    // so no scope holds them and nothing else would offer them.
    {"self^", LHAT_COMPLETION_VARIABLE},
    {"this^", LHAT_COMPLETION_VARIABLE},
    {"it^", LHAT_COMPLETION_VARIABLE},
    {"super^", LHAT_COMPLETION_VARIABLE},
    {"L^", LHAT_COMPLETION_VARIABLE},
    {"_^", LHAT_COMPLETION_VARIABLE},
};

// ---------------------------------------------------------------------------
// Remembering what a receiver answers
// ---------------------------------------------------------------------------
//
// Members are the expensive question: written_members walks a list, but
// lhat_check_builtin_members asks after every built-in spelling the language
// knows of, one at a time, and each of those does work that grows with the
// receiver. A popup asks on every keystroke and the receiver does not change
// between them, so the same answer is built over and over -- milliseconds
// apiece for a host type as wide as a game engine's node.
//
// What is remembered hangs off the program rather than off the check result,
// because a keystroke rechecks the unit: a check-result cache would be thrown
// away exactly as often as it would be read. program_internal.h says why the
// receiver's address is a key worth holding.

// The registrations an answer was built against.
typedef struct {
    size_t entries;
    size_t types;
    size_t enums;
} Registered;

static Registered registered_now(const LhatProgram *program)
{
    Registered at;
    at.entries = program->host_entry_count;
    at.types = program->host_type_entry_count;
    at.enums = program->host_enum_count;
    return at;
}

static bool same_registrations(Registered a, Registered b)
{
    return a.entries == b.entries && a.types == b.types && a.enums == b.enums;
}

// Empties the slots if a registration has arrived since they were filled,
// which is the one thing that changes what a receiver answers: a member
// registered onto a host type is written into the very type these are keyed
// by. Called holding the program's lock.
static void drop_if_stale(LhatProgram *program)
{
    Registered now = registered_now(program);
    Registered then;
    then.entries = program->completion_from_entries;
    then.types = program->completion_from_types;
    then.enums = program->completion_from_enums;
    if (same_registrations(now, then)) {
        return;
    }
    lhat_program_forget_completions(program);
    program->completion_from_entries = now.entries;
    program->completion_from_types = now.types;
    program->completion_from_enums = now.enums;
}

// Which slot a receiver belongs in.
//
// Types made one after another sit at addresses that run consecutively, and
// the receivers a file asks about are made one after another -- so the raw
// address would put a run of them in a run of slots and leave the rest
// empty. Mixing is what spreads them.
static size_t slot_of(const LhatType *receiver)
{
    uintptr_t bits = (uintptr_t)receiver;
    bits ^= bits >> 13;
    bits *= (uintptr_t)0x9E3779B1u;
    bits ^= bits >> 15;
    return (size_t)(bits % LHAT_COMPLETION_REMEMBERED);
}

// What was remembered for this receiver, copied into `fill`; false if nothing
// was. `seen` comes back with the registrations the answer would be built
// against, for remember() to check it is still building against them.
static bool recall(LhatProgram *program, const LhatType *receiver, Fill *fill,
                   Registered *seen)
{
    bool found = false;
    lhat_program_hold(program);
    drop_if_stale(program);
    *seen = registered_now(program);
    if (program->completions != NULL) {
        const LhatCompletionCached *entry =
            &program->completions[slot_of(receiver)];
        if (entry->receiver == receiver) {
            // Copied rather than handed over: hand_over frees what the Fill
            // holds, and this has to survive being answered many times.
            found = true;
            if (entry->count > 0) {
                size_t bytes = entry->count * sizeof *entry->items;
                LhatCompletionItem *items =
                    (LhatCompletionItem *)lhat_alloc(bytes);
                if (items == NULL) {
                    found = false;  // as though nothing was kept; it is rebuilt
                } else {
                    memcpy(items, entry->items, bytes);
                    lhat_free(fill->items);
                    fill->items = items;
                    fill->capacity = entry->count;
                }
            }
            if (found) {
                fill->count = entry->count;
                program->completion_hits++;
            }
        }
    }
    lhat_program_release(program);
    return found;
}

// Keeps this receiver's answer, if it is still an answer to the question that
// was asked. Failing to keep it costs only the time to build it again.
static void remember(LhatProgram *program, const LhatType *receiver,
                     const Fill *fill, Registered seen)
{
    if (fill->failed) {
        return;  // an answer cut short is not the answer
    }
    LhatCompletionItem *items = NULL;
    if (fill->count > 0) {
        size_t bytes = fill->count * sizeof *fill->items;
        items = (LhatCompletionItem *)lhat_alloc(bytes);
        if (items == NULL) {
            return;
        }
        memcpy(items, fill->items, bytes);
    }
    lhat_program_hold(program);
    drop_if_stale(program);
    // A registration that arrived while this was being built makes it the
    // answer to an older program's question. Let it go rather than keep it.
    if (!same_registrations(registered_now(program), seen)) {
        lhat_program_release(program);
        lhat_free(items);
        return;
    }
    if (program->completions == NULL) {
        program->completions = (LhatCompletionCached *)lhat_calloc(
            LHAT_COMPLETION_REMEMBERED, sizeof *program->completions);
        if (program->completions == NULL) {
            lhat_program_release(program);
            lhat_free(items);
            return;
        }
    }
    LhatCompletionCached *entry = &program->completions[slot_of(receiver)];
    lhat_free(entry->items);
    entry->receiver = receiver;
    entry->items = items;
    entry->count = fill->count;
    lhat_program_release(program);
}

// ---------------------------------------------------------------------------
// What the four questions answer
// ---------------------------------------------------------------------------

static void member_items(Fill *fill, const LhatUnit *unit, uint32_t offset)
{
    const LhatMemberSite *site =
        lhat_check_member_site_at(&unit->checked, offset);
    if (site == NULL) {
        return;
    }
    LhatCheckResult *result = (LhatCheckResult *)&unit->checked;
    // 02 の 14.8改2: 'number^.' is the word, and the constants are the whole
    // of what stands there -- not members of a number, which is a different
    // question with a different answer.
    if (site->number_word) {
        lhat_check_number_constants(result, offer_builtin, fill);
        return;
    }
    if (site->receiver == NULL) {
        return;
    }
    LhatProgram *program = unit->program;
    Registered seen;
    memset(&seen, 0, sizeof seen);
    if (program != NULL && recall(program, site->receiver, fill, &seen)) {
        return;
    }
    // The written ones first, so the dedupe above has them to compare with.
    lhat_check_written_members(site->receiver, offer_written, fill);
    lhat_check_builtin_members(result, site->receiver, offer_builtin, fill);
    if (program != NULL) {
        remember(program, site->receiver, fill, seen);
    }
}

static void add_words(Fill *fill)
{
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        size_t length = strlen(WORDS[i].word);
        if (already(fill, WORDS[i].word, length)) {
            continue;
        }
        add(fill, WORDS[i].word, length, WORDS[i].kind, NULL);
    }
}

static void word_items(Fill *fill, const LhatUnit *unit, uint32_t offset)
{
    // What the program itself put there, before what the language carries: a
    // name is worth more to the writer than a word they could have typed
    // without asking, and going first is also what lets a binding named
    // 'self^' show with the type it actually holds.
    lhat_check_bindings_at(&unit->checked, offset, offer_binding, fill);
    add_words(fill);
}

// What a Fill holds, handed to the caller and let go. The one place the
// answer crosses, so the copying rule lives here once.
static size_t hand_over(Fill *fill, LhatCompletionItem *into, size_t capacity)
{
    size_t answered = fill->count;
    if (into != NULL && fill->items != NULL) {
        size_t room = capacity < answered ? capacity : answered;
        memcpy(into, fill->items, room * sizeof *into);
    }
    lhat_free(fill->items);
    return answered;
}

size_t lhat_completion_words(LhatCompletionItem *into, size_t capacity)
{
    Fill fill;
    memset(&fill, 0, sizeof fill);
    add_words(&fill);
    return hand_over(&fill, into, capacity);
}

// The module a registration named, by number over both lists -- an enum may
// be the only thing registered under its module, so the entries alone do not
// name every module there is.
static const char *registered_module(const LhatProgram *program, size_t at)
{
    if (at < program->host_entry_count) {
        return program->host_entries[at].module;
    }
    at -= program->host_entry_count;
    return at < program->host_enum_count ? program->host_enums[at].module
                                         : NULL;
}

size_t lhat_program_completion_modules(const LhatProgram *program,
                                       const char *prefix,
                                       size_t prefix_length,
                                       LhatCompletionItem *into,
                                       size_t capacity)
{
    Fill fill;
    memset(&fill, 0, sizeof fill);
    if (program == NULL) {
        return 0;
    }
    if (prefix == NULL) {
        prefix_length = 0;
    }
    size_t all = program->host_entry_count + program->host_enum_count;
    for (size_t i = 0; i < all; i++) {
        const char *module = registered_module(program, i);
        if (module == NULL || strlen(module) <= prefix_length ||
            (prefix_length > 0 &&
             memcmp(module, prefix, prefix_length) != 0)) {
            continue;
        }
        // The one segment after the prefix, so "std." offers "io" rather
        // than "std.io" -- what is written next is a segment, and the rest
        // of the path is offered again once its own dot is typed.
        const char *rest = module + prefix_length;
        const char *dot = strchr(rest, '.');
        size_t length = dot != NULL ? (size_t)(dot - rest) : strlen(rest);
        if (length == 0 || already(&fill, rest, length)) {
            continue;
        }
        add(&fill, rest, length, LHAT_COMPLETION_MODULE_NAME, NULL);
    }
    return hand_over(&fill, into, capacity);
}

LhatCompletionAsk lhat_completion_ask_text(const char *text, size_t length,
                                           uint32_t offset, uint32_t *from)
{
    uint32_t ignored = 0;
    if (from == NULL) {
        from = &ignored;
    }
    *from = offset;
    if (text == NULL || offset > length) {
        return LHAT_COMPLETION_NOTHING;
    }
    // The two paths before the word, since all three begin with a run of
    // name bytes and only what stands to their left tells them apart.
    if (import_prefix(text, length, offset, from)) {
        return LHAT_COMPLETION_MODULE;
    }
    if (require_prefix(text, length, offset, from)) {
        return LHAT_COMPLETION_UNIT;
    }
    if (word_prefix(text, length, offset, from)) {
        return LHAT_COMPLETION_WORD;
    }
    *from = offset;
    return LHAT_COMPLETION_NOTHING;
}

LhatCompletionAsk lhat_unit_completion_ask(const LhatUnit *unit,
                                           uint32_t offset, uint32_t *from)
{
    if (unit == NULL) {
        return lhat_completion_ask_text(NULL, 0, offset, from);
    }
    // A dot before the cursor settles it: what may stand there is the
    // receiver's to say and nothing else's. This is the one the text cannot
    // answer -- only the checker knows the dot was an access rather than the
    // point of a number.
    if (offset <= unit->source.length &&
        lhat_check_member_site_at(&unit->checked, offset) != NULL) {
        if (from != NULL) {
            *from = offset;
        }
        return LHAT_COMPLETION_MEMBER;
    }
    return lhat_completion_ask_text(unit->source.text, unit->source.length,
                                    offset, from);
}

size_t lhat_unit_completion_items(const LhatUnit *unit, uint32_t offset,
                                  LhatCompletionItem *into, size_t capacity)
{
    Fill fill;
    memset(&fill, 0, sizeof fill);
    switch (lhat_unit_completion_ask(unit, offset, NULL)) {
        case LHAT_COMPLETION_MEMBER:
            member_items(&fill, unit, offset);
            break;
        case LHAT_COMPLETION_WORD:
            word_items(&fill, unit, offset);
            break;
        default:
            break;  // the host's lists, or nothing at all
    }
    return hand_over(&fill, into, capacity);
}

#else  // LHAT_WITH_RESOLUTIONS

// Without the records there is nothing to read: what a name means is what
// the checker wrote down, and this build writes nothing down. The text half
// would still answer, but a host that cannot be told the members of a
// receiver is better told so plainly than left to wonder why one question of
// the four is silent.
LhatCompletionAsk lhat_completion_ask_text(const char *text, size_t length,
                                           uint32_t offset, uint32_t *from)
{
    (void)text;
    (void)length;
    if (from != NULL) {
        *from = offset;
    }
    return LHAT_COMPLETION_NOTHING;
}

LhatCompletionAsk lhat_unit_completion_ask(const LhatUnit *unit,
                                           uint32_t offset, uint32_t *from)
{
    (void)unit;
    if (from != NULL) {
        *from = offset;
    }
    return LHAT_COMPLETION_NOTHING;
}

size_t lhat_unit_completion_items(const LhatUnit *unit, uint32_t offset,
                                  LhatCompletionItem *into, size_t capacity)
{
    (void)unit;
    (void)offset;
    (void)into;
    (void)capacity;
    return 0;
}

size_t lhat_completion_words(LhatCompletionItem *into, size_t capacity)
{
    (void)into;
    (void)capacity;
    return 0;
}

size_t lhat_program_completion_modules(const LhatProgram *program,
                                       const char *prefix,
                                       size_t prefix_length,
                                       LhatCompletionItem *into,
                                       size_t capacity)
{
    (void)program;
    (void)prefix;
    (void)prefix_length;
    (void)into;
    (void)capacity;
    return 0;
}

#endif  // LHAT_WITH_RESOLUTIONS
