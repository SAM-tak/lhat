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
    LHAT_COMPILE_UNDEFINED      // a name with no binding
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

typedef struct {
    LhatUnitResolver resolve;
    void *context;
    // 05 の 3 章: the path this unit declared, or NULL when it declared none
    // (3.2). A unit that has one registers itself under it and answers what
    // an earlier require^ registered, which is how 5.3 loads it once.
    const char *module_name;
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
    LHAT_RUN_DEAD_COROUTINE,  // resumed one that has finished
    LHAT_RUN_YIELD_OUTSIDE,   // 02 の 15.2 and 10.7: yield^ where nothing is
                              // waiting to be resumed
    LHAT_RUN_NO_CANDIDATE,    // 02 の 14.12: no signature of an overloaded
                              // member takes what the call handed over
    LHAT_RUN_COROUTINE_NOT_STARTED,     // 15.2改: resumed one that has never
                                         // been started
    LHAT_RUN_COROUTINE_ALREADY_STARTED, // 15.2改: started one that already
                                         // has been
    LHAT_RUN_NO_SUCH_UNIT     // 05 の 5.3: a require^ reached for a unit the
                              // machine was not given
} LhatRunStatus;

typedef struct {
    LhatRunStatus status;
    LhatValue value;   // what the unit returned, or nil^
    size_t at;         // the instruction that failed, when one did

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
// and a caller keeps the handle rather than the object.
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
bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters, bool takes_self,
                            LhatValue *out);

// Puts `value` at L^.modules.<module>[.<type>].<name>, making the tables on
// the way the way 02 の 8.8 does. `type` is NULL for a member of the module
// itself.
bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name,
                           LhatValue value);

// 05 の 8.8: a value standing for something the host made. `pointer` is the
// host's and nothing here reads it; `tag` is what a later call checks before
// reading it as the type it expects, and comes from lhat_register_hostdata_type.
//
// Members written against the type answer through the value, so it is the
// registered type that has to have been installed first.
bool lhat_machine_make_hostdata(LhatMachine *machine, const LhatHostDataTag *tag,
                            void *pointer, LhatValue *out);

// The pointer back, or NULL when the value is not one of these or was made
// with a different tag. 7.3 makes the tags distinct per declaration, so this
// is what stops a Texture reaching the C that expects a Sound.
void *lhat_hostdata_pointer(LhatValue value, const LhatHostDataTag *tag);

// Runs `proto` on `machine`. What the run allocates belongs to the machine,
// so the answer is good until the machine is disposed or run again -- there
// is nothing in the result to free.
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);

const char *lhat_run_status_message(LhatRunStatus status);

#endif  // LHAT_VM_H
