// L^ (lhat) -- a program: one unit and everything it requires.
//
// Section numbers refer to DesignDocuments/05-modules.md unless prefixed.
//
// 6.2 adds the unit graph to the staging of 03 の 1.1: a unit cannot be type
// checked until the units it requires have been, so this walks the graph in
// dependency order. It also holds everything alive -- the sources, the trees
// and one shared type arena -- because 6 章 has a unit keep pointing at the
// types its imports published.

#ifndef LHAT_PROGRAM_H
#define LHAT_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>

#include "check.h"
#include "code.h"  // 05 の 5.3: a unit compiles to one of these
#include "lexer.h"
#include "object.h"  // 05 の 8.7: LhatHostFn
#include "parser.h"
#include "source.h"
#include "type.h"

typedef enum {
    LHAT_PROGRAM_ERR_CANNOT_READ,  // no such unit
    LHAT_PROGRAM_ERR_CYCLE         // 6.3
} LhatProgramErrorCode;

typedef struct {
    LhatProgramErrorCode code;
    char *path;  // owned
} LhatProgramDiagnostic;

typedef enum {
    LHAT_UNIT_CHECKING,  // 6.3: meeting it again while in this state is a cycle
    LHAT_UNIT_DONE,
    LHAT_UNIT_FAILED
} LhatUnitState;

typedef struct LhatUnit {
    char *path;  // resolved and normalised; the key 5.3 loads once against
    bool loaded;

    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;

    LhatUnitState state;

    // 05 の 5.3: where this unit sits in what lhat_program_compile built, so
    // a require^ of it names it by number. Only meaningful after that ran.
    size_t index;

    struct LhatUnit *next;
} LhatUnit;

// How a unit's text is obtained. Returns NULL when there is no such unit,
// and otherwise a buffer the program frees.
typedef char *(*LhatProgramLoader)(void *context, const char *path,
                                   size_t *length);

typedef struct {
    // 6 章: shared, so the types one unit publishes stay valid in the units
    // that require it.
    LhatTypeArena types;

    LhatUnit *units;
    bool strict;

    LhatProgramLoader load;
    void *loader_context;

    LhatProgramDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

    // 05 の 5.3: what lhat_program_compile built, owned here.
    LhatModule *modules;
    size_t module_count;

    // 05 の 8.7: what the host registered, as one nested table type keyed by
    // module path -- the same shape L^.modules has, since that is where it
    // ends up. import^ resolves against this and against nothing else, which
    // is what keeps its answer independent of the order units are checked in.
    LhatType *hosted;
    struct LhatHostEntry *host_entries;
    size_t host_entry_count;
    size_t host_entry_capacity;
} LhatProgram;

// Without a loader, units are read from the file system.
void lhat_program_init(LhatProgram *program, bool strict);
void lhat_program_set_loader(LhatProgram *program, LhatProgramLoader load,
                             void *context);
void lhat_program_dispose(LhatProgram *program);

// Checks the unit at `path` and, first, everything it requires. `path` is
// taken as written; a require^ inside a unit is relative to that unit (5.1).
// Returns NULL when the unit itself could not be read.
const LhatUnit *lhat_program_check(LhatProgram *program, const char *path);

// 05 の 5.3: compiles every unit checked so far and answers the array a
// machine is given with lhat_machine_set_modules. The program owns it; a
// second call answers the same one. NULL when a unit would not compile.
//
// The unit to run is the one lhat_program_check returned: its `index` says
// which proto of the array is it.
const LhatModule *lhat_program_compile(LhatProgram *program, size_t *count);

// ---------------------------------------------------------------------------
// 05 の 8.7: what the host provides
// ---------------------------------------------------------------------------
//
// All of these belong before lhat_program_check: the checker has to know what
// a signature says, and import^ has to know what is there. Registering after
// checking is too late and answers false.
//
// A module is made by the first registration under its path. A signature is
// written in the type grammar of 13 章 and may name the builtins and any type
// registered before it -- so a pair that name each other is registered as two
// bare types first, then given their members.

// An opaque type of its own. 05 の 7.3's rule holds for it: identity is the
// declaration, not the shape, so two host types with the same members are
// still different types and a pointer cannot be handed to the wrong C code.
bool lhat_register_type(LhatProgram *program, const char *module,
                        const char *name);

// 05 の 8.8: the same, and answers the tag a value of it carries. Registering
// a `dispose` member on it is what makes it the host's to hand over and L^'s
// to give back -- 02 の 12.5 reads that off the type the way it does for
// anything else. Without one the host keeps the lifetime and L^ holds a
// pointer it never frees.
//
// NULL when the name is taken or there is no memory. The tag belongs to the
// program and lives as long as it does.
const LhatHostDataTag *lhat_register_hostdata_type(LhatProgram *program,
                                                   const char *module,
                                                   const char *name);

// A member of a type registered earlier. `signature` describes it; a p^ or f^
// whose first parameter is written self^ is an instance method (14.4).
bool lhat_register_member(LhatProgram *program, const char *module,
                          const char *type, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context);

// A subroutine of the module itself.
bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context);

// Puts what was registered into the machine's L^.modules, so that an import^
// finds it. Belongs after lhat_machine_set_modules and before the run.
typedef struct LhatMachine LhatMachine;
bool lhat_program_install(const LhatProgram *program, LhatMachine *machine);

// True when any unit, or the program itself, reported something.
bool lhat_program_has_errors(const LhatProgram *program);

const char *lhat_program_error_message(LhatProgramErrorCode code);

#endif  // LHAT_PROGRAM_H
