// Delegation chains: resolved receivers, effective types and finite cycles.
#include <stdio.h>
#include <string.h>

#include "fixture.h"
#include "rttype.h"

static const char *layers =
    "let^ Store = t^{ read:f^self^ -> number^;, bump:p^self^; }\n"
    "let^ Leaf = def^{ self^{ n = 7 },\n"
    "  read = f^self^ -> number^ { self^.n },\n"
    "  bump = p^self^ { self^.n += 1 }, hidden = f^ { 9 } }\n"
    "let^ One = def^{ self^{ abstract^ held:Leaf, n = 101 },\n"
    "  override^new = f^held:Leaf { self^{held = held} },\n"
    "  delegate^ self^.held }\n"
    "let^ Two = def^{ self^{ abstract^ held:One, n = 201 },\n"
    "  override^new = f^held:One { self^{held = held} },\n"
    "  delegate^ self^.held }\n"
    "let^ Three = def^{ self^{ abstract^ held:Two, n = 301 },\n"
    "  override^new = f^held:Two { self^{held = held} },\n"
    "  delegate^ self^.held }\n"
    "let^ Wrap = def^{ self^{ abstract^ held:Store, n = 401 },\n"
    "  override^new = f^held:Store { self^{held = held} },\n"
    "  delegate^ self^.held }\n"
    "let^ leaf = Leaf.new()\n"
    "let^ one = One.new(leaf)\n"
    "let^ two = Two.new(one)\n"
    "let^ three = Three.new(two)\n";

static void source(char *out, size_t capacity, const char *body)
{
    int n = snprintf(out, capacity, "%s%s", layers, body);
    LHAT_CHECK(n >= 0 && (size_t)n < capacity, "fixture fits");
}

static void expect_true(const char *body)
{
    char text[8192];
    source(text, sizeof text, body);
    Unit u;
    check_text(&u, text);
    CHECK_CLEAN(&u);
    unit_dispose(&u);
    check_relaxed_text(&u, text);
    CHECK_CLEAN(&u);
    unit_dispose(&u);
    Run r;
    run_checked_text(&r, text);
    CHECK_BOOL(&r, true);
    run_dispose(&r);
    run_text(&r, text);
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

static void test_receivers(void)
{
    LHAT_TEST("two and three hops read and mutate only the leaf");
    expect_true("two.bump()\nthree.bump()\n"
                "return^ two.read()*100000 + three.read()*10000 + leaf.n*1000"
                " + one.n + two.n + three.n = 999603\n");

    LHAT_TEST("structural interfaces retain the runtime receiver");
    expect_true("let^ use = p^s:Store -> number^ { s.bump() return^ s.read() }\n"
                "let^ s:Store = three\nreturn^ use(s) = 8\n");

    LHAT_TEST("a method on the middle wrapper stops lookup there");
    expect_true("let^ Middle = def^{ self^{ abstract^ held:Store, n = 55 },\n"
                " override^new = f^held:Store { self^{held = held} },\n"
                " read = f^self^ -> number^ { self^.n },\n"
                " delegate^ self^.held }\n"
                "let^ m = Middle.new(leaf)\nlet^ w = Wrap.new(Wrap.new(m))\n"
                "w.bump()\nreturn^ w.read()*100 + leaf.n = 5508\n");

    LHAT_TEST("one call site follows replacement and distinct instances");
    expect_true("let^ get = f^s:Store -> number^ { s.read() }\n"
                "let^ other = Leaf.new()\nother.n := 23\n"
                "let^ w = Wrap.new(Wrap.new(leaf))\n"
                "let^ first = get(w)\nw.held := Wrap.new(other)\n"
                "return^ first*10000 + get(w)*100 + get(three) = 72307\n");

    LHAT_TEST("a shared definition slot can delegate through another wrapper");
    expect_true("let^ Shared = def^{ self^{}, sink = two, delegate^ sink }\n"
                "let^ a = Shared.new()\nlet^ b = Shared.new()\na.bump()\n"
                "return^ b.read() = 8\n");

    LHAT_TEST("many instances of the same type form a valid chain");
    expect_true("var^ w:Store = leaf\nvar^ i = 0\n"
                "repeat^ while^ i < 2048 { w := Wrap.new(w) i += 1 }\n"
                "w.bump()\nreturn^ w.read() = 8\n");

    LHAT_TEST("C member calls use the resolved receiver, including mutation");
    char text[8192];
    source(text, sizeof text, "return^ three\n");
    Run r;
    run_checked_text(&r, text);
    LHAT_REQUIRE(r.ran.status == LHAT_RUN_OK, "wrapper returned");
    LhatValue object = r.ran.value;
    LhatRunResult answer = lhat_machine_call_member(r.machine, object,
                                                   "bump", 4, NULL, 0);
    LHAT_CHECK_EQ_INT(answer.status, LHAT_RUN_OK);
    answer = lhat_machine_call_member(r.machine, object, "read", 4, NULL, 0);
    LHAT_CHECK_EQ_INT(answer.status, LHAT_RUN_OK);
    LHAT_CHECK_EQ_INT(lhat_as_integer(answer.value), 8);
    run_dispose(&r);
}

static void test_cycles(void)
{
    // read() exists in the structural contract, but no value supplies it once
    // the objects form a ring. Raw reads must terminate and answer nil^.
    LHAT_TEST("self delegation ends as an absent member");
    expect_true("let^ w = Wrap.new(leaf)\nw.held := w\n"
                "let^ value:any^ = w.read\nreturn^ value is^ nil^\n");
    LHAT_TEST("a ring of three objects ends as an absent member");
    expect_true("let^ a = Wrap.new(leaf)\nlet^ b = Wrap.new(a)\n"
                "let^ c = Wrap.new(b)\na.held := c\n"
                "let^ value:any^ = b.read\nreturn^ value is^ nil^\n");
    LHAT_TEST("a member found before a ring repeats remains callable");
    expect_true("let^ M = def^{ self^{ abstract^ held:Store, n = 66 },\n"
                " override^new = f^held:Store { self^{held = held} },\n"
                " read = f^self^ -> number^ { self^.n },\n"
                " delegate^ self^.held }\n"
                "let^ m = M.new(leaf)\nlet^ w = Wrap.new(m)\nm.held := w\n"
                "return^ w.read() = 66\n");
    LHAT_TEST("calling a missing member in a ring uses the ordinary fault");
    char text[8192];
    source(text, sizeof text, "let^ w = Wrap.new(leaf)\nw.held := w\nw.read()\n");
    Run r;
    run_text(&r, text);
    LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_NOT_CALLABLE);
    run_dispose(&r);
}

static void test_special_calls(void)
{
    LHAT_TEST("hat aliases keep the receiver of a delegated tostring");
    expect_true("let^ Text = def^{ self^{ label = \"leaf\" },\n"
                " tostring = f^self^ -> string^ { self^.label } }\n"
                "let^ A = def^{ self^{ label = \"outer\" },\n"
                " kept = Text.new(), delegate^ kept }\n"
                "let^ B = def^{ self^{}, kept = A.new(), delegate^ kept }\n"
                "return^ B.new().tostring^() = \"leaf\"\n");

    LHAT_TEST("left and right operators replace only their own receiver");
    expect_true("let^ Left = def^{ self^{ n = 17 },\n"
                " op^+ = f^self^, x:number^ -> number^ { self^.n + x } }\n"
                "let^ Right = def^{ self^{ n = 29 },\n"
                " op^+ = f^x:number^, self^ -> number^ { x + self^.n } }\n"
                "let^ A = def^{ self^{n = 300}, held = Left.new(), delegate^ held }\n"
                "let^ B = def^{ self^{n = 400}, held = A.new(), delegate^ held }\n"
                "let^ C = def^{ self^{n = 500}, held = Right.new(), delegate^ held }\n"
                "let^ D = def^{ self^{n = 600}, held = C.new(), delegate^ held }\n"
                "return^ (B.new() + 3)*100 + (2 + D.new()) = 2031\n");
}

static void test_visibility(void)
{
    const char *bad[] = {"return^ three.hidden()\n", "return^ three.new()\n",
                        "return^ Three.read()\n",
                        "let^ Only = def^{ self^{}, held = leaf, delegate^ held }\n"
                        "return^ Only.new().n\n",
                        "let^ wrong:t^{read:f^self^ -> string^;} = three\n",
                        "let^ A = def^{ self^{}, held = leaf, delegate^ held }\n"
                        "let^ B = def^{ self^{}, held = A.new(), delegate^ held }\n"
                        "let^ missing:B = {}\n"};
    char text[8192];
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        LHAT_TEST("non-methods and incompatible signatures remain rejected");
        source(text, sizeof text, bad[i]);
        for (int relaxed = 0; relaxed < 2; relaxed++) {
            Unit u;
            if (relaxed) check_relaxed_text(&u, text);
            else check_text(&u, text);
            LHAT_CHECK_EQ_INT(syntax_errors(&u), 0);
            LHAT_CHECK(u.checked.diagnostic_count > 0, "case %zu is rejected", i);
            unit_dispose(&u);
        }
    }
    LHAT_TEST("unchecked access does not lend fields or static members");
    source(text, sizeof text,
           "let^ Only = def^{ self^{}, held = two, delegate^ held }\n"
           "let^ o = Only.new()\n"
           "if^ (o.n is^ nil^) = false^ { return^ false^ }\n"
           "if^ (o.hidden is^ nil^) = false^ { return^ false^ }\n"
           "return^ Only.read is^ nil^\n");
    Run r;
    run_text(&r, text);
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

static void test_type_chains(void)
{
    LHAT_TEST("cyclic type links enumerate each effective requirement once");
    LhatTypeArena arena;
    lhat_type_arena_init(&arena);
    LhatType *outer = lhat_type_table(&arena);
    LhatType *middle = lhat_type_table(&arena);
    LhatType *inner = lhat_type_table(&arena);
    LhatType *empty = lhat_type_table(&arena);
    LhatType *method = lhat_type_func(&arena, true);
    method->v.func.takes_self = true;
    LhatTypeMember *read = lhat_type_add_member(&arena, inner, "read", 4, method);
    outer->v.table.delegate = middle;
    middle->v.table.delegate = inner;
    inner->v.table.delegate = middle;
    LHAT_CHECK(lhat_type_find_member(outer, "read", 4) == read, "deep member");
    LHAT_CHECK(lhat_type_find_member(outer, "missing", 7) == NULL, "finite miss");
    LHAT_CHECK(!lhat_type_conforms(empty, outer), "delegated requirements count");
    LHAT_CHECK(lhat_type_conforms(inner, outer), "all effective members suffice");
    char text[256];
    lhat_type_write_full(outer, text, sizeof text);
    LHAT_CHECK(strcmp(text, "t^{ read : f^self^; }") == 0, "type spelling: %s", text);
    LhatHeap heap = {0};
    LhatRuntimeType *rt = lhat_rt_from_checked(&heap, outer);
    LHAT_REQUIRE(rt != NULL, "runtime descriptor");
    LHAT_CHECK_EQ_INT(rt->member_count, 1);
    lhat_object_free_all(&heap);

    LHAT_TEST("ambiguity and nearer non-methods block delegated requirements");
    read->ambiguous = true;
    LHAT_CHECK(lhat_type_conforms(empty, outer), "ambiguous requirement excluded");
    LhatType *required = lhat_type_table(&arena);
    lhat_type_add_member(&arena, required, "read", 4, method);
    LHAT_CHECK(!lhat_type_conforms(outer, required), "ambiguous provider refused");
    read->ambiguous = false;
    lhat_type_add_member(&arena, middle, "read", 4,
                         lhat_type_simple(&arena, LHAT_TYPE_NUMBER));
    LHAT_CHECK(lhat_type_find_member(outer, "read", 4) == NULL,
               "a nearer non-method hides a deeper method");
    lhat_type_arena_dispose(&arena);
}

int main(void)
{
    test_receivers();
    test_cycles();
    test_special_calls();
    test_visibility();
    test_type_chains();
    return lhat_test_report("test_delegate");
}
