// L^ (lhat) -- tests for std.binary (11 章).
//
// The bytes of 11 の 3.4's example are pinned exactly, since the order is the
// promise other machines read; the other kinds are pinned by a round trip
// and by what decode refuses.

#include "lhat/extension.h"
#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/binary.h"

static const LhatTestRegister regs[] = {lhatstdlib_binary_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static bool checks(const char *text)
{
    return lhat_test_check_text(regs, 1, text);
}

#define INPUT                                   \
    "import^ std.binary\n"                      \
    "let^ b = std.binary\n"                     \
    "let^ input = b.format({\n"                 \
    "    { \"aaa\", b.uint(16) },\n"            \
    "    { \"bbb\", b.uint(8) },\n"             \
    "    { \"buttonA\", b.bool() },\n"          \
    "    { \"buttonB\", b.bool() },\n"          \
    "    { \"buttonC\", b.bool() },\n"          \
    "})\n"                                      \
    "let^ value = { aaa = 11111, bbb = 111,\n"  \
    "    buttonA = true^, buttonB = true^, buttonC = false^ }\n"

static void test_order(void)
{
    LHAT_TEST("3.4: low bits first, low byte first, the rest padded with 0");
    {
        LhatTestRan ran = run_source(INPUT "return^ input.encode(value)\n");
        LHAT_CHECK_RAN_TEXT(ran, "\x67\x2B\x6F\x03");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("bits and size are the format's");
    {
        LhatTestRan ran = run_source(INPUT "return^ input.bits() * 100 + input.size()\n");
        LHAT_CHECK_RAN_INTEGER(ran, 2704);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what was encoded decodes back");
    {
        LhatTestRan ran = run_source(INPUT
                                     "let^ got = input.decode(input.encode(value))\n"
                                     "if^ got fits^ std.binary.Error { return^ 0 }\n"
                                     "if^ got[\"buttonA\"] and^ !got[\"buttonC\"] {\n"
                                     "    return^ got[\"aaa\"] + got[\"bbb\"]\n"
                                     "}\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11222);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_kinds(void)
{
    LHAT_TEST("int, range, fixed, enum, array and a nested format round trip");
    {
        LhatTestRan ran = run_source(
            "import^ std.binary\n"
            "let^ b = std.binary\n"
            "enum^ Dir { N, E, S, W }\n"
            "let^ frame = b.format({ { \"stick\", b.range(0, 8) }, { \"dir\", b.enum(Dir) } })\n"
            "let^ packet = b.format({\n"
            "    { \"delta\", b.int(5) },\n"
            "    { \"low\", b.range(-8, 7) },\n"
            "    { \"ratio\", b.fixed(0, 1, 0.01) },\n"
            "    { \"history\", b.array(3, frame) },\n"
            "})\n"
            "let^ data = packet.encode({ delta = 0 - 16, low = 0 - 8, ratio = 0.444,\n"
            "    history = { { stick = 8, dir = Dir.W }, { stick = 0, dir = Dir.N },\n"
            "                { stick = 3, dir = Dir.S } } })\n"
            "let^ got = packet.decode(data)\n"
            "if^ got fits^ std.binary.Error { return^ \"error\" }\n"
            "let^ h = got[\"history\"]\n"
            "if^ got[\"delta\"] = 0 - 16 and^ got[\"low\"] = 0 - 8\n"
            "    and^ got[\"ratio\"] > 0.4399 and^ got[\"ratio\"] < 0.4401\n"
            "    and^ h[0][\"stick\"] = 8 and^ h[0][\"dir\"] = Dir.W\n"
            "    and^ h[2][\"stick\"] = 3 and^ h[2][\"dir\"] = Dir.S {\n"
            "    return^ \"ok\"\n"
            "}\n"
            "return^ \"wrong\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "ok");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a width is read off the range");
    {
        LhatTestRan ran = run_source("import^ std.binary\n"
                                     "let^ b = std.binary\n"
                                     "return^ b.range(-8, 7).bits() * 100 + b.range(0, 100).bits()\n"
                                     "    + b.fixed(0, 1, 0.01).bits() * 10000\n");
        LHAT_CHECK_RAN_INTEGER(ran, 70407);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_errors(void)
{
    LHAT_TEST("3.5: bytes shorter than the format are Truncated");
    {
        LhatTestRan ran = run_source(INPUT
                                     "let^ got = input.decode(\"ab\")\n"
                                     "if^ got fits^ std.binary.Error.Truncated { return^ \"truncated\" }\n"
                                     "return^ \"other\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "truncated");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and a number outside its kind is Malformed");
    {
        // '?' is 0x3F: the low four bits are 15, past range(0, 8)'s 8.
        LhatTestRan ran = run_source("import^ std.binary\n"
                                     "let^ f = std.binary.format({ { \"s\", std.binary.range(0, 8) } })\n"
                                     "let^ got = f.decode(\"?\")\n"
                                     "if^ got fits^ std.binary.Error.Malformed { return^ \"malformed\" }\n"
                                     "return^ \"other\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "malformed");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a value that does not fit is the writer's mistake: a panic");
    {
        LhatTestRan ran = run_source(INPUT
                                     "value[\"bbb\"] := 256\n"
                                     "return^ input.encode(value)\n");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and so is a missing field");
    {
        LhatTestRan ran = run_source(INPUT
                                     "return^ input.encode({ aaa = 1 })\n");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and a name written twice in a format");
    {
        LhatTestRan ran = run_source("import^ std.binary\n"
                                     "let^ b = std.binary\n"
                                     "let^ f = b.format({ { \"a\", b.bool() }, { \"a\", b.bool() } })\n"
                                     "return^ 1\n");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_reuse(void)
{
    LHAT_TEST("encodeInto writes over one Bytes, and decode reads it");
    {
        LhatTestRan ran = run_source(INPUT
                                     "let^ out = std.binary.bytes()\n"
                                     "input.encodeInto(value, out)\n"
                                     "value[\"aaa\"] := 7\n"
                                     "input.encodeInto(value, out)\n"
                                     "let^ got = input.decode(out)\n"
                                     "if^ got fits^ std.binary.Error { return^ 0 }\n"
                                     "return^ got[\"aaa\"] * 100 + out.size()\n");
        LHAT_CHECK_RAN_INTEGER(ran, 704);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and toString copies the bytes out");
    {
        LhatTestRan ran = run_source(INPUT
                                     "let^ out = std.binary.bytes()\n"
                                     "input.encodeInto(value, out)\n"
                                     "return^ out.toString()\n");
        LHAT_CHECK_RAN_TEXT(ran, "\x67\x2B\x6F\x03");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("decodeInto fills the table it is given");
    {
        LhatTestRan ran = run_source(INPUT
                                     "let^ into = {}\n"
                                     "let^ first = input.decodeInto(input.encode(value), into)\n"
                                     "value[\"aaa\"] := 42\n"
                                     "let^ second = input.decodeInto(input.encode(value), into)\n"
                                     "if^ first fits^ std.binary.Error { return^ 0 }\n"
                                     "if^ second fits^ std.binary.Error { return^ 0 }\n"
                                     "return^ into[\"aaa\"]\n");
        LHAT_CHECK_RAN_INTEGER(ran, 42);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_types(void)
{
    LHAT_TEST("decode answers a table that promises nothing (B1)");
    {
        LHAT_CHECK(checks(INPUT
                          "let^ got = input.decode(\"abcd\")\n"
                          "if^ got fits^ std.binary.Error { return^ 0 }\n"
                          "let^ n = got[\"aaa\"]\n"),
                   "read by index");
        LHAT_CHECK(!checks(INPUT "let^ s = input.encode(5)\n"),
                   "and encode takes a table");
    }
}

// A native extension reaching Bytes the way 11 の 2.1 tells one to: through
// the ABI table's lookup, registering its Bytes overload only when the host
// registered std.binary. ext.total answers the sum of the bytes it is given;
// ext.fill makes a Bytes n bytes of 1.
static void total_text(LhatMachine *machine, void *context, const LhatValue *arguments,
                       size_t count, LhatValue *answers, int *answer_count)
{
    (void)machine; (void)context; (void)count;
    const LhatString *text = (const LhatString *)lhat_as_object(arguments[0]);
    int64_t sum = 0;
    for (size_t i = 0; i < text->length; i++) sum += (unsigned char)text->text[i];
    answers[0] = lhat_integer(sum);
    *answer_count = 1;
}

static void total_bytes(LhatMachine *machine, void *context, const LhatValue *arguments,
                        size_t count, LhatValue *answers, int *answer_count)
{
    (void)machine; (void)count;
    const LhatBinaryInterface *binary = (const LhatBinaryInterface *)context;
    const LhatBinaryBytes *bytes = binary->bytes(arguments[0]);
    int64_t sum = 0;
    for (size_t i = 0; bytes != NULL && i < bytes->length; i++) sum += bytes->data[i];
    answers[0] = lhat_integer(sum);
    *answer_count = 1;
}

static void fill_bytes(LhatMachine *machine, void *context, const LhatValue *arguments,
                       size_t count, LhatValue *answers, int *answer_count)
{
    (void)machine; (void)count; (void)answers; (void)answer_count;
    const LhatBinaryInterface *binary = (const LhatBinaryInterface *)context;
    LhatBinaryBytes *bytes = binary->bytes(arguments[0]);
    if (bytes != NULL && binary->resize(bytes, (size_t)lhat_as_integer(arguments[1]))) {
        memset(bytes->data, 1, bytes->length);
    }
}

static const char *install_ext(const LhatExtensionAPI *api, LhatProgram *program,
                               uint32_t phase, void **state)
{
    (void)state;
    if (phase != LHAT_EXTENSION_MEMBERS) return NULL;
    void *binary = api->lhat_lookup_host_context(program, "std.binary", NULL, "bytes");
    bool ok = api->lhat_register_func(program, "ext", "total", "f^string^ -> number^;",
                                      total_text, NULL);
    if (ok && binary != NULL) {
        ok = api->lhat_register_func(program, "ext", "total", "f^std.binary.Bytes -> number^;",
                                     total_bytes, binary) &&
             api->lhat_register_func(program, "ext", "fill", "p^std.binary.Bytes, number^;",
                                     fill_bytes, binary);
    }
    return ok ? NULL : "registration failed";
}

static const LhatExtension ext = {
    LHAT_EXTENSION_ABI, sizeof(LhatExtension), LHAT_VERSION, sizeof(LhatValue),
    "ext", NULL, 0, install_ext, NULL
};

static LhatExtensions *pool;

static bool register_ext(LhatProgram *program)
{
    static const LhatExtensionModule *module;
    if (pool == NULL) {
        pool = lhat_extensions_new(NULL);
        module = pool != NULL ? lhat_extensions_add(pool, "ext", &ext) : NULL;
    }
    return module != NULL && lhat_extensions_register(pool, program, &module, 1);
}

static void test_extension(void)
{
    static const LhatTestRegister with_binary[] = {lhatstdlib_binary_register, register_ext};
    static const LhatTestRegister alone[] = {register_ext};

    LHAT_TEST("an extension reads and fills a Bytes through the interface");
    {
        LhatTestRan ran = lhat_test_run(with_binary, 2,
                                        "import^ std.binary\n"
                                        "import^ ext\n"
                                        "let^ out = std.binary.bytes()\n"
                                        "ext.fill(out, 3)\n"
                                        "return^ ext.total(out) * 1000 + ext.total(\"ab\")\n");
        LHAT_CHECK_RAN_INTEGER(ran, 3195);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and without std.binary it offers the string^ overload alone");
    {
        LhatTestRan ran = lhat_test_run(alone, 1, "import^ ext\nreturn^ ext.total(\"ab\")\n");
        LHAT_CHECK_RAN_INTEGER(ran, 195);
        lhat_test_ran_dispose(&ran);
        LHAT_CHECK(!lhat_test_check_text(alone, 1, "import^ ext\next.fill(1, 3)\n"),
                   "fill is not there");
    }
}

int main(void)
{
    test_order();
    test_kinds();
    test_errors();
    test_reuse();
    test_types();
    test_extension();
    int failed = lhat_test_report("test_binary");
    // After every program: a pool outlives what it registered into.
    lhat_extensions_free(pool);
    return failed;
}
