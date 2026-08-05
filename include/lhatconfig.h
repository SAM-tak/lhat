// L^ (lhat)
//
// Every adjustable limit and buffer size the implementation uses, gathered
// in one place instead of scattered across the files that need them.
// Grouped by the stage of the pipeline that owns each one, lexer through
// machine, so a change to one stage's numbers stays close together.

#ifndef LHATCONFIG_H
#define LHATCONFIG_H

// lexer
// Longest numeric literal we are willing to parse. Anything beyond this is
// far past the range of uint64_t or double and is treated as malformed.
#define LHAT_NUMBER_BUFFER 256
// Nesting limit for interpolated strings. A hole may contain another
// interpolated string, which may contain another hole, and so on; anything
// approaching this depth is pathological rather than intentional.
#define LHAT_INTERP_MAX_DEPTH 32

// ast
// Nodes are handed out from fixed-size blocks rather than one allocation
// each, so this is how many share one block.
#define LHAT_ARENA_BLOCK_NODES 256

// type
// Types are handed out from fixed-size blocks the same way ast nodes are.
#define LHAT_TYPE_BLOCK_BYTES 8192

// check
// How many of a call's arguments the checker keeps type information for at
// once while resolving which overload^ it fits.
#define LHAT_CHECK_MAX_TRACKED_ARGS 16

// vm -- compiling
// A frame's registers and locals, and the upvalues one closure may capture,
// are each counted with an 8-bit field in a bytecode instruction (code.h).
// LHAT_MAX_REGISTERS and LHAT_MAX_LOCALS leave some of that range as
// margin; LHAT_MAX_UPVALUES does not need to and simply matches the field's
// own width (0xFF, checked in code.c) plus one.
#define LHAT_MAX_REGISTERS 250
#define LHAT_MAX_LOCALS 200
#define LHAT_MAX_UPVALUES 256
// How many break^ targets a nesting of loops may leave open at once.
#define LHAT_MAX_BREAKS 64
// 02 の 14.2: the chain of delegation is settled when the definition is
// written, and 14.2 says the checker can decide the parts of any expression.
// So the compiler resolves composition rather than the machine: what a def^
// composes onto is a def^ it can already see.
#define LHAT_MAX_DEF_CHAIN 8
// "Group.Kind" -- what typeof^ answers for an error made from an errordef^
// (04 の 2.3). Longer than any name a writer is likely to choose.
#define LHAT_QUALIFIED_NAME_BUFFER 256

// vm -- running
// The shared stack every frame's registers live on.
#define LHAT_STACK_SLOTS 8192
// How deep a call may recurse before the machine gives up, rather than
// letting the host's own stack decide it for us.
#define LHAT_MAX_FRAMES 200
// How many with^ bindings and finally^ clauses one frame may have open for
// unwinding at once.
#define LHAT_MAX_CLEANUPS 32
// A suspended coroutine keeps its own frame's cleanups, so this has to hold
// as many as LHAT_MAX_CLEANUPS does -- the two are the same limit seen from
// two different structures, not a coincidence.
#define LHAT_COROUTINE_CLEANUPS LHAT_MAX_CLEANUPS
// A collection's starting threshold, and what it grows to afterwards: a
// multiple of what survived, plus a floor so a nearly-empty heap does not
// collect again almost immediately.
#define LHAT_GC_INITIAL_THRESHOLD 256
#define LHAT_GC_GROWTH_FACTOR 2
#define LHAT_GC_MIN_THRESHOLD 64

// value
// 14 章 makes a table both a sequence and a mapping, and one holding itself
// is nothing the type system forbids. A depth of its own is what stops the
// walk; nothing here has to know whether it went round.
#define LHAT_WRITE_MAX_DEPTH 6

// error report
// A terminal that wraps a long line puts the mark under the wrong place,
// which says less than showing part of the line does.
#define LHAT_REPORT_MAX_COLUMNS 96
#define LHAT_REPORT_ELISION "..."
#define LHAT_REPORT_ELISION_COLUMNS 3

#endif // LHATCONFIG_H
