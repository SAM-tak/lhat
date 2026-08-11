// L^ (lhat) -- what a host registered, in the shape the compiler and the
// machine read it (05 の 8.7-8.9). Produced by program.c's registration
// calls; the strings and objects are owned by the registering LhatProgram
// and outlive every machine and every compile that reads them.

#ifndef LHAT_HOSTED_H
#define LHAT_HOSTED_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

// 05 の 8.7 の誤り版: one error kind lhat_register_error_kind (program.h)
// registered. resolve_kind (compile.c) reads these to answer a qualified
// name an import^ed module wrote -- "module...Name" for the declaration as
// a whole, "module...Name.Variant" for one of its kinds.
typedef struct LhatHostErrorKind {
    const char *module;                    // "." 区切り可; declared by
    const char *name;                      // hosted_table 参照
    const LhatErrorKind *group;            // 宣言全体。isa^ module.Name 用
    const char *const *variant_names;
    const LhatErrorKind *const *variants;  // variant_names と対応
    size_t variant_count;
} LhatHostErrorKind;

// 05 の 8.8 の isa^ 版: one hostdata type lhat_register_hostdata_type
// (program.h) registered. resolve_isa_type (compile.c) reads these to
// answer a qualified name an import^ed module wrote -- "module...Name" --
// against the tag 8.8's identity rule compares by.
typedef struct LhatHostTypeEntry {
    const char *module;
    const char *name;
    const LhatHostDataTag *tag;
} LhatHostTypeEntry;

// 05 の 8.9: the same for one host value type lhat_register_hostvalue_type
// (program.h) registered. The compiler reads these to put a tag -- and with
// it a width -- behind a written "module...Name", both for a type position
// and for isa^.
typedef struct LhatHostValueTypeEntry {
    const char *module;
    const char *name;
    const LhatHostValueTag *tag;
} LhatHostValueTypeEntry;

// 05 の 8.9: gives the machine its per-type members tables, one per
// registered host value type, found back by tag->index. Called by
// lhat_program_install after the entries have landed under L^.modules --
// the tables bound here are those very ones, so the collector reaches them
// through the environment already. program.c is the one caller.
struct LhatMachine;
bool lhat_machine_bind_hostvalues(struct LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count);

#endif  // LHAT_HOSTED_H
