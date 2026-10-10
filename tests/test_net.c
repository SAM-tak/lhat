// L^ (lhat) -- tests for std.net (11 章).
//
// Everything travels over the loopback: one socket binds to port 0, asks
// which port it got, and another sends to it. A datagram on the loopback is
// there as soon as the send returns, but receive never waits (11 の 4.2), so
// each case polls a bounded number of times rather than assuming.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/binary.h"
#include "../stdlib/net.h"

static const LhatTestRegister regs[] = {lhatstdlib_net_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// Two sockets, `a` bound on the loopback at `port` and `b` not bound at all.
#define PAIR                                            \
    "import^ std.net\n"                                 \
    "import^ std.binary\n"                              \
    "let^ a = try^ std.net.udp()\n"                     \
    "let^ b = try^ std.net.udp()\n"                     \
    "try^ a.bind(\"127.0.0.1\", 0)\n"                   \
    "let^ host, port = a.getLocal()\n"

// Polls `a` until something arrives, leaving it in `got` and the sender in
// `from_port`.
#define RECEIVE                                         \
    "var^ got = \"\"\n"                                 \
    "var^ from_port = 0\n"                              \
    "for^ i from^ 0 to^ 100000 {\n"                     \
    "    let^ d, h, p = try^ a.receive()\n"             \
    "    if^ d fits^ string^ {\n"                       \
    "        got := d\n"                                \
    "        from_port := p\n"                          \
    "        break^\n"                                  \
    "    }\n"                                           \
    "}\n"

static void test_round_trip(void)
{
    LHAT_TEST("bind on port 0 picks a port, and getLocal says which");
    {
        LhatTestRan ran = run_source(PAIR
                                     "if^ host = \"127.0.0.1\" and^ port > 0 { return^ 1 }\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("what sendTo sends, receive receives, with who sent it");
    {
        LhatTestRan ran = run_source(PAIR
                                     "try^ b.sendTo(\"hello\", \"127.0.0.1\", port)\n" RECEIVE
                                     "let^ bh, bp = b.getLocal()\n"
                                     "if^ from_port = bp { return^ got }\n"
                                     "return^ \"wrong sender\"\n");
        LHAT_CHECK_RAN_TEXT(ran, "hello");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("4.2: nothing waiting answers nil^, \"\" and 0 at once");
    {
        LhatTestRan ran = run_source(PAIR
                                     "let^ d, h, p = try^ a.receive()\n"
                                     "if^ d = nil^ and^ h = \"\" and^ p = 0 { return^ 1 }\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("setPeer names where send goes, and a Bytes goes as it is");
    {
        LhatTestRan ran = run_source(PAIR
                                     "let^ f = std.binary.format({ { \"n\", std.binary.uint(16) } })\n"
                                     "let^ out = std.binary.bytes()\n"
                                     "f.encodeInto({ n = 515 }, out)\n"
                                     "try^ b.setPeer(\"127.0.0.1\", port)\n"
                                     "try^ b.send(out)\n"
                                     "let^ into = std.binary.bytes()\n"
                                     "for^ i from^ 0 to^ 100000 {\n"
                                     "    let^ d, h, p = try^ a.receiveInto(into)\n"
                                     "    if^ d fits^ std.binary.Bytes {\n"
                                     "        let^ t = f.decode(d)\n"
                                     "        if^ t fits^ std.binary.Error { return^ 0 }\n"
                                     "        return^ t[\"n\"] * 10 + into.size()\n"
                                     "    }\n"
                                     "}\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 5152);
        lhat_test_ran_dispose(&ran);
    }
}

static void test_errors(void)
{
    LHAT_TEST("a port taken already is AddressInUse");
    {
        LhatTestRan ran = run_source(PAIR
                                     "let^ r = b.bind(\"127.0.0.1\", port)\n"
                                     "if^ r fits^ std.net.Error.AddressInUse { return^ 1 }\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a disposed socket answers Closed");
    {
        LhatTestRan ran = run_source(PAIR
                                     "a.dispose()\n"
                                     "let^ poll = p^ -> number^|std.net.Error {\n"
                                     "    let^ d, h, p = try^ a.receive()\n"
                                     "    return^ 0\n"
                                     "}\n"
                                     "let^ r = poll()\n"
                                     "if^ r fits^ std.net.Error.Closed { return^ 1 }\n"
                                     "return^ 0\n");
        LHAT_CHECK_RAN_INTEGER(ran, 1);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("send with no peer named is the writer's mistake: a panic");
    {
        LhatTestRan ran = run_source(PAIR "let^ r = b.send(\"x\")\nreturn^ 1\n");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and so is a port outside 0..65535");
    {
        LhatTestRan ran = run_source(PAIR "let^ r = b.sendTo(\"x\", \"127.0.0.1\", 70000)\nreturn^ 1\n");
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_round_trip();
    test_errors();
    return lhat_test_report("test_net");
}
