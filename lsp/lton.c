// L^ (lhat) -- LSP server: a .lton file, checked as what it is.

#include "lton.h"
#include "program_internal.h"

#include <stdlib.h>
#include <string.h>

bool lsp_lton_is_path(const char *path)
{
    if (path == NULL) {
        return false;
    }
    size_t length = strlen(path);
    size_t extension = strlen(".lton");
    return length > extension &&
           strcmp(path + length - extension, ".lton") == 0;
}


#if LHAT_WITH_RESOLUTIONS
#include "grow.h"
#include "lhat/port.h"
#include "type.h"

typedef struct {
    const LhatNode *alias;
    const LhatNode *import_path;
    const char *module;
    LhatType *type;
    const char *path;
} DataImport;

typedef struct {
    LhatUnit *unit;
    DataImport *imports;
    size_t count;
} DataNames;

static bool data_name(const LhatUnit *unit, const LhatNode *node,
                       const char **text, size_t *length)
{
    return node != NULL && lhat_node_name(node, unit->source.text, unit->lexer.strings, text, length);
}

static bool data_chain(const LhatUnit *unit, const LhatNode *node,
                        const char *path, size_t length)
{
    if (node == NULL) return false;
    if (node->kind == LHAT_NODE_MEMBER) {
        size_t dot = length;
        while (dot > 0 && path[dot - 1] != '.') dot--;
        if (dot == 0 || !data_chain(unit, node->v.access.target, path, dot - 1)) return false;
        path += dot;
        length -= dot;
        node = node->v.access.argument;
    }
    const char *name;
    size_t n;
    return data_name(unit, node, &name, &n) && n == length && memcmp(name, path, n) == 0;
}

static bool data_same_chain(const LhatUnit *unit, const LhatNode *a, const LhatNode *b)
{
    if (a == NULL || b == NULL || a->kind != b->kind) return false;
    if (a->kind == LHAT_NODE_MEMBER)
        return data_same_chain(unit, a->v.access.target, b->v.access.target) &&
               data_same_chain(unit, a->v.access.argument, b->v.access.argument);
    const char *x, *y;
    size_t nx, ny;
    return data_name(unit, a, &x, &nx) && data_name(unit, b, &y, &ny) &&
           nx == ny && memcmp(x, y, nx) == 0;
}

static void data_resolution(DataNames *names, const LhatNode *node, LhatType *type,
                             const char *path, uint32_t definition)
{
    if (node == NULL) return;
    LhatCheckResult *r = &names->unit->checked;
    LHAT_GROW(r->resolutions, r->resolution_count, r->resolution_capacity, 64, return);
    LhatResolution *entry = &r->resolutions[r->resolution_count++];
    memset(entry, 0, sizeof *entry);
    entry->use = node->offset;
    entry->use_end = node->end;
    entry->definition = definition;
    entry->definition_path = path;
    entry->has_definition = path != NULL;
    entry->immutable = true;
    entry->type = type;
}

static LhatType *data_resolve(DataNames *names, const LhatNode *node)
{
    if (node == NULL) return NULL;
    for (size_t i = names->count; i > 0; i--) {
        DataImport *import = &names->imports[i - 1];
        const char *path = import->module;
        size_t length = path != NULL ? strlen(path) : 0;
        if (import->alias != NULL && !data_name(names->unit, import->alias, &path, &length)) continue;
        if (import->import_path != NULL) {
            if (!data_same_chain(names->unit, node, import->import_path)) continue;
        } else if (!data_chain(names->unit, node, path, length)) continue;
        const LhatNode *at = node->kind == LHAT_NODE_MEMBER ? node->v.access.argument : node;
        data_resolution(names, at, import->type,
                        import->alias != NULL ? names->unit->path : import->path,
                        import->alias != NULL ? import->alias->offset : 0);
        return import->type;
    }
    if (node->kind != LHAT_NODE_MEMBER) return NULL;
    LhatType *type = data_resolve(names, node->v.access.target);
    if (type == NULL) return NULL;
    LhatCheckResult *r = &names->unit->checked;
    LHAT_GROW(r->member_sites, r->member_site_count, r->member_site_capacity, 32, return NULL);
    LhatMemberSite *site = &r->member_sites[r->member_site_count++];
    memset(site, 0, sizeof *site);
    site->dot = node->offset;
    site->end = node->end;
    site->receiver = type;
    const char *name;
    size_t length;
    if (!data_name(names->unit, node->v.access.argument, &name, &length)) return NULL;
    if (type->kind == LHAT_TYPE_ENUM) {
        for (LhatTypeList *m = type->v.error.kinds; m != NULL; m = m->next) {
            LhatType *member = m->type;
            if (member->v.error.name_length == length && memcmp(member->v.error.name, name, length) == 0) {
                data_resolution(names, node->v.access.argument, member,
                                member->v.error.declared_in, member->v.error.declared_at);
                return member;
            }
        }
        return NULL;
    }
    const LhatTypeMember *member = lhat_type_find_member(type, name, length);
    if (member == NULL) return NULL;
    data_resolution(names, node->v.access.argument, member->type, member->declared_in, member->declared_at);
    return member->type;
}

static void data_walk(DataNames *names, const LhatNode *node);
static void data_child(void *context, const char *field, bool in_list, const LhatNode *node)
{
    (void)field;
    (void)in_list;
    data_walk(context, node);
}

static void data_walk(DataNames *names, const LhatNode *node)
{
    if (node == NULL) return;
    if (node->kind == LHAT_NODE_MEMBER || node->kind == LHAT_NODE_IDENT) {
        (void)data_resolve(names, node);
        return;
    }
    // Function-local scopes need ordinary semantic analysis; do not guess
    // at shadowed imports inside functions stored as data.
    if (node->kind == LHAT_NODE_FUNC) return;
    if (node->kind == LHAT_NODE_TABLE_ENTRY) {
        if (node->v.entry.computed) data_walk(names, node->v.entry.key);
        data_walk(names, node->v.entry.value);
        return;
    }
    lhat_node_visit_children(node, data_child, names);
}

static LhatType *data_hosted(LhatProgram *program, const LhatUnit *unit, const LhatNode *path)
{
    if (path == NULL) return NULL;
    LhatType *owner = program->hosted;
    const LhatNode *name_node = path;
    if (path->kind == LHAT_NODE_MEMBER) {
        owner = data_hosted(program, unit, path->v.access.target);
        name_node = path->v.access.argument;
    }
    const char *name;
    size_t length;
    if (owner == NULL || !data_name(unit, name_node, &name, &length)) return NULL;
    const LhatTypeMember *member = lhat_type_find_member(owner, name, length);
    return member != NULL ? member->type : NULL;
}

static int data_use_order(const void *a, const void *b)
{
    uint32_t x = ((const LhatResolution *)a)->use, y = ((const LhatResolution *)b)->use;
    return x < y ? -1 : x > y;
}
#endif

void lsp_lton_resolve(LhatProgram *program, LhatUnit *unit)
{
#if LHAT_WITH_RESOLUTIONS
    if (unit->parsed.root == NULL) return;
    const LhatNode *answer = unit->parsed.root->v.list.items;
    if (answer == NULL || answer->kind != LHAT_NODE_RETURN) return;
    const LhatNode *call = answer->v.jump.value;
    if (call == NULL || call->kind != LHAT_NODE_CALL) return;
    const LhatNode *function = call->v.access.target;
    if (function == NULL || function->kind != LHAT_NODE_FUNC || function->v.func.body == NULL) return;
    DataNames names = {unit, NULL, 0};
    for (const LhatNode *s = function->v.func.body->v.list.items; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_RETURN) {
            data_walk(&names, s->v.jump.value);
            break;
        }
        const LhatNode *expr = s->kind == LHAT_NODE_DEFINE ? s->v.binding.values : s;
        if (expr == NULL) continue;
        DataImport entry = {0};
        if (s->kind == LHAT_NODE_DEFINE) entry.alias = s->v.binding.targets;
        if (expr->kind == LHAT_NODE_IMPORT || expr->kind == LHAT_NODE_IMPORT_STMT) {
            entry.type = data_hosted(program, unit, expr->v.jump.value);
            if (entry.alias == NULL) entry.import_path = expr->v.jump.value;
        } else if (expr->kind == LHAT_NODE_REQUIRE || expr->kind == LHAT_NODE_REQUIRE_STMT) {
            const LhatNode *literal = expr->v.jump.value;
            if (literal == NULL || literal->kind != LHAT_NODE_STRING) continue;
            char *resolved = lhat_program_resolve_path(unit, unit->lexer.strings + literal->v.string.offset,
                                                      literal->v.string.length);
            const LhatUnit *other = resolved != NULL ? lhat_program_check(program, resolved) : NULL;
            lhat_free(resolved);
            if (other != NULL && other->state == LHAT_UNIT_DONE) {
                entry.type = other->checked.exports;
                entry.module = other->checked.module_name;
                entry.path = other->path;
                data_resolution(&names, literal, entry.type, other->path, 0);
            }
        }
        if (entry.type == NULL || (entry.alias == NULL && entry.module == NULL && entry.import_path == NULL)) continue;
        DataImport *grown = lhat_realloc(names.imports, (names.count + 1) * sizeof *grown);
        if (grown == NULL) break;
        names.imports = grown;
        names.imports[names.count++] = entry;
    }
    lhat_free(names.imports);
    if (unit->checked.resolution_count > 1)
        qsort(unit->checked.resolutions, unit->checked.resolution_count,
              sizeof *unit->checked.resolutions, data_use_order);
#else
    (void)program;
    (void)unit;
#endif
}
