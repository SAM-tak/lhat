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
#include "value.h"

typedef enum {
    LHAT_COMPILE_OK,
    LHAT_COMPILE_UNSUPPORTED,   // a form the compiler does not cover yet
    LHAT_COMPILE_TOO_COMPLEX,   // out of registers or constants (5.2)
    LHAT_COMPILE_UNDEFINED      // a name with no binding
} LhatCompileStatus;

// Compiles one unit's statements into `chunk`. The lexer has to be the one
// the tree came from, since names and strings are spans into it.
LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatChunk *chunk);

const char *lhat_compile_status_message(LhatCompileStatus status);

typedef enum {
    LHAT_RUN_OK,
    LHAT_RUN_TYPE_ERROR,     // 5.1: an instruction was given the wrong thing
    LHAT_RUN_DIVIDE_BY_ZERO  // 04 の 11.2: only // and % can reach this
} LhatRunStatus;

typedef struct {
    LhatRunStatus status;
    LhatValue value;   // what the unit returned, or nil^
    size_t at;         // the instruction that failed, when one did
} LhatRunResult;

LhatRunResult lhat_run(const LhatChunk *chunk);

const char *lhat_run_status_message(LhatRunStatus status);

#endif  // LHAT_VM_H
