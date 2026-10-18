// lhat/prompt.h: 03 §4's loop as a host drives it, through the public
// headers alone -- which is what the prompt is for.

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lhat.h"
#include "lhat/prompt.h"
#include "testutil.h"

static void host_seven(LhatMachine *machine, void *context,
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

typedef struct {
    LhatProgram *program;
    LhatMachine *machine;
    LhatPrompt *prompt;
    char report[1024];
} Console;

static void console_open(Console *c, bool strict)
{
    c->program = lhat_program_new(strict, NULL, NULL);
    lhat_register_func(c->program, "game", "seven", "f^ -> number^;",
                       host_seven, NULL);
    c->machine = lhat_machine_new();
    lhat_program_install(c->program, c->machine);
    c->prompt = lhat_prompt_new(c->program, c->machine, "console");
    LHAT_CHECK(c->prompt != NULL, "a prompt was made");
}

static void console_close(Console *c)
{
    lhat_prompt_dispose(c->prompt);
    lhat_machine_dispose(c->machine);
    lhat_program_free(c->program);
}

// Feeds `text` and keeps what it was told in c->report.
static LhatPromptResult feed(Console *c, const char *text)
{
    LhatPromptResult r = lhat_prompt_feed(c->prompt, text, strlen(text));
    lhat_prompt_report(c->prompt, c->report, sizeof c->report);
    return r;
}

static bool told(const Console *c, const char *part)
{
    return strstr(c->report, part) != NULL;
}

static void test_answers(void)
{
    LHAT_TEST("names bound by one input are there for the next (4.3)");
    Console c;
    console_open(&c, true);
    LhatPromptResult r = feed(&c, "let^ x = 2\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK(lhat_is_nil(r.value), "a binding answers nil^");
    LHAT_CHECK_EQ_INT(c.report[0], '\0');
    r = feed(&c, "x * 3\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 6);

    LHAT_TEST("what the host registered is what the prompt reaches");
    r = feed(&c, "import^ game\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    r = feed(&c, "game.seven() + x\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 9);
    console_close(&c);
}

static void test_carrying(void)
{
    LHAT_TEST("an input that stopped early is carried, in pieces of any size");
    Console c;
    console_open(&c, true);
    LhatPromptResult r = feed(&c, "1 +\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_MORE);
    LHAT_CHECK_EQ_INT(c.report[0], '\0');
    r = feed(&c, "2 *");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_MORE);
    r = feed(&c, " 3\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 7);

    LHAT_TEST("drop throws away what is carried, and says whether it did");
    LHAT_CHECK(!lhat_prompt_drop(c.prompt), "nothing carried after a whole input");
    r = feed(&c, "1 +\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_MORE);
    LHAT_CHECK(lhat_prompt_drop(c.prompt), "the carried input was dropped");
    r = feed(&c, "4\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK_EQ_INT(lhat_as_integer(r.value), 4);
    console_close(&c);
}

static void test_reports(void)
{
    LHAT_TEST("a refused input is told about and leaves the session as it was");
    Console c;
    console_open(&c, true);
    LhatPromptResult r = feed(&c, "let^ z = nowhere\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_REFUSED);
    LHAT_CHECK(told(&c, "(console)1:10: error:"), "told: %s", c.report);
    LHAT_CHECK(told(&c, "let^ z = nowhere\n"), "the line is quoted: %s",
               c.report);
    r = feed(&c, "z\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_REFUSED);
    r = feed(&c, "let^ z = 1\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK_EQ_INT(c.report[0], '\0');

    LHAT_TEST("a run error is told with its traceback");
    r = feed(&c, "let^ g = f^ { panic^ \"boom\" }\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    r = feed(&c, "let^ h = f^ { g()\nreturn^ 1 }\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    r = feed(&c, "h()\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_FAILED);
    LHAT_CHECK(strncmp(c.report, "error: line 1: boom\ntraceback:", 30) == 0,
               "told: %s", c.report);
    LHAT_CHECK(told(&c, "in g") && told(&c, "in h"), "told: %s", c.report);

    LHAT_TEST("the report measures with (NULL, 0) and cuts to what fits");
    size_t needed = lhat_prompt_report(c.prompt, NULL, 0);
    LHAT_CHECK_EQ_INT(needed, strlen(c.report));
    char small[6];
    LHAT_CHECK_EQ_INT(lhat_prompt_report(c.prompt, small, sizeof small), needed);
    LHAT_CHECK(strcmp(small, "error") == 0, "cut to: %s", small);
    console_close(&c);

    LHAT_TEST("relaxed tells its warnings about an input it still runs");
    console_open(&c, false);
    r = feed(&c, "let^ k = f^ v { return^ v }\n");
    LHAT_CHECK_EQ_INT(r.status, LHAT_PROMPT_RAN);
    LHAT_CHECK(told(&c, "warning:"), "told: %s", c.report);
    console_close(&c);
}

int main(void)
{
    test_answers();
    test_carrying();
    test_reports();
    return lhat_test_report("test_prompt");
}
