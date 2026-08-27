// L^ (lhat) -- bytecode: instructions and the chunk they live in.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// 5.2: a register machine with one 32-bit word per instruction. Decoding is a
// shift and a mask, and a jump target is an instruction count rather than a
// byte offset, because every instruction is the same size.
//
// 5.1: every instruction here starts generic, checking the types of what it
// is given so a wrong value stops the run rather than corrupting the
// machine. Safety is strict's static checking; the specialised forms that
// skip the check come later, replacing these where strict settled the types.

#ifndef LHAT_CODE_H
#define LHAT_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LhatProto and LhatModule are what a host is handed; this file is what
// they are made of.
#include "lhat/module.h"

#include "lhat/object.h"
#include "lhat/value.h"

typedef uint32_t LhatInstruction;

typedef enum {
    // A = destination unless said otherwise.
    LHAT_BC_LOADK,      // A Bx  R[A] = K[Bx]
    LHAT_BC_LOADNIL,    // A     R[A] = nil^
    LHAT_BC_LOADBOOL,   // A B   R[A] = (B != 0)
    LHAT_BC_MOVE,       // A B   R[A] = R[B]

    LHAT_BC_ADD,        // A B C R[A] = R[B] + R[C]
    LHAT_BC_SUB,
    LHAT_BC_MUL,
    LHAT_BC_DIV,        // 04 の 11.2: real division, so this cannot fail
    LHAT_BC_IDIV,       // 04 の 11.2: fails on zero
    LHAT_BC_MOD,        // the same
    LHAT_BC_POW,
    LHAT_BC_CONCAT,     // A B C R[A] = R[B] .. R[C]  (02 の 11.2)
    LHAT_BC_NEG,        // A B   R[A] = -R[B]
    LHAT_BC_NOT,        // A B   R[A] = !R[B]
    LHAT_BC_TYPEOF,     // A B   R[A] = typeof^(R[B])  (02 の 14.16)

    LHAT_BC_EQ,         // A B C R[A] = R[B] = R[C]
    LHAT_BC_SAME,       // A B C R[A] = R[B] and R[C] are the same instance
    LHAT_BC_NE,
    LHAT_BC_LT,
    LHAT_BC_LE,
    LHAT_BC_GT,
    LHAT_BC_GE,
    // 02 の 11.9: A B C R[A] = R[B] <=> R[C], a number^ saying which of
    // the two comes first. The four above read their answer off this one
    // whenever they cannot answer for themselves.
    LHAT_BC_SPACESHIP,

    LHAT_BC_JUMP,       // Bx    signed, relative to the next instruction
    LHAT_BC_JUMP_FALSE, // A Bx  jump when R[A] is false

    // 5.3: arguments sit above the callee in the caller's frame, and one
    // value comes back -- 02 の 13.8改 lets that value be a tuple, which
    // arrives as a run of slots rather than one, and the count is settled by
    // the type either way. Lua's calling convention is complicated because
    // the count is settled at run time; that reconciliation still does not
    // exist here.
    LHAT_BC_CLOSURE,    // A Bx  R[A] = closure of protos[Bx]
    LHAT_BC_CALL,       // A B C R[A] = R[A](R[A+1] .. R[A+B]).
                        //       C bit 0: 13.7's 'expr...' -- R[A+B] is a
                        //         table whose positions are unpacked as
                        //         further arguments, in place of being the
                        //         last argument itself.
                        //       C bits 1-7 (13.8改): how many consecutive
                        //         slots the call site reserved at R[A] for
                        //         the answer. 0 and 1 both mean one, which is
                        //         every call written before tuples existed.
                        //         The byte was a boolean and had the room.
    // 03 の 5.11c: strict settled which candidate of an overloaded member
    // (02 の 14.12) the call ahead means, so the search 5.11 runs is not
    // run. Anything but a group in R[A] is left alone -- a value that got
    // here another way is still called the ordinary way.
    LHAT_BC_PICKARM,    // A Bx  R[A] = candidate Bx of the group R[A]
    LHAT_BC_GETUPVAL,   // A B   R[A] = *upvalue[B]
    LHAT_BC_SETUPVAL,   // A B   *upvalue[B] = R[A]
    LHAT_BC_CLOSE,      // A     the places at R[A] and above stop being shared
    LHAT_BC_CLOSEONE,   // A     the place at R[A] alone stops being shared.
                        //       03 の 4.3: a session's top level reuses one
                        //       slot for every let^ of a name, so severing
                        //       that binding must not reach the names above
                        //       it, which are other bindings still live
    LHAT_BC_THIS,       // A     R[A] = the subroutine running (02 の 15.10)
    LHAT_BC_ENV,        // A     R[A] = L^, the machine's own table (05 の 8.6)
    LHAT_BC_UNIT,       // A Bx  R[A] = a closure of the unit Bx (05 の 5.3).
                        //       Calling it answers what that unit publishes,
                        //       running its body only the first time.

    // 02 の 14 章: the one data structure. 04 の 11.3 makes a missing key
    // nil^ rather than a failure, so GETINDEX cannot fail on that account.
    LHAT_BC_NEWTABLE,   // A B   R[A] = { }. B != 0 marks it a definition
                        //       (14.9), which is what tells a def^ apart from
                        //       a table written by hand -- the two are the
                        //       same structure otherwise, and 14.17改 asks
                        //       which one it is
    LHAT_BC_GETINDEX,   // A B C R[A] = R[B][R[C]]
    // 03 の 5.1改: the same read where the key was written rather than
    // computed -- 'x.m', never 'x[k]'. Bx names a cache (LhatMemberCache),
    // which carries the key as well, so a hit is two comparisons instead of
    // a walk of the definition chain and a probe with a full key equality
    // in it. A miss does exactly what GETINDEX does and fills the cache.
    //
    // 5.1: this is a specialisation and nothing else -- the answer is the
    // one GETINDEX would give, so strictness plays no part (4.2) and the
    // instruction is emitted whether or not anything was checked.
    //
    // C is one byte, so a body reads at most 256 members this way and the
    // rest fall back to GETINDEX -- the same answer, unremembered.
    LHAT_BC_GETMEMBER,  // A B C R[A] = R[B].<cache[C]>

    LHAT_BC_SETINDEX,   // A B C R[A][R[B]] = R[C]
    // 02 の 13.8改: the checker settles the width where it ran, so this is
    // what relaxed and an unchecked compile fall to -- and it is what stands
    // between an error arriving where a run was expected and a register being
    // read as a position.
    LHAT_BC_CHECKRUN,   // A B   R[A] is the head of a run of B positions
    // 02 の 13.8改: pack^ -- the one bridge from a tuple to a table. A tuple
    // is not a value a name can hold; this makes one that is.
    LHAT_BC_PACK,       // A B   R[A] = t^{...} of the run of B at R[A]
    // 02 の 13.8改: a tuple written as a value (.(a, b).). The positions are
    // already in the slots above; this puts the head down over them. The one
    // place a run is built without crossing a frame boundary -- RETURN, YIELD
    // and a table.s walk all have the machine make the head instead.
    LHAT_BC_MAKERUN,    // A B   R[A] = the head of the B positions at R[A+1]
    LHAT_BC_ADDOVERLOAD,// A B C R[A][R[B]] gains R[C] as another way to call it
    LHAT_BC_OVERRIDEINDEX, // A B C R[A][R[B]] := R[C], ahead of any overload
    // 03 の 5.11c: the same write, once the checker has said which arm is
    // being replaced. The group then keeps its shape -- the arm goes in the
    // place of the one it replaces instead of in front of it, and that one is
    // dropped rather than left shadowed, so the arms a call can reach are the
    // arms the checker's type says the name carries. super^ still reaches the
    // old group: the GETINDEX binding it ran before this write (14.12改).
    //
    // The value is at R[B+1] rather than in an operand of its own, since C is
    // spent on the arm. compile_def lays the key and the value out as one run
    // the way 5.3 lays out a call, and falls back to OVERRIDEINDEX if ever
    // they are not adjacent.
    LHAT_BC_OVERRIDEARM,// A B C R[A][R[B]] := R[B+1], replacing arm C

    // 04. An error carries its kind and a table of fields, and the tests are
    // instructions because 5.1 keeps the machine independent of the checker.
    LHAT_BC_NEWERROR,   // A B   R[A] = an error of the kind in R[B]
    LHAT_BC_ISERROR,    // A B   R[A] = R[B] is an error of any kind
    // 02 の 13.11: the one question isa^ asks, whatever is written on its
    // right. R[C] carries the type: either an LhatRuntimeType the compiler
    // lowered the annotation into -- the same object as^ hands ASCAST -- or,
    // where the name reaches something only the run knows (a def^, 14.9),
    // the definition itself, tested structurally. An error kind and a host
    // type are both the first case, so this replaced ISKIND/ISHOSTDATA.
    LHAT_BC_ISA,        // A B C R[A] = R[B] isa^ R[C]
    LHAT_BC_ISNIL,      // A B   R[A] = R[B] is nil^   (02 の 11.7)

    // 02 の 14 章. A definition is a table of shared members plus the
    // prototype its self^ member holds (14.11); an instance is a copy of
    // that prototype -- a table in a field copied as its own tree, a
    // definition among the values shared -- reading the shared members
    // through a link fixed when it was made (14.2).
    LHAT_BC_NEWINSTANCE,  // A B   R[A] = an instance of the definition R[B]
    // 14.11: hangs R[B] under R[A]'s self^ as the prototype construction
    // copies, sealing it and linking it to the definition first. Refuses a
    // value nothing may share (LHAT_RUN_MUTABLE_DEFAULT) -- a coroutine, a
    // host object. Emitted once per def^, after the members.
    LHAT_BC_SETPROTO,     // A B   R[A].self^ = R[B], sealed
    // 05 の 8.9: box^ -- the host value whose head sits at R[B], put in the
    // box the heap can hold. The width travels with the head slot's tag, so
    // one operand names the whole run.
    LHAT_BC_BOX,          // A B C R[A] = a box holding the value at R[B..];
                          //       C bit 0 seals it (constbox^), bit 1 reads
                          //       R[B] as a box to copy
    LHAT_BC_CALLMETHOD,   // A B C R[A] = R[A](R[A+1] .. R[A+B]), where R[A+1]
                          //       is the receiver and is passed only when the
                          //       callee takes self^ (14.4). C as LHAT_BC_CALL.

    // 5.3: the same two calls, written where the caller has nothing left to do
    // with its frame -- 'return^ f(x)', and a bare call standing last in a
    // body. Operands mean what they mean above; what these add is permission
    // to run the callee in this frame rather than one on top of it. The
    // machine takes it only where the frame is free to go (no cleanup pending,
    // not a coroutine's, not a session's top level) and where the callee is an
    // L^ closure; anywhere else these run as the plain calls they are, and the
    // RETURN written after them answers as it always did.
    LHAT_BC_TAILCALL,
    LHAT_BC_TAILCALLMETHOD,

    // 5.5: the frame holds the cleanups it has not run, and every exit drains
    // them. One instruction pushes, one drains, one ends a cleanup body -- so
    // a finally^ and a with^ are the same thing to the machine.
    LHAT_BC_PUSHCLEANUP,  // Bx  remember the body at Bx (an instruction index)
    LHAT_BC_POPCLEANUP,   // A   run the cleanups above depth A, innermost first
    LHAT_BC_ENDCLEANUP,   //     the end of a cleanup body

    // 02 の 15.4: bidirectional. The value goes out and what the resume
    // supplies comes back starting at the same register. 13.8改: B != 0
    // means what goes out is a tuple -- R[A] is its head and B positions
    // follow. What comes back is one value in R[A], or, when the resume
    // sent several, a run -- R[A] its head and the positions after it; the
    // yield^'s own binding reserved the width and takes it apart.
// 05 の 8.9: the B that says a yield carries one host value laid out whole
// rather than B run positions. Out of every run width (LHAT_MAX_TUPLE is 31).
#define LHAT_YIELD_HOSTVALUE 0xFF
// And RESUME's C for the one-name loop whose focus is a host value: the low
// bits carry the width the loop reserved, and the walk modes still read the
// step as one value.
#define LHAT_RESUME_WIDE 0x80

    LHAT_BC_YIELD,      // A B   yield R[A]; R[A..] = what the resume sent.
                        //       B = LHAT_YIELD_HOSTVALUE: R[A..] is one host
                        //       value laid out whole (05 の 8.9)

    // 02 の 15.8: delegation, which compiles to the loop 5.7 writes out.
    // A run sitting in R[A] (a several-send arriving through this frame's
    // own suspension) is forwarded whole, positions and all.
    LHAT_BC_RESUME,     // A B   R[A] = resume the coroutine R[B], sending R[A]
    LHAT_BC_ISDONE,     // A B   R[A] = the coroutine R[B] has finished

    // 13.8改: B != 0 means the answer is a tuple of B positions, sitting in
    // R[A] .. R[A+B-1]. No head slot here -- the head is the machine's, put
    // down in the caller's frame where the width has to be readable off the
    // value itself (a union with an error tells its arms apart by that tag).
    LHAT_BC_RETURN,     // A B   return R[A], or the B positions from R[A]
    LHAT_BC_RETURN_NIL,

    LHAT_BC_PANIC,      // A     panic^ R[A]  (04 の 11.6)
    // 05 の 8.6: mark R[A] as the machine's own, so that no later
    // instruction may write into it. Emitted where a unit builds what
    // require^ answers with -- what a table holds is written before this,
    // and nothing writes into it afterwards.
    LHAT_BC_SEAL,       // A     R[A] accepts no further writes
    // 11.6改3: R[A] is unchanged where it satisfies the type in R[B] -- as^
    // narrows what the checker tracks, not the value. Where it does not,
    // R[A] becomes a localerror^.CastFailure, which is the other arm of
    // what as^ answers. C is unused.
    LHAT_BC_ASCAST,     // A B   R[A] = R[A], or a CastFailure

    LHAT_BC_COUNT
} LhatOpcode;

// 5.2. Bx is signed for a jump and unsigned everywhere else; the two are read
// through different accessors rather than stored differently.
#define LHAT_BX_BIAS 32768

// LHAT_BC_CALL's C, which was a boolean with a whole byte to itself.
#define LHAT_CALL_SPREAD 0x01u          // 13.7's 'expr...'
#define LHAT_CALL_PREPARED_SHIFT 1      // 13.8改: slots reserved for the answer
// Six bits, which is more than the room a frame's answer can take: 13.8改's
// head plus LHAT_MAX_TUPLE positions is 32. The top bit is the drop below.
#define LHAT_CALL_PREPARED_MAX 0x3Fu
// 5.3: a tail call whose answer is thrown away -- a bare call standing last in
// a body, where what the frame answers is the nil^ of falling off the end.
// Meaningless on the plain calls, which never discard what they answer.
#define LHAT_CALL_DROP 0x80u

static inline unsigned lhat_call_prepared(uint8_t c)
{
    return ((unsigned)c >> LHAT_CALL_PREPARED_SHIFT) & LHAT_CALL_PREPARED_MAX;
}

static inline uint8_t lhat_call_operand(bool spread, unsigned prepared)
{
    return (uint8_t)((spread ? LHAT_CALL_SPREAD : 0u) |
                     ((prepared & LHAT_CALL_PREPARED_MAX)
                      << LHAT_CALL_PREPARED_SHIFT));
}

static inline LhatInstruction lhat_encode_abc(LhatOpcode op, uint8_t a,
                                              uint8_t b, uint8_t c)
{
    return (LhatInstruction)op | ((LhatInstruction)a << 8) |
           ((LhatInstruction)b << 16) | ((LhatInstruction)c << 24);
}

static inline LhatInstruction lhat_encode_abx(LhatOpcode op, uint8_t a,
                                              uint16_t bx)
{
    return (LhatInstruction)op | ((LhatInstruction)a << 8) |
           ((LhatInstruction)bx << 16);
}

static inline LhatInstruction lhat_encode_jump(LhatOpcode op, uint8_t a,
                                               int32_t offset)
{
    return lhat_encode_abx(op, a, (uint16_t)(offset + LHAT_BX_BIAS));
}

static inline LhatOpcode lhat_op(LhatInstruction i) { return (LhatOpcode)(i & 0xFF); }
static inline uint8_t lhat_a(LhatInstruction i)  { return (uint8_t)(i >> 8); }
static inline uint8_t lhat_b(LhatInstruction i)  { return (uint8_t)(i >> 16); }
static inline uint8_t lhat_c(LhatInstruction i)  { return (uint8_t)(i >> 24); }
static inline uint16_t lhat_bx(LhatInstruction i) { return (uint16_t)(i >> 16); }
static inline int32_t lhat_jump_offset(LhatInstruction i)
{
    return (int32_t)lhat_bx(i) - LHAT_BX_BIAS;
}

// 03 の 5.1改: what one written 'x.m' remembers about the last receiver it
// met. The cache belongs to the CALL SITE -- one of these per member read in
// the source -- and not to a value: the member itself is shared (14.3 puts it
// on the definition), so what a site sees over and over is the same place.
//
// `answered` is the table the value was found in, which is the receiver's own
// for a host type (05 の 8.8 shares one table per type) and the definition's
// for an instance (14.7 walks there when the instance has no such key of its
// own). `version` is that table's when it was found: a layout change moves
// what is where, and the read is refused until it is looked up again.
//
// `from_definition` is what makes a site hit across instances of one def^.
// The instance is a fresh table per value, so comparing it would never
// match -- what is compared instead is that the receiver has never been
// structurally written (version 0) and points at the cached definition.
// 5.10 seals the prototype, so an untouched clone of it carries exactly the
// prototype's keys and cannot be shadowing the member.
typedef struct {
    uint16_t key;  // which constant names the member
    const struct LhatTable *answered;
    uint32_t version;
    uint32_t index;  // where in `answered`'s entries it was
    bool from_definition;
} LhatMemberCache;

// One compiled body: its instructions, the constants they name, and how many
// registers a frame needs. 5.2 fixes the frame size at compile time.
typedef struct {
    LhatInstruction *code;
    // 04 の 11 章: the source line each instruction came from, one entry per
    // instruction (parallel to `code`, same count/capacity). A runtime
    // fault names where it happened; strictness (03 の 4.2) never depends
    // on this, so it costs nothing but the memory.
    uint32_t *lines;
    size_t count;
    size_t capacity;

    LhatValue *constants;
    size_t constant_count;
    size_t constant_capacity;

    // The strings the constants name. A constant lives as long as the chunk,
    // so the chunk owns them rather than the machine that runs it.
    LhatHeap heap;

    // 03 の 5.1改: one per LHAT_BC_GETMEMBER, indexed by its C. Written
    // while running and read while running -- nothing here takes part in what
    // the body means, so 4.2 is untouched: clearing every one of these
    // changes only how long the same answers take.
    //
    // The tables it points at belong to a machine, and a chunk outlives none
    // of them -- but it may be shared by several (std.thread), so a hit is
    // only ever a hit for the machine that filled it. That is what comparing
    // the pointer takes care of: another machine's table is another pointer.
    LhatMemberCache *member_caches;
    size_t member_cache_count;
    size_t member_cache_capacity;

    uint8_t registers;
} LhatChunk;

// 5.4: how a closure gets each of the places it shares. A register of the
// frame making it, one of that frame's own upvalues -- the second is what
// carries a name down through more than one level of nesting -- or, for
// 15.10's this^^, the maker's own closure: no register ever holds it, so
// at CLOSURE time it is boxed closed on the spot.
typedef enum {
    LHAT_UPVALUE_REGISTER,
    LHAT_UPVALUE_OUTER,
    LHAT_UPVALUE_THIS
} LhatUpvalueSource;

typedef struct {
    LhatUpvalueSource source;
    uint8_t index;  // unused for LHAT_UPVALUE_THIS
} LhatUpvalueDesc;

// One compiled subroutine: its own code, the bodies written inside it, and
// what it captures. 02 の 14.9 keeps the name out of this; a proto is the
// shape of a body, not a named thing. Declared in lhat/module.h, where a
// host meets it as a handle; completed here.
// 05 の 5.3: what a unit's require^s reach, by the number the compiler
// wrote into each UNIT instruction. One table per unit, owned by the unit's
// own proto and pointed at by every body written inside it -- so a machine
// carries no list of units, and a program may grow (lhat_program_compile
// again, or std.load) under machines already running, each proto knowing
// its own units. An entry is NULL where the unit it names never compiled.
typedef struct {
    const struct LhatProto **protos;
    size_t count;
} LhatUnitTable;

struct LhatProto {
    LhatChunk chunk;

    // See LhatUnitTable. NULL for a body no program compiled (lhat_compile,
    // a session's input), where a require^ has nowhere to go anyway.
    const LhatUnitTable *units;
    // 05 の 5.6: the heap object that owns this tree, when a machine does
    // (lhat_machine_adopt_script) rather than a program. NULL otherwise.
    // Set on every proto of the tree, since a closure of any of them is
    // what keeps the whole alive.
    struct LhatObject *owner;
    // The top level of a unit -- what owns `units`, and what a traceback
    // calls "the top level".
    bool is_unit;

    // 04 の 11.6改: what a traceback prints for a frame of this body. Debug
    // labels only -- 14.9 stands: neither takes any part in typing or in
    // identity. `debug_name` is the binding or member the body was written
    // under (NULL for a bare f^), `source_name` the unit's path (NULL for a
    // session's input). Each proto owns its copies.
    char *debug_name;
    char *source_name;

    // 03 の 4.3: how many registers already hold values when this unit
    // starts. Zero everywhere but the second and later inputs of a REPL,
    // where the top-level names of the earlier ones are still in them.
    uint8_t reserved;

    // And how many it leaves behind for the inputs after it. Those slots
    // outlive the run, so 5.4's shared places pointing into them must not be
    // closed when the frame goes -- see the drain in vm.c. Zero outside a
    // session, where the frame really does take its whole width with it.
    uint8_t kept;

    uint8_t parameters;
    // 05 の 8.9: how many slots those parameters occupy, which is not the
    // count -- a host value parameter is one parameter and as many slots as
    // its type is wide. What the caller lays down ends here, so this is where
    // the frame's scratch begins (vm.c's clear_scratch).
    uint8_t parameter_slots;
    bool is_function;  // f^ rather than p^ (02 の 15 章)
    bool yields;       // 02 の 15.2: the body contains yield^, so calling it
                       // answers a coroutine rather than running it (15.5)
    bool takes_self;   // 02 の 14.4: the first parameter is written self^,
                       // which is what makes it an instance method
    bool self_last;    // 02 の 11.3改: the self^ was written last
                       // instead, which only an op^ may do -- it says the
                       // RIGHT operand is the receiver, so '1 + v' reaches it.
                       // The receiver still occupies a parameter slot; this
                       // says which one.

    struct LhatProto **protos;
    size_t proto_count;
    size_t proto_capacity;

    LhatUpvalueDesc *upvalues;
    size_t upvalue_count;
    size_t upvalue_capacity;

    // 02 の 14.12: what each parameter was declared to take, kept so that an
    // overloaded call can find the candidate that fits. NULL where nothing
    // was written, which asks nothing.
    struct LhatRuntimeType **parameter_types;

    // 02 の 14.16: what typeof^ reconstructs a subroutine value's signature
    // from. NULL when nothing was written -- 13.2 makes an f^ declare one, so
    // its absence here belongs to a p^.
    struct LhatRuntimeType *result_type;

    // 15.2, 13.9. What a yielding body's yield^ sites agreed on --
    // there is no written form for either, so these come only from 03 の
    // 5.11a's checked_type when checking ran; NULL otherwise (yields is
    // false, or the checker never settled one).
    struct LhatRuntimeType *yield_produce_type;  // Y
    struct LhatRuntimeType *yield_receive_type;  // R
    // 13.9 with 13.8改: how many arguments a resume of this body sends --
    // 0 when R is empty, 1 for a single R, and the tuple's width when the
    // yield^'s binding takes several apart. Kept apart from the type above
    // because a NULL there means two things -- an empty slot, and a proto
    // the checker never reached.
    //
    // Only `yield_receives_known` protos say which; without checking there
    // is nothing to read R off, and 03 の 4.2 keeps the run the same either
    // way, so the machine takes a resume of an unknown one with any count a
    // tuple could carry rather than picking one the checker might disagree
    // with.
    bool yield_receives_known;
    uint8_t yield_receive_count;
    // 13.9: the body cannot end, so `result_type` being NULL here says "no
    // last resume" rather than "ends without a value". typeof^ writes it '-'.
    bool yield_endless;

    // 13.7: the last parameter collects the rest into a table rather than
    // taking one argument for itself. Its element type is
    // parameter_types[parameters - 1] -- the same array, since a variadic
    // parameter is still one slot the calling convention reserves, only what
    // a call site owes it differs (13.7's ">=" rather than "==").
    bool has_variadic;
};

LhatProto *lhat_proto_new(void);

// Returns the index of the nested body, or SIZE_MAX when out of memory.
size_t lhat_proto_add(LhatProto *parent, LhatProto *child);
size_t lhat_proto_add_upvalue(LhatProto *proto, LhatUpvalueSource source,
                              uint8_t index);

// Hands a unit its table (see LhatUnitTable): every body inside `unit`
// comes to point at it, and `protos` -- the caller's array, taken over --
// is freed with the unit. False when out of memory, and then `protos` is
// still the caller's.
bool lhat_proto_give_units(LhatProto *unit, const LhatProto **protos,
                           size_t count);

// 05 の 5.7: hands every object the bodies of this tree hold over to `into`,
// leaving their chunks with nothing to free.
//
// A chunk's objects are born black because they were meant to outlive every
// machine (lhat_chunk_init), and a program's units always did. Retiring a
// body does not change what a machine may be holding: a string constant is
// what LOADK puts in a register, and what a program then stores as a table
// key -- L^.modules is keyed by the very strings the unit prologue loads --
// so the objects are reachable from the machine's own tables long after the
// body that named them is gone. The code may be freed. These may not, until
// the program itself goes.
void lhat_proto_give_objects(LhatProto *proto, LhatHeap *into);

void lhat_chunk_init(LhatChunk *chunk);
void lhat_chunk_dispose(LhatChunk *chunk);

// Returns the index of the instruction, so a jump written later can be
// patched by the caller that recorded it. `line` is the source line the
// instruction came from, 0 when there is none to give.
size_t lhat_chunk_emit(LhatChunk *chunk, LhatInstruction instruction,
                       uint32_t line);

// Adds a constant, reusing an equal one rather than storing it twice.
// Returns the index, or SIZE_MAX when the pool is full or out of memory.
size_t lhat_chunk_constant(LhatChunk *chunk, LhatValue value);

// The same for a string literal: the bytes are copied into a string the chunk
// owns. Two literals spelling the same thing share one constant, which is
// what makes t.foo and t["foo"] one key.
// 03 の 5.1改: a fresh cache for one written member read, answering its Bx.
// `key` is the constant that names the member. SIZE_MAX when there is no
// memory or the chunk already holds 65536 of them -- the caller then emits
// the unspecialised read, which answers the same thing.
size_t lhat_chunk_member_cache(LhatChunk *chunk, uint16_t key);

size_t lhat_chunk_string(LhatChunk *chunk, const char *text, size_t length);

// Rewrites the Bx of the jump at `at` so that it lands on the instruction
// after the last one emitted. Used for a branch whose target is not known
// until its body has been compiled.
void lhat_chunk_patch_here(LhatChunk *chunk, size_t at);

const char *lhat_opcode_name(LhatOpcode op);

// Writes one instruction in a readable form. `index` is what a jump is
// measured against.
void lhat_chunk_print(const LhatChunk *chunk, size_t index, char *out,
                      size_t size);

#endif  // LHAT_CODE_H
