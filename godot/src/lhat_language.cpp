#include "lhat_language.h"

#include <string.h>

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
    (void)validate_functions;
    (void)validate_warnings;
    (void)validate_safe_lines;

    Dictionary out;

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

bool LhatLanguage::_supports_documentation() const
{
    return false;
}

bool LhatLanguage::_handles_global_class_type(const String &type) const
{
    (void)type;
    return false;
}

Dictionary LhatLanguage::_get_global_class_name(const String &path) const
{
    (void)path;
    return Dictionary();
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

int32_t LhatLanguage::_find_function(const String &function,
                                     const String &code) const
{
    (void)function;
    (void)code;
    return -1;
}

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
    return false;
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

void LhatLanguage::_reload_all_scripts()
{
}

void LhatLanguage::_reload_scripts(const Array &scripts, bool soft_reload)
{
    (void)scripts;
    (void)soft_reload;
}

void LhatLanguage::_reload_tool_script(const Ref<Script> &script,
                                       bool soft_reload)
{
    (void)script;
    (void)soft_reload;
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
