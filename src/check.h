// L^ (lhat) -- the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". This is the third stage of 03 の 1.1: it walks the tree the
// parser produced, resolves written annotations into types, infers the rest,
// and reports what does not fit.
//
// It never rewrites the tree. 03 の 4.2 requires the shape of the tree and
// the meaning at run time to be the same whatever the strictness, so the
// setting only decides which failures are reported here rather than deferred
// to a runtime check.

#ifndef LHAT_CHECK_H
#define LHAT_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "source.h"
#include "type.h"

typedef enum {
    LHAT_CHECK_ERR_NONE,

    LHAT_CHECK_ERR_UNDEFINED,           // no such name in scope
    LHAT_CHECK_ERR_USED_BEFORE_DEFINED, // 8.7
    LHAT_CHECK_ERR_REDEFINED,           // 8.7: twice in one scope
    LHAT_CHECK_ERR_UNKNOWN_TYPE,        // an annotation names nothing

    LHAT_CHECK_ERR_MISMATCH,            // 13.11: the value does not fit
    LHAT_CHECK_ERR_NOT_NUMBER,          // arithmetic on something else
    LHAT_CHECK_ERR_NOT_BOOL,            // a condition, and^ / or^ / '!'
    LHAT_CHECK_ERR_NOT_CALLABLE,
    LHAT_CHECK_ERR_ARITY,               // too few or too many arguments
    LHAT_CHECK_ERR_NO_MEMBER,           // 14.10: the structure lacks it

    LHAT_CHECK_ERR_CANNOT_FAIL,         // 04 の 4.1: catch^ on what cannot fail
    LHAT_CHECK_ERR_CANNOT_BE_NIL,       // 11.7, the same for '??'
    LHAT_CHECK_ERR_TRY_OUTSIDE,         // 04 の 5.3
    LHAT_CHECK_ERR_RECURSION_NEEDS_TYPE,// 03 の 3.4
    LHAT_CHECK_ERR_NOT_DISPOSABLE,      // 12.5: with^ needs a dispose()

    LHAT_CHECK_ERR_MEMBER_EXISTS,       // 14.12: same name, no marker
    LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, // a marker with nothing under it
    LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,   // 14.12: override^ has to fit
    LHAT_CHECK_ERR_OVERLOAD_OVERLAPS    // 14.12: signatures must stay apart
} LhatCheckErrorCode;

typedef struct {
    LhatCheckErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;
} LhatCheckDiagnostic;

typedef struct {
    LhatTypeArena types;

    LhatCheckDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
} LhatCheckResult;

// 03 の 3.1. `strict` is a setting of the compilation unit, not a dialect:
// 3.2 keeps the source text identical either way, and 3.5 limits what
// relaxed defers to places where inference could not decide.
//
// `source` has to be the text the tree was parsed from, since names are held
// as spans into it.
void lhat_check(const LhatNode *unit, const LhatSource *source, bool strict,
                LhatCheckResult *result);

void lhat_check_result_dispose(LhatCheckResult *result);

const char *lhat_check_error_message(LhatCheckErrorCode code);

#endif  // LHAT_CHECK_H
