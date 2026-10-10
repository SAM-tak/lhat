// L^ (lhat) -- sample standard library: std.net (11 章).
//
// UDP: a std.net.Udp socket that sends and receives without ever waiting.
// It carries string^; when std.binary is registered on the program first,
// send, sendTo and receiveInto also take a std.binary.Bytes (11 の 2.1).
//
// One function a host calls once, before lhat_program_check (05 の 8.7),
// after lhatstdlib_binary_register when it wants the Bytes overloads.
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
