// L^ (lhat) -- compiling a tree to bytecode, and running it.

#include "vm.h"

#include <stdlib.h>
#include <string.h>

#define LHAT_MAX_REGISTERS 250
#define LHAT_MAX_LOCALS 200
#define LHAT_MAX_BREAKS 64

// 9.8: break^ is a normal end for the loop it leaves, so its jump lands where
// last^ and epilog^ are, not past them. The chain is what a break^ written
// inside a nested block still finds.
typedef struct LoopContext {
    struct LoopContext *enclosing;
    size_t jumps[LHAT_MAX_BREAKS];
    size_t count;
} LoopContext;

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    size_t length;
    uint8_t reg;  // 5.2: a name is a slot in the frame like anything else
} Local;

// One subroutine being compiled. The chain of parents is what a name search
// walks when it is not found here, which is how 5.4 decides what to capture.
typedef struct Compiler {
    struct Compiler *parent;
    const LhatLexer *lexer;
    LhatProto *proto;
    LhatCompileStatus *status;  // shared, so the first failure sticks

    Local locals[LHAT_MAX_LOCALS];
    size_t local_count;

    struct {
        const char *name;
        size_t length;
    } upvalue_names[256];

    // Slots below this hold live names; everything above is scratch for the
    // expression being compiled, released as soon as it is consumed.
    uint8_t next_register;

    LoopContext *loop;  // the innermost loop being compiled, NULL outside one

    // 04 の 2.4: a kind is the place it was declared, so the compiler keeps
    // one object per kind and hands the same one to every use. They live on
    // the outermost proto: a nested body making its own copy would give the
    // same declaration two identities.
    struct ErrorDecl *errors;
    size_t error_count;
    size_t error_capacity;
} Compiler;

typedef struct ErrorDecl {
    const char *name;
    size_t length;
    const LhatNode *node;         // the errordef^, for the field defaults
    const LhatErrorKind *group;   // stands for the whole declaration
    const LhatErrorKind **kinds;  // one per kind, in declaration order
    size_t kind_count;
} ErrorDecl;

static void fail(Compiler *c, LhatCompileStatus status)
{
    if (*c->status == LHAT_COMPILE_OK) {
        *c->status = status;
    }
}

// The trailing hats are not part of a name (01 の 2.3).
static bool node_name(const Compiler *c, const LhatNode *node,
                      const char **text, size_t *length)
{
    if (node == NULL) {
        return false;
    }
    // TYPE_NAME carries a name the same way: 13.11's is^ writes a type on the
    // right, and 04 の 14.4 lets that be a qualified error kind.
    if (node->kind == LHAT_NODE_IDENT || node->kind == LHAT_NODE_HAT_IDENT ||
        node->kind == LHAT_NODE_TYPE_NAME) {
        *text = c->lexer->source->text + node->v.name.offset;
        *length = node->v.name.length >= node->v.name.hats
                      ? node->v.name.length - node->v.name.hats
                      : node->v.name.length;
        return true;
    }
    if (node->kind == LHAT_NODE_FOCUS) {
        *text = "it";
        *length = 2;
        return true;
    }
    return false;
}

static bool name_is(const char *text, size_t length, const char *literal)
{
    size_t n = strlen(literal);
    return length == n && memcmp(text, literal, n) == 0;
}

static uint8_t reserve(Compiler *c)
{
    if (c->next_register >= LHAT_MAX_REGISTERS) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return 0;
    }
    uint8_t r = c->next_register++;
    if (c->next_register > c->proto->chunk.registers) {
        c->proto->chunk.registers = c->next_register;
    }
    return r;
}

static void emit(Compiler *c, LhatInstruction instruction)
{
    if (lhat_chunk_emit(&c->proto->chunk, instruction) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
}

static size_t emit_jump(Compiler *c, LhatOpcode op, uint8_t a)
{
    size_t at = lhat_chunk_emit(&c->proto->chunk, lhat_encode_jump(op, a, 0));
    if (at == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
    return at;
}

static void load_constant(Compiler *c, uint8_t into, LhatValue value)
{
    size_t k = lhat_chunk_constant(&c->proto->chunk, value);
    if (k == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

static const Local *find_local(const Compiler *c, const char *name,
                               size_t length)
{
    // Backwards, so an inner name shadows an outer one of the same spelling
    // as 02 の 8.6 intends.
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->length == length && memcmp(local->name, name, length) == 0) {
            return local;
        }
    }
    return NULL;
}

// 5.4: a name not held here is looked for in the enclosing subroutines, and
// found means a place to share rather than a value to copy. Each level on the
// way down records how it reaches the level above -- a register when the
// parent holds it, or one of the parent's own upvalues when it does not.
// Returns SIZE_MAX when there is no such name anywhere.
static size_t find_upvalue(Compiler *c, const char *name, size_t length)
{
    if (c->parent == NULL) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_names[i].length == length &&
            memcmp(c->upvalue_names[i].name, name, length) == 0) {
            return i;
        }
    }

    bool from_register = false;
    uint8_t index = 0;

    const Local *local = find_local(c->parent, name, length);
    if (local != NULL) {
        from_register = true;
        index = local->reg;
    } else {
        size_t outer = find_upvalue(c->parent, name, length);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        index = (uint8_t)outer;
    }

    size_t added = lhat_proto_add_upvalue(c->proto, from_register, index);
    if (added == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return SIZE_MAX;
    }
    c->upvalue_names[added].name = name;
    c->upvalue_names[added].length = length;
    return added;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into);
static void compile_statement(Compiler *c, const LhatNode *node);
static void compile_statements(Compiler *c, const LhatNode *statements);

// The outermost compiler. The kind objects and the registry of declarations
// live there so that a nested body sees the same ones the unit made.
static Compiler *root_of(Compiler *c)
{
    while (c->parent != NULL) {
        c = c->parent;
    }
    return c;
}

static const ErrorDecl *find_error_decl(Compiler *c, const char *name,
                                        size_t length)
{
    const Compiler *root = root_of(c);
    for (size_t i = 0; i < root->error_count; i++) {
        const ErrorDecl *decl = &root->errors[i];
        if (decl->length == length && memcmp(decl->name, name, length) == 0) {
            return decl;
        }
    }
    return NULL;
}

// 04 の 2.3: a declaration makes one type per kind and one for their union.
// This makes an object for each, on the root chunk, and remembers the node so
// that a construction can find the field defaults 2.2 gives it.
static void declare_error(Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.named.name, &name, &length)) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }
    if (find_error_decl(c, name, length) != NULL) {
        return;  // already registered; 8.7's pre-pass may reach it twice
    }

    Compiler *root = root_of(c);
    LhatChunk *chunk = &root->proto->chunk;

    if (root->error_count == root->error_capacity) {
        size_t grown = root->error_capacity ? root->error_capacity * 2 : 4;
        ErrorDecl *bigger =
            (ErrorDecl *)realloc(root->errors, grown * sizeof *bigger);
        if (bigger == NULL) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        root->errors = bigger;
        root->error_capacity = grown;
    }

    size_t kind_count = 0;
    for (const LhatNode *k = node->v.named.members; k != NULL; k = k->next) {
        kind_count++;
    }

    LhatString *group_name = lhat_string_new(&chunk->objects, name, length);
    const LhatErrorKind **kinds =
        (const LhatErrorKind **)calloc(kind_count ? kind_count : 1,
                                       sizeof *kinds);
    LhatErrorKind *group =
        lhat_error_kind_new(&chunk->objects, NULL, group_name);
    if (group_name == NULL || group == NULL || kinds == NULL) {
        free(kinds);
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    size_t index = 0;
    for (const LhatNode *k = node->v.named.members; k != NULL;
         k = k->next, index++) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (!node_name(c, k->v.named.name, &kind_name, &kind_length)) {
            free(kinds);
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // "IOError.NotFound" -- what typeof^ answers (2.3).
        char qualified[256];
        size_t total = length + 1 + kind_length;
        if (total >= sizeof qualified) {
            free(kinds);
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        memcpy(qualified, name, length);
        qualified[length] = '.';
        memcpy(qualified + length + 1, kind_name, kind_length);

        LhatString *text = lhat_string_new(&chunk->objects, qualified, total);
        LhatErrorKind *kind =
            text != NULL ? lhat_error_kind_new(&chunk->objects, group, text)
                         : NULL;
        if (kind == NULL) {
            free(kinds);
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        kinds[index] = kind;
    }

    ErrorDecl *decl = &root->errors[root->error_count++];
    decl->name = name;
    decl->length = length;
    decl->node = node;
    decl->group = group;
    decl->kinds = kinds;
    decl->kind_count = kind_count;
}

// 8.7's rule read for types: an errordef^ is visible across the scope it is
// written in, so the declarations are collected before anything is compiled.
static void declare_errors(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_ERRORDEF) {
            declare_error(c, s);
        }
    }
}

// Resolves the 'IOError' or 'IOError.NotFound' a use writes. `kind_node` comes
// back as the ERROR_KIND declaring the fields, or NULL when the whole
// declaration was named.
static const LhatErrorKind *resolve_kind(Compiler *c, const LhatNode *path,
                                         const LhatNode **kind_node)
{
    *kind_node = NULL;
    if (path == NULL) {
        return NULL;
    }

    const LhatNode *group_node = path;
    const LhatNode *member = NULL;
    if (path->kind == LHAT_NODE_MEMBER) {
        group_node = path->v.access.target;
        member = path->v.access.argument;
    }

    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, group_node, &name, &length)) {
        return NULL;
    }
    const ErrorDecl *decl = find_error_decl(c, name, length);
    if (decl == NULL) {
        return NULL;
    }
    if (member == NULL) {
        return decl->group;
    }

    const char *wanted = NULL;
    size_t wanted_length = 0;
    if (!node_name(c, member, &wanted, &wanted_length)) {
        return NULL;
    }
    size_t index = 0;
    for (const LhatNode *k = decl->node->v.named.members; k != NULL;
         k = k->next, index++) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (node_name(c, k->v.named.name, &kind_name, &kind_length) &&
            kind_length == wanted_length &&
            memcmp(kind_name, wanted, wanted_length) == 0) {
            *kind_node = k;
            return decl->kinds[index];
        }
    }
    return NULL;
}

static void load_string_bytes(Compiler *c, uint8_t into, const char *text,
                              size_t length)
{
    size_t k = lhat_chunk_string(&c->proto->chunk, text, length);
    if (k == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

// STRING and NAME both hold a span of the lexer's decoded bytes, so the
// escapes of 01 の 5 章 are already resolved by the time the compiler sees
// them.
static void load_string(Compiler *c, uint8_t into, const LhatNode *node)
{
    load_string_bytes(c, into, c->lexer->strings + node->v.string.offset,
                      node->v.string.length);
}

// The key of a member access or an index. 01 の 10.1 makes digits after a '.'
// an integer key, and a name after it a string key, so the two forms differ
// only in how the key was written.
static void compile_key(Compiler *c, const LhatNode *node, uint8_t into)
{
    const LhatNode *key = node->v.access.argument;
    if (node->kind == LHAT_NODE_INDEX) {
        compile_expression(c, key, into);
        return;
    }
    if (key == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }
    switch (key->kind) {
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            load_string_bytes(c, into, name, length);
            return;
        }
        case LHAT_NODE_NAME:
            load_string(c, into, key);
            return;
        case LHAT_NODE_INT:
            load_constant(c, into, lhat_integer((int64_t)key->v.integer.value));
            return;
        default:
            compile_expression(c, key, into);
            return;
    }
}

// Loads a kind object, which lives on the root chunk but is named from a
// constant of whichever chunk is being compiled.
static void load_kind(Compiler *c, uint8_t into, const LhatErrorKind *kind)
{
    size_t k = lhat_chunk_constant(&c->proto->chunk,
                                   lhat_object((LhatObject *)(void *)kind));
    if (k == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

// Whether the construction named this field.
static bool error_field_given(Compiler *c, const LhatNode *node,
                              const char *name, size_t length)
{
    for (const LhatNode *entry = node->v.named.members; entry != NULL;
         entry = entry->next) {
        const char *written = NULL;
        size_t written_length = 0;
        if (entry->v.entry.key != NULL &&
            node_name(c, entry->v.entry.key, &written, &written_length) &&
            written_length == length &&
            memcmp(written, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// 04 の 2.5: error^Kind{ … }. Every field without a default has to be given;
// one with a default may be left out, and 2.2 makes that default an
// expression evaluated at each construction -- so it is compiled here, at the
// construction, rather than stored anywhere.
static void compile_error_new(Compiler *c, const LhatNode *node, uint8_t into)
{
    const LhatNode *kind_node = NULL;
    const LhatErrorKind *kind = resolve_kind(c, node->v.named.name, &kind_node);
    if (kind == NULL || kind_node == NULL) {
        // Naming the declaration rather than one of its kinds leaves nothing
        // to construct: 2.3 makes it the union, not a type of its own.
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t holder = reserve(c);
    load_kind(c, holder, kind);
    emit(c, lhat_encode_abc(LHAT_BC_NEWERROR, into, holder, 0));
    c->next_register = mark;

    for (const LhatNode *entry = node->v.named.members; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.key == NULL ||
            !node_name(c, entry->v.entry.key, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        uint8_t at = c->next_register;
        uint8_t key = reserve(c);
        uint8_t value = reserve(c);
        load_string_bytes(c, key, name, length);
        compile_expression(c, entry->v.entry.value, value);
        emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, value));
        c->next_register = at;
    }

    // The declared fields the construction left out. 2.2 makes a default an
    // expression evaluated at each construction rather than a stored value,
    // which is why it is compiled here and not once at the declaration.
    for (const LhatNode *field = kind_node->v.named.members; field != NULL;
         field = field->next) {
        const LhatNode *fallback = field->v.param.fallback;
        if (fallback == NULL) {
            continue;  // no default; 2.5 required the construction to give it
        }
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, field->v.param.name, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        if (error_field_given(c, node, name, length)) {
            continue;
        }
        uint8_t at = c->next_register;
        uint8_t key = reserve(c);
        uint8_t value = reserve(c);
        load_string_bytes(c, key, name, length);
        compile_expression(c, fallback, value);
        emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, value));
        c->next_register = at;
    }

    // 2.3 gives every kind message and cause without either being declared.
    // cause defaults to nil^, which 11.3 already spells as the key not being
    // there, so only message needs writing.
    if (!error_field_given(c, node, "message", 7)) {
        uint8_t at = c->next_register;
        uint8_t key = reserve(c);
        uint8_t value = reserve(c);
        load_string_bytes(c, key, "message", 7);
        load_string_bytes(c, value, "", 0);
        emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, value));
        c->next_register = at;
    }
}

// 04 の 5.1: try^ hands the caller the error and keeps going otherwise. 5.6
// wants no unwinding for it, and none is needed -- returning is all it does.
static void compile_try(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_expression(c, node->v.jump.value, into);

    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISERROR, test, into, 0));
    size_t past = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    c->next_register = mark;

    emit(c, lhat_encode_abc(LHAT_BC_RETURN, into, 0, 0));
    lhat_chunk_patch_here(&c->proto->chunk, past);
}

// 04 の 4 章: catch^ replaces the value on the spot, and 4.2 names the error
// it^ inside the right side -- the same word 02 の 16.2 uses for a focus.
static void compile_catch(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_expression(c, node->v.binary.left, into);

    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    // it^ needs a place of its own: the right side writes its answer into
    // `into`, which is where the error still is.
    uint8_t caught = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_MOVE, caught, into, 0));

    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISERROR, test, caught, 0));
    size_t past = emit_jump(c, LHAT_BC_JUMP_FALSE, test);

    if (c->local_count < LHAT_MAX_LOCALS) {
        Local *local = &c->locals[c->local_count++];
        local->name = "it";
        local->length = 2;
        local->reg = caught;
    }
    compile_expression(c, node->v.binary.right, into);

    lhat_chunk_patch_here(&c->proto->chunk, past);
    c->local_count = local_mark;
    c->next_register = register_mark;
}

// 02 の 11.7: '??' is the same shape as catch^, asking about nil^ instead.
static void compile_nil_else(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_expression(c, node->v.binary.left, into);

    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, into, 0));
    size_t to_default = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    c->next_register = mark;

    compile_expression(c, node->v.binary.right, into);
    lhat_chunk_patch_here(&c->proto->chunk, to_default);
}

// 04 の 6.1: is^ against an error kind. 02 の 13.11 makes is^ a conformance
// test in general, which the checker performs; the machine only needs the one
// case the checker cannot settle on its own, which is which kind an error is.
static void compile_is(Compiler *c, const LhatNode *node, uint8_t into)
{
    const LhatNode *unused = NULL;
    const LhatErrorKind *kind = resolve_kind(c, node->v.binary.right, &unused);
    if (kind == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t value = reserve(c);
    uint8_t holder = reserve(c);
    compile_expression(c, node->v.binary.left, value);
    load_kind(c, holder, kind);
    emit(c, lhat_encode_abc(LHAT_BC_ISKIND, into, value, holder));
    c->next_register = mark;
}

// 02 の 14 章: a table literal makes a table and fills it in. A keyed entry
// names its key; a positional one takes the next integer, counting from 1 as
// 16.4 の 'for^ i := 1 to^ n' does.
static void compile_table(Compiler *c, const LhatNode *node, uint8_t into)
{
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, into, 0, 0));

    int64_t position = 0;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        uint8_t mark = c->next_register;
        uint8_t key = reserve(c);
        uint8_t value = reserve(c);

        if (entry->v.entry.key != NULL) {
            const LhatNode *named = entry->v.entry.key;
            const char *name = NULL;
            size_t length = 0;
            if (named->kind == LHAT_NODE_NAME) {
                load_string(c, key, named);
            } else if (node_name(c, named, &name, &length)) {
                load_string_bytes(c, key, name, length);
            } else {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
        } else {
            load_constant(c, key, lhat_integer(++position));
        }

        compile_expression(c, entry->v.entry.value, value);
        emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, value));
        c->next_register = mark;
    }
}

// 5.3: the callee sits in a register and its arguments follow it, so the
// machine can hand the callee's frame a contiguous run.
static void compile_call(Compiler *c, const LhatNode *node, uint8_t into)
{
    uint8_t mark = c->next_register;
    uint8_t callee = reserve(c);
    compile_expression(c, node->v.access.target, callee);

    size_t count = 0;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        uint8_t slot = reserve(c);
        compile_expression(c, arg, slot);
        count++;
    }
    if (count > 0xFF) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    emit(c, lhat_encode_abc(LHAT_BC_CALL, callee, (uint8_t)count, 0));
    if (into != callee) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, callee, 0));
    }
    c->next_register = mark;
}

// 02 の 15 章: f^ and p^ are both compiled the same way here; the difference
// they carry is for the checker, and 5.1 keeps the machine out of it.
static void compile_subroutine(Compiler *c, const LhatNode *node, uint8_t into)
{
    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    proto->is_function = node->v.func.is_function;

    size_t index = lhat_proto_add(c->proto, proto);
    if (index == SIZE_MAX) {
        lhat_proto_free(proto);
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    Compiler inner;
    memset(&inner, 0, sizeof inner);
    inner.parent = c;
    inner.lexer = c->lexer;
    inner.proto = proto;
    inner.status = c->status;

    // 5.3: the parameters are the frame's first registers, in order.
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        const char *name = NULL;
        size_t length = 0;
        if (param->v.param.variadic ||
            !node_name(&inner, param->v.param.name, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        if (inner.local_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        uint8_t slot = reserve(&inner);
        Local *local = &inner.locals[inner.local_count++];
        local->name = name;
        local->length = length;
        local->reg = slot;
        proto->parameters++;
    }

    const LhatNode *body = node->v.func.body;
    compile_statements(&inner, body != NULL ? body->v.list.items : NULL);
    if (lhat_chunk_emit(&proto->chunk,
                        lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0)) ==
        SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }

    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, into, (uint16_t)index));
}

static bool binary_opcode(LhatOpKind op, LhatOpcode *out)
{
    // The label is a token operator (01 の 7.1); the value is an opcode.
    switch (op) {
        case LHAT_OP_ADD:      *out = LHAT_BC_ADD;  return true;
        case LHAT_OP_SUB:      *out = LHAT_BC_SUB;  return true;
        case LHAT_OP_MUL:      *out = LHAT_BC_MUL;  return true;
        case LHAT_OP_DIV:      *out = LHAT_BC_DIV;  return true;
        case LHAT_OP_FLOORDIV: *out = LHAT_BC_IDIV; return true;
        case LHAT_OP_MOD:      *out = LHAT_BC_MOD;  return true;
        case LHAT_OP_POW:      *out = LHAT_BC_POW;  return true;
        case LHAT_OP_EQ:       *out = LHAT_BC_EQ;   return true;
        case LHAT_OP_NE:       *out = LHAT_BC_NE;   return true;
        case LHAT_OP_LT:       *out = LHAT_BC_LT;   return true;
        case LHAT_OP_GT:       *out = LHAT_BC_GT;   return true;
        case LHAT_OP_LE:       *out = LHAT_BC_LE;   return true;
        case LHAT_OP_GE:       *out = LHAT_BC_GE;   return true;
        default: return false;
    }
}

static void compile_binary(Compiler *c, const LhatNode *node, uint8_t into)
{
    LhatOpKind op = node->v.binary.op;

    // These three read the left side and then decide whether to bother with
    // the right, so like and^ and or^ they are branches rather than one
    // instruction.
    if (op == LHAT_OP_CATCH) {
        compile_catch(c, node, into);
        return;
    }
    if (op == LHAT_OP_NIL_ELSE) {
        compile_nil_else(c, node, into);
        return;
    }
    if (op == LHAT_OP_IS) {
        compile_is(c, node, into);
        return;
    }

    // 11.6: and^ and or^ decide without evaluating the right side when the
    // left has settled it, so they are branches rather than instructions.
    if (op == LHAT_OP_AND || op == LHAT_OP_OR) {
        compile_expression(c, node->v.binary.left, into);
        if (op == LHAT_OP_OR) {
            // Skip the right side when the left is already true: jump over a
            // jump, since the only test instruction asks about false.
            size_t to_right = emit_jump(c, LHAT_BC_JUMP_FALSE, into);
            size_t done = emit_jump(c, LHAT_BC_JUMP, 0);
            lhat_chunk_patch_here(&c->proto->chunk, to_right);
            compile_expression(c, node->v.binary.right, into);
            lhat_chunk_patch_here(&c->proto->chunk, done);
            return;
        }
        size_t done = emit_jump(c, LHAT_BC_JUMP_FALSE, into);
        compile_expression(c, node->v.binary.right, into);
        lhat_chunk_patch_here(&c->proto->chunk, done);
        return;
    }

    LhatOpcode opcode;
    if (!binary_opcode(op, &opcode)) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // Operands go above the names, and the scratch is given back as soon as
    // the instruction has consumed it.
    uint8_t mark = c->next_register;
    uint8_t left = reserve(c);
    uint8_t right = reserve(c);
    compile_expression(c, node->v.binary.left, left);
    compile_expression(c, node->v.binary.right, right);
    emit(c, lhat_encode_abc(opcode, into, left, right));
    c->next_register = mark;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node == NULL || *c->status != LHAT_COMPILE_OK) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_INT:
            load_constant(c, into, lhat_integer((int64_t)node->v.integer.value));
            return;

        case LHAT_NODE_FLOAT:
            load_constant(c, into, lhat_real(node->v.real));
            return;

        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
            load_string(c, into, node);
            return;

        case LHAT_NODE_TABLE:
            compile_table(c, node, into);
            return;

        case LHAT_NODE_ERROR_NEW:
            compile_error_new(c, node, into);
            return;

        case LHAT_NODE_TRY:
            compile_try(c, node, into);
            return;

        // 04 の 11.3: 't.foo' is resolved statically and 't[k]' is not, but
        // the machine performs one lookup either way. 5.1 keeps the checker's
        // knowledge out of the instruction set until specialisation.
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX: {
            uint8_t mark = c->next_register;
            uint8_t target = reserve(c);
            uint8_t key = reserve(c);
            compile_expression(c, node->v.access.target, target);
            compile_key(c, node, key);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, target, key));
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_IDENT:
        case LHAT_NODE_FOCUS:
        case LHAT_NODE_HAT_IDENT: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            // 01 の 2.2: three hat identifiers are values in themselves.
            // Everything else with a hat is a name like any other -- 16.2's
            // it^ is a name the for^ made, so it is looked up rather than
            // recognised.
            if (node->kind == LHAT_NODE_HAT_IDENT) {
                if (name_is(name, length, "true") ||
                    name_is(name, length, "false")) {
                    emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, into,
                                            name_is(name, length, "true") ? 1 : 0,
                                            0));
                    return;
                }
                if (name_is(name, length, "nil")) {
                    emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                    return;
                }
            }
            const Local *local = find_local(c, name, length);
            if (local != NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, local->reg, 0));
                return;
            }
            size_t upvalue = find_upvalue(c, name, length);
            if (upvalue == SIZE_MAX) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
            return;
        }

        case LHAT_NODE_UNARY: {
            uint8_t mark = c->next_register;
            uint8_t operand = reserve(c);
            compile_expression(c, node->v.unary.operand, operand);
            emit(c, lhat_encode_abc(node->v.unary.op == LHAT_OP_NOT
                                        ? LHAT_BC_NOT
                                        : LHAT_BC_NEG,
                                    into, operand, 0));
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_BINARY:
            compile_binary(c, node, into);
            return;

        case LHAT_NODE_CALL:
            compile_call(c, node, into);
            return;

        case LHAT_NODE_FUNC:
            compile_subroutine(c, node, into);
            return;

        case LHAT_NODE_IF_EXPR: {
            // 5.1 の (5.2): every clause writes into the same register, so
            // the value of the whole expression is wherever control lands.
            size_t leaving[LHAT_MAX_LOCALS];
            size_t leaving_count = 0;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    compile_expression(c, clause->v.clause.body, into);
                    break;
                }

                uint8_t mark = c->next_register;
                uint8_t test = reserve(c);
                compile_expression(c, condition, test);
                size_t next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = mark;

                compile_expression(c, clause->v.clause.body, into);
                if (leaving_count < LHAT_MAX_LOCALS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            return;
        }

        default:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
    }
}

// The name a let^ target carries, whether or not it was annotated.
static const LhatNode *define_target_name(const LhatNode *target)
{
    return target->kind == LHAT_NODE_PARAM ? target->v.param.name : target;
}

// 8.7: a let^ name is visible across the whole scope, written before its own
// definition or after it. So every slot in a block is made before anything is
// compiled into it -- which is what lets a body call itself, and two bodies
// call each other, with nothing declared ahead of them.
static void declare_names(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        for (const LhatNode *target = s->v.binding.targets; target != NULL;
             target = target->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, define_target_name(target), &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            if (c->local_count >= LHAT_MAX_LOCALS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }

            uint8_t slot = reserve(c);
            // The slot may still hold what an earlier block left in it, and
            // 8.7 lets the name be read from a body before its let^ has run.
            // Emptying it makes that nil^ rather than rubbish.
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));

            Local *local = &c->locals[c->local_count++];
            local->name = name;
            local->length = length;
            local->reg = slot;
        }
    }
}

// 02 の 8.6: let^ makes a name, and the name is a slot that stays. The slot
// itself was made by declare_names; this fills it.
static void compile_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, define_target_name(target), &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        const Local *local = find_local(c, name, length);
        if (local == NULL) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        if (value != NULL) {
            compile_expression(c, value, local->reg);
            value = value->next;
        }
    }
}

static void compile_reassign(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        // A member or an index is a place too, and the only kind that is not
        // a name. It never reaches the upvalue path below: what it reassigns
        // belongs to the table, not to a frame.
        if (target->kind == LHAT_NODE_MEMBER ||
            target->kind == LHAT_NODE_INDEX) {
            if (value == NULL) {
                continue;
            }
            uint8_t mark = c->next_register;
            uint8_t into = reserve(c);
            uint8_t key = reserve(c);
            uint8_t slot = reserve(c);
            compile_expression(c, target->v.access.target, into);
            compile_key(c, target, key);
            compile_expression(c, value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, slot));
            c->next_register = mark;
            value = value->next;
            continue;
        }

        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, target, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        const Local *local = find_local(c, name, length);
        if (local != NULL) {
            if (value != NULL) {
                compile_expression(c, value, local->reg);
                value = value->next;
            }
            continue;
        }

        // 8.6 is the reason 5.4 shares a place rather than a value: a ':='
        // written inside a nested subroutine has to reach the outer binding.
        size_t upvalue = find_upvalue(c, name, length);
        if (upvalue == SIZE_MAX) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        if (value != NULL) {
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, slot, (uint8_t)upvalue, 0));
            c->next_register = mark;
            value = value->next;
        }
    }
}

static void compile_statements(Compiler *c, const LhatNode *statements)
{
    // A block's names end with it, and so do their slots.
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    declare_errors(c, statements);
    declare_names(c, statements);
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        compile_statement(c, s);
    }

    // The slots go back to the pool, so anything sharing one has to stop
    // sharing it first -- otherwise a closure made inside the block would read
    // whatever the next block puts there.
    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }

    c->local_count = local_mark;
    c->next_register = register_mark;
}

// The statements go into the scope that is already open, so the names they
// make outlive them. 9.7 needs this: first^ runs inside the loop but declares
// outside it, in the same storage prolog^ uses.
static void compile_in_scope(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        compile_statement(c, s);
    }
}

// 9 章: the clauses of a loop body other than main^, which the parser leaves
// in `items` whether or not it was written with a heading.
static const LhatNode *clause_of(const LhatNode *body, LhatClauseKind kind)
{
    if (body == NULL) {
        return NULL;
    }
    for (const LhatNode *clause = body->v.list.extra; clause != NULL;
         clause = clause->next) {
        if (clause->v.loop_clause.kind == kind) {
            return clause->v.loop_clause.body;
        }
    }
    return NULL;
}

// The one name a numeric focus binds. 16.3's to^/downto^ advance the focus
// themselves, so they need a single name to advance, unlike the other forms.
static const Local *numeric_focus(Compiler *c, const LhatNode *focus)
{
    if (focus == NULL || focus->next != NULL ||
        focus->kind != LHAT_NODE_DEFINE) {
        return NULL;
    }
    const LhatNode *target = focus->v.binding.targets;
    if (target == NULL || target->next != NULL) {
        return NULL;
    }
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, define_target_name(target), &name, &length)) {
        return NULL;
    }
    return find_local(c, name, length);
}

// 16.4: to^ and downto^ are sugar over the conditional form, and this is that
// expansion -- 'i ≦ B' one way round and 'i ≧ B' the other. The bound was read
// once before the loop (16.4), so the test only compares.
static void compile_numeric_test(Compiler *c, const LhatNode *node,
                                 const Local *focus, uint8_t bound,
                                 uint8_t into)
{
    emit(c, lhat_encode_abc(node->v.loop.kind == LHAT_FOR_TO ? LHAT_BC_LE
                                                             : LHAT_BC_GE,
                            into, focus->reg, bound));
}

// 16.4: the sign belongs to to^ and downto^, so step^ is a positive amount
// and the expansion adds it one way or subtracts it the other. Like the
// bound, it was read once before the loop.
static void compile_numeric_advance(Compiler *c, const LhatNode *node,
                                    const Local *focus, uint8_t step)
{
    emit(c, lhat_encode_abc(node->v.loop.kind == LHAT_FOR_TO ? LHAT_BC_ADD
                                                             : LHAT_BC_SUB,
                            focus->reg, focus->reg, step));
}

// 16.3's if^ clause, which 16.1 explains is not a loop: the focus is
// introduced, used once, and goes. Written out it is the do^ block of 16.3.
static void compile_for_if(Compiler *c, const LhatNode *node)
{
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    declare_names(c, node->v.loop.focus);
    compile_in_scope(c, node->v.loop.focus);
    compile_statement(c, node->v.loop.body);

    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }
    c->local_count = local_mark;
    c->next_register = register_mark;
}

// Both for^ and repeat^ compile to the same shape; what differs is the focus
// and how the condition and the advance are written.
//
//     <focus>                     16.7: it lives across the whole loop
//     <prolog^>                   9.1: runs whether the condition holds or not
//   top:
//     <condition> false -> end
//     <save the focus>            9.7: at the head, so break^ sees this one
//     <first^> once
//     <main^>                     9.4: a new scope each time round
//     <advance>
//     jump top
//   end:                          9.8: where break^ lands
//     <last^> if the loop ever ran
//     <epilog^>
static void compile_loop(Compiler *c, const LhatNode *node)
{
    bool is_for = node->kind == LHAT_NODE_FOR;
    const LhatNode *body = is_for ? node->v.loop.body : node->v.repeat.body;
    const LhatNode *bound = is_for ? node->v.loop.bound : node->v.repeat.bound;
    const LhatNode *advance = is_for ? node->v.loop.advance : NULL;
    const LhatNode *focus = is_for ? node->v.loop.focus : NULL;

    int kind = is_for ? (int)node->v.loop.kind : -1;
    if (is_for && (kind == LHAT_FOR_IN || kind == LHAT_FOR_WHEN)) {
        // in^ takes an iterator, which 13.9 makes a coroutine (P5), and when^
        // is the pattern match of 17 章. Neither has a machine yet.
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    const LhatNode *prolog = clause_of(body, LHAT_CLAUSE_PROLOG);
    const LhatNode *first = clause_of(body, LHAT_CLAUSE_FIRST);
    const LhatNode *last = clause_of(body, LHAT_CLAUSE_LAST);
    const LhatNode *epilog = clause_of(body, LHAT_CLAUSE_EPILOG);
    if (clause_of(body, LHAT_CLAUSE_FINALLY) != NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);  // 5.5: the frame queue comes later
        return;
    }

    // 16.7 and 9.4: the focus and the names of prolog^ and first^ last for
    // the whole loop, so they are made in a scope outside it.
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    declare_names(c, focus);
    compile_in_scope(c, focus);
    size_t focus_locals = c->local_count - local_mark;

    // 16.4: the bound and the step^ of to^/downto^ are both read once, before
    // the loop. Together they say how far the loop goes and in what
    // increments, and re-reading either would let that move while it runs.
    const Local *numeric = NULL;
    uint8_t numeric_bound = 0;
    uint8_t numeric_step = 0;
    if (kind == LHAT_FOR_TO || kind == LHAT_FOR_DOWNTO) {
        numeric = numeric_focus(c, focus);
        if (numeric == NULL) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        numeric_bound = reserve(c);
        compile_expression(c, bound, numeric_bound);
        numeric_step = reserve(c);
        if (node->v.loop.step != NULL) {
            compile_expression(c, node->v.loop.step, numeric_step);
        } else {
            load_constant(c, numeric_step, lhat_integer(1));
        }
    }

    // repeat^ n counts to a limit read once, for the same reason: 'n 回' says
    // how many times, so it cannot change under the loop.
    uint8_t counter = 0;
    uint8_t limit = 0;
    if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        limit = reserve(c);
        compile_expression(c, bound, limit);
        counter = reserve(c);
        load_constant(c, counter, lhat_integer(0));
    }

    declare_names(c, prolog);
    declare_names(c, first);
    compile_in_scope(c, prolog);

    // 9.7: one bool answers both "has first^ run" and "did the loop ever run",
    // and only a loop that asks for it pays for it.
    bool need_entered = first != NULL || last != NULL;
    uint8_t entered = 0;
    if (need_entered) {
        entered = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, entered, 0, 0));
    }

    // 9.7: last^ has to see the value the condition last accepted, not the one
    // that ended the loop, so each iteration keeps a copy of the focus.
    uint8_t saved = 0;
    size_t saved_count = last != NULL ? focus_locals : 0;
    if (saved_count > 0) {
        saved = c->next_register;
        for (size_t i = 0; i < saved_count; i++) {
            (void)reserve(c);
        }
    }

    LoopContext context;
    context.enclosing = c->loop;
    context.count = 0;
    c->loop = &context;

    size_t top = c->proto->chunk.count;
    size_t leaving = SIZE_MAX;

    if (kind == LHAT_FOR_TO || kind == LHAT_FOR_DOWNTO) {
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        compile_numeric_test(c, node, numeric, numeric_bound, test);
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
    } else if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_LT, test, counter, limit));
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
    } else if (bound != NULL) {
        // 16.5: until^ is while^ negated, and the negation is all there is
        // to it -- the test happens at the same place either way.
        bool negated = is_for ? kind == LHAT_FOR_UNTIL
                              : node->v.repeat.kind == LHAT_REPEAT_UNTIL;
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        compile_expression(c, bound, test);
        if (negated) {
            emit(c, lhat_encode_abc(LHAT_BC_NOT, test, test, 0));
        }
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
    }

    for (size_t i = 0; i < saved_count; i++) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, (uint8_t)(saved + i),
                                c->locals[local_mark + i].reg, 0));
    }

    if (first != NULL) {
        size_t to_first = emit_jump(c, LHAT_BC_JUMP_FALSE, entered);
        size_t past_first = emit_jump(c, LHAT_BC_JUMP, 0);
        lhat_chunk_patch_here(&c->proto->chunk, to_first);
        compile_in_scope(c, first);
        lhat_chunk_patch_here(&c->proto->chunk, past_first);
    }
    if (need_entered) {
        emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, entered, 1, 0));
    }

    // 9.4: what main^ declares lives one iteration, so it gets its own scope.
    compile_statements(c, body != NULL ? body->v.list.items : NULL);

    if (advance != NULL) {
        compile_in_scope(c, advance);
    } else if (numeric != NULL) {
        compile_numeric_advance(c, node, numeric, numeric_step);
    } else if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        uint8_t mark = c->next_register;
        uint8_t one = reserve(c);
        load_constant(c, one, lhat_integer(1));
        emit(c, lhat_encode_abc(LHAT_BC_ADD, counter, counter, one));
        c->next_register = mark;
    }

    // Backwards, so lhat_chunk_patch_here -- which only ever aims at the end
    // of what has been emitted -- cannot write it.
    size_t back = emit_jump(c, LHAT_BC_JUMP, 0);
    if (back != SIZE_MAX) {
        int32_t offset = (int32_t)top - (int32_t)back - 1;
        c->proto->chunk.code[back] = lhat_encode_jump(LHAT_BC_JUMP, 0, offset);
    }

    if (leaving != SIZE_MAX) {
        lhat_chunk_patch_here(&c->proto->chunk, leaving);
    }
    for (size_t i = 0; i < context.count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, context.jumps[i]);
    }
    c->loop = context.enclosing;

    if (last != NULL) {
        size_t skip = emit_jump(c, LHAT_BC_JUMP_FALSE, entered);
        for (size_t i = 0; i < saved_count; i++) {
            emit(c, lhat_encode_abc(LHAT_BC_MOVE,
                                    c->locals[local_mark + i].reg,
                                    (uint8_t)(saved + i), 0));
        }
        compile_in_scope(c, last);
        lhat_chunk_patch_here(&c->proto->chunk, skip);
    }
    compile_in_scope(c, epilog);

    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }
    c->local_count = local_mark;
    c->next_register = register_mark;
}

static void compile_statement(Compiler *c, const LhatNode *node)
{
    if (node == NULL || *c->status != LHAT_COMPILE_OK) {
        return;
    }

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
            compile_define(c, node);
            return;

        case LHAT_NODE_REASSIGN:
            compile_reassign(c, node);
            return;

        case LHAT_NODE_BLOCK:
            compile_statements(c, node->v.list.items);
            return;

        case LHAT_NODE_RETURN: {
            if (node->v.jump.value == NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->v.jump.value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_RETURN, slot, 0, 0));
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_IF_STMT: {
            size_t leaving[LHAT_MAX_LOCALS];
            size_t leaving_count = 0;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    compile_statement(c, clause->v.clause.body);
                    break;
                }

                uint8_t mark = c->next_register;
                uint8_t test = reserve(c);
                compile_expression(c, condition, test);
                size_t next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = mark;

                compile_statement(c, clause->v.clause.body);
                if (clause->next != NULL && leaving_count < LHAT_MAX_LOCALS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            return;
        }

        case LHAT_NODE_CALL_STMT: {
            // 02 の 8.2: a call may stand alone, and its value is discarded.
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->v.jump.value, slot);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_FOR:
            // 16.1: for^ introduces a value; whether it repeats is up to the
            // clause after it, and if^ is the clause that does not.
            if (node->v.loop.kind == LHAT_FOR_IF) {
                compile_for_if(c, node);
            } else {
                compile_loop(c, node);
            }
            return;

        case LHAT_NODE_REPEAT:
            compile_loop(c, node);
            return;

        case LHAT_NODE_BREAK: {
            if (c->loop == NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            // 9.8 marks the multi-level form still a proposal, so the level
            // is refused rather than guessed at.
            if (node->v.jump.value != NULL ||
                c->loop->count >= LHAT_MAX_BREAKS) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            c->loop->jumps[c->loop->count++] = emit_jump(c, LHAT_BC_JUMP, 0);
            return;
        }

        case LHAT_NODE_TRY: {
            // 04 の 5.1: written as a statement when the value is not wanted.
            // The error still leaves, which is the point of writing it.
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_try(c, node, slot);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_ERRORDEF:
            // 04 の 2.2: a declaration. declare_errors made its kinds before
            // anything was compiled, and there is nothing to run.
            return;

        case LHAT_NODE_MODULE:
            return;  // 05 の 3 章: a name for the unit, nothing to run

        default:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
    }
}

LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out)
{
    *out = NULL;
    if (unit == NULL || lexer == NULL) {
        return LHAT_COMPILE_UNSUPPORTED;
    }

    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        return LHAT_COMPILE_TOO_COMPLEX;
    }

    LhatCompileStatus status = LHAT_COMPILE_OK;

    Compiler c;
    memset(&c, 0, sizeof c);
    c.lexer = lexer;
    c.proto = proto;
    c.status = &status;

    // The unit is a scope like any other, so 8.7 applies to it too.
    compile_statements(&c, unit->v.list.items);
    emit(&c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));

    // The registry was the compiler's; the kind objects it points at belong
    // to the chunk and stay.
    for (size_t i = 0; i < c.error_count; i++) {
        free((void *)c.errors[i].kinds);
    }
    free(c.errors);

    if (status != LHAT_COMPILE_OK) {
        lhat_proto_free(proto);
        return status;
    }
    *out = proto;
    return status;
}

const char *lhat_compile_status_message(LhatCompileStatus status)
{
    switch (status) {
        case LHAT_COMPILE_OK:          return "compiled";
        case LHAT_COMPILE_UNSUPPORTED: return "this form does not compile yet";
        case LHAT_COMPILE_TOO_COMPLEX: return "too many registers or constants";
        case LHAT_COMPILE_UNDEFINED:   return "no such name";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

// 5.1: the generic form checks. 02 の 14.8 makes number^ one type with two
// representations, so an operation stays in integers when both sides are and
// widens only when one of them is real.
static bool arithmetic(LhatOpcode op, LhatValue left, LhatValue right,
                       LhatValue *out, LhatRunStatus *status)
{
    if (!lhat_is_number(left) || !lhat_is_number(right)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }

    bool exact = lhat_is_integer(left) && lhat_is_integer(right);
    double a = lhat_number_as_real(left);
    double b = lhat_number_as_real(right);

    switch (op) {
        case LHAT_BC_ADD:
            *out = exact ? lhat_integer(lhat_as_integer(left) +
                                        lhat_as_integer(right))
                         : lhat_real(a + b);
            return true;
        case LHAT_BC_SUB:
            *out = exact ? lhat_integer(lhat_as_integer(left) -
                                        lhat_as_integer(right))
                         : lhat_real(a - b);
            return true;
        case LHAT_BC_MUL:
            *out = exact ? lhat_integer(lhat_as_integer(left) *
                                        lhat_as_integer(right))
                         : lhat_real(a * b);
            return true;
        case LHAT_BC_DIV:
            // 04 の 11.2: real division, so a zero divisor gives inf rather
            // than failing. That is what keeps ordinary arithmetic out of the
            // unions.
            *out = lhat_real(a / b);
            return true;
        case LHAT_BC_IDIV:
        case LHAT_BC_MOD: {
            if (b == 0) {
                *status = LHAT_RUN_DIVIDE_BY_ZERO;
                return false;
            }
            if (exact) {
                int64_t x = lhat_as_integer(left);
                int64_t y = lhat_as_integer(right);
                int64_t quotient = x / y;
                int64_t remainder = x % y;
                // Floor rather than truncate, so that '%' agrees in sign with
                // the divisor as it does in Lua.
                if (remainder != 0 && ((remainder < 0) != (y < 0))) {
                    quotient--;
                    remainder += y;
                }
                *out = op == LHAT_BC_IDIV ? lhat_integer(quotient)
                                          : lhat_integer(remainder);
                return true;
            }
            double quotient = a / b;
            double floored = quotient - (quotient < 0 ? 1.0 : 0.0);
            double truncated = (double)(int64_t)quotient;
            floored = quotient < 0 && truncated != quotient ? truncated - 1
                                                            : truncated;
            *out = op == LHAT_BC_IDIV ? lhat_real(floored)
                                      : lhat_real(a - floored * b);
            return true;
        }
        case LHAT_BC_POW: {
            double r = 1.0;
            *out = lhat_real(r);
            double base = a;
            double exponent = b;
            // Kept out of libm so the core has no maths dependency yet; only
            // whole exponents are wanted before the standard library lands.
            int64_t times = (int64_t)exponent;
            if ((double)times != exponent) {
                *status = LHAT_RUN_TYPE_ERROR;
                return false;
            }
            bool invert = times < 0;
            if (invert) {
                times = -times;
            }
            for (int64_t i = 0; i < times; i++) {
                r *= base;
            }
            *out = lhat_real(invert ? 1.0 / r : r);
            return true;
        }
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

static bool ordering(LhatOpcode op, LhatValue left, LhatValue right,
                     bool *out, LhatRunStatus *status)
{
    if (!lhat_is_number(left) || !lhat_is_number(right)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }
    double a = lhat_number_as_real(left);
    double b = lhat_number_as_real(right);
    switch (op) {
        case LHAT_BC_LT: *out = a < b;  return true;
        case LHAT_BC_LE: *out = a <= b; return true;
        case LHAT_BC_GT: *out = a > b;  return true;
        case LHAT_BC_GE: *out = a >= b; return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

#define LHAT_STACK_SLOTS 8192
#define LHAT_MAX_FRAMES 200

typedef struct {
    const LhatClosure *closure;
    size_t pc;
    LhatValue *base;   // 5.2: the frame's registers start here
    uint8_t result;    // where in the caller's frame the answer goes
} Frame;

typedef struct {
    LhatValue stack[LHAT_STACK_SLOTS];
    Frame frames[LHAT_MAX_FRAMES];
    size_t frame_count;

    // Everything allocated while running, so it can all be released at the
    // end. A collector replaces this; until then a program frees on exit.
    LhatObject *objects;
    LhatUpvalue *open;  // 5.4, innermost first
} Machine;

// What an index reads from. 04 の 2.3 gives every error message and cause
// without declaring them, so an error answers a member the same way a table
// does -- from the table its fields live in.
static LhatTable *table_of(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return (LhatTable *)lhat_as_object(value);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
        return ((LhatError *)lhat_as_object(value))->fields;
    }
    return NULL;
}

static void *allocate(Machine *m, size_t size, LhatObjectKind kind)
{
    LhatObject *object = (LhatObject *)calloc(1, size);
    if (object == NULL) {
        return NULL;
    }
    object->kind = kind;
    object->next = m->objects;
    m->objects = object;
    return object;
}

// 5.4: one place per slot, so two closures capturing the same name share it.
static LhatUpvalue *capture(Machine *m, LhatValue *slot)
{
    LhatUpvalue **link = &m->open;
    while (*link != NULL && (*link)->location > slot) {
        link = &(*link)->next_open;
    }
    if (*link != NULL && (*link)->location == slot) {
        return *link;
    }

    LhatUpvalue *upvalue =
        (LhatUpvalue *)allocate(m, sizeof *upvalue, LHAT_OBJECT_UPVALUE);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = slot;
    upvalue->next_open = *link;
    *link = upvalue;
    return upvalue;
}

// The frame is going, so anything still pointing into it carries its value
// away. Without this a closure returned from a subroutine would read a slot
// that has been reused.
static void close_upvalues(Machine *m, const LhatValue *above)
{
    while (m->open != NULL && m->open->location >= above) {
        LhatUpvalue *upvalue = m->open;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        m->open = upvalue->next_open;
        upvalue->next_open = NULL;
    }
}

static LhatRunResult finish(Machine *m, LhatRunStatus status, LhatValue value,
                            size_t at)
{
    // The answer may be a table or a string, so what the run allocated cannot
    // be freed here -- the value would point at it. Ownership passes to the
    // caller instead. A collector replaces this by freeing everything the
    // answer does not reach.
    LhatRunResult result;
    result.status = status;
    result.value = value;
    result.at = at;
    result.objects = m->objects;
    m->objects = NULL;
    m->open = NULL;
    return result;
}

void lhat_run_result_dispose(LhatRunResult *result)
{
    lhat_object_free_all(&result->objects);
    result->value = lhat_nil();
}

LhatRunResult lhat_run(const LhatProto *proto)
{
    static Machine machine;  // large enough that the stack is the wrong place
    memset(&machine, 0, sizeof machine);
    Machine *m = &machine;

    for (size_t i = 0; i < LHAT_STACK_SLOTS; i++) {
        m->stack[i] = lhat_nil();
    }

    LhatClosure *entry =
        (LhatClosure *)allocate(m, sizeof *entry, LHAT_OBJECT_SUBROUTINE);
    if (entry == NULL) {
        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
    }
    entry->proto = proto;

    Frame *frame = &m->frames[m->frame_count++];
    frame->closure = entry;
    frame->pc = 0;
    frame->base = m->stack;
    frame->result = 0;

    LhatValue *registers = frame->base;
    const LhatChunk *chunk = &proto->chunk;
    size_t pc = 0;

    while (pc < chunk->count) {
        LhatInstruction instruction = chunk->code[pc];
        size_t at = pc++;

        uint8_t a = lhat_a(instruction);
        uint8_t b = lhat_b(instruction);
        uint8_t cc = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);

        switch (op) {
            case LHAT_BC_LOADK:
                registers[a] = chunk->constants[lhat_bx(instruction)];
                break;
            case LHAT_BC_LOADNIL:
                registers[a] = lhat_nil();
                break;
            case LHAT_BC_LOADBOOL:
                registers[a] = lhat_bool(b != 0);
                break;
            case LHAT_BC_MOVE:
                registers[a] = registers[b];
                break;

            case LHAT_BC_ADD:
            case LHAT_BC_SUB:
            case LHAT_BC_MUL:
            case LHAT_BC_DIV:
            case LHAT_BC_IDIV:
            case LHAT_BC_MOD:
            case LHAT_BC_POW: {
                LhatValue out;
                LhatRunStatus status = LHAT_RUN_OK;
                if (!arithmetic(op, registers[b], registers[cc], &out,
                                &status)) {
                    return finish(m, status, lhat_nil(), at);
                }
                registers[a] = out;
                break;
            }

            case LHAT_BC_NEG: {
                if (!lhat_is_number(registers[b])) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                registers[a] = lhat_is_integer(registers[b])
                                   ? lhat_integer(-lhat_as_integer(registers[b]))
                                   : lhat_real(-lhat_as_real(registers[b]));
                break;
            }

            case LHAT_BC_NOT: {
                // 02 の 8.6's condition rule: only a bool is a truth value,
                // so this refuses anything else rather than inventing one.
                if (!lhat_is_bool(registers[b])) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                registers[a] = lhat_bool(!lhat_as_bool(registers[b]));
                break;
            }

            case LHAT_BC_EQ:
                registers[a] = lhat_bool(
                    lhat_value_equal(registers[b], registers[cc]));
                break;
            case LHAT_BC_NE:
                registers[a] = lhat_bool(
                    !lhat_value_equal(registers[b], registers[cc]));
                break;

            case LHAT_BC_LT:
            case LHAT_BC_LE:
            case LHAT_BC_GT:
            case LHAT_BC_GE: {
                bool out = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (!ordering(op, registers[b], registers[cc], &out, &status)) {
                    return finish(m, status, lhat_nil(), at);
                }
                registers[a] = lhat_bool(out);
                break;
            }

            case LHAT_BC_JUMP:
                pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                break;

            case LHAT_BC_JUMP_FALSE:
                if (!lhat_is_bool(registers[a])) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (!lhat_as_bool(registers[a])) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;

            case LHAT_BC_CLOSURE: {
                const LhatProto *nested =
                    frame->closure->proto->protos[lhat_bx(instruction)];
                LhatClosure *closure = (LhatClosure *)allocate(
                    m, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = nested;
                closure->upvalue_count = nested->upvalue_count;
                if (nested->upvalue_count > 0) {
                    closure->upvalues = (LhatUpvalue **)calloc(
                        nested->upvalue_count, sizeof *closure->upvalues);
                    if (closure->upvalues == NULL) {
                        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                // 5.4: a register of this frame, or one of its own upvalues
                // when the name came from further out.
                for (size_t i = 0; i < nested->upvalue_count; i++) {
                    const LhatUpvalueDesc *desc = &nested->upvalues[i];
                    closure->upvalues[i] =
                        desc->from_parent_register
                            ? capture(m, &registers[desc->index])
                            : frame->closure->upvalues[desc->index];
                    if (closure->upvalues[i] == NULL) {
                        return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                registers[a] = lhat_object((LhatObject *)closure);
                break;
            }

            case LHAT_BC_GETUPVAL:
                registers[a] = *frame->closure->upvalues[b]->location;
                break;

            case LHAT_BC_SETUPVAL:
                *frame->closure->upvalues[b]->location = registers[a];
                break;

            case LHAT_BC_CLOSE:
                close_upvalues(m, &registers[a]);
                break;

            case LHAT_BC_NEWTABLE: {
                LhatTable *table = lhat_table_new(&m->objects);
                if (table == NULL) {
                    return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                registers[a] = lhat_object((LhatObject *)table);
                break;
            }

            // 04 の 11.3: a key that is not there answers nil^, so the only
            // way this fails is being asked of something that is not a table.
            // An error answers from its fields: 2.3 gives every kind message
            // and cause, and they are reached the same way as a member.
            case LHAT_BC_GETINDEX: {
                const LhatTable *table = table_of(registers[b]);
                if (table == NULL) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                registers[a] = lhat_table_get(table, registers[cc]);
                break;
            }

            case LHAT_BC_SETINDEX: {
                LhatTable *table = table_of(registers[a]);
                if (table == NULL) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                bool refused = false;
                if (!lhat_table_set(table, registers[b], registers[cc],
                                    &refused)) {
                    return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // nil^ is how 11.3 spells "not there", so it cannot also be
                // a key. Neither can a NaN, which is not equal to itself.
                if (refused) {
                    return finish(m, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            case LHAT_BC_NEWERROR: {
                if (!lhat_is_object_kind(registers[b], LHAT_OBJECT_ERROR_KIND)) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatErrorKind *kind =
                    (const LhatErrorKind *)lhat_as_object(registers[b]);
                LhatError *error = lhat_error_new(&m->objects, kind);
                if (error == NULL) {
                    return finish(m, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                registers[a] = lhat_object((LhatObject *)error);
                break;
            }

            // 04 の 2.6: an error satisfies no type but an error's, so asking
            // whether a value is one is a question about the value alone.
            case LHAT_BC_ISERROR:
                registers[a] =
                    lhat_bool(lhat_is_object_kind(registers[b],
                                                  LHAT_OBJECT_ERROR));
                break;

            case LHAT_BC_ISKIND: {
                if (!lhat_is_object_kind(registers[cc], LHAT_OBJECT_ERROR_KIND)) {
                    return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatErrorKind *kind =
                    (const LhatErrorKind *)lhat_as_object(registers[cc]);
                registers[a] = lhat_bool(lhat_error_is_kind(registers[b], kind));
                break;
            }

            case LHAT_BC_ISNIL:
                registers[a] = lhat_bool(lhat_is_nil(registers[b]));
                break;

            case LHAT_BC_CALL: {
                if (!lhat_is_object_kind(registers[a], LHAT_OBJECT_SUBROUTINE)) {
                    return finish(m, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                const LhatClosure *callee =
                    (const LhatClosure *)lhat_as_object(registers[a]);
                if (callee->proto == NULL) {
                    return finish(m, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                if (b != callee->proto->parameters) {
                    return finish(m, LHAT_RUN_ARITY, lhat_nil(), at);
                }
                if (m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 5.3: the arguments already sit just above the callee, so
                // the new frame starts there and needs no shuffling.
                LhatValue *next_base = &registers[a] + 1;
                if (next_base + LHAT_MAX_REGISTERS >=
                    m->stack + LHAT_STACK_SLOTS) {
                    return finish(m, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                frame->pc = pc;
                Frame *called = &m->frames[m->frame_count++];
                called->closure = callee;
                called->pc = 0;
                called->base = next_base;
                called->result = a;

                frame = called;
                registers = frame->base;
                chunk = &callee->proto->chunk;
                pc = 0;
                break;
            }

            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL: {
                LhatValue value = op == LHAT_BC_RETURN ? registers[a]
                                                       : lhat_nil();
                // 5.4: whatever still points into this frame takes its value
                // with it, since the slots are about to be reused.
                close_upvalues(m, frame->base);
                m->frame_count--;

                if (m->frame_count == 0) {
                    return finish(m, LHAT_RUN_OK, value, at);
                }

                uint8_t into = frame->result;
                frame = &m->frames[m->frame_count - 1];
                registers = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                registers[into] = value;
                break;
            }

            case LHAT_BC_COUNT:
                return finish(m, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
    }

    return finish(m, LHAT_RUN_OK, lhat_nil(), chunk->count);
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_DIVIDE_BY_ZERO:  return "// and % cannot divide by zero";
        case LHAT_RUN_NOT_CALLABLE:    return "this is not a subroutine";
        case LHAT_RUN_ARITY:           return "the wrong number of arguments";
        case LHAT_RUN_STACK_OVERFLOW:  return "the calls went too deep";
        case LHAT_RUN_OUT_OF_MEMORY:   return "out of memory";
        case LHAT_RUN_BAD_KEY:         return "this cannot be a key";
    }
    return "unknown";
}
