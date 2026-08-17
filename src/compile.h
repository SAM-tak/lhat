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
#include "code.h"
#include "hosted.h"
#include "lhat/lexer.h"
#include "lhat/module.h"  // LhatCompileStatus, and what a compile answers

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

// 02 の 14.2: a composition is flattened where it is written, so a chain
// naming a definition another unit published needs that unit's tree rather
// than the value it will have. The program already holds it -- checking put
// it there -- and this is how the compiler asks for it.
//
// Answers the unit's top-level statements and the lexer their names are
// spans into. NULL leaves a composition across units uncompilable, which is
// what lhat_compile (no program behind it) gets.
typedef bool (*LhatUnitBodyResolver)(void *context, size_t unit,
                                     const LhatNode **out_statements,
                                     const LhatLexer **out_lexer);

typedef struct {
    LhatUnitResolver resolve;
    LhatUnitBodyResolver body;
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
LhatCompileResult lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out);

// The same, as one unit of a program: `units` says where a require^ inside it
// leads, and what path this unit registers itself under. Passing NULL is
// lhat_compile.
LhatCompileResult lhat_compile_module(const LhatNode *unit,
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
LhatCompileResult lhat_compile_next(LhatCompileSession *session,
                                    const LhatNode *unit,
                                    const LhatLexer *lexer, LhatProto **out);

#endif  // LHAT_COMPILE_H
