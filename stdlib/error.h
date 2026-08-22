// L^ (lhat) -- sample standard library: std.error.
//
// Error kinds a host might otherwise be tempted to redeclare per module --
// std.io.IOError.OutOfMemory, std.thread.ThreadError.OutOfMemory, and so on
// -- live here once instead, as std.error.*. lhatstdlib_io_register and its
// siblings call lhatstdlib_error_register themselves before using
// std.error.OutOfMemory, so a host that links this module never has to call
// it directly.
//
// lhatstdlib_error_register is idempotent: more than one stdlib module
// depends on it, so it is called once per dependent's own registration, and
// lhat_register_error_kind (05 の 8.7's "one name, one thing") answers
// false on a second attempt. Checking first with lhatstdlib_error_lookup
// and returning early is the idiom every stdlib registration function that
// more than one other module depends on should follow.

#ifndef LHATSTDLIB_ERROR_H
#define LHATSTDLIB_ERROR_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_error_register(LhatProgram *program);

// The kind std.error.<variant> was registered under, or NULL if
// lhatstdlib_error_register was never called (or has no such variant).
const LhatErrorKind *lhatstdlib_error_lookup(const LhatProgram *program,
                                             const char *variant);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_ERROR_H
