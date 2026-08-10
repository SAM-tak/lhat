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
#include "vm.h"  // LhatCompileStatus: why a compile of the program stopped

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

    // Why that answered NULL, for a caller with a diagnostic to write. It
    // compiles unit by unit and stops at the first that will not, so what a
    // reader has to be told is that one status rather than "something".
    // LHAT_COMPILE_OK until a compile has actually failed.
    LhatCompileStatus compile_status;

    // 05 の 8.7: what the host registered, as one nested table type keyed by
    // module path -- the same shape L^.modules has, since that is where it
    // ends up. import^ resolves against this and against nothing else, which
    // is what keeps its answer independent of the order units are checked in.
    LhatType *hosted;
    struct LhatHostEntry *host_entries;
    size_t host_entry_count;
    size_t host_entry_capacity;

    // 05 の 8.7 の誤り版、04 の 12.4: lhat_register_error_kind が作る実行時
    // オブジェクト専用のヒープ。どの machine の GC サイクルにも属さず、
    // program 自身と同じだけ生きる -- chunk->heap (code.h) と同じ理屈
    // (lhat_proto_new のコメント参照)。host_error_entries は
    // lhat_compile_module に渡す LhatUnits.host_errors の元になる登録簿。
    LhatHeap host_error_heap;
    LhatHostErrorKind *host_error_entries;
    size_t host_error_entry_count;
    size_t host_error_entry_capacity;

    // 05 の 8.8 の isa^ 版: lhat_register_hostdata_type が返した
    // LhatHostDataTag を、コンパイラが "module.Name" から引けるように
    // した登録簿。tag 自体は host_entries[i].tag が既に持っているが、
    // それは非公開の LhatHostEntry の中なので、vm.h から読める形の
    // 薄いコピーをここにも持つ -- host_error_entries と同じ理由。
    LhatHostTypeEntry *host_type_entries;
    size_t host_type_entry_count;
    size_t host_type_entry_capacity;

    // 05 の 8.9: the host value types, one entry per registration. Unlike
    // the two registries above this one owns its tags (and their field
    // arrays); the strings belong to host_entries as usual. Registration
    // order is the tag's index, which is how a machine finds the members
    // table it built for the type at install.
    LhatHostValueTypeEntry *hostvalue_type_entries;
    size_t hostvalue_type_entry_count;
    size_t hostvalue_type_entry_capacity;

    // 05 の 8.6: what the host put in L^ itself, as the type side of it. The
    // checker's own L^ carries these on top of what 8.6 lists.
    LhatType *globals;
    struct LhatGlobalEntry *global_entries;
    size_t global_count;
    size_t global_capacity;

    // 05 の 8.2: the names bound without a require^, kept as two arrays so
    // that check.h and vm.h can each read them without knowing a type the
    // other declares.
    char **initial_names;    // owned
    char **initial_members;  // owned
    size_t initial_count;
    size_t initial_capacity;
} LhatProgram;

// 05 の 8.9: the loader is handed over rather than defaulted to, so nothing
// embedded reaches a file system without having been told to. `load` may be
// NULL, and then no unit can be read -- which is what a host wants when the
// only units are ones it hands over itself.
//
// port.h's lhat_load_file is written in this shape, for a host that does want
// the file system.
void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context);
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
typedef struct LhatMachine LhatMachine;
bool lhat_program_install(const LhatProgram *program, LhatMachine *machine);

// 03 の 4.3: the same for a prompt, which has no program of its own driving
// it. A host that wants import^ to answer there makes an LhatProgram with no
// loader -- 5.3 gives a require^ at a prompt nowhere to go anyway -- registers
// into it as it would for a file, and hands it to all three of the prompt's
// pieces: the check session, the compile session and the machine.
//
//     LhatProgram program;
//     lhat_program_init(&program, false, NULL, NULL);
//     lhatstdlib_io_register(&program);          // or any registration
//     lhat_program_install_checks(&program, checks);
//     lhat_program_install_compiles(&program, compiles);
//     lhat_program_install(&program, machine);
//
// What each hands over is what LhatRequire and LhatUnits carry for a file:
// the registry import^ resolves against and 8.6's L^ members for the checker,
// the error kinds and hostdata types isa^ names for the compiler. Both borrow
// from the program, so it has to outlive the sessions.
void lhat_program_install_checks(const LhatProgram *program,
                                 LhatCheckSession *session);
void lhat_program_install_compiles(const LhatProgram *program,
                                   LhatCompileSession *session);

// True when any unit, or the program itself, reported something.
bool lhat_program_has_errors(const LhatProgram *program);

const char *lhat_program_error_message(LhatProgramErrorCode code);

#endif  // LHAT_PROGRAM_H
