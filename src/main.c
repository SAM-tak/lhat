// L^ (lhat) -- command line driver.
//
// There is no virtual machine yet, so the driver stops after parsing and
// prints either the token stream or the syntax tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "token.h"

static void print_token(const LhatLexer *lexer, const LhatToken *token)
{
    printf("%4u:%-3u %-18s", token->line, token->column,
           lhat_token_kind_name(token->kind));

    switch (token->kind) {
        case LHAT_TOKEN_IDENT:
            printf(" %.*s", (int)token->length,
                   lexer->source->text + token->offset);
            break;
        case LHAT_TOKEN_HAT_IDENT:
            printf(" %.*s (hats=%u)", (int)token->length,
                   lexer->source->text + token->offset, token->v.hats);
            break;
        case LHAT_TOKEN_INT:
            printf(" %llu (base %u)", (unsigned long long)token->v.integer.value,
                   token->v.integer.base);
            break;
        case LHAT_TOKEN_FLOAT:
            printf(" %g", token->v.real);
            break;
        case LHAT_TOKEN_STRING: {
            size_t length = 0;
            const char *bytes = lhat_lexer_string(lexer, token, &length);
            printf(" %s \"%.*s\"", lhat_string_kind_name(token->v.string.kind),
                   (int)length, bytes != NULL ? bytes : "");
            break;
        }
        case LHAT_TOKEN_NAME_LITERAL:
        case LHAT_TOKEN_INTERP_TEXT:
        case LHAT_TOKEN_INTERP_FORMAT: {
            size_t length = 0;
            const char *bytes = lhat_lexer_string(lexer, token, &length);
            printf(" \"%.*s\"", (int)length, bytes != NULL ? bytes : "");
            break;
        }
        case LHAT_TOKEN_SCOPE:
            printf(" %s (depth=%u)", lhat_scope_kind_name(token->v.scope.kind),
                   token->v.scope.depth);
            break;
        case LHAT_TOKEN_OP:
            printf(" %s", lhat_op_name(token->v.op));
            break;
        default:
            break;
    }

    if (token->preceded_by_newline) {
        printf("   [newline before]");
    }
    printf("\n");
}

static void print_node(const LhatLexer *lexer, const LhatNode *node, int depth);

static void print_list(const LhatLexer *lexer, const char *label,
                       const LhatNode *list, int depth)
{
    if (list == NULL) {
        return;
    }
    printf("%*s%s:\n", depth * 2, "", label);
    for (const LhatNode *item = list; item != NULL; item = item->next) {
        print_node(lexer, item, depth + 1);
    }
}

static void print_node(const LhatLexer *lexer, const LhatNode *node, int depth)
{
    if (node == NULL) {
        return;
    }

    printf("%*s%s", depth * 2, "", lhat_node_kind_name(node->kind));

    switch (node->kind) {
        case LHAT_NODE_INT:
            printf(" %llu", (unsigned long long)node->v.integer.value);
            break;
        case LHAT_NODE_FLOAT:
            printf(" %g", node->v.real);
            break;
        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
        case LHAT_NODE_INTERP_TEXT:
            printf(" \"%.*s\"", (int)node->v.string.length,
                   lexer->strings + node->v.string.offset);
            break;
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_TYPE_NAME:
            printf(" %.*s", (int)node->v.name.length,
                   lexer->source->text + node->v.name.offset);
            break;
        case LHAT_NODE_SCOPE:
            printf(" %s depth=%u", lhat_scope_kind_name(node->v.scope.kind),
                   node->v.scope.depth);
            if (node->v.scope.name != NULL) {
                printf(" %.*s", (int)node->v.scope.name->v.name.length,
                       lexer->source->text + node->v.scope.name->v.name.offset);
            }
            break;
        case LHAT_NODE_UNARY:
            printf(" %s", lhat_op_name(node->v.unary.op));
            break;
        case LHAT_NODE_BINARY:
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            if (node->kind == LHAT_NODE_BINARY) {
                printf(" %s", lhat_op_name(node->v.binary.op));
            }
            break;
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX:
        case LHAT_NODE_CALL:
            if (node->v.access.nil_safe) {
                printf(" (nil-safe)");
            }
            break;
        case LHAT_NODE_FUNC:
        case LHAT_NODE_TYPE_FUNC:
            printf(" %s%s", node->v.func.is_function ? "f^" : "p^",
                   node->v.func.yields ? " yieldable" : "");
            break;
        case LHAT_NODE_PARAM:
            if (node->v.param.variadic) {
                printf(" variadic");
            }
            break;
        case LHAT_NODE_FOR: {
            static const char *const kinds[] = {
                "to^", "downto^", "in^", "while^", "until^", "if^"
            };
            printf(" %s", kinds[node->v.loop.kind]);
            break;
        }
        case LHAT_NODE_REPEAT: {
            static const char *const kinds[] = {
                "forever", "count", "while^", "until^"
            };
            printf(" %s", kinds[node->v.repeat.kind]);
            break;
        }
        case LHAT_NODE_LOOP_CLAUSE: {
            static const char *const kinds[] = {
                "prolog^", "first^", "main^", "last^", "epilog^", "finally^"
            };
            printf(" %s", kinds[node->v.loop_clause.kind]);
            break;
        }
        default:
            break;
    }
    printf("\n");

    switch (node->kind) {
        case LHAT_NODE_UNARY:
            print_node(lexer, node->v.unary.operand, depth + 1);
            break;
        case LHAT_NODE_BINARY:
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            print_node(lexer, node->v.binary.left, depth + 1);
            print_node(lexer, node->v.binary.right, depth + 1);
            break;
        case LHAT_NODE_COMPARE_CHAIN:
            print_list(lexer, "operands", node->v.chain.operands, depth + 1);
            print_list(lexer, "operators", node->v.chain.operators, depth + 1);
            break;
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX:
            print_node(lexer, node->v.access.target, depth + 1);
            print_node(lexer, node->v.access.argument, depth + 1);
            break;
        case LHAT_NODE_CALL:
            print_node(lexer, node->v.access.target, depth + 1);
            print_list(lexer, "args", node->v.access.argument, depth + 1);
            break;
        case LHAT_NODE_AS:
            print_node(lexer, node->v.ascription.value, depth + 1);
            print_node(lexer, node->v.ascription.type, depth + 1);
            break;
        case LHAT_NODE_FUNC:
        case LHAT_NODE_TYPE_FUNC:
            print_list(lexer, "params", node->v.func.params, depth + 1);
            if (node->v.func.return_type != NULL) {
                printf("%*sreturns:\n", (depth + 1) * 2, "");
                print_node(lexer, node->v.func.return_type, depth + 2);
            }
            print_node(lexer, node->v.func.body, depth + 1);
            break;
        case LHAT_NODE_PARAM:
            print_node(lexer, node->v.param.name, depth + 1);
            print_node(lexer, node->v.param.type, depth + 1);
            print_node(lexer, node->v.param.fallback, depth + 1);
            break;
        case LHAT_NODE_TABLE_ENTRY:
        case LHAT_NODE_MEMBER_DECL:
            print_node(lexer, node->v.entry.key, depth + 1);
            print_node(lexer, node->v.entry.value, depth + 1);
            break;
        case LHAT_NODE_DEFINE:
        case LHAT_NODE_REASSIGN:
            print_list(lexer, "targets", node->v.binding.targets, depth + 1);
            print_list(lexer, "values", node->v.binding.values, depth + 1);
            break;
        case LHAT_NODE_IF_CLAUSE:
            print_node(lexer, node->v.clause.condition, depth + 1);
            print_node(lexer, node->v.clause.body, depth + 1);
            break;
        case LHAT_NODE_INTERP_HOLE:
            print_node(lexer, node->v.hole.value, depth + 1);
            print_node(lexer, node->v.hole.format, depth + 1);
            break;
        case LHAT_NODE_RETURN:
        case LHAT_NODE_BREAK:
        case LHAT_NODE_YIELD:
        case LHAT_NODE_CALL_STMT:
        case LHAT_NODE_UNPACK:
            print_node(lexer, node->v.jump.value, depth + 1);
            break;
        case LHAT_NODE_TYPE_CORO:
            print_node(lexer, node->v.coroutine.receive, depth + 1);
            print_node(lexer, node->v.coroutine.produce, depth + 1);
            print_node(lexer, node->v.coroutine.result, depth + 1);
            break;
        case LHAT_NODE_BLOCK:
            print_list(lexer, "items", node->v.list.items, depth + 1);
            print_list(lexer, "clauses", node->v.list.extra, depth + 1);
            break;
        case LHAT_NODE_TABLE:
        case LHAT_NODE_IF_STMT:
        case LHAT_NODE_IF_EXPR:
        case LHAT_NODE_INTERP:
        case LHAT_NODE_TYPE_TABLE:
            print_list(lexer, "items", node->v.list.items, depth + 1);
            break;
        case LHAT_NODE_FOR:
            print_list(lexer, "focus", node->v.loop.focus, depth + 1);
            print_node(lexer, node->v.loop.bound, depth + 1);
            print_node(lexer, node->v.loop.step, depth + 1);
            print_list(lexer, "next", node->v.loop.advance, depth + 1);
            print_node(lexer, node->v.loop.body, depth + 1);
            break;
        case LHAT_NODE_REPEAT:
            print_node(lexer, node->v.repeat.bound, depth + 1);
            print_node(lexer, node->v.repeat.body, depth + 1);
            break;
        case LHAT_NODE_LOOP_CLAUSE:
            print_list(lexer, "body", node->v.loop_clause.body, depth + 1);
            break;
        case LHAT_NODE_WITH:
            print_list(lexer, "bindings", node->v.list.items, depth + 1);
            print_node(lexer, node->v.list.extra, depth + 1);
            break;
        default:
            break;
    }
}

static int report_lexical(const LhatLexer *lexer, const LhatSource *source)
{
    int status = EXIT_SUCCESS;
    for (size_t i = 0; i < lexer->diagnostic_count; i++) {
        const LhatDiagnostic *d = &lexer->diagnostics[i];
        fprintf(stderr, "%s:%u:%u: error: %s\n", source->name, d->line, d->column,
                lhat_error_message(d->code));
        status = EXIT_FAILURE;
    }
    return status;
}

static int dump_tokens(const LhatSource *source)
{
    LhatLexer lexer;
    lhat_lexer_init(&lexer, source);

    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        print_token(&lexer, &token);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
    }

    int status = report_lexical(&lexer, source);
    lhat_lexer_dispose(&lexer);
    return status;
}

static int dump_tree(const LhatSource *source)
{
    LhatLexer lexer;
    lhat_lexer_init(&lexer, source);

    LhatParseResult result;
    lhat_parse(&lexer, &result);

    print_node(&lexer, result.root, 0);

    int status = report_lexical(&lexer, source);
    for (size_t i = 0; i < result.diagnostic_count; i++) {
        const LhatParseDiagnostic *d = &result.diagnostics[i];
        fprintf(stderr, "%s:%u:%u: error: %s\n", source->name, d->line, d->column,
                lhat_parse_error_message(d->code));
        status = EXIT_FAILURE;
    }
    if (result.incomplete) {
        fprintf(stderr, "%s: note: the input ended before the construct was "
                        "complete\n", source->name);
    }

    lhat_parse_result_dispose(&result);
    lhat_lexer_dispose(&lexer);
    return status;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool tokens_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            tokens_only = true;
        } else {
            path = argv[i];
        }
    }

    if (path == NULL) {
        printf("L^ (lhat) %s\n", LHAT_VERSION);
        printf("usage: lhat [--tokens] <file>\n");
        printf("  default    print the syntax tree\n");
        printf("  --tokens   print the token stream instead\n");
        return EXIT_SUCCESS;
    }

    LhatSource source;
    char *error = NULL;
    if (!lhat_source_init_from_file(&source, path, &error)) {
        fprintf(stderr, "lhat: %s\n", error != NULL ? error : "cannot read input");
        free(error);
        return EXIT_FAILURE;
    }

    int status = tokens_only ? dump_tokens(&source) : dump_tree(&source);
    lhat_source_dispose(&source);
    return status;
}
