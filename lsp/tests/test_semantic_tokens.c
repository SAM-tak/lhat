// L^ (lhat) -- LSP server tests: textDocument/semanticTokens/full
// (semantic_tokens.c).
//
// The first test is the one that matters as the language grows.
// semantic_tokens.c cannot use ast.c's lhat_node_visit_children -- what a
// name means is exactly what its place in the parent says, and the visitor
// hands over every child alike -- so it keeps a switch of its own, and a
// node kind added to ast.h falls into its `default` until someone names it
// there. Everything under such a node then goes uncoloured, quietly:
// try^{ } (04 の 4.5) did precisely that. So this walks the tree through
// the visitor, which needs no teaching, and asks that every name it finds
// came back with a token.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "semantic_tokens.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;
    LhatUnit unit;
} Checked;

// A standalone unit -- lhat_check takes a lexer and a tree directly, so no
// program.h graph is needed.
static void check_text(Checked *c, const char *text)
{
    lhat_source_init_from_string(&c->source, "test.lh", text, strlen(text));
    lhat_lexer_init(&c->lexer, &c->source);
    lhat_parse(&c->lexer, &c->parsed);
    lhat_check(c->parsed.root, &c->lexer, true, &c->checked);

    memset(&c->unit, 0, sizeof c->unit);
    c->unit.path = (char *)"test.lh";
    c->unit.loaded = true;
    c->unit.source = c->source;
    c->unit.lexer = c->lexer;
    c->unit.parsed = c->parsed;
    c->unit.checked = c->checked;
}

static void check_dispose(Checked *c)
{
    lhat_check_result_dispose(&c->checked);
    lhat_parse_result_dispose(&c->parsed);
    lhat_lexer_dispose(&c->lexer);
    lhat_source_dispose(&c->source);
}

// ---------------------------------------------------------------------------
// Reading the delta encoding back
// ---------------------------------------------------------------------------

typedef struct {
    int line;
    int character;
    int length;
    int type;
    int modifiers;
} Token;

typedef struct {
    Token *items;
    size_t count;
} Tokens;

// The spec's data[] is five numbers per token, each position relative to the
// one before it (semantic_tokens.c writes it; this reads it back).
static Tokens decode(cJSON *data)
{
    Tokens out;
    out.count = data != NULL ? (size_t)cJSON_GetArraySize(data) / 5 : 0;
    out.items = out.count > 0 ? (Token *)calloc(out.count, sizeof *out.items)
                              : NULL;
    int line = 0;
    int character = 0;
    for (size_t i = 0; i < out.count && out.items != NULL; i++) {
        int delta_line = cJSON_GetArrayItem(data, (int)(i * 5))->valueint;
        int delta_char = cJSON_GetArrayItem(data, (int)(i * 5 + 1))->valueint;
        line += delta_line;
        character = delta_line == 0 ? character + delta_char : delta_char;
        out.items[i].line = line;
        out.items[i].character = character;
        out.items[i].length = cJSON_GetArrayItem(data, (int)(i * 5 + 2))->valueint;
        out.items[i].type = cJSON_GetArrayItem(data, (int)(i * 5 + 3))->valueint;
        out.items[i].modifiers =
            cJSON_GetArrayItem(data, (int)(i * 5 + 4))->valueint;
    }
    return out;
}

static const char *type_name(int type)
{
    return type >= 0 && (size_t)type < LSP_SEMANTIC_TOKEN_TYPES_COUNT
               ? LSP_SEMANTIC_TOKEN_TYPES[type]
               : "?";
}

// The token covering `text`'s first occurrence of `needle`, by its position
// on the line. NULL when nothing was emitted there.
static const Token *token_for(const Tokens *tokens, const char *text,
                              const char *needle)
{
    const char *found = strstr(text, needle);
    if (found == NULL) {
        return NULL;
    }
    size_t offset = (size_t)(found - text);

    int line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < offset; i++) {
        if (text[i] == '\n') {
            line++;
            line_start = i + 1;
        }
    }
    // The sources here are ASCII, so a column counts the same in bytes as in
    // the UTF-16 units the protocol asks for.
    int character = (int)(offset - line_start);

    for (size_t i = 0; i < tokens->count; i++) {
        if (tokens->items[i].line == line &&
            tokens->items[i].character == character) {
            return &tokens->items[i];
        }
    }
    return NULL;
}

static void expect_token(const Tokens *tokens, const char *text,
                         const char *needle, const char *want_type,
                         bool want_declaration)
{
    const Token *token = token_for(tokens, text, needle);
    LHAT_CHECK(token != NULL, "no token for \"%s\"", needle);
    if (token == NULL) {
        return;
    }
    LHAT_CHECK(strcmp(type_name(token->type), want_type) == 0,
               "\"%s\": got %s, want %s", needle, type_name(token->type),
               want_type);
    bool declared = (token->modifiers & 1u) != 0;
    LHAT_CHECK(declared == want_declaration,
               "\"%s\": declaration modifier %s, want %s", needle,
               declared ? "set" : "unset", want_declaration ? "set" : "unset");
}

// ---------------------------------------------------------------------------
// Every name the tree holds gets a token
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t *offsets;
    size_t count;
    size_t capacity;
} Names;

static void collect_names(void *context, const char *field, bool in_list,
                          const LhatNode *child);

// LHAT_NODE_IDENT is the kind every written name arrives as, wherever it
// stands -- a definition's target, a call's callee, a member key, a segment
// of a qualified path. So the set of them is the set of places a semantic
// token is owed.
static void collect_from(Names *names, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    if (node->kind == LHAT_NODE_IDENT) {
        if (names->count == names->capacity) {
            size_t grown = names->capacity ? names->capacity * 2 : 32;
            uint32_t *bigger =
                (uint32_t *)realloc(names->offsets, grown * sizeof *bigger);
            if (bigger == NULL) {
                return;
            }
            names->offsets = bigger;
            names->capacity = grown;
        }
        names->offsets[names->count++] = node->v.name.offset;
    }
    lhat_node_visit_children(node, collect_names, names);
}

static void collect_names(void *context, const char *field, bool in_list,
                          const LhatNode *child)
{
    (void)field;
    (void)in_list;
    collect_from((Names *)context, child);
}

// Offsets, rather than the line/character the protocol speaks in: this is
// asking whether a name was reached at all, and an offset is what both
// sides already agree on.
static bool has_token_at(const Tokens *tokens, const char *text,
                         uint32_t offset)
{
    int line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < offset; i++) {
        if (text[i] == '\n') {
            line++;
            line_start = i + 1;
        }
    }
    int character = (int)(offset - line_start);
    for (size_t i = 0; i < tokens->count; i++) {
        if (tokens->items[i].line == line &&
            tokens->items[i].character == character) {
            return true;
        }
    }
    return false;
}

static void test_every_name_is_reached(void)
{
    LHAT_TEST("every name in the tree comes back with a token");

    // Written to put a name inside each construct that holds one, so that a
    // kind left out of semantic_tokens.c's switch shows up here as a name
    // with nothing on it. 04 の 4.5's try^{ }, 9.11's next^, 16.3's typed
    // focus and 04 の 14.4's qualified type name are the four this test was
    // written for; the rest is here so the check keeps its reach.
    static const char *source =
        "module^ demo.unit\n"
        "errordef^ E { Bad { why : string^ } }\n"
        "var^ table = { key := 1, other := 2 }\n"
        "let^ shout = f^ msg:string^ -> string^ { return^ msg }\n"
        "let^ pair = (1, 2)\n"
        "for^ index:number^ from^ 0 to^ 4 {\n"
        "    if^ index = 2 { next^ }\n"
        "    shout(\"x\")\n"
        "}\n"
        "for^ key, value in^ table { shout(key) }\n"
        // 04 の 4.5: the arms live inside the try^'s own braces, and a ':'
        // opens each body -- the shape 5.2 gives if^, not a braced block.
        "try^{\n"
        "    shout(\"y\")\n"
        "catch^ E.Bad:\n"
        "    shout(\"z\")\n"
        "catch^:\n"
        "    shout(\"w\")\n"
        "}\n"
        "let^ held : E.Bad = error^ E.Bad { why = \"no\" }\n"
        "let^ widened = table.key as^ number^\n"
        "with^ resource = table { shout(\"v\") }\n";

    Checked c;
    check_text(&c, source);

    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    Names names;
    names.offsets = NULL;
    names.count = 0;
    names.capacity = 0;
    collect_from(&names, c.parsed.root);

    LHAT_CHECK(names.count > 0, "expected the tree to hold names");
    for (size_t i = 0; i < names.count; i++) {
        uint32_t offset = names.offsets[i];
        const char *at = c.source.text + offset;
        size_t length = 0;
        while (at[length] != '\0' && (isalnum((unsigned char)at[length]) ||
                                      at[length] == '_')) {
            length++;
        }
        LHAT_CHECK(has_token_at(&tokens, c.source.text, offset),
                   "no token for the name at offset %u (\"%.*s\")", offset,
                   (int)length, at);
    }

    free(names.offsets);
    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// ---------------------------------------------------------------------------
// What each place says a name means
// ---------------------------------------------------------------------------

static void test_try_block(void)
{
    LHAT_TEST("04 の 4.5: a try^{ } body and its arms");

    static const char *source =
        "errordef^ E { Bad }\n"
        "let^ shout = f^ m:string^ -> string^ { return^ m }\n"
        "try^{\n"
        "    shout(\"body\")\n"
        "catch^ E.Bad:\n"
        "    shout(\"arm\")\n"
        "}\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    // The body and the arm both hold ordinary statements -- what stood
    // uncoloured before try^{ } was a kind of its own.
    expect_token(&tokens, source, "shout(\"body\")", "function", false);
    expect_token(&tokens, source, "shout(\"arm\")", "function", false);
    // 04 の 14.4: the arm's written type is a qualified name, and both
    // segments of it name the kind rather than a table and a member.
    expect_token(&tokens, source, "E.Bad:", "type", false);
    expect_token(&tokens, source, "Bad:", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_for_focus(void)
{
    LHAT_TEST("16.3: a focus takes every shape a let^ target does");

    // 16.3改2: the counted form advances its own focus, so it takes no
    // introducer word -- the range is spelled 'i from^ A to^ B'.
    static const char *source =
        "for^ index:number^ from^ 0 to^ 4 { }\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    // A typed focus is a PARAM, not a bare name: both the name it declares
    // and the type it was written with are owed a token.
    expect_token(&tokens, source, "index:number^", "variable", true);
    expect_token(&tokens, source, "number^ from^", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_qualified_type_name(void)
{
    LHAT_TEST("04 の 14.4: a qualified type name in an annotation");

    static const char *source =
        "errordef^ E { Bad }\n"
        "let^ held : E.Bad = error^ E.Bad { }\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "E.Bad = ", "type", false);
    expect_token(&tokens, source, "Bad = ", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_declaration_and_reference(void)
{
    LHAT_TEST("a definition's name is marked, a use of it is not");

    static const char *source =
        "let^ answer = 42\n"
        "let^ twice = f^ n:number^ -> number^ { return^ n }\n"
        "twice(answer)\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "answer = 42", "variable", true);
    expect_token(&tokens, source, "n:number^", "parameter", true);
    expect_token(&tokens, source, "twice(answer)", "function", false);
    expect_token(&tokens, source, "answer)", "variable", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_compound_assignment_is_one_token(void)
{
    LHAT_TEST("7.4改: a compound assignment names its target once");

    // The parser expands 'want[d] += 1' into the reassignment and the
    // 'want[d] + 1' standing behind it, so the tree carries both spans --
    // but the source says each name once, and 7.4改 reads the target once.
    static const char *source =
        "var^ want = { 1, 2 }\n"
        "var^ d = 1\n"
        "want[d] += 1\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    for (size_t i = 1; i < tokens.count; i++) {
        bool same = tokens.items[i].line == tokens.items[i - 1].line &&
                    tokens.items[i].character == tokens.items[i - 1].character;
        LHAT_CHECK(!same, "two tokens at %d:%d", tokens.items[i].line,
                   tokens.items[i].character);
    }
    expect_token(&tokens, source, "want[d] += 1", "variable", false);
    expect_token(&tokens, source, "d] += 1", "variable", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

int main(void)
{
    test_every_name_is_reached();
    test_compound_assignment_is_one_token();
    test_try_block();
    test_for_focus();
    test_qualified_type_name();
    test_declaration_and_reference();
    return lhat_test_report("test_lsp_semantic_tokens");
}
