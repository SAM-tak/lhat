// L^ (lhat) -- what compiling answers with.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// A host compiles a program (lhat/program.h) and runs what it gets back
// (lhat/vm.h); this is the shape that travels between the two. The body is
// opaque here -- the chunk, the instruction word and the opcodes are the
// machine's own business, and 05 の 8.7 asks a host to name none of them.

#ifndef LHAT_MODULE_H
#define LHAT_MODULE_H

#include <stddef.h>

// One compiled subroutine. src/code.h completes it for the library; nothing
// outside reads through it.
typedef struct LhatProto LhatProto;

void lhat_proto_free(LhatProto *proto);

// 05 の 5.3: one unit, compiled. The path 3 章 had it declare is kept beside
// the body, since that is where the unit registers itself and what a
// diagnostic names it by. NULL when the unit declared none (3.2), and then
// nothing registers it -- 5.5 already refused the short form for it.
typedef struct {
    LhatProto *proto;
    char *module_name;  // owned
} LhatModule;

// Frees the protos and the names. The array itself belongs to the caller.
void lhat_modules_free(LhatModule *modules, size_t count);

// Why a compile stopped. lhat_program_compile answers NULL and leaves the
// reason here (lhat_program_compile_status), since it compiles unit by unit
// and stops at the first that will not -- so what a reader has to be told is
// that one status rather than "something".
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

const char *lhat_compile_status_message(LhatCompileStatus status);

#endif  // LHAT_MODULE_H
