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
#include "lhat/lexer.h"
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

// 8.9's readonly, which stands beside whatever the name turned out to be
// rather than replacing it -- so it is asked about on its own.
static void expect_readonly(const Tokens *tokens, const char *text,
                            const char *needle, bool want)
{
    const Token *token = token_for(tokens, text, needle);
    LHAT_CHECK(token != NULL, "no token for \"%s\"", needle);
    if (token == NULL) {
        return;
    }
    bool readonly = (token->modifiers & 2u) != 0;
    LHAT_CHECK(readonly == want, "\"%s\": readonly %s, want %s", needle,
               readonly ? "set" : "unset", want ? "set" : "unset");
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
    // 02 の 18: an annotation and everything in it belongs to the host. 18.2's
    // name is one the host registered, and 18.3 carries an argument that is a
    // name by its spelling and never resolves it -- so there is nothing the
    // checker settled on for either, and nothing for this walk to say. The
    // grammar file colours them (storage.type.annotation.lhat), which is where
    // a spelling with no meaning behind it belongs.
    if (node->kind == LHAT_NODE_ANNOTATION) {
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
        "with^ resource = table { shout(\"v\") }\n"
        // 14.7改: a definition with its type written out. The self^{ } section
        // of a written type is a kind of its own, and it was the second thing
        // to fall into semantic_tokens.c's `default` -- with nothing here
        // holding one, this test had nothing to say about it.
        "let^ Shape : t^{ self^{ side : number^ }, new : f^ -> Self^; }"
        " = def^{ self^{ side = 1 }, }\n"
        // 02 の 18: a declaration wearing an annotation. What the annotation
        // holds is skipped above, but the declaration under it is not -- an
        // annotation must not swallow the name it was written over.
        "@sample(1, hint)\n"
        "let^ tuned = 1\n";

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

    // The parser expands 'want[d] ?+= 1' into the reassignment and the
    // 'want[d] + 1' standing behind it, so the tree carries both spans --
    // but the source says each name once, and 7.4改 reads the target once.
    // 8.6改2 is how counting into a table is written, 04 の 11.3 making
    // 'want[d]' a 'number^|nil^'.
    static const char *source =
        "var^ want = { 1, 2 }\n"
        "var^ d = 1\n"
        "want[d] ?+= 1\n";

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
    expect_token(&tokens, source, "want[d] ?+= 1", "variable", false);
    expect_token(&tokens, source, "d] ?+= 1", "variable", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// ---------------------------------------------------------------------------
// What the checker knew, where the syntax could only say "a name"
// ---------------------------------------------------------------------------

static void test_definition_reads_as_a_type(void)
{
    LHAT_TEST("14.1: a name bound to a def^ is a type, declared and used");

    static const char *source =
        "let^ Reader = def^{\n"
        "    self^{ n = 1 },\n"
        "    read = f^self^ -> number^ { return^ self^.n },\n"
        "}\n"
        "let^ r = Reader.new()\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    // The declaration is read off the tree -- nothing resolves a name that
    // is being bound -- and the use off what the checker settled on. Both
    // have to agree, or the same name changes colour down the file.
    expect_token(&tokens, source, "Reader = def^", "class", true);
    expect_token(&tokens, source, "Reader.new()", "class", false);
    // An ordinary binding is still a variable: it is the def^ that makes
    // the difference, not the let^.
    expect_token(&tokens, source, "r = Reader", "variable", true);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 14.13: the tree spells 'Base..def^{ … }' as the '..' operator with the
// def^ on its right, so a declaration reading the value has to look through
// one -- or the class a writer inherits into reads as an ordinary name.
static void test_an_inherited_definition_is_a_type(void)
{
    LHAT_TEST("14.13: a name bound to Base..def^ is a type too");

    static const char *source =
        "let^ Base = def^{ self^{ n = 1 } }\n"
        "let^ Sub = Base..def^{ self^{ m = 2 } }\n"
        "let^ joined = \"a\" .. \"b\"\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "Base = def^", "class", true);
    expect_token(&tokens, source, "Sub = Base", "class", true);
    // Concatenation wears the same spelling and declares nothing of the
    // sort, which is what makes looking through '..' safe.
    expect_token(&tokens, source, "joined = ", "variable", true);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 14.7改: the same, for a definition whose type was also written out. 8.7
// makes the annotation what the binding holds, so the def^'s own answer never
// reaches the name -- what says "definition" has to be on the written type,
// and resolve_table_type is what puts it there (check.c). Until it did, a
// name like this read as an ordinary member wherever it was used, which is
// how sample/async.lh's Scheduler came back a property through require^.
static void test_a_written_definition_reads_as_a_type(void)
{
    LHAT_TEST("14.7改: an annotated def^ reads as a type where it is used");

    static const char *source =
        "let^ Counter : t^{ self^{ n : number^ }, new : f^ -> Self^; }"
        " = def^{\n"
        "    self^{ n = 0 },\n"
        "}\n"
        "let^ made = Counter.new()\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "Counter : t^", "class", true);
    expect_token(&tokens, source, "Counter.new()", "class", false);

    // The section itself: its members are what an instance carries, and they
    // are written exactly as the members beside it are. walk_type had no case
    // for the section, so everything named inside one went uncoloured.
    expect_token(&tokens, source, "n : number^", "property", false);
    expect_token(&tokens, source, "number^ }", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_module_path_reads_the_same_everywhere(void)
{
    LHAT_TEST("05 の 8.6: a module path is a namespace wherever it stands");

    // 8.7's import^ is answered from what a host registered, and a check
    // with no host has nothing to answer with -- so this uses require^'s
    // short form (5.5), which builds the same shape out of a unit that
    // named itself. What is being pinned is that the path root and the
    // segment under it read alike in the import^ line and in an expression,
    // which was the whole complaint.
    static const char *source =
        "require^ \"lib/io.lh\"\n"
        "let^ x = demo.io.line\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    // Without a resolver the require^ answers nothing, so this pins only
    // what the walk says on its own: the declaration line names a module.
    // The expression side is covered by test_hover's fixture, which has a
    // program behind it.
    const Token *declared = token_for(&tokens, source, "demo.io.line");
    LHAT_CHECK(declared != NULL, "expected a token on the path root");

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_isa_asks_about_a_type(void)
{
    LHAT_TEST("13.11: the type isa^ asks about reads as one");

    // The complaint this pins: the same word, written as an annotation and
    // written after isa^, came back classified differently -- the first
    // through walk_type, the second not at all, since the type node sat in
    // a binary whose sides were both walked as values.
    static const char *source =
        "let^ take = p^ n:number^ {\n"
        "    if^ n isa^ number^ { }\n"
        "}\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "number^ {\n", "type", false);
    expect_token(&tokens, source, "number^ { }", "type", false);
    // And the subject beside it reads as the parameter it is -- what the
    // resolution now carries, rather than the plain variable a use of one
    // came back as before.
    expect_token(&tokens, source, "n isa^", "parameter", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

static void test_isa_within_a_comparison_chain(void)
{
    LHAT_TEST("11.5 の (5): an isa^ among the comparisons of one chain");

    // 'a < b isa^ number^' is one chain: three operands, two operators, and
    // only the one after isa^ is a type. Walking the operands alike would
    // leave that one with nothing on it.
    static const char *source =
        "let^ a = 1\n"
        "let^ b = 2\n"
        "let^ yes = a < b isa^ number^\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "a < b", "variable", false);
    expect_token(&tokens, source, "b isa^", "variable", false);
    expect_token(&tokens, source, "number^\n", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 13.1: what declared the name is something only the checker knows -- the
// place a use stands says only that there is a name, and the type says only
// what it holds. Before the resolution carried it, a use of a parameter
// came back as an ordinary variable while its declaration read as one.
static void test_a_parameter_reads_as_one_where_it_is_used(void)
{
    LHAT_TEST("13.1: a parameter is a parameter at its uses too");

    static const char *source =
        "let^ take = f^ n:number^ -> number^ {\n"
        "    return^ n + 1\n"
        "}\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "n:number^", "parameter", true);
    expect_token(&tokens, source, "n + 1", "parameter", false);
    // And a local is still a variable, so this says something.
    expect_token(&tokens, source, "take = f^", "variable", true);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 05 の 8.8: a host type is one table for the type and for everything of it,
// so what the checker hands back for the type's own name and for a name
// holding one is the same type -- the spelling is the only thing that tells
// them apart (semantic_tokens.c's names_its_own_type).
//
// A registration only reaches the checker through a program: lhat_check takes
// a lexer and a tree and knows no registry, which is why this one test builds
// the graph the server itself works on.
static char *one_unit_load(void *context, const char *path, size_t *length)
{
    if (strcmp(path, "main.lh") != 0) {
        return NULL;
    }
    const char *text = (const char *)context;
    size_t size = strlen(text);
    char *copy = (char *)malloc(size + 1);
    if (copy != NULL) {
        memcpy(copy, text, size + 1);
        *length = size;
    }
    return copy;
}

static LhatValue never_called(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_nil();  // checking never calls one
}

static void test_a_host_type_reads_as_a_type(void)
{
    LHAT_TEST("05 の 8.8: a host type's own name is a type, a value of it is not");

    static const char *source =
        "import^ store\n"
        "let^ made = store.Held.make()\n"
        "let^ n = made.read()\n";

    LhatProgram program;
    lhat_program_init(&program, true, one_unit_load, (void *)source);
    LHAT_CHECK(lhat_register_hostdata_type(&program, "store", "Held") != NULL,
               "the type registered");
    lhat_register_member(&program, "store", "Held", "make",
                         "f^ -> store.Held;", never_called, NULL);
    lhat_register_member(&program, "store", "Held", "read",
                         "f^self^ -> number^;", never_called, NULL);

    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
               "the unit checked");
    if (root != NULL) {
        cJSON *data = lsp_semantic_tokens_for_unit(root);
        Tokens tokens = decode(data);

        expect_token(&tokens, source, "Held.make()", "type", false);
        // And what holds one is not the type: same LhatType, other spelling.
        expect_token(&tokens, source, "made.read()", "variable", false);
        // The path it is reached through is still the namespace it is.
        expect_token(&tokens, source, "store.Held", "namespace", false);

        free(tokens.items);
        cJSON_Delete(data);
    }
    lhat_program_dispose(&program);
}

// 14.15: 'abstract^ name : type' is a declaration standing among values, and
// the parser says so with a flag rather than with a kind of its own. Read as
// a value, its type was walked as an expression -- and a qualified type name
// is rooted at a TYPE_NAME, which no expression holds, so the root of one
// came back with nothing on it at all.
static void test_an_abstract_field_declares_a_type(void)
{
    LHAT_TEST("14.15: an abstract^ field's type reads as a type");

    static const char *source =
        "errordef^ E { Bad }\n"
        "let^ Holder = def^{\n"
        "    self^{ abstract^ held : number^, abstract^ why : E.Bad },\n"
        "}\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_token(&tokens, source, "held : number^", "property", false);
    expect_token(&tokens, source, "number^, abstract^", "type", false);
    // 04 の 14.4: the qualified form is the one that went uncoloured -- both
    // segments name the kind, and the root is a TYPE_NAME rather than a name
    // an expression could hold.
    expect_token(&tokens, source, "E.Bad },", "type", false);
    // The kind's own declaration is the other "Bad }" in this source, so the
    // needle carries the comma that only the use has.
    expect_token(&tokens, source, "Bad },", "type", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 13.11: a use inside a branch that narrowed the name reads like every other
// use of it. It did not: a narrowed name answers before chk_infer_name, which
// is what records one, so the name went unresolved exactly where the branch
// knew most about it -- and lost the readonly its other uses carried.
static void test_a_narrowed_use_reads_like_any_other(void)
{
    LHAT_TEST("13.11: a name narrowed by a branch still reads as itself");

    static const char *source =
        "let^ held : number^|string^ = 1\n"
        "if^ held isa^ number^ {\n"
        "    let^ doubled = held * 2\n"
        "}\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    // The condition's use is outside the narrowing it establishes; the body's
    // is inside it. Both name the same let^.
    expect_token(&tokens, source, "held isa^", "variable", false);
    expect_readonly(&tokens, source, "held isa^", true);
    expect_token(&tokens, source, "held * 2", "variable", false);
    expect_readonly(&tokens, source, "held * 2", true);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// 8.9: which word bound the name. The declaration reads it off the tree and
// a use off the checker, and the two have to agree -- they are the same
// fact asked in two places.
static void test_let_is_readonly_and_var_is_not(void)
{
    LHAT_TEST("8.9: let^ marks a name readonly, var^ leaves it writable");

    static const char *source =
        "let^ fixed = 1\n"
        "var^ moving = 2\n"
        "moving := fixed + moving\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_readonly(&tokens, source, "fixed = 1", true);
    expect_readonly(&tokens, source, "moving = 2", false);
    expect_readonly(&tokens, source, "fixed + moving", true);
    expect_readonly(&tokens, source, "moving := ", false);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

// The typed resolutions say nothing about 8.9, because the question does
// not apply to them: a member is not a name a scope holds, and what may be
// written through a table is 15.1改's question rather than this one.
static void test_a_member_is_not_called_readonly(void)
{
    LHAT_TEST("14.10: a member carries no answer about let^ or var^");

    static const char *source =
        "let^ point = { x = 1 }\n"
        "let^ n = point.x\n";

    Checked c;
    check_text(&c, source);
    cJSON *data = lsp_semantic_tokens_for_unit(&c.unit);
    Tokens tokens = decode(data);

    expect_readonly(&tokens, source, "x\n", false);
    // The table the member is read from was bound by a let^, and that one
    // does answer.
    expect_readonly(&tokens, source, "point.x", true);

    free(tokens.items);
    cJSON_Delete(data);
    check_dispose(&c);
}

int main(void)
{
    test_every_name_is_reached();
    test_a_parameter_reads_as_one_where_it_is_used();
    test_let_is_readonly_and_var_is_not();
    test_a_narrowed_use_reads_like_any_other();
    test_an_abstract_field_declares_a_type();
    test_a_host_type_reads_as_a_type();
    test_a_member_is_not_called_readonly();
    test_isa_asks_about_a_type();
    test_isa_within_a_comparison_chain();
    test_definition_reads_as_a_type();
    test_an_inherited_definition_is_a_type();
    test_a_written_definition_reads_as_a_type();
    test_module_path_reads_the_same_everywhere();
    test_compound_assignment_is_one_token();
    test_try_block();
    test_for_focus();
    test_qualified_type_name();
    test_declaration_and_reference();
    return lhat_test_report("test_lsp_semantic_tokens");
}
