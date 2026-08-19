#include "lhat_language.h"

#include <godot_cpp/templates/local_vector.hpp>

#include <string.h>

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/script_language_extension_profiling_info.hpp>
#include <godot_cpp/core/memory.hpp>

#include "lhat.h"
#include "lhat_host.h"
#include "lhat_script.h"

namespace godot {

LhatLanguage *LhatLanguage::singleton = nullptr;

namespace {

// The groups vscode-extension/syntaxes/lhat.tmLanguage.json already sorts
// these into, kept alike so the two editors read alike. Written with the hat,
// since that is the spelling: what strips it is unhatted() below, and only
// for the one caller that needs it stripped.
const char *const control_flow_words[] = {
    "if^",     "else^", "elseif^", "elsif^",  "elif^",  "ei^",
    "el^",     "for^",  "when^",   "other^",  "from^",  "to^",
    "downto^", "step^", "in^",     "while^",  "until^", "next^",
    "skip^",   "continue^", "repeat^", "do^", "break^", "return^",
    "yield^",  "yieldall^", "await^", "defer^", "with^", "finally^",
    "try^",    "catch^", "panic^",  nullptr,
};

// What declares, and the operators 01 の 7 章 spells as words. true^ and
// false^ are here rather than among the values below: they are constants, and
// a theme telling the two apart draws a constant with its keywords.
//
// type^ appears in no design document. Left where it has always been rather
// than moved on a guess about what it would be.
const char *const declaring_words[] = {
    "var^",      "let^",     "f^",       "p^",        "op^",
    "def^",      "enum^",    "pack^",    "unpack^",   "module^",
    "public^",   "require^", "import^",  "override^", "overload^",
    "abstract^", "closed^",  "errordef^",
    "true^",     "false^",
    "and^",      "or^",      "is^",      "isa^",      "as^",     nullptr,
};

// Self^ is a type where self^ is a value: 01 の 2.3 compares a hat word byte
// for byte, so the capital is a different name and belongs to another list.
const char *const type_words[] = {
    "number^", "string^", "bool^",  "nil^",  "any^",
    "t^",      "c^",      "error^", "Self^", nullptr,
};

// The hat words that name a value rather than syntax. 05 の 8.6's L^ names
// the machine; the rest name what the definition around them is applied to.
const char *const value_words[] = {
    "this^", "it^", "self^", "super^", "class^", "L^", nullptr,
};

// 02 の 9.2's clause words, each with the long spelling the parser also takes.
const char *const clause_words[] = {
    "typeof^",   "id^",      "width^",   "autowidth^", "prolog^",
    "prologue^", "pre^",     "premain^", "first^",     "main^",
    "last^",     "epilog^",  "epilogue^", nullptr,
};

bool word_among(const char *const *list, const char *text, size_t length)
{
    for (size_t i = 0; list[i] != nullptr; i++) {
        if (strlen(list[i]) == length && memcmp(list[i], text, length) == 0) {
            return true;
        }
    }
    return false;
}

// The word the editor's own CodeHighlighter can match: the spelling above
// without its hat. That highlighter ends a word at the first character an
// identifier is not made of, and '^' is not one -- so a word registered with
// the hat is compared against text that can never contain it and never
// matches. Measured: 'def^' registered against the line 'def^ x' colours
// nothing, 'def' colours the three characters before the hat.
//
// 01 の 2.3 makes the hat part of the name, so this is not the name; it is
// the run of characters that highlighter will have in hand. What it costs is
// a bare 'def' -- a different name, and a legal one -- coloured as the
// keyword. Only the fallback pays it: LhatHighlighter reads whole words and
// does not go through here.
String unhatted(const char *word)
{
    String spelt = String::utf8(word);
    return spelt.ends_with("^") ? spelt.substr(0, spelt.length() - 1) : spelt;
}

}  // namespace

const char *const *lhat_words_of(LhatWordKind kind)
{
    switch (kind) {
        case LHAT_WORD_CONTROL: return control_flow_words;
        case LHAT_WORD_DECLARE: return declaring_words;
        case LHAT_WORD_TYPE:    return type_words;
        case LHAT_WORD_VALUE:   return value_words;
        case LHAT_WORD_CLAUSE:  return clause_words;
        default:                return nullptr;
    }
}

LhatWordKind lhat_word_kind(const char *text, size_t length)
{
    // 01 の 2.3: the hat is part of the name, and a spelling with more than
    // one is that same name reached further out -- so the word to compare is
    // the letters plus a single hat.
    size_t hats = 0;
    while (hats < length && text[length - 1 - hats] == '^') {
        hats++;
    }
    if (hats == 0) {
        return LHAT_WORD_NONE;  // no hat, so none of these
    }
    size_t canonical = length - hats + 1;

    static const LhatWordKind kinds[] = {LHAT_WORD_CONTROL, LHAT_WORD_DECLARE,
                                         LHAT_WORD_TYPE, LHAT_WORD_VALUE,
                                         LHAT_WORD_CLAUSE};
    for (LhatWordKind kind : kinds) {
        if (word_among(lhat_words_of(kind), text, canonical)) {
            return kind;
        }
    }
    return LHAT_WORD_NONE;
}

LhatLanguage::LhatLanguage()
{
    singleton = this;
}

LhatLanguage::~LhatLanguage()
{
    if (singleton == this) {
        singleton = nullptr;
    }
}

LhatLanguage *LhatLanguage::get_singleton()
{
    return singleton;
}

void LhatLanguage::_bind_methods()
{
}

String LhatLanguage::_get_name() const
{
    return "L^";
}

String LhatLanguage::_get_type() const
{
    return "LhatScript";
}

String LhatLanguage::_get_extension() const
{
    return "lh";
}

PackedStringArray LhatLanguage::_get_recognized_extensions() const
{
    PackedStringArray out;
    out.push_back("lh");
    return out;
}

void LhatLanguage::_init()
{
}

void LhatLanguage::_finish()
{
}

Dictionary LhatLanguage::_validate(const String &script, const String &path,
                                   bool validate_functions,
                                   bool validate_errors,
                                   bool validate_warnings,
                                   bool validate_safe_lines) const
{
    (void)validate_warnings;
    (void)validate_safe_lines;

    Dictionary out;
    // The members overview beside the code. Read off the text, so it is the
    // buffer's lines that are listed -- and answered whether or not what
    // follows finds the unit sound, since ScriptTextEditor::get_functions
    // takes the list only when the whole answer says valid anyway.
    if (validate_functions) {
        out["functions"] = lhat_subroutine_lines(script);
    }

    // The text, not the file -- the editor asks while the buffer is unsaved,
    // and for a script whose file does not exist yet.
    host::Units units = host::units_for(path);
    host::hold(&units, script);

    LhatProgram *program = host::program_for(&units);
    if (program == nullptr) {
        out["valid"] = false;
        return out;
    }

    const LhatUnit *root =
        lhat_program_check(program, units.path.utf8().get_data());
    bool ok = root != nullptr && !lhat_program_has_errors(program);
    out["valid"] = ok;
    if (!ok && validate_errors) {
        out["errors"] = host::diagnostics_as_errors(program, path);
    }
    lhat_program_free(program);
    return out;
}

// Empty means the path will do. 5.1 takes a path as written, so there is
// nothing here to refuse.
String LhatLanguage::_validate_path(const String &path) const
{
    (void)path;
    return String();
}

Object *LhatLanguage::_create_script() const
{
    return memnew(LhatScript);
}

Ref<Script> LhatLanguage::_make_template(const String &tmpl,
                                         const String &class_name,
                                         const String &base_class_name) const
{
    (void)tmpl;
    (void)class_name;
    (void)base_class_name;

    Ref<LhatScript> script;
    script.instantiate();
    // String::utf8, not String: the one-argument constructor reads its bytes
    // as Latin-1, and a section number written 05 の 5.5 comes out mangled.
    script->set_source_code(String::utf8(
        "# A unit is a body, not a class (05 の 5.5): what is written here\n"
        "# runs top to bottom when the unit is run.\n"
        "\n"
        "print(\"hello from L^\")\n"));
    return script;
}

bool LhatLanguage::_is_using_templates()
{
    return false;
}

PackedStringArray LhatLanguage::_get_reserved_words() const
{
    static const LhatWordKind kinds[] = {LHAT_WORD_CONTROL, LHAT_WORD_DECLARE,
                                         LHAT_WORD_TYPE, LHAT_WORD_VALUE,
                                         LHAT_WORD_CLAUSE};
    PackedStringArray out;
    for (LhatWordKind kind : kinds) {
        for (const char *const *word = lhat_words_of(kind); *word != nullptr;
             word++) {
            out.push_back(unhatted(*word));
        }
    }
    return out;
}

// Asked of what the list above answered, so it is asked without the hat too.
bool LhatLanguage::_is_control_flow_keyword(const String &keyword) const
{
    for (const char *const *word = lhat_words_of(LHAT_WORD_CONTROL);
         *word != nullptr; word++) {
        if (keyword == unhatted(*word)) {
            return true;
        }
    }
    return false;
}

PackedStringArray LhatLanguage::_get_comment_delimiters() const
{
    PackedStringArray out;
    out.push_back("#");
    out.push_back("#[ ]#");  // start and end, separated by a space
    return out;
}

PackedStringArray LhatLanguage::_get_doc_comment_delimiters() const
{
    return PackedStringArray();
}

PackedStringArray LhatLanguage::_get_string_delimiters() const
{
    PackedStringArray out;
    // 01 の 5.3: '"""' runs to the end of the line and writes no terminator,
    // so the end is empty. Measured: the editor takes the longest opening it
    // finds whatever order these arrive in, so this does not have to lead.
    out.push_back("\"\"\" ");
    out.push_back("\" \"");
    out.push_back("' '");
    // 01 の 3.1: a name written in backticks. Not a string, but delimited the
    // same way and worth telling from the code around it.
    out.push_back("` `");
    return out;
}

bool LhatLanguage::_has_named_classes() const
{
    return false;
}

bool LhatLanguage::_can_inherit_from_file() const
{
    return false;
}

// 05 の 5.1 makes a unit a file. There is no form for one kept inside a
// scene, so the dialog's "built-in script" stays greyed out.
bool LhatLanguage::_supports_builtin_mode() const
{
    return false;
}

// 01 の 6.4: L^ has no spelling for a description -- every comment is kept
// alike and the block above a thing is what it says about it, the way Go has
// it. So there is documentation to show, and this is the gate: a language
// answering false is never asked for any.
bool LhatLanguage::_supports_documentation() const
{
    return true;
}

// Asked before the editor will use _get_global_class_name at all: a language
// answering false is never scanned for the names it declares. The type is
// the base a caller is asking about, and what is answered is about this
// language rather than about that type -- a .lh names a class the same way
// whatever it is asked in aid of.
bool LhatLanguage::_handles_global_class_type(const String &type) const
{
    return type == _get_type();
}

// Asked of a path rather than of a loaded script: the editor scans the
// project for what each file declares before it opens any of them, and this
// is what puts a class in the Create Node dialog and hangs @icon's icon on
// it.
//
// A .lh has to be read and run to answer, since 05 の 5.5 has the classes a
// unit publishes come from the table it answers with -- there is no reading
// the tree for them. So one script is made, reloaded and thrown away. That
// is what GDScript's own scan costs too, a parse per file.
Dictionary LhatLanguage::_get_global_class_name(const String &path) const
{
    Dictionary out;
    Ref<FileAccess> reading = FileAccess::open(path, FileAccess::READ);
    if (reading.is_null()) {
        return out;
    }

    Ref<LhatScript> probe;
    probe.instantiate();
    // The path before the source: a require^ resolves against it, and the
    // reload below is what follows one. Written past the resource cache --
    // set_path would claim a path the real script may already hold, and two
    // resources answering one path is what that refusal is for.
    probe->set_path_cache(path);
    probe->_set_source_code(reading->get_as_text());
    probe->_reload(false);

    StringName named = probe->_get_global_name();
    if (named == StringName()) {
        return out;  // a library, or a name the engine already has
    }
    out["name"] = named;
    out["base_type"] = probe->_get_instance_base_type();
    String icon = probe->lhat_icon_path();
    if (!icon.is_empty()) {
        out["icon_path"] = icon;
    }
    return out;
}

ScriptLanguage::ScriptNameCasing LhatLanguage::_preferred_file_name_casing()
    const
{
    return ScriptLanguage::SCRIPT_NAME_CASING_SNAKE_CASE;
}

// Both of these answer a dictionary the engine takes apart, and the key it
// reads first is "result". An empty one is not "nothing to offer" but a
// malformed answer: script_language_extension.h refuses it and logs, which
// the editor does on every keystroke a completion could follow.
//
// So the shape is written out, with the answer being that nothing was found.
// 07 の lsp/ already knows how to complete a name; _complete_code is where
// that would be hooked up, and until it is this is what "no" looks like.
Dictionary LhatLanguage::_complete_code(const String &code, const String &path,
                                        Object *owner) const
{
    (void)code;
    (void)path;
    (void)owner;
    Dictionary out;
    out["result"] = (int)OK;
    out["force"] = false;
    out["call_hint"] = String();
    out["options"] = Array();
    return out;
}

Dictionary LhatLanguage::_lookup_code(const String &code, const String &symbol,
                                      const String &path, Object *owner) const
{
    (void)code;
    (void)symbol;
    (void)path;
    (void)owner;
    Dictionary out;
    out["result"] = (int)ERR_UNAVAILABLE;  // nothing to jump to, not an error
    // Read whether the result said anything was found, so it has to be there
    // even when nothing was.
    out["type"] = (int)LOOKUP_RESULT_MAX;
    return out;
}

String LhatLanguage::_auto_indent_code(const String &code, int32_t from_line,
                                       int32_t to_line) const
{
    (void)from_line;
    (void)to_line;
    return code;
}

namespace {

// The name a line binds, and what it binds it to. One pass over the text,
// which is what both the outline and the jump-to-member want.
//
// Read off the text rather than the tree, for two reasons. The line numbers
// have to agree with the buffer being shown, which the tree's do not while a
// unit is being edited; and the editor asks with the buffer in hand rather
// than the file, sometimes for a script that has never checked.
struct Bound {
    String name;
    String value;  // what stands after the '=' or ':=', stripped
    bool found = false;
};

// 8.6 and 14.12: what may lead a name before it is the name. The introducers
// take a space or none (01 の 2.3 makes the hat part of the word, so
// `public^let^Spinner` is three words with nothing between them), and the
// three markers of 14.12 stand in the same place on a member.
const char *const leading[] = {"public^",   "let^",      "var^",
                               "override^", "overload^", "abstract^"};

// Where the name ends. 01 の 2.3 keeps the hat in, and everything else that
// can follow a name closes it.
bool ends_name(char32_t c)
{
    return c == ' ' || c == '\t' || c == '=' || c == ':' || c == ',' ||
           c == '(' || c == '{' || c == '#';
}

Bound bound_on(const String &line)
{
    Bound out;
    String bare = line.strip_edges();
    // 02 の 18.6: an annotation is written above the declaration it is about,
    // and "above" is as often the same line. It is not the name, so it comes
    // off -- with its arguments, which 18.3 keeps to literals and so to one
    // pair of brackets.
    while (bare.begins_with("@")) {
        int at = 1;
        while (at < bare.length() && !ends_name(bare[at])) {
            at++;
        }
        if (at < bare.length() && bare[at] == '(') {
            int depth = 0;
            while (at < bare.length()) {
                if (bare[at] == '(') {
                    depth++;
                } else if (bare[at] == ')' && --depth == 0) {
                    at++;
                    break;
                }
                at++;
            }
        }
        String left = bare.substr(at).strip_edges();
        if (left == bare) {
            return out;  // nothing came off; an annotation and nothing else
        }
        bare = left;
    }
    bool again = true;
    while (again) {
        again = false;
        for (const char *word : leading) {
            if (bare.begins_with(word)) {
                bare = bare.substr((int)strlen(word)).strip_edges();
                again = true;
                break;
            }
        }
    }

    int at = 0;
    while (at < bare.length() && !ends_name(bare[at])) {
        at++;
    }
    if (at == 0) {
        return out;
    }
    String name = bare.substr(0, at);
    String after = bare.substr(at).strip_edges();
    // 14.14改2: '=' is the recommended spelling and ':=' reads the same.
    if (after.begins_with(":=")) {
        out.value = after.substr(2).strip_edges();
    } else if (after.begins_with("=")) {
        out.value = after.substr(1).strip_edges();
    } else {
        return out;
    }
    out.name = name;
    out.found = true;
    return out;
}

}  // namespace

// Which line writes this member, 1-based, or -1. What a jump-to-member lands
// on, and what the editor asks when it looks for a function by name.
int32_t lhat_member_line(const String &code, const String &member)
{
    PackedStringArray lines = code.split("\n");
    for (int64_t i = 0; i < lines.size(); i++) {
        Bound said = bound_on(lines[i]);
        if (said.found && said.name == member) {
            return (int32_t)(i + 1);
        }
    }
    return -1;
}

// What the script editor lists down the side of the code (its members
// overview), as "name:line" with the line 1-based -- the shape
// ScriptLanguageExtension::validate reads out of the "functions" key.
//
// Subroutines only, which is the same choice GDScript makes: its outline is
// the file's func declarations. A def^ member bound to anything else is a
// field or a static constant (14.7改), and the inspector is where those are
// read.
PackedStringArray lhat_subroutine_lines(const String &code)
{
    PackedStringArray out;
    PackedStringArray lines = code.split("\n");
    for (int64_t i = 0; i < lines.size(); i++) {
        Bound said = bound_on(lines[i]);
        if (said.found &&
            (said.value.begins_with("p^") || said.value.begins_with("f^"))) {
            out.push_back(said.name + ":" + String::num_int64(i + 1));
        }
    }
    return out;
}

int32_t LhatLanguage::_find_function(const String &function,
                                     const String &code) const
{
    return lhat_member_line(code, function);
}

// Nothing, on purpose, and the only place in the engine that asks is
// script_text_editor.cpp's add_callback -- which puts the answer at the very
// end of the file and nowhere else. A member written there falls outside the
// def^'s closing brace, so what would come back from here could not be right
// wherever it were put. C# reached the same conclusion
// (csharp_script.cpp: "The make_function() API does not work for C#
// scripts") and stopped at refusing the feature.
//
// This goes one further: _can_make_function stays true, so the connect
// dialog does not warn about something that is in fact done, and what the
// editor appends is the two newlines around this empty answer -- which
// leaves the file it saves a moment later sound. The member itself is
// written by lhat_receiver_write, in the body, from the plugin.
String LhatLanguage::_make_function(const String &class_name,
                                    const String &function_name,
                                    const PackedStringArray &args) const
{
    (void)class_name;
    (void)function_name;
    (void)args;
    return String();
}

bool LhatLanguage::_can_make_function() const
{
    return true;
}

bool LhatLanguage::_overrides_external_editor()
{
    return false;
}

void LhatLanguage::_add_global_constant(const StringName &name,
                                        const Variant &value)
{
    (void)name;
    (void)value;
}

void LhatLanguage::_add_named_global_constant(const StringName &name,
                                              const Variant &value)
{
    (void)name;
    (void)value;
}

void LhatLanguage::_remove_named_global_constant(const StringName &name)
{
    (void)name;
}

void LhatLanguage::_thread_enter()
{
}

void LhatLanguage::_thread_exit()
{
}

void LhatLanguage::_frame()
{
}

// The three ways the engine asks for a reload. Script::reload_from_file no
// longer reads the file itself -- it hands the question here -- so reading
// it is part of the answer (lhat_script.cpp's reload_from_disk).

// A .lh changed on disk while nothing named it. Every one this program holds
// is asked, since there is no telling which the change was.
void LhatLanguage::_reload_all_scripts()
{
    LocalVector<Ref<Script>> held;
    for (LhatScript *const &script : LhatScript::all()) {
        held.push_back(Ref<Script>(script));
    }
    for (const Ref<Script> &script : held) {
        Object::cast_to<LhatScript>(script.ptr())->reload_from_disk(false);
    }
}

void LhatLanguage::_reload_scripts(const Array &scripts, bool soft_reload)
{
    for (int64_t i = 0; i < scripts.size(); i++) {
        Ref<Script> held = scripts[i];
        LhatScript *ours = Object::cast_to<LhatScript>(held.ptr());
        if (ours != nullptr) {
            ours->reload_from_disk(soft_reload);
        }
    }
}

// 02 の 18: what a @tool class carries is that it runs while a scene is being
// edited, so this is the same question asked while it is running.
void LhatLanguage::_reload_tool_script(const Ref<Script> &script,
                                       bool soft_reload)
{
    LhatScript *ours = Object::cast_to<LhatScript>(script.ptr());
    if (ours != nullptr) {
        ours->reload_from_disk(soft_reload);
    }
}

TypedArray<Dictionary> LhatLanguage::_get_public_functions() const
{
    return TypedArray<Dictionary>();
}

Dictionary LhatLanguage::_get_public_constants() const
{
    return Dictionary();
}

TypedArray<Dictionary> LhatLanguage::_get_public_annotations() const
{
    return TypedArray<Dictionary>();
}

void LhatLanguage::_profiling_start()
{
}

void LhatLanguage::_profiling_stop()
{
}

void LhatLanguage::_profiling_set_save_native_calls(bool enable)
{
    (void)enable;
}

int32_t LhatLanguage::_profiling_get_accumulated_data(
    ScriptLanguageExtensionProfilingInfo *info, int32_t max)
{
    (void)info;
    (void)max;
    return 0;
}

int32_t LhatLanguage::_profiling_get_frame_data(
    ScriptLanguageExtensionProfilingInfo *info, int32_t max)
{
    (void)info;
    (void)max;
    return 0;
}

String LhatLanguage::_debug_get_error() const
{
    return String();
}

int32_t LhatLanguage::_debug_get_stack_level_count() const
{
    return 0;
}

int32_t LhatLanguage::_debug_get_stack_level_line(int32_t level) const
{
    (void)level;
    return 0;
}

String LhatLanguage::_debug_get_stack_level_function(int32_t level) const
{
    (void)level;
    return String();
}

String LhatLanguage::_debug_get_stack_level_source(int32_t level) const
{
    (void)level;
    return String();
}

Dictionary LhatLanguage::_debug_get_stack_level_locals(int32_t level,
                                                       int32_t max_subitems,
                                                       int32_t max_depth)
{
    (void)level;
    (void)max_subitems;
    (void)max_depth;
    return Dictionary();
}

Dictionary LhatLanguage::_debug_get_stack_level_members(int32_t level,
                                                        int32_t max_subitems,
                                                        int32_t max_depth)
{
    (void)level;
    (void)max_subitems;
    (void)max_depth;
    return Dictionary();
}

void *LhatLanguage::_debug_get_stack_level_instance(int32_t level)
{
    (void)level;
    return nullptr;
}

Dictionary LhatLanguage::_debug_get_globals(int32_t max_subitems,
                                            int32_t max_depth)
{
    (void)max_subitems;
    (void)max_depth;
    return Dictionary();
}

TypedArray<Dictionary> LhatLanguage::_debug_get_current_stack_info()
{
    return TypedArray<Dictionary>();
}

}  // namespace godot
