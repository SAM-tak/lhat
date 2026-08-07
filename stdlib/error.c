// L^ (lhat) -- sample standard library: std.error.
//
// See error.h for why this exists and why lhatstdlib_error_register has to
// be idempotent.

#include "error.h"

static const char *const variants[] = {"OutOfMemory"};

bool lhatstdlib_error_register(LhatProgram *program)
{
    if (lhatstdlib_error_lookup(program, "OutOfMemory") != NULL) {
        return true;  // already registered by an earlier dependent
    }
    const LhatErrorKind *kinds[1];
    return lhat_register_error_kind(program, "std", "error", variants, 1,
                                    NULL, kinds);
}

const LhatErrorKind *lhatstdlib_error_lookup(const LhatProgram *program,
                                             const char *variant)
{
    return lhat_lookup_error_kind(program, "std", "error", variant);
}
