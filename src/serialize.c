// L^ (lhat) -- a compiled unit written out as bytes and read back.
//
// 05 の 10 章. What a proto tree holds is the machine's own: instructions
// whose operands are indices, constants, descriptors. The only pointers that
// leave a chunk go to things a name finds again -- another unit, a host type
// or kind the program registered, an enum declaration -- and those travel as
// names, resolved against the program that reads them. Everything else is
// copied as it stands. Nothing here reads the checker: a binary unit is the
// VM's alone, which is what lets a build without the front end run one.
//
// The bytes are trusted (a version stamp and a hash, no verifier): a file
// this did not write is refused, and one it did is run as the compiler left
// it.

#include "serialize.h"

#include <string.h>

#include "lhat/config.h"
#include "lhat/version.h"
#include "ast.h"
#include "code.h"
#include "grow.h"
#include "hosted.h"
#include "registry.h"
#include "type.h"

// 0x89 is a UTF-8 continuation byte, which no text begins with.
static const uint8_t MAGIC[4] = { 0x89, 'L', 'H', '^' };
#define FORMAT_VERSION 1u
#define FLAG_DEBUG_NAMES 1u
#define FLAG_STRICT 2u
#define HEADER_BYTES 24u  // magic, format, flags, fingerprint, hash

// ---------------------------------------------------------------------------
// The one hash: FNV-1a 64, as program.c's hash_text
// ---------------------------------------------------------------------------

static uint64_t fnv_step(uint64_t hash, const void *bytes, size_t length)
{
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < length; i++) {
        hash ^= p[i];
        hash *= 1099511628211u;
    }
    return hash;
}

#define FNV_BASIS 1469598103934665603u

// What the instruction stream was laid out against: the register and tuple
// limits (config.h) shape frames, the opcode table shapes the code, and the
// library version stands for everything else. A file from another build is
// refused rather than read.
static uint64_t fingerprint(void)
{
    uint64_t hash = FNV_BASIS;
    uint32_t words[] = { LHAT_MAX_REGISTERS, LHAT_MAX_TUPLE,
                         LHAT_HOSTVALUE_MAX_BYTES, LHAT_BC_COUNT,
                         (uint32_t)sizeof(LhatInstruction) };
    hash = fnv_step(hash, words, sizeof words);
    hash = fnv_step(hash, LHAT_VERSION, strlen(LHAT_VERSION));
    return hash;
}

bool lhat_serialize_is_binary(const char *bytes, size_t length)
{
    return bytes != NULL && length >= sizeof MAGIC &&
           memcmp(bytes, MAGIC, sizeof MAGIC) == 0;
}

// ---------------------------------------------------------------------------
// Bytes out
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    bool failed;
} Out;

static void put_bytes(Out *o, const void *bytes, size_t length)
{
    if (o->failed) {
        return;
    }
    if (o->length + length > o->capacity) {
        size_t wanted = o->capacity ? o->capacity : 256;
        while (wanted < o->length + length) {
            wanted *= 2;
        }
        uint8_t *bigger = (uint8_t *)lhat_realloc(o->data, wanted);
        if (bigger == NULL) {
            o->failed = true;
            return;
        }
        o->data = bigger;
        o->capacity = wanted;
    }
    if (length > 0) {
        memcpy(o->data + o->length, bytes, length);
    }
    o->length += length;
}

static void put_u8(Out *o, uint8_t v) { put_bytes(o, &v, 1); }

static void put_u16(Out *o, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(o, b, 2);
}

static void put_u32(Out *o, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    put_bytes(o, b, 4);
}

static void put_u64(Out *o, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    put_bytes(o, b, 8);
}

static void put_text(Out *o, const char *text, size_t length)
{
    put_u32(o, (uint32_t)length);
    put_bytes(o, text, length);
}

// ---------------------------------------------------------------------------
// Bytes in
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool failed;
} In;

static bool take(In *in, void *into, size_t length)
{
    if (in->failed || in->length - in->at < length) {
        in->failed = true;
        return false;
    }
    if (length > 0) {
        memcpy(into, in->data + in->at, length);
    }
    in->at += length;
    return true;
}

static uint8_t get_u8(In *in)
{
    uint8_t v = 0;
    take(in, &v, 1);
    return v;
}

static uint16_t get_u16(In *in)
{
    uint8_t b[2] = { 0, 0 };
    take(in, b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t get_u32(In *in)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    take(in, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static uint64_t get_u64(In *in)
{
    uint8_t b[8] = { 0 };
    take(in, b, 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | b[i];
    }
    return v;
}

// A view into the input, valid as long as the bytes are.
static const char *get_text(In *in, size_t *length)
{
    uint32_t n = get_u32(in);
    if (in->failed || in->length - in->at < n) {
        in->failed = true;
        *length = 0;
        return "";
    }
    const char *text = (const char *)(in->data + in->at);
    in->at += n;
    *length = n;
    return text;
}

// ---------------------------------------------------------------------------
// What leaves a chunk, named
// ---------------------------------------------------------------------------

typedef enum {
    EXT_UNIT,         // a unit this one's require^s reach, by relative path
    EXT_HOSTDATA,     // 8.8: a registered type, by module and name
    EXT_HOSTVALUE,    // 8.9: the same, with the width the frames were laid
                      // out against
    EXT_HOSTKIND,     // 8.7: a registered error kind -- the group, or one
                      // variant of it
    EXT_CASTFAILURE,  // 04 の 2.7: the language's own kind
    EXT_HOSTENUM,     // 8.7改2: a registered enum's declaration
    EXT_ENUMDECL      // 02 の 19 章: an enum^ this unit or one it requires
                      // declared -- the identity fits^ compares
} ExtKind;

// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------

typedef struct {
    const char *text;
    size_t length;
} Str;

typedef struct {
    ExtKind kind;
    const void *pointer;  // what it stands for while writing
    uint32_t module;      // string ids, +1; 0 for none
    uint32_t name;
    uint32_t variant;
    uint32_t path;
    uint32_t width;
} Ext;

typedef struct {
    const LhatUnit *unit;
    const LhatProgram *program;
    bool with_debug;
    bool failed;

    Str *strings;
    size_t string_count;
    size_t string_capacity;

    Ext *exts;
    size_t ext_count;
    size_t ext_capacity;

    // The descriptors and kinds the tree holds, in an order every reference
    // points backwards in -- a reader builds them front to back.
    const LhatObject **objs;
    size_t obj_count;
    size_t obj_capacity;

    // The relative paths made while writing; the string table points into
    // them, so they live as long as the writer.
    struct OwnedText *owned;
} Writer;

static uint32_t intern_string(Writer *w, const char *text, size_t length)
{
    for (size_t i = 0; i < w->string_count; i++) {
        if (w->strings[i].length == length &&
            memcmp(w->strings[i].text, text, length) == 0) {
            return (uint32_t)i + 1;
        }
    }
    LHAT_GROW(w->strings, w->string_count, w->string_capacity, 32,
              w->failed = true; return 0);
    w->strings[w->string_count].text = text;
    w->strings[w->string_count].length = length;
    return (uint32_t)++w->string_count;
}

static uint32_t intern_cstring(Writer *w, const char *text)
{
    return text != NULL ? intern_string(w, text, strlen(text)) : 0;
}

static uint32_t find_ext(const Writer *w, ExtKind kind, const void *pointer)
{
    for (size_t i = 0; i < w->ext_count; i++) {
        if (w->exts[i].kind == kind && w->exts[i].pointer == pointer) {
            return (uint32_t)i + 1;
        }
    }
    return 0;
}

static uint32_t add_ext(Writer *w, const Ext *ext)
{
    LHAT_GROW(w->exts, w->ext_count, w->ext_capacity, 8,
              w->failed = true; return 0);
    w->exts[w->ext_count] = *ext;
    return (uint32_t)++w->ext_count;
}

static uint32_t find_obj(const Writer *w, const LhatObject *pointer)
{
    for (size_t i = 0; i < w->obj_count; i++) {
        if (w->objs[i] == pointer) {
            return (uint32_t)i + 1;
        }
    }
    return 0;
}

static uint32_t add_obj(Writer *w, const LhatObject *pointer)
{
    LHAT_GROW(w->objs, w->obj_count, w->obj_capacity, 16,
              w->failed = true; return 0);
    w->objs[w->obj_count] = pointer;
    return (uint32_t)++w->obj_count;
}

// `to` written relative to the directory `from` stands in. Both are
// normalised paths (program.c), so a common run of whole segments is what
// the walk up counts. Owned by the caller.
static char *relative_path(const char *from, const char *to)
{
    size_t dir = 0;
    for (size_t i = 0; from[i] != '\0'; i++) {
        if (from[i] == '/' || from[i] == '\\') {
            dir = i + 1;
        }
    }
    // The common prefix, cut back to a segment boundary.
    size_t common = 0;
    size_t i = 0;
    while (i < dir && to[i] != '\0') {
        bool sep_from = from[i] == '/' || from[i] == '\\';
        bool sep_to = to[i] == '/' || to[i] == '\\';
        if (sep_from && sep_to) {
            common = i + 1;
        } else if (from[i] != to[i]) {
            break;
        }
        i++;
    }
    if (i == dir && (to[i] == '\0' || to[i] == '/' || to[i] == '\\')) {
        common = dir;
    }
    size_t ups = 0;
    for (size_t k = common; k < dir; k++) {
        if (from[k] == '/' || from[k] == '\\') {
            ups++;
        }
    }
    const char *rest = to + common;
    size_t rest_length = strlen(rest);
    char *made = (char *)lhat_alloc(ups * 3 + rest_length + 1);
    if (made == NULL) {
        return NULL;
    }
    char *p = made;
    for (size_t k = 0; k < ups; k++) {
        memcpy(p, "../", 3);
        p += 3;
    }
    memcpy(p, rest, rest_length + 1);
    return made;
}

typedef struct OwnedText {
    char *text;
    struct OwnedText *next;
} OwnedText;

static const char *keep_text(Writer *w, char *text)
{
    if (text == NULL) {
        w->failed = true;
        return "";
    }
    OwnedText *o = (OwnedText *)lhat_alloc(sizeof *o);
    if (o == NULL) {
        lhat_free(text);
        w->failed = true;
        return "";
    }
    o->text = text;
    o->next = w->owned;
    w->owned = o;
    return text;
}

static void free_owned_texts(Writer *w)
{
    while (w->owned != NULL) {
        OwnedText *o = w->owned;
        w->owned = o->next;
        lhat_free(o->text);
        lhat_free(o);
    }
}

// ---- classifying what a pointer stands for ----

static uint32_t ext_hostdata(Writer *w, const LhatHostDataTag *tag)
{
    uint32_t found = find_ext(w, EXT_HOSTDATA, tag);
    if (found != 0) {
        return found;
    }
    Ext ext = { EXT_HOSTDATA, tag, intern_cstring(w, tag->module),
                intern_cstring(w, tag->name), 0, 0, 0 };
    return add_ext(w, &ext);
}

static uint32_t ext_hostvalue(Writer *w, const LhatHostValueTag *tag)
{
    uint32_t found = find_ext(w, EXT_HOSTVALUE, tag);
    if (found != 0) {
        return found;
    }
    Ext ext = { EXT_HOSTVALUE, tag, intern_cstring(w, tag->module),
                intern_cstring(w, tag->name), 0, 0, (uint32_t)tag->width };
    return add_ext(w, &ext);
}

// A kind the program registered, or the language's own -- answers 0 for a
// kind this unit declared, which travels as an object instead.
static uint32_t ext_of_kind(Writer *w, const LhatErrorKind *kind)
{
    if (kind == lhat_registry_cast_failure()) {
        uint32_t found = find_ext(w, EXT_CASTFAILURE, kind);
        if (found != 0) {
            return found;
        }
        Ext ext = { EXT_CASTFAILURE, kind, 0, 0, 0, 0, 0 };
        return add_ext(w, &ext);
    }
    const LhatProgram *program = w->program;
    for (size_t i = 0; i < program->host_error_entry_count; i++) {
        const LhatHostErrorKind *e = &program->host_error_entries[i];
        const char *variant = NULL;
        bool is_this = e->group == kind;
        for (size_t v = 0; !is_this && v < e->variant_count; v++) {
            if (e->variants[v] == kind) {
                is_this = true;
                variant = e->variant_names[v];
            }
        }
        if (!is_this) {
            continue;
        }
        uint32_t found = find_ext(w, EXT_HOSTKIND, kind);
        if (found != 0) {
            return found;
        }
        Ext ext = { EXT_HOSTKIND, kind, intern_cstring(w, e->module),
                    intern_cstring(w, e->name), intern_cstring(w, variant),
                    0, 0 };
        return add_ext(w, &ext);
    }
    return 0;
}

// The declaration behind an enum descriptor. Three places declare one: the
// host (8.7改2), this unit (02 の 19 章, anywhere a statement stands), and a
// unit this one requires (through its exports).
typedef struct {
    const void *decl;
    const LhatNode *name;
} EnumSearch;

static void find_enumdef(void *context, const char *field, bool in_list,
                         const LhatNode *child)
{
    (void)field;
    (void)in_list;
    EnumSearch *search = (EnumSearch *)context;
    if (child == NULL || search->name != NULL) {
        return;
    }
    if (child->kind == LHAT_NODE_ENUMDEF && child->checked_type == search->decl) {
        search->name = child->v.named.name;
        return;
    }
    lhat_node_visit_children(child, find_enumdef, context);
}

static uint32_t ext_of_enum(Writer *w, const void *decl)
{
    uint32_t found = find_ext(w, EXT_ENUMDECL, decl);
    if (found == 0) {
        found = find_ext(w, EXT_HOSTENUM, decl);
    }
    if (found != 0) {
        return found;
    }
    const LhatProgram *program = w->program;
    for (size_t i = 0; i < program->host_enum_count; i++) {
        const LhatProgramEnum *e = &program->host_enums[i];
        if (e->decl_rt != NULL && e->decl_rt->enum_decl == decl) {
            Ext ext = { EXT_HOSTENUM, decl, intern_cstring(w, e->module),
                        intern_cstring(w, e->name),
                        intern_cstring(w, e->type), 0, 0 };
            return add_ext(w, &ext);
        }
    }
    const LhatUnit *unit = w->unit;
    if (unit->parsed.root != NULL) {
        EnumSearch search = { decl, NULL };
        find_enumdef(&search, NULL, false, unit->parsed.root);
        const char *text = NULL;
        size_t length = 0;
        if (search.name != NULL &&
            lhat_node_name(search.name, unit->lexer.source->text,
                           unit->lexer.strings, &text, &length)) {
            Ext ext = { EXT_ENUMDECL, decl, 0, intern_string(w, text, length),
                        0, 0, 0 };
            return add_ext(w, &ext);
        }
    }
    for (size_t i = 0; i < unit->referenced_count; i++) {
        const LhatUnit *other = unit->referenced[i];
        const LhatType *exports = other->checked.exports;
        if (exports == NULL) {
            continue;
        }
        for (const LhatTypeMember *m = exports->v.table.members; m != NULL;
             m = m->next) {
            if (m->type != decl || m->type->kind != LHAT_TYPE_ENUM) {
                continue;
            }
            const char *path =
                keep_text(w, relative_path(unit->path, other->path));
            Ext ext = { EXT_ENUMDECL, decl, 0,
                        intern_string(w, m->name, m->name_length), 0,
                        intern_cstring(w, path), 0 };
            return add_ext(w, &ext);
        }
    }
    w->failed = true;  // a declaration no name reaches
    return 0;
}

// ---- gathering the objects, dependencies first ----

static uint32_t intern_rt(Writer *w, const LhatRuntimeType *rt);

static uint32_t intern_kind(Writer *w, const LhatErrorKind *kind)
{
    if (kind == NULL) {
        return 0;
    }
    uint32_t found = find_obj(w, (const LhatObject *)kind);
    if (found != 0) {
        return found;
    }
    if (ext_of_kind(w, kind) != 0) {
        return 0;  // registered: the external reference is the whole of it
    }
    if (kind->group != NULL) {
        intern_kind(w, kind->group);
    }
    intern_string(w, kind->name->text, kind->name->length);
    return add_obj(w, (const LhatObject *)kind);
}

static uint32_t intern_rt(Writer *w, const LhatRuntimeType *rt)
{
    if (rt == NULL) {
        return 0;
    }
    uint32_t found = find_obj(w, (const LhatObject *)rt);
    if (found != 0) {
        return found;
    }
    for (size_t i = 0; i < rt->part_count; i++) {
        intern_rt(w, rt->parts[i]);
    }
    intern_rt(w, rt->result);
    intern_rt(w, rt->receive);
    intern_rt(w, rt->produce);
    intern_rt(w, rt->variadic);
    intern_rt(w, rt->instance);
    for (size_t i = 0; i < rt->member_count; i++) {
        intern_string(w, rt->members[i].name->text,
                      rt->members[i].name->length);
        intern_rt(w, rt->members[i].type);
    }
    if (rt->enum_name != NULL) {
        intern_string(w, rt->enum_name->text, rt->enum_name->length);
    }
    if (rt->enum_owner_name != NULL) {
        intern_string(w, rt->enum_owner_name->text,
                      rt->enum_owner_name->length);
    }
    if (rt->hostdata_tag != NULL) {
        ext_hostdata(w, rt->hostdata_tag);
    }
    if (rt->hostvalue_tag != NULL) {
        ext_hostvalue(w, rt->hostvalue_tag);
    }
    if (rt->error_kind != NULL) {
        intern_kind(w, rt->error_kind);
    }
    if (rt->enum_decl != NULL) {
        ext_of_enum(w, rt->enum_decl);
    }
    return add_obj(w, (const LhatObject *)rt);
}

static void intern_constant(Writer *w, LhatValue v)
{
    if (lhat_is_object_kind(v, LHAT_OBJECT_STRING)) {
        const LhatString *s = (const LhatString *)lhat_as_object(v);
        intern_string(w, s->text, s->length);
    } else if (lhat_is_object_kind(v, LHAT_OBJECT_ERROR_KIND)) {
        intern_kind(w, (const LhatErrorKind *)lhat_as_object(v));
    } else if (lhat_is_object_kind(v, LHAT_OBJECT_TYPE)) {
        intern_rt(w, (const LhatRuntimeType *)lhat_as_object(v));
    } else if (!lhat_is_nil(v) && !lhat_is_bool(v) && !lhat_is_integer(v) &&
               !lhat_is_real(v)) {
        w->failed = true;  // nothing else is ever a constant
    }
}

static void intern_proto(Writer *w, const LhatProto *proto)
{
    intern_cstring(w, proto->debug_name);
    if (w->with_debug) {
        for (size_t i = 0; i < proto->chunk.local_count; i++) {
            intern_cstring(w, proto->chunk.locals[i].name);
        }
        for (size_t i = 0; i < proto->upvalue_count; i++) {
            intern_cstring(w, proto->upvalues[i].name);
        }
    }
    for (size_t i = 0; i < proto->chunk.constant_count; i++) {
        intern_constant(w, proto->chunk.constants[i]);
    }
    for (size_t i = 0; proto->parameter_types != NULL && i < proto->parameters;
         i++) {
        intern_rt(w, proto->parameter_types[i]);
    }
    intern_rt(w, proto->result_type);
    intern_rt(w, proto->yield_produce_type);
    intern_rt(w, proto->yield_receive_type);
    for (size_t i = 0; i < proto->proto_count; i++) {
        intern_proto(w, proto->protos[i]);
    }
}

// ---- emitting ----

static uint32_t obj_ref(const Writer *w, const void *pointer)
{
    return pointer != NULL ? find_obj(w, (const LhatObject *)pointer) : 0;
}

static uint32_t str_ref(const Writer *w, const char *text, size_t length)
{
    for (size_t i = 0; i < w->string_count; i++) {
        if (w->strings[i].length == length &&
            memcmp(w->strings[i].text, text, length) == 0) {
            return (uint32_t)i + 1;
        }
    }
    return 0;
}

static uint32_t cstr_ref(const Writer *w, const char *text)
{
    return text != NULL ? str_ref(w, text, strlen(text)) : 0;
}

static uint32_t lstr_ref(const Writer *w, const LhatString *s)
{
    return s != NULL ? str_ref(w, s->text, s->length) : 0;
}

static void emit_kind(Writer *w, Out *o, const LhatErrorKind *kind)
{
    put_u8(o, 0);  // an error kind
    put_u32(o, obj_ref(w, kind->group));
    put_u32(o, lstr_ref(w, kind->name));
    put_u8(o, kind->local ? 1 : 0);
}

static void emit_rt(Writer *w, Out *o, const LhatRuntimeType *rt)
{
    put_u8(o, 1);  // a descriptor
    put_u8(o, (uint8_t)rt->kind);
    // error_kind: 0 none, 1 an object of this unit's, 2 a registered one.
    if (rt->error_kind == NULL) {
        put_u8(o, 0);
    } else {
        uint32_t as_obj = obj_ref(w, rt->error_kind);
        if (as_obj != 0) {
            put_u8(o, 1);
            put_u32(o, as_obj);
        } else {
            put_u8(o, 2);
            put_u32(o, ext_of_kind(w, rt->error_kind));
        }
    }
    put_u32(o, rt->enum_decl != NULL ? ext_of_enum(w, rt->enum_decl) : 0);
    put_u32(o, (uint32_t)rt->enum_member_index);
    put_u32(o, lstr_ref(w, rt->enum_name));
    put_u32(o, lstr_ref(w, rt->enum_owner_name));
    put_u8(o, rt->error_local ? 1 : 0);
    put_u32(o, rt->hostdata_tag != NULL
                   ? find_ext(w, EXT_HOSTDATA, rt->hostdata_tag)
                   : 0);
    put_u32(o, rt->hostvalue_tag != NULL
                   ? find_ext(w, EXT_HOSTVALUE, rt->hostvalue_tag)
                   : 0);
    put_u32(o, (uint32_t)rt->part_count);
    for (size_t i = 0; i < rt->part_count; i++) {
        put_u32(o, obj_ref(w, rt->parts[i]));
    }
    put_u32(o, obj_ref(w, rt->result));
    put_u8(o, (uint8_t)((rt->is_function ? 1 : 0) | (rt->takes_self ? 2 : 0) |
                        (rt->self_last ? 4 : 0) | (rt->closed ? 8 : 0) |
                        (rt->endless ? 16 : 0)));
    put_u32(o, obj_ref(w, rt->receive));
    put_u32(o, obj_ref(w, rt->produce));
    put_u32(o, obj_ref(w, rt->variadic));
    put_u32(o, (uint32_t)rt->member_count);
    for (size_t i = 0; i < rt->member_count; i++) {
        put_u32(o, lstr_ref(w, rt->members[i].name));
        put_u32(o, obj_ref(w, rt->members[i].type));
    }
    put_u32(o, obj_ref(w, rt->instance));
    put_u32(o, (uint32_t)rt->levels);
}

static void emit_constant(Writer *w, Out *o, LhatValue v)
{
    if (lhat_is_nil(v)) {
        put_u8(o, 0);
    } else if (lhat_is_bool(v)) {
        put_u8(o, 1);
        put_u8(o, lhat_as_bool(v) ? 1 : 0);
    } else if (lhat_is_integer(v)) {
        put_u8(o, 2);
        put_u64(o, (uint64_t)lhat_as_integer(v));
    } else if (lhat_is_real(v)) {
        double d = lhat_as_real(v);
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        put_u8(o, 3);
        put_u64(o, bits);
    } else if (lhat_is_object_kind(v, LHAT_OBJECT_STRING)) {
        put_u8(o, 4);
        put_u32(o, lstr_ref(w, (const LhatString *)lhat_as_object(v)));
    } else if (lhat_is_object_kind(v, LHAT_OBJECT_ERROR_KIND)) {
        const LhatErrorKind *kind = (const LhatErrorKind *)lhat_as_object(v);
        uint32_t as_obj = obj_ref(w, kind);
        if (as_obj != 0) {
            put_u8(o, 5);
            put_u32(o, as_obj);
        } else {
            put_u8(o, 6);
            put_u32(o, ext_of_kind(w, kind));
        }
    } else if (lhat_is_object_kind(v, LHAT_OBJECT_TYPE)) {
        put_u8(o, 5);
        put_u32(o, obj_ref(w, lhat_as_object(v)));
    } else {
        w->failed = true;
    }
}

static void emit_proto(Writer *w, Out *o, const LhatProto *proto)
{
    put_u8(o, (uint8_t)((proto->is_function ? 1 : 0) |
                        (proto->yields ? 2 : 0) | (proto->takes_self ? 4 : 0) |
                        (proto->self_last ? 8 : 0) |
                        (proto->has_variadic ? 16 : 0) |
                        (proto->yield_receives_known ? 32 : 0) |
                        (proto->yield_endless ? 64 : 0)));
    put_u8(o, proto->reserved);
    put_u8(o, proto->kept);
    put_u8(o, proto->parameters);
    put_u8(o, proto->parameter_slots);
    put_u8(o, proto->yield_receive_count);
    put_u32(o, cstr_ref(w, proto->debug_name));

    put_u32(o, (uint32_t)proto->upvalue_count);
    for (size_t i = 0; i < proto->upvalue_count; i++) {
        put_u8(o, (uint8_t)proto->upvalues[i].source);
        put_u8(o, proto->upvalues[i].index);
        put_u32(o, w->with_debug ? cstr_ref(w, proto->upvalues[i].name) : 0);
    }

    put_u8(o, proto->parameter_types != NULL ? 1 : 0);
    if (proto->parameter_types != NULL) {
        for (size_t i = 0; i < proto->parameters; i++) {
            put_u32(o, obj_ref(w, proto->parameter_types[i]));
        }
    }
    put_u32(o, obj_ref(w, proto->result_type));
    put_u32(o, obj_ref(w, proto->yield_produce_type));
    put_u32(o, obj_ref(w, proto->yield_receive_type));

    const LhatChunk *chunk = &proto->chunk;
    put_u8(o, chunk->registers);
    put_u32(o, (uint32_t)chunk->count);
    for (size_t i = 0; i < chunk->count; i++) {
        put_u32(o, chunk->code[i]);
    }
    for (size_t i = 0; i < chunk->count; i++) {
        put_u32(o, chunk->lines[i]);
    }
    put_u32(o, (uint32_t)chunk->member_cache_count);
    for (size_t i = 0; i < chunk->member_cache_count; i++) {
        put_u16(o, chunk->member_caches[i].key);
    }
    put_u32(o, (uint32_t)chunk->constant_count);
    for (size_t i = 0; i < chunk->constant_count; i++) {
        emit_constant(w, o, chunk->constants[i]);
    }
    size_t locals = w->with_debug ? chunk->local_count : 0;
    put_u32(o, (uint32_t)locals);
    for (size_t i = 0; i < locals; i++) {
        put_u32(o, chunk->locals[i].from);
        put_u32(o, chunk->locals[i].to);
        put_u8(o, chunk->locals[i].reg);
        put_u8(o, chunk->locals[i].width);
        put_u32(o, cstr_ref(w, chunk->locals[i].name));
    }

    put_u32(o, (uint32_t)proto->proto_count);
    for (size_t i = 0; i < proto->proto_count; i++) {
        emit_proto(w, o, proto->protos[i]);
    }
}

bool lhat_serialize_write(const LhatUnit *unit, bool with_debug_names,
                          uint8_t **out, size_t *length)
{
    if (unit == NULL || unit->proto == NULL || out == NULL || length == NULL) {
        return false;
    }
    Writer w;
    memset(&w, 0, sizeof w);
    w.unit = unit;
    w.program = unit->program;
    w.with_debug = with_debug_names;

    // The units first, in the order the UNIT instructions count them.
    for (size_t i = 0; i < unit->referenced_count; i++) {
        const char *path = keep_text(
            &w, relative_path(unit->path, unit->referenced[i]->path));
        Ext ext = { EXT_UNIT, unit->referenced[i], 0, 0, 0,
                    intern_cstring(&w, path), 0 };
        add_ext(&w, &ext);
    }
    intern_cstring(&w, unit->module_name);
    intern_proto(&w, unit->proto);
    // 8.7: the export descriptors, built onto the root heap by the same
    // call a host makes -- so they travel as the objects they are.
    size_t exports = lhat_unit_export_count(unit);
    for (size_t i = 0; i < exports; i++) {
        LhatUnitText name = lhat_unit_export_name(unit, i);
        char *named = (char *)lhat_alloc(name.length + 1);
        if (named == NULL) {
            w.failed = true;
            break;
        }
        memcpy(named, name.text, name.length);
        named[name.length] = '\0';
        intern_string(&w, name.text, name.length);
        intern_rt(&w, lhat_unit_export_type(unit, named));
        lhat_free(named);
    }

    Out o;
    memset(&o, 0, sizeof o);
    put_bytes(&o, MAGIC, sizeof MAGIC);
    put_u16(&o, FORMAT_VERSION);
    put_u16(&o, (uint16_t)((with_debug_names ? FLAG_DEBUG_NAMES : 0) |
                           (unit->program->strict ? FLAG_STRICT : 0)));
    put_u64(&o, fingerprint());
    put_u64(&o, 0);  // the hash, patched below
    put_text(&o, LHAT_VERSION, strlen(LHAT_VERSION));

    // Each string is followed by a NUL, so a reader may hand the bytes to
    // the C library as they stand.
    put_u32(&o, (uint32_t)w.string_count);
    for (size_t i = 0; i < w.string_count; i++) {
        put_text(&o, w.strings[i].text, w.strings[i].length);
        put_u8(&o, 0);
    }

    put_u32(&o, (uint32_t)w.ext_count);
    for (size_t i = 0; i < w.ext_count; i++) {
        const Ext *e = &w.exts[i];
        put_u8(&o, (uint8_t)e->kind);
        put_u32(&o, e->module);
        put_u32(&o, e->name);
        put_u32(&o, e->variant);
        put_u32(&o, e->path);
        put_u32(&o, e->width);
    }

    put_u32(&o, (uint32_t)w.obj_count);
    for (size_t i = 0; i < w.obj_count; i++) {
        const LhatObject *object = w.objs[i];
        if (object->kind == LHAT_OBJECT_ERROR_KIND) {
            emit_kind(&w, &o, (const LhatErrorKind *)object);
        } else {
            emit_rt(&w, &o, (const LhatRuntimeType *)object);
        }
    }

    emit_proto(&w, &o, unit->proto);
    put_u32(&o, cstr_ref(&w, unit->module_name));
    put_u32(&o, (uint32_t)exports);
    for (size_t i = 0; i < exports; i++) {
        LhatUnitText name = lhat_unit_export_name(unit, i);
        char *named = (char *)lhat_alloc(name.length + 1);
        if (named == NULL) {
            w.failed = true;
            break;
        }
        memcpy(named, name.text, name.length);
        named[name.length] = '\0';
        put_u32(&o, str_ref(&w, name.text, name.length));
        put_u32(&o, obj_ref(&w, lhat_unit_export_type(unit, named)));
        lhat_free(named);
    }

    bool ok = !w.failed && !o.failed;
    if (ok) {
        uint64_t hash =
            fnv_step(FNV_BASIS, o.data + HEADER_BYTES, o.length - HEADER_BYTES);
        for (int i = 0; i < 8; i++) {
            o.data[16 + i] = (uint8_t)(hash >> (8 * i));
        }
        *out = o.data;
        *length = o.length;
    } else {
        lhat_free(o.data);
    }
    lhat_free(w.strings);
    lhat_free(w.exts);
    lhat_free((void *)w.objs);
    free_owned_texts(&w);
    return ok;
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

typedef struct {
    ExtKind kind;
    const void *resolved;
} Resolved;

typedef struct {
    LhatProgram *program;
    LhatUnit *unit;
    In in;
    LhatProgramErrorCode error;  // what to report when it fails

    Str *strings;
    size_t string_count;
    LhatString **made;  // the string objects, one per id, made on demand

    Resolved *exts;
    size_t ext_count;

    LhatObject **objs;
    size_t obj_count;

    LhatProto *root;  // the heap every object is made on
} Reader;

static void fail(Reader *r, LhatProgramErrorCode error)
{
    if (!r->in.failed) {
        r->error = error;
    }
    r->in.failed = true;
}

static bool string_at(Reader *r, uint32_t ref, const char **text,
                      size_t *length)
{
    if (ref == 0 || ref > r->string_count) {
        *text = NULL;
        *length = 0;
        return ref == 0;
    }
    *text = r->strings[ref - 1].text;
    *length = r->strings[ref - 1].length;
    return true;
}

static const char *cstring_at(Reader *r, uint32_t ref)
{
    const char *text = NULL;
    size_t length = 0;
    if (!string_at(r, ref, &text, &length)) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
    }
    return text;  // NUL-terminated: the writer put one after each string
}

// A string object for the id, on the root chunk's heap -- one per id, so
// a name written many times over the tree is one object, as the compiler
// made it.
static LhatString *lstring_at(Reader *r, uint32_t ref)
{
    if (ref == 0) {
        return NULL;
    }
    if (ref > r->string_count) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return NULL;
    }
    if (r->made[ref - 1] == NULL) {
        r->made[ref - 1] = lhat_string_new(&r->root->chunk.heap,
                                           r->strings[ref - 1].text,
                                           r->strings[ref - 1].length);
        if (r->made[ref - 1] == NULL) {
            fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    return r->made[ref - 1];
}

static const void *ext_at(Reader *r, uint32_t ref, ExtKind wanted)
{
    if (ref == 0 || ref > r->ext_count || r->exts[ref - 1].kind != wanted) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return NULL;
    }
    return r->exts[ref - 1].resolved;
}

static LhatObject *obj_at(Reader *r, uint32_t ref, LhatObjectKind wanted)
{
    if (ref == 0) {
        return NULL;
    }
    if (ref > r->obj_count || r->objs[ref - 1] == NULL ||
        r->objs[ref - 1]->kind != wanted) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return NULL;
    }
    return r->objs[ref - 1];
}

static LhatRuntimeType *rt_at(Reader *r, uint32_t ref)
{
    return (LhatRuntimeType *)obj_at(r, ref, LHAT_OBJECT_TYPE);
}

// ---- resolving what a name stands for on this program ----

static const void *resolve_ext(Reader *r, ExtKind kind, uint32_t module_ref,
                               uint32_t name_ref, uint32_t variant_ref,
                               uint32_t path_ref, uint32_t width)
{
    LhatProgram *program = r->program;
    const char *module = cstring_at(r, module_ref);
    const char *name = cstring_at(r, name_ref);
    const char *variant = cstring_at(r, variant_ref);
    const char *path = cstring_at(r, path_ref);
    if (r->in.failed) {
        return NULL;
    }
    switch (kind) {
        case EXT_UNIT: {
            if (path == NULL) {
                fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
                return NULL;
            }
            LhatUnit *other =
                lhat_program_require_unit(program, r->unit, path, strlen(path));
            if (other == NULL) {
                fail(r, LHAT_PROGRAM_ERR_CANNOT_READ);
                return NULL;
            }
            // 05 の 10 章: a binary unit's require^s reach binary units.
            if (other->state == LHAT_UNIT_DONE && !other->binary) {
                lhat_program_report(program, LHAT_PROGRAM_ERR_MIXED,
                                    other->path);
                fail(r, LHAT_PROGRAM_ERR_MIXED);
                return NULL;
            }
            return other;
        }
        case EXT_HOSTDATA:
            for (size_t i = 0; i < program->host_type_entry_count; i++) {
                const LhatHostTypeEntry *e = &program->host_type_entries[i];
                if (module != NULL && name != NULL &&
                    strcmp(e->module, module) == 0 &&
                    strcmp(e->name, name) == 0) {
                    return e->tag;
                }
            }
            fail(r, LHAT_PROGRAM_ERR_HOST_MISMATCH);
            return NULL;
        case EXT_HOSTVALUE:
            for (size_t i = 0; i < program->hostvalue_type_entry_count; i++) {
                const LhatHostValueTypeEntry *e =
                    &program->hostvalue_type_entries[i];
                if (module != NULL && name != NULL &&
                    strcmp(e->module, module) == 0 &&
                    strcmp(e->name, name) == 0) {
                    if (e->tag->width != width) {
                        break;  // laid out against another size
                    }
                    return e->tag;
                }
            }
            fail(r, LHAT_PROGRAM_ERR_HOST_MISMATCH);
            return NULL;
        case EXT_HOSTKIND: {
            const LhatErrorKind *kind_found =
                module != NULL && name != NULL
                    ? lhat_lookup_error_kind(program, module, name, variant)
                    : NULL;
            if (kind_found == NULL) {
                fail(r, LHAT_PROGRAM_ERR_HOST_MISMATCH);
            }
            return kind_found;
        }
        case EXT_CASTFAILURE:
            return lhat_registry_cast_failure();
        case EXT_HOSTENUM:
            for (size_t i = 0; i < program->host_enum_count; i++) {
                const LhatProgramEnum *e = &program->host_enums[i];
                bool same_type = (e->type == NULL) == (variant == NULL) &&
                                 (e->type == NULL ||
                                  strcmp(e->type, variant) == 0);
                if (module != NULL && name != NULL &&
                    strcmp(e->module, module) == 0 &&
                    strcmp(e->name, name) == 0 && same_type &&
                    e->decl_rt != NULL) {
                    return e->decl_rt->enum_decl;
                }
            }
            fail(r, LHAT_PROGRAM_ERR_HOST_MISMATCH);
            return NULL;
        case EXT_ENUMDECL: {
            if (name == NULL) {
                fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
                return NULL;
            }
            const void *decl = NULL;
            if (path == NULL) {
                decl = lhat_program_enum_identity(program, r->unit->path, name);
            } else {
                char *resolved = lhat_program_resolve_path(r->unit, path,
                                                           strlen(path));
                if (resolved != NULL) {
                    decl = lhat_program_enum_identity(program, resolved, name);
                    lhat_free(resolved);
                }
            }
            if (decl == NULL) {
                fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
            }
            return decl;
        }
    }
    fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
    return NULL;
}

// ---- building the objects ----

static void read_kind(Reader *r)
{
    uint32_t group_ref = get_u32(&r->in);
    uint32_t name_ref = get_u32(&r->in);
    bool local = get_u8(&r->in) != 0;
    const LhatErrorKind *group =
        (const LhatErrorKind *)obj_at(r, group_ref, LHAT_OBJECT_ERROR_KIND);
    LhatString *name = lstring_at(r, name_ref);
    if (r->in.failed || name == NULL) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return;
    }
    LhatErrorKind *made =
        lhat_error_kind_new(&r->root->chunk.heap, group, local, name);
    if (made == NULL) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return;
    }
    r->objs[r->obj_count++] = (LhatObject *)made;
}

static void read_rt(Reader *r)
{
    In *in = &r->in;
    uint8_t kind = get_u8(in);
    if (kind > LHAT_TYPE_RT_UNKNOWN) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return;
    }
    LhatRuntimeType *rt =
        lhat_type_rt_new(&r->root->chunk.heap, (LhatRuntimeTypeKind)kind);
    if (rt == NULL) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        return;
    }
    uint8_t kind_how = get_u8(in);
    if (kind_how == 1) {
        rt->error_kind = (const LhatErrorKind *)obj_at(
            r, get_u32(in), LHAT_OBJECT_ERROR_KIND);
    } else if (kind_how == 2) {
        uint32_t ref = get_u32(in);
        if (ref != 0 && ref <= r->ext_count &&
            (r->exts[ref - 1].kind == EXT_HOSTKIND ||
             r->exts[ref - 1].kind == EXT_CASTFAILURE)) {
            rt->error_kind = (const LhatErrorKind *)r->exts[ref - 1].resolved;
        } else {
            fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    } else if (kind_how != 0) {
        fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
    }
    uint32_t enum_ref = get_u32(in);
    if (enum_ref != 0) {
        if (enum_ref <= r->ext_count &&
            (r->exts[enum_ref - 1].kind == EXT_ENUMDECL ||
             r->exts[enum_ref - 1].kind == EXT_HOSTENUM)) {
            rt->enum_decl = r->exts[enum_ref - 1].resolved;
        } else {
            fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    rt->enum_member_index = get_u32(in);
    rt->enum_name = lstring_at(r, get_u32(in));
    rt->enum_owner_name = lstring_at(r, get_u32(in));
    rt->error_local = get_u8(in) != 0;
    uint32_t hostdata_ref = get_u32(in);
    if (hostdata_ref != 0) {
        rt->hostdata_tag =
            (const LhatHostDataTag *)ext_at(r, hostdata_ref, EXT_HOSTDATA);
    }
    uint32_t hostvalue_ref = get_u32(in);
    if (hostvalue_ref != 0) {
        rt->hostvalue_tag =
            (const LhatHostValueTag *)ext_at(r, hostvalue_ref, EXT_HOSTVALUE);
    }
    uint32_t parts = get_u32(in);
    for (uint32_t i = 0; i < parts && !in->failed; i++) {
        LhatRuntimeType *part = rt_at(r, get_u32(in));
        if (part == NULL || !lhat_type_rt_add_part(rt, part)) {
            fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    rt->result = rt_at(r, get_u32(in));
    uint8_t flags = get_u8(in);
    rt->is_function = (flags & 1) != 0;
    rt->takes_self = (flags & 2) != 0;
    rt->self_last = (flags & 4) != 0;
    rt->closed = (flags & 8) != 0;
    rt->endless = (flags & 16) != 0;
    rt->receive = rt_at(r, get_u32(in));
    rt->produce = rt_at(r, get_u32(in));
    rt->variadic = rt_at(r, get_u32(in));
    uint32_t members = get_u32(in);
    for (uint32_t i = 0; i < members && !in->failed; i++) {
        LhatString *name = lstring_at(r, get_u32(in));
        LhatRuntimeType *type = rt_at(r, get_u32(in));
        if (name == NULL || !lhat_type_rt_add_member(rt, name, type)) {
            fail(r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    rt->instance = rt_at(r, get_u32(in));
    rt->levels = get_u32(in);
    r->objs[r->obj_count++] = (LhatObject *)rt;
}

// ---- building the tree ----

static bool read_constant(Reader *r, LhatValue *out)
{
    In *in = &r->in;
    switch (get_u8(in)) {
        case 0:
            *out = lhat_nil();
            return true;
        case 1:
            *out = lhat_bool(get_u8(in) != 0);
            return true;
        case 2:
            *out = lhat_integer((int64_t)get_u64(in));
            return true;
        case 3: {
            uint64_t bits = get_u64(in);
            double d;
            memcpy(&d, &bits, sizeof d);
            *out = lhat_real(d);
            return true;
        }
        case 4: {
            LhatString *s = lstring_at(r, get_u32(in));
            if (s == NULL) {
                return false;
            }
            *out = lhat_object((LhatObject *)s);
            return true;
        }
        case 5: {
            uint32_t ref = get_u32(in);
            if (ref == 0 || ref > r->obj_count) {
                return false;
            }
            *out = lhat_object(r->objs[ref - 1]);
            return true;
        }
        case 6: {
            uint32_t ref = get_u32(in);
            if (ref == 0 || ref > r->ext_count ||
                (r->exts[ref - 1].kind != EXT_HOSTKIND &&
                 r->exts[ref - 1].kind != EXT_CASTFAILURE)) {
                return false;
            }
            *out = lhat_object((LhatObject *)r->exts[ref - 1].resolved);
            return true;
        }
        default:
            return false;
    }
}

static bool read_proto(Reader *r, LhatProto *proto)
{
    In *in = &r->in;
    uint8_t flags = get_u8(in);
    proto->is_function = (flags & 1) != 0;
    proto->yields = (flags & 2) != 0;
    proto->takes_self = (flags & 4) != 0;
    proto->self_last = (flags & 8) != 0;
    proto->has_variadic = (flags & 16) != 0;
    proto->yield_receives_known = (flags & 32) != 0;
    proto->yield_endless = (flags & 64) != 0;
    proto->reserved = get_u8(in);
    proto->kept = get_u8(in);
    proto->parameters = get_u8(in);
    proto->parameter_slots = get_u8(in);
    proto->yield_receive_count = get_u8(in);
    uint32_t name_ref = get_u32(in);
    if (name_ref != 0) {
        const char *text = cstring_at(r, name_ref);
        if (text != NULL) {
            size_t length = strlen(text);
            proto->debug_name = (char *)lhat_alloc(length + 1);
            if (proto->debug_name == NULL) {
                return false;
            }
            memcpy(proto->debug_name, text, length + 1);
        }
    }

    uint32_t upvalues = get_u32(in);
    for (uint32_t i = 0; i < upvalues && !in->failed; i++) {
        uint8_t source = get_u8(in);
        uint8_t index = get_u8(in);
        const char *name = cstring_at(r, get_u32(in));
        if (source > LHAT_UPVALUE_THIS ||
            lhat_proto_add_upvalue(proto, (LhatUpvalueSource)source, index,
                                   name != NULL ? name : "",
                                   name != NULL ? strlen(name) : 0) != i) {
            return false;
        }
    }

    if (get_u8(in) != 0) {
        proto->parameter_types = (LhatRuntimeType **)lhat_calloc(
            proto->parameters ? proto->parameters : 1,
            sizeof *proto->parameter_types);
        if (proto->parameter_types == NULL) {
            return false;
        }
        for (size_t i = 0; i < proto->parameters; i++) {
            proto->parameter_types[i] = rt_at(r, get_u32(in));
        }
    }
    proto->result_type = rt_at(r, get_u32(in));
    proto->yield_produce_type = rt_at(r, get_u32(in));
    proto->yield_receive_type = rt_at(r, get_u32(in));

    LhatChunk *chunk = &proto->chunk;
    chunk->registers = get_u8(in);
    uint32_t count = get_u32(in);
    if (in->failed || (uint64_t)count * 8 > in->length - in->at) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (lhat_chunk_emit(chunk, get_u32(in), 0) != i) {
            return false;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        chunk->lines[i] = get_u32(in);
    }
    uint32_t caches = get_u32(in);
    for (uint32_t i = 0; i < caches && !in->failed; i++) {
        if (lhat_chunk_member_cache(chunk, get_u16(in)) != i) {
            return false;
        }
    }
    uint32_t constants = get_u32(in);
    for (uint32_t i = 0; i < constants && !in->failed; i++) {
        LhatValue v;
        if (!read_constant(r, &v) || lhat_chunk_constant_raw(chunk, v) != i) {
            return false;
        }
    }
    uint32_t locals = get_u32(in);
    for (uint32_t i = 0; i < locals && !in->failed; i++) {
        uint32_t from = get_u32(in);
        uint32_t to = get_u32(in);
        uint8_t reg = get_u8(in);
        uint8_t width = get_u8(in);
        const char *name = cstring_at(r, get_u32(in));
        size_t at = lhat_chunk_add_local(chunk, name != NULL ? name : "",
                                         name != NULL ? strlen(name) : 0, reg,
                                         width);
        if (at != i) {
            return false;
        }
        chunk->locals[at].from = from;
        chunk->locals[at].to = to;
    }

    uint32_t children = get_u32(in);
    for (uint32_t i = 0; i < children && !in->failed; i++) {
        LhatProto *child = lhat_proto_new();
        if (child == NULL || lhat_proto_add(proto, child) != i) {
            lhat_proto_free(child);
            return false;
        }
        if (!read_proto(r, child)) {
            return false;
        }
    }
    return !in->failed;
}

static void free_binary_unit(LhatBinaryUnit *out)
{
    lhat_proto_free(out->proto);
    lhat_free(out->module_name);
    for (size_t i = 0; i < out->export_count; i++) {
        lhat_free(out->export_names[i]);
    }
    lhat_free(out->export_names);
    lhat_free(out->export_rt);
    memset(out, 0, sizeof *out);
}

bool lhat_serialize_load(LhatProgram *program, LhatUnit *unit,
                         const uint8_t *bytes, size_t length,
                         LhatBinaryUnit *out)
{
    Reader r;
    memset(&r, 0, sizeof r);
    r.program = program;
    r.unit = unit;
    r.in.data = bytes;
    r.in.length = length;
    r.error = LHAT_PROGRAM_ERR_BAD_BINARY;
    memset(out, 0, sizeof *out);

    In *in = &r.in;
    uint8_t magic[4];
    bool ok = take(in, magic, sizeof magic) && memcmp(magic, MAGIC, 4) == 0 &&
              get_u16(in) == FORMAT_VERSION;
    uint16_t flags = get_u16(in);
    (void)flags;
    uint64_t stamped = get_u64(in);
    uint64_t hash = get_u64(in);
    if (!ok || in->failed || stamped != fingerprint() ||
        hash != fnv_step(FNV_BASIS, bytes + HEADER_BYTES,
                         length - HEADER_BYTES)) {
        lhat_program_report(program, LHAT_PROGRAM_ERR_BAD_BINARY, unit->path);
        return false;
    }
    size_t version_length = 0;
    const char *version = get_text(in, &version_length);
    if (in->failed || version_length != strlen(LHAT_VERSION) ||
        memcmp(version, LHAT_VERSION, version_length) != 0) {
        lhat_program_report(program, LHAT_PROGRAM_ERR_BAD_BINARY, unit->path);
        return false;
    }

    r.root = lhat_proto_new();
    if (r.root == NULL) {
        lhat_program_report(program, LHAT_PROGRAM_ERR_BAD_BINARY, unit->path);
        return false;
    }

    // Strings: views into the bytes, and a slot for each one's object.
    r.string_count = get_u32(in);
    if (!in->failed && r.string_count > 0) {
        r.strings = (Str *)lhat_calloc(r.string_count, sizeof *r.strings);
        r.made = (LhatString **)lhat_calloc(r.string_count, sizeof *r.made);
        if (r.strings == NULL || r.made == NULL) {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    for (size_t i = 0; i < r.string_count && !in->failed; i++) {
        r.strings[i].text = get_text(in, &r.strings[i].length);
        if (get_u8(in) != 0) {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }

    // External references, resolved as they are read -- a unit first, so
    // that what this one requires is closed before its bodies point at it.
    r.ext_count = get_u32(in);
    if (!in->failed && r.ext_count > 0) {
        r.exts = (Resolved *)lhat_calloc(r.ext_count, sizeof *r.exts);
        if (r.exts == NULL) {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    for (size_t i = 0; i < r.ext_count && !in->failed; i++) {
        uint8_t kind = get_u8(in);
        uint32_t module = get_u32(in);
        uint32_t name = get_u32(in);
        uint32_t variant = get_u32(in);
        uint32_t path = get_u32(in);
        uint32_t width = get_u32(in);
        if (kind > EXT_ENUMDECL) {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
            break;
        }
        r.exts[i].kind = (ExtKind)kind;
        r.exts[i].resolved =
            resolve_ext(&r, (ExtKind)kind, module, name, variant, path, width);
    }

    // The objects, front to back.
    size_t objects = get_u32(in);
    if (!in->failed && objects > 0) {
        r.objs = (LhatObject **)lhat_calloc(objects, sizeof *r.objs);
        if (r.objs == NULL) {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }
    for (size_t i = 0; i < objects && !in->failed; i++) {
        uint8_t what = get_u8(in);
        if (what == 0) {
            read_kind(&r);
        } else if (what == 1) {
            read_rt(&r);
        } else {
            fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
        }
    }

    out->proto = r.root;
    if (!in->failed && read_proto(&r, r.root)) {
        r.root->is_unit = true;
        uint32_t module_ref = get_u32(in);
        const char *module_name = cstring_at(&r, module_ref);
        if (!in->failed && module_name != NULL) {
            size_t n = strlen(module_name);
            out->module_name = (char *)lhat_alloc(n + 1);
            if (out->module_name != NULL) {
                memcpy(out->module_name, module_name, n + 1);
            }
        }
        // 8.7: the export descriptors, for lhat_unit_export_* to answer.
        uint32_t exports = get_u32(in);
        if (!in->failed && exports > 0) {
            out->export_names =
                (char **)lhat_calloc(exports, sizeof *out->export_names);
            out->export_rt = (struct LhatRuntimeType **)lhat_calloc(
                exports, sizeof *out->export_rt);
            if (out->export_names == NULL || out->export_rt == NULL) {
                fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
            } else {
                out->export_count = exports;
            }
        }
        for (uint32_t i = 0; i < exports && !in->failed; i++) {
            const char *name = cstring_at(&r, get_u32(in));
            LhatRuntimeType *type = rt_at(&r, get_u32(in));
            if (name == NULL) {
                fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
                break;
            }
            size_t n = strlen(name);
            out->export_names[i] = (char *)lhat_alloc(n + 1);
            if (out->export_names[i] == NULL) {
                fail(&r, LHAT_PROGRAM_ERR_BAD_BINARY);
                break;
            }
            memcpy(out->export_names[i], name, n + 1);
            out->export_rt[i] = type;
        }
    } else {
        fail(&r, r.error);
    }

    bool loaded = !in->failed && in->at == in->length;
    if (!loaded) {
        free_binary_unit(out);
        lhat_program_report(program, r.error, unit->path);
    }
    lhat_free(r.strings);
    lhat_free(r.made);
    lhat_free(r.exts);
    lhat_free(r.objs);
    return loaded;
}
