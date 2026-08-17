#include "lhat_highlighter.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
// lhat_language.h's _make_template answers a Ref<Script>, and a Ref needs the
// whole of what it holds to be instantiated.
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/text_edit.hpp>

#include "lhat.h"
#include "lhat_language.h"

// Where a word begins and ends is the lexer's answer and nobody else's: 01 の
// 2.3 makes the hat part of the name, 6.2's block comments nest, 5.2's raw
// strings write a quote by doubling it, 5.4's interpolations hold code. None
// of it is reachable with a regular expression, so this reads the text with
// the same lexer the compiler does. Not reached by lhat.h, which is the
// header for running a program -- this is the one for reading one.
#include "lhat/lexer.h"

namespace godot {

void LhatHighlighter::_bind_methods()
{
}

String LhatHighlighter::_get_name() const
{
    return LhatLanguage::get_singleton() != nullptr
               ? LhatLanguage::get_singleton()->_get_name()
               : String("L^");
}

// The editor pairs a highlighter with a script by matching this against the
// language's own name, so the two are read from one place.
PackedStringArray LhatHighlighter::_get_supported_languages() const
{
    PackedStringArray out;
    out.push_back(_get_name());
    return out;
}

Ref<EditorSyntaxHighlighter> LhatHighlighter::_create() const
{
    Ref<LhatHighlighter> made;
    made.instantiate();
    return made;
}

namespace {

// What the text is made of, as coarsely as a colour needs it. Coarser than
// the lexer on purpose: the parser's distinctions are not the ones a reader
// sees.
//
// Every hatted word is a name here. 01 の 2.1 reserves nothing -- the lexer
// answers one token kind for all of them, the parser asks at each place a
// word may stand, and a spelling that means nothing is refused by the
// checker. Telling `def^` from 14.17's `tostring^` is a guess from the
// spelling, and lhat_language.h is where this host makes it.
enum SpanKind {
    SPAN_OTHER,  // whitespace, and anything not worth a colour
    SPAN_NAME,   // an identifier, hatted or not
    SPAN_NUMBER,
    SPAN_STRING,      // 01 の 5 章, the interpolated form included
    SPAN_COMMENT,     // 01 の 6 章
    SPAN_ANNOTATION,  // 02 の 18.2: '@' and the name glued to it
    SPAN_OPERATOR     // 01 の 7 章
};

SpanKind span_kind_of(const LhatToken *token)
{
    switch (token->kind) {
        case LHAT_TOKEN_HAT_IDENT:
        case LHAT_TOKEN_IDENT:
        case LHAT_TOKEN_SCOPE:
            return SPAN_NAME;
        case LHAT_TOKEN_ANNOTATION:
            return SPAN_ANNOTATION;
        case LHAT_TOKEN_INT:
        case LHAT_TOKEN_FLOAT:
            return SPAN_NUMBER;
        // 01 の 5.4: an interpolation is a string with holes in it. The holes
        // are ordinary expressions and answer for themselves; the rest of it
        // -- the quotes, the runs between, the format after ':' -- is string.
        case LHAT_TOKEN_STRING:
        case LHAT_TOKEN_INTERP_BEGIN:
        case LHAT_TOKEN_INTERP_TEXT:
        case LHAT_TOKEN_INTERP_FORMAT:
        case LHAT_TOKEN_INTERP_END:
            return SPAN_STRING;
        case LHAT_TOKEN_OP:
        case LHAT_TOKEN_INTERP_EXPR_BEGIN:
        case LHAT_TOKEN_INTERP_EXPR_END:
            return SPAN_OPERATOR;
        default:
            return SPAN_OTHER;
    }
}

// The editor's own colours, so a light theme and a dark one each look like
// the rest of the window. Asked for by the same keys GDScript's highlighting
// is drawn with.
Color themed(const String &key, const Color &fallback)
{
    // Asking for the editor where there is none is an error the engine
    // reports, so the question is asked the way that has an answer either way.
    if (!Engine::get_singleton()->is_editor_hint()) {
        return fallback;
    }
    EditorInterface *editor = EditorInterface::get_singleton();
    if (editor == nullptr) {
        return fallback;
    }
    Ref<EditorSettings> settings = editor->get_editor_settings();
    if (settings.is_null()) {
        return fallback;
    }
    Variant held =
        settings->get_setting("text_editor/theme/highlighting/" + key);
    return held.get_type() == Variant::COLOR ? (Color)held : fallback;
}

// Nothing to colour is left out rather than written down: a gap says "plain"
// more cheaply than an entry saying so.
void put_span(Vector<LhatHighlighter::Span> &into, SpanKind kind,
              int64_t offset, int64_t length)
{
    if (length <= 0 || kind == SPAN_OTHER) {
        return;
    }
    LhatHighlighter::Span made;
    made.kind = kind;
    made.offset = offset;
    made.length = length;
    into.push_back(made);
}

// Whether a '^' ending the span is 01 の 2.3's hat -- a mark on a name --
// rather than a character that happens to be last. Only a word wears one; a
// string or a comment can end in the character and mean nothing by it.
bool wears_a_hat(int kind)
{
    return kind == SPAN_NAME;
}

}  // namespace

void LhatHighlighter::_update_cache()
{
    palette.plain = themed("text_color", Color(0.9f, 0.9f, 0.9f));
    palette.keyword = themed("keyword_color", Color(1.0f, 0.44f, 0.52f));
    palette.control =
        themed("control_flow_keyword_color", Color(1.0f, 0.55f, 0.8f));
    // The keys the editor already draws these ideas with, so a .lh reads the
    // way the .gd beside it does and one theme settles both.
    palette.type = themed("base_type_color", Color(0.26f, 1.0f, 0.76f));
    palette.value =
        themed("member_variable_color", Color(0.736f, 0.88f, 1.0f));
    palette.clause = themed("function_color", Color(0.34f, 0.7f, 1.0f));
    palette.number = themed("number_color", Color(0.63f, 1.0f, 0.88f));
    palette.string = themed("string_color", Color(1.0f, 0.93f, 0.63f));
    palette.comment = themed("comment_color", Color(0.8f, 0.81f, 0.83f, 0.5f));
    palette.symbol = themed("symbol_color", Color(0.67f, 0.79f, 1.0f));
    // 02 の 18: an annotation is neither a name nor a keyword -- it is
    // information for the host. Godot draws GDScript's the same way, so the
    // same key is asked for.
    palette.annotation =
        themed("gdscript/annotation_color", Color(1.0f, 0.7f, 0.45f));
    // Written out rather than derived from the theme. A middle grey at half
    // alpha settles toward whatever is behind it -- dark on a dark ground,
    // light on a light one -- so the hat is equally quiet either way, which
    // is the whole of what is wanted of it. Taking the text colour and fading
    // it would instead follow the theme's hue, and there is no reason a hat
    // should be pink where a keyword is.
    palette.hat = Color(0.5f, 0.5f, 0.5f, 0.5f);
    scanned = String();  // the theme changed, and so may the text have
    spans.clear();
    line_starts.clear();
}

void LhatHighlighter::_clear_highlighting_cache()
{
    scanned = String();
    spans.clear();
    line_starts.clear();
}

// One scan for the whole text, kept until the text moves. The editor asks a
// line at a time and asks again on every redraw, so scanning per line would
// read the file once per visible line.
void LhatHighlighter::refresh(const String &text) const
{
    if (text == scanned && !line_starts.is_empty()) {
        return;
    }
    scanned = text;
    spans.clear();
    line_starts.clear();

    CharString whole = text.utf8();
    const char *bytes = whole.get_data();
    size_t length = (size_t)whole.length();

    // Where each line begins, in the bytes a span's offset is measured in.
    // Built once here rather than counted per call.
    line_starts.push_back(0);
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == '\n') {
            line_starts.push_back((int64_t)(i + 1));
        }
    }

    LhatSource source;
    if (!lhat_source_init_from_string(&source, "", bytes, length)) {
        return;
    }
    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);

    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
        // A token the lexer could not make says where the trouble is, and an
        // editor colours on either side of it. Reading on is what lets a
        // buffer somebody is typing into still be coloured.
        if (token.kind == LHAT_TOKEN_ERROR) {
            continue;
        }
        int64_t offset = token.offset;
        int64_t span = token.length;
        // 02 の 18.2: '@' and the name glued to it are one token, and the
        // lexer records the name alone because that is what the parser reads.
        // A reader sees the mark, so the span takes the byte back.
        if (token.kind == LHAT_TOKEN_ANNOTATION && offset > 0) {
            offset--;
            span++;
        }
        put_span(spans, span_kind_of(&token), offset, span);
    }

#if LHAT_WITH_COMMENTS
    // 01 の 6.4: comments are not tokens, so they arrive on the side rather
    // than in the stream -- which is why what was collected is in two runs
    // and has to be put in one order below.
    for (size_t i = 0; i < lexer.comment_count; i++) {
        const LhatComment *comment = &lexer.comments[i];
        put_span(spans, SPAN_COMMENT, comment->offset,
                 (int64_t)comment->end - (int64_t)comment->offset);
    }
#endif

    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);

    // A caller reading a line at a time wants one order.
    for (int64_t i = 1; i < spans.size(); i++) {
        Span moving = spans[i];
        int64_t j = i;
        while (j > 0 && spans[j - 1].offset > moving.offset) {
            spans.write[j] = spans[j - 1];
            j--;
        }
        spans.write[j] = moving;
    }
}

// A span's offset is a byte into the text; the editor's columns count
// characters. The two part company on any line holding something outside
// ASCII -- a comment written in Japanese, a string with an accent -- and a
// colour placed at a byte would land in the middle of a word.
//
// Counted rather than kept: a line is short, and this runs once per span.
int32_t LhatHighlighter::column_of(const char *text, int64_t line_start,
                                   int64_t at)
{
    int32_t column = 0;
    for (int64_t i = line_start; i < at; i++) {
        // Every byte of a UTF-8 sequence but the first has 10 in its top
        // bits, so counting the others counts characters.
        if ((text[i] & 0xC0) != 0x80) {
            column++;
        }
    }
    return column;
}

// `word` is the whole span as written, which for a name is the whole word --
// the line it falls on has not been taken into account yet, and must not be:
// what a word is decides its colour, and half a word decides nothing.
Color LhatHighlighter::colour_of(int kind, const char *word,
                                 size_t length) const
{
    switch ((SpanKind)kind) {
        case SPAN_NUMBER:     return palette.number;
        case SPAN_STRING:     return palette.string;
        case SPAN_COMMENT:    return palette.comment;
        case SPAN_ANNOTATION: return palette.annotation;
        case SPAN_OPERATOR:   return palette.symbol;
        case SPAN_NAME:       break;
        default:              return palette.plain;
    }

    // 01 の 2.1 reserves no word, so the scan calls every name a name and the
    // guess about which spellings are the language's is made here. See
    // lhat_language.h for what that guess costs.
    switch (lhat_word_kind(word, length)) {
        case LHAT_WORD_CONTROL: return palette.control;
        case LHAT_WORD_DECLARE: return palette.keyword;
        case LHAT_WORD_TYPE:    return palette.type;
        case LHAT_WORD_VALUE:   return palette.value;
        case LHAT_WORD_CLAUSE:  return palette.clause;
        default:                return palette.plain;
    }
}

// 07 の 4 章: where a word begins and ends is the scan's answer, and this
// only picks the colour it is drawn in.
//
// The editor reads an entry as "this colour from this column on", so a run
// ends by writing the plain colour where it stops. A construct that spans
// lines (01 の 6.2's nested comments, 5.1's strings) is one span, and the
// part of it falling on this line is what is coloured here.
Dictionary LhatHighlighter::_get_line_syntax_highlighting(int32_t line) const
{
    Dictionary out;
    TextEdit *text_edit = get_text_edit();
    if (text_edit == nullptr) {
        return out;
    }
    refresh(text_edit->get_text());
    if (line < 0 || line >= line_starts.size()) {
        return out;
    }

    CharString whole = scanned.utf8();
    const char *text = whole.get_data();
    int64_t line_start = line_starts[line];
    int64_t line_end = line + 1 < line_starts.size()
                           ? line_starts[line + 1] - 1  // before the newline
                           : (int64_t)whole.length();

    Dictionary plain;
    plain["color"] = palette.plain;
    out[0] = plain;

    for (int64_t i = 0; i < spans.size(); i++) {
        int64_t begin = spans[i].offset;
        int64_t end = begin + spans[i].length;
        if (end <= line_start || begin >= line_end) {
            continue;  // some other line's
        }

        // Settled from the whole span, before the clipping below cuts it to
        // this line. A name never crosses a newline, but reading the colour
        // off a clipped `begin` would be true only by accident.
        Color drawn =
            colour_of(spans[i].kind, text + begin, (size_t)(end - begin));

        // 01 の 2.3: the hat is part of the name, which is why the scan
        // answers one span. What a reader wants of it is the other half of
        // that sentence -- the word is the word, and the hat is the mark
        // saying so. The tmLanguage grammar splits it the same way
        // (punctuation.definition.hat), so the two editors read alike.
        int64_t hats = end;
        if (wears_a_hat(spans[i].kind)) {
            while (hats > begin && text[hats - 1] == '^') {
                hats--;
            }
        }

        if (begin < line_start) {
            begin = line_start;  // a construct that started above
        }
        if (end > line_end) {
            end = line_end;
        }
        if (hats < begin) {
            hats = begin;
        }
        if (hats > end) {
            hats = end;
        }

        if (hats > begin) {
            Dictionary at;
            at["color"] = drawn;
            out[column_of(text, line_start, begin)] = at;
        }
        if (hats < end) {
            Dictionary mark;
            mark["color"] = palette.hat;
            out[column_of(text, line_start, hats)] = mark;
        }

        // Where the run stops, unless the next one starts there -- it is
        // written in order, so a later entry at the same column wins.
        Dictionary back;
        back["color"] = palette.plain;
        out[column_of(text, line_start, end)] = back;
    }
    return out;
}

// ---------------------------------------------------------------------------

void LhatEditorPlugin::_bind_methods()
{
}

void LhatEditorPlugin::_enter_tree()
{
    ScriptEditor *scripts = EditorInterface::get_singleton()->get_script_editor();
    if (scripts == nullptr) {
        return;
    }
    highlighter.instantiate();
    scripts->register_syntax_highlighter(highlighter);
}

void LhatEditorPlugin::_exit_tree()
{
    if (highlighter.is_null()) {
        return;
    }
    ScriptEditor *scripts = EditorInterface::get_singleton()->get_script_editor();
    if (scripts != nullptr) {
        scripts->unregister_syntax_highlighter(highlighter);
    }
    highlighter.unref();
}

}  // namespace godot
