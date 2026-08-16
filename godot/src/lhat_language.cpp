#include "lhat_language.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

#include "lhat.h"
#include "lhat_host.h"
#include "lhat_script.h"

namespace godot {

LhatLanguage *LhatLanguage::singleton = nullptr;

namespace {

// The spellings vscode-extension/syntaxes/lhat.tmLanguage.json groups the
// same way, which is the one place that already had to make this call. The
// language itself keeps no such list: 01 の 2 章 scans every hatted word
// alike and it is the checker that refuses the ones it does not know.
const char *const control_flow_words[] = {
    "if^",     "else^", "elseif^", "elsif^",  "elif^",  "ei^",
    "el^",     "for^",  "when^",   "other^",  "from^",  "to^",
    "downto^", "step^", "in^",     "while^",  "until^", "next^",
    "skip^",   "continue^", "repeat^", "do^", "break^", "return^",
    "yield^",  "yieldall^", "await^", "defer^", "with^", "finally^",
    "try^",    "catch^", "panic^",
};

const char *const other_words[] = {
    "var^",      "let^",     "f^",       "p^",        "op^",
    "def^",      "enum^",    "pack^",    "unpack^",   "module^",
    "public^",   "require^", "import^",  "override^", "overload^",
    "abstract^", "closed^",  "errordef^",
    "number^",   "string^",  "bool^",    "nil^",      "any^",
    "t^",        "type^",    "c^",       "error^",    "Self^",
    "true^",     "false^",
    "and^",      "or^",      "is^",      "isa^",      "as^",
    "this^",     "it^",      "self^",    "super^",    "class^",  "L^",
    "typeof^",   "id^",      "width^",   "autowidth^",
    "prolog^",   "prologue^", "pre^",    "premain^",  "first^",
    "main^",     "last^",    "epilog^",  "epilogue^",
};

}  // namespace

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
    PackedStringArray out;
    for (const char *word : control_flow_words) {
        out.push_back(word);
    }
    for (const char *word : other_words) {
        out.push_back(word);
    }
    return out;
}

bool LhatLanguage::_is_control_flow_keyword(const String &keyword) const
{
    for (const char *word : control_flow_words) {
        if (keyword == word) {
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
    out.push_back("\" \"");
    out.push_back("' '");
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

Dictionary LhatLanguage::_complete_code(const String &code, const String &path,
                                        Object *owner) const
{
    (void)code;
    (void)path;
    (void)owner;
    return Dictionary();
}

Dictionary LhatLanguage::_lookup_code(const String &code, const String &symbol,
                                      const String &path, Object *owner) const
{
    (void)code;
    (void)symbol;
    (void)path;
    (void)owner;
    return Dictionary();
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
