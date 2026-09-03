// L^ (lhat) -- the one registry of what a host declared, for the process.
//
// Section numbers refer to DesignDocuments/05-modules.md.
//
// 7.3 makes a registered type its own type by its declaration and not by its
// members, and the declaration is the C call that registers it. Two programs
// registering std.io.File are that one declaration made twice, so they have
// to come away with the same identity: the run time compares tags by their
// address (object.c's lhat_type_rt_equal, and fits^), and a second tag would
// make one host type into two that agree about everything except the one
// thing that decides.
//
// So the identities live here, for as long as the process does, and a
// program's registration looks them up rather than making them:
//
//   - LhatHostDataTag        (8.8)
//   - LhatHostValueTag       (8.9), and the index a machine finds its
//                            members table by
//   - LhatErrorKind          (04 の 2.4), a declaration's group and variants
//
// Everything else a registration makes stays with the program: the checker's
// types come from its arena, and the entries carry the `context` a host
// function is handed, which a module like std.load needs to be the program's.
//
// A second declaration that AGREES answers what is already there. One that
// DISAGREES is refused -- a different payload under a name that is already
// taken is two types wearing one name, which is what 7.3 exists to prevent.
//
// Not thread safe, and not meant to be: registration belongs before anything
// runs (8.7), which is before std.thread has started a thread.

#ifndef LHAT_REGISTRY_H
#define LHAT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "lhat/object.h"

// 8.8: the tag for `module.name`, made on the first declaration and answered
// to every one after it. NULL only when out of memory -- a hostdata
// declaration carries nothing that could disagree.
const LhatHostDataTag *lhat_registry_hostdata(const char *module,
                                              const char *name);

// 8.8改: the type this one is under. Answers false when a base is already
// set and it is not this one (a name stands for one declaration, and what it
// is under is part of that), and when the two would make a cycle.
bool lhat_registry_set_hostdata_base(const LhatHostDataTag *tag,
                                     const LhatHostDataTag *base);

// 8.8: what the type registered as dispose^. Answers false when a release is
// already set and it is not this one -- the same declaration cannot hand
// back two different ways.
bool lhat_registry_set_release(const LhatHostDataTag *tag, LhatHostFn release,
                               void *context);

// 8.8改2: the sharing contract the type declared. False when one is
// already set and it is not this one.
bool lhat_registry_set_hold(const LhatHostDataTag *tag, LhatHostHoldFn retain,
                            LhatHostHoldFn let_go, void *context);

// 8.9: the same for a host value type. `size` is the payload the host
// declared; a second declaration of a different size is refused (NULL),
// since the width is what every frame that holds one was laid out against.
const LhatHostValueTag *lhat_registry_hostvalue(const char *module,
                                                const char *name, size_t size);

// 8.9: one field for direct access. A field already there under the same
// name has to match in offset and kind; one that does not is refused.
bool lhat_registry_hostvalue_field(const LhatHostValueTag *tag,
                                   const char *name, size_t offset,
                                   LhatHostValueFieldKind kind);

// How many host value types the process has declared, which is one past the
// largest index any tag carries. A machine's members table array is taken to
// this width so that a program declaring only some of them still finds its
// own at the index its tags name.
size_t lhat_registry_hostvalue_count(void);

// 04 の 2.4: the kinds one errordef^-shaped declaration makes -- the group
// and one per variant, in the order given. `out_variants` takes
// `variant_count` of them and may be NULL, as may `out_group`.
//
// A second declaration with a different list of variant names is refused:
// the kinds are the declaration, and two lists are two declarations.
// 04 の 2.7 with 11.6改3: localerror^.CastFailure. Nobody declares it and
// nobody registers it -- it is the language's own, and it is here for the
// reason every other identity is: 2.4 compares kinds by the declaration, so
// two programs have to reach the same object. NULL only out of memory.
const LhatErrorKind *lhat_registry_cast_failure(void);

// `local` is 04 の 2.7's family: a name declared under one top is not the
// same declaration as the same name under the other, so it takes part in the
// agreement check the way the variant list does.
bool lhat_registry_error_kind(const char *module, const char *name,
                              const char *const *variant_names,
                              size_t variant_count, bool local,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants);

// Frees every identity the process declared. May be called only when no
// LhatProgram exists: what is here is what their registrations point at.
// See lhat_registry_dispose in program.h, which is this under the name a
// host sees.
void lhat_registry_dispose(void);

#endif  // LHAT_REGISTRY_H
