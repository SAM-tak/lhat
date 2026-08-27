// L^ (lhat) -- what a host registered, in the shape the compiler and the
// machine read it (05 の 8.7-8.9). Produced by program.c's registration
// calls; the strings and objects are owned by the registering LhatProgram
// and outlive every machine and every compile that reads them.

#ifndef LHAT_HOSTED_H
#define LHAT_HOSTED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/object.h"

// 05 の 8.7 の誤り版: one error kind lhat_register_error_kind (program.h)
// registered. resolve_kind (compile.c) reads these to answer a qualified
// name an import^ed module wrote -- "module...Name" for the declaration as
// a whole, "module...Name.Variant" for one of its kinds.
typedef struct LhatHostErrorKind {
    const char *module;                    // "." 区切り可; declared by
    const char *name;                      // hosted_table 参照
    const LhatErrorKind *group;            // 宣言全体。fits^ module.Name 用
    const char *const *variant_names;
    const LhatErrorKind *const *variants;  // variant_names と対応
    size_t variant_count;
} LhatHostErrorKind;

// 02 の 18.5: one annotation lhat_register_annotation (program.h) registered.
// The checker reads these -- an annotation the host did not register is not
// one a program may write, and where it may be written is what `targets`
// says. Nothing at run time reads them: 18.1 makes an annotation information
// for the host, and the host asks the unit for it after checking.
typedef struct LhatAnnotationDecl {
    const char *module;
    const char *name;
    uint32_t targets;   // LhatAnnotationTarget, or-ed
    // What the arguments have to look like, in 13 章's grammar, parsed. NULL
    // when the registration wrote none, which asks nothing of them.
    const struct LhatType *signature;
    // 18.5.1: the names this one is an alternative to, as the host said
    // them. A file carries this or one of those and never both -- which is
    // the one thing FILEUNIQUE cannot say, counting each name on its own.
    // The names are the program's, and need not be registered themselves.
    const char **exclusives;
    size_t exclusive_count;
    // 18.5.2: the names one of which has to stand beside this one, on the
    // same declaration. The other side of the pair above: that one says two
    // marks are one choice, this one says a mark is half of something.
    // Written together is written together, so a name found elsewhere in the
    // file is not one of these.
    const char **requisites;
    size_t requisite_count;
} LhatAnnotationDecl;

// 05 の 8.8 の fits^ 版: one hostdata type lhat_register_hostdata_type
// (program.h) registered. resolve_fits_type (compile.c) reads these to
// answer a qualified name an import^ed module wrote -- "module...Name" --
// against the tag 8.8's identity rule compares by.
typedef struct LhatHostTypeEntry {
    const char *module;
    const char *name;
    const LhatHostDataTag *tag;
    // 05 の 8.8改: whether this one has been given what its base registered.
    // program.c's flatten pass sets it; a base is done before what is under
    // it, so a chain settles from the root down however the entries are
    // ordered. Nothing outside that pass reads it.
    bool flattened;
} LhatHostTypeEntry;

// 05 の 8.9: the same for one host value type lhat_register_hostvalue_type
// (program.h) registered. The compiler reads these to put a tag -- and with
// it a width -- behind a written "module...Name", both for a type position
// and for fits^.
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

// 05 の 8.8改: puts one registered type's members table under its base's, so
// that what the base declared is reached by walking. See the body in vm.c;
// program.c's install is the one caller, once per type that has a base.
bool lhat_machine_link_hostdata_base(struct LhatMachine *machine,
                                     const char *module, const char *name,
                                     const char *base_module,
                                     const char *base_name);

bool lhat_machine_bind_hostvalues(struct LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count, size_t slots);

#endif  // LHAT_HOSTED_H
