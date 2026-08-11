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
#include "lexer.h"
#include "parser.h"
#include "program.h"
#include "source.h"
#include "type.h"

struct LhatUnit {
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
};

struct LhatProgram {
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
    // それは非公開の LhatHostEntry の中なので、hosted.h から読める形の
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

#endif  // LHAT_PROGRAM_INTERNAL_H
