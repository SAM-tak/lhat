// L^ (lhat) -- sample standard library: std.binary (01§11).
//
// Packs a table into bits by a format written once, and reads it back.
// What is packed travels as a string^ (11§2), or in a std.binary.Bytes
// a program keeps and writes over, so that a frame's worth of encoding makes
// no new string.
//
// One function a host calls once, before lhat_program_check (05§8.7).
// Calling it again on the same program is harmless.

#ifndef LHATSTDLIB_BINARY_H
#define LHATSTDLIB_BINARY_H

#include <stdint.h>

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_binary_register(LhatProgram *program);

// Schema-free, framed table data. No front end is required. Callbacks consume
// their buffers synchronously; read answers 0 bytes only at EOF. Streams stay
// open. A failed write may have emitted an incomplete record.
typedef enum {
    LHAT_BINARY_DATA_OK,
    LHAT_BINARY_DATA_TRUNCATED,
    LHAT_BINARY_DATA_MALFORMED,
    LHAT_BINARY_DATA_UNSUPPORTED,
    LHAT_BINARY_DATA_CYCLE,
    LHAT_BINARY_DATA_LIMIT,
    LHAT_BINARY_DATA_IO,
    LHAT_BINARY_DATA_OUT_OF_MEMORY
} LhatBinaryDataStatus;

typedef bool (*LhatBinaryWriteFn)(void *context, const uint8_t *data, size_t length);
typedef bool (*LhatBinaryReadFn)(void *context, uint8_t *data, size_t capacity,
                                 size_t *length);
typedef struct {
    size_t max_bytes;   // complete framed record, including header
    size_t max_entries; // sum of array positions and dictionary entries
    size_t max_depth;   // at most 96; NULL limits use 256 MiB / 1M / 96
} LhatBinaryDataLimits;

// The source must remain unchanged during serialization, including callbacks.
// Deserialization roots partially built objects across callbacks; on failure
// *out is nil. On success, root the answer before running the machine again.
LhatBinaryDataStatus lhatstdlib_binary_serialize(
    LhatMachine *machine, LhatValue table, LhatBinaryWriteFn write, void *context,
    const LhatBinaryDataLimits *limits);
LhatBinaryDataStatus lhatstdlib_binary_deserialize(
    LhatMachine *machine, LhatBinaryReadFn read, void *context,
    const LhatBinaryDataLimits *limits, LhatValue *out);

// A reusable buffer for native Reader/Writer implementations. The module must
// be registered first. Size is both initial length and minimum capacity.
bool lhatstdlib_binary_make_bytes(LhatMachine *machine, size_t size, LhatValue *out);

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
