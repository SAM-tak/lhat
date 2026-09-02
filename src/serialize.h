// L^ (lhat) -- a compiled unit written out as bytes and read back.
//
// 05 の 10 章. Internal to the library: program.c reads a unit through
// this when the loader hands it bytes that begin with the magic, and
// lhat_unit_write_binary (program.h) is the host's way in to the writer.

#ifndef LHAT_SERIALIZE_H
#define LHAT_SERIALIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "program_internal.h"

// Whether what a loader handed over is a binary unit rather than text. The
// magic begins with a byte no UTF-8 text can start on, so a source file is
// never mistaken for one.
bool lhat_serialize_is_binary(const char *bytes, size_t length);

// Writes a checked and compiled unit. `out` is lhat_alloc'd and the
// caller's to free. False when the unit has no compiled body, or holds
// something the format cannot carry (a pointer no name finds again).
bool lhat_serialize_write(const LhatUnit *unit, bool with_debug_names,
                          uint8_t **out, size_t *length);

// What a read answers: the root body, the module name (owned, or NULL),
// and the export descriptors a host asks for (8.7's lhat_unit_export_*),
// as parallel arrays the unit takes over.
typedef struct {
    LhatProto *proto;
    char *module_name;
    char **export_names;
    struct LhatRuntimeType **export_rt;
    size_t export_count;
} LhatBinaryUnit;

// Reads a binary unit for `unit` (its path set, its state CHECKING): the
// referenced units are required first, the identities resolved against the
// program, and the tree built. False reports the failure on the program
// and leaves nothing behind.
bool lhat_serialize_load(LhatProgram *program, LhatUnit *unit,
                         const uint8_t *bytes, size_t length,
                         LhatBinaryUnit *out);

// 10.7: the signature table. Writes one record per distinct signature text
// the program's registrations carry -- the descriptor install builds for
// the entry -- under the same header a unit has.
bool lhat_serialize_write_signatures(const LhatProgram *program,
                                     uint8_t **out, size_t *length);

// Checks the header of a table and lists its records, sorted by text. The
// index points into `bytes`, which the caller keeps for as long as it does.
// False (with the reason reported on the program) when the bytes are not a
// table this build wrote.
bool lhat_serialize_index_signatures(LhatProgram *program,
                                     const uint8_t *bytes, size_t length,
                                     LhatSignatureIndex **out_index,
                                     size_t *out_count);

// Decodes one record's body onto the program's host heap. NULL when a name
// in it is not registered on this program (HOST_MISMATCH reported) or the
// record is damaged.
const LhatRuntimeType *lhat_serialize_read_signature(LhatProgram *program,
                                                     const uint8_t *body,
                                                     size_t length);

#endif  // LHAT_SERIALIZE_H
