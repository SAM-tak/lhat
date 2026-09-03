// L^ (lhat) -- the machine: running compiled bytecode.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// This is the loop that follows 1.1's fourth stage (compile.h). Safety is
// strict's static checking; 5.1 starts every instruction generic, checking
// what it is given so a wrong value stops the run instead of corrupting the
// machine -- which is also what lets relaxed (3.5) run on this same machine,
// and what specialisation later removes where strict settled the types.
// Nothing here reaches the front end: a unit already compiled to bytecode
// needs no lexer, no parser and no checker (02 の 14.17改2), and this header
// is what keeps that true at the include level.

#ifndef LHAT_VM_H
#define LHAT_VM_H

#include <stdbool.h>
#include <stddef.h>

#include "lhat/module.h"
#include "lhat/object.h"
#include "lhat/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LHAT_RUN_OK,
    LHAT_RUN_TYPE_ERROR,      // 5.1: an instruction was given the wrong thing
    LHAT_RUN_NOT_CALLABLE,    // called something that is not a subroutine
    LHAT_RUN_ARITY,           // 5.3: the wrong number of arguments
    LHAT_RUN_STACK_OVERFLOW,  // the frames went too deep
    LHAT_RUN_OUT_OF_MEMORY,
    LHAT_RUN_BAD_KEY,         // 04 の 11.3: nil^ means "not there", so it
                              // cannot also be a key
    LHAT_RUN_BAD_FORMAT,      // 02 の 14.17: tostring was handed a format a
                              // number^ cannot be written through
    LHAT_RUN_DEAD_COROUTINE,  // resumed one that has finished
    LHAT_RUN_YIELD_OUTSIDE,   // 02 の 15.2 and 10.7: yield^ where nothing is
                              // waiting to be resumed
    LHAT_RUN_NO_CANDIDATE,    // 02 の 14.12: no signature of an overloaded
                              // member takes what the call handed over
    LHAT_RUN_COROUTINE_NOT_STARTED,     // 15.2: resumed one that has never
                                         // been started
    LHAT_RUN_COROUTINE_ALREADY_STARTED, // 15.2: started one that already
                                         // has been
    LHAT_RUN_SEALED,          // 05 の 8.6: wrote into a table the
                              // machine owns -- L^, or what require^ answers
                              // with. check.c refuses the ones it can name;
                              // this is one reached through a parameter
    LHAT_RUN_MUTABLE_DEFAULT, // 02 の 14.11: a definition's prototype took a
                              // value nothing may share -- a coroutine, a
                              // host object -- or a default tree deeper than
                              // the machine follows. The checker refuses
                              // these where it ran; this is the same question
                              // asked where the values actually are
    LHAT_RUN_NO_SUCH_UNIT,    // 05 の 5.3: a require^ reached for a unit the
                              // machine was not given
    LHAT_RUN_TUPLE_ARITY,     // 02 の 13.8改: a call reserved slots for one
                              // width of tuple and the callee answered
                              // another -- or answered a single value where
                              // a run was expected. Settled by the type
                              // wherever the checker ran; this is where an
                              // unchecked compile, 03 の 4.3's session and
                              // 05 の 5.3's separately compiled units land
    LHAT_RUN_TUPLE_UNEXPECTED,// 02 の 13.8改: the other direction -- a tuple
                              // came back where the call site reserved one
                              // slot. Not quietly boxed into a table: a
                              // tuple and a t^{...} are different types, so
                              // converting between them is the program's
                              // word (pack^) and never the machine's
    LHAT_RUN_PANIC            // 04 の 11.6: panic^, written by the program
                              // itself -- `value` is what it panicked with
} LhatRunStatus;

typedef struct {
    LhatRunStatus status;
    LhatValue value;   // what the unit returned, or nil^

    // 02 の 13.8改: the several values, when the answer was a tuple. `value`
    // is position 1 then, so a host that reads only it still reads a value
    // it can use -- which is why nothing written before tuples has to change.
    // `positions` aims into the machine's own room and is good until the
    // next run or call on that machine; copy what has to outlive it.
    // `position_count` is 0 for an ordinary one-value answer.
    const LhatValue *positions;
    size_t position_count;

    size_t at;         // the instruction that failed, when one did
    // 04 の 11 章: where `at` came from, for a host to report -- 0 when
    // status is LHAT_RUN_OK, since nothing failed to name a line for.
    uint32_t line;
    // 02 の 11.8: the operator `at` was, when it was one -- NULL for a
    // fault that is not about an operator (a bad call, a full stack, ...).
    const char *op_name;
    size_t op_name_length;

    // How many objects the collector freed while the program ran, and how
    // many were still live at the end. Kept so that a test can see the
    // collector working -- nothing in the language reads them.
    size_t collected;
    size_t live;
} LhatRunResult;

// 03 の 4 章: a REPL is one machine answering many inputs, so the machine is
// the caller's rather than any one run's. It holds the stack, the frames and
// everything a program allocates, which is what lets a value one input made
// still be there for the next.
//
// It is large -- a whole stack and a frame array -- so it comes from the heap
// and a caller keeps the handle rather than the object. The inside of it is
// machine.h, which vm.c and gc.c include and nothing else does.
typedef struct LhatMachine LhatMachine;

LhatMachine *lhat_machine_new(void);

// Frees the machine and everything still allocated on it. Any LhatValue that
// came out of a run on it points into that, so nothing read from a result
// outlives this call.
void lhat_machine_dispose(LhatMachine *machine);

// 05 の 8.6: what L^.collectgarbage() is, for a host that has a machine and
// no L^ code it wants to run to reach one. A whole cycle, now: what the
// machine cannot reach when this is called has been freed when it answers.
// It is the only thing that gives a pause the size of the heap, which is why
// it has to be asked for by name.
//
// Answers how many objects are still live -- the same count
// LhatRunResult.live carries. Objects, not bytes: a heap counts what it
// holds and not how large each one is, so this is a figure to watch rather
// than one to report as memory.
//
// May be called from inside a host function of a running machine: every
// frame's registers are roots for as long as the frame is there.
//
// WHAT A HOST IS HOLDING IS NOT A ROOT. The machine reaches L^ itself (and
// so everything under L^.modules), every frame's closure, coroutine and
// registers, the places closures share, the room a host is answering
// through, and the last fault -- and nothing else. A value made with
// lhat_machine_make_* and not yet written anywhere the machine can reach is
// unreachable by that account, and this call is exactly what takes it. Put
// it somewhere first, or collect before making it rather than after.
//
// 02 の 10.7: a coroutine dropped with cleanups still pending has them run
// here, as L^.collectgarbage() does -- so this runs L^ code, and a finally^
// or a with^ of a dropped coroutine is what it runs. Only what the cycle
// above found: a cleanup that drops another coroutine leaves that one for the
// next call, so this cannot be made to go on for ever.
//
// It is what makes the heap settled when this answers, and a host retiring
// compiled bodies needs that: until its cleanups have run, a dropped
// coroutine still holds the closure it was suspended in, and so the body
// that closure was made from. See lhat_program_discard_retired.
//
// Some cannot be run, and then they keep: one whose cleanup faulted (read
// lhat_machine_fault_* for it), one waiting while a disposal is already
// under way -- this call refuses to interleave two unwindings, as the
// interpreter does -- and one there is no frame or stack room for.
// lhat_machine_pending_disposals below is how a host tells.
size_t lhat_machine_collectgarbage(LhatMachine *machine);

// 02 の 10.7: how many dropped coroutines are still waiting to have their
// cleanups run. Zero after an ordinary lhat_machine_collectgarbage, which is
// what a host wants to see before it frees anything those cleanups would
// run: a waiting one still holds the closure it was suspended in.
size_t lhat_machine_pending_disposals(const LhatMachine *machine);

// 05 の 5.7: whether anything on the machine's heap still runs one of
// `bodies` -- a closure of one, or a coroutine suspended in one. The nested
// bodies of a unit are the caller's to include; lhat_reload flattens them.
//
// Ask after lhat_machine_collectgarbage, when the heap holds only what is
// reachable. Asked before one, a dropped closure not yet swept still
// answers true -- which errs the only safe way there is.
bool lhat_machine_holds_body(const LhatMachine *machine,
                             const LhatProto *const *bodies, size_t count);

// 02 の 14.16: what typeof^ answers, asked from C. The same reading, so a
// host and a program never learn different things about one value.
//
// What this is FOR: a host that has to act on what a value's type says
// rather than on a string beside it. A def^ wrapping something the host
// made carries the truth in its constructor's signature -- ask this of the
// `new` member, read parts[0], and a HOSTDATA answer hands back the very
// tag the host registered (hostdata_tag->module and ->name in object.h).
// Writing the class name out beside the wrapper is the other way, and
// nothing keeps the two in step.
//
// A subroutine answers from the types its proto carries, which were made
// at compile time -- including a parameter nothing was written for, whose
// type inference settled (03 の 3.4). A table answers t^ and no more: what
// a structure holds deeply is the checker's to say, at compile time. For
// the declared shape of a public^ name -- a def^'s self^{...} included --
// ask the unit instead: lhat_unit_export_type (program.h) answers the
// walkable descriptor, and lhat_unit_export_type_text the 14.16 spelling.
//
// The answer is the machine's, so it is readable until the next lhat_run /
// lhat_machine_call / lhat_machine_resume or the machine's disposal,
// whichever comes first -- the same lifetime the fault frames below have.
// What hangs off it is longer-lived: a signature's parameter types belong
// to the chunk that compiled them, which outlives every machine, and are
// answered here rather than copied. NULL only out of memory.
const LhatRuntimeType *lhat_value_type(LhatMachine *machine,
                                       LhatValue value);

// 05 の 8.7: the three steps a host registration takes at run time. Kept here
// rather than in program.c because only this file may touch the heap -- the
// values belong to the machine and the collector has to see them.
bool lhat_machine_make_table(LhatMachine *machine, LhatValue *out);

// object.h's lhat_table_set with 5.12's barrier around it, which is what a
// host writing into a table the machine already holds has to go through.
//
// The collector runs between instructions and no further, so a cycle can be
// left half done when a run ends -- and a table that was marked before it
// ended is black. A white value written into one of those with no barrier is
// unreachable as far as the next step can see, and is swept while live. The
// barrier is nothing at all when the table is young or no cycle is under way,
// so there is no call site that has to know which it has.
//
// A table the host has just made and not handed over yet is the one case
// that does not need this, and using it there costs nothing either.
bool lhat_machine_table_set(LhatMachine *machine, LhatTable *table,
                            LhatValue key, LhatValue value, bool *refused);

// 13.7: `has_variadic` makes `parameters` a floor rather than an exact count,
// so the registration's signature ended in '...' and a call may write more.
// The extra arguments reach LhatHostFn as the tail of `arguments`, uncollected
// -- there is nothing to collect them into that a C function would want.
// 02 の 11.3改: `self_last` says the receiver is the operand written after
// the member, which only the two operator searches read.
//
// 02 の 14.12: `parameter_types` is what the search resolving an overloaded
// call asks each candidate, one entry per declared parameter (self^ is not
// one -- 13.4). NULL leaves the candidate judged by its counts alone, which
// is what a registration that built none gets. The array is taken over by
// the host and freed with it; the types in it belong to the machine.
bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters,
                            bool has_variadic, bool takes_self, bool self_last,
                            struct LhatRuntimeType **parameter_types,
                            LhatValue *out);

// Puts `value` at L^.modules.<module>[.<type>].<name>, making the tables on
// the way the way 02 の 8.8 does. `type` is NULL for a member of the module
// itself.
bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name,
                           LhatValue value);

// 05 の 5.7: takes L^.modules.<module> away again, so that running a unit
// registered under that path registers it afresh.
//
// A unit loads once because its own body begins by asking the registry for
// itself and answering what is there (5.3) -- so a program that retired a
// unit and compiled it again has to say so here, or the new body will find
// the old table and hand that back instead. Once per machine the program was
// installed on; the program does not know its machines (a machine is given
// nothing) and cannot do it for you.
//
// `module` is the dotted path the unit declared with module^, which
// lhat_unit_module_name answers with. The tables above it stay -- other
// modules live under them. False when nothing stood there, which is also
// what a script (3.2) answers, since one registers nothing.
bool lhat_machine_forget_unit(LhatMachine *machine, const char *module);

// 05 の 8.6: a member of L^ itself. Writes through the machine rather than
// through an instruction, which is why 8.6改's seal does not refuse it.
bool lhat_machine_set_global(LhatMachine *machine, const char *name,
                             LhatValue value);

// 05 の 8.8: a value standing for something the host made. `pointer` is the
// host's and nothing here reads it; `tag` is what a later call checks before
// reading it as the type it expects, and comes from lhat_register_hostdata_type.
//
// Members written against the type answer through the value, so it is the
// registered type that has to have been installed first.
bool lhat_machine_make_hostdata(LhatMachine *machine, const LhatHostDataTag *tag,
                            void *pointer, LhatValue *out);

// 04 の 2.5 の host 版: error^kind{message:=..., cause:=...} が LHAT_BC_NEWERROR
// と compile_error_new のフィールド既定処理でしていることを、C から直接行う。
// `kind` は lhat_register_error_kind が返したもの(host 登録は v1 ではフィール
// ド無し限定なので、message/cause 以外のフィールドを埋める経路はない -- 04 の
// 2.3 が言う「どの種別も持つ」2つだけがここで書ける)。`message` は NULL なら
// ""、`cause` は lhat_nil() なら書かれない(2.3 の既定と同じ)。
bool lhat_machine_make_error(LhatMachine *machine, const LhatErrorKind *kind,
                             const char *message, LhatValue cause,
                             LhatValue *out);

// The pointer back, or NULL when the value is not one of these or was made
// with a different tag. 7.3 makes the tags distinct per declaration, so this
// is what stops a Texture reaching the C that expects a Sound.
void *lhat_hostdata_pointer(LhatValue value, const LhatHostDataTag *tag);

// 05 の 8.9: the bytes of a host value argument, or NULL when the argument
// is not one of `tag`'s -- the same double check lhat_hostdata_pointer
// makes, over bytes instead of a pointer. The bytes belong to the machine's
// stack and are good for the duration of the call that handed them over;
// writing through them changes a scratch copy and nothing the program sees.
void *lhat_hostvalue_data(LhatValue argument, const LhatHostValueTag *tag);

// 05 の 8.9: the value a host answers with -- `size` bytes (the registered
// size) copied into the machine's scratch and handed back as a value the
// machine writes out whole the moment the call returns. Never stored: the
// scratch has room for exactly one answer, which is all a call can make.
// False when the tag is NULL or the machine is.
bool lhat_make_hostvalue(LhatMachine *machine, const LhatHostValueTag *tag,
                         const void *bytes, LhatValue *out);

// 05 の 8.9改: the same value placed in the caller's own room instead of the
// machine's scratch -- the scratch holds exactly one answer, and arguments
// may stand several at once. The room is the caller's to keep until the call
// that takes them returns; lhat_machine_call and lhat_machine_call_member
// widen such an argument into the callee's frame, head and continuation
// slots, so a parameter written against the registered type receives the
// value whole. No machine is involved: the room is plain memory of the
// caller's. False when the tag is NULL, the bytes are, or the registered
// size outgrows the room.
typedef struct {
    LhatValueUnion run[1 + LHAT_HOSTVALUE_MAX_BYTES / 8];
} LhatHostValueRoom;

bool lhat_place_hostvalue(const LhatHostValueTag *tag, const void *bytes,
                          LhatHostValueRoom *room, LhatValue *out);

// 05 の 8.7改2: the enum a registration described, built on this machine
// the way 02 の 19 章's declaration builds one -- the enum object, its
// sealed members table, one enumerator per name with its integer. `decl`
// is the identity descriptor the registration made (fits^ compares it).
// For lhat_program_install; a host wanting one machine's enum reads it
// off L^.modules instead.
// 05 の 8.7 の読み: what stands at L^.modules.<module>[.<type>].<name> on
// this machine -- the read twin of lhat_machine_register. A host that
// registered an enum reads it back through this to hand its members out
// again: an enumerator is a singleton, so the one the machine built is the
// one a program compares against. False when nothing of that name is
// there, with `out` left nil^.
//
// The value belongs to the machine and is reachable from L^, so a host may
// keep it for as long as the machine lives -- what it must not do is keep
// it after lhat_machine_dispose.
bool lhat_machine_registered(LhatMachine *machine, const char *module,
                             const char *type, const char *name,
                             LhatValue *out);

bool lhat_machine_make_enum(LhatMachine *machine, const char *name,
                            const struct LhatRuntimeType *decl,
                            const char *const *members,
                            const int64_t *values, size_t count,
                            LhatValue *out);

// A string on `machine`'s own heap, made the way 05 の 8.7's registration
// makes a table or a host -- the machine has to make it, since the value's
// object header and its place in the collector's heap are the machine's to
// give (05 の 8.8: "LhatValue is not widened; the machine makes it").
bool lhat_machine_make_string(LhatMachine *machine, const char *text,
                              size_t length, LhatValue *out);

// A closure of `proto` on `machine`, with no upvalues. Answers false when
// `proto` captures anything (upvalue_count != 0) -- a captured place lives on
// whatever machine made it, and a closure carrying one is not safe to hand to
// another. What this is for is std.thread's spawn: a proto is shared,
// unwritten data (see the comment on lhat_chunk_init/lhat_proto_new), so
// wrapping it fresh on a machine of its own is always safe when there is
// nothing captured to carry across.
bool lhat_machine_make_closure(LhatMachine *machine, const LhatProto *proto,
                               LhatValue *out);

// The same with the captures carried in -- what stdlib/carry needs to
// rebuild a closure on another machine. The proto is shared (see above);
// the cells are made here, one per upvalue the proto declares, and handed
// over already holding their values. Two closures that captured the one
// place on the other side are rebuilt sharing one cell, which is why the
// cells are made apart from the closure: lhat_machine_make_cell makes a
// closed cell holding a value (its contents may be written later through
// lhat_upvalue_closed_ref, value.h, which is how a cycle through a closure
// is tied up), and this takes them. `count` has to be the proto's own.
// 05 の 8.7改2: what a host function does instead of answering, when what
// it was asked is a programmer's mistake -- love.graphics.draw handed a
// width of -1, say -- and not a failure a caller is meant to write a
// catch^ for. Call it inside the host function and return anything (the
// answer is dropped): when the function returns, the run ends exactly as if
// the call site had written 'panic^ value' -- LHAT_RUN_PANIC, `value` in
// LhatRunResult.value, the traceback standing at the call. An f^
// registration may panic as well as a p^: purity is the checker's
// concern, and a panic is the end of the run, not an effect within it.
// `_text` makes the string first; false when that did not fit.
void lhat_machine_panic(LhatMachine *machine, LhatValue value);
bool lhat_machine_panic_text(LhatMachine *machine, const char *text);

// 05 の 5.6: a closure of a proto the machine takes ownership of -- what
// lhat_program_load_* answered. The proto (the whole tree under it) is freed
// when the last closure of any body in it is collected; a program's own
// units never go this way. What its constants named and that got out -- the
// strings a table it answered has for keys -- becomes the machine's as the
// body goes, and is collected when nothing reaches it. The proto must
// capture nothing, as a unit's top level does not.
bool lhat_machine_adopt_script(LhatMachine *machine, LhatProto *proto,
                               LhatValue *out);

bool lhat_machine_make_cell(LhatMachine *machine, LhatValue held,
                            LhatUpvalue **out);
bool lhat_machine_make_closure_with(LhatMachine *machine,
                                    const LhatProto *proto,
                                    LhatUpvalue *const *cells, size_t count,
                                    LhatValue *out);

// Reading a closure back out: its proto, how many places it captured, each
// captured place's current value (an open cell reads through to the stack,
// a closed one from itself -- the machine's own dereference), and the
// place's identity -- an opaque token equal for two captures of the one
// place, which is what a copy needs to keep sharing shared.
const LhatProto *lhat_closure_proto(LhatValue closure);
size_t lhat_closure_capture_count(LhatValue closure);
LhatValue lhat_closure_capture(LhatValue closure, size_t index);
const void *lhat_closure_capture_id(LhatValue closure, size_t index);

// Runs `proto` on `machine`. What the run allocates belongs to the machine,
// so the answer is good until the machine is disposed or run again -- there
// is nothing in the result to free.
//
// 02 の 13.7 with 05 の 3.2: a script's top level is 'p^...', and
// `arguments` is what its '...' collects -- a command line, say. A module^
// unit takes none (LHAT_RUN_ARITY if given any). lhat_run is the same with
// nothing handed over.
LhatRunResult lhat_run_arguments(LhatMachine *machine, const LhatProto *proto,
                                 const LhatValue *arguments, size_t count);
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);

// Calls an ordinary L^ value the way an instruction would, without a proto of
// its own to run first -- for a host that already holds a callable value
// (05 の 8.7's `LhatHostFn` has no way to call back into L^ on its own, since
// a call site is normally compiled). `callee` has to be a plain subroutine
// (LHAT_OBJECT_SUBROUTINE) or a function the host registered
// (LHAT_OBJECT_HOST) -- not an overload^ group; arity is checked the same
// way a CALL instruction checks it, variadics collect the same way, but
// there is no self^ and no 'expr...' spread -- a host handing values over
// already has them as a flat array.
//
// 02 の 15.5: a yieldable procedure answers its coroutine rather than
// running, exactly as a compiled call does -- lua_newthread's shape at this
// boundary. lhat_machine_resume below is what drives it.
//
// May be called on a fresh machine (nothing run on it yet) or nested inside
// a running one -- what a call already on the stack looks like from here is
// only how many frames are already open, which the machine already knows.
LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count);

// 02 の 14.4: the same, for a member reached through a receiver -- what a
// compiled `x.m(a, b)` does, for a host that has the receiver and the name.
// Without this a host can hold an object of the language's and call nothing
// on it: an instance's members are shared and take self^, so there is no way
// to arrange the receiver from outside.
//
// 14.7's search is the one lhat_table_get already makes, so an instance
// reaches its definition's members here as it does anywhere. 14.12's
// overloads are resolved the way a call site resolves them, and 11.3改's
// self^-last op^ is handed its receiver in the slot it asked for. A member
// that takes no self^ is a static one and is simply called, receiver unused.
//
// The callee may end up a plain L^ subroutine or a member the host itself
// registered in C (LHAT_OBJECT_HOST) -- 05 の 8.8's hostdata members are all
// the second kind, so `packed.at(3)` works from here the way it does from a
// compiled call. A built-in (LHAT_OBJECT_NATIVE) still answers
// LHAT_RUN_NOT_CALLABLE -- a coroutine is driven with lhat_machine_resume
// below instead. A receiver with no members to reach answers
// LHAT_RUN_TYPE_ERROR, and a name that reaches nothing answers
// LHAT_RUN_NOT_CALLABLE -- 11.3 makes an absent key nil^, and nil^ is not
// callable.
LhatRunResult lhat_machine_call_member(LhatMachine *machine,
                                       LhatValue receiver, const char *name,
                                       size_t length,
                                       const LhatValue *arguments,
                                       size_t count);

// ---------------------------------------------------------------------------
// Coroutines at the host boundary (05 の 8.8)
// ---------------------------------------------------------------------------
//
// The three calls that put a host on the same footing as a compiled body:
// make a coroutine whose body is C, drive any coroutine from C, and ask
// whether one is finished. Lua's lua_newthread / lua_resume / lua_status,
// re-cut for a machine with one stack: a resumed body runs on the machine's
// own frames, so these nest anywhere lhat_machine_call does.

// A coroutine whose body is `step`, called once per resume -- what a host's
// iterate^ answers so that `for^ x in^ value` can walk something the host
// holds. No frame, no registers: the walk's state is `context`, which
// `release` (may be NULL) is handed back when the walk ends or is collected,
// under the dispose^ contract -- once, and never reaching back into the L^
// API, since the sweep may be the caller. It is a hook and not a call, so
// it is handed no room to answer into: `answers` and `answer_count` reach
// it NULL, the way a dispose^ does (object.h). `held` is one value kept reachable
// for the walk's sake -- the hostdata being walked, usually -- and nil^ when
// nothing needs holding. `step` writes its answers the way any registration
// does -- a `for^ k, v` walk hands over two of them -- and answers true
// while the walk goes on, false when it is over. 13.9: what it writes
// with false is T, the value a body coroutine ends with by writing
// return^; writing nothing there is a walk that ends with nothing.
//
// False from THIS call only when a pointer is missing or memory is.
bool lhat_machine_make_coroutine(LhatMachine *machine, LhatHostStepFn step,
                                 void *context, LhatHostFn release,
                                 LhatValue held, LhatValue *out);

// One resume, from C -- lua_resume's shape, so resume subsumes start: a
// fresh body runs from the top, and that first send is discarded (there is
// no yield^ awaiting it yet; none is the idiom). On a suspended body the
// send arrives where the yield^ waits, exactly as resume(...) sends it:
// `sent`/`sent_count` carry none, one, or 13.8改's several -- as many as
// the body's R takes, a tuple R being taken apart by the yield^'s own
// binding. NULL with 0 sends nothing.
//
// The answer is what the body yielded or returned -- 13.9's union(Y, T),
// told apart with lhat_machine_coroutine_done below, the same line 15.6改
// draws. A tuple crosses as `positions`, as any tuple crosses this
// boundary. Resuming a finished or currently-running coroutine answers
// LHAT_RUN_DEAD_COROUTINE. A walk with no body -- a table's, or
// lhat_machine_make_coroutine's above -- takes one step, no frame entered
// (a host step reads sent[0], or nil^ when nothing was sent).
LhatRunResult lhat_machine_resume(LhatMachine *machine, LhatValue coroutine,
                                  const LhatValue *sent, size_t sent_count);

// 15.6改's done(), askable from C: whether the body has run to its end.
// False for any value that is not a coroutine.
bool lhat_machine_coroutine_done(LhatValue coroutine);

const char *lhat_run_status_message(LhatRunStatus status);

// 04 の 11.6改: one frame of a traceback. `source` is the unit's path and
// `name` the binding or member the body was written under -- both debug
// labels stamped at compile time (02 の 14.9 stands: neither is part of
// what a proto is), and either may be NULL (a session's input, a bare f^).
// `line` stays at 11.6's file:line granularity; 0 when unknown.
typedef struct {
    const char *source;
    const char *name;
    uint32_t line;
    bool top_level;   // the unit's own top-level frame
    bool coroutine;   // a coroutine's body
    bool disposing;   // running 10.7's finally^ cleanups
} LhatFrameInfo;

// The frames a fault left standing -- nothing unwinds on one, so they stay
// readable until the next lhat_run / lhat_machine_call / lhat_machine_resume
// or the machine's disposal, whichever comes first. A host function that
// saw a nested call fault reads them before returning. With no fault
// recorded the walk answers the frames standing right now, which is what a
// host asking "where am I" wants (std.debug.traceback rides this).
//
// `level` counts from the innermost frame (0 = where it stopped).
size_t lhat_machine_fault_depth(const LhatMachine *machine);
bool lhat_machine_fault_frame(const LhatMachine *machine, size_t level,
                              LhatFrameInfo *out);

// The same frames written out as text, one line each, innermost first:
//
//   traceback:
//     main.lh:12: in gen
//     main.lh:40: at the top level
//
// Fills up to `capacity` bytes (NUL-terminated when capacity > 0) and
// answers the length the whole text needs, lhat_value_text's convention.
// Empty (0) when there is nothing to trace.
size_t lhat_machine_traceback(const LhatMachine *machine, char *out,
                              size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_VM_H
