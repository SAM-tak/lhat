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

// Compiles one unit into a proto, which owns the bodies written inside it.
// The lexer has to be the one the tree came from, since names and strings are
// spans into it. The caller frees the proto with lhat_proto_free().
LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out);

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
    LHAT_RUN_DIVIDE_BY_ZERO,  // 04 の 11.2: only // and % can reach this
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
    LHAT_RUN_COROUTINE_ALREADY_STARTED  // 15.2改: started one that already
                                         // has been
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

// Runs `proto` on `machine`. What the run allocates belongs to the machine,
// so the answer is good until the machine is disposed or run again -- there
// is nothing in the result to free.
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);

const char *lhat_run_status_message(LhatRunStatus status);

#endif  // LHAT_VM_H
