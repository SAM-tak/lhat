// Definite initialization of captures at statically known task transfers.
// Uses completed semantic identities, including independently checked bodies.
#include "check_internal.h"
#include "lhat/port.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const LhatNode *node;
    size_t parent;
    bool running;
    bool copying;
} InitNode;

typedef struct {
    uintptr_t key;
    size_t index;
} InitIndex;

typedef struct {
    Checker *checker;
    InitNode *nodes;
    InitIndex *index;
    bool *ready;
    const LhatType **values;
    size_t count, capacity, parent;
    bool transfers, failed;
    const LhatNode *call;
} Initialization;

static void collect(void *context, const char *field, bool in_list, const LhatNode *node)
{
    (void)field;
    (void)in_list;
    Initialization *s = context;
    if (s->failed) return;
    LHAT_GROW(s->nodes, s->count, s->capacity, 64, { s->failed = true; return; });
    size_t at = s->count++;
    s->nodes[at] = (InitNode){node, s->parent, false, false};
    if (node->checked_transfer_arguments != 0) s->transfers = true;
    size_t parent = s->parent;
    s->parent = at;
    lhat_node_visit_children(node, collect, s);
    for (const LhatFunctionInstance *i = node->checked_instances; i != NULL; i = i->next) {
        collect(s, NULL, false, i->body);
    }
    s->parent = parent;
}

static int compare_index(const void *a, const void *b)
{
    uintptr_t left = ((const InitIndex *)a)->key;
    uintptr_t right = ((const InitIndex *)b)->key;
    return left < right ? -1 : left > right ? 1 : 0;
}

static size_t lookup(const Initialization *s, const LhatNode *node)
{
    uintptr_t key = (uintptr_t)node;
    size_t low = 0, high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (s->index[mid].key < key) low = mid + 1;
        else high = mid;
    }
    return low < s->count && s->index[low].key == key ? s->index[low].index : SIZE_MAX;
}

static bool within(const Initialization *s, size_t node, size_t body)
{
    for (; node != SIZE_MAX; node = s->nodes[node].parent) {
        if (node == body) return true;
    }
    return false;
}

static bool declaration_is_active(const Initialization *s, size_t declaration)
{
    for (size_t p = s->nodes[declaration].parent; p != SIZE_MAX; p = s->nodes[p].parent) {
        if (s->nodes[p].node->kind == LHAT_NODE_FUNC) return s->nodes[p].running;
    }
    return true;
}

static void snapshot_type(Initialization *s, const LhatType *type);

static const LhatType *value_type(const Initialization *s, const LhatNode *node)
{
    if (node == NULL) return NULL;
    if (node->checked_binding != NULL && node->checked_binding != node) {
        size_t at = lookup(s, node->checked_binding);
        if (at != SIZE_MAX) return s->values[at];
    }
    return node->checked_callable_type;
}

static void report_capture(Initialization *s, const LhatNode *declaration)
{
    Checker *c = s->checker;
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, declaration, &name, &length)) return;
    for (size_t k = 0; k < c->result->diagnostic_count; k++) {
        const LhatCheckDiagnostic *d = &c->result->diagnostics[k];
        if (d->code == LHAT_CHECK_ERR_UNINITIALIZED_TASK_CAPTURE &&
            d->offset == s->call->offset && d->name == name) return;
    }
    size_t before = c->result->diagnostic_count;
    chk_report_named(c, s->call, LHAT_CHECK_ERR_UNINITIALIZED_TASK_CAPTURE, name, length);
    if (c->result->diagnostic_count == before) return;
    LhatCheckDiagnostic *cause = lhat_type_semantic_alloc(c->result->types, sizeof *cause);
    if (cause == NULL) return;
    cause->code = LHAT_CHECK_ERR_CAPTURE_INITIALIZER;
    cause->offset = declaration->offset;
    cause->line = declaration->line;
    cause->column = declaration->column;
    c->result->diagnostics[before].cause = cause;
    c->result->diagnostics[before].cause_path = c->lexer->source->name;
}

static void snapshot_body(Initialization *s, const LhatNode *body)
{
    size_t owner = lookup(s, body);
    if (owner == SIZE_MAX || s->nodes[owner].copying) return;
    s->nodes[owner].copying = true;
    // Captures of nested closures are also captures of their enclosing body.
    for (size_t k = owner; k < s->count && within(s, k, owner); k++) {
        const LhatNode *use = s->nodes[k].node;
        if (use->checked_import_global || use->checked_binding == NULL ||
            use->checked_binding == use) continue;
        size_t declaration = lookup(s, use->checked_binding);
        if (declaration == SIZE_MAX || within(s, declaration, owner)) continue;
        // A lexical declaration is not an allocation identity for an escaped
        // closure's old activation. Only judge readiness in active frames.
        bool active = declaration_is_active(s, declaration);
        if (active && !s->ready[declaration]) report_capture(s, use->checked_binding);
        else snapshot_type(s, active ? s->values[declaration] : use->checked_callable_type);
    }
    s->nodes[owner].copying = false;
}

static void snapshot_type(Initialization *s, const LhatType *type)
{
    if (type == NULL) return;
    if (type->source_body != NULL) snapshot_body(s, type->source_body);
    if (type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL; arm = arm->next) {
            snapshot_type(s, arm->type);
        }
    }
}

static void execute(Initialization *s, const LhatNode *node);

static void execute_child(void *context, const char *field, bool in_list, const LhatNode *node)
{
    (void)field;
    (void)in_list;
    execute(context, node);
}

static void invoke(Initialization *s, const LhatNode *body, const LhatNode *arguments,
                   bool resume)
{
    size_t at = lookup(s, body);
    if (at == SIZE_MAX || s->nodes[at].running || (body->v.func.yields && !resume)) return;
    bool *saved = lhat_alloc(s->count * sizeof *saved);
    const LhatType **saved_values = lhat_alloc(s->count * sizeof *saved_values);
    if (saved == NULL || saved_values == NULL) {
        lhat_free(saved);
        lhat_free(saved_values);
        return;
    }
    memcpy(saved, s->ready, s->count * sizeof *saved);
    memcpy(saved_values, s->values, s->count * sizeof *saved_values);
    s->nodes[at].running = true;
    s->ready[at] = true;
    for (const LhatNode *p = body->v.func.params; p != NULL; p = p->next) {
        execute(s, p);
        size_t parameter = lookup(s, p->v.param.name);
        if (parameter != SIZE_MAX) s->values[parameter] = value_type(s, arguments);
        if (arguments != NULL) arguments = arguments->next;
    }
    execute(s, body->v.func.body);
    s->nodes[at].running = false;
    memcpy(s->ready, saved, s->count * sizeof *saved);
    memcpy(s->values, saved_values, s->count * sizeof *saved_values);
    lhat_free(saved);
    lhat_free(saved_values);
}

static void execute(Initialization *s, const LhatNode *node)
{
    if (node == NULL || node->kind == LHAT_NODE_FUNC) return;
    if (node->checked_binding == node) {
        size_t at = lookup(s, node);
        if (at != SIZE_MAX) s->ready[at] = true;
    }
    if (node->kind == LHAT_NODE_BLOCK) {
        for (const LhatNode *item = node->v.list.items; item != NULL; item = item->next) {
            execute(s, item);
            if (item->kind == LHAT_NODE_RETURN || item->kind == LHAT_NODE_PANIC ||
                item->kind == LHAT_NODE_BREAK || item->kind == LHAT_NODE_NEXT) break;
        }
        for (const LhatNode *item = node->v.list.arms; item != NULL; item = item->next) execute(s, item);
        for (const LhatNode *item = node->v.list.extra; item != NULL; item = item->next) execute(s, item);
        return;
    }
    if (node->kind == LHAT_NODE_DEFINE) {
        for (const LhatNode *v = node->v.binding.values; v != NULL; v = v->next) execute(s, v);
        const LhatNode *v = node->v.binding.values;
        for (const LhatNode *t = node->v.binding.targets; t != NULL; t = t->next) {
            execute(s, t);
            const LhatNode *name = lhat_define_target_name(t);
            size_t at = lookup(s, name != NULL ? name->checked_binding : NULL);
            if (at != SIZE_MAX) s->values[at] = value_type(s, v);
            if (v != NULL) v = v->next;
        }
        return;
    }
    lhat_node_visit_children(node, execute_child, s);
    if (node->kind != LHAT_NODE_CALL) return;
    const LhatNode *outer = s->call;
    if (s->call == NULL) s->call = node;
    size_t argument = 0;
    for (const LhatNode *a = node->v.access.argument; a != NULL; a = a->next, argument++) {
        if (argument < 64 && (node->checked_transfer_arguments & (UINT64_C(1) << argument))) {
            const LhatType *job = value_type(s, a);
            snapshot_type(s, job);
            if (job != NULL && job->source_body != NULL) {
                invoke(s, job->source_body,
                       a->kind == LHAT_NODE_CALL ? a->v.access.argument : NULL, true);
            }
        }
    }
    const LhatType *callee = value_type(s, node->v.access.target);
    const LhatNode *body = node->checked_instance != NULL ? node->checked_instance->body
                        : node->v.access.target->checked_this_body != NULL ? node->v.access.target->checked_this_body
                        : callee != NULL ? callee->source_body : NULL;
    if (body != NULL) invoke(s, body, node->v.access.argument, false);
    s->call = outer;
}

void chk_check_task_initialization(Checker *c)
{
    Initialization s = {0};
    s.checker = c;
    s.parent = SIZE_MAX;
    collect(&s, NULL, false, c->unit);
    if (!s.failed && s.transfers) {
        s.index = lhat_alloc(s.count * sizeof *s.index);
        s.ready = lhat_calloc(s.count, sizeof *s.ready);
        s.values = lhat_calloc(s.count, sizeof *s.values);
        if (s.index != NULL && s.ready != NULL && s.values != NULL) {
            for (size_t k = 0; k < s.count; k++) {
                s.index[k] = (InitIndex){(uintptr_t)s.nodes[k].node, k};
            }
            qsort(s.index, s.count, sizeof *s.index, compare_index);
            execute(&s, c->unit);
        }
    }
    lhat_free(s.ready);
    lhat_free(s.values);
    lhat_free(s.index);
    lhat_free(s.nodes);
}
