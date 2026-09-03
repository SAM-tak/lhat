// L^ (lhat) -- tests for 05 の 10 章: a compiled unit as bytes.
//
// A unit is checked and compiled from text, written out, and read back by
// a second program that never sees the text -- through the same loader and
// the same lhat_program_check, since the bytes say what they are. What the
// two programs answer has to agree, and what a binary unit refuses (a
// damaged file, another build, a text unit beside it) is pinned here too.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "lhat/port.h"
#include "lhat/program.h"
#include "program_internal.h"
#include "check.h"
#include "rttype.h"

// A disk of bytes: text or binary, told apart by nothing but the content.
typedef struct {
    const char *path;
    const uint8_t *bytes;
    size_t length;
} Entry;

typedef struct {
    Entry entries[4];
    size_t count;
} Disk;

static char *disk_load(void *context, const char *path, size_t *length)
{
    Disk *disk = (Disk *)context;
    for (size_t i = 0; i < disk->count; i++) {
        if (strcmp(disk->entries[i].path, path) != 0) {
            continue;
        }
        char *copy = (char *)malloc(disk->entries[i].length + 1);
        if (copy != NULL) {
            memcpy(copy, disk->entries[i].bytes, disk->entries[i].length);
            copy[disk->entries[i].length] = '\0';
            *length = disk->entries[i].length;
        }
        return copy;
    }
    return NULL;
}

static void disk_text(Disk *disk, const char *path, const char *text)
{
    disk->entries[disk->count].path = path;
    disk->entries[disk->count].bytes = (const uint8_t *)text;
    disk->entries[disk->count].length = strlen(text);
    disk->count++;
}

static void disk_bytes(Disk *disk, const char *path, const uint8_t *bytes,
                       size_t length)
{
    disk->entries[disk->count].path = path;
    disk->entries[disk->count].bytes = bytes;
    disk->entries[disk->count].length = length;
    disk->count++;
}

static bool has_program_error(const LhatProgram *program,
                              LhatProgramErrorCode code)
{
    for (size_t i = 0; i < program->diagnostic_count; i++) {
        if (program->diagnostics[i].code == code) {
            return true;
        }
    }
    return false;
}

// Checks, compiles and runs `root`, answering the integer the unit returned
// (or -1 when anything along the way refused).
static int64_t run_root(LhatProgram *program, const char *root_path,
                        const LhatUnit **out_root)
{
    const LhatUnit *root = lhat_program_check(program, root_path);
    if (out_root != NULL) {
        *out_root = root;
    }
    if (root == NULL || lhat_program_has_errors(program) ||
        !lhat_program_compile(program)) {
        return -1;
    }
    LhatMachine *machine = lhat_machine_new();
    lhat_program_install(program, machine);
    LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
    int64_t answer = ran.status == LHAT_RUN_OK && lhat_is_integer(ran.value)
                         ? lhat_as_integer(ran.value)
                         : -1;
    lhat_machine_dispose(machine);
    return answer;
}

// Everything a chunk can hold: closures with captured places, a def^ with
// a member read through a cache, an errordef^ kind as a constant, an enum^
// with its descriptor, a type value, reals, strings, a loop.
static const char *const SPECIMEN =
    "errordef^ Bad { Oops }\n"
    "enum^ Color { Red, Green = 5, Blue }\n"
    "let^ Vec = t^{number^[2]}\n"
    "let^ Pt = def^{\n"
    "    self^{ x := 0, y := 0 },\n"
    "    sum := f^self^ -> number^ { return^ self^.x + self^.y },\n"
    "}\n"
    "let^ make = f^ base:number^ -> f^number^ -> number^; {\n"
    "    return^ f^ k:number^ -> number^ { return^ base * 10 + k }\n"
    "}\n"
    "var^ n = 0\n"
    "let^ p = Pt.new()\n"
    "p.x := 3\n"
    "p.y := 4\n"
    "n := n + p.sum()\n"
    "n := n + make(2)(1)\n"
    "let^ v : Vec = {1, 2}\n"
    "if^ v fits^ Vec { n := n + 100 }\n"
    "if^ Color.Blue.value = 6 { n := n + 1000 }\n"
    "let^ c = Color.Green\n"
    "if^ c fits^ Color { n := n + 10000 }\n"
    "let^ e = error^Bad.Oops{ message := \"x\" }\n"
    "if^ e fits^ Bad { n := n + 100000 }\n"
    "let^ s = $\"{n}\"\n"
    "if^ s.length = 6 { n := n + 1000000 }\n"
    "let^ half = 0.5\n"
    "if^ half * 2 = 1 { n := n + 10000000 }\n"
    "for^ i from^ 1 to^ 3 { n := n + i }\n"
    "return^ n\n";
#define SPECIMEN_ANSWER 11111134

// Compiles `text` as main.lh in a program of its own and writes it out.
static bool write_text(const char *text, bool with_debug, uint8_t **bytes,
                       size_t *length, int64_t *answer)
{
    Disk disk;
    memset(&disk, 0, sizeof disk);
    disk_text(&disk, "main.lh", text);
    LhatProgram program;
    lhat_program_init(&program, true, disk_load, &disk);
    const LhatUnit *root = NULL;
    *answer = run_root(&program, "main.lh", &root);
    bool ok = root != NULL &&
              lhat_unit_write_binary(root, with_debug, bytes, length);
    lhat_program_dispose(&program);
    return ok;
}

static void test_roundtrip(void)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    int64_t from_text = -1;

    LHAT_TEST("a unit written out and read back answers the same");
    LHAT_CHECK(write_text(SPECIMEN, true, &bytes, &length, &from_text),
               "the text compiled and wrote");
    LHAT_CHECK_EQ_INT(from_text, SPECIMEN_ANSWER);
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", bytes, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatUnit *root = NULL;
        int64_t from_binary = run_root(&program, "main.lh", &root);
        LHAT_CHECK_EQ_INT(from_binary, SPECIMEN_ANSWER);
        LHAT_CHECK(root != NULL && lhat_unit_proto(root) != NULL,
                   "the unit has its body");
        LHAT_CHECK(root != NULL && lhat_unit_diagnostic_count(root) == 0,
                   "and nothing to report");
        lhat_program_dispose(&program);
    }
    lhat_free(bytes);

    LHAT_TEST("without the debug names it runs the same");
    LHAT_CHECK(write_text(SPECIMEN, false, &bytes, &length, &from_text),
               "wrote without names");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", bytes, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK_EQ_INT(run_root(&program, "main.lh", NULL),
                          SPECIMEN_ANSWER);
        lhat_program_dispose(&program);
    }
    lhat_free(bytes);
}

static void test_refusals(void)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    int64_t answer = -1;
    LHAT_CHECK(write_text("return^ 42\n", true, &bytes, &length, &answer),
               "a small unit wrote");

    // The hash covers everything after the header.
    LHAT_TEST("a damaged payload is refused");
    {
        uint8_t *damaged = (uint8_t *)malloc(length);
        memcpy(damaged, bytes, length);
        damaged[length - 3] ^= 0x55;
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", damaged, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL,
                   "nothing loads");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_BAD_BINARY),
                   "and the reason is said");
        lhat_program_dispose(&program);
        free(damaged);
    }

    LHAT_TEST("another build's fingerprint is refused");
    {
        uint8_t *other = (uint8_t *)malloc(length);
        memcpy(other, bytes, length);
        other[9] ^= 0x01;  // inside the fingerprint field
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", other, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL,
                   "nothing loads");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_BAD_BINARY),
                   "and the reason is said");
        lhat_program_dispose(&program);
        free(other);
    }
    lhat_free(bytes);
}

static const char *const LIB =
    "module^ lib\n"
    "public^ enum^ E { A, B }\n"
    "public^ let^ twice = f^ x:number^ -> number^ { return^ x * 2 }\n";

static const char *const MAIN =
    "let^ lib = require^ \"lib.lh\"\n"
    "var^ n = lib.twice(21)\n"
    "let^ x = lib.E.A\n"
    "if^ x fits^ lib.E { n := n + 100 }\n"
    "return^ n\n";
#define TWO_ANSWER 142

static void test_units(void)
{
    // Both units written from one text program.
    uint8_t *lib_bytes = NULL;
    size_t lib_length = 0;
    uint8_t *main_bytes = NULL;
    size_t main_length = 0;
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_text(&disk, "main.lh", MAIN);
        disk_text(&disk, "lib.lh", LIB);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatUnit *root = NULL;
        LHAT_CHECK_EQ_INT(run_root(&program, "main.lh", &root), TWO_ANSWER);
        const LhatUnit *lib = NULL;
        for (const LhatUnit *u = lhat_program_units(&program); u != NULL;
             u = lhat_unit_next(u)) {
            if (u != root) {
                lib = u;
            }
        }
        LHAT_CHECK(root != NULL && lib != NULL, "two units");
        LHAT_CHECK(lhat_unit_write_binary(root, true, &main_bytes,
                                          &main_length),
                   "main wrote");
        LHAT_CHECK(lhat_unit_write_binary(lib, true, &lib_bytes, &lib_length),
                   "lib wrote");
        lhat_program_dispose(&program);
    }

    // 05 の 10 章: a binary unit's require^ reaches a binary unit, and the
    // enum declared in one is the identity the other's fits^ compares.
    LHAT_TEST("binary units require each other, and an enum keeps its identity");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", main_bytes, main_length);
        disk_bytes(&disk, "lib.lh", lib_bytes, lib_length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK_EQ_INT(run_root(&program, "main.lh", NULL), TWO_ANSWER);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a binary unit beside a text one is refused, either way round");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", main_bytes, main_length);
        disk_text(&disk, "lib.lh", LIB);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL,
                   "binary main over text lib does not load");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_MIXED),
                   "and says why");
        lhat_program_dispose(&program);
    }
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_text(&disk, "main.lh", MAIN);
        disk_bytes(&disk, "lib.lh", lib_bytes, lib_length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        (void)lhat_program_check(&program, "main.lh");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_MIXED),
                   "text main over binary lib says why");
        lhat_program_dispose(&program);
    }
    lhat_free(main_bytes);
    lhat_free(lib_bytes);
}

static void test_traceback(void)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    int64_t answer = -1;
    LHAT_CHECK(write_text("let^ boom = p^ { panic^ 7 }\n"
                          "boom()\n",
                          false, &bytes, &length, &answer),
               "a panicking unit wrote");

    // 04 の 11.6改: the lines and the labels stay in the bytes, with or
    // without the debug names -- a traceback from a binary unit reads the
    // same as from the text.
    LHAT_TEST("a traceback out of a binary unit names the line");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", bytes, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program),
                   "loaded and linked");
        if (root != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
            LhatFrameInfo frame;
            LHAT_CHECK(lhat_machine_fault_frame(machine, 0, &frame),
                       "a frame stands");
            LHAT_CHECK_EQ_INT(frame.line, 1);
            LHAT_CHECK(frame.name != NULL && strcmp(frame.name, "boom") == 0,
                       "under its name");
            LHAT_CHECK(frame.source != NULL &&
                           strcmp(frame.source, "main.lh") == 0,
                       "in its unit");
            lhat_machine_dispose(machine);
        }
        lhat_program_dispose(&program);
    }
    lhat_free(bytes);
}

// The names that leave a chunk: a hostdata type behind a fits^, a host
// value type behind a parameter (its width baked into the frame), a
// registered error kind as a constant and behind a fits^, the language's
// CastFailure, and a registered enum's declaration.
static const char *const HOSTED =
    "import^ k\n"
    "var^ n = 0\n"
    "let^ x : any^ = 1\n"
    "if^ x fits^ k.T { n := n + 1 else^: n := n + 2 }\n"
    "let^ f = f^ v:k.V -> number^ { return^ 1 }\n"
    "let^ e = error^k.Bad.Oops{ message := \"m\" }\n"
    "if^ e fits^ k.Bad { n := n + 10 }\n"
    "if^ x fits^ localerror^.CastFailure { n := n + 100 else^: n := n + 200 }\n"
    "if^ k.Mode.A fits^ k.Mode { n := n + 1000 }\n"
    "return^ n\n";
#define HOSTED_ANSWER 1212

static void register_host(LhatProgram *program, size_t value_size)
{
    static const char *const variants[] = { "Oops" };
    static const char *const modes[] = { "A", "B" };
    LHAT_CHECK(lhat_register_hostdata_type(program, "k", "T") != NULL,
               "hostdata registered");
    LHAT_CHECK(lhat_register_hostvalue_type(program, "k", "V", value_size) !=
                   NULL,
               "host value registered");
    LHAT_CHECK(lhat_register_error_kind(program, "k", "Bad", variants, 1, NULL,
                                        NULL),
               "error kind registered");
    LHAT_CHECK(lhat_register_enum(program, "k", NULL, "Mode", modes, 2),
               "enum registered");
}

static void test_host_references(void)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_text(&disk, "main.lh", HOSTED);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        register_host(&program, 8);
        const LhatUnit *root = NULL;
        LHAT_CHECK_EQ_INT(run_root(&program, "main.lh", &root), HOSTED_ANSWER);
        LHAT_CHECK(root != NULL &&
                       lhat_unit_write_binary(root, true, &bytes, &length),
                   "the hosted unit wrote");
        lhat_program_dispose(&program);
    }

    LHAT_TEST("host names resolve against the same registrations");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", bytes, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        register_host(&program, 8);
        LHAT_CHECK_EQ_INT(run_root(&program, "main.lh", NULL), HOSTED_ANSWER);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("and a program that registered nothing refuses the unit");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "main.lh", bytes, length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        LHAT_CHECK(lhat_program_check(&program, "main.lh") == NULL,
                   "nothing loads");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_HOST_MISMATCH),
                   "and says why");
        lhat_program_dispose(&program);
    }

    // 8.9: the width is checked too, but not here -- the registry is the
    // process's (registry.h), so one name has one size for as long as
    // the process runs, and a second registration of k.V at another size
    // is refused before any binary could meet it. The check matters
    // across processes, where the host may have changed the type.
    lhat_free(bytes);
}

static void test_exports(void)
{
    uint8_t *lib_bytes = NULL;
    size_t lib_length = 0;
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_text(&disk, "lib.lh", LIB);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatUnit *lib = lhat_program_check(&program, "lib.lh");
        LHAT_CHECK(lib != NULL && lhat_program_compile(&program), "lib built");
        LHAT_CHECK(lib != NULL && lhat_unit_write_binary(lib, true, &lib_bytes,
                                                         &lib_length),
                   "lib wrote");
        lhat_program_dispose(&program);
    }

    // 8.7: what a host asks of a unit's exports is answered off the
    // descriptors the bytes carried.
    LHAT_TEST("a binary unit answers its export descriptors");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_bytes(&disk, "lib.lh", lib_bytes, lib_length);
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatUnit *lib = lhat_program_check(&program, "lib.lh");
        LHAT_CHECK(lib != NULL, "loaded");
        if (lib != NULL) {
            LHAT_CHECK_EQ_INT(lhat_unit_export_count(lib), 2);
            LhatUnitText first = lhat_unit_export_name(lib, 0);
            LHAT_CHECK(first.text != NULL && first.length == 1 &&
                           first.text[0] == 'E',
                       "the enum comes first");
            const LhatRuntimeType *twice = lhat_unit_export_type(lib, "twice");
            LHAT_CHECK(twice != NULL && twice->kind == LHAT_TYPE_RT_SUBROUTINE,
                       "twice is a subroutine");
            char text[64];
            size_t n = lhat_unit_export_type_text(lib, "twice", text,
                                                  sizeof text);
            LHAT_CHECK(n != SIZE_MAX && strncmp(text, "f^", 2) == 0,
                       "and spells as one");
            LHAT_CHECK(lhat_unit_export_type(lib, "nope") == NULL,
                       "a name it did not publish answers nothing");
        }
        lhat_program_dispose(&program);
    }
    lhat_free(lib_bytes);
}

// 10.7: the signature table. What a full build writes for a registration
// is what install builds for it -- the parameters as parts, the receiver
// and variadic marks -- and a program of any build reads it back.
static void host_noop(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    (void)answers;
    *answer_count = 0;
}

static const char *const SIGNATURES[] = {
    "f^number^, k.T -> number^;",
    "f^string^ -> number^|nil^;",
    "f^t^{number^[]} -> string^|k.Bad.Oops;",
    "f^k.Mode, k.V -> k.V;",
    "f^ -> nil^;",
    "p^ ...:number^;",
};
#define SIGNATURE_COUNT (sizeof SIGNATURES / sizeof SIGNATURES[0])
static const char *const MEMBER_SIGNATURES[] = {
    "f^self^ -> c^{p^ -> any^};",
    "p^self^, string^;",
};
#define MEMBER_SIGNATURE_COUNT \
    (sizeof MEMBER_SIGNATURES / sizeof MEMBER_SIGNATURES[0])

static void register_signatures(LhatProgram *program)
{
    char name[8];
    for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
        snprintf(name, sizeof name, "f%zu", i);
        LHAT_CHECK(lhat_register_func(program, "k", name, SIGNATURES[i],
                                      host_noop, NULL),
                   "a function registered");
    }
    for (size_t i = 0; i < MEMBER_SIGNATURE_COUNT; i++) {
        snprintf(name, sizeof name, "m%zu", i);
        LHAT_CHECK(lhat_register_member(program, "k", "T", name,
                                        MEMBER_SIGNATURES[i], host_noop, NULL),
                   "a member registered");
    }
}

// The descriptor the table holds against the one the front end builds for
// the same text: the same parts, the same marks.
static void check_signature(LhatProgram *program, const char *text)
{
    const LhatRuntimeType *held = lhat_program_signature_type(program, text);
    LHAT_CHECK(held != NULL, text);
    const LhatType *written = lhat_type_of_text(text, strlen(text),
                                                &program->types,
                                                program->hosted, NULL);
    LHAT_CHECK(written != NULL, "the text reads");
    if (held == NULL || written == NULL) {
        return;
    }
    const LhatRuntimeType *expected =
        lhat_rt_from_checked(&program->host_heap, written);
    LHAT_CHECK(expected != NULL && held->kind == LHAT_TYPE_RT_SUBROUTINE,
               "a subroutine");
    if (expected == NULL) {
        return;
    }
    LHAT_CHECK_EQ_INT(held->part_count, expected->part_count);
    for (size_t i = 0; i < held->part_count && i < expected->part_count; i++) {
        bool same = (held->parts[i] == NULL) == (expected->parts[i] == NULL) &&
                    (held->parts[i] == NULL ||
                     lhat_runtime_type_equal(held->parts[i], expected->parts[i]));
        LHAT_CHECK(same, "the parameter descriptors agree");
    }
    LHAT_CHECK(held->takes_self == expected->takes_self &&
                   held->self_last == expected->self_last &&
                   (held->variadic != NULL) == (expected->variadic != NULL),
               "the marks agree");
}

static void test_signatures(void)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    {
        LhatProgram program;
        lhat_program_init(&program, true, NULL, NULL);
        register_host(&program, 8);
        register_signatures(&program);
        LHAT_CHECK(lhat_program_write_signatures(&program, &bytes, &length),
                   "the table wrote");
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a signature table reads back what install would build");
    {
        LhatProgram program;
        lhat_program_init(&program, true, NULL, NULL);
        register_host(&program, 8);
        LHAT_CHECK(lhat_program_read_signatures(&program, bytes, length),
                   "the table read");
        for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
            check_signature(&program, SIGNATURES[i]);
        }
        for (size_t i = 0; i < MEMBER_SIGNATURE_COUNT; i++) {
            check_signature(&program, MEMBER_SIGNATURES[i]);
        }
        LHAT_CHECK(lhat_program_signature_type(&program, "f^ -> bool^;") ==
                       NULL,
                   "a text the table does not hold answers nothing");
        LHAT_CHECK(!has_program_error(&program, LHAT_PROGRAM_ERR_BAD_BINARY),
                   "and reports nothing");
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a name the reading program did not register is a mismatch");
    {
        LhatProgram program;
        lhat_program_init(&program, true, NULL, NULL);
        LHAT_CHECK(lhat_program_read_signatures(&program, bytes, length),
                   "the table read");
        LHAT_CHECK(lhat_program_signature_type(&program, SIGNATURES[0]) == NULL,
                   "k.T is not there");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_HOST_MISMATCH),
                   "and says why");
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a table from another build is refused");
    {
        uint8_t *other = (uint8_t *)malloc(length);
        memcpy(other, bytes, length);
        other[9] ^= 0x01;
        LhatProgram program;
        lhat_program_init(&program, true, NULL, NULL);
        LHAT_CHECK(!lhat_program_read_signatures(&program, other, length),
                   "refused");
        LHAT_CHECK(has_program_error(&program, LHAT_PROGRAM_ERR_BAD_BINARY),
                   "and says why");
        lhat_program_dispose(&program);
        free(other);
    }
    lhat_free(bytes);
}

// 10.6: what a host asks of the declarations (02 の 18) is answered off the
// records the bytes carried -- the same answers the tree gave when the unit
// was written. The specimen puts something at every address: the unit's
// head, a plain binding, a definition with template fields and members of
// each shape, and a composition.
static const char REFLECTED[] =
    "# What the unit is for.\n"
    "@tool\n"
    "module^ ns.main\n"
    "\n"
    "let^ note = p^ tag:string^, more:string^ { }\n"
    "\n"
    "# A thing with parts.\n"
    "@icon(\"res://x.svg\")\n"
    "public^ let^ Thing = def^{\n"
    "  self^{\n"
    "    # Hit points.\n"
    "    @export(0, 100) hp = 5,\n"
    "    label : string^ = \"x\",\n"
    "    flag = true^,\n"
    "  },\n"
    "  # Goes somewhere.\n"
    "  @rpc(\"any_peer\") go = p^self^, where:number^, ...:string^ {\n"
    "    note(\"moved\", id^done)\n"
    "  },\n"
    "  abstract^ waiting : p^string^;,\n"
    "  hollow = p^self^ { },\n"
    "}\n"
    "@kinds(-1.5, \"text\", SOME_NAME, true^, false^)\n"
    "let^ x = 1\n"
    "public^ let^ Both = def^{ a = 1 } .. def^{ @rpc(\"b\") b = p^self^ { } }\n";

static void register_reflected(LhatProgram *program)
{
    lhat_register_annotation(program, "h", "tool", LHAT_ANNOTATION_UNIT);
    lhat_register_annotation(program, "h", "icon", LHAT_ANNOTATION_BINDING);
    lhat_register_annotation_signature(program, "icon", "p^ string^;");
    lhat_register_annotation(program, "h", "export", LHAT_ANNOTATION_FIELD);
    lhat_register_annotation_signature(program, "export",
                                       "p^ number^, number^;");
    lhat_register_annotation(program, "h", "rpc", LHAT_ANNOTATION_MEMBER);
    lhat_register_annotation_signature(program, "rpc", "p^ string^;");
    lhat_register_annotation(program, "h", "kinds", LHAT_ANNOTATION_BINDING);
    lhat_register_annotation_signature(
        program, "kinds", "p^ number^, string^, string^, bool^, bool^;");
}

static LhatUnitText text_of(const char *text, size_t length)
{
    LhatUnitText out;
    out.text = text;
    out.length = length;
    return out;
}

static void check_same_text(const char *what, LhatUnitText a, LhatUnitText b)
{
    LHAT_CHECK((a.text == NULL) == (b.text == NULL) && a.length == b.length &&
                   (a.text == NULL || memcmp(a.text, b.text, a.length) == 0),
               "%s: \"%.*s\" became \"%.*s\"", what, (int)a.length,
               a.text != NULL ? a.text : "", (int)b.length,
               b.text != NULL ? b.text : "");
}

static void check_same_about(const LhatUnit *text, const LhatUnit *binary,
                             const char *definition, const char *name)
{
    size_t count = lhat_unit_annotation_count(text, definition, name);
    LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(binary, definition, name),
                      count);
    for (size_t i = 0; i < count; i++) {
        LhatAnnotation a = lhat_unit_annotation(text, definition, name, i);
        LhatAnnotation b = lhat_unit_annotation(binary, definition, name, i);
        check_same_text("annotation", text_of(a.name, a.name_length),
                        text_of(b.name, b.name_length));
        LHAT_CHECK_EQ_INT(b.argument_count, a.argument_count);
        for (size_t j = 0; j < a.argument_count; j++) {
            LhatAnnotationArgument x = lhat_annotation_argument(a, j);
            LhatAnnotationArgument y = lhat_annotation_argument(b, j);
            LHAT_CHECK(x.kind == y.kind && x.number == y.number &&
                           x.boolean == y.boolean,
                       "argument %zu of @%.*s", j, (int)a.name_length, a.name);
            check_same_text("argument", text_of(x.text, x.length),
                            text_of(y.text, y.length));
        }
    }
    char from_text[256];
    char from_binary[256];
    size_t n = lhat_unit_documentation(text, definition, name, from_text,
                                       sizeof from_text);
    size_t m = lhat_unit_documentation(binary, definition, name, from_binary,
                                       sizeof from_binary);
    LHAT_CHECK(n == m && strcmp(from_text, from_binary) == 0,
               "documentation at (%s, %s): \"%s\" became \"%s\"",
               definition != NULL ? definition : "-",
               name != NULL ? name : "-", from_text, from_binary);
}

static void check_same_members(const LhatUnit *text, const LhatUnit *binary,
                               const char *definition)
{
    size_t count = lhat_unit_member_count(text, definition);
    LHAT_CHECK_EQ_INT(lhat_unit_member_count(binary, definition), count);
    for (size_t i = 0; i < count; i++) {
        LhatUnitMember a = lhat_unit_member(text, definition, i);
        LhatUnitMember b = lhat_unit_member(binary, definition, i);
        check_same_text("member", text_of(a.name, a.name_length),
                        text_of(b.name, b.name_length));
        LHAT_CHECK(a.declared == b.declared && a.empty_body == b.empty_body &&
                       a.type == b.type &&
                       a.parameter_count == b.parameter_count,
                   "member %zu of %s", i, definition);
        for (size_t j = 0; j < a.parameter_count; j++) {
            LhatUnitParameter p =
                lhat_unit_member_parameter(text, definition, i, j);
            LhatUnitParameter q =
                lhat_unit_member_parameter(binary, definition, i, j);
            check_same_text("parameter", text_of(p.name, p.name_length),
                            text_of(q.name, q.name_length));
            LHAT_CHECK(p.type == q.type && p.variadic == q.variadic,
                       "parameter %zu of member %zu of %s", j, i, definition);
        }
        size_t names =
            lhat_unit_member_written_name_count(text, definition, i);
        LHAT_CHECK_EQ_INT(
            lhat_unit_member_written_name_count(binary, definition, i),
            names);
        for (size_t j = 0; j < names; j++) {
            check_same_text(
                "written name",
                lhat_unit_member_written_name(text, definition, i, j),
                lhat_unit_member_written_name(binary, definition, i, j));
        }
        if (a.name != NULL && a.name_length < 64) {
            char named[64];
            memcpy(named, a.name, a.name_length);
            named[a.name_length] = '\0';
            check_same_about(text, binary, definition, named);
        }
    }
}

static void test_reflection(void)
{
    Disk text_disk;
    memset(&text_disk, 0, sizeof text_disk);
    disk_text(&text_disk, "main.lh", REFLECTED);
    LhatProgram from_text;
    lhat_program_init(&from_text, true, disk_load, &text_disk);
    register_reflected(&from_text);
    const LhatUnit *text = lhat_program_check(&from_text, "main.lh");
    uint8_t *bytes = NULL;
    size_t length = 0;

    LHAT_TEST("a binary unit answers what the tree answered");
    LHAT_CHECK(text != NULL && !lhat_program_has_errors(&from_text) &&
                   lhat_program_compile(&from_text),
               "the text built");
    LHAT_CHECK(text != NULL &&
                   lhat_unit_write_binary(text, false, &bytes, &length),
               "and wrote");
    // The specimen has to say something at every address for the
    // comparison below to mean anything.
    LHAT_CHECK(lhat_unit_annotation_count(text, NULL, NULL) == 1 &&
                   lhat_unit_member_count(text, "Thing") == 6 &&
                   lhat_unit_member_count(text, "Both") == 2 &&
                   lhat_unit_member_written_name_count(text, "Thing", 3) == 2,
               "the specimen says what it should");

    Disk binary_disk;
    memset(&binary_disk, 0, sizeof binary_disk);
    disk_bytes(&binary_disk, "main.lh", bytes, length);
    LhatProgram from_binary;
    lhat_program_init(&from_binary, true, disk_load, &binary_disk);
    const LhatUnit *binary = lhat_program_check(&from_binary, "main.lh");
    LHAT_CHECK(binary != NULL && !lhat_program_has_errors(&from_binary),
               "the bytes loaded");
    if (text != NULL && binary != NULL) {
        check_same_about(text, binary, NULL, NULL);
        static const char *const bindings[] = {"note", "Thing", "x", "Both"};
        for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
            check_same_about(text, binary, NULL, bindings[i]);
            check_same_about(text, binary, bindings[i], NULL);
            check_same_members(text, binary, bindings[i]);
        }
        // An address nothing was written at answers nothing, as before.
        LHAT_CHECK_EQ_INT(lhat_unit_annotation_count(binary, "Nope", "hp"), 0);
        LHAT_CHECK_EQ_INT(lhat_unit_member_count(binary, "x"), 0);
        LHAT_CHECK(lhat_unit_member(binary, "Thing", 99).name == NULL,
                   "an index past the end is nobody");
    }
    lhat_program_dispose(&from_binary);
    lhat_program_dispose(&from_text);
    lhat_free(bytes);
}

// 05 の 8.8改: a wrapper over a wide host type. The export descriptor names
// the host type by its tag rather than copying its members, so the bytes do
// not grow with the host's API -- which is what took a 1.4KB unit to 160KB.
static void test_host_wrapper_size(void)
{
    LHAT_TEST("a wrapper's descriptor holds its own members and a tag");
    {
        Disk disk;
        memset(&disk, 0, sizeof disk);
        disk_text(&disk, "main.lh",
                  "import^ k\n"
                  "public^ let^ Wrap = def^{\n"
                  "  self^{ abstract^ h : k.T },\n"
                  "  delegate^ self^.h\n"
                  "}\n");
        LhatProgram program;
        lhat_program_init(&program, true, disk_load, &disk);
        const LhatHostDataTag *tag =
            lhat_register_hostdata_type(&program, "k", "T");
        LHAT_CHECK(tag != NULL, "hostdata registered");
        for (int i = 0; i < 64; i++) {
            char name[16];
            snprintf(name, sizeof name, "m%d", i);
            LHAT_CHECK(lhat_register_member(
                           &program, "k", "T", name,
                           "f^self^, number^, string^ -> number^|nil^;",
                           host_noop, NULL),
                       "member %s", name);
        }
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program) &&
                       lhat_program_compile(&program),
                   "built");
        const LhatRuntimeType *wrap =
            root != NULL ? lhat_unit_export_type(root, "Wrap") : NULL;
        LHAT_CHECK(wrap != NULL && wrap->kind == LHAT_TYPE_RT_TABLE &&
                       wrap->instance != NULL,
                   "the export is a definition");
        if (wrap != NULL && wrap->instance != NULL) {
            LHAT_CHECK_EQ_INT(wrap->instance->member_count, 1);
            LHAT_CHECK(wrap->instance->hostdata_tag == tag,
                       "and its instances hold a k.T");
            LHAT_CHECK(wrap->hostdata_tag == NULL,
                       "the definition itself holds none");
        }
        uint8_t *bytes = NULL;
        size_t length = 0;
        LHAT_CHECK(root != NULL &&
                       lhat_unit_write_binary(root, false, &bytes, &length),
                   "wrote");
        LHAT_CHECK(length < 2048,
                   "%zu bytes for a wrapper over 64 methods", length);
        lhat_free(bytes);
        lhat_program_dispose(&program);
    }
}

int main(void)
{
    test_roundtrip();
    test_refusals();
    test_units();
    test_traceback();
    test_host_references();
    test_exports();
    test_signatures();
    test_reflection();
    test_host_wrapper_size();
    return lhat_test_report("test_serialize");
}
