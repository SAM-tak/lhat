// Static task semantics shared by runtime registration and host-API replay.
// This header uses only the public checker API: an editor need not link the
// worker pool or start any runtime machinery to check a task call.
#ifndef LHATSTDLIB_TASK_CHECK_H
#define LHATSTDLIB_TASK_CHECK_H

#include <string.h>
#include "lhat/program.h"

#define LHAT_TASK_ERRORS \
    "|std.task.TaskError.NotStarted|std.task.TaskError.Refused" \
    "|std.task.TaskError.Failed|std.error.OutOfMemory;"
// Two arms of async, registered in this order: a coroutine not yet started,
// and a subroutine called with nothing, with or without a result.
#define LHAT_TASK_ASYNC_SIGNATURE \
    "p^c^{->*->*} -> std.task.Task<ARG0.ResultType>" LHAT_TASK_ERRORS
#define LHAT_TASK_ASYNC_CALL_SIGNATURE \
    "p^(p^;)|(p^->any^;) -> std.task.Task<ARG0.ReturnType>" LHAT_TASK_ERRORS
#define LHAT_TASK_AWAIT_SIGNATURE \
    "p^std.task.Task -> ARG0.T0" LHAT_TASK_ERRORS

#if LHAT_WITH_FRONTEND
static LhatInstantiationStatus task_check_async(
    LhatInstantiationContext *context, void *user,
    const LhatCheckType *signature, const LhatCheckType *receiver,
    const LhatCheckType *const *arguments, size_t count,
    const LhatCheckType **resolved)
{
    (void)user;
    (void)receiver;
    lhat_check_transfer_argument(context, 0);
    if (count == 0) return LHAT_INSTANTIATION_DEFAULT;
    if (lhat_check_type_pending(arguments[0])) return LHAT_INSTANTIATION_PENDING;
    for (size_t i = 0; i < lhat_check_type_union_count(arguments[0]); i++) {
        const LhatCheckType *job = lhat_check_type_union_at(arguments[0], i);
        if (lhat_check_type_pending(job)) return LHAT_INSTANTIATION_PENDING;
        const LhatCheckType *answer = lhat_check_coroutine_result(context, job);
        if (answer == NULL) {
            // A subroutine job is called on the task's machine. A yielding
            // one would answer a coroutine there; it is handed over as the
            // call of it instead.
            answer = lhat_check_function_result(context, job);
            if (answer != NULL && !lhat_check_type_pending(answer) &&
                lhat_check_coroutine_result(context, answer) != NULL) {
                return LHAT_INSTANTIATION_REFUSED;
            }
        }
        // The signature does the transformation; this hook only checks the
        // transport's single-slot restriction.
        if (answer == NULL) return LHAT_INSTANTIATION_DEFAULT;
        if (lhat_check_type_pending(answer)) return LHAT_INSTANTIATION_PENDING;
        if (!lhat_check_type_single_slot(answer)) return LHAT_INSTANTIATION_REFUSED;
    }
    (void)signature;
    (void)resolved;
    return LHAT_INSTANTIATION_DEFAULT;
}

#endif
#endif
