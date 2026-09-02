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

int main(void)
{
    test_roundtrip();
    test_refusals();
    test_units();
    test_traceback();
    test_host_references();
    test_exports();
    return lhat_test_report("test_serialize");
}
