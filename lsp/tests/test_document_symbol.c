// L^ (lhat) -- LSP server tests: the outline of a unit (document_symbol.c),
// which is what textDocument/documentSymbol answers with.
//
// Everything here is read off the tree, so the unit is parsed and never
// checked -- an outline is owed to a file that will not check, too.

#include <stdlib.h>
#include <string.h>

#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/source.h"

#include "document_symbol.h"
#include "lton.h"
#include "testutil.h"

typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatUnit unit;
    cJSON *symbols;
} Outline;

static void outline_of_path(Outline *o, const char *path, const char *text,
                            size_t length)
{
    lhat_source_init_from_string(&o->source, path, text, length);
    lhat_lexer_init(&o->lexer, &o->source);
    lhat_parse(&o->lexer, &o->parsed);

    memset(&o->unit, 0, sizeof o->unit);
    o->unit.path = (char *)path;
    o->unit.loaded = true;
    o->unit.source = o->source;
    o->unit.lexer = o->lexer;
    o->unit.parsed = o->parsed;

    o->symbols = lsp_document_symbols_for_unit(&o->unit);
}

static void outline_of(Outline *o, const char *text)
{
    outline_of_path(o, "test.lh", text, strlen(text));
}

static void outline_dispose(Outline *o)
{
    cJSON_Delete(o->symbols);
    lhat_parse_result_dispose(&o->parsed);
    lhat_lexer_dispose(&o->lexer);
    lhat_source_dispose(&o->source);
}

static const char *name_of(const cJSON *symbol)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(symbol, "name");
    return cJSON_IsString(name) ? name->valuestring : "";
}

static const char *detail_of(const cJSON *symbol)
{
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(symbol, "detail");
    return cJSON_IsString(detail) ? detail->valuestring : "";
}

static int kind_of(const cJSON *symbol)
{
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(symbol, "kind");
    return cJSON_IsNumber(kind) ? kind->valueint : -1;
}

static cJSON *children_of(const cJSON *symbol)
{
    return cJSON_GetObjectItemCaseSensitive(symbol, "children");
}

static int count_of(const cJSON *array)
{
    return array != NULL ? cJSON_GetArraySize(array) : 0;
}

// The symbol at `index`, checked to be named `name` and of `kind`. NULL when
// it is not there at all, so a caller can stop rather than read past it.
static cJSON *expect(const cJSON *array, int index, const char *name, int kind)
{
    cJSON *symbol = cJSON_GetArrayItem(array, index);
    LHAT_CHECK(symbol != NULL, "expected a symbol at %d named %s", index, name);
    if (symbol == NULL) {
        return NULL;
    }
    LHAT_CHECK(strcmp(name_of(symbol), name) == 0, "expected %s, got %s", name,
               name_of(symbol));
    LHAT_CHECK(kind_of(symbol) == kind, "%s: expected kind %d, got %d", name,
               kind, kind_of(symbol));
    return symbol;
}

static int position_field(const cJSON *symbol, const char *range,
                          const char *edge, const char *field)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(symbol, range);
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, edge);
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(e, field);
    return cJSON_IsNumber(f) ? f->valueint : -1;
}

// LSP's SymbolKind numbers, the ones the outline uses.
enum {
    MODULE = 2,
    CLASS = 5,
    METHOD = 6,
    PROPERTY = 7,
    FIELD = 8,
    ENUM = 10,
    FUNCTION = 12,
    VARIABLE = 13,
    CONSTANT = 14,
    OBJECT = 19,
    ENUM_MEMBER = 22
};

static void test_bindings(void)
{
    Outline o;

    LHAT_TEST("8 章: let^ is a constant, var^ a variable, and a function a function");
    outline_of(&o,
               "let^ answer = 42\n"
               "var^ count = 0\n"
               "let^ twice = f^ n:number^ -> number^ {\n"
               "    let^ doubled = n * 2\n"
               "    return^ doubled\n"
               "}\n");
    LHAT_CHECK(count_of(o.symbols) == 3, "expected 3 symbols, got %d",
               count_of(o.symbols));
    expect(o.symbols, 0, "answer", CONSTANT);
    expect(o.symbols, 1, "count", VARIABLE);
    cJSON *twice = expect(o.symbols, 2, "twice", FUNCTION);
    if (twice != NULL) {
        // The signature as written, up to the body.
        LHAT_CHECK(strcmp(detail_of(twice), "f^ n:number^ -> number^") == 0,
                   "detail was \"%s\"", detail_of(twice));
        // Its locals under it.
        expect(children_of(twice), 0, "doubled", CONSTANT);
        // The range is the whole statement; the selection is the name.
        LHAT_CHECK(position_field(twice, "range", "start", "line") == 2 &&
                       position_field(twice, "range", "end", "line") == 5,
                   "range should cover lines 2-5");
        LHAT_CHECK(position_field(twice, "selectionRange", "start", "character") == 5 &&
                       position_field(twice, "selectionRange", "end", "character") == 10,
                   "selection should be the name");
    }
    outline_dispose(&o);

    LHAT_TEST("13.10: several targets pair with several values");
    outline_of(&o, "let^ a, b = 1, f^ { }\n");
    expect(o.symbols, 0, "a", CONSTANT);
    expect(o.symbols, 1, "b", FUNCTION);
    outline_dispose(&o);

    LHAT_TEST("8.8: a path target is the member it adds");
    outline_of(&o,
               "let^ m = {}\n"
               "let^ m.helper = f^ { }\n");
    expect(o.symbols, 0, "m", OBJECT);
    expect(o.symbols, 1, "m.helper", FUNCTION);
    outline_dispose(&o);

    LHAT_TEST("05 の 5 章, 8.7: what a require^ or import^ binds is a module");
    outline_of(&o,
               "let^ util = require^ \"lib/util.lh\"\n"
               "let^ io = import^ std.io\n");
    cJSON *util = expect(o.symbols, 0, "util", MODULE);
    if (util != NULL) {
        LHAT_CHECK(strcmp(detail_of(util), "require^ \"lib/util.lh\"") == 0,
                   "detail was \"%s\"", detail_of(util));
    }
    expect(o.symbols, 1, "io", MODULE);
    outline_dispose(&o);
}

static void test_a_class(void)
{
    Outline o;

    LHAT_TEST("14 章: a def^ is a class of fields, methods and properties");
    outline_of(&o,
               "let^ Reader = def^{\n"
               "    self^{\n"
               "        text = \"\",\n"
               "        at = 1,\n"
               "    },\n"
               "    limit = 10,\n"
               "    override^new = f^line { self^{ text = line } },\n"
               "    peek = p^self^ -> string^ { self^.text.at(self^.at) },\n"
               "    abstract^ close : p^self^;,\n"
               "}\n");
    cJSON *reader = expect(o.symbols, 0, "Reader", CLASS);
    if (reader == NULL) {
        outline_dispose(&o);
        return;
    }
    cJSON *members = children_of(reader);
    LHAT_CHECK(count_of(members) == 6, "expected 6 members, got %d",
               count_of(members));
    expect(members, 0, "text", FIELD);
    expect(members, 1, "at", FIELD);
    expect(members, 2, "limit", PROPERTY);
    expect(members, 3, "new", METHOD);
    cJSON *peek = expect(members, 4, "peek", METHOD);
    if (peek != NULL) {
        LHAT_CHECK(strcmp(detail_of(peek), "p^self^ -> string^") == 0,
                   "detail was \"%s\"", detail_of(peek));
    }
    // 14.15: declared and given nothing, still a member -- typed as written.
    cJSON *close = expect(members, 5, "close", METHOD);
    if (close != NULL) {
        LHAT_CHECK(strcmp(detail_of(close), "p^self^;") == 0,
                   "detail was \"%s\"", detail_of(close));
    }
    outline_dispose(&o);

    LHAT_TEST("14.13: 'Base..def^' is a class too, and says what it was made from");
    outline_of(&o,
               "let^ Base = def^{ self^{ n = 0 } }\n"
               "let^ Derived = Base..def^{ more = f^self^ { } }\n");
    cJSON *derived = expect(o.symbols, 1, "Derived", CLASS);
    if (derived != NULL) {
        LHAT_CHECK(strcmp(detail_of(derived), "Base") == 0, "detail was \"%s\"",
                   detail_of(derived));
        expect(children_of(derived), 0, "more", METHOD);
    }
    outline_dispose(&o);

    LHAT_TEST("a table literal lists its named entries, and only those");
    outline_of(&o,
               "let^ point = { x = 1, y = 2, 3, tostring^ = f^self^ { \"p\" } }\n");
    cJSON *point = expect(o.symbols, 0, "point", OBJECT);
    if (point != NULL) {
        cJSON *entries = children_of(point);
        LHAT_CHECK(count_of(entries) == 3, "expected 3 entries, got %d",
                   count_of(entries));
        expect(entries, 0, "x", PROPERTY);
        expect(entries, 1, "y", PROPERTY);
        // 01 の 2.3: the hat is part of the name.
        expect(entries, 2, "tostring^", FUNCTION);
    }
    outline_dispose(&o);
}

static void test_errors_and_modules(void)
{
    Outline o;

    LHAT_TEST("04 の 2.2: an errordef^ is an enum of kinds, each with its fields");
    outline_of(&o,
               "errordef^ Parse {\n"
               "    Unexpected { at : number^ },\n"
               "    Spent,\n"
               "}\n");
    cJSON *parse = expect(o.symbols, 0, "Parse", ENUM);
    if (parse != NULL) {
        cJSON *kinds = children_of(parse);
        cJSON *unexpected = expect(kinds, 0, "Unexpected", ENUM_MEMBER);
        expect(kinds, 1, "Spent", ENUM_MEMBER);
        if (unexpected != NULL) {
            cJSON *at = expect(children_of(unexpected), 0, "at", FIELD);
            if (at != NULL) {
                LHAT_CHECK(strcmp(detail_of(at), "number^") == 0,
                           "detail was \"%s\"", detail_of(at));
            }
        }
    }
    outline_dispose(&o);

    LHAT_TEST("05 の 3 章: module^ names the unit");
    outline_of(&o, "module^ app.util\nlet^ x = 1\n");
    expect(o.symbols, 0, "app.util", MODULE);
    expect(o.symbols, 1, "x", CONSTANT);
    outline_dispose(&o);
}

static void test_what_is_looked_through(void)
{
    Outline o;

    LHAT_TEST("a let^ inside an if^ or a loop body is listed; a focus is not");
    outline_of(&o,
               "if^ true^ {\n"
               "    let^ inner = 1\n"
               "}\n"
               "for^ i to^ 3 {\n"
               "    let^ each = i\n"
               "}\n"
               "run(f^ { let^ hidden = 1 })\n");
    LHAT_CHECK(count_of(o.symbols) == 2, "expected 2 symbols, got %d",
               count_of(o.symbols));
    expect(o.symbols, 0, "inner", CONSTANT);
    expect(o.symbols, 1, "each", CONSTANT);
    outline_dispose(&o);

    LHAT_TEST("the table a unit returns is the unit's own shape");
    outline_of(&o,
               "let^ helper = f^ { }\n"
               "return^ { run = helper, limit = 3 }\n");
    LHAT_CHECK(count_of(o.symbols) == 3, "expected 3 symbols, got %d",
               count_of(o.symbols));
    expect(o.symbols, 1, "run", PROPERTY);
    expect(o.symbols, 2, "limit", PROPERTY);
    outline_dispose(&o);

    LHAT_TEST("08 章: an LTON file lists its entries, at the file's own columns");
    const char *lton = "name = \"spinner\",\nspeed = 2\n";
    size_t whole = 0;
    char *wrapped = lsp_lton_wrap(lton, strlen(lton), &whole);
    outline_of_path(&o, "data.lton", wrapped, whole);
    LHAT_CHECK(count_of(o.symbols) == 2, "expected 2 symbols, got %d",
               count_of(o.symbols));
    cJSON *name = expect(o.symbols, 0, "name", PROPERTY);
    if (name != NULL) {
        LHAT_CHECK(position_field(name, "selectionRange", "start", "line") == 0 &&
                       position_field(name, "selectionRange", "start", "character") == 0,
                   "the first entry should stand at the file's own 0:0");
    }
    expect(o.symbols, 1, "speed", PROPERTY);
    outline_dispose(&o);
    free(wrapped);

    LHAT_TEST("a unit that does not parse still has an outline of what did");
    outline_of(&o,
               "let^ before = 1\n"
               "let^ = \n");
    LHAT_CHECK(o.symbols != NULL, "expected an array");
    LHAT_CHECK(count_of(o.symbols) >= 1, "expected at least the first let^");
    expect(o.symbols, 0, "before", CONSTANT);
    outline_dispose(&o);
}

int main(void)
{
    test_bindings();
    test_a_class();
    test_errors_and_modules();
    test_what_is_looked_through();
    return lhat_test_report("test_document_symbol");
}
