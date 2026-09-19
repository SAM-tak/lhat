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
    int result_index;
    const LhatType *owner;
    const LhatType *actual;
    const LhatNode *annotation;
    const LhatNode *binding;
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
                site->binding = name;
                site->found = true;
                return;
            }
        }
    }
    if (node->kind == LHAT_NODE_PARAM && node->v.param.name &&
        site->offset == lhat_node_span_start(node->v.param.name)) {
        site->actual = node->display_type;
        site->annotation = node->v.param.type;
        site->binding = node->v.param.name;
        site->found = true;
        return;
    }
    if (node->kind == LHAT_NODE_PARAM && !node->v.param.name && node->v.param.type &&
        site->offset == lhat_node_span_start(node->v.param.type)) {
        site->annotation = node->v.param.type;
        site->found = true;
        return;
    }
    if ((node->kind == LHAT_NODE_FUNC || node->kind == LHAT_NODE_TYPE_FUNC) &&
        (site->offset == lhat_node_span_start(node) ||
         (node->v.func.return_type &&
          site->offset == lhat_node_span_start(node->v.func.return_type)))) {
        const LhatType *func = node->display_type;
        site->actual = func != NULL && func->kind == LHAT_TYPE_FUNC
                           ? lhat_type_call_answer(func) : NULL;
        site->annotation = node->v.func.return_type;
        if (site->result_index >= 0) {
            if (site->actual && lhat_type_tuple_width(site->actual)) {
                if ((size_t)site->result_index >= lhat_type_tuple_width(site->actual)) return;
                site->actual = lhat_type_tuple_at(site->actual, (size_t)site->result_index);
            } else if (site->result_index > 0 && (!site->annotation || site->annotation->kind != LHAT_NODE_TYPE_TUPLE)) return;
            if (site->annotation && site->annotation->kind == LHAT_NODE_TYPE_TUPLE) {
                site->annotation = site->annotation->v.list.items;
                for (int i = 0; i < site->result_index && site->annotation; i++) site->annotation = site->annotation->next;
                if (!site->annotation) return;
            }
        }
        site->found = true;
        return;
    }
    if ((node->kind == LHAT_NODE_TABLE_ENTRY || node->kind == LHAT_NODE_MEMBER_DECL) && node->v.entry.key &&
        site->offset == lhat_node_span_start(node->v.entry.key)) {
        site->found = true;
        site->member = true;
        bool declared = node->v.entry.declared || node->kind == LHAT_NODE_MEMBER_DECL;
        site->annotation = declared ? node->v.entry.value : node->v.entry.type;
        site->actual = declared || !node->v.entry.value
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
    const char *name = NULL;
    size_t length = 0;
    bool own = out->site->binding && lhat_node_name(out->site->binding, out->site->unit->source.text,
        out->site->unit->lexer.strings, &name, &length) &&
        length == site->name_length && memcmp(name, site->name, length) == 0;
    // Keep a shadowing entry, but no type meaning: neither this binding nor
    // a hidden outer namesake may resolve inside its own annotation. Parsing
    // candidates through this scope also rejects nested|nil^ and nested.Box^.
    LhatTypeMember *member = lhat_type_add_member(&out->arena, out->names, site->name, site->name_length, own ? NULL : site->type);
    if (member && !own) {
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
    if (out->site->actual && out->site->actual->kind != LHAT_TYPE_PENDING &&
        out->site->actual->kind != LHAT_TYPE_UNKNOWN &&
        !lhat_type_conforms_strict(out->site->actual, type)) return;
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
        if (m->names_type || (m->type && m->type->kind == LHAT_TYPE_TABLE &&
            (m->type->v.table.is_definition || (prefix[0] && m->type->v.table.hostdata_tag))) ||
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
    return lsp_graph_type_options_result(unit, offset, -1);
}

cJSON *lsp_graph_type_options_result(const LhatUnit *unit, uint32_t offset, int result_index)
{
#if LHAT_WITH_RESOLUTIONS
    if (!unit || !unit->parsed.root) return NULL;
    Site site = {0}; site.unit = unit; site.offset = offset; site.result_index = result_index;
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
    (void)unit; (void)offset; (void)result_index; return NULL;
#endif
}
