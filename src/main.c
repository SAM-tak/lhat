// L^ (lhat) -- command line driver.
//
// There is no virtual machine yet, so the only thing the driver can do is run
// the lexer over a file and print the token stream.

#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"
#include "source.h"
#include "token.h"

static void print_token(const LhatLexer *lexer, const LhatToken *token)
{
    printf("%4u:%-3u %-12s", token->line, token->column,
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

static int dump_tokens(const char *path)
{
    LhatSource source;
    char *error = NULL;
    if (!lhat_source_init_from_file(&source, path, &error)) {
        fprintf(stderr, "lhat: %s\n", error != NULL ? error : "cannot read input");
        free(error);
        return EXIT_FAILURE;
    }

    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);

    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        print_token(&lexer, &token);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
    }

    int status = EXIT_SUCCESS;
    for (size_t i = 0; i < lexer.diagnostic_count; i++) {
        const LhatDiagnostic *d = &lexer.diagnostics[i];
        fprintf(stderr, "%s:%u:%u: error: %s\n", source.name, d->line, d->column,
                lhat_error_message(d->code));
        status = EXIT_FAILURE;
    }

    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return status;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("L^ (lhat) %s\n", LHAT_VERSION);
        printf("usage: lhat <file>    dump the token stream of <file>\n");
        return EXIT_SUCCESS;
    }
    return dump_tokens(argv[1]);
}
