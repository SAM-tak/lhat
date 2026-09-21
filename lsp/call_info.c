#include "call_info.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "type.h"
#include "lhat/lexer.h"

static cJSON *type_json(const LhatType *type)
{
    if (type == NULL) return cJSON_CreateString("?");
    size_t size = lhat_type_write_full(type, NULL, 0) + 1;
    char *text = malloc(size);
    if (text == NULL) return NULL;
    lhat_type_write_full(type, text, size);
    cJSON *value = cJSON_CreateString(text);
    free(text);
    return value;
}

static void source_range(cJSON *object, const char *key,
                         const LhatUnit *unit, uint32_t start, uint32_t end)
{
    if (start > end || end > unit->source.length) return;
    size_t length = end - start;
    char *text = malloc(length + 1);
    if (text == NULL) return;
    memcpy(text, unit->source.text + start, length);
    text[length] = '\0';
    cJSON_AddStringToObject(object, key, text);
    free(text);
}

static void source_text(cJSON *object, const char *key,
                        const LhatUnit *unit, const LhatNode *node)
{
    if (node != NULL) source_range(object, key, unit, lhat_node_span_start(node), node->end);
}

// Expression spans need not include grouping parentheses. The parameter owns
// the whole default, from the token after '=' through its final closing ')'.
static void default_text(cJSON *input, const LhatUnit *unit, const LhatNode *param)
{
    if (param->v.param.fallback == NULL || param->v.param.name == NULL) return;
    LhatLexer lexer;
    lhat_lexer_init(&lexer, &unit->source);
    lexer.pos = param->v.param.type == NULL ? param->v.param.name->end : param->v.param.type->end;
    LhatToken token;
    do {
        token = lhat_lexer_next(&lexer);
        if (token.kind == LHAT_TOKEN_OP && token.v.op == LHAT_OP_EQ) {
            token = lhat_lexer_next(&lexer);
            source_range(input, "default", unit, token.offset, param->end);
            break;
        }
    } while (token.kind != LHAT_TOKEN_EOF && token.offset < param->end);
    lhat_lexer_dispose(&lexer);
}

typedef struct {
    uint32_t offset;
    const LhatType *signature;
    const LhatNode *value;
    const LhatNode *literal;
} Declaration;

static void find_declaration(Declaration *found, const LhatNode *node);
static void find_child(void *context, const char *field, bool list,
                        const LhatNode *node)
{
    (void)field;
    (void)list;
    find_declaration(context, node);
}

static void find_declaration(Declaration *found, const LhatNode *node)
{
    if (node->kind == LHAT_NODE_FUNC && node->checked_type == found->signature)
        found->literal = node;
    if (node->kind == LHAT_NODE_DEFINE) {
        const LhatNode *value = node->v.binding.values;
        for (const LhatNode *target = node->v.binding.targets; target != NULL;
             target = target->next) {
            const LhatNode *name = target->kind == LHAT_NODE_PARAM
                                       ? target->v.param.name : target;
            if (name != NULL && name->offset == found->offset) found->value = value;
            if (value != NULL) value = value->next;
        }
    } else if (node->kind == LHAT_NODE_TABLE_ENTRY && node->v.entry.key != NULL &&
               node->v.entry.key->offset == found->offset && !node->v.entry.declared) {
        found->value = node->v.entry.value;
    }
    lhat_node_visit_children(node, find_child, found);
}

// Resolution supplies the owning unit even for an imported member. Aliases
// are followed only through declarations; a parameter has no known value.
static const LhatNode *callee_literal(const LhatUnit **owner, const LhatNode *target,
                                      const LhatType *signature, unsigned depth)
{
    if (target == NULL || depth == 32) return NULL;
    if (target->kind == LHAT_NODE_FUNC) return target;
    bool member = target->kind == LHAT_NODE_MEMBER;
    if (member) target = target->v.access.argument;
    if (target == NULL) return NULL;
    const LhatResolution *resolution = lhat_check_resolution_at(&(*owner)->checked, target->offset);
    if (resolution == NULL || !resolution->has_definition || resolution->is_parameter) return NULL;
    if (!member && !resolution->immutable) return NULL;
    if (resolution->definition_path != NULL && strcmp(resolution->definition_path, (*owner)->path) != 0) {
        const LhatUnit *unit = (*owner)->program == NULL ? NULL : (*owner)->program->units;
        while (unit != NULL && strcmp(unit->path, resolution->definition_path) != 0) unit = unit->next;
        if (unit == NULL) return NULL;
        *owner = unit;
    }
    if ((*owner)->parsed.root == NULL) return NULL;
    Declaration found = { resolution->definition, signature, NULL, NULL };
    find_declaration(&found, (*owner)->parsed.root);
    // An intersection's resolved arm can be a different declaration of the
    // same member. Pointer identity finds that literal without re-resolving.
    if (member && found.literal != NULL) return found.literal;
    if (found.value == target) return NULL;
    return callee_literal(owner, found.value, signature, depth + 1);
}

static bool receiver(const LhatUnit *unit, const LhatNode *param)
{
    const LhatNode *name = param->v.param.name;
    return name != NULL && name->kind == LHAT_NODE_HAT_IDENT &&
           name->v.name.length == 5 &&
           memcmp(unit->source.text + name->v.name.offset, "self^", 5) == 0;
}

static cJSON *input_json(const LhatType *type, const LhatUnit *owner, const LhatNode *param)
{
    cJSON *input = cJSON_CreateObject();
    if (input == NULL) return NULL;
    cJSON_AddItemToObject(input, "type", type_json(type));
    if (param != NULL && param->kind == LHAT_NODE_PARAM) {
        source_text(input, "name", owner, param->v.param.name);
        default_text(input, owner, param);
    }
    return input;
}

cJSON *lsp_call_info(const LhatUnit *unit, const LhatNode *node)
{
    if (unit == NULL || node->kind != LHAT_NODE_CALL || node->v.access.target == NULL) return NULL;
    const LhatNode *target = node->v.access.target;
    const LhatType *signature = target->display_type;
    if (signature != NULL && signature->kind == LHAT_TYPE_INTERSECT) {
        const LhatTypeList *arm = signature->v.composite.arms;
        unsigned index = node->checked_arm;
        if (index == 0) return NULL;
        while (arm != NULL && --index > 0) arm = arm->next;
        signature = arm == NULL ? NULL : arm->type;
    }
    if (signature == NULL || signature->kind != LHAT_TYPE_FUNC) return NULL;
    const LhatUnit *owner = unit;
    const LhatNode *literal = callee_literal(&owner, target, signature, 0);
    const LhatNode *param = literal == NULL ? NULL : literal->v.func.params;
    cJSON *info = cJSON_CreateObject();
    if (info == NULL) return NULL;
    cJSON *inputs = cJSON_AddArrayToObject(info, "inputs");
    cJSON *outputs = cJSON_AddArrayToObject(info, "outputs");
    cJSON_AddItemToObject(info, "signature", type_json(signature));
    if (signature->v.func.takes_self && target->kind != LHAT_NODE_MEMBER &&
        !(target->kind == LHAT_NODE_HAT_IDENT && target->v.name.length == 6 &&
          memcmp(unit->source.text + target->v.name.offset, "super^", 6) == 0)) {
        cJSON *self = input_json(node->v.access.argument == NULL ? NULL : node->v.access.argument->display_type, owner, NULL);
        cJSON_AddStringToObject(self, "name", "self^");
        cJSON_AddItemToArray(inputs, self);
    }
    for (const LhatTypeList *type = signature->v.func.params; type != NULL; type = type->next) {
        while (param != NULL && receiver(owner, param)) param = param->next;
        cJSON_AddItemToArray(inputs, input_json(type->type, owner, param));
        if (param != NULL) param = param->next;
    }
    while (param != NULL && receiver(owner, param)) param = param->next;
    if (signature->v.func.variadic != NULL)
        cJSON_AddItemToObject(info, "variadic", input_json(signature->v.func.variadic, owner, param));
    const LhatType *answer = node->display_type;
    if (answer == NULL) answer = lhat_type_call_answer((LhatType *)signature);
    if (answer != NULL && answer->kind == LHAT_TYPE_TUPLE) {
        for (const LhatTypeList *position = answer->v.composite.arms; position != NULL; position = position->next)
            cJSON_AddItemToArray(outputs, type_json(position->type));
    } else if (answer != NULL && answer->kind != LHAT_TYPE_NONE) {
        cJSON_AddItemToArray(outputs, type_json(answer));
    }
    return info;
}
