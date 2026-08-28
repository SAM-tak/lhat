// L^ (lhat) -- nothing a program and a machine took is still held when both
// have gone.
//
// This has a binary of its own because of when it has to run rather than what
// it covers: lhat_set_allocator refuses once anything has been taken
// (port.h), so the counting allocator has to be registered before the first
// allocation in the process.
//
// What it pins is the plain host lifecycle -- register, check, compile, run,
// and then let go -- which is what an embedder does on every reload. A block
// still held afterwards is a leak per reload, and those add up in the one
// place a leak matters most: an editor left open all day.

#include <stdlib.h>
#include <string.h>

#include "lhat/port.h"
#include "lhat/program.h"
#include "lhat/value.h"
#include "lhat/vm.h"
#include "testutil.h"

#if LHAT_LEAK_WITH_STDLIB
#include "debug.h"
#include "io.h"
#include "load.h"
#include "math.h"
#include "mathvector3.h"
#include "random.h"
#include "regex.h"
#endif

// Every block carries its size, so realloc can take the old one off the
// count and free needs no table beside it. 16 keeps what follows aligned for
// anything the core stores in it.
#define HEADER 16

static size_t live_blocks;
static size_t live_bytes;
static size_t taken_blocks;

static void *counted_alloc(void *context, size_t size)
{
    (void)context;
    char *block = (char *)malloc(size + HEADER);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &size, sizeof size);
    live_blocks++;
    taken_blocks++;
    live_bytes += size;
    return block + HEADER;
}

static void *counted_calloc(void *context, size_t count, size_t size)
{
    void *block = counted_alloc(context, count * size);
    if (block != NULL) {
        memset(block, 0, count * size);
    }
    return block;
}

static void counted_free(void *context, void *pointer)
{
    (void)context;
    if (pointer == NULL) {
        return;
    }
    char *block = (char *)pointer - HEADER;
    size_t was = 0;
    memcpy(&was, block, sizeof was);
    live_blocks--;
    live_bytes -= was;
    free(block);
}

static void *counted_realloc(void *context, void *pointer, size_t size)
{
    if (pointer == NULL) {
        return counted_alloc(context, size);
    }
    char *block = (char *)pointer - HEADER;
    size_t was = 0;
    memcpy(&was, block, sizeof was);
    char *grown = (char *)realloc(block, size + HEADER);
    if (grown == NULL) {
        return NULL;
    }
    memcpy(grown, &size, sizeof size);
    live_bytes += size - was;
    taken_blocks++;
    return grown + HEADER;
}

typedef struct {
    const char *path;
    const char *text;
} File;

// Not const: a round rewrites the library so that the invalidation in it
// really retires something (an unchanged text retires nothing, 05 の 5.7).
static File files[] = {
    {"lib.lh",
     "module^ ns.lib\n"
     "public^ let^ answer = f^ -> number^ { return^ 41 }\n"
     "public^ let^ table = { a := 1, b := \"two\" }\n"},
    {"main.lh",
     "require^ \"lib.lh\"\n"
     "errordef^ Trouble { Late, Lost }\n"
     "var^ kept = { }\n"
     "for^ i from^ 1 to^ 20 { kept[i] := { n := i } }\n"
     "var^ gen = p^ {\n"
     "  do^{\n"
     "    yield^ 1\n"
     "  finally^:\n"
     "    var^ t = { done := true^ }\n"
     "  }\n"
     "}\n"
     "var^ running = gen()\n"
     "running.start()\n"
     "return^ ns.lib.answer() + 1\n"},
};

static char *load(void *context, const char *path, size_t *length)
{
    (void)context;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (strcmp(files[i].path, path) != 0) {
            continue;
        }
        // 05 の 5.1: the program frees this with lhat_free, so it has to
        // come from lhat_alloc -- which under this test is the counting
        // allocator, and pairing it with the C library's malloc would be
        // reading a header that was never written.
        size_t size = strlen(files[i].text);
        char *copy = (char *)lhat_alloc(size + 1);
        if (copy != NULL) {
            memcpy(copy, files[i].text, size + 1);
            *length = size;
        }
        return copy;
    }
    return NULL;
}

// 05 の 8.8: something for a registration to have made, so what a
// registration owns is counted too.
typedef struct {
    int n;
} Held;

static Held held = {7};

static void held_read(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    answers[0] = lhat_integer(held.n);
    *answer_count = 1;
    return;
}

static void held_make(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    lhat_machine_make_hostdata(machine, (const LhatHostDataTag *)context,
                               &held, &out);
    answers[0] = out;
    *answer_count = 1;
    return;
}

// One whole turn of what a host does: build it, run it, let it go.
static void one_round(void)
{
    files[0].text =
        "module^ ns.lib\n"
        "public^ let^ answer = f^ -> number^ { return^ 41 }\n"
        "public^ let^ table = { a := 1, b := \"two\" }\n";
    LhatProgram *program = lhat_program_new(true, load, NULL);
    LHAT_CHECK(program != NULL, "the program was made");
    if (program == NULL) {
        return;
    }

    const LhatHostDataTag *tag =
        lhat_register_hostdata_type(program, "store", "Held");
    lhat_register_member(program, "store", "Held", "read",
                         "f^self^ -> number^;", held_read, NULL);
    lhat_register_func(program, "store", "make", "f^ -> store.Held;",
                       held_make, (void *)tag);
    static const char *const variants[] = {"Torn", "Bent"};
    lhat_register_error_kind(program, "store", "Broken", variants, 2, NULL,
                             NULL);

    const LhatUnit *root = lhat_program_check(program, "main.lh");
    LHAT_CHECK(root != NULL && !lhat_program_has_errors(program),
               "the program checked");
    LHAT_CHECK(lhat_program_compile(program), "and compiled");

    LhatMachine *machine = lhat_machine_new();
    LHAT_CHECK(machine != NULL && lhat_program_install(program, machine),
               "the registrations went on to a machine");
    if (machine != NULL && root != NULL && lhat_unit_proto(root) != NULL) {
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);

        // 05 の 5.7: one unit changes and is read again, so what an
        // invalidation retires is counted too -- including the objects a
        // discarded body's constants named, which the program keeps.
        files[0].text =
            "module^ ns.lib\n"
            "public^ let^ answer = f^ -> number^ { return^ 41 }\n"
            "public^ let^ table = { a := 2, b := \"three\" }\n";
        LHAT_CHECK(lhat_program_invalidate(program, "lib.lh") > 0,
                   "the library retired");
        LHAT_CHECK(lhat_program_check(program, "main.lh") != NULL &&
                       lhat_program_compile(program),
                   "and both units were made again");
        lhat_machine_forget_unit(machine, "ns.lib");
        lhat_machine_collectgarbage(machine);
        LHAT_CHECK_EQ_INT(lhat_machine_pending_disposals(machine), 0);
        lhat_program_discard_retired(program);
    }

    lhat_machine_dispose(machine);
    lhat_program_free(program);
}

int main(void)
{
    LhatAllocator counting;
    counting.alloc = counted_alloc;
    counting.calloc = counted_calloc;
    counting.realloc = counted_realloc;
    counting.free = counted_free;
    counting.context = NULL;

    LHAT_TEST("the counting allocator takes, before anything else has");
    LHAT_CHECK(lhat_set_allocator(&counting), "registered");

    // 05 の 8.7: what the first round leaves behind is what the process
    // keeps, not what a round leaks -- the identities a declaration makes
    // belong to the process (registry.h) and are made once however many
    // programs declare them. So the first round sets the mark, and what
    // matters is that every round after it comes back to that same mark.
    // That is the difference between a one-time cost and a cost per reload.
    LHAT_TEST("a program and a machine give back everything but the declarations");
    one_round();
    LHAT_CHECK(taken_blocks > 100, "the round really allocated: %zu",
               taken_blocks);
    size_t declared_blocks = live_blocks;
    size_t declared_bytes = live_bytes;
    LHAT_CHECK(declared_blocks > 0,
               "the declarations are what is left: %zu blocks, %zu bytes",
               declared_blocks, declared_bytes);

    LHAT_TEST("and every round after it comes back to the same mark");
    one_round();
    LHAT_CHECK_EQ_INT(live_blocks, declared_blocks);
    LHAT_CHECK_EQ_INT(live_bytes, declared_bytes);
    one_round();
    LHAT_CHECK_EQ_INT(live_blocks, declared_blocks);
    LHAT_CHECK_EQ_INT(live_bytes, declared_bytes);

    LHAT_TEST("and the declarations themselves go when the process is done");
    lhat_registry_dispose();
    LHAT_CHECK_EQ_INT(live_blocks, 0);
    LHAT_CHECK_EQ_INT(live_bytes, 0);

#if LHAT_LEAK_WITH_STDLIB
    // The sample library, the same way round: the modules that hold only
    // identities are static now, and what a round of registering costs the
    // second time is nothing.
    LHAT_TEST("registering the sample library costs the same twice");
    {
        for (int round = 0; round < 3; round++) {
            LhatProgram *program = lhat_program_new(true, load, NULL);
            LHAT_CHECK(program != NULL, "the program was made");
            if (program == NULL) {
                break;
            }
            LHAT_CHECK(lhatstdlib_io_register(program) &&
                           lhatstdlib_random_register(program) &&
                           lhatstdlib_regex_register(program) &&
                           lhatstdlib_load_register(program) &&
                           lhatstdlib_math_register(program) &&
                           lhatstdlib_mathvector3_register(program) &&
                           lhatstdlib_debug_register(program),
                       "the library registered");
            lhat_program_free(program);
            if (round == 0) {
                declared_blocks = live_blocks;
                declared_bytes = live_bytes;
            } else {
                LHAT_CHECK_EQ_INT(live_blocks, declared_blocks);
                LHAT_CHECK_EQ_INT(live_bytes, declared_bytes);
            }
        }
        lhat_registry_dispose();
        LHAT_CHECK_EQ_INT(live_blocks, 0);
        LHAT_CHECK_EQ_INT(live_bytes, 0);
    }
#endif

    return lhat_test_report("test_leak");
}
