// L^ (lhat) -- tests for 05 の 8.8改: a host type declared under another.
//
// A host whose own model is a class tree writes the tree here instead of
// flattening it into one opaque type. 8.8's nominal rule is not loosened --
// what changes is that a declaration may say what it is under, and the host
// promises the pointer may be read as the base's.
//
// Two things are pinned above all. The first is that the two sides agree:
// the checker walks the tag chain and so does the machine, because 03 の 4.2
// does not let what runs depend on whether checking ran. The second is that
// a derived type inherits BOTH halves of a dispose^ -- the member the checker
// reads for 12.5, and the tag field the collector reads. Inheriting only the
// member would make with^ check as disposable and then release nothing:
// silent, which is worse than refused.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "machine.h"
#include "program_internal.h"
#include "registry.h"
#include "fixture.h"
#include "lhat/port.h"
#include "lhat/value.h"
#include "lhat/vm.h"

// ---------------------------------------------------------------------------
// The program fixture test_host_coroutine.c keeps: a disk of literal files.

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

static void program_with(LhatProgram *program, Disk *disk, const File *files,
                         size_t count)
{
    disk->files = files;
    disk->count = count;
    lhat_program_init(program, true, disk_load, disk);
}

static LhatRunResult run_program(LhatProgram *program)
{
    LhatRunResult failed;
    memset(&failed, 0, sizeof failed);
    failed.status = LHAT_RUN_TYPE_ERROR;
    const LhatUnit *root = lhat_program_check(program, "main.lh");
    LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0,
               "the program checked: %s",
               root != NULL && root->checked.diagnostic_count > 0
                   ? lhat_check_error_message(root->checked.diagnostics[0].code)
                   : "(nothing)");
    if (!lhat_program_compile(program) || root == NULL) {
        LHAT_CHECK(false, "the program compiled");
        return failed;
    }
    LhatMachine *machine = lhat_machine_new();
    lhat_program_install(program, machine);
    LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
    lhat_machine_dispose(machine);
    return ran;
}

static bool checked_clean(LhatProgram *program)
{
    const LhatUnit *root = lhat_program_check(program, "main.lh");
    return root != NULL && root->checked.diagnostic_count == 0;
}

// ---------------------------------------------------------------------------
// scene.Node <- scene.Node2D <- scene.Sprite2D, the shape a class tree has.
//
// One C struct behind all three: single inheritance with the base first is
// what the host promises when it declares the relation, and this is that
// promise made real in the smallest way.

typedef struct {
    int64_t id;       // scene.Node's half
    int64_t x;        // Node2D adds this
    int64_t frame;    // Sprite2D adds this
} SceneNode;

static const LhatHostDataTag *node_tag;
static const LhatHostDataTag *node2d_tag;
static const LhatHostDataTag *sprite_tag;
static const LhatHostDataTag *other_tag;

static SceneNode the_node;      // one per test
static int node_releases;       // how many times the base's dispose^ ran
static int sprite_releases;     // and the derived one's

static void node_id(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count,
                    LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    // Asked for the BASE's tag off a value that may be of any type under it.
    // 8.8改 is what makes this answer rather than NULL, and it is the whole
    // of what the host gets out of declaring the relation.
    SceneNode *self = (SceneNode *)lhat_hostdata_pointer(arguments[0], node_tag);
    answers[0] = self != NULL ? lhat_integer(self->id) : lhat_nil();
    *answer_count = 1;
}

static void node2d_x(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    SceneNode *self =
        (SceneNode *)lhat_hostdata_pointer(arguments[0], node2d_tag);
    answers[0] = self != NULL ? lhat_integer(self->x) : lhat_nil();
    *answer_count = 1;
}

// The same name the base declares, on the leaf. 8.8改 says the nearest
// declaration answers, and this is what says which one ran.
static void sprite_id(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    answers[0] = lhat_integer(7);
    *answer_count = 1;
}

static void node_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    node_releases++;
}

static void sprite_dispose(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    sprite_releases++;
}

// Makes a value of whichever type the registration bound this to.
static void make_of(LhatMachine *machine, void *context,
                    const LhatValue *arguments, size_t count,
                    LhatValue *answers, int *answer_count)
{
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    lhat_machine_make_hostdata(machine, (const LhatHostDataTag *)context,
                               &the_node, &out);
    answers[0] = out;
    *answer_count = 1;
}

// The tree, with members registered AFTER every type and every relation --
// program.h:503's own advice for types that name each other, and the order
// that makes flattening at registration time useless.
static bool register_scene(LhatProgram *program, bool with_dispose)
{
    node_tag = lhat_register_hostdata_type(program, "scene", "Node");
    node2d_tag = lhat_register_hostdata_subtype(program, "scene", "Node2D",
                                                "scene", "Node");
    sprite_tag = lhat_register_hostdata_subtype(program, "scene", "Sprite2D",
                                                "scene", "Node2D");
    other_tag = lhat_register_hostdata_type(program, "scene", "Resource");
    if (node_tag == NULL || node2d_tag == NULL || sprite_tag == NULL ||
        other_tag == NULL) {
        return false;
    }

    bool ok = lhat_register_member(program, "scene", "Node", "id",
                                   "f^self^ -> number^;", node_id, NULL) &&
              lhat_register_member(program, "scene", "Node2D", "x",
                                   "f^self^ -> number^;", node2d_x, NULL) &&
              lhat_register_func(program, "scene", "makeSprite",
                                 "f^ -> scene.Sprite2D;", make_of,
                                 (void *)sprite_tag) &&
              lhat_register_func(program, "scene", "makeResource",
                                 "f^ -> scene.Resource;", make_of,
                                 (void *)other_tag);
    if (ok && with_dispose) {
        ok = lhat_register_member(program, "scene", "Node", "dispose",
                                  "p^self^;", node_dispose, NULL);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// The relation itself

static void test_the_relation(void)
{
    LhatProgram program;
    Disk disk;

    // The point of the whole thing: a boundary that can say what it takes.
    LHAT_TEST("a derived value stands where the base is asked for");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ takesNode = f^ n:scene.Node -> number^ { n.id() }\n"
             "return^ takesNode(scene.makeSprite())\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 7;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("and the base does not stand where the derived is asked for");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ takesSprite = f^ s:scene.Sprite2D -> number^ { s.id() }\n"
             "let^ n : scene.Node = scene.makeSprite()\n"
             "return^ takesSprite(n)\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LHAT_CHECK(!checked_clean(&program), "the narrowing is not implied");
        lhat_program_dispose(&program);
    }

    // The regression that matters: two types with no relation are what 8.8
    // was written to keep apart, and they stay apart.
    LHAT_TEST("two unrelated host types are still separate");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ takesNode = f^ n:scene.Node -> number^ { n.id() }\n"
             "return^ takesNode(scene.makeResource())\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LHAT_CHECK(!checked_clean(&program), "a Resource is not a Node");
        lhat_program_dispose(&program);
    }

    // 8.8改 walks the whole chain, not one step of it.
    LHAT_TEST("the chain is walked to its root");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ takesNode = f^ n:scene.Node -> number^ { n.id() }\n"
             // Sprite2D <- Node2D <- Node: two steps up.
             "return^ takesNode(scene.makeSprite())\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 3;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// Members, and the order they may be registered in

static void test_inherited_members(void)
{
    LhatProgram program;
    Disk disk;

    // register_scene puts every member on after every type and relation --
    // program.h:503's order, and the reason the flattening happens when
    // registration closes rather than at the subtype call.
    LHAT_TEST("a derived type has its base's members, registered in any order");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ s = scene.makeSprite()\n"
             // id() is Node's, x() is Node2D's; s is a Sprite2D.
             "return^ s.id() * 10 + s.x()\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 4;
        the_node.x = 2;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// fits^, which is where the two sides have to agree (03 の 4.2)

static void test_fits(void)
{
    LhatProgram program;
    Disk disk;

    // 8.8改 with 14.7: the nearest declaration of a name answers. The
    // members table a derived type carries is LINKED to its base's rather
    // than holding a copy of it, so which one is met first is the walk's
    // answer and not a comparison somebody had to write.
    LHAT_TEST("a name declared twice answers with the nearest");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ s = scene.makeSprite()\n"
             "let^ n : scene.Node = scene.makeSprite()\n"
             "return^ s.id() * 10 + n.id()\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        // The same name the base registered, on the leaf of the chain.
        LHAT_CHECK(lhat_register_member(&program, "scene", "Sprite2D", "id",
                                        "f^self^ -> number^;", sprite_id,
                                        NULL),
                   "the derived one registered");
        the_node.id = 3;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // 7 from the derived member both times: the annotation says Node,
        // but 8.8 makes the VALUE what decides, and the value is a Sprite2D.
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 77);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("fits^ answers the chain at run time as the checker reads it");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ s = scene.makeSprite()\n"
             "var^ n = 0\n"
             "if^ s fits^ scene.Node { n := n + 1 }\n"
             "if^ s fits^ scene.Node2D { n := n + 2 }\n"
             "if^ s fits^ scene.Sprite2D { n := n + 4 }\n"
             "return^ n\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 7);
        lhat_program_dispose(&program);
    }

    // 02 の 13.11: fits^ asks the question the checker asks of an annotation,
    // so the two have to answer alike. A host value's members are its type's,
    // not its own -- a reading that lives in one place and that the run time
    // used to skip, answering no to every structure while the checker was
    // taking the same value where the same type was written.
    LHAT_TEST("a host value answers a structure naming what it registered");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ s = scene.makeSprite()\n"
             "let^ asked : t^{ id : f^self^ -> number^; } = s\n"
             "var^ n = 0\n"
             "if^ s fits^ t^{ id : f^self^ -> number^; } { n := n + 1 }\n"
             "if^ s fits^ t^{ x : f^self^ -> number^; } { n := n + 2 }\n"
             "if^ s fits^ t^{ nothing : f^self^ -> number^; } { n := n + 4 }\n"
             "return^ n + asked.id() * 0\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3);
        lhat_program_dispose(&program);
    }

    // 05 の 8.7 with 02 の 14.12: a registered C function is a subroutine of
    // the language, and so is a group -- both are written f^/p^ where the
    // checker reads them, so both have to stand where one is asked for.
    LHAT_TEST("a registered function is a subroutine to fits^");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ t = { make = scene.makeSprite }\n"
             "if^ t fits^ t^{ make : f^ -> scene.Sprite2D; } { return^ 1 }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
        lhat_program_dispose(&program);
    }

    // 02 の 14.7改2: the delegate walk and the reading above are the same
    // reading, so what shows through as a member shows through to fits^ too
    // -- while the host type itself stays nominal (05 の 8.8), which is the
    // whole point of the distinction.
    LHAT_TEST("what a delegate shows answers a structure, not the host type");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ held = scene.makeSprite()\n"
             "let^ A = def^{ self^{ }, kept = held, delegate^ kept }\n"
             "let^ a = A.new()\n"
             "var^ n = 0\n"
             "if^ a fits^ t^{ id : f^self^ -> number^; } { n := n + 1 }\n"
             "if^ a fits^ scene.Sprite2D { n := n + 2 }\n"
             "return^ n\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 1);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("and answers false for a type off the chain");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ r = scene.makeResource()\n"
             "if^ r fits^ scene.Node { return^ 1 }\n"
             "return^ 0\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 0);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// dispose^, the half that does not go through the member table

static void test_inherited_dispose(void)
{
    LhatProgram program;
    Disk disk;

    // 12.5 reads a lifetime off whether a `dispose` member is there, and the
    // collector reads tag->release. A derived type given one and not the
    // other checks as disposable and then releases nothing -- so this asserts
    // the release actually ran, not that the program compiled.
    LHAT_TEST("with^ on a derived value runs the base's dispose^");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "with^ s = scene.makeSprite() { return^ s.id() }\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, true), "registered");
        the_node.id = 1;
        node_releases = 0;
        sprite_releases = 0;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(node_releases, 1);
        lhat_program_dispose(&program);
    }

    // And a derived type that registers its own is handed back that way --
    // the walk finds the nearest, which is what overriding means here.
    LHAT_TEST("a derived dispose^ is what runs for a derived value");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "with^ s = scene.makeSprite() { return^ s.id() }\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, true), "registered");
        LHAT_CHECK(lhat_register_member(&program, "scene", "Sprite2D",
                                        "dispose", "p^self^;", sprite_dispose,
                                        NULL),
                   "the derived dispose^ registered");
        the_node.id = 1;
        node_releases = 0;
        sprite_releases = 0;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(sprite_releases, 1);
        LHAT_CHECK_EQ_INT(node_releases, 0);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// 02 の 14.16 from C: lhat_value_type
//
// A host wrapping its own objects in a def^ has to know which of its types a
// wrapper takes. The truth is in the constructor's signature; writing the
// name out beside the wrapper is a copy of it, and nothing holds the two
// together. This is the reading that makes the copy needless.

// Reads the class name a def^'s `new` takes, the way a host would. NULL
// where the chain of readings gives out.
static const char *class_new_takes(LhatMachine *machine, LhatValue definition)
{
    if (!lhat_is_object_kind(definition, LHAT_OBJECT_TABLE)) {
        return NULL;
    }
    LhatValue key = lhat_nil();
    if (!lhat_machine_make_string(machine, "new", 3, &key)) {
        return NULL;
    }
    LhatValue ctor =
        lhat_table_get((const LhatTable *)lhat_as_object(definition), key);
    const LhatRuntimeType *signature = lhat_value_type(machine, ctor);
    if (signature == NULL || signature->kind != LHAT_TYPE_RT_SUBROUTINE ||
        signature->part_count != 1) {
        return NULL;
    }
    const LhatRuntimeType *takes = signature->parts[0];
    if (takes == NULL || takes->kind != LHAT_TYPE_RT_HOSTDATA ||
        takes->hostdata_tag == NULL) {
        return NULL;
    }
    return takes->hostdata_tag->name;
}

// One field of a table the program returned.
static LhatValue field_of(LhatMachine *machine, LhatValue table,
                          const char *name)
{
    LhatValue key = lhat_nil();
    if (!lhat_is_object_kind(table, LHAT_OBJECT_TABLE) ||
        !lhat_machine_make_string(machine, name, strlen(name), &key)) {
        return lhat_nil();
    }
    return lhat_table_get((const LhatTable *)lhat_as_object(table), key);
}

// What a public definition's instances are, off the export descriptor.
static const LhatRuntimeType *instance_type(const LhatUnit *unit,
                                            const char *name)
{
    const LhatRuntimeType *definition = lhat_unit_export_type(unit, name);
    return definition != NULL ? definition->instance : NULL;
}

static void test_value_type(void)
{
    LhatProgram program;
    Disk disk;

    // Two wrappers over the same host type: one saying what its new takes,
    // one leaving it to inference (03 の 3.4 settles it from the field the
    // body writes). A host cannot be asked to care which way it was written,
    // so both have to answer the same.
    static const File files[] = {
        {"main.lh",
             "import^ scene\n"
             "let^ Said = def^{\n"
             "    self^{ abstract^ gd : scene.Sprite2D },\n"
             "    override^new = f^ obj:scene.Sprite2D { self^{ gd = obj } },\n"
             "}\n"
             "let^ Bare = def^{\n"
             "    self^{ abstract^ gd : scene.Sprite2D },\n"
             "    override^new = f^ obj { self^{ gd = obj } },\n"
             "}\n"
             "return^ { said = Said, bare = Bare }\n"},
    };

    LHAT_TEST("14.16: a host reads what a def^'s new takes");
    {
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && !lhat_program_has_errors(&program),
                   "checked clean");
        LHAT_CHECK(lhat_program_compile(&program), "compiled");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(machine != NULL && lhat_program_install(&program, machine),
                   "installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);

        // The machine is kept alive on purpose: what lhat_value_type answers
        // is the machine's until it next runs, and this is a host reading it
        // in exactly that window.
        LhatValue key = lhat_nil();
        for (int which = 0; which < 2; which++) {
            const char *name = which == 0 ? "said" : "bare";
            LHAT_CHECK(
                lhat_machine_make_string(machine, name, strlen(name), &key),
                "the key was made");
            LhatValue held = lhat_table_get(
                (const LhatTable *)lhat_as_object(ran.value), key);
            const char *takes = class_new_takes(machine, held);
            LHAT_CHECK(takes != NULL, "the signature was read");
            if (takes != NULL) {
                LHAT_CHECK_EQ_STR(takes, strlen(takes), "Sprite2D");
            }
        }
        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// What the registration itself refuses

static void test_registration(void)
{
    LhatProgram program;
    Disk disk;
    static const File files[] = {{"main.lh", "return^ 1\n"}};

    LHAT_TEST("a base that is not registered is refused");
    {
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostdata_subtype(&program, "scene", "Node2D",
                                                  "scene", "NoSuchThing") ==
                       NULL,
                   "no such base");
        lhat_program_dispose(&program);
    }

    // 05 の 8.7改: a name stands for one declaration across the process, and
    // what it is under is part of that declaration.
    LHAT_TEST("the same name under a different base is refused");
    {
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostdata_type(&program, "two", "A") != NULL,
                   "A registered");
        LHAT_CHECK(lhat_register_hostdata_type(&program, "two", "B") != NULL,
                   "B registered");
        LHAT_CHECK(lhat_register_hostdata_subtype(&program, "two", "C", "two",
                                                  "A") != NULL,
                   "C is under A");
        lhat_program_dispose(&program);

        // A second program, saying something else about the same name.
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(lhat_register_hostdata_type(&program, "two", "A") != NULL,
                   "A again");
        LHAT_CHECK(lhat_register_hostdata_type(&program, "two", "B") != NULL,
                   "B again");
        LHAT_CHECK(lhat_register_hostdata_subtype(&program, "two", "C", "two",
                                                  "B") == NULL,
                   "C cannot be under B as well");
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a cycle is refused");
    {
        program_with(&program, &disk, files, 1);
        const LhatHostDataTag *x =
            lhat_register_hostdata_type(&program, "loop", "X");
        const LhatHostDataTag *y = lhat_register_hostdata_subtype(
            &program, "loop", "Y", "loop", "X");
        LHAT_CHECK(x != NULL && y != NULL, "X, and Y under it");
        // X under Y would close the ring, and every walk over the chain --
        // conformance, fits^, the release lookup -- would run for ever.
        LHAT_CHECK(!lhat_registry_set_hostdata_base(x, y),
                   "X cannot go under Y");
        lhat_program_dispose(&program);
    }
}

// 02 の 14.7改2 over a host type, which is the arrangement delegate^ was
// written for: an L^ wrapper holding what the host made, showing the host's
// members as its own.
//
// 05 の 8.8改's flatten is what makes the base's members come along, so this
// also says the two features meet.
static void test_delegate_to_host(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a wrapper delegating to a host value carries its members");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ Sprite = def^{\n"
             "    self^{ abstract^ gdobj : scene.Sprite2D },\n"
             "    override^new = f^ { self^{ gdobj = scene.makeSprite() } },\n"
             "    delegate^ self^.gdobj\n"
             "}\n"
             // id() is scene.Node's, reached through Sprite2D by 8.8改's
             // flatten and then through the delegation by 14.7改2.
             "return^ Sprite.new().id()\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 6;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 6);
        lhat_program_dispose(&program);
    }

    // 05 の 8.8: and the wrapper is still not one. A host type is where 11.3
    // gives way, so delegating to one lends its members and not its identity
    // -- the C behind scene.Node would otherwise be handed a table.
    LHAT_TEST("but the wrapper does not stand where the host type is asked");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "let^ Sprite = def^{\n"
             "    self^{ abstract^ gdobj : scene.Sprite2D },\n"
             "    override^new = f^ { self^{ gdobj = scene.makeSprite() } },\n"
             "    delegate^ self^.gdobj\n"
             "}\n"
             "let^ takes = f^ n:scene.Node -> number^ { n.id() }\n"
             "return^ takes(Sprite.new())\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LHAT_CHECK(!checked_clean(&program), "a wrapper is not a scene.Node");
        lhat_program_dispose(&program);
    }

    // 05 の 8.8改: the wrapper's descriptor names the host type it holds
    // rather than copying its members -- a class tree's whole API, twice --
    // and fits^ reaches the held value the way a lookup does: one delegate
    // step, the tag's base chain included.
    LHAT_TEST("the wrapper's type names the host type instead of copying it");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "module^ ns.main\n"
             "public^ let^ Sprite = def^{\n"
             "    self^{ abstract^ gdobj : scene.Sprite2D },\n"
             "    override^new = f^ { self^{ gdobj = scene.makeSprite() } },\n"
             "    delegate^ self^.gdobj\n"
             "}\n"
             // The same field, held without delegating.
             "public^ let^ Plain = def^{\n"
             "    self^{ abstract^ gdobj : scene.Sprite2D },\n"
             "    override^new = f^ { self^{ gdobj = scene.makeSprite() } },\n"
             "}\n"
             // Asks for the base of what it actually holds.
             "public^ let^ AsNode = def^{\n"
             "    self^{ abstract^ gdobj : scene.Node },\n"
             "    override^new = f^ { self^{ gdobj = scene.makeSprite() } },\n"
             "    delegate^ self^.gdobj\n"
             "}\n"
             "return^ {\n"
             "    spelt = typeof^(Sprite.new()).signature,\n"
             "    sprite = Sprite.new(), plain = Plain.new(),\n"
             "    raw = scene.makeSprite(),\n"
             "}\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && root->checked.diagnostic_count == 0 &&
                       lhat_program_compile(&program),
                   "built");
        LhatMachine *machine = lhat_machine_new();
        lhat_program_install(&program, machine);
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);

        LhatValue spelt = field_of(machine, ran.value, "spelt");
        LHAT_CHECK(lhat_is_object_kind(spelt, LHAT_OBJECT_STRING) &&
                       strcmp(((const LhatString *)lhat_as_object(spelt))->text,
                              "t^{ gdobj : scene.Sprite2D } & scene.Sprite2D") ==
                           0,
                   "typeof^ spells the held type after the members: %s",
                   lhat_is_object_kind(spelt, LHAT_OBJECT_STRING)
                       ? ((const LhatString *)lhat_as_object(spelt))->text
                       : "(not a string)");

        LhatValue sprite = field_of(machine, ran.value, "sprite");
        LhatValue plain = field_of(machine, ran.value, "plain");
        LhatValue raw = field_of(machine, ran.value, "raw");
        // 14.16: the run does not build a deep shape out of data, so the
        // descriptor holding the tag is the checker's -- what the export
        // answers, and what a compiled fits^ carries.
        const LhatRuntimeType *sprite_type = instance_type(root, "Sprite");
        const LhatRuntimeType *plain_type = instance_type(root, "Plain");
        const LhatRuntimeType *node_type = instance_type(root, "AsNode");
        LHAT_CHECK(sprite_type != NULL && plain_type != NULL &&
                       node_type != NULL,
                   "the exports answered");
        LHAT_CHECK(sprite_type != NULL &&
                       sprite_type->kind == LHAT_TYPE_RT_TABLE &&
                       sprite_type->member_count == 1 &&
                       sprite_type->hostdata_tag == sprite_tag,
                   "one member of its own, and the tag");
        LHAT_CHECK(plain_type != NULL && plain_type->hostdata_tag == NULL,
                   "holding without delegating names nothing");
        LHAT_CHECK(lhat_value_satisfies(sprite, sprite_type),
                   "a wrapper fits its own type");
        LHAT_CHECK(!lhat_value_satisfies(plain, sprite_type),
                   "the same field without the delegation does not");
        LHAT_CHECK(lhat_value_satisfies(sprite, plain_type),
                   "though the wrapper fits the plain shape");
        LHAT_CHECK(!lhat_value_satisfies(raw, sprite_type),
                   "and the host value itself lacks the member");
        LHAT_CHECK(lhat_value_satisfies(sprite, node_type),
                   "8.8改: a Sprite2D held is a Node held");
        LHAT_CHECK(!lhat_runtime_type_equal(sprite_type, plain_type),
                   "14.9: the tag is part of the shape");

        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }
}

// 05 の 8.8 with 8.8改: every value of a registered type answers through
// the type's own members table, which lhat_machine_make_hostdata reaches by
// walking L^.modules by name -- once per value. A walk that answered "not
// there" would put a fresh empty table in its place, and from then on new
// values would answer nothing while the ones already made kept working.
//
// Made and collected in a loop, because that is the shape a binding has: a
// wrapper per host object, thousands of them, with the collector running in
// between.
static void test_values_keep_their_members(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("every value of a type reaches the same members, value after value");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "var^ n = 0\n"
             "for^ i from^ 1 to^ 2000 {\n"
             // id() is scene.Node's, reached through the base chain -- the
             // walk that a wrong members table breaks.
             "    let^ s = scene.makeSprite()\n"
             "    n := n + s.id()\n"
             "    let^ r = scene.makeResource()\n"
             "    if^ i % 32 = 0 { L^.collectgarbage() }\n"
             "}\n"
             "return^ n\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 3;
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3 * 2000);
        lhat_program_dispose(&program);
    }
}

// 05 の 8.8改 with 8.7: what a value answers and what the host may ask
// about the type are one question. A member the type inherits is reached by
// a call on the value, so lhat_machine_registered has to find it too --
// which takes the read that climbs the chain rather than the one that sees
// a single table.
static void test_registered_sees_the_chain(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("a member a type inherits is one the host can ask for");
    {
        static const File files[] = {
            {"main.lh", "import^ scene\nreturn^ scene.makeSprite().id()\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 5;
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, machine), "installed");

        LhatValue held = lhat_nil();
        LHAT_CHECK(lhat_machine_registered(machine, "scene", "Node", "id",
                                           &held),
                   "the one the base declares");
        LHAT_CHECK(lhat_machine_registered(machine, "scene", "Sprite2D", "id",
                                           &held),
                   "and the one a derived type reaches through it");
        LHAT_CHECK(!lhat_machine_registered(machine, "scene", "Sprite2D",
                                            "nope", &held),
                   "a name nothing declared is still nothing");

        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 5);
        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// 05 の 8.12: what a host remembers about its own objects
//
// The cache a binding wants and cannot write for itself. What makes it
// unsound in a host's own map is that the collection is incremental: between
// the marking deciding a wrapper is unreachable and the sweep freeing it, the
// map still answers with it. So the guarantee has to come from the collector
// -- it takes the entry out at the end of the marking, and a read during a
// marking brings the value back.

static SceneNode cache_nodes[4];
static int cache_hits;
static int cache_misses;

// The shape a binding writes: ask, and make one only where the answer was
// nothing.
static void cached_node(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    SceneNode *node = &cache_nodes[lhat_as_integer(arguments[0]) & 3];
    LhatValue found = lhat_machine_weak_cache_get(machine, node);
    if (!lhat_is_nil(found)) {
        cache_hits++;
        answers[0] = found;
        *answer_count = 1;
        return;
    }
    cache_misses++;
    LhatValue made = lhat_nil();
    lhat_machine_make_hostdata(machine, sprite_tag, node, &made);
    lhat_machine_weak_cache_put(machine, node, made);
    answers[0] = made;
    *answer_count = 1;
}

// A machine with scene registered on it, for a test that drives the cache
// from C rather than through a program.
static LhatMachine *scene_machine(LhatProgram *program, Disk *disk)
{
    static const File files[] = {{"main.lh", "return^ 1\n"}};
    program_with(program, disk, files, 1);
    if (!register_scene(program, false) ||
        lhat_program_check(program, "main.lh") == NULL ||
        !lhat_program_compile(program)) {
        return NULL;
    }
    LhatMachine *machine = lhat_machine_new();
    if (machine == NULL || !lhat_program_install(program, machine)) {
        return NULL;
    }
    return machine;
}

static void test_the_weak_cache(void)
{
    LhatProgram program;
    Disk disk;

    // What the cache is for. The same pointer is handed over turn after turn
    // and answers the same wrapper, with collections running throughout --
    // because something in L^ is holding it, so the marking reaches it.
    LHAT_TEST("a wrapper something holds is answered again and again");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "var^ same = 0\n"
             "let^ held = scene.cached(0)\n"
             "for^ i from^ 1 to^ 200 {\n"
             "    let^ again = scene.cached(0)\n"
             "    if^ again is^ held { same := same + 1 }\n"
             "    L^.collectgarbage()\n"
             "}\n"
             "return^ same\n"},
        };
        cache_hits = 0;
        cache_misses = 0;
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        LHAT_CHECK(lhat_register_func(&program, "scene", "cached",
                                      "f^number^ -> scene.Sprite2D;",
                                      cached_node, NULL),
                   "the cache's call registered");
        LhatRunResult ran = run_program(&program);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        // Not "mostly the same": the same, every turn. is^ is what tells two
        // wrappers of one pointer apart (8.8 makes them equal under '=').
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 200);
        LHAT_CHECK_EQ_INT(cache_misses, 1);
        LHAT_CHECK_EQ_INT(cache_hits, 200);
        lhat_program_dispose(&program);
    }

    // And the other half: nothing holds it, so the entry goes. Driven from C
    // because what is being pinned is that a HOST's own hold counts for
    // nothing (vm.h says so) -- the value below is reachable from this
    // function and from nowhere the collector looks.
    LHAT_TEST("an entry nothing reaches is gone by the end of the marking");
    {
        LhatMachine *machine = scene_machine(&program, &disk);
        LHAT_CHECK(machine != NULL, "installed");

        LhatValue wrapper = lhat_nil();
        LHAT_CHECK(lhat_machine_make_hostdata(machine, sprite_tag,
                                              &cache_nodes[1], &wrapper),
                   "made");
        LHAT_CHECK(lhat_machine_weak_cache_put(machine, &cache_nodes[1],
                                               wrapper),
                   "remembered");
        LHAT_CHECK(!lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                            &cache_nodes[1])),
                   "and answered while it is there");

        lhat_machine_collectgarbage(machine);
        LHAT_CHECK(lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                           &cache_nodes[1])),
                   "the collector took the entry out itself");

        // A pointer never put is nothing, and a removal by hand is too.
        LHAT_CHECK(lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                           &cache_nodes[2])),
                   "a key nothing remembered");
        LhatValue other = lhat_nil();
        LHAT_CHECK(lhat_machine_make_hostdata(machine, sprite_tag,
                                              &cache_nodes[2], &other) &&
                       lhat_machine_weak_cache_put(machine, &cache_nodes[2],
                                                   other),
                   "remembered another");
        lhat_machine_weak_cache_forget(machine, &cache_nodes[2]);
        LHAT_CHECK(lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                           &cache_nodes[2])),
                   "a key the host took out by hand");

        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }

    // The resurrection, which is the whole reason this cannot be a host's
    // own map plus a callback. The marking is under way and has already
    // passed everything that could name the wrapper; the host asks for it;
    // the entry has to survive THIS cycle, because asking is what made it
    // reachable again.
    LHAT_TEST("a read during a marking brings the value back");
    {
        LhatMachine *machine = scene_machine(&program, &disk);
        LHAT_CHECK(machine != NULL, "installed");
        Machine *m = (Machine *)machine;

        LhatValue wrapper = lhat_nil();
        LHAT_CHECK(lhat_machine_make_hostdata(machine, sprite_tag,
                                              &cache_nodes[3], &wrapper),
                   "made");
        LHAT_CHECK(lhat_machine_weak_cache_put(machine, &cache_nodes[3],
                                               wrapper),
                   "remembered");

        // 05 の 8.7改5 made a machine's heap small enough that a whole
        // cycle passes inside one step (LHAT_GC_STEP_WORK is 20), and then
        // the loop below would never SEE the marking. So give the marking
        // enough to walk -- REACHABLE objects, since unreachable ones are
        // work for the sweep and none for the marking.
        {
            LhatValue held = lhat_nil();
            LHAT_CHECK(lhat_machine_make_table(machine, &held), "a bag");
            LHAT_CHECK(lhat_machine_set_global(machine, "bag", held),
                       "rooted");
            LhatTable *bag = (LhatTable *)lhat_as_object(held);
            for (int i = 0; i < 400; i++) {
                LhatValue one = lhat_nil();
                bool refused = false;
                if (!lhat_machine_make_table(machine, &one) ||
                    !lhat_machine_table_set(machine, bag, lhat_integer(i),
                                            one, &refused)) {
                    LHAT_CHECK(false, "filling the bag");
                    break;
                }
            }
        }

        // Into a marking, and no further: the step that follows the last of
        // the gray is the one that empties the cache, so stop before it.
        int guard = 0;
        while (m->gcstate != LHAT_GC_PROPAGATE && guard++ < 10000) {
            lhat_gc_step(machine);
        }
        LHAT_CHECK_EQ_INT(m->gcstate, LHAT_GC_PROPAGATE);

        // The ask. Nothing else in the machine names this wrapper.
        LHAT_CHECK(!lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                            &cache_nodes[3])),
                   "answered mid-marking");

        // Now to the end of that cycle, past the point where the entries
        // that were not reached are dropped.
        guard = 0;
        while (m->gcstate == LHAT_GC_PROPAGATE && guard++ < 100000) {
            lhat_gc_step(machine);
        }
        LHAT_CHECK(lhat_is_nil(lhat_machine_weak_cache_get(machine,
                                                           &cache_nodes[3])) ==
                       false,
                   "and kept, because asking for it is what reached it");

        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }
}

// ---------------------------------------------------------------------------
// 05 の 8.7改5: the registrations are the program's
//
// What install used to build per machine is built once on the program's own
// heap, born black, and hung off each machine's L^.modules by name. What is
// pinned here is that several machines answer the same, that the shared
// tables outlive a machine, and that the seal holds where a unit reaches for
// a name the host registered.

static void test_registrations_are_shared(void)
{
    LhatProgram program;
    Disk disk;

    LHAT_TEST("two machines of one program answer the same registrations");
    {
        static const File files[] = {
            {"main.lh", "import^ scene\nreturn^ scene.makeSprite().id()\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 9;
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");

        LhatMachine *first = lhat_machine_new();
        LhatMachine *second = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, first) &&
                       lhat_program_install(&program, second),
                   "both installed");

        // The same table, not a copy: what one machine reaches through
        // L^.modules is the object the other reaches.
        LhatValue here = lhat_nil();
        LhatValue there = lhat_nil();
        LHAT_CHECK(lhat_machine_registered(first, "scene", "Node", "id",
                                           &here) &&
                       lhat_machine_registered(second, "scene", "Node", "id",
                                               &there),
                   "both find the base's member");
        LHAT_CHECK(lhat_is_object(here) && lhat_is_object(there) &&
                       lhat_as_object(here) == lhat_as_object(there),
                   "and it is one object, not two");
        // 8.8改's base link is built once too, so the walk still climbs it.
        LHAT_CHECK(lhat_machine_registered(second, "scene", "Sprite2D", "id",
                                           &there),
                   "a derived type still reaches through its base");

        LhatRunResult ran = lhat_run(first, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 9);

        // The first machine goes; the shared tables belong to the program,
        // so the second goes on answering.
        lhat_machine_dispose(first);
        ran = lhat_run(second, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 9);
        lhat_machine_dispose(second);
        lhat_program_dispose(&program);
    }

    // A machine made after another has run, on a program whose shared tables
    // were built long before: the build is once, the attaching is per
    // machine, and a value made on the new machine reads its members off the
    // shared table like any other.
    LHAT_TEST("a machine installed later gets the same tables");
    {
        static const File files[] = {
            {"main.lh",
             "import^ scene\n"
             "var^ n = 0\n"
             "for^ i from^ 1 to^ 200 {\n"
             "    n := n + scene.makeSprite().id()\n"
             "    if^ i % 16 = 0 { L^.collectgarbage() }\n"
             "}\n"
             "return^ n\n"},
        };
        program_with(&program, &disk, files, 1);
        LHAT_CHECK(register_scene(&program, false), "registered");
        the_node.id = 2;
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");

        LhatMachine *first = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, first), "installed");
        LhatRunResult ran = lhat_run(first, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);

        LhatMachine *later = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, later), "installed later");
        ran = lhat_run(later, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 2 * 200);

        lhat_machine_dispose(first);
        lhat_machine_dispose(later);
        lhat_program_dispose(&program);
    }

    // The one corner the granularity leaves: a unit naming a path BELOW a
    // registered module writes into the sealed table, and SETINDEX refuses
    // it. Naming the module's own path is not that -- it replaces the entry
    // in the machine's own L^.modules, shadowing the registrations for that
    // machine exactly as it did before any of this.
    LHAT_TEST("a unit may not publish inside a module the host registered");
    {
        // A unit publishes where a require^ brings it in, so the reach has
        // to be one -- a root run on its own registers nothing.
        static const File files[] = {
            {"main.lh", "require^ \"extra.lh\"\nreturn^ 1\n"},
            {"extra.lh", "module^ scene.extra\npublic^ let^ answer = 42\n"},
        };
        program_with(&program, &disk, files, 2);
        LHAT_CHECK(register_scene(&program, false), "registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, machine), "installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_SEALED);
        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }

    LHAT_TEST("a unit under its own name publishes as it always did");
    {
        static const File files[] = {
            {"main.lh", "require^ \"own.lh\"\nreturn^ mine.own.answer\n"},
            {"own.lh", "module^ mine.own\npublic^ let^ answer = 42\n"},
        };
        program_with(&program, &disk, files, 2);
        LHAT_CHECK(register_scene(&program, false), "registered");
        const LhatUnit *root = lhat_program_check(&program, "main.lh");
        LHAT_CHECK(root != NULL && lhat_program_compile(&program), "built");
        LhatMachine *machine = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(&program, machine), "installed");
        LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        lhat_machine_dispose(machine);
        lhat_program_dispose(&program);
    }
}

int main(void)
{
    test_the_relation();
    test_inherited_members();
    test_fits();
    test_inherited_dispose();
    test_value_type();
    test_registration();
    test_delegate_to_host();
    test_the_weak_cache();
    test_values_keep_their_members();
    test_registered_sees_the_chain();
    test_registrations_are_shared();
    return lhat_test_report("test_hostdata_base");
}
