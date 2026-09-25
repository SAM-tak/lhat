// L^ (lhat) -- LSP server tests: the syntax tree as JSON (ast_json.c), which
// is what lhat/ast answers with. 06 の 4 章 defines the shape.

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "ast_json.h"
#include "check.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatUnit unit;
    cJSON *json;
} Tree;

// A standalone unit -- no program.h graph is needed, since serialising reads
// only the tree and the source it came from.
static void tree_of(Tree *t, const char *text)
{
    lhat_source_init_from_string(&t->source, "test.lh", text, strlen(text));
    lhat_lexer_init(&t->lexer, &t->source);
    lhat_parse(&t->lexer, &t->parsed);

    memset(&t->unit, 0, sizeof t->unit);
    t->unit.path = (char *)"test.lh";
    t->unit.loaded = true;
    t->unit.source = t->source;
    t->unit.lexer = t->lexer;
    t->unit.parsed = t->parsed;

    t->json = lsp_ast_json_for_unit(&t->unit);
}

static void tree_dispose(Tree *t)
{
    cJSON_Delete(t->json);
    lhat_parse_result_dispose(&t->parsed);
    lhat_lexer_dispose(&t->lexer);
    lhat_source_dispose(&t->source);
}

static cJSON *root_of(const Tree *t)
{
    return cJSON_GetObjectItemCaseSensitive(t->json, "root");
}

static cJSON *field(cJSON *node, const char *name)
{
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(node, "fields");
    return cJSON_GetObjectItemCaseSensitive(fields, name);
}

// The first statement of the unit.
static cJSON *first_statement(const Tree *t)
{
    cJSON *items = field(root_of((Tree *)t), "items");
    return cJSON_GetArrayItem(items, 0);
}

static const char *kind_of(cJSON *node)
{
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(node, "kind");
    return cJSON_IsString(kind) ? kind->valuestring : "<none>";
}

static int start_of(cJSON *node)
{
    return cJSON_GetObjectItemCaseSensitive(node, "start")->valueint;
}

static int end_of(cJSON *node)
{
    return cJSON_GetObjectItemCaseSensitive(node, "end")->valueint;
}

// The source a node's span picks out, which is what the editor cuts its
// labels from. Positions are UTF-16 units and this indexes the bytes C holds,
// so it is only usable where the source is ASCII -- which every case calling
// it is.
static void check_span(const Tree *t, cJSON *node, const char *expected)
{
    if (node == NULL) {
        LHAT_CHECK(false, "no node for span \"%s\"", expected);
        return;
    }
    cJSON *source = cJSON_GetObjectItemCaseSensitive(t->json, "source");
    int start = start_of(node);
    int end = end_of(node);
    LHAT_CHECK_EQ_STR(source->valuestring + start, (size_t)(end - start),
                      expected);
}

static void test_shape(void)
{
    LHAT_TEST("the reply carries the source once and the tree beside it");
    Tree t;
    tree_of(&t, "var^ x = 1\n");
    LHAT_CHECK(t.json != NULL, "no reply");
    cJSON *source = cJSON_GetObjectItemCaseSensitive(t.json, "source");
    LHAT_CHECK(cJSON_IsString(source), "source is not a string");
    LHAT_CHECK_EQ_STR(source->valuestring, strlen(source->valuestring),
                      "var^ x = 1\n");
    LHAT_CHECK_EQ_STR(kind_of(root_of(&t)), strlen(kind_of(root_of(&t))),
                      "block");
    tree_dispose(&t);

    // The root is the whole file, not the stretch from the first statement to
    // the last -- otherwise a comment on the first or last line sits outside
    // the tree with nothing to belong to.
    LHAT_TEST("the root spans the whole unit");
    tree_of(&t, "# lead\nvar^ x = 1\n# tail\n");
    check_span(&t, root_of(&t), "# lead\nvar^ x = 1\n# tail\n");
    tree_dispose(&t);

    // Nothing about how to draw it: the editor decides direction, containers
    // and wrapping (06 の 5 章), so none of that may leak into the reply.
    LHAT_TEST("the reply says nothing about layout");
    tree_of(&t, "if^ x { f() }\n");
    char *printed = cJSON_PrintUnformatted(t.json);
    LHAT_CHECK(printed != NULL, "could not print");
    if (printed != NULL) {
        LHAT_CHECK(strstr(printed, "direction") == NULL, "reply mentions direction");
        LHAT_CHECK(strstr(printed, "container") == NULL, "reply mentions container");
        free(printed);
    }
    tree_dispose(&t);
}

static void test_fields(void)
{
    // ast.h's own member names, so a reader need not know the order each kind
    // writes its children in.
    LHAT_TEST("children are named after the places they came from");
    Tree t;
    tree_of(&t, "var^ x = a + b\n");
    cJSON *define = first_statement(&t);
    LHAT_CHECK_EQ_STR(kind_of(define), strlen(kind_of(define)), "define");

    cJSON *values = field(define, "values");
    LHAT_CHECK(cJSON_IsArray(values), "values is not an array");
    cJSON *sum = cJSON_GetArrayItem(values, 0);
    LHAT_CHECK_EQ_STR(kind_of(sum), strlen(kind_of(sum)), "binary");
    LHAT_CHECK_EQ_STR(kind_of(field(sum, "left")),
                      strlen(kind_of(field(sum, "left"))), "ident");
    LHAT_CHECK_EQ_STR(kind_of(field(sum, "right")),
                      strlen(kind_of(field(sum, "right"))), "ident");
    tree_dispose(&t);

    // A place holding a list is an array even when it holds one, so a reader
    // never has to test which shape it got.
    LHAT_TEST("a list is an array even with one element");
    tree_of(&t, "f(1)\n");
    cJSON *call = field(first_statement(&t), "value");
    LHAT_CHECK_EQ_STR(kind_of(call), strlen(kind_of(call)), "call");
    LHAT_CHECK(cJSON_IsArray(field(call, "argument")),
               "a one-argument call did not give an array");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(field(call, "argument")), 1);
    tree_dispose(&t);

    LHAT_TEST("a leaf carries no fields at all");
    tree_of(&t, "var^ x = 1\n");
    cJSON *one = cJSON_GetArrayItem(field(first_statement(&t), "values"), 0);
    LHAT_CHECK_EQ_STR(kind_of(one), strlen(kind_of(one)), "int");
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(one, "fields") == NULL,
               "a leaf was given an empty fields object");
    tree_dispose(&t);
}

static void test_spans(void)
{
    Tree t;

    // 06 の 4.2: the left edge comes from the subtree, so a call reaches back
    // over its callee even though the node itself begins at the '('.
    LHAT_TEST("a span covers the whole construct");
    tree_of(&t, "let^ v = f(1, 2)\n");
    check_span(&t, field(cJSON_GetArrayItem(field(first_statement(&t), "values"), 0),
                         "target"),
               "f");
    check_span(&t, cJSON_GetArrayItem(field(first_statement(&t), "values"), 0),
               "f(1, 2)");
    tree_dispose(&t);

    LHAT_TEST("a span reaches the token that closes the construct");
    tree_of(&t, "if^ x { f() }\n");
    check_span(&t, first_statement(&t), "if^ x { f() }");
    tree_dispose(&t);

    tree_of(&t, "do^ { f() }\n");
    check_span(&t, first_statement(&t), "do^ { f() }");
    tree_dispose(&t);

    // The editor holds the source as a UTF-16 string, so positions are counted
    // in those units. Byte offsets would slide the moment a file holds a
    // non-ASCII comment -- three bytes but one unit each -- and every label
    // after it would be cut in the wrong place.
    //
    // Checked as numbers rather than through check_span, which indexes the
    // source as the bytes C holds it in.
    LHAT_TEST("positions are UTF-16 units, not bytes");
    tree_of(&t, "# \xE5\x88\x86\xE5\xB2\x90\nvar^ x = 1\n");
    // "# ", two code points, "\n" -- five units, where the bytes are nine.
    LHAT_CHECK_EQ_INT(start_of(first_statement(&t)), 5);
    LHAT_CHECK_EQ_INT(end_of(first_statement(&t)), 15);
    tree_dispose(&t);

    // Beyond the BMP: one code point, but two UTF-16 units.
    tree_of(&t, "var^ x = \"\xF0\x9F\x8D\xA3\"\nvar^ y = 2\n");
    LHAT_CHECK_EQ_INT(start_of(cJSON_GetArrayItem(field(root_of(&t), "items"), 1)),
                      14);
    tree_dispose(&t);
}

#if LHAT_WITH_COMMENTS
static void test_comments(void)
{
    Tree t;

    LHAT_TEST("a node carries the comments written against it");
    tree_of(&t, "# why\nvar^ x = 1  # here\n");
    cJSON *comments =
        cJSON_GetObjectItemCaseSensitive(first_statement(&t), "comments");
    LHAT_CHECK(cJSON_IsArray(comments), "no comments on the statement");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(comments), 2);

    // Spans again, so the editor cuts the text out of the same source string.
    cJSON *source = cJSON_GetObjectItemCaseSensitive(t.json, "source");
    cJSON *first = cJSON_GetArrayItem(comments, 0);
    LHAT_CHECK_EQ_STR(source->valuestring + start_of(first),
                      (size_t)(end_of(first) - start_of(first)), "# why");
    LHAT_CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(first, "block")),
               "a line comment was reported as a block one");
    tree_dispose(&t);

    LHAT_TEST("a node with no comment carries no comments key");
    tree_of(&t, "var^ x = 1\n");
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(first_statement(&t),
                                                "comments") == NULL,
               "an uncommented node was given a comments array");
    tree_dispose(&t);
}
#endif

static void test_absent(void)
{
    LHAT_TEST("a unit with no tree gives nothing");
    LhatUnit empty;
    memset(&empty, 0, sizeof empty);
    LHAT_CHECK(lsp_ast_json_for_unit(&empty) == NULL, "expected no reply");
    LHAT_CHECK(lsp_ast_json_for_unit(NULL) == NULL, "expected no reply for NULL");
}

#if LHAT_WITH_COMMENTS
static cJSON *item(cJSON *array, int index)
{
    return cJSON_GetArrayItem(array, index);
}

static void check_kind(cJSON *node, const char *expected)
{
    LHAT_CHECK_EQ_STR(kind_of(node), strlen(kind_of(node)), expected);
}

// 01 の 6.5: code switched off with '#[~ ... ]#' is listed where it stands
// among statements, holding the statements it would be.
static void test_disabled_code(void)
{
    Tree t;

    LHAT_TEST("disabled code is listed among the statements, holding its own");
    tree_of(&t, "f()\n#[~\ng()\n]#\nh()\n");
    cJSON *items = field(root_of(&t), "items");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 3);
    cJSON *off = item(items, 1);
    check_kind(off, "disabled");
    check_span(&t, off, "#[~\ng()\n]#");
    check_span(&t, item(field(off, "items"), 0), "g()");
    // Listed once: not a comment of the statement below it as well.
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(item(items, 2), "comments") ==
                   NULL,
               "the disabled code was given as a comment too");
    tree_dispose(&t);

    LHAT_TEST("in a body, after the last statement");
    tree_of(&t, "if^ true^ {\n    f()\n    #[~\n    g()\n    ]#\n}\n");
    cJSON *body = field(item(field(first_statement(&t), "items"), 0), "body");
    items = field(body, "items");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(items), 2);
    check_kind(item(items, 1), "disabled");
    check_span(&t, item(field(item(items, 1), "items"), 0), "g()");
    tree_dispose(&t);

    LHAT_TEST("nested, each level lists its own");
    tree_of(&t, "#[~\n#[~\nf()\n]#\n]#\n");
    off = first_statement(&t);
    check_kind(off, "disabled");
    cJSON *inner = item(field(off, "items"), 0);
    check_kind(inner, "disabled");
    check_span(&t, item(field(inner, "items"), 0), "f()");
    tree_dispose(&t);

    LHAT_TEST("a body that does not parse is listed with nothing under it");
    tree_of(&t, "#[~ this is ( not code ]#\n");
    off = first_statement(&t);
    check_kind(off, "disabled");
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(off, "fields") == NULL,
               "an unparsed body listed a tree");
    tree_dispose(&t);

    LHAT_TEST("a block comment without the '~' is still a comment");
    tree_of(&t, "#[ note ]#\nf()\n");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(field(root_of(&t), "items")), 1);
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(first_statement(&t),
                                                "comments") != NULL,
               "the comment went missing");
    tree_dispose(&t);

    LHAT_TEST("away from statements, disabled code stays a comment");
    tree_of(&t, "let^ x = #[~ 1 ]# 2\n");
    char *printed = cJSON_PrintUnformatted(t.json);
    LHAT_CHECK(printed != NULL && strstr(printed, "disabled") == NULL &&
                   strstr(printed, "comments") != NULL,
               "listed code that stands among no statements");
    free(printed);
    tree_dispose(&t);
}
#endif

static void test_callable_info(void)
{
    LHAT_TEST("call slots retain resolved names, defaults, types and multi-value output");
    Tree t;
    tree_of(&t, "let^ f = f^x:number^ = (2 + 3) * 4, y:string^ = \"hi\" -> number^, string^ { return^ x, y }\n"
                "let^ a, b = f(1, \"s\")\n");
    LhatCheckResult checked;
    lhat_check(t.parsed.root, &t.lexer, true, &checked);
    t.unit.checked = checked;
    cJSON_Delete(t.json);
    t.json = lsp_ast_json_for_unit(&t.unit);
    cJSON *binding = cJSON_GetArrayItem(field(root_of(&t), "items"), 1);
    cJSON *call = cJSON_GetArrayItem(field(binding, "values"), 0);
    cJSON *info = cJSON_GetObjectItemCaseSensitive(call, "callable");
    LHAT_CHECK(info != NULL, "no callable metadata");
    cJSON *inputs = cJSON_GetObjectItemCaseSensitive(info, "inputs");
    cJSON *outputs = cJSON_GetObjectItemCaseSensitive(info, "outputs");
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(inputs), 2);
    LHAT_CHECK_EQ_INT(cJSON_GetArraySize(outputs), 2);
    cJSON *first = cJSON_GetArrayItem(inputs, 0);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "name"));
    const char *fallback = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "default"));
    LHAT_CHECK(name != NULL && strcmp(name, "x") == 0, "parameter name was lost");
    LHAT_CHECK(fallback != NULL && strcmp(fallback, "(2 + 3) * 4") == 0, "default grouping was lost");
    const char *output = cJSON_GetStringValue(cJSON_GetArrayItem(outputs, 1));
    LHAT_CHECK(output != NULL && strcmp(output, "string^") == 0, "second output has the wrong type");
    lhat_check_result_dispose(&checked);
    tree_dispose(&t);

    LHAT_TEST("variadic calls and callable parameters do not invent argument names");
    tree_of(&t, "let^ apply = f^fn:f^number^ -> number^; { fn(1) }\n"
                "let^ v = f^...:number^ { return^ 1 }\nlet^ answer = v(1, 2)\n");
    lhat_check(t.parsed.root, &t.lexer, true, &checked);
    t.unit.checked = checked;
    cJSON_Delete(t.json);
    t.json = lsp_ast_json_for_unit(&t.unit);
    cJSON *fn = cJSON_GetArrayItem(field(first_statement(&t), "values"), 0);
    cJSON *ret = cJSON_GetArrayItem(field(field(fn, "body"), "items"), 0);
    call = cJSON_GetArrayItem(field(ret, "value"), 0);
    info = cJSON_GetObjectItemCaseSensitive(call, "callable");
    LHAT_CHECK(info != NULL, "callable parameter signature is missing");
    first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(info, "inputs"), 0);
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(first, "name") == NULL, "invented a parameter name");
    binding = cJSON_GetArrayItem(field(root_of(&t), "items"), 2);
    call = cJSON_GetArrayItem(field(binding, "values"), 0);
    info = cJSON_GetObjectItemCaseSensitive(call, "callable");
    LHAT_CHECK(cJSON_GetObjectItemCaseSensitive(info, "variadic") != NULL, "variadic tail is missing");
    lhat_check_result_dispose(&checked);
    tree_dispose(&t);
}

static void test_callable_origins(void)
{
    const char *sources[] = {
        "let^ original = f^named:number^ = (2 + 3) { named }\nlet^ alias = original\nlet^ out = alias(1)\n",
        "var^ original = f^named:number^ { named }\nlet^ alias = original\nlet^ out = alias(1)\n",
        "let^ Table = def^{ method = f^self^, named:number^ { named } }\nlet^ alias = Table.new()\nlet^ out = alias.method(1)\n",
        "let^ Table = def^{ method = f^named:number^ { named }, overload^method := f^text:string^ { text } }\nlet^ alias = Table\nlet^ out = alias.method(\"a\")\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST("callable origins follow immutable aliases and bound receivers");
        Tree t; tree_of(&t, sources[i]);
        LhatCheckResult checked;
        lhat_check(t.parsed.root, &t.lexer, true, &checked);
        LHAT_CHECK_EQ_INT(t.parsed.diagnostic_count, 0);
        LHAT_CHECK_EQ_INT(checked.diagnostic_count, 0);
        t.unit.checked = checked;
        cJSON_Delete(t.json); t.json = lsp_ast_json_for_unit(&t.unit);
        cJSON *binding = item(field(root_of(&t), "items"), 2);
        cJSON *call = item(field(binding, "values"), 0);
        cJSON *info = cJSON_GetObjectItemCaseSensitive(call, "callable");
        cJSON *inputs = cJSON_GetObjectItemCaseSensitive(info, "inputs");
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(inputs), 1);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item(inputs, 0), "name"));
        if (i == 1) LHAT_CHECK(name == NULL, "mutable alias incorrectly promises the initializer's parameter name");
        else LHAT_CHECK(name != NULL && strcmp(name, i == 3 ? "text" : "named") == 0, "resolved parameter name is missing");
        if (i == 0) {
            const char *fallback = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item(inputs, 0), "default"));
            LHAT_CHECK(fallback != NULL && strcmp(fallback, "(2 + 3)") == 0, "outer parentheses were lost");
        }
        lhat_check_result_dispose(&checked); tree_dispose(&t);
    }
}

static void test_callable_receivers(void)
{
    const struct { const char *source; const char *binding; int inputs; } cases[] = {
        { "let^ values = {1, 2, 3}\nlet^ out = values.slice^(1, 2)\n", "member", 2 },
        { "let^ values = {1, 2, 3}\nlet^ out = values.slice^(1)\n", "member", 1 },
        { "let^ values = {1, 2, 3}\nlet^ out = values.join^(\",\")\n", "member", 1 },
        { "let^ values = {1, 2, 3}\nlet^ out = values.contains^(2)\n", "member", 1 },
        { "let^ values = {1, 2, 3}\nlet^ out = values.clone^()\n", "member", 0 },
        { "let^ values = {1, 2, 3}\nlet^ out = values.keys^()\n", "member", 0 },
        { "let^ text = \"ABC\"\nlet^ out = text.tolower()\n", "member", 0 },
        { "let^ value = 1\nlet^ out = value.tostring()\n", "member", 0 },
        { "let^ Table = def^{ method = f^self^, value:number^ { value } }\n"
          "let^ value = Table.new()\nlet^ out = value.method(1)\n", "member", 1 },
        { "let^ method = f^self^, value:number^ { value }\nlet^ out = method({}, 1)\n", "argument", 2 },
        { "let^ values = { slice = f^a:number^, b:number^ { a + b } }\n"
          "let^ out = values.slice(1, 2)\n", NULL, 2 },
        { "let^ Table = def^{ method = f^value:number^ { value } }\n"
          "let^ out = Table.method(1)\n", NULL, 1 },
        { "let^ plain = f^value:number^ { value }\nlet^ out = plain(1)\n", NULL, 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        LHAT_TEST("callable receivers distinguish bound built-ins, methods and ordinary functions");
        Tree t; tree_of(&t, cases[i].source);
        LhatCheckResult checked;
        lhat_check(t.parsed.root, &t.lexer, true, &checked);
        LHAT_CHECK_EQ_INT(t.parsed.diagnostic_count, 0);
        LHAT_CHECK_EQ_INT(checked.diagnostic_count, 0);
        t.unit.checked = checked;
        cJSON_Delete(t.json); t.json = lsp_ast_json_for_unit(&t.unit);
        cJSON *items = field(root_of(&t), "items");
        cJSON *call = item(field(item(items, cJSON_GetArraySize(items) - 1), "values"), 0);
        cJSON *info = cJSON_GetObjectItemCaseSensitive(call, "callable");
        LHAT_CHECK(info != NULL, "callable metadata is missing");
        LHAT_CHECK_EQ_INT(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(info, "inputs")), cases[i].inputs);
        cJSON *receiver = cJSON_GetObjectItemCaseSensitive(info, "receiver");
        if (cases[i].binding == NULL) {
            LHAT_CHECK(cJSON_IsNull(receiver), "ordinary function gained a receiver");
        } else {
            const char *binding = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(receiver, "binding"));
            LHAT_CHECK(binding != NULL && strcmp(binding, cases[i].binding) == 0, "receiver binding is wrong");
            LHAT_CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(receiver, "type")), "receiver type is missing");
        }
        lhat_check_result_dispose(&checked); tree_dispose(&t);
    }
}

int main(void)
{
    test_shape();
    test_fields();
    test_spans();
#if LHAT_WITH_COMMENTS
    test_comments();
    test_disabled_code();
#endif
    test_absent();
    test_callable_info();
    test_callable_origins();
    test_callable_receivers();
    return lhat_test_report("test_ast_json");
}
