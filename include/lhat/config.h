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
// 02 の 11.7改2: how many '?' one postfix run may carry. Each is a jump that
// stays open until the run ends, and a run is written in one line of source
// -- 'a?.b?[i]?(x)' is three, and nothing readable goes far past that.
#define LHAT_MAX_NIL_CHAIN 16
// 02 の 14.2: the chain of delegation is settled when the definition is
// written, and 14.2 says the checker can decide the parts of any expression.
// So the compiler resolves composition rather than the machine: what a def^
// composes onto is a def^ it can already see.
#define LHAT_MAX_DEF_CHAIN 8
// 02 の 14.11: how deep the tree a field's default may be. The checker keeps
// one to what a literal spells, which never goes far -- this bound is the
// machine's own C-stack guard: baking and copying a tree recurse, and the
// recursion must stop while the stack is whole. A cycle cannot be written as
// a literal tree, so no correct program ever sees this limit.
#define LHAT_MAX_PROTOTYPE_DEPTH 32
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
// The shared stack every frame's registers live on -- what
// lhat_machine_new asks for. 03 の 4.3改: a machine's own length is in the
// machine (Machine.slot_capacity), since a caller may choose another.
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
// letting the host's own stack decide it for us -- again what
// lhat_machine_new asks for, with Machine.frame_capacity the real one.
#define LHAT_MAX_FRAMES 200
// How many with^ bindings and finally^ clauses one frame may have open for
// unwinding at once.
#define LHAT_MAX_CLEANUPS 32
// A suspended coroutine keeps its own frame's cleanups, so this has to hold
// as many as LHAT_MAX_CLEANUPS does -- the two are the same limit seen from
// two different structures, not a coincidence.
#define LHAT_COROUTINE_CLEANUPS LHAT_MAX_CLEANUPS
// A collection's starting threshold, and the floor added to what it grows to
// afterwards so a nearly-empty heap does not collect again almost at once.
#define LHAT_GC_INITIAL_THRESHOLD 256
#define LHAT_GC_MIN_THRESHOLD 64
// What a machine's collector starts from (vm.h's LhatGcSettings), all counted
// in objects. GROWTH: a cycle starts once the heap reaches this percentage of
// what the last one left alive. STEPMUL: a step does this percentage of the
// allocations since the last one in work -- traversed while marking, passed
// over while sweeping -- which is what decides whether a cycle keeps up with
// the program; too little, and what is allocated while it runs piles up as
// garbage the next cycle has to find. STEP_SIZE: how many allocations go by
// between steps, and so, with STEPMUL, how long one pause is.
//
// Measured against a game's frame loop (300k live objects): 200/200 let the
// heap reach 3.3 times what was live; 150/800 holds it near twice, for about
// 1.7 times the collector's total work.
#define LHAT_GC_GROWTH 150
#define LHAT_GC_STEPMUL 800
#define LHAT_GC_STEP_SIZE 10
// 16.3: how many finished loop coroutines a machine keeps to reuse. Enough
// for loops nested this deep over generators; more would only hold memory.
#define LHAT_SPARE_COROUTINES 8

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
