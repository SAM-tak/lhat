#include "graph_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "check.h"
#include "type.h"

#if LHAT_WITH_RESOLUTIONS
typedef struct {
    const LhatUnit *unit;
    uint32_t offset;
    const LhatType *owner;
    const LhatType *actual;
    const LhatNode *annotation;
    bool found;
    bool member;
} Site;

static void find_site(Site *site, const LhatNode *node);
static void find_child(void *context, const char *field, bool list, const LhatNode *node)
{
    (void)field; (void)list;
    find_site(context, node);
}

static void find_site(Site *site, const LhatNode *node)
{
    if (site->found || site->offset < lhat_node_span_start(node) || site->offset >= node->end) return;
    if (node->kind == LHAT_NODE_DEFINE) {
        for (const LhatNode *target = node->v.binding.targets; target; target = target->next) {
            const LhatNode *name = target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
            if (name && site->offset == lhat_node_span_start(name)) {
                site->actual = target->display_type;
                if (!target->next && target == node->v.binding.targets && node->v.binding.values)
                    site->actual = node->v.binding.values->display_type;
                site->annotation = target->kind == LHAT_NODE_PARAM ? target->v.param.type : NULL;
                site->found = true;
                return;
            }
        }
    }
    if (node->kind == LHAT_NODE_TABLE_ENTRY && node->v.entry.key &&
        site->offset == lhat_node_span_start(node->v.entry.key)) {
        site->found = true;
        site->member = true;
        site->annotation = node->v.entry.declared ? node->v.entry.value : node->v.entry.type;
        site->actual = node->v.entry.declared || !node->v.entry.value
                           ? NULL : node->v.entry.value->display_type;
        return;
    }
    const LhatType *outer = site->owner;
    if (node->kind == LHAT_NODE_DEF || node->kind == LHAT_NODE_TABLE)
        site->owner = node->display_type;
    else if (node->kind == LHAT_NODE_SELF_TABLE && outer && outer->kind == LHAT_TYPE_TABLE)
        site->owner = outer->v.table.is_definition ? outer->v.table.instance : outer;
    lhat_node_visit_children(node, find_child, site);
    if (!site->found) site->owner = outer;
}

typedef struct {
    Site *site;
    LhatTypeArena arena;
    LhatType *names;
    cJSON *choices;
} Options;

static void bind_name(void *context, const LhatBindingSite *site)
{
    Options *out = context;
    if (!site->name_length || lhat_type_own_member(out->names, site->name, site->name_length)) return;
    LhatTypeMember *member = lhat_type_add_member(&out->arena, out->names, site->name, site->name_length, site->type);
    if (member) {
        member->names_type = site->names_type;
        member->named_type = site->named_type;
    }
}

static void offer(Options *out, const char *text)
{
    cJSON *item;
    cJSON_ArrayForEach(item, out->choices) if (strcmp(item->valuestring, text) == 0) return;
    LhatType *type = lhat_type_of_text(text, strlen(text), &out->arena, out->names, (LhatType *)out->site->owner);
    if (!type || type->kind == LHAT_TYPE_PENDING || type->kind == LHAT_TYPE_NONE ||
        lhat_type_tuple_width(type) || (out->site->member && lhat_type_hostvalue_arm(type))) return;
    if (out->site->actual && lhat_type_hostvalue_arm(out->site->actual) && !lhat_type_hostvalue_arm(type)) return;
    if (out->site->actual && !lhat_type_conforms_strict(out->site->actual, type)) return;
    cJSON_AddItemToArray(out->choices, cJSON_CreateString(text));
}

// Only namespaces are traversed; ordinary value members are never offered as types.
static void offer_names(Options *out, const LhatType *names, const char *prefix, int depth)
{
    if (!names || names->kind != LHAT_TYPE_TABLE || depth > 8) return;
    for (const LhatTypeMember *m = names->v.table.members; m; m = m->next) {
        char text[1024];
        int length = snprintf(text, sizeof text, "%s%.*s", prefix, (int)m->name_length, m->name);
        if (length < 0 || (size_t)length >= sizeof text - 8) continue;
        if (m->names_type || (m->type && m->type->kind == LHAT_TYPE_TABLE && m->type->v.table.is_definition) ||
            (m->type && ((prefix[0] && m->type->kind == LHAT_TYPE_HOSTVALUE) || m->type->kind == LHAT_TYPE_ENUM ||
                         m->type->kind == LHAT_TYPE_ERROR_SET || m->type->kind == LHAT_TYPE_ERROR_KIND))) {
            offer(out, text);
            strcat(text, ".Box^");
            offer(out, text);
        }
        if (m->type && m->type->kind == LHAT_TYPE_TABLE && m->type->v.table.is_module) {
            text[length] = '.'; text[length + 1] = '\0';
            offer_names(out, m->type, text, depth + 1);
        }
    }
}
#endif

cJSON *lsp_graph_type_options(const LhatUnit *unit, uint32_t offset)
{
#if LHAT_WITH_RESOLUTIONS
    if (!unit || !unit->parsed.root) return NULL;
    Site site = {0}; site.unit = unit; site.offset = offset;
    find_site(&site, unit->parsed.root);
    if (!site.found) return NULL;
    Options out = {0}; out.site = &site;
    lhat_type_arena_init(&out.arena);
    out.names = lhat_type_table(&out.arena);
    out.choices = cJSON_CreateArray();
    lhat_check_bindings_at(&unit->checked, offset, bind_name, &out);
    if (site.annotation) {
        uint32_t start = lhat_node_span_start(site.annotation);
        size_t length = site.annotation->end - start;
        char *written = malloc(length + 1);
        if (written) {
            memcpy(written, unit->source.text + start, length); written[length] = '\0';
            offer(&out, written); free(written);
        }
    }
    if (site.actual) {
        char written[4096];
        if (lhat_type_write_full(site.actual, written, sizeof written) < sizeof written) offer(&out, written);
    }
    static const char *builtins[] = {"number^", "string^", "bool^", "nil^", "any^", "unknown^", "t^{}", "error^", "localerror^"};
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++) offer(&out, builtins[i]);
    offer_names(&out, out.names, "", 0);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "source", unit->source.text);
    cJSON_AddItemToObject(reply, "candidates", out.choices);
    lhat_type_arena_dispose(&out.arena);
    return reply;
#else
    (void)unit; (void)offset; return NULL;
#endif
}
