// L^ (lhat)

#ifndef LHATCONFIG_H
#define LHATCONFIG_H

// lexer
// Longest numeric literal we are willing to parse. Anything beyond this is
// far past the range of uint64_t or double and is treated as malformed.
#define LHAT_LEXER_NUMBER_BUFFER 256
// Nesting limit for interpolated strings. A hole may contain another
// interpolated string, which may contain another hole, and so on; anything
// approaching this depth is pathological rather than intentional.
#define LHAT_LEXER_INTERP_MAX_DEPTH 32

// vm
#define LHAT_MAX_REGISTERS 250
#define LHAT_MAX_LOCALS 200
#define LHAT_MAX_BREAKS 64
#define LHAT_STACK_SLOTS 8192
#define LHAT_MAX_FRAMES 200
#define LHAT_MAX_CLEANUPS 32
// 02 の 14.2: the chain of delegation is settled when the definition is
// written, and 14.2 says the checker can decide the parts of any expression.
// So the compiler resolves composition rather than the machine: what a def^
// composes onto is a def^ it can already see.
#define LHAT_MAX_DEF_CHAIN 8

// value
// 14 章 makes a table both a sequence and a mapping, and one holding itself
// is nothing the type system forbids. A depth of its own is what stops the
// walk; nothing here has to know whether it went round.
#define LHAT_WRITE_MAX_DEPTH 6

// object
#define LHAT_COROUTINE_CLEANUPS LHAT_MAX_CLEANUPS

// type check
#define LHAT_CHECK_MAX_TRACKED_ARGS 16

// ast
#define LHAT_ARENA_BLOCK_NODES 256

// error report
// A terminal that wraps a long line puts the mark under the wrong place,
// which says less than showing part of the line does.
#define LHAT_REPORT_MAX_COLUMNS 96

#define LHAT_REPORT_ELISION "..."
#define LHAT_REPORT_ELISION_COLUMNS 3

// type
#define LHAT_TYPE_BLOCK_BYTES 8192

#endif // LHATCONFIG_H
