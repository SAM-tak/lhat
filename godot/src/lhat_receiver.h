// L^ (lhat) -- where a signal's receiver goes, and who writes it.
//
// Godot's connect dialog asks for a receiver, and the editor puts what the
// language hands back at the very end of the file -- the position is written
// into script_text_editor.cpp's add_callback and no language has a say in
// it. For a language whose class body is braced that lands outside the
// closing brace. C# met the same wall and gave the feature up
// (csharp_script.cpp: "It will always append the generated function at the
// very end of the script").
//
// So the extension writes the member itself, in the body where it belongs.
// The language's _make_function is left answering nothing, which is what
// makes the editor's own insertion two blank lines rather than a member in
// the wrong place -- so the file it saves a moment later is still sound.

#ifndef LHAT_GODOT_RECEIVER_H
#define LHAT_GODOT_RECEIVER_H

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class LhatScript;

// 02 の 14.3's member, written as one: a name bound to a p^ taking self^,
// with the arguments the signal carries and a line left in for the body.
// Three lines, indented as the editor is set to indent, with no newline at
// either end -- the caller decides what it is joined to.
String lhat_receiver_text(const String &function,
                          const PackedStringArray &args);

// Where a member may be added to the body of the definition `klass` holds:
// just past the last thing written in it, counted the way CodeEdit counts
// lines and columns. Answers false where nothing says -- a buffer somebody
// is still typing into, or a file whose class is written elsewhere.
//
// `needs_comma` says whether what stands there ends without one. 14.13
// allows a trailing comma but does not require it, so a body written without
// one needs it added before another member can follow.
//
// Read with the lexer rather than by looking for a brace: a '}' inside a
// string or a comment is not one (01 の 5 章 and 6 章), and the composition
// a class is often written as puts braces in the way besides.
bool lhat_receiver_place(const String &code, const String &klass, int *line,
                         int *column, bool *needs_comma);

// Writes the receiver into the editor's buffer and puts the caret in its
// body. Does nothing where the editor is showing something else or where the
// body cannot be found -- what a writer has half-typed is theirs, and the
// worst thing to do to it is to edit it wrongly.
void lhat_receiver_write(LhatScript *script, const String &function,
                         const PackedStringArray &args);

}  // namespace godot

#endif  // LHAT_GODOT_RECEIVER_H
