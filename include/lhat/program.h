// L^ (lhat) -- a program: one unit and everything it requires.
//
// Section numbers refer to DesignDocuments/05-modules.md unless prefixed.
//
// 6.2 adds the unit graph to the staging of 03 の 1.1: a unit cannot be type
// checked until the units it requires have been, so this walks the graph in
// dependency order. It also holds everything alive -- the sources, the trees
// and one shared type arena -- because 6 章 has a unit keep pointing at the
// types its imports published.
//
// A program and its units are opaque here: 8.7's contract is register,
// check, compile, run and read reports. Reading reports walks the units --
// 6.2 makes the graph the thing to show -- but never into one: what a stage
// built stays the library's own, and its diagnostics come out as positions
// and text. The library's own tools read the results themselves, through
// program_internal.h; a host never needs to.

#ifndef LHAT_PROGRAM_H
#define LHAT_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/module.h"  // what a compile answers with, and why one stopped
#include "lhat/object.h"  // 05 の 8.7: LhatHostFn
#include "lhat/source.h"  // what a diagnostic's position indexes
#include "lhat/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LhatProgram LhatProgram;
typedef struct LhatUnit LhatUnit;

typedef enum {
    LHAT_PROGRAM_ERR_CANNOT_READ,  // no such unit
    LHAT_PROGRAM_ERR_CYCLE         // 6.3
} LhatProgramErrorCode;

typedef struct {
    LhatProgramErrorCode code;
    char *path;  // owned by the program
} LhatProgramDiagnostic;

typedef enum {
    LHAT_UNIT_CHECKING,  // 6.3: meeting it again while in this state is a cycle
    LHAT_UNIT_DONE,
    LHAT_UNIT_FAILED
} LhatUnitState;

// How a unit's text is obtained. Returns NULL when there is no such unit,
// and otherwise a buffer the program frees.
typedef char *(*LhatProgramLoader)(void *context, const char *path,
                                   size_t *length);

// 05 の 8.9: the loader is handed over rather than defaulted to, so nothing
// embedded reaches a file system without having been told to. `load` may be
// NULL, and then no unit can be read -- which is what a host wants when the
// only units are ones it hands over itself.
//
// port.h's lhat_load_file is written in this shape, for a host that does want
// the file system. NULL when out of memory.
LhatProgram *lhat_program_new(bool strict, LhatProgramLoader load,
                              void *context);
void lhat_program_free(LhatProgram *program);

// Checks the unit at `path` and, first, everything it requires. `path` is
// taken as written; a require^ inside a unit is relative to that unit (5.1).
// Returns NULL when the unit itself could not be read.
const LhatUnit *lhat_program_check(LhatProgram *program, const char *path);

// 05 の 5.3: compiles every unit checked so far and answers the array a
// machine is given with lhat_machine_set_modules. The program owns it; a
// second call answers the same one. NULL when a unit would not compile.
//
// The unit to run is the one lhat_program_check returned: its index says
// which proto of the array is it.
const LhatModule *lhat_program_compile(LhatProgram *program, size_t *count);

// Why that answered NULL. LHAT_COMPILE_OK until a compile has actually
// failed, so a caller with a diagnostic to write asks this rather than
// having to say "something".
LhatCompileStatus lhat_program_compile_status(const LhatProgram *program);

// The same failure with where it was. The position indexes the source of the
// unit that would not compile, which `path` names -- pass NULL for it where
// the name is not wanted. Answers a zeroed result while nothing has failed.
LhatCompileResult lhat_program_compile_failure(const LhatProgram *program,
                                               const char **path);

// ---------------------------------------------------------------------------
// What a unit answers
// ---------------------------------------------------------------------------

// Where the unit sits in what lhat_program_compile answered.
size_t lhat_unit_index(const LhatUnit *unit);

const char *lhat_unit_path(const LhatUnit *unit);
LhatUnitState lhat_unit_state(const LhatUnit *unit);

// Whether the unit read, parsed and checked without one diagnostic from any
// stage -- the whole of what a host asks before running it.
bool lhat_unit_ok(const LhatUnit *unit);

// The program's own diagnostics (a unit that could not be read, a cycle),
// as against what a unit's stages reported, which is below.
size_t lhat_program_diagnostic_count(const LhatProgram *program);
const LhatProgramDiagnostic *lhat_program_diagnostic(const LhatProgram *program,
                                                     size_t index);

// ---------------------------------------------------------------------------
// What the stages reported
// ---------------------------------------------------------------------------
//
// 03 の 1.1 gives each stage its own codes and each unit its own results, and
// 6.2 makes the graph the thing to show rather than one file -- a unit fails
// on what one of its imports says as readily as on its own text. So this is
// a walk over the units and a question to each, not one flat list keyed to
// the path that was asked for.

// The units the graph reached, in the order they were reached:
//
//     for (const LhatUnit *u = lhat_program_units(program); u != NULL;
//          u = lhat_unit_next(u)) { ... }
const LhatUnit *lhat_program_units(const LhatProgram *program);
const LhatUnit *lhat_unit_next(const LhatUnit *unit);

// What a unit's positions index, or NULL when it was never read.
const LhatSource *lhat_unit_source(const LhatUnit *unit);

// Which stage spoke. The codes stay each stage's own; what is shared is
// where a diagnostic is and how it is written out.
typedef enum {
    LHAT_STAGE_LEXER,
    LHAT_STAGE_PARSER,
    LHAT_STAGE_CHECKER
} LhatStage;

// One thing one stage had to say about one place. The message is not here:
// the parser's can name the token it wanted and the checker's the name it
// could not find, so neither is a literal to borrow -- it is asked for
// separately, into a buffer.
typedef struct {
    LhatStage stage;
    uint32_t offset;
    uint32_t line;
    uint32_t column;
    uint32_t length;  // the span, and zero where the stage points at a place
} LhatUnitDiagnostic;

// All three stages' together, in the order they ran. Zero for a unit that was
// never read -- what stopped that is one of the program's own diagnostics.
size_t lhat_unit_diagnostic_count(const LhatUnit *unit);

// A zeroed diagnostic when there is no such one.
LhatUnitDiagnostic lhat_unit_diagnostic(const LhatUnit *unit, size_t index);

// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it. So measuring
// is a call with (NULL, 0).
size_t lhat_unit_diagnostic_message(const LhatUnit *unit, size_t index,
                                    char *out, size_t capacity);

// The whole line a driver writes: the message, the unit it was in and where
// -- and with `rich`, the line it happened on and a caret beneath. The same
// convention, and what a host wants unless it is placing the position
// itself, as a language server does.
size_t lhat_unit_diagnostic_write(const LhatUnit *unit, size_t index,
                                  bool rich, char *out, size_t capacity);


// ---------------------------------------------------------------------------
// 02 の 18: what a unit wrote as annotations
// ---------------------------------------------------------------------------
//
// 18.1 makes an annotation information for the host, so this is the one place
// the language hands something back rather than being told it. What a
// registration meant is the host's to know; what is here is what was written.

// Which of 18.3's four an argument is. A name is not resolved -- it arrives
// as its spelling, and what it means is the host's to decide.
typedef enum {
    LHAT_ANNOTATION_ARG_NUMBER,
    LHAT_ANNOTATION_ARG_STRING,
    LHAT_ANNOTATION_ARG_NAME,
    LHAT_ANNOTATION_ARG_BOOL
} LhatAnnotationArgumentKind;

typedef struct {
    LhatAnnotationArgumentKind kind;
    double number;   // NUMBER, with 18.3's leading '-' already applied
    bool boolean;    // BOOL
    // STRING and NAME. Borrowed from the unit, which has to outlive it.
    const char *text;
    size_t length;
} LhatAnnotationArgument;

typedef struct {
    const char *name;  // borrowed; the spelling without the '@'
    size_t name_length;
    size_t argument_count;
    // The node it was read from and the unit it belongs to, opaque here --
    // what makes an argument reachable without repeating the keys that found
    // the annotation, and what its spans index.
    const void *written;
    const void *unit;
} LhatAnnotation;

// What was written above one thing. `definition` names a def^ a top-level
// binding holds and `name` a member or field of it; either may be NULL:
//
//   (NULL, NULL)   the unit itself, written at its head
//   (NULL, "x")    the top-level binding x
//   ("D", NULL)    the binding that holds the definition D
//   ("D", "hp")    the member or field hp of D
size_t lhat_unit_annotation_count(const LhatUnit *unit, const char *definition,
                                  const char *name);
LhatAnnotation lhat_unit_annotation(const LhatUnit *unit,
                                    const char *definition, const char *name,
                                    size_t index);

// A zeroed argument when there is no such one.
LhatAnnotationArgument lhat_annotation_argument(LhatAnnotation annotation,
                                                size_t at);

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

// 05 の 8.9: a host-defined value type -- `size` bytes the machine keeps in
// consecutive stack slots rather than behind a pointer. Nominal like a
// hostdata type, and additionally barred by the checker from every place
// that outlives a frame (a table, a capture, an any^): boxing into a
// hostdata container is the library's job, not the language's. NULL when
// the name is taken, the size is zero, or there is no memory. The tag
// belongs to the program and lives as long as it does.
const LhatHostValueTag *lhat_register_hostvalue_type(LhatProgram *program,
                                                     const char *module,
                                                     const char *name,
                                                     size_t size);

// A member of a host value type: an operator ("+", "-", ...) or a method.
// `signature` names the value type by its full written path
// ("std.math.LVector3"); a first parameter of self^ is the receiver (14.4).
bool lhat_register_hostvalue_member(LhatProgram *program, const char *module,
                                    const char *type, const char *name,
                                    const char *signature, LhatHostFn call,
                                    void *context);

// One field read and written directly -- 'v.x' compiles to bytes at `offset`
// without a host call, converted with number^ by `kind`. The field also
// appears to the checker as a number^ member. False when the type is not a
// registered host value type, the field name is taken, or offset+size of
// `kind` overruns the registered size.
bool lhat_register_hostvalue_field(LhatProgram *program, const char *module,
                                   const char *type, const char *field,
                                   size_t offset, LhatHostValueFieldKind kind);

// 02 の 18: an annotation the host understands. A program may write no
// others -- 18.5 makes an unregistered name an error rather than something
// carried in silence, since a misspelling that vanishes costs more than it
// saves.
//
// `signature` is written in 13 章's grammar as a p^ -- the arguments are
// what it takes and there is nothing for it to answer. NULL asks nothing of
// them. 18.3 keeps an argument a literal, and a bare name arrives as a
// string^: what the name means is the host's to decide.
//
// Belongs before lhat_program_check, with every other registration (8.7).
bool lhat_register_annotation(LhatProgram *program, const char *module,
                              const char *name, uint32_t targets,
                              const char *signature);

// A subroutine of the module itself.
bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context);

// 04 の 2.2/12.4 の host 版: an errordef^ the host declares in C rather than
// in an L^ unit -- "L^ has its own errordef^-shaped things without writing
// errordef^" (04 の 12.4 が撤回しなかった経路)。v1 は付随フィールドを持たな
// い variant のみ対応(std.io の IOError{NotFound,Denied,Eof} のようなもの)。
// module/name/variant_names の文字列は program と同じだけ生きねばならない
// (register_func 同様、通常は文字列リテラルを渡す)。
//
// lhat_program_check より前に呼ぶこと(8.7 と同じ制約 -- 検査器が型を知る
//必要がある)。out_group/out_variants はどちらも NULL でよく、後から
// lhat_lookup_error_kind で引ける。out_variants は variant_count 個分の
// 領域を呼び出し側が用意する。
bool lhat_register_error_kind(LhatProgram *program, const char *module,
                              const char *name,
                              const char *const *variant_names,
                              size_t variant_count,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants);

// 登録済みの誤り種別を module・name・variant の完全一致で引く。variant が
// NULL なら宣言全体(lhat_register_error_kind の out_group 相当)を返す。
// 見つからなければ NULL。
// 05 の 8.7: the `context` a registration was handed, found by what it was
// registered under -- `type` is NULL for a member of the module itself. What
// a library's own C needs when something outside a call has to reach the
// state it registered: stdlib/async.c's completions are pushed by a host
// holding nothing but the program.
//
// NULL when no such registration was made. The pointer belongs to whoever
// registered it, and this only hands it back.
void *lhat_lookup_host_context(const LhatProgram *program, const char *module,
                               const char *type, const char *name);

const LhatErrorKind *lhat_lookup_error_kind(const LhatProgram *program,
                                            const char *module,
                                            const char *name,
                                            const char *variant);

// 05 の 8.6: a member of L^ itself rather than of a module under it. Reserved
// for what a program cannot provide for itself -- 8.6 keeps that list short,
// and 8.1 sends everything else through require^.
bool lhat_register_global(LhatProgram *program, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context);

// 05 の 8.2: bind `name` to a member of L^, so that a program may write it
// without any qualification. `member` is spelled "L^.<member>" -- the only
// form for now, since 8.6's table is where a host puts what it wants seen
// without a require^.
//
// 8.1 is unaffected. The language hands out no names; a host that binds none
// leaves a program seeing nothing. A let^ of the same spelling shadows the
// binding, and L^.<member> still reaches what it named.
bool lhat_bind_initial(LhatProgram *program, const char *name,
                       const char *member);

// Puts what was registered into the machine's L^.modules, so that an import^
// finds it. Belongs after lhat_machine_set_modules and before the run.
bool lhat_program_install(const LhatProgram *program, LhatMachine *machine);

// 03 の 4.3: the same for a prompt, which has no program of its own driving
// it. A host that wants import^ to answer there makes an LhatProgram with no
// loader -- 5.3 gives a require^ at a prompt nowhere to go anyway -- registers
// into it as it would for a file, and hands it to all three of the prompt's
// pieces: the check session, the compile session and the machine.
//
//     LhatProgram *program = lhat_program_new(false, NULL, NULL);
//     lhatstdlib_io_register(program);           // or any registration
//     lhat_program_install_checks(program, checks);
//     lhat_program_install_compiles(program, compiles);
//     lhat_program_install(program, machine);
//
// What each hands over is what LhatRequire and LhatUnits carry for a file:
// the registry import^ resolves against and 8.6's L^ members for the checker,
// the error kinds and hostdata types isa^ names for the compiler. Both borrow
// from the program, so it has to outlive the sessions.
typedef struct LhatCheckSession LhatCheckSession;
typedef struct LhatCompileSession LhatCompileSession;
void lhat_program_install_checks(const LhatProgram *program,
                                 LhatCheckSession *session);
void lhat_program_install_compiles(const LhatProgram *program,
                                   LhatCompileSession *session);

// True when any unit, or the program itself, reported something.
bool lhat_program_has_errors(const LhatProgram *program);

const char *lhat_program_error_message(LhatProgramErrorCode code);

// Writes everything the host registered -- the types, the signatures, the
// initial bindings -- as JSON, so a tool that cannot run the host's C can
// still be told what the checker was told. The language server is the
// reader: `lhat --dump-host-api > lhat-host.json` at a workspace root gives
// it the same registrations this program carries, minus the callbacks,
// which no text can carry -- a reader that only checks (never runs) does
// not miss them.
//
// Two arrays, "types" then "functions", so a reader registering in that
// order never meets a signature naming a type it has not seen -- type
// declarations carry no signatures, so they cannot refer to each other and
// their relative order within the array is free. "bindings" last, for 8.2's
// initial names.
//
// Follows lhat_report_write: answers how many bytes the whole thing wants,
// not counting the terminating NUL, and fills up to `capacity` including
// it. So measuring is a call with (NULL, 0).
size_t lhat_program_dump_host_api(const LhatProgram *program, char *out,
                                  size_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_PROGRAM_H
