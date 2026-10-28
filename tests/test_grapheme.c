// L^ (lhat) -- Unicode 17 UAX #29 conformance and byte-string boundaries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grapheme.h"
#include "lhat/object.h"
#include "testutil.h"

static size_t encode(char *out, unsigned long cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63));
    out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static bool check_case(char *line)
{
    char text[4096];
    size_t boundaries[1024], count = 0, length = 0;
    char *comment = strchr(line, '#');
    if (comment != NULL) *comment = '\0';
    for (char *token = strtok(line, " \t\r\n"); token != NULL;
         token = strtok(NULL, " \t\r\n")) {
        if (count == 1024 || length + 4 > sizeof text) return false;
        if (strcmp(token, "\xC3\xB7") == 0) boundaries[count++] = length;
        else if (strcmp(token, "\xC3\x97") != 0) {
            length += encode(text + length, strtoul(token, NULL, 16));
        }
    }
    if (count < 2 || boundaries[0] != 0 || boundaries[count - 1] != length)
        return false;
    LhatHeap heap = {0};
    LhatString *s = lhat_string_new(&heap, text, length);
    bool ok = s != NULL && lhat_string_characters(s) == count - 1;
    size_t at = 0;
    for (size_t i = 0; i < count && ok; i++) {
        ok = at == boundaries[i] && lhat_string_byte_at(s, i) == boundaries[i];
        at = lhat_grapheme_next(text, length, at);
    }
    lhat_object_free_all(&heap);
    return ok;
}

static void test_conformance(void)
{
    LHAT_TEST("all Unicode 17 extended grapheme break cases");
    FILE *file = fopen(LHAT_SOURCE_DIR "/tests/data/GraphemeBreakTest-17.0.0.txt", "rb");
    LHAT_CHECK(file != NULL, "open Unicode conformance data");
    char line[8192];
    size_t cases = 0, line_number = 0, failed_line = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        line_number++;
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        cases++;
        if (!check_case(line)) { failed_line = line_number; break; }
    }
    bool read_ok = !ferror(file);
    fclose(file);
    LHAT_CHECK(read_ok, "read Unicode conformance data");
    LHAT_CHECK_EQ_INT(failed_line, 0);
    LHAT_CHECK_EQ_INT(cases, 766);
}

static void test_byte_strings(void)
{
    LHAT_TEST("invalid UTF-8 bytes are separate clusters and never lost");
    const char text[] = "a\x80\xFF\xE3\0b";
    size_t length = sizeof text - 1;
    LHAT_CHECK_EQ_INT(lhat_grapheme_count(text, length), length);
    for (size_t i = 0; i < length; i++) {
        LHAT_CHECK_EQ_INT(lhat_grapheme_next(text, length, i), i + 1);
    }
    LHAT_CHECK_EQ_INT(lhat_grapheme_next(NULL, 0, 0), 0);
    LHAT_CHECK_EQ_INT(lhat_grapheme_count(NULL, 0), 0);

    LHAT_TEST("concatenation resegments combining marks and CRLF at the seam");
    const char *left[] = {"\xE3\x81\x8B", "\r", "\xF0\x9F\x87\xAF"};
    const char *right[] = {"\xE3\x82\x99", "\n", "\xF0\x9F\x87\xB5"};
    LhatHeap heap = {0};
    bool ok = true;
    for (size_t i = 0; i < sizeof left / sizeof left[0]; i++) {
        LhatString *a = lhat_string_new(&heap, left[i], strlen(left[i]));
        LhatString *b = lhat_string_new(&heap, right[i], strlen(right[i]));
        LhatString *joined = a && b ? lhat_string_concat(&heap, a, b) : NULL;
        ok = ok && joined != NULL && joined->characters == 1;
    }
    lhat_object_free_all(&heap);
    LHAT_CHECK(ok, "joined text is one cluster");
}

int main(void)
{
    test_conformance();
    test_byte_strings();
    return lhat_test_report("test_grapheme");
}
