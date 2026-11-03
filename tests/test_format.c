// L^ (lhat) -- tests for the formatter ([lsp.format]).
//
// Every case is formatted twice: what the formatter writes, it must leave as
// it is. lhat_format reads its own output back and refuses a result whose
// tokens differ, so a case that comes back OK has kept them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format.h"
#include "lhat/port.h"
#include "testutil.h"

static LhatFormatOptions defaults(void)
{
    LhatFormatOptions options;
    lhat_format_defaults(&options);
    return options;
}

static void expect(const char *input, const LhatFormatOptions *options,
                   const char *expected)
{
    char *out = NULL;
    size_t length = 0;
    LhatFormatStatus status =
        lhat_format(input, strlen(input), false, options, &out, &length);
    LHAT_REQUIRE(status == LHAT_FORMAT_OK, "status %d for:\n%s", (int)status, input);
    LHAT_CHECK(length == strlen(expected) && memcmp(out, expected, length) == 0,
               "got:\n%.*s\nwanted:\n%s", (int)length, out, expected);

    char *again = NULL;
    size_t again_length = 0;
    status = lhat_format(out, length, false, options, &again, &again_length);
    LHAT_CHECK(status == LHAT_FORMAT_OK && again_length == length &&
                   memcmp(again, out, length) == 0,
               "not stable; the second pass wrote:\n%.*s", (int)again_length, again);
    lhat_free(again);
    lhat_free(out);
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

int main(void)
{
    LhatFormatOptions o = defaults();

    LHAT_TEST("[lsp.format.hat]: nothing follows a hat but the next word");
    expect("let^ f = f^ a -> number^ { 0 }\n", &o,
           "let^f = f^a -> number^{ 0 }\n");
    expect("let^ x = self^ + 1\n", &o, "let^x = self^+ 1\n");

    LHAT_TEST("[lsp.format.hat]: lhat_space_after_hat puts the space back");
    {
        LhatFormatOptions spaced = defaults();
        spaced.space_after_hat = true;
        expect("let^f = f^a -> number^{ 0 }\n", &spaced,
               "let^ f = f^ a -> number^ { 0 }\n");
    }

    LHAT_TEST("[lsp.format.spacing]: one space between words, none inside calls");
    expect("let^x  =  foo ( 1 ,2 )+bar[ 3 ]\n", &o,
           "let^x = foo(1, 2) + bar[3]\n");
    expect("let^x = - 1 * ( -y )\n", &o, "let^x = -1 * (-y)\n");
    expect("var^ts : t^{ std.task.Task<number^> [] } = {}\n"
           "var^ns : t^{ Name [9] } = {}\n", &o,
           "var^ts : t^{ std.task.Task<number^>[] } = {}\n"
           "var^ns : t^{ Name[9] } = {}\n");
    expect("let^t = {a = 1, b = 2}\nlet^u = { a = 1 }\nlet^v = { }\n", &o,
           "let^t = {a = 1, b = 2}\nlet^u = { a = 1 }\nlet^v = {}\n");

    LHAT_TEST("an interpolated string is written as it was");
    expect("print( $\"a {  x  } b\" )\n", &o, "print($\"a {  x  } b\")\n");

    LHAT_TEST("indentation follows the braces, whatever it was");
    expect("let^f = p^x {\n"
           "if^x > 1 {\n"
           "        print(x)\n"
           "  }\n"
           "}\n", &o,
           "let^f = p^x {\n"
           "    if^x > 1 {\n"
           "        print(x)\n"
           "    }\n"
           "}\n");

    LHAT_TEST("blank lines shrink to one, trailing blanks go");
    expect("let^a = 1   \n\n\n\nlet^b = 2\n\n\n", &o,
           "let^a = 1\n\nlet^b = 2\n");

    LHAT_TEST("comments stay where they were written");
    expect("let^a = 1  # one\n"
           "#[ block\n"
           "   comment ]#\n"
           "let^b = 2\n", &o,
           "let^a = 1  # one\n"
           "#[ block\n"
           "   comment ]#\n"
           "let^b = 2\n");

    LHAT_TEST("[lsp.format.labels]: a label stands with its construct");
    expect("let^f = p^x {\n"
           "    if^x = 1 {\n"
           "        print(1)\n"
           "        el^:\n"
           "        print(2)\n"
           "    }\n"
           "}\n", &o,
           "let^f = p^x {\n"
           "    if^x = 1 {\n"
           "        print(1)\n"
           "    el^:\n"
           "        print(2)\n"
           "    }\n"
           "}\n");
    expect("let^f = p^x {\n"
           "    for^x {\n"
           "        when^1:\n"
           "            print(1)\n"
           "    }\n"
           "}\n", &o,
           "let^f = p^x {\n"
           "    for^x {\n"
           "    when^1:\n"
           "        print(1)\n"
           "    }\n"
           "}\n");

    LHAT_TEST("[lsp.format.labels]: a match written with ':' indents its clauses");
    expect("let^label = for^mode:\n"
           "when^1: \"one\"\n"
           "other^: \"other\"\n"
           ";\n", &o,
           "let^label = for^mode:\n"
           "    when^1: \"one\"\n"
           "    other^: \"other\"\n"
           ";\n");

    LHAT_TEST("for^ definitions stand in a row");
    expect("let^g = f^x {\n"
           "    for^let^y = x\n"
           "    for^let^z = y\n"
           "    do^: z + 1;\n"
           "}\n", &o,
           "let^g = f^x {\n"
           "    for^let^y = x\n"
           "    for^let^z = y\n"
           "    do^: z + 1;\n"
           "}\n");

    LHAT_TEST("a body opened after a long head is one deeper than the head");
    expect("let^g:f^number^\n"
           "    -> number^; = f^x {\n"
           "x\n"
           "}\n", &o,
           "let^g:f^number^\n"
           "    -> number^; = f^x {\n"
           "    x\n"
           "}\n");

    LHAT_TEST("[lsp.format.braces]: same_line pulls a block's brace up");
    expect("let^f = p^x\n"
           "{\n"
           "    if^x\n"
           "    {\n"
           "        print(1)\n"
           "    el^:\n"
           "        print(2)\n"
           "    }\n"
           "}\n", &o,
           "let^f = p^x {\n"
           "    if^x {\n"
           "        print(1)\n"
           "    el^:\n"
           "        print(2)\n"
           "    }\n"
           "}\n");

    LHAT_TEST("[lsp.format.braces]: next_line puts it on a line of its own");
    {
        LhatFormatOptions next = defaults();
        next.brace_next_line = true;
        expect("let^f = p^x {\n"
               "    if^x {\n"
               "        print(1)\n"
               "    el^:\n"
               "        print(2)\n"
               "    }\n"
               "    if^x { print(3) }\n"
               "}\n", &next,
               "let^f = p^x\n"
               "{\n"
               "    if^x\n"
               "    {\n"
               "        print(1)\n"
               "    el^:\n"
               "        print(2)\n"
               "    }\n"
               "    if^x { print(3) }\n"
               "}\n");
    }

    LHAT_TEST("[lsp.format.length]: a long line breaks inside its widest brackets");
    {
        LhatFormatOptions narrow = defaults();
        narrow.max_line_length = 30;
        expect("print(alpha, beta(gamma, delta), epsilon)\n", &narrow,
               "print(\n"
               "    alpha,\n"
               "    beta(gamma, delta),\n"
               "    epsilon\n"
               ")\n");
    }

    LHAT_TEST("tabs, and the line ends the input used");
    {
        LhatFormatOptions tabs = defaults();
        tabs.indent_with_tabs = true;
        expect("let^f = p^{\r\nprint(1)\r\n}\r\n", &tabs,
               "let^f = p^{\r\n\tprint(1)\r\n}\r\n");
    }

    LHAT_TEST("input that does not read is not formatted");
    {
        char *out = NULL;
        size_t length = 0;
        LHAT_CHECK(lhat_format("let^ = (", 8, false, &o, &out, &length) ==
                       LHAT_FORMAT_SYNTAX_ERROR,
                   "a syntax error should be reported");
        LHAT_CHECK(out == NULL, "nothing should be written");
    }

    LHAT_TEST("[lsp.format.settings]: .editorconfig, nearest last, down to root");
    {
        write_file(".editorconfig",
                   "root = true\n"
                   "[*]\n"
                   "indent_size = 8\n"
                   "max_line_length = 80\n"
                   "[*.{lh,lton}]\n"
                   "indent_style = tab\n"
                   "[sub/**.lh]\n"
                   "lhat_brace_style = next_line\n");
#ifdef _WIN32
        (void)system("mkdir sub 2>NUL");
#else
        (void)system("mkdir -p sub");
#endif
        write_file("sub/.editorconfig",
                   "[a.lh]\n"
                   "indent_size = 2\n"
                   "indent_style = space\n"
                   "lhat_space_after_hat = true\n"
                   "max_line_length = off\n"
                   "[file{1..3}.lh]\n"
                   "end_of_line = crlf\n");
        LhatFormatOptions read = defaults();
        lhat_format_read_editorconfig("./sub/a.lh", &read);
        LHAT_CHECK_EQ_INT(read.indent_size, 2);
        LHAT_CHECK_EQ_BOOL(read.indent_with_tabs, false);
        LHAT_CHECK_EQ_BOOL(read.space_after_hat, true);
        LHAT_CHECK_EQ_BOOL(read.brace_next_line, true);
        LHAT_CHECK_EQ_INT(read.max_line_length, 0);
        LHAT_CHECK_EQ_INT(read.end_of_line, LHAT_EOL_KEEP);

        read = defaults();
        lhat_format_read_editorconfig("./sub/file2.lh", &read);
        LHAT_CHECK_EQ_INT(read.indent_size, 8);
        LHAT_CHECK_EQ_BOOL(read.indent_with_tabs, true);
        LHAT_CHECK_EQ_INT(read.max_line_length, 80);
        LHAT_CHECK_EQ_INT(read.end_of_line, LHAT_EOL_CRLF);

        read = defaults();
        lhat_format_read_editorconfig("./other.txt", &read);
        LHAT_CHECK_EQ_INT(read.indent_size, 8);
        LHAT_CHECK_EQ_BOOL(read.indent_with_tabs, false);
        LHAT_CHECK_EQ_BOOL(read.brace_next_line, false);
    }

    return lhat_test_report("test_format");
}
