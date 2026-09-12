// L^ (lhat) -- LSP server: what may stand where the cursor is.

#include "completion.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain.h"
#include "check.h"
#include "type.h"

#include "util.h"

#include "lhat/completion.h"

// LSP's CompletionItemKind, by the numbers the spec assigns. Only the ones
// used.
enum {
    ITEM_METHOD = 2,
    ITEM_FUNCTION = 3,
    ITEM_VARIABLE = 6,
    ITEM_FIELD = 5,
    ITEM_CLASS = 7,
    ITEM_MODULE = 9,
    ITEM_KEYWORD = 14,
    ITEM_FILE = 17,
    ITEM_CONSTANT = 21,
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

// Whether one of these labels is on the list already.
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

// LSP's CompletionItemKind for what the core answered. The two lists are
// coarse in the same places, which is why this is a table and not a
// judgement -- lhat/completion.h's kinds were cut to LSP's own legend.
static int kind_of(LhatCompletionKind kind)
{
    switch (kind) {
        case LHAT_COMPLETION_FIELD:            return ITEM_FIELD;
        case LHAT_COMPLETION_METHOD:           return ITEM_METHOD;
        case LHAT_COMPLETION_FUNCTION:         return ITEM_FUNCTION;
        case LHAT_COMPLETION_CLASS:            return ITEM_CLASS;
        case LHAT_COMPLETION_MODULE_NAME:      return ITEM_MODULE;
        case LHAT_COMPLETION_WORD_OF_LANGUAGE: return ITEM_KEYWORD;
        case LHAT_COMPLETION_CONSTANT:         return ITEM_CONSTANT;
        case LHAT_COMPLETION_VARIABLE:
        default:                               return ITEM_VARIABLE;
    }
}

// What the core answered at `offset`, as a CompletionItem[]. Measured and
// then filled, which is the bargain lhat_unit_completion_items states.
static cJSON *items_from_core(const LhatUnit *unit, uint32_t offset)
{
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        return NULL;
    }
    size_t count = lhat_unit_completion_items(unit, offset, NULL, 0);
    if (count == 0) {
        return items;
    }
    LhatCompletionItem *answered =
        (LhatCompletionItem *)calloc(count, sizeof *answered);
    if (answered == NULL) {
        return items;
    }
    count = lhat_unit_completion_items(unit, offset, answered, count);
    for (size_t i = 0; i < count; i++) {
        add_item(items, answered[i].label, strlen(answered[i].label),
                 kind_of(answered[i].kind), answered[i].detail);
    }
    free(answered);
    return items;
}

// The words of the language alone, for a buffer with no unit behind it.
static cJSON *items_of_words(void)
{
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        return NULL;
    }
    size_t count = lhat_completion_words(NULL, 0);
    LhatCompletionItem *words =
        count > 0 ? (LhatCompletionItem *)calloc(count, sizeof *words) : NULL;
    if (words == NULL) {
        return items;
    }
    count = lhat_completion_words(words, count);
    for (size_t i = 0; i < count; i++) {
        add_item(items, words[i].label, strlen(words[i].label),
                 kind_of(words[i].kind), NULL);
    }
    free(words);
    return items;
}

cJSON *lsp_completion_members_for_unit(const LhatUnit *unit, uint32_t offset)
{
    if (unit == NULL ||
        lhat_unit_completion_ask(unit, offset, NULL) !=
            LHAT_COMPLETION_MEMBER) {
        return cJSON_CreateArray();
    }
    return items_from_core(unit, offset);
}

cJSON *lsp_completion_for_unit(const LhatUnit *unit, uint32_t offset,
                               const LspUnitExports *others, size_t count)
{
    uint32_t from = 0;
    switch (lhat_unit_completion_ask(unit, offset, &from)) {
        case LHAT_COMPLETION_MEMBER:
            return items_from_core(unit, offset);
        case LHAT_COMPLETION_WORD:
            return lsp_completion_word_items(unit, offset, from, others,
                                             count);
        default:
            // A module path and a require^ string are the host's lists, and
            // the handler answers those before a unit is ever checked.
            return cJSON_CreateArray();
    }
}

bool lsp_completion_import_prefix(const char *text, size_t length,
                                  uint32_t offset, uint32_t *from)
{
    return lhat_completion_ask_text(text, length, offset, from) ==
           LHAT_COMPLETION_MODULE;
}

bool lsp_completion_require_prefix(const char *text, size_t length,
                                   uint32_t offset, uint32_t *from)
{
    return lhat_completion_ask_text(text, length, offset, from) ==
           LHAT_COMPLETION_UNIT;
}

bool lsp_completion_word_prefix(const char *text, size_t length,
                                uint32_t offset, uint32_t *from)
{
    return lhat_completion_ask_text(text, length, offset, from) ==
           LHAT_COMPLETION_WORD;
}

// ---------------------------------------------------------------------------
// What this unit has not taken in yet
// ---------------------------------------------------------------------------

// The start of the line after the one `at` stands on, or the end of the text
// when it stands on the last.
static uint32_t line_after(const char *text, size_t length, uint32_t at)
{
    while (at < length && text[at] != '\n') {
        at++;
    }
    return at < length ? at + 1 : (uint32_t)length;
}

// Where a new import^ or require^ line goes.
//
// 05 の 3 章 puts module^ first and the parser refuses a second one
// (parser.c's MODULE_MISPLACED), so nothing may be written above it -- an
// insertion there would not merely look wrong, it would stop the file
// parsing. And a comment block at the head of a file belongs to whatever
// follows it (01 の 6.4; 07 の 4 章 makes module^'s the unit's own
// description), so inserting into the middle of that hands the block to the
// new line instead.
//
// So: after module^ when there is one, else after whatever comments stand
// above the first statement, else the very top.
static uint32_t import_anchor(const LhatUnit *unit)
{
    const LhatNode *root = unit->parsed.root;
    const LhatNode *first =
        root != NULL && root->kind == LHAT_NODE_BLOCK ? root->v.list.items
                                                      : NULL;
    const char *text = unit->source.text;
    size_t length = unit->source.length;
    if (first != NULL && first->kind == LHAT_NODE_MODULE) {
        // Past the whole line, so a comment written after module^ on it
        // stays with module^.
        return line_after(text, length, first->end);
    }
    uint32_t head = first != NULL ? lhat_node_span_start(first)
                                  : (uint32_t)length;
#if LHAT_WITH_COMMENTS
    // The table rather than the text: a '#[ ]#' runs over lines, and the
    // lines it runs over do not look like comments themselves.
    uint32_t after = 0;
    for (size_t i = 0; i < unit->lexer.comment_count; i++) {
        const LhatComment *c = &unit->lexer.comments[i];
        if (c->end > head) {
            break;
        }
        after = line_after(text, length, c->end);
    }
    return after;
#else
    (void)head;
    return 0;
#endif
}

// The path an import^ statement names, in either spelling -- on its own, and
// bound with a let^. NULL for anything else. (check.c's imported_path says
// the same thing for the checker's own walk.)
static const LhatNode *imported_path(const LhatNode *statement)
{
    if (statement->kind == LHAT_NODE_IMPORT_STMT) {
        return statement->v.jump.value;
    }
    if (statement->kind == LHAT_NODE_DEFINE &&
        statement->v.binding.values != NULL &&
        statement->v.binding.values->kind == LHAT_NODE_IMPORT) {
        return statement->v.binding.values->v.jump.value;
    }
    return NULL;
}

// Whether this unit already reaches `path`. An import^ of a parent reaches
// its children -- 05 の 8.7's registry is one nested table, so 'import^ std'
// puts std.io within reach as well -- which is why this is a prefix test and
// not an equality.
static bool already_imports(const LhatUnit *unit, const char *path,
                            size_t length)
{
    const LhatNode *root = unit->parsed.root;
    if (root == NULL || root->kind != LHAT_NODE_BLOCK) {
        return false;
    }
    for (const LhatNode *s = root->v.list.items; s != NULL; s = s->next) {
        const LhatNode *named = imported_path(s);
        if (named == NULL) {
            continue;
        }
        // Written out as it stands, dots and all -- the same reading
        // document_symbol.c takes of a qualified name.
        uint32_t from = lhat_node_span_start(named);
        if (named->end <= from || named->end > unit->source.length) {
            continue;
        }
        size_t written = named->end - from;
        if (written <= length &&
            memcmp(unit->source.text + from, path, written) == 0 &&
            (written == length || path[written] == '.')) {
            return true;
        }
    }
    return false;
}

static cJSON *text_edit(const LhatUnit *unit, uint32_t from, uint32_t to,
                        const char *replacement)
{
    cJSON *edit = cJSON_CreateObject();
    if (edit != NULL) {
        cJSON_AddItemToObject(edit, "range",
                              lsp_unit_range_json(unit, from, to));
        cJSON_AddStringToObject(edit, "newText", replacement);
    }
    return edit;
}

// One name the writer could reach by taking something in.
//
// The label is the word they typed toward (`print`), because that is what
// they are thinking of; what gets written is the whole path (`std.io.print`),
// because 05 の 5.5 and 8.7 both bind a root and leave the rest as members.
// Neither form invents a name -- the path is the one the module or the unit
// declared for itself.
//
// `statement` is the line to add, or NULL when the unit already reaches this:
// the name is worth offering either way, and only the line is spared.
static void offer_taking_in(cJSON *items, const LhatUnit *unit,
                            uint32_t word_from, uint32_t word_to,
                            const char *path, const char *member,
                            size_t member_length, const LhatType *type,
                            const char *statement)
{
    if (already_offered(items, member, member_length)) {
        return;  // in scope already: that is the one they meant
    }
    // A workspace row carries no type: it is a copy taken behind the
    // workspace lock, and a type belongs to the arena of the root that
    // made it. The path alone is then the whole of what is worth saying.
    char written[LSP_COMPLETION_TYPE_BUFFER];
    written[0] = '\0';
    if (type != NULL) {
        size_t room = lhat_type_write((LhatType *)type, written,
                                      sizeof written);
        if (room > sizeof written - 1) {
            room = strlen(written);
        }
        (void)room;
    }

    char whole[512];
    if ((size_t)snprintf(whole, sizeof whole, "%s.%.*s", path,
                         (int)member_length, member) >= sizeof whole) {
        return;
    }
    char detail[512 + LSP_COMPLETION_TYPE_BUFFER];
    snprintf(detail, sizeof detail, "%s%s%s", whole,
             written[0] != '\0' ? " : " : "", written);

    cJSON *item = add_item(items, member, member_length,
                           type != NULL && type->kind == LHAT_TYPE_TABLE &&
                                   !type->v.table.is_module
                               ? ITEM_CLASS
                           : type != NULL && (type->kind == LHAT_TYPE_FUNC ||
                                              type->kind == LHAT_TYPE_INTERSECT)
                               ? ITEM_FUNCTION
                               : ITEM_VARIABLE,
                           detail);
    if (item == NULL) {
        return;
    }
    // Under everything already in scope. LSP sorts on this and falls back to
    // the label, so one character in front of the label is the whole rule.
    char order[256];
    snprintf(order, sizeof order, "~%.*s", (int)member_length, member);
    cJSON_AddStringToObject(item, "sortText", order);
    cJSON_AddItemToObject(item, "textEdit",
                          text_edit(unit, word_from, word_to, whole));
    if (statement == NULL) {
        return;
    }
    cJSON *extra = cJSON_AddArrayToObject(item, "additionalTextEdits");
    if (extra != NULL) {
        uint32_t at = import_anchor(unit);
        cJSON_AddItemToArray(extra, text_edit(unit, at, at, statement));
    }
}

// 05 の 8.7: every member of every module the host registered, whether or
// not this unit wrote the import^ that would reach it. `hosted` is the one
// nested table import^ resolves against, keyed by path segment, so walking
// it is walking the paths.
static void offer_hosted(cJSON *items, const LhatUnit *unit,
                         uint32_t word_from, uint32_t word_to,
                         const LhatType *owner, const char *path)
{
    if (owner == NULL || owner->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = owner->v.table.members; m != NULL;
         m = m->next) {
        if (m->name == NULL || m->name_length == 0) {
            continue;
        }
        if (m->type != NULL && m->type->kind == LHAT_TYPE_TABLE &&
            m->type->v.table.is_module) {
            // A module of its own, so its members are what may be reached --
            // 'std' holds no name a writer takes, 'std.io' holds print.
            char deeper[512];
            if ((size_t)snprintf(deeper, sizeof deeper, "%s%s%.*s", path,
                                 path[0] != '\0' ? "." : "",
                                 (int)m->name_length,
                                 m->name) < sizeof deeper) {
                offer_hosted(items, unit, word_from, word_to, m->type, deeper);
            }
            continue;
        }
        if (path[0] == '\0') {
            continue;  // 05 の 8.6's own members are reached through L^
        }
        char statement[512];
        const char *line = NULL;
        if (!already_imports(unit, path, strlen(path)) &&
            (size_t)snprintf(statement, sizeof statement, "import^ %s\n",
                             path) < sizeof statement) {
            line = statement;
        }
        offer_taking_in(items, unit, word_from, word_to, path, m->name,
                        m->name_length, m->type, line);
    }
}

// Whether this unit already requires the file `written` names. Unlike an
// import^ there is no parent to subsume it, and unlike an import^ a second
// one is an error (05 の 5.5 through 8.8's last-segment rule), so this has
// to be right rather than merely tidy.
static bool already_requires(const LhatUnit *unit, const char *written)
{
    const LhatNode *root = unit->parsed.root;
    if (root == NULL || root->kind != LHAT_NODE_BLOCK) {
        return false;
    }
    size_t length = strlen(written);
    for (const LhatNode *s = root->v.list.items; s != NULL; s = s->next) {
        const LhatNode *path = NULL;
        if (s->kind == LHAT_NODE_REQUIRE_STMT) {
            path = s->v.jump.value;
        } else if (s->kind == LHAT_NODE_DEFINE &&
                   s->v.binding.values != NULL &&
                   s->v.binding.values->kind == LHAT_NODE_REQUIRE) {
            path = s->v.binding.values->v.jump.value;
        }
        if (path == NULL || path->kind != LHAT_NODE_STRING) {
            continue;
        }
        // The decoded bytes, so an escape written in the path compares by
        // what it means rather than by how it was spelt.
        if (path->v.string.length == length &&
            memcmp(unit->lexer.strings + path->v.string.offset, written,
                   length) == 0) {
            return true;
        }
    }
    return false;
}

// 05 の 5.5: what another unit of this workspace publishes, offered with the
// bare require^ that would reach it. The name is the one that unit declared
// with module^ -- nothing here picks one.
static void offer_workspace(cJSON *items, const LhatUnit *unit,
                            uint32_t word_from, uint32_t word_to,
                            const LspUnitExports *units, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const LspUnitExports *other = &units[i];
        if (other->path == NULL || other->module_name == NULL ||
            (unit->path != NULL && strcmp(other->path, unit->path) == 0)) {
            continue;  // 5.1: a unit does not require^ itself
        }
        // 5.1: the path is written from the unit that writes it.
        char *written = lsp_completion_relative_path(unit->path, other->path);
        if (written == NULL) {
            continue;
        }
        char statement[512];
        const char *line = NULL;
        if (!already_requires(unit, written) &&
            (size_t)snprintf(statement, sizeof statement,
                             "require^ \"%s\"\n", written) < sizeof statement) {
            line = statement;
        }
        for (size_t e = 0; e < other->export_count; e++) {
            const char *name = other->exports[e];
            if (name == NULL) {
                continue;
            }
            // No type in hand: the row is a copy taken behind the workspace
            // lock, and a type belongs to the arena of the root that made it.
            offer_taking_in(items, unit, word_from, word_to,
                            other->module_name, name, strlen(name), NULL,
                            line);
        }
        free(written);
    }
}

cJSON *lsp_completion_word_items(const LhatUnit *unit, uint32_t offset,
                                 uint32_t word_from,
                                 const LspUnitExports *others, size_t count)
{
    // The names in scope and the words of the language, in that order: a
    // name is worth more to the writer than a word they could have typed
    // without asking. Both are the core's answer (lhat/completion.h).
    // With no unit there is nothing in scope to read, and the words are
    // still worth offering: an empty buffer has those and no more.
    cJSON *items = unit != NULL ? items_from_core(unit, offset)
                                : items_of_words();
    if (items == NULL) {
        return NULL;
    }
    // Last, so already_offered has everything in scope and every word to
    // compare against: a name the writer can already reach is the one they
    // meant, and this offers only what is out of reach.
    if (unit != NULL && unit->program != NULL) {
        offer_hosted(items, unit, word_from, offset,
                     unit->program->hosted, "");
    }
    if (unit != NULL && others != NULL) {
        offer_workspace(items, unit, word_from, offset, others, count);
    }
    return items;
}

// ---------------------------------------------------------------------------
// Modules and paths
// ---------------------------------------------------------------------------

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
