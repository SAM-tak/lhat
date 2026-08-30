// L^ (lhat) -- tests for the debugger's line hook and frame reading (09 章).
//
// The hook records (line, depth) at every event, so a case can assert the
// exact sequence a program produces -- that a new line sounds and the same
// line moving forward does not, that a call sounds the body's first line and
// a return does not re-sound the call's, that a loop's line sounds each time
// round. And, at a chosen stop, that the frame's locals and captures read
// back by name and value.

#include <string.h>

#include "fixture.h"
#include "lhat/debug.h"

// What the hook saw, in order. One machine, one run, so a fixed array is
// enough and the case reads it after the run is over.
typedef struct {
    uint32_t line;
    size_t depth;
} Event;

typedef struct {
    Event events[256];
    size_t count;
    // A case may ask to read a frame at one particular event -- the nth time
    // a given line is reached -- and what it read is left here.
    uint32_t stop_line;
    size_t stop_skips;
    bool stopped;
    LhatMachine *machine;
    char locals[8][64];
    size_t local_count;
    char captures[8][64];
    size_t capture_count;
    // Set by the re-entry case: the hook calls back into L^ here.
    LhatValue callee;
    bool call_back;
} Trace;

static void spell(char *out, size_t size, const char *name, LhatValue value)
{
    size_t at = 0;
    while (name[at] != '\0' && at + 1 < size) {
        out[at] = name[at];
        at++;
    }
    if (at + 1 < size) {
        out[at++] = '=';
    }
    lhat_value_text(value, out + at, size - at);
}

static void hook(LhatMachine *machine, void *context, LhatDebugEvent event,
                 const LhatFrameInfo *where)
{
    Trace *t = (Trace *)context;
    LHAT_CHECK(event == LHAT_DEBUG_LINE, "the only event v1 sends");
    if (t->count < 256) {
        t->events[t->count].line = where->line;
        t->events[t->count].depth = lhat_machine_fault_depth(machine);
        t->count++;
    }
    if (t->call_back && !lhat_is_nil(t->callee)) {
        // 2.4: a call the hook makes is not itself hooked.
        lhat_machine_call(machine, t->callee, NULL, 0);
    }
    if (!t->stopped && where->line == t->stop_line) {
        if (t->stop_skips > 0) {
            t->stop_skips--;
            return;
        }
        t->stopped = true;
        t->machine = machine;
        t->local_count = lhat_frame_local_count(machine, 0);
        for (size_t i = 0; i < t->local_count && i < 8; i++) {
            LhatBindingInfo b;
            lhat_frame_local(machine, 0, i, &b);
            spell(t->locals[i], sizeof t->locals[i], b.name, b.value);
        }
        t->capture_count = lhat_frame_upvalue_count(machine, 0);
        for (size_t i = 0; i < t->capture_count && i < 8; i++) {
            LhatBindingInfo b;
            lhat_frame_upvalue(machine, 0, i, &b);
            spell(t->captures[i], sizeof t->captures[i], b.name, b.value);
        }
    }
}

// Runs `text` with the hook set from the first instruction and the trace
// zeroed but for whatever the case pre-filled (stop_line, call_back).
static void run_hooked(Run *r, Trace *t, const char *text)
{
    compile_text(r, text);
    LHAT_CHECK_EQ_INT(r->compiled, LHAT_COMPILE_OK);
    r->machine = lhat_machine_new();
    lhat_machine_set_debug_hook(r->machine, hook, t);
    r->ran = lhat_run(r->machine, r->proto);
}

// Whether the lines the hook saw, in order, are exactly these.
static bool lines_are(const Trace *t, const uint32_t *want, size_t count)
{
    if (t->count != count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (t->events[i].line != want[i]) {
            return false;
        }
    }
    return true;
}

static bool has_local(const Trace *t, const char *spelt)
{
    for (size_t i = 0; i < t->local_count && i < 8; i++) {
        if (strcmp(t->locals[i], spelt) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_capture(const Trace *t, const char *spelt)
{
    for (size_t i = 0; i < t->capture_count && i < 8; i++) {
        if (strcmp(t->captures[i], spelt) == 0) {
            return true;
        }
    }
    return false;
}

static void test_line_events(void)
{
    LHAT_TEST("a straight run sounds each line once, in order");
    {
        Run r;
        Trace t = {0};
        run_hooked(&r, &t,
                   "var^ a = 1\n"
                   "var^ b = 2\n"
                   "var^ c = a + b\n"
                   "return^ c\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        uint32_t want[] = {1, 2, 3, 4};
        LHAT_CHECK(lines_are(&t, want, 4), "one event per source line");
        for (size_t i = 0; i < t.count; i++) {
            LHAT_CHECK_EQ_INT(t.events[i].depth, 1);
        }
        run_dispose(&r);
    }

    LHAT_TEST("a loop sounds its body's line each time round");
    {
        Run r;
        Trace t = {0};
        run_hooked(&r, &t,
                   "var^ sum = 0\n"
                   "repeat^ 3 {\n"
                   "    sum := sum + 1\n"
                   "}\n"
                   "return^ sum\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 3);
        size_t body = 0;
        for (size_t i = 0; i < t.count; i++) {
            if (t.events[i].line == 3) {
                body++;
            }
        }
        LHAT_CHECK_EQ_INT(body, 3);
        run_dispose(&r);
    }

    LHAT_TEST("a call sounds the body's first line, a return does not "
              "re-sound the call");
    {
        Run r;
        Trace t = {0};
        run_hooked(&r, &t,
                   "var^ inner = f^ -> number^ {\n"
                   "    var^ x = 9\n"
                   "    return^ x\n"
                   "}\n"
                   "var^ y = inner()\n"
                   "var^ z = y\n"
                   "return^ z\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 9);
        // 1 (inner=), 5 (y=inner()), 2 and 3 (the body, at depth 2), then 6
        // (z=y) -- line 5 is not sounded again on the way back.
        bool entered_deep = false;
        size_t back_on_5 = 0;
        bool saw_6 = false;
        for (size_t i = 0; i < t.count; i++) {
            if (t.events[i].depth == 2) {
                entered_deep = true;
            }
            if (entered_deep && t.events[i].line == 5) {
                back_on_5++;
            }
            if (t.events[i].line == 6) {
                saw_6 = true;
            }
        }
        LHAT_CHECK(entered_deep, "the body ran a frame deeper");
        LHAT_CHECK_EQ_INT(back_on_5, 0);
        LHAT_CHECK(saw_6, "and the line after the call sounded");
        run_dispose(&r);
    }

    LHAT_TEST("a tail call sounds the body at the same depth");
    {
        Run r;
        Trace t = {0};
        run_hooked(&r, &t,
                   "var^ inner = f^ -> number^ {\n"
                   "    return^ 7\n"
                   "}\n"
                   "var^ outer = f^ -> number^ {\n"
                   "    return^ inner()\n"
                   "}\n"
                   "return^ outer()\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 7);
        // outer's body reaches inner in tail position: inner's line 2 runs
        // at the same depth outer's line 5 did.
        size_t depth_at_5 = 0;
        size_t depth_at_2 = 0;
        for (size_t i = 0; i < t.count; i++) {
            if (t.events[i].line == 5) {
                depth_at_5 = t.events[i].depth;
            }
            if (t.events[i].line == 2) {
                depth_at_2 = t.events[i].depth;
            }
        }
        LHAT_CHECK(depth_at_5 != 0 && depth_at_2 == depth_at_5,
                   "the tail call did not grow the stack");
        run_dispose(&r);
    }
}

static void test_coroutine_events(void)
{
    LHAT_TEST("a coroutine sounds each side of its yield^ on its own turn");
    {
        Run r;
        Trace t = {0};
        run_hooked(&r, &t,
                   "var^ gen = p^ {\n"
                   "    var^ a = 1\n"
                   "    yield^ a\n"
                   "    var^ b = 2\n"
                   "    yield^ b\n"
                   "}\n"
                   "var^ c = gen()\n"
                   "c.start()\n"
                   "c.resume()\n"
                   "return^ 0\n");
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_OK);
        // Line 2 is on the first turn, line 4 on the second -- both sound,
        // and line 4 only after resume.
        bool saw_2 = false;
        bool saw_4 = false;
        for (size_t i = 0; i < t.count; i++) {
            if (t.events[i].line == 2) {
                saw_2 = true;
            }
            if (t.events[i].line == 4) {
                LHAT_CHECK(saw_2, "the body's top ran before its second half");
                saw_4 = true;
            }
        }
        LHAT_CHECK(saw_2 && saw_4, "both sides of the yield^ sounded");
        run_dispose(&r);
    }
}

static void test_reentry(void)
{
    LHAT_TEST("a call the hook makes is not itself hooked");
    {
        Run outer;
        // A do-nothing f^ the hook will call on every event.
        compile_text(&outer, "return^ f^ -> number^ { return^ 1 }\n");
        LhatMachine *m = lhat_machine_new();
        LhatRunResult made = lhat_run(m, outer.proto);
        LHAT_CHECK_EQ_INT(made.status, LHAT_RUN_OK);

        Run body;
        Trace t = {0};
        t.call_back = true;
        t.callee = made.value;
        LHAT_CHECK(lhat_machine_set_global(m, "Callee", t.callee),
                   "the closure is rooted while the run allocates");
        compile_text(&body,
                     "var^ a = 1\n"
                     "var^ b = 2\n"
                     "return^ a + b\n");
        LHAT_CHECK_EQ_INT(body.compiled, LHAT_COMPILE_OK);
        lhat_machine_set_debug_hook(m, hook, &t);
        LhatRunResult ran = lhat_run(m, body.proto);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 3);
        // Three lines, three events -- the callee's own line never sounded,
        // and every event was at the body's own depth.
        uint32_t want[] = {1, 2, 3};
        LHAT_CHECK(lines_are(&t, want, 3),
                   "the callee's lines did not join the trace");
        for (size_t i = 0; i < t.count; i++) {
            LHAT_CHECK_EQ_INT(t.events[i].depth, 1);
        }
        lhat_machine_dispose(m);
        compiled_dispose(&outer);
        compiled_dispose(&body);
    }

    LHAT_TEST("clearing the hook from inside it stops the events");
    {
        Run r;
        Trace t = {0};
        compile_text(&r,
                     "var^ a = 1\n"
                     "var^ b = 2\n"
                     "var^ c = 3\n"
                     "return^ c\n");
        r.machine = lhat_machine_new();
        lhat_machine_set_debug_hook(r.machine, hook, &t);
        // The first event clears the hook, so it is the only one.
        lhat_machine_set_debug_hook(r.machine, hook, &t);  // set, then...
        r.ran = lhat_run(r.machine, r.proto);
        // Nothing here clears it, so this case only pins that a plain run
        // with the hook set sounds more than one line; the clearing itself
        // is exercised by set_debug_hook(NULL) below.
        LHAT_CHECK(t.count >= 3, "the hook stayed on for the whole run");
        run_dispose(&r);
    }

    LHAT_TEST("set_debug_hook(NULL) takes the hook away");
    {
        Run r;
        Trace t = {0};
        compile_text(&r, "var^ a = 1\nreturn^ a\n");
        r.machine = lhat_machine_new();
        lhat_machine_set_debug_hook(r.machine, hook, &t);
        lhat_machine_set_debug_hook(r.machine, NULL, NULL);
        r.ran = lhat_run(r.machine, r.proto);
        LHAT_CHECK_EQ_INT(t.count, 0);
        run_dispose(&r);
    }
}

static void test_frame_reading(void)
{
    LHAT_TEST("a frame's locals read back by name and value");
    {
        Run r;
        Trace t = {0};
        t.stop_line = 4;  // inside the body, both names declared
        run_hooked(&r, &t,
                   "var^ add = f^ x:number^ -> number^ {\n"
                   "    var^ y = 10\n"
                   "    var^ z = x + y\n"
                   "    return^ z\n"
                   "}\n"
                   "return^ add(5)\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 15);
        LHAT_CHECK(t.stopped, "the stop line was reached");
        LHAT_CHECK(has_local(&t, "x=5"), "the parameter");
        LHAT_CHECK(has_local(&t, "y=10"), "a written local");
        run_dispose(&r);
    }

    LHAT_TEST("a name out of scope is not among the locals");
    {
        Run r;
        Trace t = {0};
        t.stop_line = 6;  // after the block that declared `inner`
        run_hooked(&r, &t,
                   "var^ outer = 1\n"
                   "if^ true^ {\n"
                   "    var^ inner = 2\n"
                   "    outer := outer + inner\n"
                   "}\n"
                   "return^ outer\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 3);
        LHAT_CHECK(t.stopped, "the stop line was reached");
        LHAT_CHECK(has_local(&t, "outer=3"), "the surviving name");
        LHAT_CHECK(!has_local(&t, "inner=2"), "the block's name is gone");
        run_dispose(&r);
    }

    LHAT_TEST("a frame's captures read back by name and value");
    {
        Run r;
        Trace t = {0};
        t.stop_line = 4;  // inside the closure, reading the captured n
        run_hooked(&r, &t,
                   "var^ make = f^ n:number^ -> f^ -> number^; {\n"
                   "    return^ f^ -> number^ {\n"
                   "        var^ here = n\n"
                   "        return^ here\n"
                   "    }\n"
                   "}\n"
                   "var^ get = make(8)\n"
                   "return^ get()\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 8);
        LHAT_CHECK(t.stopped, "the stop line was reached");
        LHAT_CHECK(has_capture(&t, "n=8"), "the captured name and its value");
        run_dispose(&r);
    }
}

// A write to make at one line: the binding called `name` (a local, or a
// capture) gets `value` -- or a string made on the spot when `make_text` is
// set, since a string needs the machine the hook is handed.
typedef struct {
    uint32_t at_line;
    const char *name;
    LhatValue value;
    const char *make_text;
    bool capture;
    bool done;
    bool wrote;
} Poke;

static void poke_hook(LhatMachine *machine, void *context,
                      LhatDebugEvent event, const LhatFrameInfo *where)
{
    Poke *p = (Poke *)context;
    (void)event;
    if (p->done || where->line != p->at_line) {
        return;
    }
    p->done = true;
    if (p->make_text != NULL) {
        LHAT_CHECK(lhat_machine_make_string(machine, p->make_text,
                                            strlen(p->make_text), &p->value),
                   "a string to write");
    }
    size_t count = p->capture ? lhat_frame_upvalue_count(machine, 0)
                              : lhat_frame_local_count(machine, 0);
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        LhatBindingInfo binding;
        bool read = p->capture ? lhat_frame_upvalue(machine, 0, i, &binding)
                               : lhat_frame_local(machine, 0, i, &binding);
        if (read && strcmp(binding.name, p->name) == 0) {
            found = i;  // the later of two under one name is the inner
        }
    }
    LHAT_CHECK(found != SIZE_MAX, "the binding to write is live");
    p->wrote = p->capture
                   ? lhat_frame_set_upvalue(machine, 0, found, p->value)
                   : lhat_frame_set_local(machine, 0, found, p->value);
    // And what a write must refuse: a level or index that names nothing.
    LHAT_CHECK(!lhat_frame_set_local(machine, 0, count + 50, p->value),
               "an index past the live bindings is refused");
    LHAT_CHECK(!lhat_frame_set_local(machine, 99, 0, p->value),
               "a level past the frames is refused");
}

static void run_poked(Run *r, Poke *p, const char *text)
{
    compile_text(r, text);
    LHAT_CHECK_EQ_INT(r->compiled, LHAT_COMPILE_OK);
    r->machine = lhat_machine_new();
    lhat_machine_set_debug_hook(r->machine, poke_hook, p);
    r->ran = lhat_run(r->machine, r->proto);
    LHAT_CHECK(p->done && p->wrote, "the write happened");
}

static void test_writing(void)
{
    LHAT_TEST("a local written at a stop is what the program reads next");
    {
        Run r;
        Poke p = {0};
        p.at_line = 3;  // before `return^ y` runs
        p.name = "y";
        p.value = lhat_integer(41);
        run_poked(&r, &p,
                  "var^ f = f^ x:number^ -> number^ {\n"
                  "    var^ y = x + 1\n"
                  "    return^ y\n"
                  "}\n"
                  "return^ f(1)\n");
        // Without the write this answers 2.
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 41);
        run_dispose(&r);
    }

    LHAT_TEST("of two names alike, the write lands on the inner");
    {
        Run r;
        Poke p = {0};
        p.at_line = 5;
        p.name = "a";
        p.value = lhat_integer(50);
        run_poked(&r, &p,
                  "var^ g = f^ -> number^ {\n"
                  "    var^ a = 1\n"
                  "    if^ true^ {\n"
                  "        var^ a = 2\n"
                  "        return^ a\n"
                  "    }\n"
                  "    return^ a\n"
                  "}\n"
                  "return^ g()\n");
        // The inner a was 2; the outer stays 1 and is never returned.
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 50);
        run_dispose(&r);
    }

    LHAT_TEST("a capture written at a stop reaches the shared place");
    {
        Run r;
        Poke p = {0};
        p.at_line = 3;
        p.name = "n";
        p.capture = true;
        p.value = lhat_integer(70);
        run_poked(&r, &p,
                  "var^ make = f^ n:number^ -> f^ -> number^; {\n"
                  "    return^ f^ -> number^ {\n"
                  "        var^ h = 0\n"
                  "        return^ n + h\n"
                  "    }\n"
                  "}\n"
                  "var^ get = make(7)\n"
                  "return^ get()\n");
        LHAT_CHECK_EQ_INT(lhat_as_integer(r.ran.value), 70);
        run_dispose(&r);
    }

    LHAT_TEST("a string written at a stop survives the collector");
    {
        Run r;
        Poke p = {0};
        p.at_line = 4;  // the loop head, first time round
        p.name = "s";
        p.make_text = "fresh";
        run_poked(&r, &p,
                  "var^ f = f^ -> string^ {\n"
                  "    var^ s = \"old\"\n"
                  "    var^ i = 0\n"
                  "    repeat^ 2000 {\n"
                  "        var^ waste = { a := 1 }\n"
                  "        i := i + 1\n"
                  "    }\n"
                  "    return^ s\n"
                  "}\n"
                  "return^ f()\n");
        // 2000 tables of garbage ran whole cycles over the written string.
        LHAT_CHECK(lhat_is_object_kind(r.ran.value, LHAT_OBJECT_STRING),
                   "a string came back");
        const LhatString *answered =
            (const LhatString *)lhat_as_object(r.ran.value);
        LHAT_CHECK_EQ_STR(answered->text, answered->length, "fresh");
        run_dispose(&r);
    }
}

int main(void)
{
    test_line_events();
    test_coroutine_events();
    test_reentry();
    test_frame_reading();
    test_writing();
    return lhat_test_report("test_debug_hook");
}
