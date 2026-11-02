// L^ (lhat) -- the numeric literal grammar.
//
// Sections 4, [syntax.finally.block], [lex.ambig.dotdot] and [lex.ambig.number-ident] of
// DesignDocuments/01-lexical-structure.md.
//
// Its own translation unit rather than a part of the lexer, for two reasons
// that point the same way. [syntax.obj.tonumber]'s tonumber has to read exactly what
// L^ reads, so there can only be one of this grammar -- and the machine that
// runs tonumber has to be linkable without a front end, since a unit already
// compiled to bytecode needs no lexer, no parser and no checker. A grammar
// both sides need belongs in neither of them.
//
// So nothing here knows what a source, a token or a diagnostic is. It reads
// bytes and answers what they name; naming the error to a reader is the
// lexer's, and [syntax.with.errors]'s is the machine's.
//
// The other direction -- a number^ written as text, and a number^ read
// through a written format -- lives in value.h beside the value it is about.

#ifndef LHAT_NUMBER_H
#define LHAT_NUMBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/config.h"

// [syntax.obj.number]: one type, two representations, and which one a text names is this
// grammar's answer rather than a reading applied afterwards.
typedef enum {
    LHAT_NUMBER_INTEGER,
    LHAT_NUMBER_REAL
} LhatNumberKind;

typedef enum {
    LHAT_NUMBER_OK,
    LHAT_NUMBER_MALFORMED,     // no digits, '0x' with none after it, a '_'
                               // that is not between two digits
    LHAT_NUMBER_BAD_EXPONENT,  // [modules.public.host]: 'e' with no digits after it
    LHAT_NUMBER_OVERFLOW,      // more than an integer literal can hold
    LHAT_NUMBER_TOO_LONG       // past LHAT_NUMBER_BUFFER, which is far past
                               // the range of either representation
} LhatNumberStatus;

typedef struct {
    LhatNumberKind kind;
    LhatNumberStatus status;

    // Bytes the literal took, whatever the status -- a caller scanning a
    // longer text has to know where this one stopped even when it stopped
    // badly. [errors.catch.clause] forbids backtracking, so a malformed literal has consumed
    // what it read.
    size_t length;

    // The magnitude. [syntax.finally] has no sign in it: '-1' is [syntax.op]'s
    // unary minus applied to a literal, so whoever wants a sign reads it
    // themselves -- see lhat_number_read.
    uint64_t integer;
    uint8_t base;  // 2, 8, 10 or 16
    LhatReal real;
} LhatNumberLiteral;

// Reads one literal from the front of `text`. `after_dot` is [lex.ambig.int-keys]'s rule:
// immediately after a '.' the digits form an integer key and must not swallow
// a further '.', so no prefix, fraction or exponent is considered there.
//
// Answers whether the literal is one, and fills `out` either way.
bool lhat_number_literal(const char *text, size_t length, bool after_dot,
                         LhatNumberLiteral *out);

// [syntax.obj.tonumber]: the whole of `text` as one number^, which is what tonumber
// asks. Answers false unless every byte is part of one literal.
//
// Leading and trailing ASCII whitespace is allowed and a single '-' or '+'
// may sit in front. The sign is read here rather than by the grammar, and
// this is the only place holding both it and the magnitude -- so it is the
// only place that can tell a number^ from a run of digits naming none, which
// is why a magnitude past the range answers false rather than wrapping.
//
// `is_real` says which of [syntax.obj.number]'s two representations the text named; `whole`
// or `real` holds the value accordingly.
bool lhat_number_read(const char *text, size_t length, bool *is_real,
                      LhatInteger *whole, LhatReal *real);

#endif  // LHAT_NUMBER_H
