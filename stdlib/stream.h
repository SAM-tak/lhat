// Synchronous byte Readers/Writers. Structural contracts, no common base type.
#ifndef LHATSTDLIB_STREAM_H
#define LHATSTDLIB_STREAM_H
#include "lhat.h"
#ifdef __cplusplus
extern "C" {
#endif
// Registers std.binary as a dependency. reader(string|Bytes) copies its input;
// writer() accumulates output. Neither factory references borrowed byte storage.
bool lhatstdlib_stream_register(LhatProgram *program);
#ifdef __cplusplus
}
#endif
#endif
