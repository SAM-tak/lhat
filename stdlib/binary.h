// L^ (lhat) -- sample standard library: std.binary (11 章).
//
// Packs a table into bits by a format written once, and reads it back.
// What is packed travels as a string^ (11 の 2), or in a std.binary.Bytes
// a program keeps and writes over, so that a frame's worth of encoding makes
// no new string.
//
// One function a host calls once, before lhat_program_check (05 の 8.7).
// Calling it again on the same program is harmless.

#ifndef LHATSTDLIB_BINARY_H
#define LHATSTDLIB_BINARY_H

#include <stdint.h>

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_binary_register(LhatProgram *program);

// What a std.binary.Bytes holds: `length` bytes at `data`, in room for
// `capacity`. Code outside std.binary reads and writes the bytes and the
// length, and leaves the room to `resize`.
typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} LhatBinaryBytes;

// How C that std.binary did not build -- std.net, a native extension --
// reaches a Bytes: lhat_lookup_host_context(program, "std.binary", NULL,
// "bytes") answers this when the module is registered on the program, and
// NULL when it is not, which is the cue to register no Bytes overloads.
// The functions are the process's and outlive every program.
#define LHAT_BINARY_INTERFACE_VERSION 1u
typedef struct {
    uint32_t version;
    // The Bytes behind `value`, or NULL when it is not one or was disposed.
    LhatBinaryBytes *(*bytes)(LhatValue value);
    // Makes room for `length` bytes and sets the length to it; what was
    // there is not kept. False when there is no memory.
    bool (*resize)(LhatBinaryBytes *bytes, size_t length);
} LhatBinaryInterface;

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_BINARY_H
