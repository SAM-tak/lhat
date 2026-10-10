// L^ (lhat) -- sample standard library: std.net (11 章).
//
// UDP: a std.net.Udp socket that sends a string^ or a std.binary.Bytes and
// receives without ever waiting. Registering it registers std.binary too,
// for the Bytes it reads and fills.
//
// One function a host calls once, before lhat_program_check (05 の 8.7).
// Built when LHAT_BUILD_STDLIB_NET is on.

#ifndef LHATSTDLIB_NET_H
#define LHATSTDLIB_NET_H

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_net_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_NET_H
