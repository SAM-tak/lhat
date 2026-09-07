// L^ (lhat) -- LSP server: what may stand where the cursor is.

#include "completion.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "chain.h"
#include "check.h"
#include "type.h"

#include "util.h"

// LSP's CompletionItemKind, by the numbers the spec assigns. Only the ones
// used.
enum {
    ITEM_METHOD = 2,
    ITEM_FUNCTION = 3,
    ITEM_FIELD = 5,
    ITEM_CLASS = 7,
    ITEM_MODULE = 9,
    ITEM_FILE = 17,
};

// A type written out for one item. Past this a reader is helped by opening
// the definition instead, and lhat_type_write cuts with an ellipsis rather
// than refusing (the same buffer hover keeps, for the same reason).
#define LSP_COMPLETION_TYPE_BUFFER 256

static cJSON *add_item(cJSON *into, const char *label, size_t label_length,
                       int kind, const char *detail)
{
    char *name = lsp_strndup(label, label_length);
    if (name == NULL) {
        return NULL;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        free(name);
        return NULL;
    }
    cJSON_AddItemToArray(into, item);
    cJSON_AddStringToObject(item, "label", name);
    free(name);
    cJSON_AddNumberToObject(item, "kind", kind);
    if (detail != NULL && *detail != '\0') {
        cJSON_AddStringToObject(item, "detail", detail);
    }
    return item;
}

// 14.10 with 13.4: a member that takes a receiver is reached through a value
// and is written as a call on it; one that does not is reached through the
// definition. 13.14 names a type instead, and 05 の 8.7's module tables are
// namespaces rather than values.
static int kind_of_member(const LhatTypeMember *m)
{
    if (m->names_type) {
        return ITEM_CLASS;
    }
    if (lhat_type_takes_receiver(m->type)) {
        return ITEM_METHOD;
    }
    if (m->type != NULL && m->type->kind == LHAT_TYPE_TABLE &&
        m->type->v.table.is_module) {
        return ITEM_MODULE;
    }
    if (m->type != NULL && (m->type->kind == LHAT_TYPE_FUNC ||
                            m->type->kind == LHAT_TYPE_INTERSECT)) {
        return ITEM_FUNCTION;
    }
    return ITEM_FIELD;
}

// 14.10改: a table is a sequence as well as a mapping, and the sequence half
// is members whose names are the digits of their position. 01 の 3.1 spells
// a name as an identifier, so a program can never write one of these -- and
// offering "1" where a member name goes would be offering a spelling the
// language has no way to accept.
static bool is_positional(const LhatTypeMember *m)
{
    if (m->name_length == 0) {
        return false;
    }
    for (size_t i = 0; i < m->name_length; i++) {
        if (!isdigit((unsigned char)m->name[i])) {
            return false;
        }
    }
    return true;
}

// The written half: what the type itself holds. 05 の 8.9: a host value type
// keeps its members on the same list a table keeps its own, and an error kind
// keeps its fields on another.
static void add_written_members(cJSON *items, const LhatType *receiver)
{
    if (receiver->kind == LHAT_TYPE_ERROR_KIND) {
        for (const LhatTypeMember *m = receiver->v.error.fields; m != NULL;
             m = m->next) {
            add_item(items, m->name, m->name_length, ITEM_FIELD, NULL);
        }
        return;
    }
    if (receiver->kind != LHAT_TYPE_TABLE &&
        receiver->kind != LHAT_TYPE_HOSTVALUE) {
        return;
    }

    // 05 の 8.8改 and 14.7改2: two of the three places a member can be are
    // links, so the chain is what the machine walks and this walks it too.
    //
    // Unlike rttype.c's walk (src/rttype.c), this does NOT stop at a host
    // type's tag. That one keeps the tag rather than copying an engine's
    // whole API into every descriptor, which is a size decision about what
    // travels; here the host's API is the very thing a reader is asking for.
    LhatChain walk = lhat_type_chain(receiver);
    const LhatType *up;
    while ((up = lhat_chain_next(&walk)) != NULL) {
        for (const LhatTypeMember *m = up->v.table.members; m != NULL;
             m = m->next) {
            // The one rule, and it is the lookup's own: a member the search
            // does not answer with is one a nearer type shadows, or one a
            // delegate does not lend (14.7改2 lends only what takes a
            // receiver). Asking here is what keeps this from ever offering
            // a name the checker would refuse.
            if (lhat_type_find_member(receiver, m->name, m->name_length) != m) {
                continue;
            }
            // 14.5改: reaching one is refused outright, so offering it would
            // offer a diagnostic.
            if (m->ambiguous || is_positional(m)) {
                continue;
            }
            char written[LSP_COMPLETION_TYPE_BUFFER];
            size_t length = lhat_type_write(m->type, written, sizeof written);
            if (length > sizeof written - 1) {
                length = strlen(written);  // what fits is the cut form
            }
            (void)length;
            // 01 の 2.3: the hat is part of the name, so the label is the
            // spelling the member was written with and nothing is inserted
            // in its place.
            add_item(items, m->name, m->name_length, kind_of_member(m),
                     written);
        }
    }
}

// The built-in half (14.19, 15.6改). Nothing holds these as members: they are
// what the checker answers, so the checker is asked -- once per spelling it
// knows of, with the conditions (which receiver, bare or hatted, whether a
// written member won first) staying where they were written.
static void offer_builtin(void *context, const char *name, size_t length,
                          LhatType *type)
{
    cJSON *items = (cJSON *)context;
    // 14.17改: a written member wins, and it has already been added. Adding
    // the built-in of the same spelling would show one member twice.
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strlen(at->valuestring) == length &&
            memcmp(at->valuestring, name, length) == 0) {
            return;
        }
    }

    char written[LSP_COMPLETION_TYPE_BUFFER];
    size_t room = lhat_type_write(type, written, sizeof written);
    if (room > sizeof written - 1) {
        room = strlen(written);
    }
    (void)room;
    add_item(items, name, length,
             lhat_type_takes_receiver(type) ? ITEM_METHOD
             : type != NULL && type->kind == LHAT_TYPE_FUNC ? ITEM_FUNCTION
                                                            : ITEM_FIELD,
             written);
}

cJSON *lsp_completion_members_of(LhatCheckResult *result,
                                 LhatType *receiver)
{
    cJSON *items = cJSON_CreateArray();
    if (items == NULL || receiver == NULL) {
        return items;
    }
    // The written ones first, so the dedupe above has them to compare with.
    add_written_members(items, receiver);
    if (result != NULL) {
        lhat_check_builtin_members(result, receiver, offer_builtin, items);
    }
    return items;
}

cJSON *lsp_completion_members_for_unit(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL) {
        return cJSON_CreateArray();
    }
    const LhatMemberSite *site =
        lhat_check_member_site_at(&unit->checked, offset);
    if (site == NULL) {
        return cJSON_CreateArray();
    }
    // 02 の 14.8改2: 'number^.' is the word, and the constants are the whole
    // of what stands there -- not members of a number, which is a different
    // question with a different answer.
    if (site->number_word) {
        cJSON *items = cJSON_CreateArray();
        if (items != NULL) {
            lhat_check_number_constants((LhatCheckResult *)&unit->checked,
                                        offer_builtin, items);
        }
        return items;
    }
    // The unit's own result: the built-ins are made in its arena, which lives
    // exactly as long as the unit the answer is about.
    return lsp_completion_members_of((LhatCheckResult *)&unit->checked,
                                     site->receiver);
}

// ---------------------------------------------------------------------------
// The two that are read off the text
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

// 01 の 6.1: a '#' begins a comment, so a word after one on the same line is
// prose rather than a path. Only the line matters -- a block comment would
// take the lexer to say, and a request inside one answers nothing worse than
// a list nobody asked for.
static bool commented(const char *text, uint32_t from, uint32_t to)
{
    for (uint32_t at = from; at < to; at++) {
        if (text[at] == '#') {
            return true;
        }
    }
    return false;
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

bool lsp_completion_import_prefix(const char *text, size_t length,
                                  uint32_t offset, uint32_t *from)
{
    if (text == NULL || offset > length) {
        return false;
    }
    uint32_t start = line_start(text, offset);
    uint32_t at = offset;
    while (at > start && is_path_byte((unsigned char)text[at - 1])) {
        at--;
    }
    uint32_t word = 0;
    if (!word_before(text, start, at, "import^", &word) ||
        commented(text, start, word)) {
        return false;
    }
    *from = at;
    return true;
}

bool lsp_completion_require_prefix(const char *text, size_t length,
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
        commented(text, start, word)) {
        return false;
    }
    *from = at;
    return true;
}

// ---------------------------------------------------------------------------
// Modules and paths
// ---------------------------------------------------------------------------

static bool already_offered(cJSON *items, const char *label, size_t length)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsString(at) && strlen(at->valuestring) == length &&
            memcmp(at->valuestring, label, length) == 0) {
            return true;
        }
    }
    return false;
}

cJSON *lsp_completion_module_items(const char *const *modules, size_t count,
                                   const char *prefix, size_t prefix_length)
{
    cJSON *items = cJSON_CreateArray();
    if (items == NULL || modules == NULL) {
        return items;
    }
    for (size_t i = 0; i < count; i++) {
        const char *module = modules[i];
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
        if (length == 0 || already_offered(items, rest, length)) {
            continue;
        }
        add_item(items, rest, length, ITEM_MODULE, NULL);
    }
    return items;
}

// How many bytes of the two paths agree, cut back to the last '/' so that
// "src/a" and "src/ab" share "src/" and not "src/a".
static size_t shared_prefix(const char *a, const char *b)
{
    size_t at = 0;
    size_t last_slash = 0;
    while (a[at] != '\0' && a[at] == b[at]) {
        if (a[at] == '/') {
            last_slash = at + 1;
        }
        at++;
    }
    return last_slash;
}

char *lsp_completion_relative_path(const char *from, const char *target)
{
    if (from == NULL || target == NULL) {
        return NULL;
    }
    // The directory the requiring unit stands in, which is what 05 の 5 章
    // resolves a written path against (program.c's resolve_against).
    const char *slash = strrchr(from, '/');
    size_t base_length = slash != NULL ? (size_t)(slash - from) + 1 : 0;
    char *base = lsp_strndup(from, base_length);
    if (base == NULL) {
        return NULL;
    }

    size_t shared = shared_prefix(base, target);
    size_t ups = 0;
    for (size_t at = shared; base[at] != '\0'; at++) {
        if (base[at] == '/') {
            ups++;
        }
    }
    free(base);

    const char *tail = target + shared;
    size_t room = ups * 3 + strlen(tail) + 1;
    char *written = (char *)malloc(room);
    if (written == NULL) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < ups; i++) {
        memcpy(written + used, "../", 3);
        used += 3;
    }
    memcpy(written + used, tail, strlen(tail) + 1);
    return written;
}

cJSON *lsp_completion_path_items(const char *unit_path,
                                 const char *const *candidates, size_t count)
{
    cJSON *items = cJSON_CreateArray();
    if (items == NULL || candidates == NULL || unit_path == NULL) {
        return items;
    }
    for (size_t i = 0; i < count; i++) {
        if (candidates[i] == NULL || strcmp(candidates[i], unit_path) == 0) {
            continue;  // 05 の 6.3: a unit does not require^ itself
        }
        char *written = lsp_completion_relative_path(unit_path, candidates[i]);
        if (written == NULL) {
            continue;
        }
        add_item(items, written, strlen(written), ITEM_FILE, NULL);
        free(written);
    }
    return items;
}
