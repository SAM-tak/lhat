// L^ (lhat) -- LSP server: the syntax tree as JSON, for lhat/ast.
//
// 06 の 4 章. Structure, positions and comments -- nothing about how any of it
// should be drawn. Which way a construct runs, what is a container and what is
// a leaf, where a list wraps: all of that is the editor's, so that trying a
// different layout does not mean rebuilding the language server.
//
// One case per node kind is what this file does NOT have. ast.c's
// lhat_node_visit_children names each child and says whether its place holds
// a list, which is enough to write the whole tree out generically. What it
// cannot say is which lists hold statements -- where code switched off with
// '#[~ ... ]#' is listed (01 の 6.5) -- and disabled_code.h answers that.

#include "ast_json.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "disabled_code.h"

// lhat records positions as byte offsets into the source. The editor holds
// that source as a JavaScript string, which is indexed in UTF-16 code units,
// and LSP counts the same units everywhere else -- so the tree is written out
// in them too. Anything else silently slides as soon as a file holds a
// non-ASCII comment: one Japanese character is three bytes and one unit.
//
// A table rather than a conversion per position, since the walk does not
// visit offsets in order and each lookup would otherwise rescan the source.
// One entry per byte, with a continuation byte mapping to the position of the
// character it belongs to.
typedef struct {
    uint32_t *units;  // length + 1 entries
    size_t length;
} Utf16Map;

static void utf16_map_dispose(Utf16Map *map)
{
    free(map->units);
    map->units = NULL;
    map->length = 0;
}

static bool utf16_map_build(Utf16Map *map, const char *text, size_t length)
{
    map->units = (uint32_t *)malloc((length + 1) * sizeof *map->units);
    if (map->units == NULL) {
        return false;
    }
    map->length = length;

    uint32_t units = 0;
    size_t i = 0;
    while (i < length) {
        unsigned char lead = (unsigned char)text[i];
        size_t sequence = 1;
        uint32_t width = 1;
        if (lead >= 0xF0) {
            sequence = 4;
            width = 2;  // beyond the BMP: a surrogate pair
        } else if (lead >= 0xE0) {
            sequence = 3;
        } else if (lead >= 0xC0) {
            sequence = 2;
        }
        if (i + sequence > length) {
            sequence = length - i;  // truncated; the lexer already reported it
        }
        for (size_t k = 0; k < sequence; k++) {
            map->units[i + k] = units;
        }
        units += width;
        i += sequence;
    }
    map->units[length] = units;
    return true;
}

static uint32_t utf16_at(const Utf16Map *map, uint32_t byte_offset)
{
    return byte_offset <= map->length ? map->units[byte_offset]
                                      : map->units[map->length];
}

// One text a tree was parsed from: the unit's own, or the body of a
// '#[~ ... ]#' read on its own. The body is parsed with everything around it
// blanked, so its offsets are the unit's too and one table converts them all.
typedef struct {
    const char *text;
    size_t length;
    const Utf16Map *map;
    const LhatType *owner;
#if LHAT_WITH_COMMENTS
    const LhatComment *comments;
    size_t comment_count;
    // Per comment: listed as code among the statements it stands between,
    // rather than as a comment. NULL when the text holds no disabled code --
    // nearly always -- and then nothing below looks.
    bool *listed;
#endif
} Layer;

static bool layer_init(Layer *layer, const LhatSource *source,
                       const LhatLexer *lexer, const Utf16Map *map)
{
    layer->text = source->text;
    layer->length = source->length;
    layer->map = map;
    layer->owner = NULL;
#if LHAT_WITH_COMMENTS
    layer->comments = lexer->comments;
    layer->comment_count = lexer->comment_count;
    layer->listed = NULL;
    for (size_t i = 0; i < lexer->comment_count; i++) {
        if (lhat_comment_is_disabled_code(&lexer->comments[i], source->text,
                                          source->length)) {
            layer->listed =
                (bool *)calloc(lexer->comment_count, sizeof *layer->listed);
            return layer->listed != NULL;
        }
    }
#else
    (void)lexer;
#endif
    return true;
}

static void layer_dispose(Layer *layer)
{
#if LHAT_WITH_COMMENTS
    free(layer->listed);
    layer->listed = NULL;
#else
    (void)layer;
#endif
}

// The tree is deep in proportion to how far expressions nest, so building the
// JSON recursively is bounded by what the parser already accepted.
static cJSON *node_to_json(const LhatNode *node, const Layer *layer);

typedef struct {
    cJSON *fields;
    const Layer *layer;
    bool failed;
#if LHAT_WITH_COMMENTS
    const char *statements;  // where listed code goes; NULL where none can
    size_t next;             // the first comment not yet passed
#endif
} FieldSink;

// A place holding a list is written as an array even when it holds one,
// so a reader never has to test which it got.
static cJSON *array_in(FieldSink *sink, const char *field)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(sink->fields, field);
    if (array == NULL) {
        array = cJSON_CreateArray();
        if (array == NULL) {
            sink->failed = true;
            return NULL;
        }
        cJSON_AddItemToObject(sink->fields, field, array);
    }
    return array;
}

#if LHAT_WITH_COMMENTS
static cJSON *disabled_to_json(const LhatComment *comment, const Layer *layer);

static size_t first_comment_at(const Layer *layer, uint32_t offset)
{
    size_t low = 0;
    size_t high = layer->comment_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (layer->comments[middle].offset < offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// What stands before `limit` and is listed, written among the statements.
static void list_disabled_before(FieldSink *sink, uint32_t limit)
{
    const Layer *layer = sink->layer;
    for (; sink->next < layer->comment_count &&
           layer->comments[sink->next].offset < limit;
         sink->next++) {
        if (!layer->listed[sink->next]) {
            continue;
        }
        cJSON *array = array_in(sink, sink->statements);
        cJSON *json = array != NULL
                          ? disabled_to_json(&layer->comments[sink->next], layer)
                          : NULL;
        if (json == NULL) {
            sink->failed = true;
            return;
        }
        cJSON_AddItemToArray(array, json);
    }
}

// Past what a child covers, whose code is the child's own to list.
static void pass_child(const Layer *layer, size_t *next, const LhatNode *child)
{
    while (*next < layer->comment_count &&
           layer->comments[*next].offset < child->end) {
        (*next)++;
    }
}
#endif

static void add_child(void *context, const char *field, bool in_list,
                      const LhatNode *child)
{
    FieldSink *sink = (FieldSink *)context;
    if (sink->failed) {
        return;
    }
#if LHAT_WITH_COMMENTS
    if (sink->statements != NULL) {
        list_disabled_before(sink, lhat_node_span_start(child));
        if (sink->failed) {
            return;
        }
        pass_child(sink->layer, &sink->next, child);
    }
#endif

    cJSON *json = node_to_json(child, sink->layer);
    if (json == NULL) {
        sink->failed = true;
        return;
    }

    if (!in_list) {
        cJSON_AddItemToObject(sink->fields, field, json);
        return;
    }
    cJSON *array = array_in(sink, field);
    if (array == NULL) {
        cJSON_Delete(json);
        return;
    }
    cJSON_AddItemToArray(array, json);
}

#if LHAT_WITH_COMMENTS
// 01 の 6.5: which disabled code between this node's children stands among
// its statements. Decided before anything of the node is written, because
// the comment is attached (6.4) to this node or to a child of it, and
// whichever holds it must leave it out of its comments.
typedef struct {
    const Layer *layer;
    const char *statements;
    const char *previous;  // the place of the child before; NULL at the first
    size_t next;
} ListedWalk;

static void mark_listed_before(ListedWalk *walk, uint32_t limit)
{
    // Past one of 9 章's clauses, what follows is that clause's -- and the
    // clause ends where its last statement does, so neither can list it in
    // the right place. It stays the comment it lexes as.
    bool among = walk->previous == NULL ||
                 strcmp(walk->previous, walk->statements) == 0;
    const Layer *layer = walk->layer;
    for (; walk->next < layer->comment_count &&
           layer->comments[walk->next].offset < limit;
         walk->next++) {
        if (among && lhat_comment_is_disabled_code(&layer->comments[walk->next],
                                                   layer->text, layer->length)) {
            layer->listed[walk->next] = true;
        }
    }
}

static void mark_child(void *context, const char *field, bool in_list,
                       const LhatNode *child)
{
    (void)in_list;
    ListedWalk *walk = (ListedWalk *)context;
    mark_listed_before(walk, lhat_node_span_start(child));
    pass_child(walk->layer, &walk->next, child);
    walk->previous = field;
}

static const char *mark_listed(const LhatNode *node, const Layer *layer)
{
    const char *statements =
        layer->listed != NULL ? lsp_disabled_code_statements_field(node) : NULL;
    if (statements == NULL) {
        return NULL;
    }
    ListedWalk walk = {layer, statements, NULL,
                       first_comment_at(layer, lhat_node_span_start(node))};
    lhat_node_visit_children(node, mark_child, &walk);
    mark_listed_before(&walk, node->end);
    return statements;
}

// 01 の 6.4. Spans only: the text is a slice of the source, which the reply
// carries once.
static bool add_comments(cJSON *out, const LhatNode *node, const Layer *layer)
{
    cJSON *array = NULL;
    for (const LhatComment *c = node->comments; c != NULL;
         c = c->next_for_node) {
        if (layer->listed != NULL && layer->listed[c - layer->comments]) {
            continue;
        }
        if (array == NULL) {
            array = cJSON_CreateArray();
            if (array == NULL) {
                return false;
            }
            cJSON_AddItemToObject(out, "comments", array);
        }
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            return false;
        }
        cJSON_AddItemToArray(array, item);
        if (cJSON_AddNumberToObject(item, "start",
                                    utf16_at(layer->map, c->offset)) == NULL ||
            cJSON_AddNumberToObject(item, "end", utf16_at(layer->map, c->end)) ==
                NULL ||
            cJSON_AddBoolToObject(item, "block", c->block) == NULL) {
            return false;
        }
    }
    return true;
}
#endif

static cJSON *open_node(const char *kind, uint32_t start, uint32_t end,
                        uint32_t line, uint32_t column, const Utf16Map *map)
{
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(out, "kind", kind) == NULL ||
        cJSON_AddNumberToObject(out, "start", utf16_at(map, start)) == NULL ||
        cJSON_AddNumberToObject(out, "end", utf16_at(map, end)) == NULL ||
        cJSON_AddNumberToObject(out, "line", line) == NULL ||
        cJSON_AddNumberToObject(out, "column", column) == NULL) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

// The comments and children of `node`, written into `out`.
static bool fill_node(cJSON *out, const LhatNode *node, const Layer *layer)
{
#if LHAT_WITH_COMMENTS
    const char *statements = mark_listed(node, layer);
    if (!add_comments(out, node, layer)) {
        return false;
    }
#endif

    cJSON *fields = cJSON_CreateObject();
    if (fields == NULL) {
        return false;
    }
    FieldSink sink = {fields, layer, false};
#if LHAT_WITH_COMMENTS
    sink.statements = statements;
    sink.next = statements != NULL
                    ? first_comment_at(layer, lhat_node_span_start(node))
                    : 0;
#endif
    lhat_node_visit_children(node, add_child, &sink);
#if LHAT_WITH_COMMENTS
    if (statements != NULL && !sink.failed) {
        list_disabled_before(&sink, node->end);
    }
#endif
    if (sink.failed) {
        cJSON_Delete(fields);
        return false;
    }

    // A leaf gets no "fields" at all rather than an empty object.
    if (fields->child == NULL) {
        cJSON_Delete(fields);
    } else {
        cJSON_AddItemToObject(out, "fields", fields);
    }
    return true;
}

static cJSON *node_to_json(const LhatNode *node, const Layer *layer)
{
    // 06 の 4.2: `start` comes from the subtree because an infix or postfix
    // node is written starting at its own operator, while `end` is the node's
    // own -- the token that closes a construct belongs to no child.
    cJSON *out = open_node(lhat_node_kind_name(node->kind),
                           lhat_node_span_start(node), node->end, node->line,
                           node->column, layer->map);
    Layer inner = *layer;
    if (out != NULL && (node->kind == LHAT_NODE_TABLE_ENTRY || node->kind == LHAT_NODE_MEMBER_DECL)) {
        cJSON_AddBoolToObject(out, "declared", node->v.entry.declared || node->kind == LHAT_NODE_MEMBER_DECL);
        cJSON_AddBoolToObject(out, "computed", node->v.entry.computed);
    }
#if LHAT_WITH_RESOLUTIONS
    const LhatType *type = node->display_type;
    if (node->kind == LHAT_NODE_SELF_TABLE && layer->owner != NULL &&
        layer->owner->kind == LHAT_TYPE_TABLE && layer->owner->v.table.is_definition) {
        type = layer->owner->v.table.instance;
    }
    if (node->kind == LHAT_NODE_TABLE_ENTRY && layer->owner != NULL &&
        node->v.entry.key != NULL && !node->v.entry.computed) {
        const LhatNode *key = node->v.entry.key;
        const LhatTypeMember *member = lhat_type_find_member(layer->owner,
            layer->text + key->v.name.offset, key->v.name.length);
        if (member != NULL) type = member->type;
    }
    if (out != NULL && type != NULL) {
        char written[512];
        lhat_type_write(type, written, sizeof written);
        cJSON_AddStringToObject(out, "inferredType", written);
    }
    if (node->kind == LHAT_NODE_TABLE || node->kind == LHAT_NODE_DEF ||
        node->kind == LHAT_NODE_SELF_TABLE) inner.owner = type;
#endif
    if (out != NULL && !fill_node(out, node, &inner)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

#if LHAT_WITH_COMMENTS
// 06 の 4.1: a node of its own kind spanning the markers, listing the
// statements it holds the way a block lists its own.
static cJSON *disabled_to_json(const LhatComment *comment, const Layer *layer)
{
    cJSON *out = open_node("disabled", comment->offset, comment->end,
                           comment->line, comment->column, layer->map);
    LspDisabledCode code;
    if (out == NULL || !lsp_disabled_code_parse(&code, layer->text, comment)) {
        cJSON_Delete(out);
        return NULL;
    }

    // A body that does not parse lists nothing: the editor still has its
    // span, and shows the text rather than a tree nobody can trust.
    bool written = true;
    if (code.lexer.diagnostic_count == 0 && code.parsed.diagnostic_count == 0 &&
        code.parsed.root != NULL) {
        Layer inner;
        written = layer_init(&inner, &code.source, &code.lexer, layer->map) &&
                  fill_node(out, code.parsed.root, &inner);
        layer_dispose(&inner);
    }
    lsp_disabled_code_dispose(&code);
    if (!written) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}
#endif

cJSON *lsp_ast_json_for_unit(const LhatUnit *unit)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return NULL;
    }

    Utf16Map map;
    if (!utf16_map_build(&map, unit->source.text, unit->source.length)) {
        return NULL;
    }
    Layer layer;
    if (!layer_init(&layer, &unit->source, &unit->lexer, &map)) {
        utf16_map_dispose(&map);
        return NULL;
    }

    cJSON *out = cJSON_CreateObject();
    // The source once, rather than a slice of it on every node. Every span in
    // the reply indexes into this, so the editor cuts its own labels out and
    // the reply does not carry the same bytes at every level of the tree.
    cJSON *root = NULL;
    if (out != NULL &&
        cJSON_AddStringToObject(out, "source", unit->source.text) != NULL) {
        root = node_to_json(unit->parsed.root, &layer);
    }
    layer_dispose(&layer);
    utf16_map_dispose(&map);
    if (root == NULL) {
        cJSON_Delete(out);
        return NULL;
    }
    cJSON_AddItemToObject(out, "root", root);
    return out;
}
