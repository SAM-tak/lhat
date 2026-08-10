// L^ (lhat) -- the numeric literal grammar.

#include "number.h"

#include <errno.h>
#include <stdlib.h>

#include "lhatconfig.h"

static bool is_decimal_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_digit_of_base(char c, int base)
{
    switch (base) {
        case 2:  return c == '0' || c == '1';
        case 8:  return c >= '0' && c <= '7';
        case 10: return c >= '0' && c <= '9';
        case 16:
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        default: return false;
    }
}

// Where the lexer reads past its source, this reads past `length`. Both
// answer '\0', so neither has a run of digits ending in a special case.
typedef struct {
    const char *text;
    size_t length;
    size_t at;
} Reader;

static char byte_at(const Reader *r, size_t ahead)
{
    size_t index = r->at + ahead;
    return index >= r->length ? '\0' : r->text[index];
}

static char current_byte(const Reader *r)
{
    return byte_at(r, 0);
}

// Consumes a run of digits in the given base, allowing '_' only between two
// digits. Reports how many digits were seen and whether a separator was
// misplaced.
static void scan_digit_run(Reader *r, int base, size_t *digits, bool *malformed)
{
    *digits = 0;
    *malformed = false;

    bool previous_was_digit = false;
    for (;;) {
        char c = current_byte(r);
        if (c == '_') {
            if (!previous_was_digit || !is_digit_of_base(byte_at(r, 1), base)) {
                *malformed = true;
            }
            r->at++;
            previous_was_digit = false;
            continue;
        }
        if (!is_digit_of_base(c, base)) {
            break;
        }
        r->at++;
        (*digits)++;
        previous_was_digit = true;
    }
}

static bool finish(LhatNumberLiteral *out, LhatNumberStatus status, size_t length)
{
    out->status = status;
    out->length = length;
    return status == LHAT_NUMBER_OK;
}

bool lhat_number_literal(const char *text, size_t length, bool after_dot,
                         LhatNumberLiteral *out)
{
    Reader r;
    r.text = text;
    r.length = length;
    r.at = 0;

    out->kind = LHAT_NUMBER_INTEGER;
    out->integer = 0;
    out->real = 0.0;
    out->base = 10;

    int base = 10;
    bool is_float = false;
    bool malformed = false;
    size_t digits = 0;

    // Section 10.1: immediately after a '.' the digits form an integer key and
    // must not swallow a further '.', so no prefix, fraction or exponent is
    // considered here.
    if (after_dot) {
        bool bad = false;
        scan_digit_run(&r, 10, &digits, &bad);
        malformed = malformed || bad;
    } else {
        if (current_byte(&r) == '0') {
            char marker = byte_at(&r, 1);
            if (marker == 'x' || marker == 'X') {
                base = 16;
            } else if (marker == 'b' || marker == 'B') {
                base = 2;
            } else if (marker == 'o' || marker == 'O') {
                base = 8;
            }
            if (base != 10) {
                r.at += 2;
            }
        }

        bool bad = false;
        scan_digit_run(&r, base, &digits, &bad);
        malformed = malformed || bad;

        if (base == 10) {
            // Section 10.2: the fraction is taken only when a digit follows the
            // '.', so "1..2" scans as INT CONCAT INT.
            if (current_byte(&r) == '.' && is_decimal_digit(byte_at(&r, 1))) {
                r.at++;
                is_float = true;
                size_t fraction_digits = 0;
                scan_digit_run(&r, 10, &fraction_digits, &bad);
                malformed = malformed || bad;
            }

            char exponent = current_byte(&r);
            if (exponent == 'e' || exponent == 'E') {
                size_t sign = (byte_at(&r, 1) == '+' || byte_at(&r, 1) == '-') ? 1 : 0;
                if (!is_decimal_digit(byte_at(&r, 1 + sign))) {
                    // Section 4.5: no backtracking. Q7 makes "1e^3" illegal
                    // anyway, so a malformed exponent is simply an error, and
                    // the 'e' stays consumed.
                    return finish(out, LHAT_NUMBER_BAD_EXPONENT, r.at + 1);
                }
                r.at += 1 + sign;
                is_float = true;
                size_t exponent_digits = 0;
                scan_digit_run(&r, 10, &exponent_digits, &bad);
                malformed = malformed || bad;
            }
        }
    }

    if (digits == 0) {
        malformed = true;
    }

    out->kind = is_float ? LHAT_NUMBER_REAL : LHAT_NUMBER_INTEGER;
    out->base = (uint8_t)base;

    if (malformed) {
        return finish(out, LHAT_NUMBER_MALFORMED, r.at);
    }
    if (r.at >= LHAT_NUMBER_BUFFER) {
        return finish(out, LHAT_NUMBER_TOO_LONG, r.at);
    }

    // Strip the '_' separators before handing the text to the C library.
    char buffer[LHAT_NUMBER_BUFFER];
    size_t written = 0;
    size_t from = base != 10 ? 2 : 0;  // skip the 0x / 0b / 0o marker
    for (size_t i = from; i < r.at; i++) {
        if (text[i] != '_') {
            buffer[written++] = text[i];
        }
    }
    buffer[written] = '\0';

    errno = 0;
    if (is_float) {
        // Where a whole number past the range is an error, a real past it is
        // an infinity, exactly as a literal in a unit produces one.
        out->real = strtod(buffer, NULL);
    } else {
        unsigned long long value = strtoull(buffer, NULL, base);
        if (errno == ERANGE) {
            return finish(out, LHAT_NUMBER_OVERFLOW, r.at);
        }
        out->integer = (uint64_t)value;
    }
    return finish(out, LHAT_NUMBER_OK, r.at);
}

static bool is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

bool lhat_number_read(const char *text, size_t length, bool *is_real,
                      int64_t *whole, double *real)
{
    size_t from = 0;
    while (from < length && is_ascii_space(text[from])) {
        from++;
    }
    while (length > from && is_ascii_space(text[length - 1])) {
        length--;
    }

    // Section 10 has no sign in it: '-1' is 02 の 11 章's unary minus applied
    // to a literal. Nothing else reads this text, so the sign is read here.
    bool negative = false;
    if (from < length && (text[from] == '-' || text[from] == '+')) {
        negative = text[from] == '-';
        from++;
    }
    if (from >= length) {
        return false;
    }

    LhatNumberLiteral read;
    if (!lhat_number_literal(text + from, length - from, false, &read) ||
        read.length != length - from) {
        // Either it is not a literal or it is a literal with something after
        // it. A gap after the sign lands here too: the grammar starts where
        // the sign left off and a space is not a digit.
        return false;
    }

    if (read.kind == LHAT_NUMBER_REAL) {
        *is_real = true;
        *real = negative ? -read.real : read.real;
        return true;
    }

    // The one place holding the sign and the magnitude at once, and so the
    // only one that can tell a number^ from a run of digits naming none.
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (read.integer > limit) {
        return false;
    }
    *is_real = false;
    if (!negative) {
        *whole = (int64_t)read.integer;
    } else if (read.integer == (uint64_t)INT64_MAX + 1) {
        *whole = INT64_MIN;
    } else {
        *whole = -(int64_t)read.integer;
    }
    return true;
}
