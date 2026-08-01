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
#include "lexer.h"
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
    LHAT_CHECK_ERR_NEVER_RETURNS,       // 03 の 3.4: every way out of the body
                                        // goes through the subroutine itself
    LHAT_CHECK_ERR_FUNCTION_FALLS_OUT,  // 13.2: an f^ that can reach its end
    LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT, // the value-less exit does not fit the
                                        // result that was written
    LHAT_CHECK_ERR_NOT_DISPOSABLE,      // 12.5: with^ needs a dispose()

    LHAT_CHECK_ERR_MEMBER_EXISTS,       // 14.12: same name, no marker
    LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, // a marker with nothing under it
    LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,   // 14.12: override^ has to fit
    LHAT_CHECK_ERR_OVERLOAD_OVERLAPS,   // 14.12: signatures must stay apart
    LHAT_CHECK_ERR_INCOMPARABLE,        // no value inhabits both sides
    LHAT_CHECK_ERR_MISSING_FIELD,       // 04 の 2.5: no default to fall back to
    LHAT_CHECK_ERR_REQUIRE_FAILED,      // 05 の 6 章: the unit could not be had
    LHAT_CHECK_ERR_COROUTINE_DROPPED,   // 15.8: a call that makes a coroutine
                                        // and does nothing with it
    LHAT_CHECK_ERR_NOT_COROUTINE        // 15.8: yieldall^ needs one
} LhatCheckErrorCode;

typedef struct {
    LhatCheckErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;
} LhatCheckDiagnostic;

// 05 の 6 章: a unit's exports are types that the units requiring it hold on
// to, so the arena has to outlive any one result. The caller may pass its own
// and share it across a whole program; without one, the result keeps its own.
typedef struct {
    LhatTypeArena *types;
    LhatTypeArena owned;

    LhatCheckDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

    // 05 の 4 章: the structure of what this unit publishes, or NULL when it
    // publishes nothing. What a require^ of it yields.
    LhatType *exports;
} LhatCheckResult;

// 05 の 5 章. Asked for the unit at `path`, relative to whatever the resolver
// considers the requiring unit. Returns its export structure, or NULL when it
// could not be had -- a missing file or a cycle (6.3), which the resolver
// reports in its own terms.
typedef LhatType *(*LhatRequireResolver)(void *context, const char *path,
                                         size_t length);

typedef struct {
    LhatRequireResolver resolve;
    void *context;
} LhatRequire;

// 03 の 3.1. `strict` is a setting of the compilation unit, not a dialect:
// 3.2 keeps the source text identical either way, and 3.5 limits what
// relaxed defers to places where inference could not decide.
//
// `lexer` has to be the one the tree was parsed from: names are spans into
// its source and strings are spans into its decoded storage.
void lhat_check(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                LhatCheckResult *result);

// The same, for a unit that is part of a program. `arena` is shared so the
// types this unit publishes stay valid in the units that require it, and
// `require` is how those imports are answered. Either may be NULL.
void lhat_check_unit(const LhatNode *unit, const LhatLexer *lexer,
                     bool strict, LhatTypeArena *arena,
                     const LhatRequire *require, LhatCheckResult *result);

void lhat_check_result_dispose(LhatCheckResult *result);

const char *lhat_check_error_message(LhatCheckErrorCode code);

#endif  // LHAT_CHECK_H
