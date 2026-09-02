// L^ (lhat) -- the inside of LhatProgram and LhatUnit.
//
// program.h keeps both opaque: a host registers, checks, compiles, runs and
// reads reports, and never walks a unit's tree (05 の 8.7). What is here is
// for the library itself and for the in-tree tools that are the language's
// own front ends -- program.c, cli/'s dump and REPL paths, lsp/, and the
// tests -- which read the stages' results directly.

#ifndef LHAT_PROGRAM_INTERNAL_H
#define LHAT_PROGRAM_INTERNAL_H

#include "check.h"
#include "hosted.h"
#include "lhat/lexer.h"
#include "parser.h"
#include "lhat/program.h"
#include "lhat/source.h"
#include "type.h"

struct LhatUnit {
    char *path;  // resolved and normalised; the key 5.3 loads once against
    bool loaded;
    // The program this is a unit of -- what lhat_unit_export_conforms reads
    // a written type against (its arena and its registrations).
    struct LhatProgram *program;

    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatCheckResult checked;

    // 05 の 4.5: the export descriptors lhat_unit_export_type answered,
    // built lazily out of `checked` onto the proto's chunk heap (where
    // 5.13's fits^ descriptors live, with the same lifetime). The array is
    // parallel to the exports walk and cleared with the stages -- a
    // reload's recheck starts it empty again.
    struct LhatRuntimeType **export_rt;
    size_t export_rt_count;
    // 05 の 10 章: a binary unit has no checked exports to walk, so it
    // carries the names beside the descriptors, parallel to export_rt.
    // Owned; NULL for a text unit.
    char **export_names;

    LhatUnitState state;

    // 05 の 5.7: what module^ the last check of this unit read, copied here
    // rather than left in `checked`. An invalidation throws the check away,
    // and the name is exactly what the host needs afterwards -- it is the
    // key a machine's L^.modules holds the unit under, which
    // lhat_machine_forget_unit has to be handed. NULL for a script (3.2).
    char *module_name;

    // What lhat_program_compile made of it, owned here. NULL until that ran
    // -- or while it will not compile.
    LhatProto *proto;

    // 05 の 5.3: the units this one's require^s named, in the order its
    // compile met them -- the number in each UNIT instruction is a position
    // here. Filled by the compile of this unit and read by the filling of
    // its table (lhat_program_compile's second pass), since a unit named
    // here may not have compiled yet when this one does.
    //
    // 5.7: kept afterwards rather than freed, because it is the only record
    // of which way the graph runs. lhat_program_invalidate walks it backwards
    // to find every unit a changed one reaches.
    struct LhatUnit **referenced;
    size_t referenced_count;
    size_t referenced_capacity;

    // 05 の 8.2 with 08: read as data rather than as a program, so the
    // initial bindings are not in scope. Set by lhat_program_load_text_with
    // before anything is checked; false for every unit of the program.
    bool as_data;

    // 05 の 10 章: read from bytes rather than text. No source, no tree, no
    // check result -- `proto` is the whole of it, and the stages above are
    // zero. What reads them guards on parsed.root already.
    bool binary;

    // 05 の 5.7: the text this unit was last read from, hashed, so that
    // lhat_program_invalidate can tell a save that changed something from a
    // save that changed nothing. Zero before anything was read.
    uint64_t source_hash;

    struct LhatUnit *next;
};

// 05 の 8.7改2: one registered enum. The strings are owned; decl_rt is the
// per-program identity on host_heap.
typedef struct LhatProgramEnum {
    char *module;
    char *type;  // NULL for an enum of the module itself
    char *name;
    char **members;   // owned, each owned
    int64_t *values;  // owned
    size_t count;
    LhatRuntimeType *decl_rt;
} LhatProgramEnum;

// 05 の 10 章: one entry of the signature table a program read (10.7): the
// text a registration is written with, and where its record stands in the
// bytes the program keeps. Sorted by text, so a lookup halves its way in.
typedef struct LhatSignatureIndex {
    const char *text;  // into the program's copy of the bytes; NUL-terminated
    size_t text_length;
    size_t body_offset;
    size_t body_length;
} LhatSignatureIndex;

// 05 の 10 章: the identity an enum^ declaration has in a binary program --
// the token fits^ compares (RT_ENUM.enum_decl), found again by the unit it
// was declared in and its name. The type object is the arena's; the strings
// are owned here.
typedef struct LhatEnumIdentity {
    char *path;
    char *name;
    LhatType *decl;
} LhatEnumIdentity;

// One thing the host registered, kept so that lhat_program_install can build
// the values once a machine exists. The type side is in `program->hosted`
// already, since the checker needs it before anything runs.
// 05 の 8.7改: which scalar a constant entry carries. NONE for a function
// or a type declaration -- the two shapes every entry had before.
typedef enum {
    LHAT_HOST_CONST_NONE = 0,
    LHAT_HOST_CONST_INTEGER,
    LHAT_HOST_CONST_REAL,
    LHAT_HOST_CONST_BOOL,
    LHAT_HOST_CONST_STRING
} LhatHostConstKind;

typedef struct LhatHostEntry {
    char *module;   // owned; the dotted path
    char *type;     // owned; NULL when the entry belongs to the module itself
    char *name;     // owned
    // The signature as the registration wrote it, owned; NULL for a type
    // declaration, which has none. `signature` below is what checking uses;
    // this is for writing the registration back out
    // (lhat_program_dump_host_api) -- the parsed type cannot be turned back
    // into text without losing the names it was written with (a hostdata
    // type prints structurally, an error kind loses its module prefix), so
    // the text itself is what survives.
    char *signature_text;
    LhatHostFn call;  // NULL for a type, which carries no value of its own
    void *context;
    uint8_t parameters;
    bool has_variadic;  // 13.7: the signature ended in '...' -- see LhatHost
    bool takes_self;
    bool self_last;     // 02 の 11.3改: the receiver is the right operand
    // 02 の 14.12: the descriptor the machine's overload search reads, one
    // per parameter, lowered once at registration onto host_heap (the
    // nodes) -- install hands every machine the same ones. The array is
    // owned; NULL where there is nothing to compare.
    LhatRuntimeType **parameter_types;
    // 05 の 8.8: the tag values of this type carry. Kept on the entry so that
    // it lives as long as the program and points at the entry's own strings.
    LhatHostDataTag *tag;

    // 05 の 8.7改: the constant's shape and value, when the entry is one.
    // `call` is NULL then, as for a type declaration -- const_kind is what
    // tells the two apart. const_text is owned.
    LhatHostConstKind const_kind;
    int64_t const_integer;
    double const_real;
    bool const_bool;
    char *const_text;
} LhatHostEntry;

// 05 の 8.6: one member of L^ itself. Separate from the above because it does
// not land under L^.modules, so install puts it somewhere else.
typedef struct LhatGlobalEntry {
    char *name;  // owned
    char *signature_text;  // owned, as on LhatHostEntry
    LhatHostFn call;
    void *context;
    uint8_t parameters;
    bool has_variadic;
    bool takes_self;
    bool self_last;
    LhatRuntimeType **parameter_types;  // 14.12, as on LhatHostEntry
} LhatGlobalEntry;

struct LhatProgram {
    // 6 章: shared, so the types one unit publishes stay valid in the units
    // that require it. Never emptied while the program lives: 05 の 5.7's
    // invalidation cannot free what it retired, since a unit it did not
    // touch may still be naming it.
    LhatTypeArena types;

    LhatUnit *units;
    bool strict;

    // 05 の 5.7: bodies lhat_program_invalidate took off their units and did
    // not free. A closure made before the invalidation still points into one,
    // so they wait here until lhat_program_discard_retired -- or until the
    // program goes, which outlives every machine that could hold one.
    LhatProto **retired;
    size_t retired_count;
    size_t retired_capacity;

    // 05 の 5.7: what those bodies' constants named, taken off them when
    // they were freed. A chunk's objects were always meant to outlive every
    // machine, and a machine goes on holding them after the body is gone --
    // L^.modules is keyed by the strings a unit's own prologue loads. So
    // discarding a retired body frees its code and hands these here, where
    // they stay until the program does. See lhat_proto_give_objects.
    LhatHeap retired_objects;

    LhatProgramLoader load;
    void *loader_context;

    LhatProgramDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

    // Why lhat_program_compile answered false, for a caller with a
    // diagnostic to write. It compiles unit by unit and stops at the first
    // that will not, so what a reader has to be told is that one status
    // rather than "something".
    // LHAT_COMPILE_OK until a compile has actually failed.
    LhatCompileStatus compile_status;

    // The same failure with where it was, and the unit it was in -- a
    // position means nothing without the source it indexes. NULL until a
    // compile has failed, and left NULL for a failure with no unit to blame
    // (a table not fitting in memory).
    LhatCompileResult compile_result;
    const LhatUnit *compile_unit;

    // 05 の 5.6: why the last lhat_program_load_* answered false, rendered
    // (every diagnostic, one per line). Owned; NULL until one has failed.
    char *load_failure;

    // 05 の 8.7: what the host registered, as one nested table type keyed by
    // module path -- the same shape L^.modules has, since that is where it
    // ends up. import^ resolves against this and against nothing else, which
    // is what keeps its answer independent of the order units are checked in.
    LhatType *hosted;
    struct LhatHostEntry *host_entries;
    size_t host_entry_count;

    // 05 の 8.7改2: the enums the host registered. Names owned; the RT_ENUM
    // descriptor on host_heap is the per-program identity fits^ compares;
    // install builds the value objects per machine.
    struct LhatProgramEnum *host_enums;
    size_t host_enum_count;
    size_t host_enum_capacity;

    // 05 の 10 章: see LhatEnumIdentity.
    LhatEnumIdentity *enum_identities;
    size_t enum_identity_count;
    size_t enum_identity_capacity;

    // 10.7: the signature table lhat_program_read_signatures kept -- a copy
    // of the bytes and the sorted index into them. A build without the
    // front end registers a signature by looking it up here instead of
    // reading the text.
    uint8_t *signatures;
    size_t signature_length;
    LhatSignatureIndex *signature_index;
    size_t signature_count;
    size_t host_entry_capacity;

    // 02 の 18.5: what lhat_register_annotation recorded. Only the checker
    // reads these -- an annotation never runs, so nothing is installed into
    // a machine for one. The strings are owned here.
    // The module and name strings are owned here, through the declaration
    // that points at them; the signature text is kept beside it because the
    // declaration carries the parsed type and not the words it was written
    // with (the same reason LhatHostEntry keeps both).
    LhatAnnotationDecl *annotations;
    char **annotation_signatures;
    size_t annotation_count;
    size_t annotation_capacity;

    // 05 の 8.7: the run-time objects a registration makes -- 04 の 12.4's
    // error kinds, and 02 の 14.12's descriptors of every registered
    // signature. On a heap of the program's own, born black, so they belong
    // to no machine's collection and live as long as the program: the same
    // reason chunk->heap (code.h) holds a unit's (lhat_proto_new's comment).
    // A machine is handed pointers into it at install and builds nothing.
    // host_error_entries is what lhat_compile_module is given as
    // LhatUnits.host_errors.
    LhatHeap host_heap;
    LhatHostErrorKind *host_error_entries;
    size_t host_error_entry_count;
    size_t host_error_entry_capacity;

    // 05 の 8.8 の fits^ 版: lhat_register_hostdata_type が返した
    // LhatHostDataTag を、コンパイラが "module.Name" から引けるように
    // した登録簿。tag 自体は host_entries[i].tag が既に持っているが、
    // それは非公開の LhatHostEntry の中なので、hosted.h から読める形の
    // 薄いコピーをここにも持つ -- host_error_entries と同じ理由。
    LhatHostTypeEntry *host_type_entries;
    size_t host_type_entry_count;
    size_t host_type_entry_capacity;
    // 05 の 8.8改: whether a derived type has been given what its base
    // registered. Done once, when registration closes -- which is the first
    // check, since registering after one "is too late and answers false"
    // (program.h). Doing it at each lhat_register_hostdata_subtype would be
    // too early: program.h's own advice is to register the bare types first
    // and give them their members after, and a tree registered that way has
    // no members to inherit at the moment the relation is declared.
    bool hostdata_flattened;

    // 05 の 8.9: the host value types, one entry per registration. Unlike
    // the two registries above this one owns its tags (and their field
    // arrays); the strings belong to host_entries as usual. Registration
    // order is the tag's index, which is how a machine finds the members
    // table it built for the type at install.
    LhatHostValueTypeEntry *hostvalue_type_entries;
    size_t hostvalue_type_entry_count;
    size_t hostvalue_type_entry_capacity;

    // 05 の 8.7: what lhat_program_on_dispose recorded -- what a
    // registration made and the program gives back. Run in reverse order at
    // dispose, before anything of the program's own goes.
    struct LhatProgramDisposal {
        LhatProgramDisposeFn call;
        void *context;
    } *disposals;
    size_t disposal_count;
    size_t disposal_capacity;

    // 05 の 8.6: what the host put in L^ itself, as the type side of it. The
    // checker's own L^ carries these on top of what 8.6 lists.
    LhatType *globals;
    struct LhatGlobalEntry *global_entries;
    size_t global_count;
    size_t global_capacity;

    // 05 の 8.2: the names bound without a require^, kept as two arrays so
    // that check.h and compile.h can each read them without knowing a type
    // the other declares.
    char **initial_names;    // owned
    char **initial_members;  // owned
    size_t initial_count;
    size_t initial_capacity;
};

// The by-value forms, for this library and the in-tree tools; a host uses
// lhat_program_new/lhat_program_free (program.h), which wrap these around
// an allocation.
void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context);
void lhat_program_dispose(LhatProgram *program);

// 05 の 10 章: what serialize.c asks of the program while reading a unit.
// `relative` is read against `from`'s directory, as a require^ is.
LhatUnit *lhat_program_require_unit(LhatProgram *program, LhatUnit *from,
                                    const char *relative, size_t length);
// The normalised path `relative` names from `from`, owned by the caller.
char *lhat_program_resolve_path(const LhatUnit *from, const char *relative,
                                size_t length);
void lhat_program_report(LhatProgram *program, LhatProgramErrorCode code,
                         const char *path);
// The identity for the enum^ `name` declared in the unit at `path`, made
// on first asking.
const LhatType *lhat_program_enum_identity(LhatProgram *program,
                                           const char *path,
                                           const char *name);
// 10.7: the descriptor the signature table holds for `text` -- a
// SUBROUTINE whose parts are the parameters, with the receiver and variadic
// marks, built onto host_heap. NULL when the table has no such text, or a
// name in it is not registered on this program (reported as
// HOST_MISMATCH).
const LhatRuntimeType *lhat_program_signature_type(LhatProgram *program,
                                                   const char *text);

#endif  // LHAT_PROGRAM_INTERNAL_H
