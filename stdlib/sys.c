// L^ (lhat) -- sample standard library: std.sys.
//
// See sys.h for why this exists and why lhatstdlib_sys_register has to
// be idempotent.

#include "sys.h"

static const char *const variants[] = {"OutOfMemory"};

bool lhatstdlib_sys_register(LhatProgram *program)
{
    if (lhatstdlib_sys_lookup(program, "OutOfMemory") != NULL) {
        return true;  // already registered by an earlier dependent
    }
    const LhatErrorKind *kinds[1];
    return lhat_register_error_kind(program, "std.sys", "Error", variants, 1,
                                    NULL, kinds);
}

const LhatErrorKind *lhatstdlib_sys_lookup(const LhatProgram *program,
                                             const char *variant)
{
    return lhat_lookup_error_kind(program, "std.sys", "Error", variant);
}
