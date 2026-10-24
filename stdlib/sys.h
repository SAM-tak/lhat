// L^ (lhat) -- sample standard library: std.sys.
//
// Error kinds a host might otherwise be tempted to redeclare per module --
// std.io.IOError.OutOfMemory, std.thread.ThreadError.OutOfMemory, and so on
// -- live here once instead, as std.sys.Error.*. lhatstdlib_io_register and its
// siblings call lhatstdlib_sys_register themselves before using
// std.sys.Error.OutOfMemory, so a host that links this module never has to call
// it directly.
//
// lhatstdlib_sys_register is idempotent: more than one stdlib module
// depends on it, so it is called once per dependent's own registration, and
// lhat_register_error_kind (05 の 8.7's "one name, one thing") answers
// false on a second attempt. Checking first with lhatstdlib_sys_lookup
// and returning early is the idiom every stdlib registration function that
// more than one other module depends on should follow.

#ifndef LHATSTDLIB_SYS_H
#define LHATSTDLIB_SYS_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_sys_register(LhatProgram *program);

// The kind std.sys.Error.<variant> was registered under, or NULL if
// lhatstdlib_sys_register was never called (or has no such variant).
const LhatErrorKind *lhatstdlib_sys_lookup(const LhatProgram *program,
                                             const char *variant);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_SYS_H
