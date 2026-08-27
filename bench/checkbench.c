// L^ (lhat) -- what checking a wrapper over a wide host type costs.
//
// A Godot binding writes one def^ per engine class and hands it the class's
// members with delegate^ (02 の 14.7改2):
//
//     public^let^ Sprite2D = def^{
//         self^{ abstract^gdobj : godot.Sprite2D },
//         override^new = f^obj:godot.Sprite2D { self^{ gdobj = obj } },
//         delegate^ self^.gdobj
//     }
//
// godot.Sprite2D carries a few hundred members, most of them its bases'
// (05 の 8.8改's flatten). 03 の 1.1 puts the check on the editor's save, so
// what this costs is what a writer waits for.
//
// WHAT THIS ANSWERS. Three numbers, apart:
//
//     register   -- declaring the tree and its members
//     check      -- M wrappers naming the host type, WITHOUT delegate^
//     delegate   -- the same M wrappers WITH it
//
// N (members per class) and M (wrappers) are varied so the shape shows: a
// cost linear in N is a walk, one quadratic in N is a walk inside a walk.
// The second is what a list of members answers by scanning.
//
// This is not a test. A claim about time is not a thing that passes or
// fails, so it prints and asserts nothing. Build with -DLHAT_BUILD_BENCH=ON
// and read the numbers; they belong to the machine that produced them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"
#include "port/thread.h"

// The deepest the tree goes, and the widest a class gets. The Godot shape is
// Object -> Node -> CanvasItem -> Node2D -> Sprite2D, so five is the real
// depth; the width is what varies between them.
#define DEPTH 5
#define MAX_MEMBERS 512
#define MAX_WRAPPERS 64
#define MAX_LEAVES 1024

// ---------------------------------------------------------------------------
// The disk: one file, written into a buffer big enough for the widest case.

typedef struct {
    const char *path;
    char *text;
} File;

// Two of them: the wrappers in one unit and what composes onto them in
// another, which is the shape a binding has -- a generated lhat/Godot.lh
// and the script that require^s it (05 の 5.3).
typedef struct {
    File files[2];
    size_t count;
} Disk;

static char *disk_load(void *context, const char *path, size_t *length)
{
    const Disk *disk = (const Disk *)context;
    const File *file = NULL;
    for (size_t i = 0; i < disk->count; i++) {
        if (strcmp(disk->files[i].path, path) == 0) {
            file = &disk->files[i];
            break;
        }
    }
    if (file == NULL) {
        return NULL;
    }
    size_t n = strlen(file->text);
    char *copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, file->text, n + 1);
    *length = n;
    return copy;
}

// ---------------------------------------------------------------------------
// bench.C0 .. bench.C4, each under the one before it, each declaring N
// members of its own. C4 therefore carries N * DEPTH once 8.8改 has run.

static const char *level_name(size_t level)
{
    static char names[DEPTH][8];
    snprintf(names[level], sizeof names[level], "C%zu", (size_t)level);
    return names[level];
}

static LhatValue answer_one(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    return lhat_integer(1);
}

// Declares the tree and gives every level `per_level` members. Answers false
// only out of memory or on a registration the program refused.
// The leaves, all under the deepest of the chain -- what makes the tree a
// tree rather than a line. An engine has hundreds of classes and 8.8改
// flattens each one against its base, so how many there are is as much a
// part of the cost as how wide one is.
static const char *leaf_name(size_t leaf)
{
    static char name[16];
    snprintf(name, sizeof name, "L%zu", leaf);
    return name;
}

static bool register_tree(LhatProgram *program, size_t per_level,
                          size_t leaves)
{
    for (size_t level = 0; level < DEPTH; level++) {
        const LhatHostDataTag *tag =
            level == 0
                ? lhat_register_hostdata_type(program, "bench",
                                              level_name(level))
                : lhat_register_hostdata_subtype(program, "bench",
                                                 level_name(level), "bench",
                                                 level_name(level - 1));
        if (tag == NULL) {
            return false;
        }
    }
    // The members after the whole tree is declared, which is what program.h
    // advises for types that name each other -- and what makes 8.8改's
    // flatten wait for the first check.
    for (size_t level = 0; level < DEPTH; level++) {
        for (size_t i = 0; i < per_level; i++) {
            char name[32];
            snprintf(name, sizeof name, "m%zu_%zu", (size_t)level,
                     (size_t)i);
            if (!lhat_register_member(program, "bench", level_name(level),
                                      name, "f^self^ -> number^;", answer_one,
                                      NULL)) {
                return false;
            }
        }
    }
    for (size_t leaf = 0; leaf < leaves; leaf++) {
        char under[16];
        snprintf(under, sizeof under, "%s", level_name(DEPTH - 1));
        const char *name = leaf_name(leaf);
        char kept[16];
        snprintf(kept, sizeof kept, "%s", name);
        if (lhat_register_hostdata_subtype(program, "bench", kept,
                                           "bench", under) == NULL) {
            return false;
        }
        for (size_t i = 0; i < per_level; i++) {
            char member[32];
            snprintf(member, sizeof member, "k%zu_%zu", leaf, i);
            if (!lhat_register_member(program, "bench", kept, member,
                                      "f^self^ -> number^;",
                                      answer_one, NULL)) {
                return false;
            }
        }
    }
    char signature[64];
    snprintf(signature, sizeof signature, "f^ -> bench.%s;",
             level_name(DEPTH - 1));
    return lhat_register_func(program, "bench", "make", signature, answer_one,
                              NULL);
}

// ---------------------------------------------------------------------------
// The two sources. `lib.lh` is what a generator writes: M wrappers over the
// deepest class, with or without delegate^. `main.lh` is the script, which
// either just names them or composes onto each -- the shape a Godot script
// has, `Godot.Sprite2D .. def^{ … }`.

static void write_library(char *out, size_t capacity, size_t wrappers,
                          bool delegating)
{
    const char *deep = level_name(DEPTH - 1);
    size_t used = 0;
    used += (size_t)snprintf(out + used, capacity - used,
                             "module^ bench.lib\nimport^ bench\n");
    for (size_t i = 0; i < wrappers; i++) {
        used += (size_t)snprintf(
            out + used, capacity - used,
            "public^ let^ W%zu = def^{\n"
            "    self^{ abstract^gdobj : bench.%s },\n"
            "    override^new = f^obj:bench.%s { self^{ gdobj = obj } },\n"
            "    tag%zu = \"W%zu\",\n"
            "%s"
            "}\n",
            (size_t)i, deep, deep, (size_t)i, (size_t)i,
            delegating ? "    delegate^ self^.gdobj,\n" : "");
    }
}

static void write_script(char *out, size_t capacity, size_t wrappers,
                         bool composing)
{
    size_t used = 0;
    used += (size_t)snprintf(out + used, capacity - used,
                             "let^ lib = require^ \"lib.lh\"\n");
    for (size_t i = 0; i < wrappers; i++) {
        if (composing) {
            used += (size_t)snprintf(
                out + used, capacity - used,
                "let^ S%zu = lib.W%zu .. def^{\n"
                "    self^{ },\n"
                "    own%zu = f^self^ -> number^ { return^ %zu },\n"
                "}\n",
                (size_t)i, (size_t)i, (size_t)i, (size_t)i);
        } else {
            used += (size_t)snprintf(out + used, capacity - used,
                                     "let^ S%zu = lib.W%zu\n", (size_t)i,
                                     (size_t)i);
        }
    }
}

// ---------------------------------------------------------------------------
// One measurement: build a program, register, check. Answers milliseconds
// for each of the two halves through the out parameters.
//
// ROUNDS of them, because lhat_now_ms is the wall clock the machine offers
// and on Windows that ticks about every 16 ms -- one check of anything small
// reads as zero. Each round is its own program: the registry keeps the
// identities (05 の 8.7) but the types, the units and the arena belong to the
// program, so what is timed is a save's worth of work rather than a second
// pass over warm structures.

#define ROUNDS 40

static bool time_one(size_t per_level, size_t leaves, size_t wrappers,
                     bool delegating, bool composing,
                     double *registering, double *checking,
                     double *installing)
{
    static char library[MAX_WRAPPERS * 512 + 64];
    static char script[MAX_WRAPPERS * 256 + 64];
    write_library(library, sizeof library, wrappers, delegating);
    write_script(script, sizeof script, wrappers, composing);
    Disk disk = {{{"lib.lh", library}, {"main.lh", script}}, 2};

    double spent_registering = 0.0;
    double spent_checking = 0.0;
    double spent_installing = 0.0;
    for (size_t round = 0; round < ROUNDS; round++) {
        LhatProgram *program = lhat_program_new(true, disk_load, &disk);
        if (program == NULL) {
            return false;
        }

        double started = lhat_now_ms();
        if (!register_tree(program, per_level, leaves)) {
            lhat_program_free(program);
            return false;
        }
        spent_registering += lhat_now_ms() - started;

        // 8.8改's flatten happens inside the first check, which is where it
        // belongs: this measures what an editor's save waits for.
        started = lhat_now_ms();
        const LhatUnit *root = lhat_program_check(program, "main.lh");
        spent_checking += lhat_now_ms() - started;

        if (root == NULL || !lhat_unit_ok(root)) {
            fprintf(stderr, "checkbench: the program did not check\n");
            lhat_program_free(program);
            return false;
        }

        // 05 の 8.7: what a host does after the check -- compile, make a
        // machine, and put the registrations into it. A binding that rebuilds
        // its world whenever a script is loaded pays this as often as it pays
        // the check, so it is timed apart rather than left in the noise.
        if (!lhat_program_compile(program)) {
            fprintf(stderr, "checkbench: the program did not compile\n");
            lhat_program_free(program);
            return false;
        }
        LhatMachine *machine = lhat_machine_new();
        if (machine == NULL) {
            lhat_program_free(program);
            return false;
        }
        started = lhat_now_ms();
        bool put = lhat_program_install(program, machine);
        spent_installing += lhat_now_ms() - started;
        lhat_machine_dispose(machine);
        if (!put) {
            fprintf(stderr, "checkbench: the install failed\n");
            lhat_program_free(program);
            return false;
        }
        lhat_program_free(program);
    }
    *registering = spent_registering / ROUNDS;
    *checking = spent_checking / ROUNDS;
    *installing = spent_installing / ROUNDS;
    return true;
}

// The registry is the process's (05 の 8.7), and a second declaration of a
// name has to agree with the first -- so a run that varied N would be asking
// for one name with two member lists. Each width is therefore its own
// process, which is what `argv[1]` selects.
//
// Four rows per wrapper count, since what a binding does is two things and
// either may be the cost: the generated unit writes delegate^, and the
// script composes onto what it published (05 の 5.3 across units).
static void run_row(size_t per_level, size_t leaves, size_t wrappers,
                    bool delegating, bool composing)
{
    size_t total = per_level * DEPTH;
    double reg = 0.0;
    double timed = 0.0;
    double put = 0.0;
    if (!time_one(per_level, leaves, wrappers, delegating, composing, &reg,
                  &timed, &put)) {
        return;
    }
    (void)total;
    printf("  %2zu wrapper(s)  %-8s %-7s  register %7.1f ms  "
           "check %7.1f ms  install %9.1f ms\n",
           (size_t)wrappers, delegating ? "delegate" : "plain",
           composing ? "compose" : "name", reg, timed, put);
}

int main(int argc, char **argv)
{
    // One width per process, for the reason above. Without an argument this
    // prints how to ask for one rather than guessing.
    if (argc < 2) {
        printf("usage: lhat_checkbench <members-per-class>\n");
        printf("  runs 1 and 6 wrappers over a %d-deep host tree,\n", DEPTH);
        printf("  with and without delegate^, named and composed onto\n");
        return 0;
    }
    long asked = strtol(argv[1], NULL, 10);
    if (asked <= 0 || asked > MAX_MEMBERS) {
        printf("checkbench: members-per-class out of range\n");
        return 1;
    }
    size_t per_level = (size_t)asked;
    long wide = argc > 2 ? strtol(argv[2], NULL, 10) : 0;
    size_t leaves = wide > 0 && wide <= MAX_LEAVES ? (size_t)wide : 0;
    printf("members per class: %zu (%zu after the flatten, depth %d), "
           "%zu leaves\n",
           (size_t)per_level, (size_t)(per_level * DEPTH), DEPTH,
           (size_t)leaves);

    static const size_t counts[] = {1, 6};
    for (size_t w = 0; w < sizeof counts / sizeof counts[0]; w++) {
        run_row(per_level, leaves, counts[w], false, false);
        run_row(per_level, leaves, counts[w], true, false);
        run_row(per_level, leaves, counts[w], false, true);
        run_row(per_level, leaves, counts[w], true, true);
    }
    return 0;
}
