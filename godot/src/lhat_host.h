// L^ (lhat) -- what every one of the engine's questions needs first.
//
// Checking a path, checking a buffer the editor has not saved, and running a
// unit are three questions with one answer underneath: a program that reads
// through Godot and has the host's names in it (05 の 8.7). This is that,
// and the rendering of what it has to say.

#ifndef LHAT_GODOT_HOST_H
#define LHAT_GODOT_HOST_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "lhat.h"

namespace godot {
namespace host {

// 05 の 5.1 folds separators away and keeps nothing else, so "res://a/b.lh"
// would come back from the language as "res:/a/b.lh" -- one slash, and no
// longer a path the engine will open. The scheme is therefore taken off
// before the language sees anything and put back when a unit is read, which
// leaves L^ working in a flat, '/'-separated space where a require^ resolves
// the way 5.1 says.
//
// One scheme per program follows from that. Mixing res:// and user:// units
// in one graph is not something a require^ can express here.
struct Units {
    String base;      // "res://" or "user://"
    String path;      // the entry unit, with the scheme taken off
    // What to answer for a path instead of reading it. A map rather than one
    // slot: the world is shared, so several tabs may hold text the file does
    // not have yet, and each unit has to read its own.
    HashMap<String, CharString> held;

    // What program_for registered, for the conversions that carry a handle.
    // 05 の 7.3 makes a host type its declaration, so this belongs to the one
    // program and a value of it never crosses to another.
    const struct Godot *godot;
};


// The entry, split. `hold` makes a path answer the text the editor has rather
// than what is on disk -- which is the whole of what validating an unsaved
// buffer needs, and works for a path with no file behind it at all. The path
// is the engine's, scheme and all; what is kept is the stripped form the
// language asks for.
// What the language calls a path the engine spelt: the scheme taken off, so
// a require^ resolves in 5.1's flat space.
String unit_path(const Units &units, const String &path);

Units units_for(const String &path);
void hold(Units *units, const String &path, const String &text);

// A program reading through `units`, with 8.2's initial names bound. NULL
// when there was no memory. `units` has to outlive it.
LhatProgram *program_for(Units *units);

// What every stage of every unit in the graph reported, one line each.
void diagnostics_into(const LhatProgram *program, PackedStringArray &said);

// The same, as the engine wants them: line, column, message, path.
TypedArray<Dictionary> diagnostics_as_errors(const LhatProgram *program,
                                             const String &path);

// 04 の 11.6改 renders a panic with what the program panicked with; a value
// on its own renders as tostring would write it.
String text_of(LhatValue value);

// One shape for what this side has to say, so that a line the extension
// wrote reads like a line a stage wrote.
String problem(const String &where, const String &what);
String run_problem(LhatMachine *machine, const String &where,
                   const LhatRunResult &ran);

// One level of indentation, as the editor is set to write one. A tab
// wherever there is no editor to ask.
String indentation();

// 03 の 4.2 puts the refusals in the checker, so a compile that stops is a
// hole in it -- and says where, so whoever closes the hole is told.
String compile_failure(const LhatProgram *program, const String &path);

}  // namespace host
}  // namespace godot

#endif  // LHAT_GODOT_HOST_H
