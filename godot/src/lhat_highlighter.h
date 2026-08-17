// L^ (lhat) -- what colours a .lh file in the script editor.
//
// The editor's own highlighter is built from what a language answers about
// its words and delimiters (lhat_language.cpp), and that is as far as a word
// list can reach: 01 の 2.3 makes the hat part of the name, and the editor
// ends a word before one. So `def^` is coloured as `def` plus a symbol, and a
// bare `def` -- a different name, and a legal one -- is coloured as though it
// were the keyword.
//
// Reading the text here instead settles both. It also puts L^ in the Syntax
// Highlighter menu, which is what makes it the one chosen for a .lh rather
// than plain text.
//
// Editor-only: registered at MODULE_INITIALIZATION_LEVEL_EDITOR and handed to
// the ScriptEditor, neither of which exists in an exported game.

#ifndef LHAT_GODOT_HIGHLIGHTER_H
#define LHAT_GODOT_HIGHLIGHTER_H

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "lhat/semantic.h"

#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class LhatHighlighter : public EditorSyntaxHighlighter {
    GDCLASS(LhatHighlighter, EditorSyntaxHighlighter)

    // Read from the editor's theme, so a light setting and a dark one each
    // look like everything else in the window. Refreshed by _update_cache.
    struct Palette {
        Color plain;
        Color keyword;
        Color control;
        // The hatted words the lexer sees as one kind, which a reader does
        // not: a type, a value, and a clause each get the colour the editor
        // already draws that idea in for GDScript.
        Color type;
        Color value;
        Color clause;
        // 07 の 4 章's layer: what only a check can tell apart. A class the
        // writer declared, a module reached through, a name that turned out
        // to be called.
        Color klass;
        Color space;
        Color function;
        Color property;
        // 13.4's parameters. Made from the text colour rather than read from
        // a key: the editor names no colour for one.
        Color parameter;
        Color number;
        Color string;
        Color comment;
        Color annotation;
        Color symbol;
        // 01 の 2.3: the hat is part of the name, so it is not punctuation
        // between things -- it is a mark on one. Drawn the way the editor
        // draws the marks it puts on whitespace: the text colour, faded until
        // it is there to be looked at rather than read.
        Color hat;
    } palette;

public:
    // A run of the text drawn in one colour. Public only because the scan in
    // the .cpp fills these; nothing outside that file has any use for one.
    struct Span {
        int kind;        // the .cpp's SpanKind
        int64_t offset;  // byte into the scanned text
        int64_t length;
    };

private:
    // One scan for the whole text, kept until the text moves. The editor asks
    // a line at a time and asks again on every redraw, so scanning per line
    // would read the file once per visible line. Mutable because the answer
    // is asked for through a const method.
    mutable String scanned;
    mutable Vector<Span> spans;
    mutable PackedInt64Array line_starts;  // byte offset of each line

    // 07 の 4 章: what the checker made of each name, in offset order. The
    // second layer -- `Godot.Sprite2D` is spelled exactly as `self^.ticks`,
    // and only a check tells them apart. Empty while the buffer does not
    // check, which is most of the time somebody is typing, and the lexical
    // layer carries the colouring alone until it does.
    mutable Vector<LhatSemanticName> meanings;

    void refresh(const String &text) const;
    void read_meanings(const String &text) const;
    const LhatSemanticName *meaning_at(int64_t offset) const;
    Color colour_of(int kind, const char *word, size_t length,
                    int64_t offset) const;
    static int32_t column_of(const char *text, int64_t line_start, int64_t at);

protected:
    static void _bind_methods();

public:
    // What the Syntax Highlighter menu shows, and which language it claims.
    // The language name is LhatLanguage::_get_name's, byte for byte -- that
    // match is the whole of how the editor pairs the two.
    String _get_name() const override;
    PackedStringArray _get_supported_languages() const override;
    Ref<EditorSyntaxHighlighter> _create() const override;

    void _update_cache() override;
    void _clear_highlighting_cache() override;
    Dictionary _get_line_syntax_highlighting(int32_t line) const override;
};

// What hands the highlighter to the script editor.
//
// Not the module's initialiser: MODULE_INITIALIZATION_LEVEL_EDITOR is reached
// while the editor is still being built, and the ScriptEditor a highlighter
// is registered with does not exist yet. A plugin's _enter_tree runs once it
// does, which is the earliest the registration can be made.
class LhatEditorPlugin : public EditorPlugin {
    GDCLASS(LhatEditorPlugin, EditorPlugin)

    Ref<LhatHighlighter> highlighter;

protected:
    static void _bind_methods();

public:
    void _enter_tree() override;
    void _exit_tree() override;
};

}  // namespace godot

#endif  // LHAT_GODOT_HIGHLIGHTER_H
