// L^ (lhat) -- values crossing between L^ and Variant.
//
// **Never interleave this with running.** A collection happens inside the
// interpreter loop and nowhere else (src/gc.c's roots are L^, the frames and
// the pending disposals -- a host holds no root). So a value built here is
// safe exactly as long as nothing runs between building it and handing it
// over: convert every argument, then call, then convert the answer.

#ifndef LHAT_GODOT_VARIANT_H
#define LHAT_GODOT_VARIANT_H

#include <godot_cpp/variant/variant.hpp>

#include "lhat.h"

namespace godot {
namespace host {

// What L^ answered, as the engine holds it. A subroutine, a coroutine, an
// error and a host value have nothing to land in and answer nil -- as does
// anything nested deeper than a conversion will follow.
Variant to_variant(LhatValue value);

// The other way. False when the Variant has no shape in L^ (a Callable, a
// Node, a Vector2 -- 05 の 8.8 and 8.9 are where those will land) or when
// the machine ran out of memory.
bool from_variant(LhatMachine *machine, const Variant &value, LhatValue *out);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_VARIANT_H
