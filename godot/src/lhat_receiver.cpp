#include "lhat_receiver.h"

#include <string.h>

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>

#include "lhat/lexer.h"
#include "lhat/source.h"
#include "lhat/token.h"
#include "lhat_script.h"

namespace godot {
namespace {

// One level of indentation, as the editor is set to write one.
String indentation()
{
    EditorInterface *editor = Engine::get_singleton()->is_editor_hint()
                                  ? EditorInterface::get_singleton()
                                  : nullptr;
    Ref<EditorSettings> settings = editor != nullptr
                                       ? editor->get_editor_settings()
                                       : Ref<EditorSettings>();
    if (settings.is_null()) {
        return "\t";
    }
    // 0 is Tabs, 1 is Spaces -- the enum the setting is declared with.
    if ((int)settings->get_setting("text_editor/behavior/indent/type") == 0) {
        return "\t";
    }
    int wide = (int)settings->get_setting("text_editor/behavior/indent/size");
    return String(" ").repeat(wide > 0 ? wide : 4);
}

// An argument arrives as "name:Type" -- Godot's type, which is not L^'s. The
// three the two share are carried over and everything else is any^: 03 の 3.1
// has the checker act on what is written, so a name L^ knows nothing about
// would have to be deleted before the file checked.
String lhat_type_of(const String &godot_type)
{
    if (godot_type == "int" || godot_type == "float") {
        return "number^";
    }
    if (godot_type == "String" || godot_type == "StringName") {
        return "string^";
    }
    if (godot_type == "bool") {
        return "bool^";
    }
    return "any^";
}

// The bytes of a token, as the source holds them.
bool token_is(const char *text, const LhatToken &token, const char *spelling)
{
    size_t length = strlen(spelling);
    return token.length == length &&
           memcmp(text + token.offset, spelling, length) == 0;
}

bool token_is(const char *text, const LhatToken &token, const String &spelling)
{
    CharString bytes = spelling.utf8();
    return token.length == (uint32_t)bytes.length() &&
           memcmp(text + token.offset, bytes.get_data(),
                  (size_t)bytes.length()) == 0;
}

bool is_op(const LhatToken &token, LhatOpKind op)
{
    return token.kind == LHAT_TOKEN_OP && token.v.op == op;
}

// A byte offset as CodeEdit counts places: lines from 0, and columns in
// characters rather than bytes -- what a caret is set with.
void place_of(const char *text, size_t length, uint32_t offset, int *line,
              int *column)
{
    int at_line = 0;
    int at_column = 0;
    for (size_t i = 0; i < length && i < offset; i++) {
        if (text[i] == '\n') {
            at_line++;
            at_column = 0;
            continue;
        }
        // A continuation byte is the middle of a character, not one of its
        // own -- the caret counts characters (01 の 1.2 keeps the source
        // UTF-8).
        if ((text[i] & 0xC0) != 0x80) {
            at_column++;
        }
    }
    *line = at_line;
    *column = at_column;
}

}  // namespace

String lhat_receiver_text(const String &function,
                          const PackedStringArray &args)
{
    String tab = indentation();
    String out = tab + function + " = p^self^";
    for (int64_t i = 0; i < args.size(); i++) {
        String named = args[i].get_slice(":", 0);
        String typed = args[i].get_slice_count(":") > 1
                           ? args[i].get_slice(":", 1)
                           : String();
        out += ", " + named;
        if (!typed.is_empty()) {
            out += ":" + lhat_type_of(typed);
        }
    }
    // A line in for the body, so there is somewhere for the caret to land.
    out += " {\n" + tab + tab + "\n" + tab + "},";
    return out;
}

bool lhat_receiver_place(const String &code, const String &klass, int *line,
                         int *column, bool *needs_comma)
{
    CharString bytes = code.utf8();
    const char *text = bytes.get_data();
    size_t length = (size_t)bytes.length();
    if (text == NULL || klass.is_empty()) {
        return false;
    }

    LhatSource source;
    if (!lhat_source_init_from_string(&source, "", text, length)) {
        return false;
    }
    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);

    // 05 の 5.5 puts the class in a top-level binding, so its name is written
    // where nothing is open. Counting the depth is what keeps the same
    // spelling inside another body from being taken for it.
    int depth = 0;
    bool named = false;   // the binding's name has gone by
    bool opened = false;  // and so has the '{' of its def^
    int body_depth = 0;
    uint32_t last_end = 0;   // just past the last token inside the body
    bool last_was_comma = false;
    bool last_was_open = false;
    bool found = false;

    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
        if (token.kind == LHAT_TOKEN_ERROR) {
            continue;  // a buffer being typed into is still worth reading
        }

        if (opened) {
            if (is_op(token, LHAT_OP_RBRACE) && depth == body_depth + 1) {
                found = true;
                break;
            }
            // The last thing written before the brace is where a member may
            // follow, and whether it is a comma says if one has to be added.
            if (depth == body_depth + 1) {
                last_was_comma = is_op(token, LHAT_OP_COMMA);
                last_was_open = false;
            }
            last_end = token.offset + token.length;
        }

        if (is_op(token, LHAT_OP_LBRACE)) {
            if (named && !opened) {
                opened = true;
                body_depth = depth;
                last_end = token.offset + token.length;
                last_was_comma = false;
                last_was_open = true;
            }
            depth++;
            continue;
        }
        if (is_op(token, LHAT_OP_RBRACE)) {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        // 14.5: the class is often written as a composition, so the def^ is
        // not always the next thing after the name -- what is looked for is
        // the first body opened after it, whatever stands between.
        if (!named && depth == 0 && token.kind == LHAT_TOKEN_IDENT &&
            token_is(text, token, klass)) {
            named = true;
        }
    }

    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);

    if (!found) {
        return false;
    }
    place_of(text, length, last_end, line, column);
    // An empty body needs no comma: there is nothing for one to follow.
    *needs_comma = !last_was_comma && !last_was_open;
    return true;
}

void lhat_receiver_write(LhatScript *script, const String &function,
                         const PackedStringArray &args)
{
    EditorInterface *editor = EditorInterface::get_singleton();
    if (script == nullptr || editor == nullptr) {
        return;
    }
    // Open it first: what is written goes into the buffer the writer is
    // looking at, and the tab has to be that one.
    editor->edit_script(Ref<Script>(script));

    ScriptEditor *scripts = editor->get_script_editor();
    ScriptEditorBase *shown =
        scripts != nullptr ? scripts->get_current_editor() : nullptr;
    CodeEdit *edit =
        shown != nullptr ? Object::cast_to<CodeEdit>(shown->get_base_editor())
                         : nullptr;
    if (edit == nullptr || scripts->get_current_script() != Ref<Script>(script)) {
        return;  // the editor is showing something else
    }

    int line = 0;
    int column = 0;
    bool needs_comma = false;
    if (!lhat_receiver_place(edit->get_text(), script->lhat_class_name(), &line,
                             &column, &needs_comma)) {
        return;
    }

    String written = lhat_receiver_text(function, args);
    edit->begin_complex_operation();
    edit->remove_secondary_carets();
    edit->insert_text((needs_comma ? String(",") : String()) + "\n" + written,
                      line, column);
    edit->end_complex_operation();

    // The body is the second of the three lines written, and the caret goes
    // where the writing does.
    editor->edit_script(Ref<Script>(script), line + 2,
                        (int)indentation().length() * 2);
}

}  // namespace godot
