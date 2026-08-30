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
    LHAT_UNIT_FAILED,
    // 05 の 5.7: lhat_program_invalidate retired what was made of it. The
    // unit is still the program's -- the path it stands for has not gone --
    // but nothing of it is checked or compiled, and the next
    // lhat_program_check reads it again.
    LHAT_UNIT_STALE
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

// 05 の 5.3: compiles every checked unit that is not compiled yet. The
// program owns what it makes, and each unit's proto carries its own table of
// the units its require^s reach -- so a machine is given nothing, and this
// may be called again after a later lhat_program_check, under machines
// already running, to bring the units that check reached in. False when a
// unit would not compile; what did compile is still usable.
//
// The unit to run is the one lhat_program_check returned, through
// lhat_unit_proto.
bool lhat_program_compile(LhatProgram *program);

// Why that answered false. LHAT_COMPILE_OK until a compile has actually
// failed, so a caller with a diagnostic to write asks this rather than
// having to say "something".
LhatCompileStatus lhat_program_compile_status(const LhatProgram *program);

// ---------------------------------------------------------------------------
// 05 の 5.7: a unit changed under the program
// ---------------------------------------------------------------------------

// The text at `path` may have changed. Retires what was checked and compiled
// of that unit and of every unit whose require^s reach it, so that the next
// lhat_program_check reads them all again and lhat_program_compile makes
// them again.
//
// Answers how many units it retired: 0 when the text still reads exactly as
// it did (an editor saving an unchanged buffer costs nothing), and SIZE_MAX
// when the program has no unit at that path. A path that can no longer be
// read is retired rather than kept -- a deleted file has to stop being what
// the program compiled.
//
// The cascade is why this reaches further than the one unit: a requirer's
// checked types come from what the required unit publishes, and a requirer's
// compiled body holds a table of the very protos its require^s answer.
//
// NOTHING IS FREED HERE. A closure made before this call keeps running the
// body it was made from, and the unit table that body reads still names the
// bodies it was compiled against -- the old world stays whole beside the new
// one. lhat_program_discard_retired is where the old one goes.
size_t lhat_program_invalidate(LhatProgram *program, const char *path);

// Frees the code of every retired body. The host says when, because only the
// host knows: no machine may be running one, and nothing on any machine's
// heap may still hold a closure of one. Getting that wrong is a
// use-after-free, which is why this is not done for you.
//
// What a body's constants named is not freed here but kept until the program
// is. Those objects were always meant to outlive every machine, and a machine
// goes on holding them after the body has gone: a string constant is what
// LOADK puts in a register and what the program then stores as a table key,
// and L^.modules is keyed by the very strings a unit's own prologue loads.
// There is no way for a host to know which escaped, so none are freed.
//
// How to know, per machine the program was installed on: drop what held the
// old bodies (in an editor, dress everything in the new ones), then
// lhat_machine_collectgarbage, and then check
// lhat_machine_pending_disposals is zero. The collection is what takes the
// closures nothing holds any more; the second is the one thing a collection
// cannot finish on its own -- 02 の 10.7 keeps a dropped coroutine alive
// until its finally^ has run, and that coroutine holds the closure it was
// suspended in. Free a body under one of those and the cleanup runs freed
// bytecode the next time the machine runs anything.
//
// lhat_program_free does this too, for a host that never calls it.
void lhat_program_discard_retired(LhatProgram *program);

// How many are waiting, so a host can tell whether a pass is worth taking.
size_t lhat_program_retired_count(const LhatProgram *program);

// 05 の 5.7, whole: the one call an editor's save becomes. Invalidates
// `path`, forgets every retired unit's module on every machine handed in,
// rechecks every retired unit (the requirers too -- a shell left staled
// would be a NULL proto to whoever runs it) and recompiles -- and then,
// instead of trusting anyone about
// the old bodies, asks: after a collection on each machine, does any still
// hold a closure or suspended coroutine of a retired body (nested ones
// included), or a cleanup not yet run? Only when none does are the retired
// bodies discarded. A held body is simply kept for the next reload -- the
// same waiting a host that never discards pays, never worse.
//
// `machines` is every machine the program was installed on -- the program
// keeps no list of its own (8.7 has registration done before anything
// runs, and a list written to at machine birth would end that). Answers as
// lhat_program_invalidate does: how many units retired, 0 for text that
// reads as it did, SIZE_MAX for a path no unit sits at. When the reread no
// longer checks or compiles, the old world keeps running, the diagnostics
// say why, and the next save tries again.
size_t lhat_reload(LhatProgram *program, const char *path,
                   LhatMachine *const *machines, size_t machine_count);

// 05 の 5.6: a script brought in while the program runs -- what std.load
// is built on. The text is checked and compiled as a unit of its own and
// then forgotten: what it requires joins the program as any unit does
// (checked, compiled, registered once it first runs), but the script itself
// enters no list and no registry, and a second load of the same text is a
// second compile. The proto answered is the caller's -- lhat_proto_free,
// or lhat_machine_adopt_script to have a machine own it.
//
// A script (no module^) compiles to 'p^...' (3.2) and answers its return^;
// a module^ unit compiles without 5.3's guard and registry write and
// answers its public^ table to whoever calls it -- so loading one twice
// makes two tables, and import^ sees neither.
//
// `name` is what diagnostics call it and what a require^ inside it is
// relative to (5.1). `_file` reads through the program's loader
// (lhat_program_new), and answers CANNOT_READ when there is none or it has
// nothing for the path. REJECTED leaves what the checker or the compiler
// said in lhat_program_load_failure: every diagnostic the load raised, one
// per line, the way the CLI writes them.
typedef enum {
    LHAT_LOAD_OK,
    LHAT_LOAD_CANNOT_READ,
    LHAT_LOAD_REJECTED,
    LHAT_LOAD_OUT_OF_MEMORY
} LhatLoadStatus;

// 05 の 8.9: the bytes at `path`, read through the loader the program was
// given and through nothing else -- so a host that handed none over reads
// nothing here either. NULL when there is no loader or it has nothing for
// the path; otherwise a buffer the caller frees with lhat_free.
//
// For a module that has to see a file's text before the program does, which
// is std.lton (08): it wraps what it reads before handing it back to be
// checked. A module that wants the text checked as it stands wants
// lhat_program_load_file instead.
char *lhat_program_read(LhatProgram *program, const char *path,
                        size_t *length);

// 08: how a text that is data rather than a program is read. What differs is
// only what its names may reach.
typedef struct {
    // 05 の 8.2: whether the initial bindings the host set are in scope.
    //
    // They are the host's convenience for a program it means to run, and a
    // configuration file is not that: what the host bound for its own units
    // is no part of what a text read as data may name. std.lton (08) is what
    // this exists for, and it leaves them out.
    bool initial_bindings;
} LhatLoadOptions;

LhatLoadStatus lhat_program_load_text_with(LhatProgram *program,
                                           const char *name, const char *text,
                                           size_t length,
                                           const LhatLoadOptions *options,
                                           LhatProto **out);

LhatLoadStatus lhat_program_load_text(LhatProgram *program, const char *name,
                                      const char *text, size_t length,
                                      LhatProto **out);
LhatLoadStatus lhat_program_load_file(LhatProgram *program, const char *path,
                                      LhatProto **out);
const char *lhat_program_load_failure(const LhatProgram *program);

// The same failure with where it was. The position indexes the source of the
// unit that would not compile, which `path` names -- pass NULL for it where
// the name is not wanted. Answers a zeroed result while nothing has failed.
LhatCompileResult lhat_program_compile_failure(const LhatProgram *program,
                                               const char **path);

// ---------------------------------------------------------------------------
// What a unit answers
// ---------------------------------------------------------------------------

// What lhat_program_compile made of the unit -- what lhat_run takes. NULL
// until that ran, or when the unit would not compile.
const LhatProto *lhat_unit_proto(const LhatUnit *unit);

// 05 の 3 章: the path the unit declared with module^, or NULL when it
// declared none (3.2) -- a script, whose body is a run of statements.
const char *lhat_unit_module_name(const LhatUnit *unit);

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

// 03 の 3.1: true when this is one of the three the checker reports only
// under strict -- a gap inference left in a result, a parameter, or a
// binding, reaching a place with nothing else to say about it. relaxed
// would not have reported it at all, since it treats the gap as dynamic
// and waves it through (03 の 3.5) -- so a host that builds this unit
// relaxed will not stop on it either. For a tool showing both what strict
// would say and what the host actually builds with (a language server,
// say): this is the one it may show as advisory rather than fatal.
bool lhat_unit_diagnostic_relaxed_ok(const LhatUnit *unit, size_t index);

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

// As much of a written type as a host can act on. Nothing else in a host's
// world is shaped like L^'s types, so this is the same coarse reading 18.3
// gives an annotation's arguments rather than a second type system.
typedef enum {
    LHAT_UNIT_TYPE_OTHER,
    LHAT_UNIT_TYPE_NUMBER,
    LHAT_UNIT_TYPE_STRING,
    LHAT_UNIT_TYPE_BOOL
} LhatUnitTypeKind;

// What a definition declares, in the order it was written. An annotation says
// what a member is for (18.1), and acting on that takes knowing its shape --
// which the tree holds whether or not the member has a value at run time.
typedef struct {
    const char *name;  // borrowed; NULL when there is no such one
    size_t name_length;
    bool declared;  // 14.15: written with a type and no value
    // 18.7改: written as an f^ or p^ whose body holds no statement. Not a
    // declaration -- there is a value, and its type is read off the
    // signature the same way as any other member's. It is the one spelling a
    // writer has for "there is nothing here for the language to run", which
    // is what lets a host put its own value under the name without any of
    // what was written going quietly unrun.
    bool empty_body;
    // How many parameters it wrote, read off a written signature or off the
    // f^ / p^ the member holds. 13.4 makes self^ none of them, so a receiver
    // is not counted -- what is here is what a call writes.
    size_t parameter_count;
    // 14.6: the type of 'name : type = value', and where only a value was
    // written, what that value is written as. A host showing a field in an
    // editor has to put a widget on it before anything has run, and the tree
    // is all there is to go on then.
    LhatUnitTypeKind type;
} LhatUnitMember;

// The members of the definition `definition` holds, its template's fields
// included -- 18.4 puts an annotation on both, and a host asking what a
// class declares does not care which it was written as.
size_t lhat_unit_member_count(const LhatUnit *unit, const char *definition);
LhatUnitMember lhat_unit_member(const LhatUnit *unit, const char *definition,
                                size_t index);

// One parameter of the member at `member`, as it was written. The name is
// what a host shows a reader -- an editor's argument list, a signal's
// connection dialog -- and is why this is asked of the tree at all: a type
// alone would have to number them.
typedef struct {
    const char *name;  // borrowed; NULL when it was written without one
    size_t name_length;
    LhatUnitTypeKind type;
    bool variadic;  // 13.7's '...' tail
} LhatUnitParameter;

LhatUnitParameter lhat_unit_member_parameter(const LhatUnit *unit,
                                             const char *definition,
                                             size_t member, size_t at);

// The plain names the member's body writes as a call argument, in written
// order. A string and an id^ (01 の 3.3) are the same thing here, and an
// argument the call works out is not one at all.
//
// This is what lets a host check a body against the meaning it gave the
// member: 18.1 leaves that meaning entirely to the host, so the language
// cannot say a body disagrees with it -- but it can hand over the names the
// body wrote and let the host say so.
typedef struct {
    const char *text;  // borrowed; NULL when there is no such one
    size_t length;
} LhatUnitText;

size_t lhat_unit_member_written_name_count(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member);
LhatUnitText lhat_unit_member_written_name(const LhatUnit *unit,
                                           const char *definition,
                                           size_t member, size_t at);

// 05 の 4.5: what the unit published (public^), as the checker settled it
// -- not the coarse reading above, the type itself. For a host holding a
// contract the unit is meant to keep -- a game's update is 'p^number^;' --
// and wanting the break found before the first frame, with what the unit
// actually wrote in the message. Empty for a unit that is not a module^,
// or did not check.
size_t lhat_unit_export_count(const LhatUnit *unit);
LhatUnitText lhat_unit_export_name(const LhatUnit *unit, size_t index);

// The export's type in 02 の 14.10's spelling -- what typeof^(x).signature
// answers. lhat_report_write's convention: measures with (NULL, 0), fills
// up to `capacity` including the NUL. SIZE_MAX when nothing of that name
// was published.
size_t lhat_unit_export_type(const LhatUnit *unit, const char *name,
                             char *out, size_t capacity);

// Whether the export conforms to `signature` -- 02 の 13.5's conformance,
// the question a call asks of its argument. The text may name what the
// program registered before the check, as a registration's own may (8.7).
// False when nothing of that name was published, or the text does not read
// as a type.
bool lhat_unit_export_conforms(const LhatUnit *unit, const char *name,
                               const char *signature);

// ---------------------------------------------------------------------------
// 01 の 6.4: what a unit says about itself
// ---------------------------------------------------------------------------

// The comment block written directly above one thing, as prose. 6.4 settles
// that L^ has no spelling for a description -- every comment is kept alike,
// and the block above a thing is what it says about it, the way Go has it.
// So this is where a host reads one, and there is nothing to write.
//
// `definition` and `name` address the same four things annotations do:
//
//   (NULL, NULL)   the unit itself -- the block at the head of the file
//   (NULL, "x")    the top-level binding x
//   ("D", NULL)    the binding that holds the definition D
//   ("D", "hp")    the member or field hp of D
//
// A blank line ends a block, so a note left further up belongs to whatever
// it was written under and not to what follows it.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it. So measuring
// is a call with (NULL, 0).
size_t lhat_unit_documentation(const LhatUnit *unit, const char *definition,
                               const char *name, char *out, size_t capacity);


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

// 05 の 8.8改: the same, under a type registered earlier. A host whose own
// model is a class tree -- an engine's ClassDB, a widget toolkit -- writes
// the tree here instead of flattening it into one opaque type and losing
// every boundary the checker could have held.
//
// Single inheritance, which is what such models have. A value of the derived
// type stands where the base is asked for, fits^ answers for the chain, and
// lhat_hostdata_pointer gives the pointer back for a base's tag.
//
// THIS IS A PROMISE ABOUT POINTERS. Declaring the relation says a pointer of
// this type may be read as one of the base's -- true for C++ single
// inheritance with no virtual bases, and unverifiable from here. 02 の 15.13's
// closed^ is the same kind of thing: written by whoever knows, taken at its
// word by everything downstream.
//
// The base has to be registered already; NULL when it is not, when the name
// is taken, or when there is no memory. Nothing else about the order
// changes -- what a derived type inherits is settled when registration
// closes (the first check), so members may be registered before or after
// this call, in whichever order suits the host.
const LhatHostDataTag *lhat_register_hostdata_subtype(LhatProgram *program,
                                                      const char *module,
                                                      const char *name,
                                                      const char *base_module,
                                                      const char *base_name);

// A member of a type registered earlier. `signature` describes it; a p^ or f^
// whose first parameter is written self^ is an instance method (14.4).
//
// 8.8改: a derived type inherits what its base registered. Registering the
// same name on the derived one is what replaces it there.
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
// ("std.math.Vector3"); a first parameter of self^ is the receiver (14.4).
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
// saves. `targets` is where it may be written and how many times.
//
// What else a registration has to say is said by the two below, one thing
// each. A registration gains parts as hosts want more of them, and a call
// that grew a parameter for every one would make every caller carry the
// ones it has no use for.
//
// Belongs before lhat_program_check, with every other registration (8.7).
bool lhat_register_annotation(LhatProgram *program, const char *module,
                              const char *name, uint32_t targets);

// 18.3: what the arguments have to look like, written in 13 章's grammar as
// a p^ -- they are what it takes and there is nothing for it to answer.
// Without this nothing is asked of them. 18.3 keeps an argument a literal,
// and a bare name arrives as a string^: what the name means is the host's.
bool lhat_register_annotation_signature(LhatProgram *program,
                                        const char *name,
                                        const char *signature);

// 18.5.1: two names that are two answers to one question -- @game and @tool
// on a Godot class, where one says a node wears it and the other says the
// same and that it runs while a scene is being edited. A file carries one of
// them or the other and never both.
//
// FILEUNIQUE cannot say this: it counts each registration on its own, so two
// names each written once are each within their count and the pair is what
// nothing sees. Only the host knows the two are one choice, so this is the
// host saying it.
//
// Said from either side -- the checker looks both ways, so which of the pair
// was written first changes nothing. Saying it from both is how the pair
// reads as a pair to somebody looking at the registrations.
//
// Neither of the two takes a module: 18.2 keeps the annotation namespace
// flat, so a name is the whole of an address and a second registration of
// one is refused whatever module asked for it.
bool lhat_register_annotation_exclusive(LhatProgram *program,
                                        const char *name, const char *other);

// 18.5.2: a name that means nothing on its own -- @icon on a Godot class,
// which the engine hangs on a global class name and so on the @export_class
// that asks for one. Writing it without that one is writing a mark that
// quietly does nothing, which is the loss 18.4改 refused.
//
// Read on the declaration the annotation is written above, not over the
// file: standing beside something is standing beside it, and an @icon over
// one class with the @export_class over another are not beside each other.
// That is what tells this from the exclusion above, which is a file's to
// answer because two answers to one question can be written anywhere in it.
//
// Several may be said, and any one of them satisfies it -- the host is
// naming what would do, not a list to write out.
bool lhat_register_annotation_requisite(LhatProgram *program,
                                        const char *name, const char *other);

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

// 04 の 2.7: the same, under the other top -- what localerrordef^ declares.
// A caller of one of these may not return it, so the checker refuses a try^
// that would and refuses a signature written to admit one: what a host
// registers here is a failure the calling code has to settle where it is.
//
// Registering the same module/name under a different top than an earlier
// program did is refused, as a different variant list is (05 の 8.7改): one
// name stands for one declaration across the process.
bool lhat_register_local_error_kind(LhatProgram *program, const char *module,
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
// finds it. Belongs before the run.
// 05 の 8.7: something a registration made, given back when the program is.
//
// A module that registers functions almost always keeps a little state of
// its own beside them -- the error kinds and tags its functions read, which
// belong to this program and cannot be shared with another. That state has
// to live as long as the program and go when it goes, and a registering
// function that answers `bool` hands its caller no handle to free. So it
// says here what to call instead.
//
// Called when the program is disposed, before anything of the program's own
// is torn down, in the reverse of the order registered -- so a module may
// undo what a module it built on top of has not undone yet. False when out
// of memory, and then nothing was recorded.
//
// This is for state a registration handed to the program. A host that keeps
// its own handle to what it made needs nothing here: it frees the program
// and frees its own thing, in whichever order it likes.
typedef void (*LhatProgramDisposeFn)(void *context);

bool lhat_program_on_dispose(LhatProgram *program, LhatProgramDisposeFn call,
                             void *context);

// ---------------------------------------------------------------------------
// 05 の 8.7: what a declaration is, is the C call that makes it
// ---------------------------------------------------------------------------
//
// 7.3 makes a registered type its own type by its declaration and not by its
// members, and 04 の 2.4 says the same of an error kind. That declaration is
// the register call itself -- so registering std.io.File into two programs
// is one declaration made twice, and both come away with the SAME identity.
// The run time compares tags by address, and a second tag would make one
// host type into two that agree about everything except the one thing that
// decides.
//
// So lhat_register_hostdata_type, lhat_register_hostvalue_type and
// lhat_register_error_kind answer what a previous program already declared,
// and a declaration that DISAGREES is refused: a host value type of another
// size, an error kind with another list of variants, a dispose^ that is
// another function. Everything else a registration makes -- the types the
// checker reads, and the context a host function is handed -- stays the
// program's, which is why std.load can keep the program in its own.
//
// The identities live for as long as the process, which is what makes them
// cost nothing per reload. lhat_registry_dispose gives them back, and may be
// called only when NO LhatProgram exists: what is here is what every
// program's registrations point at. A host that runs to exit needs it only
// to leave a clean heap for a leak check; one loaded and unloaded as a
// plugin wants it on the way out.
//
// Not thread safe, and not meant to be: registration belongs before anything
// runs, which is before std.thread has started a thread.
void lhat_registry_dispose(void);

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
// the error kinds and hostdata types fits^ names for the compiler. Both borrow
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
// A top-level "strict" boolean names the value this program was made with
// (03 の 3.1) -- what this host actually passes to lhat_program_init or
// lhat_check_unit when it is not a tool reading the dump. A reader checking
// the same units for a different purpose (a language server, wanting the
// most it can say) may check strict regardless and use this only to soften
// what lhat_unit_diagnostic_relaxed_ok marks: a host that builds relaxed
// will not stop on those.
//
// An annotation's places (18.5) are written as an object keyed by name --
// {"field": true, "public": true} -- rather than as the mask this API takes
// them as. A mask reads as a mask only to something holding the same enum,
// and the reader is a tool that cannot run this C and does not have one. The
// names are LhatAnnotationTarget's tails, lowercased. Only what is set is
// written; a reader takes a name written false, and one it does not know,
// and passes over both.
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
