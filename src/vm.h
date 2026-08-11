// L^ (lhat) -- the machine: running compiled bytecode.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// This is the loop that follows 1.1's fourth stage (compile.h). 5.1 has
// every instruction check what it is given, so the machine does not rely on
// the checker having run -- which is also what makes relaxed (3.5) work
// without a second machine. Nothing here reaches the front end: a unit
// already compiled to bytecode needs no lexer, no parser and no checker
// (02 の 14.17改2), and this header is what keeps that true at the include
// level.

#ifndef LHAT_VM_H
#define LHAT_VM_H

#include <stdbool.h>
#include <stddef.h>

#include "lhat/module.h"
#include "object.h"
#include "value.h"

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

// 05 の 5.3: the units a require^ in what runs next may reach. The caller
// keeps them -- a program owns its units and outlives the runs of them.
void lhat_machine_set_modules(LhatMachine *machine, const LhatModule *modules,
                              size_t count);

// 05 の 8.7: the three steps a host registration takes at run time. Kept here
// rather than in program.c because only this file may touch the heap -- the
// values belong to the machine and the collector has to see them.
bool lhat_machine_make_table(LhatMachine *machine, LhatValue *out);

// 13.7: `has_variadic` makes `parameters` a floor rather than an exact count,
// so the registration's signature ended in '...' and a call may write more.
// The extra arguments reach LhatHostFn as the tail of `arguments`, uncollected
// -- there is nothing to collect them into that a C function would want.
bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters,
                            bool has_variadic, bool takes_self,
                            LhatValue *out);

// Puts `value` at L^.modules.<module>[.<type>].<name>, making the tables on
// the way the way 02 の 8.8 does. `type` is NULL for a member of the module
// itself.
bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name,
                           LhatValue value);

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

// The modules `machine` was given by lhat_machine_set_modules, borrowed back
// out. A proto is shared, unwritten data once compiled (same comment as
// above), so handing the same array to a second machine -- another OS thread
// -- is safe without copying anything.
void lhat_machine_modules(const LhatMachine *machine,
                          const LhatModule **out_modules, size_t *out_count);

// Runs `proto` on `machine`. What the run allocates belongs to the machine,
// so the answer is good until the machine is disposed or run again -- there
// is nothing in the result to free.
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);

// Calls an ordinary L^ value the way an instruction would, without a proto of
// its own to run first -- for a host that already holds a callable value
// (05 の 8.7's `LhatHostFn` has no way to call back into L^ on its own, since
// a call site is normally compiled). `callee` has to be a plain subroutine
// (LHAT_OBJECT_SUBROUTINE, not a member, not an overload^ group); arity is
// checked the same way a CALL instruction checks it, variadics collect the
// same way, but there is no self^ and no 'expr...' spread -- a host handing
// values over already has them as a flat array.
//
// May be called on a fresh machine (nothing run on it yet) or nested inside
// a running one -- what a call already on the stack looks like from here is
// only how many frames are already open, which the machine already knows.
LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count);

const char *lhat_run_status_message(LhatRunStatus status);

#endif  // LHAT_VM_H
