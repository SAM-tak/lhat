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
// 07 の 4 章: how far lhat_type_write descends before writing an ellipsis.
// A table may hold itself (14 章), so something has to stop the walk; and
// past this depth a written-out type says less to a reader than a short one
// does anyway.
#define LHAT_TYPE_WRITE_MAX_DEPTH 3
// How many members or parameters it writes before eliding the rest.
#define LHAT_TYPE_WRITE_MAX_ITEMS 6

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
// 02 の 13.11 with 14.9: a def^ name written as a type lowers to the shape it
// stands for, and 14.15 lets a field of that shape be annotated with another
// definition -- so the walk nests. How far it may, before a member is left as
// its name alone. A definition already inside the walk is answered that way
// too, whatever the depth, which is what closes a cycle.
#define LHAT_MAX_TYPE_NESTING 8
// 02 の 14.10改: how many positions one 'type[n]' may ask for. The count is
// written out and the positions are made one by one, so this is what keeps a
// single token from asking for as much of the checker's arena as it likes.
// Far past any table a writer names position by position.
#define LHAT_MAX_TYPE_POSITIONS 256
// "Group.Kind" -- what typeof^ answers for an error made from an errordef^
// (04 の 2.3). Longer than any name a writer is likely to choose.
#define LHAT_QUALIFIED_NAME_BUFFER 256
// How many "." segments a qualified name reaching into a host-registered
// error kind (05 の 8.7 の誤り版) may have -- "module...Name.Variant". Same
// margin as LHAT_MAX_DEF_CHAIN gives a chain of composition.
#define LHAT_MAX_QUALIFIER_SEGMENTS 8

// vm -- running
// The shared stack every frame's registers live on.
#define LHAT_STACK_SLOTS 8192
// 05 の 8.9: the largest payload a host value type may register, sized so a
// whole value (one head slot plus the data slots) fits the machine's answer
// scratch. Generous for the small mathematical types the feature exists for.
#define LHAT_HOSTVALUE_MAX_BYTES 248
// 02 の 13.8改: how many positions a tuple may have. A tuple rides a frame's
// answer room through the cleanup drain the way a host value does, so it is
// bounded by the same scratch -- one head slot plus this many positions. Far
// beyond anything a signature is written with; the bound exists so the room
// can be a fixed array rather than an allocation on the way out.
#define LHAT_MAX_TUPLE (LHAT_HOSTVALUE_MAX_BYTES / 8)
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
// 5.12: how much of a cycle one step does, counted in objects looked at --
// traversed while marking, or passed over while sweeping. The pause a
// collection costs is this, not the whole heap.
#define LHAT_GC_STEP_WORK 20
// And how many allocations go by before the next step. Their **ratio** is
// what decides whether the collector keeps up with the program: more work per
// allocation finishes cycles sooner and leaves less floating garbage. The
// sizes themselves decide only how long one pause is, so they are small --
// a heap of a hundred objects should be collected over several steps and not
// in one, or the whole point of stepping is lost on everything but the
// largest heaps.
#define LHAT_GC_STEP_SIZE 10

// value
// 14 章 makes a table both a sequence and a mapping, and one holding itself
// is nothing the type system forbids. A depth of its own is what stops the
// walk; nothing here has to know whether it went round.
#define LHAT_WRITE_MAX_DEPTH 6
// 02 の 14.17: how long a format written for a number^ may be. It carries
// one conversion and whatever text the writer put around it, so the room is
// for the text -- a format needing more than this is doing something a
// tostring of one number was not meant for.
#define LHAT_FORMAT_MAX_BYTES 128

// error report
// A terminal that wraps a long line puts the mark under the wrong place,
// which says less than showing part of the line does.
#define LHAT_REPORT_MAX_COLUMNS 96
#define LHAT_REPORT_ELISION "..."
#define LHAT_REPORT_ELISION_COLUMNS 3

#endif // LHATCONFIG_H
