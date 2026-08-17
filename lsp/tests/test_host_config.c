// L^ (lhat) -- LSP server tests: lhat-host.json -> registrations.
//
// The round trip that matters: the JSON lhat_program_dump_host_api writes
// (pinned by test_program.c's test_dump_host_api), applied by
// host_config.c, has to make a program that checks a unit using those
// registrations clean -- that is the whole point of the file.

#include <stdlib.h>
#include <string.h>

#include "program_internal.h"

#include "host_config.h"
#include "testutil.h"

// The same table-driven fake disk tests/test_program.c reads units from.
typedef struct {
    const char *path;
    const char *text;
} File;

typedef struct {
    const File *files;
    size_t count;
} Disk;

static char *disk_load(void *context, const char *path, size_t *length)
{
    Disk *disk = (Disk *)context;
    for (size_t i = 0; i < disk->count; i++) {
        if (strcmp(disk->files[i].path, path) != 0) {
            continue;
        }
        size_t size = strlen(disk->files[i].text);
        char *copy = (char *)malloc(size + 1);
        if (copy != NULL) {
            memcpy(copy, disk->files[i].text, size + 1);
            *length = size;
        }
        return copy;
    }
    return NULL;
}

// A cut-down lhat --dump-host-api output: every kind of entry once.
static const char *const CONFIG =
    "{\n"
    "  \"types\": [\n"
    "    {\"kind\": \"errordef\", \"module\": \"std\", \"name\": \"error\","
    " \"variants\": [\"OutOfMemory\"]},\n"
    "    {\"kind\": \"hostdata\", \"module\": \"std.io\", \"name\": \"File\"},\n"
    "    {\"kind\": \"hostvalue\", \"module\": \"std.geo\", \"name\": \"Vec2\","
    " \"size\": 8, \"fields\": ["
    "{\"name\": \"x\", \"offset\": 0, \"type\": \"f32\"},"
    " {\"name\": \"y\", \"offset\": 4, \"type\": \"f32\"}]}\n"
    "  ],\n"
    "  \"functions\": [\n"
    // Answering the type alone (no error union) keeps the member test below
    // a one-liner -- f.write on a File|OutOfMemory union would rightly ask
    // for a catch^ first, which is 04 の 4 章's business, not this file's.
    "    {\"kind\": \"func\", \"module\": \"std.io\", \"name\": \"open\","
    " \"signature\": \"f^string^ -> std.io.File;\"},\n"
    "    {\"kind\": \"member\", \"module\": \"std.io\", \"type\": \"File\","
    " \"name\": \"write\", \"signature\": \"p^self^, string^;\"},\n"
    "    {\"kind\": \"hostvalue_member\", \"module\": \"std.geo\","
    " \"type\": \"Vec2\", \"name\": \"+\","
    " \"signature\": \"f^self^, std.geo.Vec2 -> std.geo.Vec2;\"},\n"
    "    {\"kind\": \"func\", \"module\": \"std.geo\", \"name\": \"vec2\","
    " \"signature\": \"f^number^, number^ -> std.geo.Vec2;\"},\n"
    "    {\"kind\": \"global\", \"name\": \"print\","
    " \"signature\": \"f^...->nil^;\"}\n"
    "  ],\n"
    // 18.5's places, by name. The two extra keys are the ones a reader has
    // to pass over: a place written false, and a place it has never heard of
    // -- which is what a dump from a later build would carry.
    "  \"annotations\": [\n"
    "    {\"module\": \"h\", \"name\": \"badge\", \"targets\":"
    " {\"field\": true, \"fileunique\": true,"
    " \"binding\": false, \"nonesuch\": true}}\n"
    "  ],\n"
    "  \"bindings\": [\n"
    "    {\"name\": \"print\", \"member\": \"L^.print\"}\n"
    "  ]\n"
    "}\n";

static void check_clean(const char *label, const char *source)
{
    static const File files[1] = {{"main.lh", NULL}};
    File file = files[0];
    file.text = source;
    Disk disk;
    disk.files = &file;
    disk.count = 1;

    LspHostConfig *config = lsp_host_config_parse(CONFIG, strlen(CONFIG));
    LHAT_CHECK(config != NULL, "%s: the config parsed", label);
    if (config == NULL) {
        return;
    }

    LhatProgram program;
    lhat_program_init(&program, true, disk_load, &disk);
    lsp_host_config_apply(config, &program);

    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(root != NULL, "%s: the unit was read", label);
    LHAT_CHECK(!lhat_program_has_errors(&program),
               "%s: the program checked clean", label);
    if (root != NULL) {
        LHAT_CHECK_EQ_INT(root->checked.diagnostic_count, 0);
    }

    lhat_program_dispose(&program);
    lsp_host_config_free(config);
}

// The same, for a source that has to be refused. Half the point of a place
// is what it keeps out, and a reader that took every name it saw would pass
// the tests above without ever having read one.
static void check_refused(const char *label, const char *source)
{
    static const File files[1] = {{"main.lh", NULL}};
    File file = files[0];
    file.text = source;
    Disk disk;
    disk.files = &file;
    disk.count = 1;

    LspHostConfig *config = lsp_host_config_parse(CONFIG, strlen(CONFIG));
    LHAT_CHECK(config != NULL, "%s: the config parsed", label);
    if (config == NULL) {
        return;
    }

    LhatProgram program;
    lhat_program_init(&program, true, disk_load, &disk);
    lsp_host_config_apply(config, &program);
    lhat_program_check(&program, "main.lh");
    LHAT_CHECK(lhat_program_has_errors(&program), "%s: refused", label);

    lhat_program_dispose(&program);
    lsp_host_config_free(config);
}

static void test_round_trip(void)
{
    LHAT_TEST("an import^ of a configured module checks clean");
    check_clean("import",
                "import^ std.io\n"
                "let^ f = std.io.open(\"a.txt\")\n");

    LHAT_TEST("a configured member is reached through its type");
    check_clean("member",
                "import^ std.io\n"
                "let^ f = std.io.open(\"a.txt\")\n"
                "f.write(\"hi\")\n");

    LHAT_TEST("a configured hostvalue type adds and reads fields");
    check_clean("hostvalue",
                "import^ std.geo\n"
                "let^ v = std.geo.vec2(1, 2) + std.geo.vec2(3, 4)\n"
                "let^ n : number^ = v.x\n");

    LHAT_TEST("a configured binding is written unqualified");
    check_clean("binding", "print(\"hi\")\n");

    // 18.5's places arrive as names rather than as a mask, so what the reader
    // made of them is only visible in where the annotation is taken.
    LHAT_TEST("an annotation is taken where its named place says");
    check_clean("annotation",
                "let^ D = def^{\n"
                "    self^{ @badge hp = 1 },\n"
                "}\n");

    LHAT_TEST("and refused where a place was written false");
    check_refused("annotation false",
                  "@badge\n"
                  "let^ x = 1\n");

    // 18.5's count travels the same way its places do, and only a second
    // occurrence shows whether it arrived.
    LHAT_TEST("a file-unique annotation is refused a second time");
    check_refused("annotation twice",
                  "let^ D = def^{\n"
                  "    self^{ @badge hp = 1, @badge mp = 2 },\n"
                  "}\n");
}

static void test_without_config(void)
{
    // The other half of the round trip: without the config the same source
    // has to fail -- otherwise the test above proves nothing.
    LHAT_TEST("the same import^ fails with no config applied");
    static const File files[] = {
        {"main.lh", "import^ std.io\n"},
    };
    Disk disk;
    disk.files = files;
    disk.count = 1;

    LhatProgram program;
    lhat_program_init(&program, true, disk_load, &disk);
    const LhatUnit *root = lhat_program_check(&program, "main.lh");
    LHAT_CHECK(root != NULL, "the unit was read");
    LHAT_CHECK(lhat_program_has_errors(&program), "the import was refused");
    lhat_program_dispose(&program);
}

static void test_malformed(void)
{
    LHAT_TEST("text that is not JSON does not parse");
    LHAT_CHECK(lsp_host_config_parse("not json", 8) == NULL,
               "the parse refused");

    // A well-formed file with an entry of an unknown kind keeps the rest:
    // half-written configs happen, and the registrations that do parse are
    // worth more applied than discarded.
    LHAT_TEST("an unknown kind is skipped, the rest is kept");
    static const char *const mixed =
        "{\"functions\": ["
        "{\"kind\": \"wormhole\", \"name\": \"x\", \"signature\": \"f^;\"},"
        "{\"kind\": \"global\", \"name\": \"print\","
        " \"signature\": \"f^...->nil^;\"}"
        "], \"bindings\": [{\"name\": \"print\", \"member\": \"L^.print\"}]}";
    LspHostConfig *config = lsp_host_config_parse(mixed, strlen(mixed));
    LHAT_CHECK(config != NULL, "the config parsed");
    if (config != NULL) {
        static const File files[] = {
            {"main.lh", "print(\"hi\")\n"},
        };
        Disk disk;
        disk.files = files;
        disk.count = 1;
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        lsp_host_config_apply(config, &program);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "the known entries still applied");
        lhat_program_dispose(&program);
        lsp_host_config_free(config);
    }
}

int main(void)
{
    test_round_trip();
    test_without_config();
    test_malformed();
    return lhat_test_report("test_host_config");
}
