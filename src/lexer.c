// L^ (lhat) -- lexical analyser.

#include "lexer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LHAT_CP_INVALID 0xFFFFFFFFu

// Longest numeric literal we are willing to parse. Anything beyond this is
// far past the range of uint64_t or double and is treated as malformed.
#define LHAT_NUMBER_BUFFER 256

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

static uint32_t decode_utf8(const char *p, size_t available, int *width)
{
    unsigned char c0 = (unsigned char)p[0];

    if (c0 < 0x80) {
        *width = 1;
        return c0;
    }

    int n;
    uint32_t cp;
    if ((c0 & 0xE0u) == 0xC0u) {
        n = 2;
        cp = c0 & 0x1Fu;
    } else if ((c0 & 0xF0u) == 0xE0u) {
        n = 3;
        cp = c0 & 0x0Fu;
    } else if ((c0 & 0xF8u) == 0xF0u) {
        n = 4;
        cp = c0 & 0x07u;
    } else {
        *width = 1;
        return LHAT_CP_INVALID;
    }

    if (available < (size_t)n) {
        *width = 1;
        return LHAT_CP_INVALID;
    }

    for (int i = 1; i < n; i++) {
        unsigned char ci = (unsigned char)p[i];
        if ((ci & 0xC0u) != 0x80u) {
            *width = 1;
            return LHAT_CP_INVALID;
        }
        cp = (cp << 6) | (ci & 0x3Fu);
    }

    bool overlong = (n == 2 && cp < 0x80u) || (n == 3 && cp < 0x800u) ||
                    (n == 4 && cp < 0x10000u);
    bool surrogate = cp >= 0xD800u && cp <= 0xDFFFu;
    if (overlong || surrogate || cp > 0x10FFFFu) {
        *width = 1;
        return LHAT_CP_INVALID;
    }

    *width = n;
    return cp;
}

static size_t encode_utf8(uint32_t cp, char out[4])
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

static bool is_unicode_space(uint32_t cp)
{
    return cp == 0x00A0u || cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200Au) ||
           cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu ||
           cp == 0x205Fu || cp == 0x3000u || cp == 0xFEFFu;
}

// Code points that must never be swallowed into an identifier because they
// are operators. Both the U+2264 / U+2265 and the U+2266 / U+2267 spellings
// of the comparison operators are accepted (Q10).
static bool is_reserved_symbol(uint32_t cp)
{
    return cp == 0x2260u || cp == 0x2264u || cp == 0x2265u ||
           cp == 0x2266u || cp == 0x2267u;
}

// Section 3.1 asks for UAX #31 XID_Start / XID_Continue. Shipping the full
// tables is deferred; every non-ASCII code point that is neither whitespace
// nor a reserved symbol is accepted instead. This is deliberately more
// permissive than UAX #31 and is the one place where the implementation
// knowingly diverges from the specification.
static bool is_ident_start(uint32_t cp)
{
    if (cp == LHAT_CP_INVALID) {
        return false;
    }
    if (cp < 0x80u) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_';
    }
    return !is_unicode_space(cp) && !is_reserved_symbol(cp);
}

static bool is_ident_continue(uint32_t cp)
{
    if (cp < 0x80u) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
               (cp >= '0' && cp <= '9') || cp == '_';
    }
    return is_ident_start(cp);
}

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

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

static bool at_end(const LhatLexer *lexer)
{
    return lexer->pos >= lexer->source->length;
}

static char current_byte(const LhatLexer *lexer)
{
    return at_end(lexer) ? '\0' : lexer->source->text[lexer->pos];
}

static char byte_at(const LhatLexer *lexer, size_t ahead)
{
    size_t index = lexer->pos + ahead;
    return index >= lexer->source->length ? '\0' : lexer->source->text[index];
}

static uint32_t current_cp(const LhatLexer *lexer, int *width)
{
    if (at_end(lexer)) {
        *width = 0;
        return 0;
    }
    return decode_utf8(lexer->source->text + lexer->pos,
                       lexer->source->length - lexer->pos, width);
}

// Advancing byte-wise keeps the column count in code points: continuation
// bytes are skipped when counting.
static void advance(LhatLexer *lexer)
{
    if (at_end(lexer)) {
        return;
    }
    char c = lexer->source->text[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else if (((unsigned char)c & 0xC0u) != 0x80u) {
        lexer->column++;
    }
}

static void advance_n(LhatLexer *lexer, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        advance(lexer);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics and string storage
// ---------------------------------------------------------------------------

static void report_at(LhatLexer *lexer, LhatErrorCode code, uint32_t offset,
                      uint32_t line, uint32_t column)
{
    if (lexer->diagnostic_count == lexer->diagnostic_capacity) {
        size_t grown = lexer->diagnostic_capacity ? lexer->diagnostic_capacity * 2 : 8;
        LhatDiagnostic *bigger =
            (LhatDiagnostic *)realloc(lexer->diagnostics, grown * sizeof *bigger);
        if (bigger == NULL) {
            return;  // drop the diagnostic rather than fail the scan
        }
        lexer->diagnostics = bigger;
        lexer->diagnostic_capacity = grown;
    }

    LhatDiagnostic *d = &lexer->diagnostics[lexer->diagnostic_count++];
    d->code = code;
    d->offset = offset;
    d->line = line;
    d->column = column;
}

static void report(LhatLexer *lexer, LhatErrorCode code)
{
    report_at(lexer, code, (uint32_t)lexer->pos, lexer->line, lexer->column);
}

static bool string_reserve(LhatLexer *lexer, size_t extra)
{
    if (lexer->strings_length + extra <= lexer->strings_capacity) {
        return true;
    }
    size_t grown = lexer->strings_capacity ? lexer->strings_capacity : 64;
    while (grown < lexer->strings_length + extra) {
        grown *= 2;
    }
    char *bigger = (char *)realloc(lexer->strings, grown);
    if (bigger == NULL) {
        return false;
    }
    lexer->strings = bigger;
    lexer->strings_capacity = grown;
    return true;
}

static void string_push(LhatLexer *lexer, const char *bytes, size_t length)
{
    if (length == 0 || !string_reserve(lexer, length)) {
        return;
    }
    memcpy(lexer->strings + lexer->strings_length, bytes, length);
    lexer->strings_length += length;
}

static void string_push_byte(LhatLexer *lexer, char byte)
{
    string_push(lexer, &byte, 1);
}

// ---------------------------------------------------------------------------
// Trivia: whitespace, newlines and comments (sections 6 and 9)
// ---------------------------------------------------------------------------

static void skip_block_comment(LhatLexer *lexer)
{
    uint32_t start_offset = (uint32_t)lexer->pos;
    uint32_t start_line = lexer->line;
    uint32_t start_column = lexer->column;

    advance_n(lexer, 2);  // '#['
    int depth = 1;

    while (!at_end(lexer)) {
        char c = current_byte(lexer);
        if (c == '#' && byte_at(lexer, 1) == '[') {
            advance_n(lexer, 2);
            depth++;
        } else if (c == ']' && byte_at(lexer, 1) == '#') {
            advance_n(lexer, 2);
            if (--depth == 0) {
                return;
            }
        } else {
            if (c == '\n') {
                lexer->pending_newline = true;
            }
            advance(lexer);
        }
    }

    report_at(lexer, LHAT_ERR_UNTERMINATED_BLOCK_COMMENT, start_offset,
              start_line, start_column);
}

static void skip_trivia(LhatLexer *lexer)
{
    for (;;) {
        char c = current_byte(lexer);

        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            advance(lexer);
            continue;
        }
        if (c == '\n') {
            lexer->pending_newline = true;
            advance(lexer);
            continue;
        }
        if (c == '#') {
            if (byte_at(lexer, 1) == '[') {
                skip_block_comment(lexer);
            } else {
                while (!at_end(lexer) && current_byte(lexer) != '\n') {
                    advance(lexer);
                }
            }
            continue;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Token construction
// ---------------------------------------------------------------------------

typedef struct {
    size_t offset;
    uint32_t line;
    uint32_t column;
} Mark;

static Mark mark(const LhatLexer *lexer)
{
    Mark m;
    m.offset = lexer->pos;
    m.line = lexer->line;
    m.column = lexer->column;
    return m;
}

static LhatToken finish(LhatLexer *lexer, Mark start, LhatTokenKind kind)
{
    LhatToken token;
    memset(&token, 0, sizeof token);
    token.kind = kind;
    token.offset = (uint32_t)start.offset;
    token.length = (uint32_t)(lexer->pos - start.offset);
    token.line = start.line;
    token.column = start.column;
    token.preceded_by_newline = lexer->pending_newline;
    return token;
}

// ---------------------------------------------------------------------------
// Identifiers (sections 2 and 3)
// ---------------------------------------------------------------------------

static LhatToken scan_identifier(LhatLexer *lexer, Mark start)
{
    int width;
    for (;;) {
        uint32_t cp = current_cp(lexer, &width);
        if (width == 0 || !is_ident_continue(cp)) {
            break;
        }
        advance_n(lexer, (size_t)width);
    }

    // Section 2.3: a run of '^' glued to the identifier turns it into a hat
    // identifier, and the count is retained (super^^^).
    uint32_t hats = 0;
    while (current_byte(lexer) == '^') {
        hats++;
        advance(lexer);
    }

    if (hats > 0) {
        LhatToken token = finish(lexer, start, LHAT_TOKEN_HAT_IDENT);
        token.v.hats = hats;
        return token;
    }
    return finish(lexer, start, LHAT_TOKEN_IDENT);
}

// Section 3.4. `a name` is a name written out in full, spaces and symbols
// included. A doubled backtick stands for one backtick, following the same
// convention as the raw string in 5.2.
//
// Newlines are not allowed inside: an identifier never spans lines, and
// stopping at the end of the line keeps an unclosed delimiter from consuming
// the rest of the file the way an unterminated string can (5.5).
static LhatToken scan_name_literal(LhatLexer *lexer, Mark start)
{
    advance(lexer);  // opening backtick

    size_t value_offset = lexer->strings_length;

    for (;;) {
        if (at_end(lexer) || current_byte(lexer) == '\n') {
            report_at(lexer, LHAT_ERR_UNTERMINATED_NAME_LITERAL,
                      (uint32_t)start.offset, start.line, start.column);
            return finish(lexer, start, LHAT_TOKEN_ERROR);
        }

        char c = current_byte(lexer);
        if (c == '`') {
            if (byte_at(lexer, 1) == '`') {
                string_push_byte(lexer, '`');
                advance_n(lexer, 2);
                continue;
            }
            advance(lexer);
            break;
        }

        string_push_byte(lexer, c);
        advance(lexer);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_NAME_LITERAL);
    token.v.string.kind = LHAT_STRING_RAW;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);

    if (token.v.string.length == 0) {
        report_at(lexer, LHAT_ERR_EMPTY_NAME_LITERAL, (uint32_t)start.offset,
                  start.line, start.column);
        token.kind = LHAT_TOKEN_ERROR;
    }
    return token;
}

// ---------------------------------------------------------------------------
// Numbers (sections 4, 10.1, 10.2, 10.3)
// ---------------------------------------------------------------------------

// Consumes a run of digits in the given base, allowing '_' only between two
// digits. Reports how many digits were seen and whether a separator was
// misplaced.
static void scan_digit_run(LhatLexer *lexer, int base, size_t *digits, bool *malformed)
{
    *digits = 0;
    *malformed = false;

    bool previous_was_digit = false;
    for (;;) {
        char c = current_byte(lexer);
        if (c == '_') {
            if (!previous_was_digit || !is_digit_of_base(byte_at(lexer, 1), base)) {
                *malformed = true;
            }
            advance(lexer);
            previous_was_digit = false;
            continue;
        }
        if (!is_digit_of_base(c, base)) {
            break;
        }
        advance(lexer);
        (*digits)++;
        previous_was_digit = true;
    }
}

static LhatToken scan_number(LhatLexer *lexer, Mark start)
{
    int base = 10;
    bool is_float = false;
    bool malformed = false;
    size_t digits = 0;

    // Section 10.1: immediately after a '.' the digits form an integer key and
    // must not swallow a further '.', so no prefix, fraction or exponent is
    // considered here.
    if (lexer->after_dot) {
        bool bad = false;
        scan_digit_run(lexer, 10, &digits, &bad);
        malformed = malformed || bad;
    } else {
        if (current_byte(lexer) == '0') {
            char marker = byte_at(lexer, 1);
            if (marker == 'x' || marker == 'X') {
                base = 16;
            } else if (marker == 'b' || marker == 'B') {
                base = 2;
            } else if (marker == 'o' || marker == 'O') {
                base = 8;
            }
            if (base != 10) {
                advance_n(lexer, 2);
            }
        }

        bool bad = false;
        scan_digit_run(lexer, base, &digits, &bad);
        malformed = malformed || bad;

        if (base == 10) {
            // Section 10.2: the fraction is taken only when a digit follows the
            // '.', so "1..2" scans as INT CONCAT INT.
            if (current_byte(lexer) == '.' && is_decimal_digit(byte_at(lexer, 1))) {
                advance(lexer);
                is_float = true;
                size_t fraction_digits = 0;
                scan_digit_run(lexer, 10, &fraction_digits, &bad);
                malformed = malformed || bad;
            }

            char exponent = current_byte(lexer);
            if (exponent == 'e' || exponent == 'E') {
                size_t sign = (byte_at(lexer, 1) == '+' || byte_at(lexer, 1) == '-') ? 1 : 0;
                if (!is_decimal_digit(byte_at(lexer, 1 + sign))) {
                    // Section 4.5: no backtracking. Q7 makes "1e^3" illegal
                    // anyway, so a malformed exponent is simply an error.
                    advance(lexer);
                    report_at(lexer, LHAT_ERR_MALFORMED_EXPONENT, (uint32_t)start.offset,
                              start.line, start.column);
                    return finish(lexer, start, LHAT_TOKEN_ERROR);
                }
                advance_n(lexer, 1 + sign);
                is_float = true;
                size_t exponent_digits = 0;
                scan_digit_run(lexer, 10, &exponent_digits, &bad);
                malformed = malformed || bad;
            }
        }
    }

    if (digits == 0) {
        malformed = true;
    }

    // Section 10.3 (Q7): "1to^3" is an error; a space is required.
    int width;
    uint32_t next = current_cp(lexer, &width);
    if (width > 0 && is_ident_start(next)) {
        report_at(lexer, LHAT_ERR_IDENT_AFTER_NUMBER, (uint32_t)lexer->pos,
                  lexer->line, lexer->column);
        while (!at_end(lexer)) {
            uint32_t cp = current_cp(lexer, &width);
            if (width == 0 || !is_ident_continue(cp)) {
                break;
            }
            advance_n(lexer, (size_t)width);
        }
        return finish(lexer, start, LHAT_TOKEN_ERROR);
    }

    size_t length = lexer->pos - start.offset;
    if (malformed || length >= LHAT_NUMBER_BUFFER) {
        report_at(lexer, LHAT_ERR_MALFORMED_NUMBER, (uint32_t)start.offset,
                  start.line, start.column);
        return finish(lexer, start, LHAT_TOKEN_ERROR);
    }

    // Strip the '_' separators before handing the text to the C library.
    char buffer[LHAT_NUMBER_BUFFER];
    size_t out = 0;
    size_t from = start.offset;
    if (base != 10) {
        from += 2;  // skip the 0x / 0b / 0o marker
    }
    for (size_t i = from; i < lexer->pos; i++) {
        char c = lexer->source->text[i];
        if (c != '_') {
            buffer[out++] = c;
        }
    }
    buffer[out] = '\0';

    LhatToken token = finish(lexer, start, is_float ? LHAT_TOKEN_FLOAT : LHAT_TOKEN_INT);
    errno = 0;
    if (is_float) {
        token.v.real = strtod(buffer, NULL);
    } else {
        unsigned long long value = strtoull(buffer, NULL, base);
        if (errno == ERANGE) {
            report_at(lexer, LHAT_ERR_INTEGER_OVERFLOW, (uint32_t)start.offset,
                      start.line, start.column);
            token.kind = LHAT_TOKEN_ERROR;
            return token;
        }
        token.v.integer.value = (uint64_t)value;
        token.v.integer.base = (uint8_t)base;
    }
    return token;
}

// ---------------------------------------------------------------------------
// Strings (section 5)
// ---------------------------------------------------------------------------

static void scan_escape(LhatLexer *lexer)
{
    advance(lexer);  // the backslash
    char c = current_byte(lexer);

    switch (c) {
        case 'n':  string_push_byte(lexer, '\n'); advance(lexer); return;
        case 'r':  string_push_byte(lexer, '\r'); advance(lexer); return;
        case 't':  string_push_byte(lexer, '\t'); advance(lexer); return;
        case '0':  string_push_byte(lexer, '\0'); advance(lexer); return;
        case '\\': string_push_byte(lexer, '\\'); advance(lexer); return;
        case '"':  string_push_byte(lexer, '"');  advance(lexer); return;
        case '\n': advance(lexer); return;  // line continuation
        default: break;
    }

    if (c == 'x') {
        char hi = byte_at(lexer, 1);
        char lo = byte_at(lexer, 2);
        if (is_digit_of_base(hi, 16) && is_digit_of_base(lo, 16)) {
            char digits[3] = { hi, lo, '\0' };
            string_push_byte(lexer, (char)strtoul(digits, NULL, 16));
            advance_n(lexer, 3);
            return;
        }
        report(lexer, LHAT_ERR_MALFORMED_ESCAPE);
        advance(lexer);
        return;
    }

    if (c == 'u' && byte_at(lexer, 1) == '{') {
        size_t ahead = 2;
        char digits[8];
        size_t count = 0;
        while (count < 7 && is_digit_of_base(byte_at(lexer, ahead), 16)) {
            digits[count++] = byte_at(lexer, ahead);
            ahead++;
        }
        digits[count] = '\0';
        if (count > 0 && count <= 6 && byte_at(lexer, ahead) == '}') {
            uint32_t cp = (uint32_t)strtoul(digits, NULL, 16);
            if (cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
                char encoded[4];
                size_t n = encode_utf8(cp, encoded);
                string_push(lexer, encoded, n);
                advance_n(lexer, ahead + 1);
                return;
            }
        }
        report(lexer, LHAT_ERR_MALFORMED_ESCAPE);
        advance(lexer);
        return;
    }

    report(lexer, LHAT_ERR_UNKNOWN_ESCAPE);
    if (!at_end(lexer)) {
        advance(lexer);
    }
}

static LhatToken scan_escaped_string(LhatLexer *lexer, Mark start)
{
    advance(lexer);  // opening quote
    size_t value_offset = lexer->strings_length;

    for (;;) {
        if (at_end(lexer)) {
            report_at(lexer, LHAT_ERR_UNTERMINATED_STRING, (uint32_t)start.offset,
                      start.line, start.column);
            return finish(lexer, start, LHAT_TOKEN_ERROR);
        }
        char c = current_byte(lexer);
        if (c == '"') {
            advance(lexer);
            break;
        }
        if (c == '\\') {
            scan_escape(lexer);
            continue;
        }
        string_push_byte(lexer, c);
        advance(lexer);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_STRING);
    token.v.string.kind = LHAT_STRING_ESCAPED;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);
    return token;
}

static LhatToken scan_raw_string(LhatLexer *lexer, Mark start)
{
    advance(lexer);  // opening quote
    size_t value_offset = lexer->strings_length;

    for (;;) {
        if (at_end(lexer)) {
            report_at(lexer, LHAT_ERR_UNTERMINATED_STRING, (uint32_t)start.offset,
                      start.line, start.column);
            return finish(lexer, start, LHAT_TOKEN_ERROR);
        }
        char c = current_byte(lexer);
        if (c == '\'') {
            if (byte_at(lexer, 1) == '\'') {
                string_push_byte(lexer, '\'');
                advance_n(lexer, 2);
                continue;
            }
            advance(lexer);
            break;
        }
        string_push_byte(lexer, c);
        advance(lexer);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_STRING);
    token.v.string.kind = LHAT_STRING_RAW;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);
    return token;
}

// Section 5.3. Runs to the end of the line and needs no closing delimiter,
// which also makes it usable as a comment. Escapes are not processed: a line
// string used as a comment must not fail on a stray backslash.
static LhatToken scan_line_string(LhatLexer *lexer, Mark start)
{
    advance_n(lexer, 3);
    size_t value_offset = lexer->strings_length;

    while (!at_end(lexer) && current_byte(lexer) != '\n') {
        string_push_byte(lexer, current_byte(lexer));
        advance(lexer);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_STRING);
    token.v.string.kind = LHAT_STRING_LINE;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);
    return token;
}

// ---------------------------------------------------------------------------
// Scope specifiers (section 8) and interpolation (section 5.4)
// ---------------------------------------------------------------------------

// Section 5.4. Scans the literal run between two holes. Never skips trivia:
// whitespace inside a string is content.
static LhatToken scan_interpolation_segment(LhatLexer *lexer)
{
    Mark start = mark(lexer);

    if (current_byte(lexer) == '{' && byte_at(lexer, 1) != '{') {
        advance(lexer);
        if (lexer->interp_depth >= LHAT_INTERP_MAX_DEPTH) {
            report(lexer, LHAT_ERR_INTERPOLATION_TOO_DEEP);
            return finish(lexer, start, LHAT_TOKEN_ERROR);
        }
        lexer->interp[lexer->interp_depth].in_hole = true;
        lexer->interp[lexer->interp_depth].brace_depth = 0;
        lexer->interp_depth++;
        return finish(lexer, start, LHAT_TOKEN_INTERP_EXPR_BEGIN);
    }

    if (current_byte(lexer) == '"') {
        advance(lexer);
        lexer->interp_depth--;  // pop the string frame
        return finish(lexer, start, LHAT_TOKEN_INTERP_END);
    }

    size_t value_offset = lexer->strings_length;

    while (!at_end(lexer)) {
        char c = current_byte(lexer);

        // '{{' and '}}' stand for a single brace. A lone '}' is literal text:
        // no hole is open in this mode, so there is nothing it could close.
        if (c == '{') {
            if (byte_at(lexer, 1) != '{') {
                break;
            }
            string_push_byte(lexer, '{');
            advance_n(lexer, 2);
            continue;
        }
        if (c == '}') {
            string_push_byte(lexer, '}');
            advance_n(lexer, byte_at(lexer, 1) == '}' ? 2 : 1);
            continue;
        }
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            scan_escape(lexer);
            continue;
        }
        string_push_byte(lexer, c);
        advance(lexer);
    }

    if (at_end(lexer)) {
        report_at(lexer, LHAT_ERR_UNTERMINATED_STRING, (uint32_t)start.offset,
                  start.line, start.column);
        lexer->interp_depth--;
        return finish(lexer, start, LHAT_TOKEN_ERROR);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_INTERP_TEXT);
    token.v.string.kind = LHAT_STRING_ESCAPED;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);
    return token;
}

// Section 5.4. Everything from the ':' up to the closing '}' is taken as raw
// text; a format specifier is not an expression.
static LhatToken scan_interpolation_format(LhatLexer *lexer)
{
    Mark start = mark(lexer);
    advance(lexer);  // ':'

    size_t value_offset = lexer->strings_length;
    while (!at_end(lexer) && current_byte(lexer) != '}') {
        string_push_byte(lexer, current_byte(lexer));
        advance(lexer);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_INTERP_FORMAT);
    token.v.string.kind = LHAT_STRING_RAW;
    token.v.string.offset = (uint32_t)value_offset;
    token.v.string.length = (uint32_t)(lexer->strings_length - value_offset);
    return token;
}

static LhatToken scan_dollar(LhatLexer *lexer, Mark start)
{
    char next = byte_at(lexer, 1);

    // Section 10.6: one byte of lookahead decides between an interpolated
    // string and a scope specifier.
    if (next == '"') {
        advance_n(lexer, 2);
        if (lexer->interp_depth >= LHAT_INTERP_MAX_DEPTH) {
            report(lexer, LHAT_ERR_INTERPOLATION_TOO_DEEP);
            return finish(lexer, start, LHAT_TOKEN_ERROR);
        }
        lexer->interp[lexer->interp_depth].in_hole = false;
        lexer->interp[lexer->interp_depth].brace_depth = 0;
        lexer->interp_depth++;
        return finish(lexer, start, LHAT_TOKEN_INTERP_BEGIN);
    }

    // Memo.md L71 notes that interpolation requires double quotes, so $'...'
    // is not a thing. Saying so beats letting it fail as a scope specifier.
    if (next == '\'') {
        report(lexer, LHAT_ERR_INTERPOLATION_NEEDS_QUOTES);
        advance(lexer);
        return finish(lexer, start, LHAT_TOKEN_ERROR);
    }

    advance(lexer);  // '$'

    LhatScopeKind kind = LHAT_SCOPE_GLOBAL;
    uint32_t depth = 0;

    if (current_byte(lexer) == '$') {
        advance(lexer);
        kind = LHAT_SCOPE_FILE;
    } else if (current_byte(lexer) == '^') {
        kind = LHAT_SCOPE_RELATIVE;
        while (current_byte(lexer) == '^') {
            depth++;
            advance(lexer);
        }
    }

    // The sigil must be glued to a name; whitespace is not allowed between.
    int width;
    uint32_t cp = current_cp(lexer, &width);
    if (width == 0 || !is_ident_start(cp)) {
        report_at(lexer, LHAT_ERR_SCOPE_WITHOUT_NAME, (uint32_t)start.offset,
                  start.line, start.column);
        return finish(lexer, start, LHAT_TOKEN_ERROR);
    }

    LhatToken token = finish(lexer, start, LHAT_TOKEN_SCOPE);
    token.v.scope.kind = kind;
    token.v.scope.depth = depth;
    return token;
}

// ---------------------------------------------------------------------------
// Operators (section 7)
// ---------------------------------------------------------------------------

typedef struct {
    const char *text;
    size_t length;
    LhatOpKind op;
} OperatorEntry;

// Ordered longest first so that a linear scan implements maximal munch.
static const OperatorEntry operator_table[] = {
    { "...", 3, LHAT_OP_ELLIPSIS },     // must precede ".." (13.7)

    { "\xE2\x89\xA0", 3, LHAT_OP_NE },  // U+2260 NOT EQUAL TO
    { "\xE2\x89\xA4", 3, LHAT_OP_LE },  // U+2264 LESS-THAN OR EQUAL TO
    { "\xE2\x89\xA5", 3, LHAT_OP_GE },  // U+2265 GREATER-THAN OR EQUAL TO
    { "\xE2\x89\xA6", 3, LHAT_OP_LE },  // U+2266 LESS-THAN OVER EQUAL TO
    { "\xE2\x89\xA7", 3, LHAT_OP_GE },  // U+2267 GREATER-THAN OVER EQUAL TO

    { ":=", 2, LHAT_OP_DEFINE },
    { "<<", 2, LHAT_OP_REASSIGN },
    { "->", 2, LHAT_OP_ARROW },
    { "::", 2, LHAT_OP_COLONCOLON },  // withdrawn; kept for diagnostics
    { "!=", 2, LHAT_OP_NE },
    { "=/", 2, LHAT_OP_NE },
    { "<=", 2, LHAT_OP_LE },
    { ">=", 2, LHAT_OP_GE },
    { "**", 2, LHAT_OP_POW },
    { "//", 2, LHAT_OP_FLOORDIV },
    { "..", 2, LHAT_OP_CONCAT },
    { "?.", 2, LHAT_OP_NIL_DOT },
    { "?(", 2, LHAT_OP_NIL_CALL },
    { "?[", 2, LHAT_OP_NIL_INDEX },

    { "(", 1, LHAT_OP_LPAREN },
    { ")", 1, LHAT_OP_RPAREN },
    { "[", 1, LHAT_OP_LBRACKET },
    { "]", 1, LHAT_OP_RBRACKET },
    { "{", 1, LHAT_OP_LBRACE },
    { "}", 1, LHAT_OP_RBRACE },
    { ",", 1, LHAT_OP_COMMA },
    { ";", 1, LHAT_OP_SEMICOLON },
    { ":", 1, LHAT_OP_COLON },
    { "=", 1, LHAT_OP_EQ },
    { "<", 1, LHAT_OP_LT },
    { ">", 1, LHAT_OP_GT },
    { "+", 1, LHAT_OP_ADD },
    { "-", 1, LHAT_OP_SUB },
    { "*", 1, LHAT_OP_MUL },
    { "/", 1, LHAT_OP_DIV },
    { "%", 1, LHAT_OP_MOD },
    { "!", 1, LHAT_OP_NOT },
    { "|", 1, LHAT_OP_UNION },
    { ".", 1, LHAT_OP_DOT },
    { "@", 1, LHAT_OP_AT },
};

static bool match_operator(const LhatLexer *lexer, const OperatorEntry **found)
{
    size_t remaining = lexer->source->length - lexer->pos;
    const char *at = lexer->source->text + lexer->pos;

    for (size_t i = 0; i < sizeof operator_table / sizeof operator_table[0]; i++) {
        const OperatorEntry *entry = &operator_table[i];
        if (entry->length <= remaining && memcmp(at, entry->text, entry->length) == 0) {
            *found = entry;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void lhat_lexer_init(LhatLexer *lexer, const LhatSource *source)
{
    memset(lexer, 0, sizeof *lexer);
    lexer->source = source;
    lexer->line = 1;
    lexer->column = 1;
    // The first token sits at the start of a line, so it counts as preceded by
    // a newline for the purposes of section 10.9.
    lexer->pending_newline = true;
}

void lhat_lexer_dispose(LhatLexer *lexer)
{
    free(lexer->strings);
    free(lexer->diagnostics);
    lexer->strings = NULL;
    lexer->diagnostics = NULL;
    lexer->strings_length = lexer->strings_capacity = 0;
    lexer->diagnostic_count = lexer->diagnostic_capacity = 0;
}

static bool inside_interpolated_text(const LhatLexer *lexer)
{
    return lexer->interp_depth > 0 && !lexer->interp[lexer->interp_depth - 1].in_hole;
}

static bool inside_interpolation_hole(const LhatLexer *lexer)
{
    return lexer->interp_depth > 0 && lexer->interp[lexer->interp_depth - 1].in_hole;
}

LhatToken lhat_lexer_next(LhatLexer *lexer)
{
    // Checked before skipping trivia: inside a string, whitespace is content.
    if (inside_interpolated_text(lexer)) {
        LhatToken token = scan_interpolation_segment(lexer);
        lexer->pending_newline = false;
        lexer->after_dot = false;
        return token;
    }

    skip_trivia(lexer);

    if (inside_interpolation_hole(lexer) &&
        lexer->interp[lexer->interp_depth - 1].brace_depth == 0) {
        Mark hole = mark(lexer);
        if (current_byte(lexer) == '}') {
            advance(lexer);
            lexer->interp_depth--;
            lexer->pending_newline = false;
            lexer->after_dot = false;
            return finish(lexer, hole, LHAT_TOKEN_INTERP_EXPR_END);
        }
        // A single ':' ends the expression and begins the format specifier.
        // '::' is a return type marker and is left to the normal path.
        if (current_byte(lexer) == ':' && byte_at(lexer, 1) != ':') {
            LhatToken token = scan_interpolation_format(lexer);
            lexer->pending_newline = false;
            lexer->after_dot = false;
            return token;
        }
    }

    Mark start = mark(lexer);

    if (at_end(lexer)) {
        LhatToken token = finish(lexer, start, LHAT_TOKEN_EOF);
        lexer->pending_newline = false;
        lexer->after_dot = false;
        return token;
    }

    int width;
    uint32_t cp = current_cp(lexer, &width);
    char c = current_byte(lexer);
    LhatToken token;

    if (cp == LHAT_CP_INVALID) {
        report(lexer, LHAT_ERR_INVALID_UTF8);
        advance(lexer);
        token = finish(lexer, start, LHAT_TOKEN_ERROR);
    } else if (c == '"') {
        // Section 10.5: maximal munch, so `"""` wins over an empty string.
        token = (byte_at(lexer, 1) == '"' && byte_at(lexer, 2) == '"')
                    ? scan_line_string(lexer, start)
                    : scan_escaped_string(lexer, start);
    } else if (c == '\'') {
        token = scan_raw_string(lexer, start);
    } else if (c == '`') {
        token = scan_name_literal(lexer, start);
    } else if (c == '$') {
        token = scan_dollar(lexer, start);
    } else if (is_decimal_digit(c)) {
        token = scan_number(lexer, start);
    } else if (is_ident_start(cp)) {
        token = scan_identifier(lexer, start);
    } else if (c == '^') {
        // Section 2.5: '^' only ever follows an identifier.
        report(lexer, LHAT_ERR_BARE_HAT);
        advance(lexer);
        token = finish(lexer, start, LHAT_TOKEN_ERROR);
    } else {
        const OperatorEntry *entry = NULL;
        if (match_operator(lexer, &entry)) {
            advance_n(lexer, entry->length);
            token = finish(lexer, start, LHAT_TOKEN_OP);
            token.v.op = entry->op;
        } else if (c == '?') {
            // '?' exists only as part of ?. ?( ?[ and must be glued to them.
            report(lexer, LHAT_ERR_LONE_QUESTION_MARK);
            advance(lexer);
            token = finish(lexer, start, LHAT_TOKEN_ERROR);
        } else {
            report(lexer, LHAT_ERR_UNEXPECTED_CHARACTER);
            advance_n(lexer, (size_t)width);
            token = finish(lexer, start, LHAT_TOKEN_ERROR);
        }
    }

    // Track braces opened inside a hole so that a table literal's '}' is not
    // taken for the end of the hole.
    if (inside_interpolation_hole(lexer) && token.kind == LHAT_TOKEN_OP) {
        uint32_t *depth = &lexer->interp[lexer->interp_depth - 1].brace_depth;
        if (token.v.op == LHAT_OP_LBRACE) {
            (*depth)++;
        } else if (token.v.op == LHAT_OP_RBRACE && *depth > 0) {
            (*depth)--;
        }
    }

    lexer->pending_newline = false;
    lexer->after_dot = token.kind == LHAT_TOKEN_OP &&
                       (token.v.op == LHAT_OP_DOT || token.v.op == LHAT_OP_NIL_DOT);
    return token;
}

const char *lhat_lexer_string(const LhatLexer *lexer, const LhatToken *token,
                              size_t *length)
{
    if (token->kind != LHAT_TOKEN_STRING &&
        token->kind != LHAT_TOKEN_NAME_LITERAL &&
        token->kind != LHAT_TOKEN_INTERP_TEXT &&
        token->kind != LHAT_TOKEN_INTERP_FORMAT) {
        *length = 0;
        return NULL;
    }
    *length = token->v.string.length;
    return lexer->strings + token->v.string.offset;
}

const char *lhat_error_message(LhatErrorCode code)
{
    switch (code) {
        case LHAT_ERR_NONE:
            return "no error";
        case LHAT_ERR_UNEXPECTED_CHARACTER:
            return "unexpected character";
        case LHAT_ERR_INVALID_UTF8:
            return "invalid UTF-8 sequence";
        case LHAT_ERR_BARE_HAT:
            return "'^' must directly follow an identifier";
        case LHAT_ERR_LONE_QUESTION_MARK:
            return "'?' is only valid as part of '?.', '?(' or '?['";
        case LHAT_ERR_IDENT_AFTER_NUMBER:
            return "a number must be separated from an identifier by whitespace";
        case LHAT_ERR_MALFORMED_NUMBER:
            return "malformed number literal";
        case LHAT_ERR_MALFORMED_EXPONENT:
            return "exponent must be followed by digits";
        case LHAT_ERR_INTEGER_OVERFLOW:
            return "integer literal is out of range";
        case LHAT_ERR_UNTERMINATED_STRING:
            return "unterminated string literal";
        case LHAT_ERR_UNTERMINATED_NAME_LITERAL:
            return "name literal is not closed before the end of the line";
        case LHAT_ERR_EMPTY_NAME_LITERAL:
            return "name literal is empty";
        case LHAT_ERR_UNKNOWN_ESCAPE:
            return "unknown escape sequence";
        case LHAT_ERR_MALFORMED_ESCAPE:
            return "malformed escape sequence";
        case LHAT_ERR_UNTERMINATED_BLOCK_COMMENT:
            return "unterminated block comment";
        case LHAT_ERR_SCOPE_WITHOUT_NAME:
            return "scope specifier must be followed directly by a name";
        case LHAT_ERR_INTERPOLATION_NEEDS_QUOTES:
            return "string interpolation requires double quotes: $\"...\"";
        case LHAT_ERR_INTERPOLATION_TOO_DEEP:
            return "interpolated strings are nested too deeply";
    }
    return "unknown error";
}
