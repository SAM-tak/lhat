// L^ (lhat) -- LSP server: the syntax tree as JSON, for lhat/ast.
//
// 06 の 4 章. Structure, positions and comments -- nothing about how any of it
// should be drawn. Which way a construct runs, what is a container and what is
// a leaf, where a list wraps: all of that is the editor's, so that trying a
// different layout does not mean rebuilding the language server.
//
// One case per node kind is what this file does NOT have. ast.c's
// lhat_node_visit_children names each child and says whether its place holds
// a list, which is enough to write the whole tree out generically.

#include "ast_json.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"

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

// The tree is deep in proportion to how far expressions nest, so building the
// JSON recursively is bounded by what the parser already accepted.
static cJSON *node_to_json(const LhatNode *node, const Utf16Map *map);

typedef struct {
    cJSON *fields;
    const Utf16Map *map;
    bool failed;
} FieldSink;

static void add_child(void *context, const char *field, bool in_list,
                      const LhatNode *child)
{
    FieldSink *sink = (FieldSink *)context;
    if (sink->failed) {
        return;
    }

    cJSON *json = node_to_json(child, sink->map);
    if (json == NULL) {
        sink->failed = true;
        return;
    }

    if (!in_list) {
        cJSON_AddItemToObject(sink->fields, field, json);
        return;
    }

    // A place holding a list is written as an array even when it holds one,
    // so a reader never has to test which it got.
    cJSON *array = cJSON_GetObjectItemCaseSensitive(sink->fields, field);
    if (array == NULL) {
        array = cJSON_CreateArray();
        if (array == NULL) {
            cJSON_Delete(json);
            sink->failed = true;
            return;
        }
        cJSON_AddItemToObject(sink->fields, field, array);
    }
    cJSON_AddItemToArray(array, json);
}

#ifdef LHAT_WITH_COMMENTS
// 01 の 6.4. Spans only: the text is a slice of the source, which the reply
// carries once.
static bool add_comments(cJSON *out, const LhatNode *node, const Utf16Map *map)
{
    if (node->comments == NULL) {
        return true;
    }
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return false;
    }
    cJSON_AddItemToObject(out, "comments", array);

    for (const LhatComment *c = node->comments; c != NULL;
         c = c->next_for_node) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            return false;
        }
        cJSON_AddItemToArray(array, item);
        if (cJSON_AddNumberToObject(item, "start", utf16_at(map, c->offset)) ==
                NULL ||
            cJSON_AddNumberToObject(item, "end", utf16_at(map, c->end)) == NULL ||
            cJSON_AddBoolToObject(item, "block", c->block) == NULL) {
            return false;
        }
    }
    return true;
}
#endif

static cJSON *node_to_json(const LhatNode *node, const Utf16Map *map)
{
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        return NULL;
    }

    // 06 の 4.2: `start` comes from the subtree because an infix or postfix
    // node is written starting at its own operator, while `end` is the node's
    // own -- the token that closes a construct belongs to no child.
    if (cJSON_AddStringToObject(out, "kind",
                                lhat_node_kind_name(node->kind)) == NULL ||
        cJSON_AddNumberToObject(
            out, "start", utf16_at(map, lhat_node_span_start(node))) == NULL ||
        cJSON_AddNumberToObject(out, "end", utf16_at(map, node->end)) == NULL ||
        cJSON_AddNumberToObject(out, "line", node->line) == NULL ||
        cJSON_AddNumberToObject(out, "column", node->column) == NULL) {
        cJSON_Delete(out);
        return NULL;
    }

#ifdef LHAT_WITH_COMMENTS
    if (!add_comments(out, node, map)) {
        cJSON_Delete(out);
        return NULL;
    }
#endif

    cJSON *fields = cJSON_CreateObject();
    if (fields == NULL) {
        cJSON_Delete(out);
        return NULL;
    }
    FieldSink sink = {fields, map, false};
    lhat_node_visit_children(node, add_child, &sink);
    if (sink.failed) {
        cJSON_Delete(fields);
        cJSON_Delete(out);
        return NULL;
    }

    // A leaf gets no "fields" at all rather than an empty object.
    if (fields->child == NULL) {
        cJSON_Delete(fields);
    } else {
        cJSON_AddItemToObject(out, "fields", fields);
    }
    return out;
}

cJSON *lsp_ast_json_for_unit(const LhatUnit *unit)
{
    if (unit == NULL || unit->parsed.root == NULL) {
        return NULL;
    }

    Utf16Map map;
    if (!utf16_map_build(&map, unit->source.text, unit->source.length)) {
        return NULL;
    }

    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        utf16_map_dispose(&map);
        return NULL;
    }

    // The source once, rather than a slice of it on every node. Every span in
    // the reply indexes into this, so the editor cuts its own labels out and
    // the reply does not carry the same bytes at every level of the tree.
    if (cJSON_AddStringToObject(out, "source", unit->source.text) == NULL) {
        cJSON_Delete(out);
        utf16_map_dispose(&map);
        return NULL;
    }

    cJSON *root = node_to_json(unit->parsed.root, &map);
    utf16_map_dispose(&map);
    if (root == NULL) {
        cJSON_Delete(out);
        return NULL;
    }
    cJSON_AddItemToObject(out, "root", root);
    return out;
}
