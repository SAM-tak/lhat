// L^ (lhat) -- tests for std.regex.
//
// The engine's own dialect is pinned through the module rather than by
// linking the engine directly: what a program sees is the acceptance test,
// and the smoke of the engine's corners (classes over UTF-8, laziness,
// captures under backtracking) reads the same either way. Every case asks
// its question from inside L^ and answers a value the C side checks.

#include "stdlibutil.h"
#include "testutil.h"

#include "../stdlib/regex.h"

static const LhatTestRegister regs[] = {lhatstdlib_regex_register};

static LhatTestRan run_source(const char *text)
{
    return lhat_test_run(regs, 1, text);
}

// One compiled pattern, narrowed, used, disposed. `body` names it `r`.
// 13.11 is what narrows it: inside the branch `made` is the arm that fitted,
// so binding it takes that type and no cast is wanted.
#define WITH_REGEX(pattern, body)              \
    "import^ std.regex\n"                      \
    "let^ made = std.regex.new(" pattern ")\n" \
    "if^ made fits^ std.regex.Regex {\n"        \
    "    let^ r = made\n" body                 \
    "}\n"                                      \
    "return^ \"never\"\n"

static void test_compile(void)
{
    LHAT_TEST("new answers a Regex for a good pattern");
    {
        LhatTestRan ran = run_source(WITH_REGEX("\"a+b\"",
                                                "    r.dispose()\n"
                                                "    return^ \"ok\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "ok");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and a BadPattern for a broken one");
    {
        LhatTestRan ran = run_source(
            "import^ std.regex\n"
            "let^ made = std.regex.new(\"(abc\")\n"
            "return^ if^ made fits^ std.regex.Error.BadPattern: made.message "
            "el^: \"?\" ;\n");
        LHAT_CHECK_RAN_TEXT(ran, "the pattern ends inside (...)");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_match(void)
{
    LHAT_TEST("match answers the leftmost stand");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\d+-\\\\d+\"",
            "    let^ hit = r.match(\"ab 12-34 cd\")\n"
            "    r.dispose()\n"
            "    return^ hit ?? \"none\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "12-34");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and nil^ where nothing stands");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\d\"",
            "    let^ hit = r.match(\"abc\")\n"
            "    r.dispose()\n"
            "    return^ hit ?? \"none\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "none");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("captures answers the whole and the groups");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"(\\\\d+)-(\\\\d+)\"",
            "    let^ caps = r.captures(\"12-34\")\n"
            "    r.dispose()\n"
            "    if^ caps? {\n"
            "        return^ (caps[1] ?? \"\") .. \"/\" .. (caps[2] ?? \"\")"
            " .. \"/\" .. (caps[3] ?? \"\")\n"
            "    }\n"
            "    return^ \"none\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "12-34/12/34");
        lhat_test_ran_dispose(&ran);
    }

    // 02 の 14.19's counting: ordinals are characters, so a UTF-8 subject
    // answers the same ordinal a substring call would use.
    LHAT_TEST("a class ranges over code points");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"[あ-ん]+\"",
            "    let^ hit = r.match(\"xかなy\")\n"
            "    r.dispose()\n"
            "    return^ hit ?? \"none\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "かな");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_gmatch(void)
{
    LHAT_TEST("gmatch walks every stand with its ordinal");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\w+\"",
            "    var^ log = {}\n"
            "    for^ i, w in^ r.gmatch(\"one two three\") {\n"
            "        log.push^(w .. \"@\" .. i.tostring())\n"
            "    }\n"
            "    r.dispose()\n"
            "    return^ log.join^(\",\")\n"));
        LHAT_CHECK_RAN_TEXT(ran, "one@1,two@5,three@9");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("the walk survives the pattern being disposed");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\d\"",
            "    let^ walk = r.gmatch(\"1a2\")\n"
            "    r.dispose()\n"
            "    var^ total = \"\"\n"
            "    for^ i, d in^ walk { total := total .. d }\n"
            "    return^ total\n"));
        LHAT_CHECK_RAN_TEXT(ran, "12");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_gsub(void)
{
    LHAT_TEST("a written replacement expands its group references");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"(\\\\d+)-(\\\\d+)\"",
            "    let^ swapped = r.gsub(\"12-34 and 56-78\", \"[$2/$1]\")\n"
            "    r.dispose()\n"
            "    return^ if^ swapped fits^ string^: swapped el^: \"?\" ;\n"));
        LHAT_CHECK_RAN_TEXT(ran, "[34/12] and [78/56]");
        lhat_test_ran_dispose(&ran);
    }

    // 3.4改: the callback's three parameters read off the seat's one func
    // arm, so nothing is written on them.
    LHAT_TEST("a function replacement is handed match, ordinal and captures");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"(\\\\d+)\"",
            "    let^ swapped = r.gsub(\"a1b22\", f^ m, at, caps {\n"
            "        return^ \"<\" .. (caps[1] ?? \"\") .. \"@\" .. "
            "at.tostring() .. \">\"\n"
            "    })\n"
            "    r.dispose()\n"
            "    return^ if^ swapped fits^ string^: swapped el^: \"?\" ;\n"));
        LHAT_CHECK_RAN_TEXT(ran, "a<1@2>b<22@4>");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a nil^ answer keeps the original match");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\d+\"",
            "    let^ swapped = r.gsub(\"1 and 22\", f^ m, at, caps {\n"
            "        return^ if^ m = \"1\": \"one\" el^: nil^ ;\n"
            "    })\n"
            "    r.dispose()\n"
            "    return^ if^ swapped fits^ string^: swapped el^: \"?\" ;\n"));
        LHAT_CHECK_RAN_TEXT(ran, "one and 22");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_split(void)
{
    LHAT_TEST("split keeps the pieces between the matches, empties too");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\"\\\\s*,\\\\s*\"",
            "    let^ parts = r.split(\"a , b,,c\")\n"
            "    r.dispose()\n"
            "    if^ parts fits^ t^{} {\n"
            "        return^ \"[\" .. parts.join^(\"|\") .. \"]\"\n"
            "    }\n"
            "    return^ \"?\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "[a|b||c]");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("a subject with no match answers itself whole");
    {
        LhatTestRan ran = run_source(WITH_REGEX(
            "\",\"",
            "    let^ parts = r.split(\"abc\")\n"
            "    r.dispose()\n"
            "    if^ parts fits^ t^{} {\n"
            "        return^ parts.join^(\"|\") .. \"#\" .. "
            "parts.count^.tostring()\n"
            "    }\n"
            "    return^ \"?\"\n"));
        LHAT_CHECK_RAN_TEXT(ran, "abc#1");
        lhat_test_ran_dispose(&ran);
    }
}

static void test_module_forms(void)
{
    LHAT_TEST("the convenience forms compile on the spot");
    {
        LhatTestRan ran = run_source(
            "import^ std.regex\n"
            "let^ hit = std.regex.match(\"l+\", \"hello\")\n"
            "return^ if^ hit fits^ string^: hit el^: \"none\" ;\n");
        LHAT_CHECK_RAN_TEXT(ran, "ll");
        lhat_test_ran_dispose(&ran);
    }

    LHAT_TEST("and hand a bad pattern back as the error kind");
    {
        LhatTestRan ran = run_source(
            "import^ std.regex\n"
            "let^ hit = std.regex.match(\"(a\", \"aaa\")\n"
            "return^ if^ hit fits^ std.regex.Error.BadPattern: \"bad\" "
            "el^: \"?\" ;\n");
        LHAT_CHECK_RAN_TEXT(ran, "bad");
        lhat_test_ran_dispose(&ran);
    }

    // The budget is the answer to a backtracking blowup -- reported as its
    // own kind, never a hang and never a stack.
    LHAT_TEST("a pathological pattern answers Exhausted");
    {
        LhatTestRan ran = run_source(
            "import^ std.regex\n"
            "let^ hit = std.regex.match(\"(a+)+b\", "
            "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\")\n"
            "return^ if^ hit fits^ std.regex.Error.Exhausted: \"blown\" "
            "el^: \"?\" ;\n");
        LHAT_CHECK_RAN_TEXT(ran, "blown");
        lhat_test_ran_dispose(&ran);
    }
}

int main(void)
{
    test_compile();
    test_match();
    test_gmatch();
    test_gsub();
    test_split();
    test_module_forms();
    return lhat_test_report("test_regex");
}
