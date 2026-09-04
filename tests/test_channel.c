// L^ (lhat) -- tests for the sharing contract stdlib/carry moves hostdata
// under (05 の 8.8改2), and for std.channel, which is built on it.
//
// A type that declared it may cross does: the carried tree holds the
// pointer once, every wrapper rebuilt from it once more, and each is given
// back -- the tree's when it is freed, a wrapper's through dispose^. The
// counts here are what a host's reference count would see.

#include <string.h>

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/carry.h"
#include "../stdlib/channel.h"
#include "../stdlib/thread.h"
#include "port/thread.h"

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

// ---------------------------------------------------------------------------
// std.channel, as an L^ program sees it
// ---------------------------------------------------------------------------

static const LhatTestRegister regs[] = {lhatstdlib_channel_register};
static const LhatTestRegister with_thread[] = {lhatstdlib_channel_register,
                                               lhatstdlib_thread_register};

// Every case names a channel of its own, since the table of names is the
// process's and the cases run in one.
static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

static void test_one_machine(void)
{
    LHAT_TEST("what goes in comes out, in order and unchanged");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    c.push(42)\n"
            "    c.push(\"two\")\n"
            "    c.push({ x = 3, y = 4 })\n"
            "    var^ n = 0\n"
            "    let^ first = c.pop()\n"
            // 05 の 8.8改: an integer stays an integer through carry, which
            // is what a channel of a Variant cannot promise.
            "    if^ first fits^ number^ { n := n + first }\n"
            "    let^ second = c.pop()\n"
            "    if^ second = \"two\" { n := n + 100 }\n"
            "    let^ third = c.pop()\n"
            "    if^ third fits^ t^{ x : number^, y : number^ } {\n"
            "        n := n + third.x * third.y\n"
            "    }\n"
            "    if^ c.pop() fits^ nil^ { n := n + 1000 }\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1154);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("count, peek, hasRead and clear");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    var^ n = 0\n"
            "    let^ first = c.push(1)\n"
            "    c.push(2)\n"
            "    if^ first fits^ number^ and^ first = 1 { n := n + 1 }\n"
            "    if^ c.count() = 2 { n := n + 10 }\n"
            "    if^ c.peek() = 1 { n := n + 100 }\n"
            "    if^ c.count() = 2 { n := n + 1000 }\n"
            "    if^ first fits^ number^ and^ !c.hasRead(first) { n := n + 10000 }\n"
            "    c.pop()\n"
            "    if^ first fits^ number^ and^ c.hasRead(first) { n := n + 100000 }\n"
            "    c.clear()\n"
            "    if^ c.count() = 0 { n := n + 1000000 }\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1111111);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a closure crosses, and a channel goes into a channel");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ outer = std.channel.new()\n"
            "let^ inner = std.channel.new()\n"
            "if^ outer fits^ std.channel.Channel"
            "   and^ inner fits^ std.channel.Channel {\n"
            "    let^ scale = 7\n"
            "    outer.push(f^ n:number^ -> number^ { return^ n * scale })\n"
            // 8.8改2: the channel itself crosses, so a worker may be handed
            // the one it is to answer on.
            "    outer.push(inner)\n"
            "    var^ n = 0\n"
            "    let^ fn = outer.pop()\n"
            "    if^ fn fits^ f^number^ -> number^; { n := n + fn(6) }\n"
            "    let^ got = outer.pop()\n"
            "    if^ got fits^ std.channel.Channel {\n"
            "        got.push(5)\n"
            "        if^ inner.count() = 1 { n := n + 100 }\n"
            "    }\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 142);
        lhat_test_ran_dispose(&ran);
    }

    // carry's boundary, answered as a value the way std.thread answers it.
    // 05 の 8.8改3: a coroutine crosses while it has not started; once it
    // has, its frame is the machine's.
    LHAT_TEST("what cannot cross is refused with carry's own reason");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ co = (p^ { yield^ 1 yield^ 2 })()\n"
            "    co.start()\n"
            "    let^ said = c.push(co)\n"
            "    co.dispose()\n"
            "    if^ said fits^ std.channel.ChannelError.Refused {\n"
            "        return^ said.message\n"
            "    }\n"
            "    return^ \"took it\"\n"
            "}\n"
            "return^ \"no channel\"\n");
        LHAT_CHECK_RAN_TEXT(ran,
                            "a coroutine that has started stays on its machine");
        lhat_test_ran_dispose(&ran);
    }

    // 05 の 8.8改3: and the fresh one goes, arguments and captures with it.
    LHAT_TEST("a coroutine that has not started crosses and runs there");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ scale = 10\n"
            "    let^ gen = p^ n:number^ { yield^ n * scale }\n"
            "    c.push(gen(4))\n"
            "    let^ got = c.pop()\n"
            "    if^ got fits^ c^{p^ -> number^ -> nil^} {\n"
            "        let^ first = got.start()\n"
            "        if^ first fits^ number^ { return^ first }\n"
            "    }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 40);
        lhat_test_ran_dispose(&ran);
    }

    // The copy is its own: starting one leaves the other startable.
    LHAT_TEST("the copy and the original are two coroutines");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ gen = p^ { yield^ 7 }\n"
            "    let^ mine = gen()\n"
            "    c.push(mine)\n"
            "    let^ theirs = c.pop()\n"
            "    var^ n = 0\n"
            "    let^ a = mine.start()\n"
            "    if^ a fits^ number^ { n += a }\n"
            "    if^ theirs fits^ c^{p^ -> number^ -> nil^} {\n"
            "        let^ b = theirs.start()\n"
            "        if^ b fits^ number^ { n += b * 100 }\n"
            "    }\n"
            "    mine.dispose()\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 707);
        lhat_test_ran_dispose(&ran);
    }

    // The lock is held across the body, and the reads inside it answer
    // rather than wait (channel.h).
    LHAT_TEST("atomic holds the channel across the body");
    {
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    var^ n = 0\n"
            "    c.push(3)\n"
            "    c.atomic(p^ it:std.channel.Channel {\n"
            "        let^ got = it.demand()\n"
            "        if^ got fits^ number^ { n := n + got }\n"
            "        if^ it.demand(1) fits^ nil^ { n := n + 10 }\n"
            "        it.push(9)\n"
            "        n := n + it.count() * 100\n"
            "    })\n"
            "    if^ c.pop() = 9 { n := n + 1000 }\n"
            "    return^ n\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1113);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a wait that runs out answers nothing");
    {
        int64_t before = lhat_now_ms();
        LhatTestRan ran = run_source(
            "import^ std.channel\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    if^ c.demand(0.05) fits^ nil^ { return^ 1 }\n"
            "    return^ 0\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        LHAT_CHECK(lhat_now_ms() - before >= 40,
                   "it waited: %lld ms", (long long)(lhat_now_ms() - before));
        lhat_test_ran_dispose(&ran);
    }
}

static void test_between_machines(void)
{
    // The shape a worker pool is written in: the worker waits on one
    // channel and answers on another, and the body is a closure of the
    // same unit (std.thread carries it over).
    LHAT_TEST("a worker waits on a named channel and answers on another");
    {
        LhatTestRan ran = lhat_test_run(
            with_thread, 2,
            "import^ std.channel\n"
            "import^ std.thread\n"
            "let^ jobs = std.channel.named(\"jobs\")\n"
            "let^ done = std.channel.named(\"results\")\n"
            "if^ jobs fits^ std.channel.Channel"
            "   and^ done fits^ std.channel.Channel {\n"
            "    let^ h = std.thread.spawn(p^ ... {\n"
            "        let^ mine = std.channel.named(\"jobs\")\n"
            "        let^ back = std.channel.named(\"results\")\n"
            "        if^ mine fits^ std.channel.Channel"
            "           and^ back fits^ std.channel.Channel {\n"
            "            repeat^ {\n"
            "                let^ job = mine.demand()\n"
            "                if^ job fits^ string^ { break^ }\n"
            "                if^ job fits^ number^ { back.push(job * 2) }\n"
            "            }\n"
            "        }\n"
            "    })\n"
            "    if^ h fits^ std.thread.ThreadHandle {\n"
            "        jobs.push(1)\n"
            "        jobs.push(2)\n"
            // supply waits until the worker has taken it, so by the time
            // this returns both numbers are answered for.
            "        let^ taken = jobs.supply(\"stop\")\n"
            "        h.join()\n"
            "        var^ n = 0\n"
            "        if^ taken fits^ bool^ and^ taken { n := n + 1000 }\n"
            "        repeat^ {\n"
            "            let^ got = done.pop()\n"
            "            if^ got fits^ nil^ { break^ }\n"
            "            if^ got fits^ number^ { n := n + got }\n"
            "        }\n"
            "        return^ n\n"
            "    }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1006);
        lhat_test_ran_dispose(&ran);
        lhatstdlib_channel_forget_named();
    }

    // 8.8改2 again, over the boundary it was written for: the channel is
    // handed to the worker as an argument rather than found by name.
    LHAT_TEST("a channel handed to spawn is the same channel");
    {
        LhatTestRan ran = lhat_test_run(
            with_thread, 2,
            "import^ std.channel\n"
            "import^ std.thread\n"
            "let^ c = std.channel.new()\n"
            "if^ c fits^ std.channel.Channel {\n"
            "    let^ h = std.thread.spawn(p^ ... {\n"
            "        let^ mine = ...[1]\n"
            "        if^ mine fits^ std.channel.Channel { mine.push(11) }\n"
            "    }, c)\n"
            "    if^ h fits^ std.thread.ThreadHandle {\n"
            "        h.join()\n"
            "        let^ got = c.demand(2)\n"
            "        if^ got fits^ number^ { return^ got }\n"
            "    }\n"
            "}\n"
            "return^ -1\n");
        LHAT_CHECK_RAN_INTEGER(ran, 11);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_sharing_contract();
    test_one_machine();
    test_between_machines();
    lhatstdlib_channel_forget_named();
    return lhat_test_report("test_channel");
}
