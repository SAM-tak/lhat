// L^ (lhat) -- compiling a tree to bytecode.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed. This is the fourth stage of 1.1; running what it emits is vm.h,
// which needs nothing from the front end -- keeping the two headers apart is
// what lets a bytecode-only build carry no lexer, no parser and no checker
// (02 の 14.17改2).

#ifndef LHAT_COMPILE_H
#define LHAT_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "check.h"
#include "code.h"
#include "hosted.h"
#include "lhat/lexer.h"
#include "lhat/module.h"  // LhatCompileStatus, and what a compile answers

// Shared with tooling: 1/0 when fits^ is folded at compile time, -1 when
// its value must be tested at runtime. Reads the checked operand types.
int lhat_compile_fits_answer(const LhatLexer *lexer, const LhatNode *asked,
                             const struct LhatType *actual);

// 05 の 5 章. The compile-time twin of check.h's LhatRequireResolver: asked
// for the unit at `path`, it answers where that unit sits in what the machine
// will be given, or LHAT_NO_UNIT when there is none. The checker has already
// refused a path that cannot be had, so a miss here means the caller compiled
// without the program that resolved it.
#define LHAT_NO_UNIT SIZE_MAX

// `module_name` is filled with the path 3 章 had that unit declare, the way
// check.h's resolver fills it -- 5.5's short form needs it to know where
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
    // 05 の 5.3's guard and registry write, for a unit that has a module
    // name. Off for a script std.load brought in (5.6): it answers its
    // table to whoever called it and enters no registry.
    bool registers;

    // Host registration metadata retained with the session configuration.
    // Error construction uses the checked type's linked identity, not a
    // second lookup through this table. NULL/0 when none were registered.
    const LhatHostErrorKind *host_errors;
    size_t host_error_count;

    // What the program's lhat_register_hostdata_type calls registered, so
    // that fits^ against a hostdata type (05 の 8.8) can be compiled. NULL/0
    // when the host registered none.
    const LhatHostTypeEntry *host_types;
    size_t host_type_count;

    // 05 の 8.9: the host value types, the same way. NULL/0 when the host
    // registered none.
    const LhatHostValueTypeEntry *hostvalue_types;
    size_t hostvalue_type_count;
} LhatUnits;

// Compiles one unit into a proto, which owns the bodies written inside it.
// Requires a completed semantic result whose arena remains alive. Frontends
// decide whether diagnostics permit execution before calling this low-level
// emitter; VM tests may also emit analyzed programs with expected failures.
// The lexer has to be the one the tree came from, since names and strings are
// spans into it. The caller frees the proto with lhat_proto_free().
LhatCompileResult lhat_compile(const LhatCheckResult *checked, const LhatLexer *lexer,
                               LhatProto **out);

// The same, as one unit of a program: `units` says where a require^ inside it
// leads, and what path this unit registers itself under. Passing NULL is
// lhat_compile.
LhatCompileResult lhat_compile_module(const LhatCheckResult *checked,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out);

// 03 の 4.3: a REPL compiles many inputs into one running machine, so the
// top-level names of one input have to still be there for the next. The
// session carries them -- their slots, and the names themselves, copied,
// since each input's lexer goes when the input does.
typedef struct LhatCompileSession LhatCompileSession;

LhatCompileSession *lhat_compile_session_new(void);
void lhat_compile_session_dispose(LhatCompileSession *session);

// 04 の 12.4 and 05 の 8.8: the other half of what LhatUnits carries for a
// file -- the error kinds and hostdata types a host registered, so that fits^
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

// 09 の 3.5: tells the session a name that already holds a register, the way
// an earlier input's top-level let^ would have. A debugger's evaluation is
// the caller: it copies a stopped frame's values into a fresh frame's first
// registers and seeds their names here, then compiles one input against
// them. False when the session is full or out of memory.
bool lhat_compile_session_seed(LhatCompileSession *session, const char *name,
                               size_t length, uint8_t reg, const LhatNode *declaration);

// Compiles `unit` as the next input of `session`. The top-level names already
// in it are in scope, and the ones this input declares stay for the next.
//
// The proto answers where the machine has to leave the stack alone, so a run
// of it belongs to the machine the earlier inputs ran on and no other.
LhatCompileResult lhat_compile_next(LhatCompileSession *session,
                                    const LhatCheckResult *checked,
                                    const LhatLexer *lexer, LhatProto **out);

#endif  // LHAT_COMPILE_H
