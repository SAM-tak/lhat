// L^ (lhat) -- tests for the sharing contract stdlib/carry moves hostdata
// under (05 の 8.8改2), and for std.channel, which is built on it.
//
// A type that declared it may cross does: the carried tree holds the
// pointer once, every wrapper rebuilt from it once more, and each is given
// back -- the tree's when it is freed, a wrapper's through dispose^. The
// counts here are what a host's reference count would see.

#include <string.h>

#include "testutil.h"

#include "../stdlib/carry.h"

// The contract's state. One per process, like the tag it is declared on:
// a second program declaring the type has to hand over the same functions
// and context, so every case below shares these.
typedef struct {
    const LhatHostDataTag *tag;
    int holds;
    int disposed;
} Shared;

static Shared shared;

static void shared_retain(void *pointer, void *context)
{
    (void)pointer;
    ((Shared *)context)->holds++;
}

static void shared_let_go(void *pointer, void *context)
{
    (void)pointer;
    ((Shared *)context)->holds--;
}

// The wrapper's dispose^, which is how a wrapper gives its hold back.
static void shared_dispose(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    Shared *state = (Shared *)context;
    if (lhat_hostdata_pointer(arguments[0], state->tag) != NULL) {
        state->holds--;
        state->disposed++;
    }
}

static bool register_shared(LhatProgram *program)
{
    shared.tag = lhat_register_hostdata_type(program, "k", "T");
    return shared.tag != NULL &&
           lhat_register_member(program, "k", "T", "dispose", "p^self^;",
                                shared_dispose, &shared) &&
           lhat_register_hostdata_shared(program, "k", "T", shared_retain,
                                         shared_let_go, &shared);
}

// A wrapper made by hand holds the pointer once, the way a host's own
// wrapper would (its dispose^ gives one back).
static LhatValue wrapped(LhatMachine *machine, void *pointer)
{
    LhatValue value = lhat_nil();
    if (lhat_machine_make_hostdata(machine, shared.tag, pointer, &value)) {
        shared_retain(pointer, &shared);
    }
    return value;
}

static void test_sharing_contract(void)
{
    static int object;

    LHAT_TEST("hostdata of a type declared shared crosses, holding the pointer");
    {
        shared.holds = 0;
        shared.disposed = 0;
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK(register_shared(program), "registered");
        LhatMachine *from = lhat_machine_new();
        LhatMachine *to = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, from) &&
                       lhat_program_install(program, to),
                   "installed on both");
        LhatValue value = wrapped(from, &object);
        LHAT_CHECK(!lhat_is_nil(value), "made");
        LHAT_CHECK_EQ_INT(shared.holds, 1);

        LhatCarried *tree = NULL;
        const char *why = NULL;
        LHAT_CHECK(lhat_carry(value, &tree, &why), "carried: %s",
                   why != NULL ? why : "");
        LHAT_CHECK_EQ_INT(shared.holds, 2);

        LhatValue a = lhat_nil();
        LhatValue b = lhat_nil();
        LHAT_CHECK(lhat_uncarry(to, tree, &a) && lhat_uncarry(to, tree, &b),
                   "rebuilt twice");
        LHAT_CHECK_EQ_INT(shared.holds, 4);
        LHAT_CHECK(lhat_hostdata_pointer(a, shared.tag) == &object &&
                       lhat_hostdata_pointer(b, shared.tag) == &object,
                   "the same pointer, wrapped again");

        lhat_carried_free(tree);
        LHAT_CHECK_EQ_INT(shared.holds, 3);
        lhat_machine_dispose(to);
        LHAT_CHECK_EQ_INT(shared.holds, 1);
        LHAT_CHECK_EQ_INT(shared.disposed, 2);
        lhat_machine_dispose(from);
        LHAT_CHECK_EQ_INT(shared.holds, 0);
        lhat_program_free(program);
    }

    LHAT_TEST("inside a table it crosses the same way");
    {
        shared.holds = 0;
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK(register_shared(program), "registered");
        LhatMachine *from = lhat_machine_new();
        LhatMachine *to = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, from) &&
                       lhat_program_install(program, to),
                   "installed on both");
        LhatValue table = lhat_nil();
        LhatValue key = lhat_nil();
        LHAT_CHECK(lhat_machine_make_table(from, &table) &&
                       lhat_machine_make_string(from, "it", 2, &key) &&
                       lhat_machine_table_set(
                           from, (LhatTable *)lhat_as_object(table), key,
                           wrapped(from, &object), NULL),
                   "a table holding one");
        LhatCarried *tree = NULL;
        LHAT_CHECK(lhat_carry(table, &tree, NULL), "carried");
        LhatValue got = lhat_nil();
        LHAT_CHECK(lhat_uncarry(to, tree, &got) &&
                       lhat_is_object_kind(got, LHAT_OBJECT_TABLE),
                   "rebuilt");
        LhatValue far_key = lhat_nil();
        LHAT_CHECK(lhat_machine_make_string(to, "it", 2, &far_key), "key");
        LhatValue inside =
            lhat_table_get((const LhatTable *)lhat_as_object(got), far_key);
        LHAT_CHECK(lhat_hostdata_pointer(inside, shared.tag) == &object,
                   "the field is the wrapper");
        LHAT_CHECK_EQ_INT(shared.holds, 3);
        lhat_carried_free(tree);
        lhat_machine_dispose(to);
        lhat_machine_dispose(from);
        LHAT_CHECK_EQ_INT(shared.holds, 0);
        lhat_program_free(program);
    }

    // The members table comes up empty on a machine the program was never
    // installed on, but the wrapper is still one of the type: dispose^ is
    // the tag's (8.8), so the hold it took is given back all the same.
    LHAT_TEST("a machine the program was not installed on still gives it back");
    {
        shared.holds = 0;
        shared.disposed = 0;
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK(register_shared(program), "registered");
        LhatMachine *from = lhat_machine_new();
        LhatMachine *bare = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, from), "installed on one");
        LhatValue value = wrapped(from, &object);
        LhatCarried *tree = NULL;
        LHAT_CHECK(lhat_carry(value, &tree, NULL), "carried");
        LhatValue got = lhat_nil();
        LHAT_CHECK(lhat_uncarry(bare, tree, &got) &&
                       lhat_hostdata_pointer(got, shared.tag) == &object,
                   "rebuilt there too");
        LHAT_CHECK_EQ_INT(shared.holds, 3);
        lhat_carried_free(tree);
        lhat_machine_dispose(bare);
        LHAT_CHECK_EQ_INT(shared.disposed, 1);
        lhat_machine_dispose(from);
        LHAT_CHECK_EQ_INT(shared.holds, 0);
        lhat_program_free(program);
    }

    LHAT_TEST("a type that declared nothing stays on its machine");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        const LhatHostDataTag *tag =
            lhat_register_hostdata_type(program, "k", "U");
        LHAT_CHECK(tag != NULL, "registered");
        LhatMachine *from = lhat_machine_new();
        LHAT_CHECK(lhat_program_install(program, from), "installed");
        LhatValue value = lhat_nil();
        LHAT_CHECK(lhat_machine_make_hostdata(from, tag, &object, &value),
                   "made");
        LhatCarried *tree = NULL;
        const char *why = NULL;
        LHAT_CHECK(!lhat_carry(value, &tree, &why) && why != NULL &&
                       strcmp(why, "a host's value stays on its machine") == 0,
                   "refused by name: %s", why != NULL ? why : "(none)");
        lhat_machine_dispose(from);
        lhat_program_free(program);
    }

    LHAT_TEST("a second declaration has to agree");
    {
        LhatProgram *program = lhat_program_new(true, NULL, NULL);
        LHAT_CHECK(register_shared(program), "the same contract again");
        LHAT_CHECK(!lhat_register_hostdata_shared(program, "k", "T",
                                                  shared_let_go, shared_retain,
                                                  &shared),
                   "a different one is refused");
        LHAT_CHECK(!lhat_register_hostdata_shared(program, "k", "Nope",
                                                  shared_retain, shared_let_go,
                                                  &shared),
                   "and so is a type nobody registered");
        lhat_program_free(program);
    }
}

int main(void)
{
    test_sharing_contract();
    return lhat_test_report("test_channel");
}
