// L^ (lhat) -- sequential Unicode walks and exact byte-offset slicing.
#include "fixture.h"

static void test_chars(void)
{
    Run r;
    LHAT_TEST("chars yields complete graphemes, including combining and emoji sequences");
    run_checked_text(&r,
        "var^ out = \"\"\n"
        "for^ ch in^ \"か\\u{3099}🇯🇵👨‍👩‍👧\\r\\n\".chars() { out := out .. ch .. \"|\" }\n"
        "return^ out\n");
    CHECK_STRING(&r, "か\xE3\x82\x99|🇯🇵|👨‍👩‍👧|\r\n|");
    run_dispose(&r);

    LHAT_TEST("each chars call has an independent cursor and can be called from f^");
    run_checked_text(&r,
        "let^ count = f^ s:string^ -> number^ {\n"
        " var^ n = 0\n"
        " for^ a in^ s.chars() { for^ b in^ s.chars() { n += 1 } }\n"
        " return^ n }\n"
        "return^ count(\"か\\u{3099}X\")\n");
    CHECK_INTEGER(&r, 4);
    run_dispose(&r);

    LHAT_TEST("chars retains a temporary source through collections");
    run_checked_text(&r,
        "let^ parts = {}\n"
        "for^ ch in^ (\"か\" .. \"\\u{3099}Z\").chars() {\n"
        " L^.gc.collect()\nparts.push^(ch) }\n"
        "return^ parts.join^(\"|\")\n");
    CHECK_STRING(&r, "か\xE3\x82\x99|Z");
    run_dispose(&r);

    LHAT_TEST("an empty chars walk yields nothing");
    run_checked_text(&r,
        "var^ n = 0\nfor^ ch in^ \"\".chars() { n += 1 }\nreturn^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);

    LHAT_TEST("string walks satisfy their f^ coroutine contracts at runtime");
    run_checked_text(&r,
        "let^ chars = f^ w:c^{f^ -> string^} -> number^ {\n"
        " var^ n = 0\nfor^ ch in^ w { n += ch.size }\nreturn^ n }\n"
        "let^ points = f^ w:c^{f^ -> number^} -> number^ {\n"
        " var^ n = 0\nfor^ cp in^ w { n += 1 }\nreturn^ n }\n"
        "return^ chars(\"か\\u{3099}\".chars()) * 10 + points(\"か\\u{3099}\".codepoints())\n");
    CHECK_INTEGER(&r, 62);
    run_dispose(&r);
}

static void test_codepoints(void)
{
    Run r;
    LHAT_TEST("codepoints yields scalar integers instead of grapheme strings");
    run_checked_text(&r,
        "let^ points = {}\n"
        "for^ cp in^ \"か\\u{3099}😀\\u{0}\".codepoints() { points.push^(cp) }\n"
        "return^ points.length^ = 4 and^ points[0] = 12363 and^ "
        "points[1] = 12441 and^ points[2] = 128512 and^ points[3] = 0\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("invalid UTF-8 consumes one byte per replacement codepoint");
    run_checked_text(&r,
        "let^ points = {}\n"
        "for^ cp in^ \"\\x80\\xffA\".codepoints() { points.push^(cp) }\n"
        "return^ points.length^ = 3 and^ points[0] = 65533 and^ "
        "points[1] = 65533 and^ points[2] = 65\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("chars preserves invalid bytes and embedded NUL exactly");
    run_checked_text(&r,
        "let^ pieces = {}\n"
        "for^ ch in^ \"\\x80\\xff\\u{0}A\".chars() { pieces.push^(ch) }\n"
        "return^ pieces.length^ = 4 and^ pieces.join^(\"\") = \"\\x80\\xff\\u{0}A\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("empty codepoints walks yield nothing");
    run_checked_text(&r,
        "var^ n = 0\nfor^ cp in^ \"\".codepoints() { n += 1 }\nreturn^ n\n");
    CHECK_INTEGER(&r, 0);
    run_dispose(&r);
}

static bool failing_step(LhatMachine *machine, void *context,
                         const LhatValue *sent, size_t sent_count,
                         LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)sent;
    (void)sent_count;
    (void)answers;
    (void)answer_count;
    lhat_machine_panic(machine, lhat_integer(42));
    return false;
}

static void release_counter(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    (*(int *)context)++;
}

// A string-producing step can fail to allocate. The host-walk boundary
// must propagate a failure rather than quietly treating it as normal EOF.
static void test_walk_failure(void)
{
    LHAT_TEST("host-walk panics propagate through C resume, start and compiled loops");
    const char *scripts[] = {
        NULL,
        "return^ L^.walk.start()\n",
        "for^ x in^ L^.walk { }\nreturn^ 0\n",
    };
    for (size_t i = 0; i < sizeof scripts / sizeof scripts[0]; i++) {
        Run r = {0};
        if (scripts[i] != NULL) compile_text(&r, scripts[i]);
        LhatMachine *machine = lhat_machine_new();
        LhatValue co = lhat_nil();
        int released = 0;
        LHAT_CHECK(lhat_machine_make_coroutine(machine, failing_step, &released,
                                              release_counter, lhat_nil(), &co), "made walk");
        LhatRunResult ran;
        if (scripts[i] == NULL) {
            ran = lhat_machine_resume(machine, co, NULL, 0);
        } else {
            LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
            LHAT_CHECK(lhat_machine_set_global(machine, "walk", co), "rooted walk");
            ran = lhat_run(machine, r.proto);
        }
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_PANIC);
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        LHAT_CHECK(lhat_machine_coroutine_done(co), "failed walk is done");
        lhat_machine_dispose(machine);
        LHAT_CHECK_EQ_INT(released, 1);
        if (scripts[i] != NULL) compiled_dispose(&r);
    }
}

static void test_slice_bytes(void)
{
    Run r;
    LHAT_TEST("sliceBytes uses zero-based half-open byte offsets and an optional end");
    run_checked_text(&r,
        "let^ s = \"Aか\\u{3099}Z\"\n"
        "return^ s.sliceBytes(1, 7) .. \"|\" .. s.sliceBytes(7) .. \"|\" .. "
        "s.sliceBytes(1.0, 4.0)\n");
    CHECK_STRING(&r, "か\xE3\x82\x99|Z|か");
    run_dispose(&r);

    LHAT_TEST("sliceBytes neither adjusts Unicode boundaries nor drops NUL bytes");
    run_checked_text(&r,
        "return^ \"か\".sliceBytes(1, 2) = \"\\x81\" and^ "
        "\"a\\u{0}b\".sliceBytes(1, 2) = \"\\u{0}\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("empty and invalid byte ranges answer the empty string");
    run_checked_text(&r,
        "let^ s = \"abc\"\n"
        "return^ s.sliceBytes(2, 2) = \"\" and^ s.sliceBytes(2, 1) = \"\" and^ "
        "s.sliceBytes(4) = \"\" and^ s.sliceBytes(0, 4) = \"\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("a whole byte slice returns the original string");
    run_checked_text(&r,
        "let^ s = \"か\\u{3099}\"\n"
        "return^ s.sliceBytes(0) is^ s and^ s.sliceBytes(0, s.size) is^ s\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);

    LHAT_TEST("byte offsets reject negatives, fractions and non-numbers instead of rounding");
    const char *invalid[] = {
        "return^ \"abc\".sliceBytes(-1)\n",
        "return^ \"abc\".sliceBytes(0, -1)\n",
        "return^ \"abc\".sliceBytes(0.5)\n",
        "return^ \"abc\".sliceBytes(0, 1.5)\n",
        "return^ \"abc\".sliceBytes(\"0\")\n",
        "return^ \"abc\".sliceBytes(number^.nan)\n",
        "return^ \"abc\".sliceBytes(number^.inf)\n",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        run_text(&r, invalid[i]);
        LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_TYPE_ERROR);
        run_dispose(&r);
    }

    LHAT_TEST("text methods retain their arity checks through runtime calls");
    const char *wrong_arity[] = {
        "return^ \"abc\".chars(1)\n",
        "return^ \"abc\".codepoints(1)\n",
        "return^ \"abc\".sliceBytes()\n",
        "return^ \"abc\".sliceBytes(0, 1, 2)\n",
    };
    for (size_t i = 0; i < sizeof wrong_arity / sizeof wrong_arity[0]; i++) {
        run_text(&r, wrong_arity[i]);
        LHAT_CHECK_EQ_INT(r.compiled, LHAT_COMPILE_OK);
        LHAT_CHECK_EQ_INT(r.ran.status, LHAT_RUN_ARITY);
        run_dispose(&r);
    }

    LHAT_TEST("stored text methods retain their receiver and exact iterator type");
    run_checked_text(&r,
        "let^ s = \"か\\u{3099}\"\n"
        "let^ walk = s.chars\nlet^ cut = s.sliceBytes\n"
        "var^ out = \"\"\nfor^ ch in^ walk() { out := out .. ch }\n"
        "return^ out = s and^ cut(0, 3) = \"か\"\n");
    CHECK_BOOL(&r, true);
    run_dispose(&r);
}

int main(void)
{
    test_chars();
    test_codepoints();
    test_slice_bytes();
    test_walk_failure();
    return lhat_test_report("test_string_walk");
}
