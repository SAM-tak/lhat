// L^ (lhat) -- sample standard library: std.binary (11 章).
//
// Packs a table into bits by a format written once, and reads it back.
// What is packed travels as a string^ (11 の 2), or in a std.binary.Bytes
// a program keeps and writes over, so that a frame's worth of encoding makes
// no new string.
//
// One function a host calls once, before lhat_program_check (05 の 8.7).
// Calling it again on the same program is harmless: std.net calls it for
// the Bytes it sends from.

#ifndef LHATSTDLIB_BINARY_H
#define LHATSTDLIB_BINARY_H

#include <stdint.h>

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_binary_register(LhatProgram *program);

// What a std.binary.Bytes holds: `length` bytes at `data`, in room for
// `capacity`. std.net reads and fills one through these.
typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} LhatBinaryBytes;

// The Bytes behind `value`, or NULL when it is not one or was disposed.
// Answers NULL before lhatstdlib_binary_register has run.
LhatBinaryBytes *lhatstdlib_binary_bytes(LhatValue value);

// Makes room for `length` bytes and sets the length to it; what was there
// is not kept. False when there is no memory.
bool lhatstdlib_binary_bytes_resize(LhatBinaryBytes *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_BINARY_H
