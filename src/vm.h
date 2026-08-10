// L^ (lhat) -- compiling a tree to bytecode, and running it.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// This is the fourth stage of 1.1 and the loop that follows it. 5.1 has every
// instruction check what it is given, so the machine does not rely on the
// checker having run -- which is also what makes relaxed (3.5) work without a
// second machine.

#ifndef LHAT_VM_H
#define LHAT_VM_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "code.h"
#include "lexer.h"
#include "object.h"
#include "value.h"

typedef enum {
    LHAT_COMPILE_OK,
    LHAT_COMPILE_UNSUPPORTED,   // a form the compiler does not cover yet
    LHAT_COMPILE_TOO_COMPLEX,   // out of registers or constants (5.2)
    LHAT_COMPILE_UNDEFINED,     // a name with no binding
    // 02 の 9.8: a break^ naming more loops than stand around it, or one
    // written outside any. Not the same as UNSUPPORTED above -- nothing is
    // waiting to be implemented here, the count is simply wrong.
    LHAT_COMPILE_BREAK_TOO_FAR,
    // 01 の 8 章: a '$^' counting more scopes out than are open here. The
    // same kind of miscount as BREAK_TOO_FAR, and not a form still waiting
    // on the compiler.
    LHAT_COMPILE_SCOPE_TOO_FAR
} LhatCompileStatus;

// 05 の 5 章. The compile-time twin of check.h's LhatRequireResolver: asked
// for the unit at `path`, it answers where that unit sits in what the machine
// will be given, or LHAT_NO_UNIT when there is none. The checker has already
// refused a path that cannot be had, so a miss here means the caller compiled
// without the program that resolved it.
#define LHAT_NO_UNIT SIZE_MAX

// `module_name` is filled with the path 3 章 had that unit declare, the way
// check.h's resolver fills it -- 5.4改's short form needs it to know where
// the unit it brought in goes.
typedef size_t (*LhatUnitResolver)(void *context, const char *path,
                                   size_t length, const char **module_name);

// 05 の 8.7 の誤り版: one error kind lhat_register_error_kind (program.h)
// registered. resolve_kind (vm.c) reads these to answer a qualified name an
// import^ed module wrote -- "module...Name" for the declaration as a whole,
// "module...Name.Variant" for one of its kinds. The strings and the kind
// objects are all owned by the LhatProgram that registered them (its
// host_error_heap, 04 の 12.4), and outlive every machine and every compile
// that reads this.
typedef struct LhatHostErrorKind {
    const char *module;                    // "." 区切り可; declared by
    const char *name;                      // hosted_table 参照
    const LhatErrorKind *group;            // 宣言全体。isa^ module.Name 用
    const char *const *variant_names;
    const LhatErrorKind *const *variants;  // variant_names と対応
    size_t variant_count;
} LhatHostErrorKind;

// 05 の 8.8 の isa^ 版: one hostdata type lhat_register_hostdata_type
// (program.h) registered. resolve_isa_type (vm.c) reads these to answer a
// qualified name an import^ed module wrote -- "module...Name" -- against
// the tag 8.8's identity rule compares by. `tag` is owned by the
// LhatProgram that registered it and outlives every machine and every
// compile that reads this, the same as LhatHostErrorKind's objects.
typedef struct LhatHostTypeEntry {
    const char *module;
    const char *name;
    const LhatHostDataTag *tag;
} LhatHostTypeEntry;

// 05 の 8.9: the same for one host value type lhat_register_hostvalue_type
// (program.h) registered. The compiler reads these to put a tag -- and with
// it a width -- behind a written "module...Name", both for a type position
// and for isa^. Owned by the registering LhatProgram, as above.
typedef struct LhatHostValueTypeEntry {
    const char *module;
    const char *name;
    const LhatHostValueTag *tag;
} LhatHostValueTypeEntry;

typedef struct {
    LhatUnitResolver resolve;
    void *context;
    // 05 の 3 章: the path this unit declared, or NULL when it declared none
    // (3.2). A unit that has one registers itself under it and answers what
    // an earlier require^ registered, which is how 5.3 loads it once.
    const char *module_name;

    // 05 の 8.2: the names the host bound before anything ran, each to a
    // member of L^ (8.6). A name no scope holds is one of these, and compiles
    // to reading that member -- so nothing new exists at run time. 8.1 stays
    // as it was: the language hands out no names, the host does, and a host
    // that binds none leaves the program seeing nothing.
    //
    // Two arrays rather than one of pairs, so that neither this header nor
    // check.h has to know a type the other declares.
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // What the program's lhat_register_error_kind calls registered, so that
    // resolve_kind can answer a name none of this unit's own errordef^s
    // declared. NULL/0 when the host registered none.
    const LhatHostErrorKind *host_errors;
    size_t host_error_count;

    // What the program's lhat_register_hostdata_type calls registered, so
    // that isa^ against a hostdata type (05 の 8.8) can be compiled. NULL/0
    // when the host registered none.
    const LhatHostTypeEntry *host_types;
    size_t host_type_count;

    // 05 の 8.9: the host value types, the same way. NULL/0 when the host
    // registered none.
    const LhatHostValueTypeEntry *hostvalue_types;
    size_t hostvalue_type_count;
} LhatUnits;

// Compiles one unit into a proto, which owns the bodies written inside it.
// The lexer has to be the one the tree came from, since names and strings are
// spans into it. The caller frees the proto with lhat_proto_free().
LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out);

// The same, as one unit of a program: `units` says where a require^ inside it
// leads, and what path this unit registers itself under. Passing NULL is
// lhat_compile.
LhatCompileStatus lhat_compile_module(const LhatNode *unit,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out);

// 03 の 4.3: a REPL compiles many inputs into one running machine, so the
// top-level names of one input have to still be there for the next. The
// session carries them -- their slots, and the names themselves, copied,
// since each input's lexer goes when the input does.
typedef struct LhatCompileSession LhatCompileSession;

LhatCompileSession *lhat_compile_session_new(void);
void lhat_compile_session_dispose(LhatCompileSession *session);

// 05 の 8.2: the names the host bound to members of L^, which a bare name
// falls back on. The same two arrays LhatUnits carries for a file; a prompt
// has no program to hold them, so the session does. They belong to the caller
// and have to outlive it.
void lhat_compile_session_bind(LhatCompileSession *session,
                               const char *const *names,
                               const char *const *members, size_t count);

// 04 の 12.4 and 05 の 8.8: the other half of what LhatUnits carries for a
// file -- the error kinds and hostdata types a host registered, so that isa^
// against either compiles at a prompt too. Both arrays belong to whatever
// registered them (an LhatProgram, normally) and have to outlive the session.
// program.h's lhat_program_install_compiles is this call written out.
//
// import^ itself needs nothing here: it compiles to reading L^.modules, which
// is filled by lhat_program_install once the machine exists.
void lhat_compile_session_hosted(LhatCompileSession *session,
                                 const LhatHostErrorKind *errors,
                                 size_t error_count,
                                 const LhatHostTypeEntry *types,
                                 size_t type_count);

// Compiles `unit` as the next input of `session`. The top-level names already
// in it are in scope, and the ones this input declares stay for the next.
//
// The proto answers where the machine has to leave the stack alone, so a run
// of it belongs to the machine the earlier inputs ran on and no other.
LhatCompileStatus lhat_compile_next(LhatCompileSession *session,
                                    const LhatNode *unit,
                                    const LhatLexer *lexer, LhatProto **out);

const char *lhat_compile_status_message(LhatCompileStatus status);

typedef enum {
    LHAT_RUN_OK,
    LHAT_RUN_TYPE_ERROR,      // 5.1: an instruction was given the wrong thing
    LHAT_RUN_NOT_CALLABLE,    // called something that is not a subroutine
    LHAT_RUN_ARITY,           // 5.3: the wrong number of arguments
    LHAT_RUN_STACK_OVERFLOW,  // the frames went too deep
    LHAT_RUN_OUT_OF_MEMORY,
    LHAT_RUN_BAD_KEY,         // 04 の 11.3: nil^ means "not there", so it
                              // cannot also be a key
    LHAT_RUN_UNPACK_SHORT,    // 02 の 13.10: a destructuring bind reached a
                              // position the value does not have. Under
                              // strict the checker says so first; this is
                              // where relaxed lands
    LHAT_RUN_BAD_FORMAT,      // 02 の 14.17: tostring was handed a format a
                              // number^ cannot be written through
    LHAT_RUN_DEAD_COROUTINE,  // resumed one that has finished
    LHAT_RUN_YIELD_OUTSIDE,   // 02 の 15.2 and 10.7: yield^ where nothing is
                              // waiting to be resumed
    LHAT_RUN_NO_CANDIDATE,    // 02 の 14.12: no signature of an overloaded
                              // member takes what the call handed over
    LHAT_RUN_COROUTINE_NOT_STARTED,     // 15.2改: resumed one that has never
                                         // been started
    LHAT_RUN_COROUTINE_ALREADY_STARTED, // 15.2改: started one that already
                                         // has been
    LHAT_RUN_SEALED,          // 05 の 8.6改 (M5): wrote into a table the
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
    LHAT_RUN_PANIC            // 04 の 11.6改: panic^, written by the program
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

// 05 の 8.9: gives the machine its per-type members tables, one per
// registered host value type, found back by tag->index. Called by
// lhat_program_install after the entries have landed under L^.modules --
// the tables bound here are those very ones, so the collector reaches them
// through the environment already.
bool lhat_machine_bind_hostvalues(LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count);

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
