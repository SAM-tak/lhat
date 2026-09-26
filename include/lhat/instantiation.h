// Compile-time host participation in call-shape instantiation checking.
#ifndef LHAT_INSTANTIATION_H
#define LHAT_INSTANTIATION_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LhatType LhatCheckType;
typedef struct LhatInstantiationContext LhatInstantiationContext;

typedef enum {
    LHAT_INSTANTIATION_DEFAULT,   // Keep the registered signature.
    LHAT_INSTANTIATION_RESOLVED,  // Use resolved_signature for this call only.
    LHAT_INSTANTIATION_PENDING,   // Arguments are not settled yet.
    LHAT_INSTANTIATION_REFUSED    // Reject this call shape.
} LhatInstantiationStatus;

// Types are borrowed and immutable. New types belong to the current check;
// never cache them across checks or programs. Handlers must be deterministic,
// side-effect free, and may run repeatedly during inference. Arguments exclude
// the implicit receiver (available separately), and tuple spreads are expanded.
typedef LhatInstantiationStatus (*LhatInstantiationCheckHandler)(
    LhatInstantiationContext *context, void *user,
    const LhatCheckType *signature, const LhatCheckType *receiver,
    const LhatCheckType *const *arguments, size_t count,
    const LhatCheckType **resolved_signature);

bool lhat_check_type_pending(const LhatCheckType *type);
// Look up a registered, fully qualified type name (not a signature parser).
const LhatCheckType *lhat_check_type_named(LhatInstantiationContext *context,
                                         const char *name);
const LhatCheckType *lhat_check_type_result(const LhatCheckType *type);
// NULL means not a coroutine; a coroutine ending without a value answers nil.
const LhatCheckType *lhat_check_coroutine_result(LhatInstantiationContext *context,
                                               const LhatCheckType *type);
const LhatCheckType *lhat_check_type_any(LhatInstantiationContext *context);
const LhatCheckType *lhat_check_type_nil(LhatInstantiationContext *context);
bool lhat_check_type_single_slot(const LhatCheckType *type);
bool lhat_check_type_same(const LhatCheckType *a, const LhatCheckType *b);
size_t lhat_check_type_union_count(const LhatCheckType *type);
const LhatCheckType *lhat_check_type_union_at(const LhatCheckType *type, size_t index);
const LhatCheckType *lhat_check_type_union(LhatInstantiationContext *context,
                                         const LhatCheckType *a, const LhatCheckType *b);
// A concrete nominal type with invariant arguments; runtime identity is base's.
const LhatCheckType *lhat_check_type_specialize(LhatInstantiationContext *context,
    const LhatCheckType *base, const LhatCheckType *const *arguments, size_t count);
const LhatCheckType *lhat_check_type_base(const LhatCheckType *type);
const LhatCheckType *lhat_check_type_argument(const LhatCheckType *type, size_t index);
// Clone a signature with a refined answer; its calling convention is unchanged.
// Checking refuses a result outside the registered result or a changed tuple
// width. The initial API refines results only, not parameters or effects.
const LhatCheckType *lhat_check_signature_result(LhatInstantiationContext *context,
    const LhatCheckType *signature, const LhatCheckType *result);

#ifdef __cplusplus
}
#endif
#endif
