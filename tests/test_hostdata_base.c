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

static LhatValue node_id(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    // Asked for the BASE's tag off a value that may be of any type under it.
    // 8.8改 is what makes this answer rather than NULL, and it is the whole
    // of what the host gets out of declaring the relation.
    SceneNode *self = (SceneNode *)lhat_hostdata_pointer(arguments[0], node_tag);
    return self != NULL ? lhat_integer(self->id) : lhat_nil();
}

static LhatValue node2d_x(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    SceneNode *self =
        (SceneNode *)lhat_hostdata_pointer(arguments[0], node2d_tag);
    return self != NULL ? lhat_integer(self->x) : lhat_nil();
}

static LhatValue node_dispose(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    node_releases++;
    return lhat_nil();
}

static LhatValue sprite_dispose(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    sprite_releases++;
    return lhat_nil();
}

// Makes a value of whichever type the registration bound this to.
static LhatValue make_of(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count)
{
    (void)arguments;
    (void)count;
    LhatValue out = lhat_nil();
    lhat_machine_make_hostdata(machine, (const LhatHostDataTag *)context,
                               &the_node, &out);
    return out;
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
}

int main(void)
{
    test_the_relation();
    test_inherited_members();
    test_fits();
    test_inherited_dispose();
    test_registration();
    test_delegate_to_host();
    return lhat_test_report("test_hostdata_base");
}
