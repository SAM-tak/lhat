// L^ (lhat) -- what compiling answers with.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// A host compiles a program (lhat/program.h) and runs what it gets back
// (lhat/vm.h); this is the shape that travels between the two. The body is
// opaque here -- the chunk, the instruction word and the opcodes are the
// machine's own business, and [modules.host.import] asks a host to name none of them.

#ifndef LHAT_MODULE_H
#define LHAT_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One compiled subroutine. src/code.h completes it for the library; nothing
// outside reads through it.
typedef struct LhatProto LhatProto;

void lhat_proto_free(LhatProto *proto);

// The calling shape of a body, for a host holding one and deciding what it
// may do with it -- std.thread's spawn asks all four before it hands a
// closure to another machine. [syntax.sub.yieldable] makes a yieldable body answer a
// coroutine rather than run; [pipeline.bc.capture]'s captured places belong to the
// machine that made them; [syntax.type.variadic]'s variadic makes `parameters` a floor.
bool lhat_proto_yields(const LhatProto *proto);
size_t lhat_proto_upvalue_count(const LhatProto *proto);
size_t lhat_proto_parameters(const LhatProto *proto);
bool lhat_proto_has_variadic(const LhatProto *proto);
// [modules.require.load]: whether a machine owns the body (lhat_machine_adopt_script)
// rather than a program -- then it goes with that machine's heap, and a
// closure of it cannot be handed to another.
bool lhat_proto_is_owned(const LhatProto *proto);

// 09 の D8: the first source line at or after `at_or_after` which has an
// instruction in this body or in a body written inside it. Zero says there
// is none (and instruction lines numbered zero are never candidates). This
// lets a debugger move a source breakpoint from a comment or blank line to
// the next place code can actually run without exposing bytecode itself.
uint32_t lhat_proto_next_instruction_line(const LhatProto *proto,
                                          uint32_t at_or_after);

// Why a compile stopped. lhat_program_compile answers NULL and leaves the
// reason here (lhat_program_compile_status), since it compiles unit by unit
// and stops at the first that will not -- so what a reader has to be told is
// that one status rather than "something".
typedef enum {
    LHAT_COMPILE_OK,
    LHAT_COMPILE_UNSUPPORTED,   // a form the compiler does not cover yet
    LHAT_COMPILE_TOO_COMPLEX,   // out of registers or constants ([l10n.compose.hole-kinds])
    LHAT_COMPILE_UNDEFINED,     // a name with no binding
    // [syntax.loop.exit]: a break^ naming more loops than stand around it, or one
    // written outside any. Not the same as UNSUPPORTED above -- nothing is
    // waiting to be implemented here, the count is simply wrong.
    LHAT_COMPILE_BREAK_TOO_FAR,
    // [lex.scope]: a '$^' counting more scopes out than are open here. The
    // same kind of miscount as BREAK_TOO_FAR, and not a form still waiting
    // on the compiler.
    LHAT_COMPILE_SCOPE_TOO_FAR,
    // [modules.public] と [syntax.obj.chain]: a definition composed out of another unit is
    // flattened where it is written, so what its body names has to be
    // reachable from anywhere -- what that unit published, or an import^
    // root, both of which live under L^.modules. A name of that unit's own
    // top level is a register in a frame this body does not have.
    LHAT_COMPILE_NOT_PUBLISHED,
    LHAT_COMPILE_OUT_OF_MEMORY  // allocation of compiler working storage failed
} LhatCompileStatus;

const char *lhat_compile_status_message(LhatCompileStatus status);
// [l10n.id]: the stable ID of that message, which a host draws its own language's
// text from (lhat_program_text). NULL for a status the table does not hold.
const char *lhat_compile_status_id(LhatCompileStatus status);

// What a compile answers: the status above and where it stopped.
//
// [pipeline.repl.no-fork] puts the refusals in the checker, so a form that reaches the
// compiler and will not compile is a hole in the checker rather than the
// writer's mistake. Saying only which status was reached leaves whoever has
// to close that hole with nothing to look at, which is what this carries.
//
// The fields are the ones LhatCheckDiagnostic carries, and mean the same, so
// a driver renders either the same way. All zero where nothing failed.
typedef struct {
    LhatCompileStatus status;
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // The name it was about, for the statuses that are about one. Borrowed
    // from the source the unit was read from, which outlives the compile,
    // and NULL for the rest.
    const char *name;
    uint32_t name_length;
} LhatCompileResult;

// lhat/program.h writes the message one of these makes: a compile result is
// read in the language of the program the compile was for ([l10n.choose.program]).

#ifdef __cplusplus
}
#endif

#endif  // LHAT_MODULE_H
