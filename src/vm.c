// L^ (lhat) -- compiling a tree to bytecode, and running it.

#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "lhatconfig.h"
#include "machine.h"
// 02 の 14.17改2: tonumber reads 01 の 10 章's grammar. number.h is that
// grammar and nothing else -- no source, no token, no diagnostic -- so the
// machine carries it without carrying a front end.
#include "number.h"
#include "port.h"
#include "type.h"

// 9.8: break^ is a normal end for the loop it leaves, so its jump lands where
// last^ and epilog^ are, not past them. The chain is what a break^ written
// inside a nested block still finds.
typedef struct LoopContext {
    struct LoopContext *enclosing;
    size_t jumps[LHAT_MAX_BREAKS];
    size_t count;
    size_t cleanup_depth;  // what a break^ has to drain back down to
} LoopContext;

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    size_t length;
    uint8_t reg;  // 5.2: a name is a slot in the frame like anything else
    // 01 の 8 章: which scope declared it, counted from this subroutine's
    // own body outwards -- what '$^' walks past. See Compiler.scope_depth.
    uint32_t depth;
    // 05 の 8.9: how many consecutive slots the name holds -- 1 for
    // everything but a host value, whose registered width the checker's
    // stamp (or the written annotation) decided at declaration. Reading or
    // writing the name moves this many slots.
    uint8_t width;
} Local;

typedef struct DefChain {
    const LhatNode *parts[LHAT_MAX_DEF_CHAIN];  // base first, derived last
    // 03 の 4.3: a node carries an offset into the text it was read from, so
    // reading one through another input's lexer answers a different word.
    // A chain may cross inputs -- 'A .. def^{ … }' where A came from an
    // earlier one -- so the lexer belongs to the part and not to the chain.
    const LhatLexer *lexers[LHAT_MAX_DEF_CHAIN];
    size_t count;
} DefChain;

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
        // 01 の 2.3改 (S35): how many inner bindings of the name this capture
        // was resolved past -- it^^ and it^ are two different targets under
        // one spelling, so the cache tells them apart by the count. SIZE_MAX
        // marks a capture_at entry, which no name search may ever answer
        // with: a '$…name' capture was aimed at a place, not at a name.
        size_t skip;
    } upvalue_names[LHAT_MAX_UPVALUES];

    // Slots below this hold live names; everything above is scratch for the
    // expression being compiled, released as soon as it is consumed.
    uint8_t next_register;

    // 04 の 11 章: the line of whatever node compile_statement/
    // compile_expression is currently under, so emit() has something to
    // give lhat_chunk_emit without every one of its callers passing it.
    uint32_t line;

    LoopContext *loop;  // the innermost loop being compiled, NULL outside one

    // 01 の 8 章: how many scopes are open here, counting this subroutine's
    // body as 0. A scope is what a '{' opens when names become visible in
    // it -- a block, a loop body, an if^ arm, a def^ body -- which is
    // exactly where the checker pushes a Scope, so '$^' counts the same on
    // both sides. A table literal, a self^{ }, an error^ construction and
    // an interpolation hole hold entries or an expression rather than
    // bindings, and open none; catch^'s it^ and with^'s names are written
    // without braces at all and so add none either.
    uint32_t scope_depth;

    // 03 の 4.3: the statements being declared are the top level of a
    // session's input, where a name written again keeps the slot it had.
    bool session_top;

    // 03 の 4.3: how many of `locals` were seeded from the session rather
    // than declared by this input. A let^ over one of those is a name from
    // an earlier input written again, which reuses the slot (declare_names)
    // -- so 5.4's sharing of it has to be severed there, or a closure an
    // earlier input made would see the new binding. Zero outside a session.
    size_t session_locals;

    // 5.5: how many cleanups are pending here. The compiler tracks it so that
    // an exit knows how far to drain; the machine holds the cleanups.
    size_t cleanup_depth;
    bool in_cleanup;  // 02 の 10.5: no return^ inside a finally^

    // 04 の 2.4: a kind is the place it was declared, so the compiler keeps
    // one object per kind and hands the same one to every use. They live on
    // the outermost proto: a nested body making its own copy would give the
    // same declaration two identities.
    struct ErrorDecl *errors;
    size_t error_count;
    size_t error_capacity;

    // The def^ bound to each name, so that '..' and a self^{ … } inside new
    // can be resolved without running anything (14.2).
    struct DefDecl *defs;
    size_t def_count;
    size_t def_capacity;

    // The definition being compiled, which is what a self^{ … } inside its
    // new builds and what class^ names.
    const DefChain *building;

    // 14.9: the definitions lower_def_chain is inside right now, kept on the
    // root the way `defs` is. A def^ reached again while its own shape is
    // being built is answered by its name alone rather than descended into.
    // One chain per level of nesting, so the bound is the two multiplied.
    const LhatNode *lowering[LHAT_MAX_TYPE_NESTING * LHAT_MAX_DEF_CHAIN];
    size_t lowering_count;

    // 05 の 5 章: where a require^ inside this unit leads. NULL when the unit
    // is being compiled on its own, and then a require^ has nowhere to go.
    const LhatUnits *units;
} Compiler;

typedef struct DefDecl {
    const char *name;
    size_t length;
    DefChain chain;
} DefDecl;

typedef struct ErrorDecl {
    const char *name;
    size_t length;
    const LhatNode *node;         // the errordef^, for the field defaults
    // 03 の 4.3: read the node through the lexer it came from. An earlier
    // input of a session is still where its offsets mean something.
    const LhatLexer *lexer;
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

// 01 の 2.3改 (S34): the hat is part of the name. A hat identifier's name is
// the word plus one hat -- 'self^' is a different name from 'self' -- and a
// spelling with more hats than one is that same name reached further out:
// this^^^ is the this^ two levels up, its name still 'this^' with the
// count kept on the node (v.name.hats) for the constructs that stack. So
// what this answers is the canonical spelling, word + one hat at most.
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
        // The hats sit right after the word in the source, so the canonical
        // name is the span cut after the first of them.
        size_t word = node->v.name.length >= node->v.name.hats
                          ? node->v.name.length - node->v.name.hats
                          : node->v.name.length;
        *length = node->v.name.hats > 0 ? word + 1 : word;
        return true;
    }
    if (node->kind == LHAT_NODE_FOCUS) {
        // 16.2: the focus with no name written is called it^, and the source
        // need not contain the word for that to be its name.
        *text = "it^";
        *length = 3;
        return true;
    }
    // 01 の 8 章: the sigil says where to look, and the name is what to look
    // for -- so a specifier answers with the name it is glued to.
    if (node->kind == LHAT_NODE_SCOPE) {
        return node_name(c, node->v.scope.name, text, length);
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

// 05 の 8.9: what the checker stamped on an expression that holds a host
// value (check.c's infer), or NULL for every other expression. The compiler
// is otherwise type-blind; this is the one channel a width arrives through,
// so compiling without checking simply never sees one -- and the checker's
// escape rules have already refused every place a width could go wrong.
static const struct LhatHostValueTag *hostvalue_of(const LhatNode *node)
{
    const LhatType *checked =
        node != NULL ? (const LhatType *)node->checked_type : NULL;
    return checked != NULL && checked->kind == LHAT_TYPE_HOSTVALUE
               ? checked->v.table.hostvalue_tag
               : NULL;
}

static size_t width_of(const LhatNode *node)
{
    const struct LhatHostValueTag *tag = hostvalue_of(node);
    return tag != NULL ? tag->width : 1;
}

// A run of consecutive slots, first one answered. The scratch discipline
// (mark/restore of next_register) frees a wide reservation the same way it
// frees a narrow one.
static uint8_t reserve_wide(Compiler *c, size_t width)
{
    uint8_t first = reserve(c);
    for (size_t i = 1; i < width; i++) {
        reserve(c);
    }
    return first;
}

// The slot(s) an expression's value will need: reserve_wide sized by the
// checker's stamp.
static uint8_t reserve_for(Compiler *c, const LhatNode *node)
{
    return reserve_wide(c, width_of(node));
}

static void emit(Compiler *c, LhatInstruction instruction);

// 05 の 8.9: MOVE copies one slot blindly (payload and tag alike), so a wide
// value moves as that many MOVEs. Ranges from the register allocator never
// interleave, so an ascending copy is safe wherever this is emitted.
static void emit_move_wide(Compiler *c, uint8_t into, uint8_t from,
                           size_t width)
{
    if (into == from) {
        return;
    }
    for (size_t i = 0; i < width; i++) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, (uint8_t)(into + i),
                                (uint8_t)(from + i), 0));
    }
}

static void emit(Compiler *c, LhatInstruction instruction)
{
    if (lhat_chunk_emit(&c->proto->chunk, instruction, c->line) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
}

static size_t emit_jump(Compiler *c, LhatOpcode op, uint8_t a)
{
    size_t at = lhat_chunk_emit(&c->proto->chunk, lhat_encode_jump(op, a, 0),
                                c->line);
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
// The same backwards search find_local does, passing over the innermost
// `*skip` bindings of the name -- 01 の 2.3改's stacked reach, where it^^
// means the it^ one binding out. What was not consumed stays in `*skip`, so
// the search may continue into an enclosing body.
static const Local *find_local_skipping(const Compiler *c, const char *name,
                                        size_t length, size_t *skip)
{
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->length == length && memcmp(local->name, name, length) == 0) {
            if (*skip == 0) {
                return local;
            }
            (*skip)--;
        }
    }
    return NULL;
}

static size_t find_upvalue_skipping(Compiler *c, const char *name,
                                    size_t length, size_t skip)
{
    if (c->parent == NULL) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_names[i].length == length &&
            c->upvalue_names[i].skip == skip &&
            memcmp(c->upvalue_names[i].name, name, length) == 0) {
            return i;
        }
    }

    LhatUpvalueSource source = LHAT_UPVALUE_OUTER;
    uint8_t index = 0;
    size_t remaining = skip;

    const Local *local = find_local_skipping(c->parent, name, length,
                                             &remaining);
    if (local != NULL) {
        // 05 の 8.9: an upvalue is one slot and a capture outlives the
        // frame, so a host value is never captured. The checker refused
        // this first (LHAT_CHECK_ERR_HOSTVALUE_ESCAPES); this is the
        // backstop for a compile without checking.
        if (local->width > 1) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return SIZE_MAX;
        }
        source = LHAT_UPVALUE_REGISTER;
        index = local->reg;
    } else {
        size_t outer = find_upvalue_skipping(c->parent, name, length,
                                             remaining);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        index = (uint8_t)outer;
    }

    size_t added = lhat_proto_add_upvalue(c->proto, source, index);
    if (added == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return SIZE_MAX;
    }
    c->upvalue_names[added].name = name;
    c->upvalue_names[added].length = length;
    c->upvalue_names[added].skip = skip;
    return added;
}

static size_t find_upvalue(Compiler *c, const char *name, size_t length)
{
    return find_upvalue_skipping(c, name, length, 0);
}

// 15.10改 (S35): this^^ is the subroutine enclosing the one running --
// `levels` bodies out. No register ever holds an enclosing body's closure
// (BC_THIS reads the running frame), so the capture is the third upvalue
// source: at CLOSURE time the maker boxes its own closure, and the chain
// through any intermediate bodies is the ordinary one. Cached under the
// this^ spelling with the level for a count, beside the other captures.
static size_t resolve_this(Compiler *c, size_t levels)
{
    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_names[i].length == 5 &&
            c->upvalue_names[i].skip == levels &&
            memcmp(c->upvalue_names[i].name, "this^", 5) == 0) {
            return i;
        }
    }

    size_t added;
    if (levels == 1) {
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_THIS, 0);
    } else {
        size_t outer = resolve_this(c->parent, levels - 1);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_OUTER,
                                       (uint8_t)outer);
    }
    if (added == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return SIZE_MAX;
    }
    c->upvalue_names[added].name = "this^";
    c->upvalue_names[added].length = 5;
    c->upvalue_names[added].skip = levels;
    return added;
}

// ---------------------------------------------------------------------------
// Scope specifiers (01 の 8 章)
// ---------------------------------------------------------------------------

static Compiler *root_of(Compiler *c);

// The same backwards search find_local does, but blind to anything declared
// deeper than `max_depth` -- which is how '$^' looks past the scope it was
// written in without looking past the name it wants.
static const Local *find_local_at(const Compiler *c, const char *name,
                                  size_t length, uint32_t max_depth)
{
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->depth <= max_depth && local->length == length &&
            memcmp(local->name, name, length) == 0) {
            return local;
        }
    }
    return NULL;
}

// Where a specifier says to start looking, counted either way. 01 の 8 章:
// '$^' steps out of the scope the name was written in and '$^^' out of that
// one too; '$' names the unit, '$$' the one scope inside it and '$$$' the
// one inside that. Crossing into an enclosing subroutine spends every scope
// still open in this one plus its body's own, since 5.4 makes that body one
// scope the way any block is -- which is what lets an absolute index run
// through a chain of them as one numbering.
//
// Answers false when there are not that many scopes.
static bool scope_start(Compiler *c, const LhatNode *node, Compiler **owner,
                        uint32_t *max_depth)
{
    if (node->v.scope.kind == LHAT_SCOPE_FILE) {
        // The absolute index counts inwards, so it is turned into a place in
        // the chain by asking how many scopes stand outside each body.
        uint32_t total = 0;
        for (Compiler *at = c; at != NULL; at = at->parent) {
            total += at->scope_depth + 1;
        }
        if (node->v.scope.depth >= total) {
            return false;  // naming a scope further in than the one here
        }
        uint32_t inside = 0;
        for (Compiler *at = c; at != NULL; at = at->parent) {
            inside += at->scope_depth + 1;
            uint32_t outside = total - inside;
            if (node->v.scope.depth >= outside) {
                *owner = at;
                *max_depth = node->v.scope.depth - outside;
                return true;
            }
        }
        return false;
    }
    uint32_t out = node->v.scope.depth;
    Compiler *at = c;
    while (out > at->scope_depth) {
        out -= at->scope_depth + 1;
        at = at->parent;
        if (at == NULL) {
            return false;
        }
    }
    *owner = at;
    *max_depth = at->scope_depth - out;
    return true;
}

// 5.4's capture, aimed at one place rather than at the nearest name. The
// ordinary find_upvalue searches by name from the innermost enclosing body,
// which would stop at a shadow the specifier was written to look past.
static size_t capture_at(Compiler *c, const char *name, size_t length,
                         const Compiler *owner, uint32_t max_depth)
{
    if (c->parent == NULL) {
        return SIZE_MAX;
    }

    bool from_register = false;
    uint8_t index = 0;
    if (c->parent == owner) {
        const Local *local = find_local_at(c->parent, name, length, max_depth);
        if (local == NULL) {
            return SIZE_MAX;
        }
        from_register = true;
        index = local->reg;
    } else {
        size_t outer = capture_at(c->parent, name, length, owner, max_depth);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        index = (uint8_t)outer;
    }

    // Not shared with the upvalues find_upvalue names: two of them over one
    // slot read and write the same place, and giving this one its own entry
    // keeps a name from being taken for a target it was not resolved to --
    // the SIZE_MAX skip is what keeps every name search from answering with
    // this entry.
    size_t added = lhat_proto_add_upvalue(
        c->proto, from_register ? LHAT_UPVALUE_REGISTER : LHAT_UPVALUE_OUTER,
        index);
    if (added == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return SIZE_MAX;
    }
    c->upvalue_names[added].name = name;
    c->upvalue_names[added].length = length;
    c->upvalue_names[added].skip = SIZE_MAX;
    return added;
}

// What a '$…name' resolves to. `reg` is filled when the name is a register of
// this body, `upvalue` when it is a place an enclosing one holds.
typedef enum {
    SCOPED_NONE,      // no such name from there outwards
    SCOPED_TOO_FAR,   // fewer scopes are open than the specifier counts
    SCOPED_REGISTER,
    SCOPED_UPVALUE
} ScopedKind;

static ScopedKind resolve_scoped(Compiler *c, const LhatNode *node,
                                 const char *name, size_t length,
                                 uint8_t *reg, size_t *upvalue)
{
    Compiler *owner = NULL;
    uint32_t max_depth = 0;
    if (!scope_start(c, node, &owner, &max_depth)) {
        return SCOPED_TOO_FAR;
    }

    // 01 の 8 章: a specifier says where to begin, not where to stop, so an
    // ancestor further out still answers when the scope named holds nothing.
    for (Compiler *at = owner; at != NULL; at = at->parent) {
        const Local *local = find_local_at(at, name, length, max_depth);
        if (local != NULL) {
            if (at == c) {
                *reg = local->reg;
                return SCOPED_REGISTER;
            }
            size_t index = capture_at(c, name, length, at, max_depth);
            if (index == SIZE_MAX) {
                return SCOPED_NONE;
            }
            *upvalue = index;
            return SCOPED_UPVALUE;
        }
        // Every scope of an enclosing body is open to the search.
        if (at->parent != NULL) {
            max_depth = at->parent->scope_depth;
        }
    }
    return SCOPED_NONE;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into);
static void compile_statement(Compiler *c, const LhatNode *node);
static void compile_statements(Compiler *c, const LhatNode *statements);
static void compile_block(Compiler *c, const LhatNode *block);
static void compile_block_in_scope(Compiler *c, const LhatNode *block);
static const LhatNode *define_target_name(const LhatNode *target);
static void compile_for_once(Compiler *c, const LhatNode *node, uint8_t into,
                             bool as_expression);
static bool def_chain_of(Compiler *c, const LhatNode *node, DefChain *out);
static void compile_def(Compiler *c, const LhatNode *node, uint8_t into);
static const char *required_module_name(Compiler *c, const LhatNode *node);
static void compile_bind_path(Compiler *c, const char *path, uint8_t value);
static LhatRuntimeType *lower_type(Compiler *c, const LhatNode *node);
static bool resolve_name(Compiler *c, const char *name, size_t length,
                         uint8_t into);
static const LhatNode *template_of(const LhatNode *def);

static void compile_import_path(Compiler *c, const LhatNode *path, uint8_t into,
                                uint8_t key);

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
            (ErrorDecl *)lhat_realloc(root->errors, grown * sizeof *bigger);
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

    LhatString *group_name = lhat_string_new(&chunk->heap, name, length);
    const LhatErrorKind **kinds =
        (const LhatErrorKind **)lhat_calloc(kind_count ? kind_count : 1,
                                       sizeof *kinds);
    LhatErrorKind *group =
        lhat_error_kind_new(&chunk->heap, NULL, group_name);
    if (group_name == NULL || group == NULL || kinds == NULL) {
        lhat_free(kinds);
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    size_t index = 0;
    for (const LhatNode *k = node->v.named.members; k != NULL;
         k = k->next, index++) {
        const char *kind_name = NULL;
        size_t kind_length = 0;
        if (!node_name(c, k->v.named.name, &kind_name, &kind_length)) {
            lhat_free(kinds);
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // "IOError.NotFound" -- what typeof^ answers (2.3).
        char qualified[LHAT_QUALIFIED_NAME_BUFFER];
        size_t total = length + 1 + kind_length;
        if (total >= sizeof qualified) {
            lhat_free(kinds);
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        memcpy(qualified, name, length);
        qualified[length] = '.';
        memcpy(qualified + length + 1, kind_name, kind_length);

        LhatString *text = lhat_string_new(&chunk->heap, qualified, total);
        LhatErrorKind *kind =
            text != NULL ? lhat_error_kind_new(&chunk->heap, group, text)
                         : NULL;
        if (kind == NULL) {
            lhat_free(kinds);
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        kinds[index] = kind;
    }

    ErrorDecl *decl = &root->errors[root->error_count++];
    decl->name = name;
    decl->length = length;
    decl->node = node;
    decl->lexer = c->lexer;
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

typedef struct {
    const char *text;
    size_t length;
} QualifierSegment;

// Flattens a chain of MEMBER nodes into [root, ..., leaf] -- "io.IOError.
// NotFound" becomes [io, IOError, NotFound]. 0 when a segment along the way
// is not a plain name, or there are more than fit in `capacity`.
static size_t flatten_qualified_path(Compiler *c, const LhatNode *node,
                                     QualifierSegment *segments,
                                     size_t capacity)
{
    if (node->kind == LHAT_NODE_MEMBER) {
        size_t count = flatten_qualified_path(c, node->v.access.target,
                                              segments, capacity);
        if (count == 0 || count >= capacity) {
            return 0;
        }
        const char *text = NULL;
        size_t length = 0;
        if (!node_name(c, node->v.access.argument, &text, &length)) {
            return 0;
        }
        segments[count].text = text;
        segments[count].length = length;
        return count + 1;
    }
    if (capacity == 0) {
        return 0;
    }
    const char *text = NULL;
    size_t length = 0;
    if (!node_name(c, node, &text, &length)) {
        return 0;
    }
    segments[0].text = text;
    segments[0].length = length;
    return 1;
}

// segments[from..to), joined by ".", spells exactly `entry_text`.
static bool segments_match(const QualifierSegment *segments, size_t from,
                           size_t to, const char *entry_text)
{
    for (size_t i = from; i < to; i++) {
        size_t seg_len = segments[i].length;
        if (strncmp(entry_text, segments[i].text, seg_len) != 0) {
            return false;
        }
        entry_text += seg_len;
        if (i + 1 < to) {
            if (*entry_text != '.') {
                return false;
            }
            entry_text++;
        }
    }
    return *entry_text == '\0';
}

// 05 の 8.7 の誤り版: what lhat_register_error_kind (program.h) put in
// LhatUnits.host_errors. Tried only once the unit's own errordef^s have had
// their chance (resolve_kind below) -- import^'s own rule, so a local name
// always wins and the answer for a host name does not depend on what else
// this unit happens to declare.
static const LhatErrorKind *resolve_host_kind(Compiler *c, const LhatNode *path,
                                              bool *from_host)
{
    const LhatUnits *units = root_of(c)->units;
    if (units == NULL || units->host_errors == NULL) {
        return NULL;
    }

    QualifierSegment segments[LHAT_MAX_QUALIFIER_SEGMENTS];
    size_t count = flatten_qualified_path(c, path, segments,
                                          LHAT_MAX_QUALIFIER_SEGMENTS);
    if (count < 2) {
        return NULL;  // a module name alone never reaches a kind
    }

    for (size_t i = 0; i < units->host_error_count; i++) {
        const LhatHostErrorKind *entry = &units->host_errors[i];

        // "module...Name.Variant" -- the last segment names one kind.
        if (count >= 3 &&
            segments_match(segments, 0, count - 2, entry->module) &&
            segments_match(segments, count - 2, count - 1, entry->name)) {
            const char *wanted = segments[count - 1].text;
            size_t wanted_length = segments[count - 1].length;
            for (size_t v = 0; v < entry->variant_count; v++) {
                size_t vlen = strlen(entry->variant_names[v]);
                if (vlen == wanted_length &&
                    memcmp(entry->variant_names[v], wanted, vlen) == 0) {
                    if (from_host != NULL) {
                        *from_host = true;
                    }
                    return entry->variants[v];
                }
            }
        }

        // "module...Name" -- the declaration as a whole (04 の 2.3: what
        // isa^ against the union, rather than one kind, asks for).
        if (segments_match(segments, 0, count - 1, entry->module) &&
            segments_match(segments, count - 1, count, entry->name)) {
            return entry->group;
        }
    }
    return NULL;
}

// Resolves the 'IOError' or 'IOError.NotFound' a use writes. `kind_node`
// comes back as the ERROR_KIND declaring the fields, or NULL either when the
// whole declaration was named or when the answer came from a host
// registration (05 の 8.7 の誤り版) rather than an errordef^ in this unit --
// a host kind has no AST of fields to point at. `from_host`, when not NULL,
// is set to say which of those two NULL cases it was; compile_error_new is
// the only caller that needs to tell them apart.
static const LhatErrorKind *resolve_kind(Compiler *c, const LhatNode *path,
                                         const LhatNode **kind_node,
                                         bool *from_host)
{
    *kind_node = NULL;
    if (from_host != NULL) {
        *from_host = false;
    }
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
    const ErrorDecl *decl = node_name(c, group_node, &name, &length)
                                ? find_error_decl(c, name, length)
                                : NULL;
    if (decl != NULL) {
        if (member == NULL) {
            return decl->group;
        }

        const char *wanted = NULL;
        size_t wanted_length = 0;
        if (node_name(c, member, &wanted, &wanted_length)) {
            // The declaration may be an earlier input's, and its offsets
            // index that input's text rather than this one's.
            Compiler reading = *c;
            reading.lexer = decl->lexer;

            size_t index = 0;
            for (const LhatNode *k = decl->node->v.named.members; k != NULL;
                 k = k->next, index++) {
                const char *kind_name = NULL;
                size_t kind_length = 0;
                if (node_name(&reading, k->v.named.name, &kind_name,
                              &kind_length) &&
                    kind_length == wanted_length &&
                    memcmp(kind_name, wanted, wanted_length) == 0) {
                    *kind_node = k;
                    return decl->kinds[index];
                }
            }
        }
        return NULL;
    }

    return resolve_host_kind(c, path, from_host);
}

// 05 の 8.8 の isa^ 版: what lhat_register_hostdata_type (program.h) put
// in LhatUnits.host_types. "module...Name" only -- a hostdata type has no
// variant to walk into the way an errordef^'s declaration does; 8.8's
// identity is the tag alone.
static const LhatHostDataTag *resolve_host_type_tag(Compiler *c,
                                                     const LhatNode *path)
{
    const LhatUnits *units = root_of(c)->units;
    if (units == NULL || units->host_types == NULL) {
        return NULL;
    }

    QualifierSegment segments[LHAT_MAX_QUALIFIER_SEGMENTS];
    size_t count = flatten_qualified_path(c, path, segments,
                                          LHAT_MAX_QUALIFIER_SEGMENTS);
    if (count < 2) {
        return NULL;  // a module name alone never reaches a type
    }

    for (size_t i = 0; i < units->host_type_count; i++) {
        const LhatHostTypeEntry *entry = &units->host_types[i];
        if (segments_match(segments, 0, count - 1, entry->module) &&
            segments_match(segments, count - 1, count, entry->name)) {
            return entry->tag;
        }
    }
    return NULL;
}

// 05 の 8.9: the same for a host value type, out of
// LhatUnits.hostvalue_types. This is how a written annotation carries a
// width into the compiler when the checker's stamp is not there to.
static const LhatHostValueTag *resolve_hostvalue_type_tag(Compiler *c,
                                                          const LhatNode *path)
{
    const LhatUnits *units = root_of(c)->units;
    if (units == NULL || units->hostvalue_types == NULL || path == NULL) {
        return NULL;
    }

    QualifierSegment segments[LHAT_MAX_QUALIFIER_SEGMENTS];
    size_t count = flatten_qualified_path(c, path, segments,
                                          LHAT_MAX_QUALIFIER_SEGMENTS);
    if (count < 2) {
        return NULL;
    }

    for (size_t i = 0; i < units->hostvalue_type_count; i++) {
        const LhatHostValueTypeEntry *entry = &units->hostvalue_types[i];
        if (segments_match(segments, 0, count - 1, entry->module) &&
            segments_match(segments, count - 1, count, entry->name)) {
            return entry->tag;
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
    bool from_host = false;
    const LhatErrorKind *kind =
        resolve_kind(c, node->v.named.name, &kind_node, &from_host);
    if (kind == NULL || (kind_node == NULL && !from_host)) {
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
    // kind_node is NULL for a host-registered kind (from_host above) --
    // v1 registers none of those with fields, so there is nothing to add.
    for (const LhatNode *field =
             kind_node != NULL ? kind_node->v.named.members : NULL;
         field != NULL; field = field->next) {
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
        local->name = "it^";
        local->length = 3;
        local->reg = caught;
        local->depth = c->scope_depth;  // catch^ opens no brace of its own
        local->width = 1;
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

// 02 の 13.11: isa^ asks whether the left side may stand where the right side
// is written. Every spelling of the right side is that one question at run
// time, so there is one instruction for it: lower_type turns the written type
// into the descriptor LHAT_BC_ISA tests a value against, which is the same
// object 11.6's as^ hands LHAT_BC_ASCAST. An error kind (04 の 6.1) and a
// host type (05 の 8.8) are both ordinary results of that lowering, which is
// why neither needs a case here any more.
//
// 5.13改: the right side is a type the COMPILER settles, always. A value that
// only arrives while the program runs -- a definition passed as a parameter,
// another unit's member -- carries no type to ask about: it is a plain table
// (13.7's t^{}), however it was made, and a program that receives one probes
// it the dynamic way, member by member, nil^ by nil^. There used to be a
// fallback here that loaded such a name as a value and had the machine read a
// shape off the table at run time; it was withdrawn -- constructing type
// information out of runtime data is the wrong direction (the table may be
// pure data, arbitrarily large), and the checker never accepted the form
// anyway.
//
// So NULL from lower_type is answered by what was written: any^ makes the
// question empty (13.7; check.c reports the writing itself), a bare name
// reaches no type (UNDEFINED), and anything else is a written form nothing
// settles (UNSUPPORTED).
// The test itself, against a left operand already in a register. 11.5 の (5)
// shares an operand between two links of a chain and evaluates it once, so
// there the left is compiled by the caller.
static void compile_isa_test(Compiler *c, const LhatNode *asked, uint8_t value,
                             uint8_t into)
{
    const char *name = NULL;
    size_t length = 0;
    if (node_name(c, asked, &name, &length) && name_is(name, length, "any^")) {
        emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, into, 1, 0));
        return;
    }

    LhatRuntimeType *wanted = lower_type(c, asked);
    if (wanted == NULL) {
        fail(c, asked->kind == LHAT_NODE_TYPE_NAME ||
                     asked->kind == LHAT_NODE_MEMBER
                 ? LHAT_COMPILE_UNDEFINED
                 : LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t holder = reserve(c);
    load_constant(c, holder, lhat_object((LhatObject *)wanted));
    emit(c, lhat_encode_abc(LHAT_BC_ISA, into, value, holder));
    c->next_register = mark;
}

static void compile_isa(Compiler *c, const LhatNode *node, uint8_t into)
{
    // The left side runs whatever the answer turns out to be -- for an any^
    // it is still the reason compile_expression keeps typeof^'s operand.
    uint8_t mark = c->next_register;
    // 05 の 8.9: a host value operand keeps its width here as anywhere.
    uint8_t value = reserve_for(c, node->v.binary.left);
    compile_expression(c, node->v.binary.left, value);
    compile_isa_test(c, node->v.binary.right, value, into);
    c->next_register = mark;
}

// One entry of a def^ -- a keyed member or a field of its template -- as a
// member of the shape the definition promises.
//
// A type is read only where one was written: 14.15's 'abstract^ name : type'
// puts the type where a value would otherwise be, and everything else carries
// an initialiser whose type is the checker's business (03 の 4.2 keeps this
// path independent of whether checking ran). A member with no type asks only
// that the name is there, which is what lhat_value_satisfies does with one.
static bool add_shape_member(Compiler *c, LhatRuntimeType *into,
                             const LhatNode *entry)
{
    const char *name = NULL;
    size_t length = 0;
    // 14.6改: a computed key names no member, so it asks for nothing.
    if (entry->v.entry.computed || entry->v.entry.key == NULL ||
        !node_name(c, entry->v.entry.key, &name, &length)) {
        return true;
    }

    LhatRuntimeType *member =
        entry->v.entry.declared ? lower_type(c, entry->v.entry.value) : NULL;

    // 14.5: the parts are walked in order and the later one is what answers,
    // so a name written again replaces what an earlier part put there.
    for (size_t i = 0; i < into->member_count; i++) {
        const LhatString *written = into->members[i].name;
        if (written->length == length &&
            memcmp(written->text, name, length) == 0) {
            into->members[i].type = member;
            return true;
        }
    }

    LhatHeap *owner = &root_of(c)->proto->chunk.heap;
    LhatString *text = lhat_string_new(owner, name, length);
    return text != NULL && lhat_type_rt_add_member(into, text, member);
}

// 14.9: a definition is a shape and its name is a label, so a name standing
// for one asks exactly what 14.10's t^{ ... } asks -- with the members read
// off the def^ rather than written out. 14.2 settles the chain without running
// anything (def_chain_of), which is what puts both halves of the promise in
// reach here: the template's fields (14.11) are what an instance carries, and
// the keyed entries are what it reaches through the definition (14.3).
static LhatRuntimeType *lower_def_chain(Compiler *c, const DefChain *chain)
{
    LhatRuntimeType *type =
        lhat_type_rt_new(&root_of(c)->proto->chunk.heap, LHAT_TYPE_RT_STRUCTURE);
    if (type == NULL) {
        return NULL;
    }

    const LhatLexer *enclosing = c->lexer;
    for (size_t i = 0; i < chain->count; i++) {
        // 03 の 4.3: a part read from an earlier input carries offsets into
        // that input's text, so the lexer travels with it -- the same swap
        // compile_def makes to read the same names.
        c->lexer = chain->lexers[i];
        for (const LhatNode *entry = chain->parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (!add_shape_member(c, type, entry)) {
                c->lexer = enclosing;
                return NULL;
            }
        }
        const LhatNode *fields = template_of(chain->parts[i]);
        for (const LhatNode *field = fields != NULL ? fields->v.list.items : NULL;
             field != NULL; field = field->next) {
            if (!add_shape_member(c, type, field)) {
                c->lexer = enclosing;
                return NULL;
            }
        }
    }
    c->lexer = enclosing;

    // The order rt_from_checked leaves its own structures in, so 11.3's
    // structural comparison lines two of them up without a search.
    lhat_type_rt_sort_members(type);
    return type;
}

// 02 の 14.12: what a parameter was written to take, in the form the machine
// can ask a value about. 03 の 2.1's tags are what make the question
// answerable at all; 13.11's is^ and 3.3's relaxed checks want the same
// descriptor.
//
// Anything not covered answers `any^`, which asks nothing -- a conservative
// direction, since the checker has already refused what is statically wrong.
static LhatRuntimeType *lower_type(Compiler *c, const LhatNode *node)
{
    Compiler *root = root_of(c);
    LhatHeap *owner = &root->proto->chunk.heap;

    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
        // 13.5 and 14.5: the parser leaves both as a left-leaning tree of two
        // sides (parse_type, parse_type_intersection), so the arms are
        // gathered by walking it rather than by reading a list. An arm that
        // lowers to nothing takes the whole type with it: a union is only as
        // exact as its widest arm, and one that asks nothing would make the
        // union ask nothing while still looking like a real question.
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT: {
            LhatRuntimeType *type = lhat_type_rt_new(
                owner, node->kind == LHAT_NODE_TYPE_UNION
                           ? LHAT_TYPE_RT_UNION
                           : LHAT_TYPE_RT_INTERSECT);
            if (type == NULL) {
                return NULL;
            }
            const LhatNode *sides[2] = {node->v.binary.left,
                                        node->v.binary.right};
            for (size_t i = 0; i < 2; i++) {
                LhatRuntimeType *arm = lower_type(c, sides[i]);
                if (arm == NULL || !lhat_type_rt_add_part(type, arm)) {
                    return NULL;
                }
            }
            return type;
        }

        case LHAT_NODE_TYPE_TABLE: {
            // 14.10: the structure asks for at least these members.
            LhatRuntimeType *type =
                lhat_type_rt_new(owner, LHAT_TYPE_RT_STRUCTURE);
            if (type == NULL) {
                return NULL;
            }
            for (const LhatNode *member = node->v.list.items; member != NULL;
                 member = member->next) {
                const char *name = NULL;
                size_t length = 0;
                if (!node_name(c, member->v.entry.key, &name, &length)) {
                    continue;
                }
                LhatString *text = lhat_string_new(owner, name, length);
                if (text == NULL ||
                    !lhat_type_rt_add_member(type, text,
                                             lower_type(c, member->v.entry.value))) {
                    return NULL;
                }
            }
            return type;
        }

        case LHAT_NODE_MEMBER:
        case LHAT_NODE_TYPE_NAME: {
            // 04 の 2.4: a kind is the object its declaration made, so a
            // qualified name resolves to that rather than to any structure.
            const LhatNode *unused = NULL;
            const LhatErrorKind *kind = resolve_kind(c, node, &unused, NULL);
            if (kind != NULL) {
                LhatRuntimeType *type =
                    lhat_type_rt_new(owner, LHAT_TYPE_RT_ERROR_KIND);
                if (type != NULL) {
                    type->error_kind = kind;
                }
                return type;
            }

            // 05 の 8.8: a host-registered type, which resolve_kind never
            // answers -- it only reaches an errordef^-shaped kind. Tried
            // after it for the same reason compile_isa used to try it last:
            // a local declaration is what a name means first.
            const LhatHostDataTag *tag = resolve_host_type_tag(c, node);
            if (tag != NULL) {
                LhatRuntimeType *type =
                    lhat_type_rt_new(owner, LHAT_TYPE_RT_HOSTDATA);
                if (type != NULL) {
                    type->hostdata_tag = tag;
                }
                return type;
            }
            // 05 の 8.9: a host value type, written the same qualified way.
            const LhatHostValueTag *value_tag =
                resolve_hostvalue_type_tag(c, node);
            if (value_tag != NULL) {
                LhatRuntimeType *type =
                    lhat_type_rt_new(owner, LHAT_TYPE_RT_HOSTVALUE);
                if (type != NULL) {
                    type->hostvalue_tag = value_tag;
                }
                return type;
            }

            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                return NULL;
            }
            // 13.13: the back edge of a written structure. Asking nothing
            // there ends the walk, which is what the def^ chain below already
            // does when a shape is written from inside itself -- a descriptor
            // is a finite question about one value, so the recursion is where
            // it stops rather than something to unfold.
            if (name_is(name, length, "Self^")) {
                return NULL;
            }
            LhatRuntimeTypeKind simple = LHAT_TYPE_RT_ANY;
            // 14.8: one type, and int^/float^ are the two representations of
            // it -- the three spellings check.c's resolve_type reads together.
            if (name_is(name, length, "number^") || name_is(name, length, "int^") ||
                name_is(name, length, "float^")) {
                simple = LHAT_TYPE_RT_NUMBER;
            } else if (name_is(name, length, "string^")) {
                simple = LHAT_TYPE_RT_STRING;
            } else if (name_is(name, length, "bool^")) {
                simple = LHAT_TYPE_RT_BOOL;
            } else if (name_is(name, length, "nil^")) {
                simple = LHAT_TYPE_RT_NIL;
            } else if (name_is(name, length, "table^") ||
                       name_is(name, length, "t^")) {
                // 14.10: bare t^ asks for nothing in particular -- the top of
                // tables, which is the other spelling of this one name (the
                // pair check.c's resolve_type reads together).
                simple = LHAT_TYPE_RT_TABLE;
            } else if (name_is(name, length, "error^")) {
                simple = LHAT_TYPE_RT_ERROR;
            } else if (name_is(name, length, "any^")) {
                return NULL;  // asks nothing
            } else {
                // 14.9: a definition's name stands for its shape, which 14.2
                // lets the compiler assemble without running anything.
                DefChain chain;
                chain.count = 0;
                if (!def_chain_of(c, node, &chain)) {
                    return NULL;  // nothing this compile can see
                }

                // A definition whose shape is already being built is one of
                // 14.15's mutually annotated pairs, or a field annotated with
                // its own definition. Answering by name alone ends the walk
                // where descending would not.
                size_t room = sizeof root->lowering / sizeof root->lowering[0];
                for (size_t i = 0; i < chain.count; i++) {
                    for (size_t j = 0; j < root->lowering_count; j++) {
                        if (root->lowering[j] == chain.parts[i]) {
                            return NULL;
                        }
                    }
                }
                if (root->lowering_count + chain.count > room) {
                    return NULL;  // nested deeper than a written shape needs
                }

                size_t mark = root->lowering_count;
                for (size_t i = 0; i < chain.count; i++) {
                    root->lowering[root->lowering_count++] = chain.parts[i];
                }
                LhatRuntimeType *shape = lower_def_chain(c, &chain);
                root->lowering_count = mark;
                return shape;
            }
            return lhat_type_rt_new(owner, simple);
        }

        case LHAT_NODE_TYPE_FUNC:
            return lhat_type_rt_new(owner, LHAT_TYPE_RT_SUBROUTINE);
        // 13.9 with 15.3改: a written 'c^{ f^R -> Y;, T }' names its three
        // slots and the kind of the body -- read each rather than leaving the
        // bare tag S28 used to stop at.
        case LHAT_NODE_TYPE_CORO: {
            LhatRuntimeType *type =
                lhat_type_rt_new(owner, LHAT_TYPE_RT_COROUTINE);
            if (type == NULL) {
                return NULL;
            }
            type->receive = lower_type(c, node->v.coroutine.receive);
            type->produce = lower_type(c, node->v.coroutine.produce);
            type->result = lower_type(c, node->v.coroutine.result);
            type->is_function = node->v.coroutine.is_function;  // 15.3改
            return type;
        }

        default:
            return NULL;
    }
}

// Converts one of the checker's own LhatType objects into the shape
// lower_type builds from a written annotation. Used only where nothing was
// written at all -- lower_type already has nothing to read there, so this is
// a fallback onto what infer_func (check.c) settled instead, not a second
// opinion on what was written.
//
// A checker type may hold itself (an instance whose member is its own
// definition), so the tables on the way in are remembered on the C stack --
// meeting one again answers 13.7's unstructured top of tables, which is what
// ends the walk. The same convention typeof^'s old value reflection used.
typedef struct RtSeen {
    const LhatType *type;
    const struct RtSeen *outer;
} RtSeen;

static LhatRuntimeType *rt_from_checked(LhatHeap *heap,
                                        const LhatType *type,
                                        const RtSeen *seen)
{
    if (type == NULL) {
        return NULL;
    }
    if (type->kind == LHAT_TYPE_TABLE) {
        for (const RtSeen *s = seen; s != NULL; s = s->outer) {
            if (s->type == type) {
                return lhat_type_rt_new(heap, LHAT_TYPE_RT_TABLE);
            }
        }
    }
    RtSeen here = { type, seen };
    seen = &here;
    switch (type->kind) {
        // 03 の 3.4: inference did not decide this one. Asks nothing of a
        // value, the same as nothing written -- but 14.16 writes it out as
        // UNKNOWN rather than any^, so a signature says which parameter or
        // member is still waiting for an annotation.
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_UNKNOWN);

        case LHAT_TYPE_ANY:
        case LHAT_TYPE_NONE:
            return NULL;  // asks nothing, same as nothing written (13.7)

        case LHAT_TYPE_NIL:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_NIL);
        case LHAT_TYPE_BOOL:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_BOOL);
        case LHAT_TYPE_NUMBER:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_NUMBER);
        case LHAT_TYPE_STRING:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_STRING);

        // 05 の 8.9: identity is the tag, carried across whole.
        case LHAT_TYPE_HOSTVALUE: {
            LhatRuntimeType *rt =
                lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE);
            if (rt != NULL) {
                rt->hostvalue_tag = type->v.table.hostvalue_tag;
            }
            return rt;
        }

        case LHAT_TYPE_TABLE: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_STRUCTURE);
            if (rt == NULL) {
                return NULL;
            }
            // 14.10改: the sequence half first, in position order, the way a
            // written t^{ ... } puts it.
            size_t sequence = 0;
            for (;;) {
                const LhatTypeMember *m =
                    lhat_type_member_at(type, sequence + 1);
                if (m == NULL) {
                    break;
                }
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, m->type, seen))) {
                    return NULL;
                }
                sequence++;
            }
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                bool positional = false;
                for (size_t i = 0; i < sequence; i++) {
                    if (lhat_type_member_at(type, i + 1) == m) {
                        positional = true;
                        break;
                    }
                }
                if (positional) {
                    continue;
                }
                LhatString *name = lhat_string_new(heap, m->name, m->name_length);
                if (name == NULL ||
                    !lhat_type_rt_add_member(rt, name, rt_from_checked(heap, m->type, seen))) {
                    return NULL;
                }
            }
            if (type->v.table.variadic != NULL) {
                rt->variadic = rt_from_checked(heap, type->v.table.variadic, seen);
            }
            lhat_type_rt_sort_members(rt);
            return rt;
        }

        case LHAT_TYPE_FUNC: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_SUBROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            rt->is_function = type->v.func.is_function;
            rt->takes_self = type->v.func.takes_self;
            for (LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, p->type, seen))) {
                    return NULL;
                }
            }
            if (type->v.func.variadic != NULL) {
                rt->variadic = rt_from_checked(heap, type->v.func.variadic, seen);
            }
            rt->result = rt_from_checked(heap, type->v.func.result, seen);
            return rt;
        }

        case LHAT_TYPE_CORO: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            rt->receive = rt_from_checked(heap, type->v.coroutine.receive, seen);
            rt->produce = rt_from_checked(heap, type->v.coroutine.produce, seen);
            rt->result = rt_from_checked(heap, type->v.coroutine.result, seen);
            rt->is_function = type->v.coroutine.is_function;  // 15.3改
            return rt;
        }

        case LHAT_TYPE_ERROR:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR);
        // 04 の 2.3 makes ERROR the supertype of every kind; nothing here
        // reaches the runtime LhatErrorKind object a checker-side name would
        // need to name a precise one, so this is the sound coarser answer.
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR);

        case LHAT_TYPE_UNION: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_UNION);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }

        case LHAT_TYPE_INTERSECT: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_INTERSECT);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }
    }
    return NULL;
}

// 14.16改 (S36): typeof^ answers the checker's settled type wherever one
// exists -- with one carve-out. An error's identity is the declaration (04
// の 2.4), and the checker's type carries only its NAME; the runtime
// LhatErrorKind object it would take to build the descriptor is not
// reachable from here. The value carries the kind as a pointer read, so an
// operand whose type mentions an error anywhere is answered by the tag
// instruction instead -- which is also what keeps typeof^(e) naming the
// leaf kind rather than the declared union.
//
// 13.13: a Self^ makes the walk come back to a type it has already read, so
// the tables on the way in are remembered on the C stack the way
// rt_from_checked remembers its own -- `seen` is NULL at the outermost
// call. Meeting one again says false: nothing is found here that the first
// visit will not find.
static bool mentions_error(const LhatType *type, const RtSeen *seen)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_TABLE) {
        for (const RtSeen *s = seen; s != NULL; s = s->outer) {
            if (s->type == type) {
                return false;
            }
        }
    }
    RtSeen here = { type, seen };
    seen = &here;
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_KIND:
        case LHAT_TYPE_ERROR_SET:
            return true;

        case LHAT_TYPE_TABLE:
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (mentions_error(m->type, seen)) {
                    return true;
                }
            }
            return type->v.table.variadic != NULL &&
                   mentions_error(type->v.table.variadic, seen);

        case LHAT_TYPE_FUNC:
            for (LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (mentions_error(p->type, seen)) {
                    return true;
                }
            }
            return (type->v.func.variadic != NULL &&
                    mentions_error(type->v.func.variadic, seen)) ||
                   mentions_error(type->v.func.result, seen);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (mentions_error(a->type, seen)) {
                    return true;
                }
            }
            return false;

        case LHAT_TYPE_CORO:
            return mentions_error(type->v.coroutine.receive, seen) ||
                   mentions_error(type->v.coroutine.produce, seen) ||
                   mentions_error(type->v.coroutine.result, seen);

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// 02 の 14 章: the object model
// ---------------------------------------------------------------------------

// Reads a name into a register, wherever it is held. Returns false when there
// is no such name anywhere.
// 05 の 8.2: the member of L^ a host-bound name reaches, or NULL when the host
// bound no such name. Read after the scopes, so a let^ of the same spelling
// shadows it -- and nothing is placed in a scope for one, which is what keeps
// 8.1's "the let^ in this place tell you what is here" true of the source.
static const char *initial_binding_member(Compiler *c, const char *name,
                                          size_t length)
{
    const LhatUnits *units = root_of(c)->units;
    if (units == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < units->initial_count; i++) {
        const char *bound = units->initial_names[i];
        if (bound != NULL && strlen(bound) == length &&
            memcmp(bound, name, length) == 0) {
            return units->initial_members[i];
        }
    }
    return NULL;
}

static bool resolve_name(Compiler *c, const char *name, size_t length,
                         uint8_t into)
{
    const Local *local = find_local(c, name, length);
    if (local != NULL) {
        // 05 の 8.9: a host value name moves as its width of slots. The
        // destination was sized by the same checker stamp, so the two
        // agree. (emit_move_wide drops a self-move, which was always a
        // no-op here.)
        emit_move_wide(c, into, local->reg,
                       local->width > 1 ? local->width : 1);
        return true;
    }
    size_t upvalue = find_upvalue(c, name, length);
    if (upvalue != SIZE_MAX) {
        emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
        return true;
    }
    // 05 の 8.2: nothing new exists at run time for one of these -- the name
    // compiles to reading the member of L^ the host bound it to.
    const char *member = initial_binding_member(c, name, length);
    if (member == NULL) {
        return false;
    }
    uint8_t mark = c->next_register;
    uint8_t key = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
    load_string_bytes(c, key, member, strlen(member));
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
    c->next_register = mark;
    return true;
}

// 02 の 14.12改: whether this is super^ written out. Only the hatted spelling
// means it, so an ordinary name `super` is untouched.
static bool is_super_ident(Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           node_name(c, node, &name, &length) && name_is(name, length, "super^");
}

static void compile_default_new(Compiler *c, const LhatNode *node,
                                uint8_t definition);

static const DefDecl *find_def_decl(Compiler *c, const char *name,
                                    size_t length)
{
    const Compiler *root = root_of(c);
    for (size_t i = 0; i < root->def_count; i++) {
        const DefDecl *decl = &root->defs[i];
        if (decl->length == length && memcmp(decl->name, name, length) == 0) {
            return decl;
        }
    }
    return NULL;
}

// The chain an expression stands for: a def^ literal is one link, and a
// composition is whatever the left names followed by the right. 14.2 makes
// this decidable without running anything, which is the point of fixing the
// chain at the definition.
static bool def_chain_of(Compiler *c, const LhatNode *node, DefChain *out)
{
    if (node == NULL) {
        return false;
    }
    if (node->kind == LHAT_NODE_DEF) {
        if (out->count >= LHAT_MAX_DEF_CHAIN) {
            return false;
        }
        out->lexers[out->count] = c->lexer;
        out->parts[out->count++] = node;
        return true;
    }
    // 14.5: composition is '..', and the order matters -- the right side is
    // what may override.
    if (node->kind == LHAT_NODE_BINARY && node->v.binary.op == LHAT_OP_CONCAT) {
        return def_chain_of(c, node->v.binary.left, out) &&
               def_chain_of(c, node->v.binary.right, out);
    }

    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node, &name, &length)) {
        return false;
    }
    const DefDecl *decl = find_def_decl(c, name, length);
    if (decl == NULL) {
        return false;
    }
    for (size_t i = 0; i < decl->chain.count; i++) {
        if (out->count >= LHAT_MAX_DEF_CHAIN) {
            return false;
        }
        out->lexers[out->count] = decl->chain.lexers[i];
        out->parts[out->count++] = decl->chain.parts[i];
    }
    return true;
}

// 14.9 keeps the name out of the definition, so it is picked up from the
// let^ that binds it. That is enough for 14.2's chain to be followed.
static void declare_defs(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        const LhatNode *target = s->v.binding.targets;
        const LhatNode *value = s->v.binding.values;
        if (target == NULL || target->next != NULL || value == NULL) {
            continue;
        }

        DefChain chain;
        chain.count = 0;
        if (!def_chain_of(c, value, &chain)) {
            continue;
        }
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, define_target_name(target), &name, &length)) {
            continue;
        }

        Compiler *root = root_of(c);
        if (root->def_count == root->def_capacity) {
            size_t grown = root->def_capacity ? root->def_capacity * 2 : 4;
            DefDecl *bigger =
                (DefDecl *)lhat_realloc(root->defs, grown * sizeof *bigger);
            if (bigger == NULL) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            root->defs = bigger;
            root->def_capacity = grown;
        }
        DefDecl *decl = &root->defs[root->def_count++];
        decl->name = name;
        decl->length = length;
        decl->chain = chain;
    }
}

// The template of one def^, or NULL. 14.13 allows one per definition, and it
// is the entry with no key.
static const LhatNode *template_of(const LhatNode *def)
{
    for (const LhatNode *entry = def->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key == NULL) {
            return entry->v.entry.value;
        }
    }
    return NULL;
}

static bool entry_named(Compiler *c, const LhatNode *entries, const char *name,
                        size_t length)
{
    for (const LhatNode *entry = entries; entry != NULL; entry = entry->next) {
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

// 14.5改: whether two parts of the chain both write this name with no marker
// between them. The checker calls that ambiguous and refuses to read it
// through the composition, so nothing is written under it -- a table holding
// the last writer would be one the checker says is not there.
//
// Only a member is ever ambiguous. A field collision stays an error, since
// there is no per-part storage to fall back to.
static bool ambiguous_member(Compiler *c, const DefChain *chain,
                             const char *name, size_t length)
{
    size_t plain = 0;
    for (size_t i = 0; i < chain->count; i++) {
        const LhatLexer *enclosing = c->lexer;
        c->lexer = chain->lexers[i];
        for (const LhatNode *entry = chain->parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            const char *written = NULL;
            size_t written_length = 0;
            if (entry->v.entry.key == NULL || entry->v.entry.declared ||
                entry->v.entry.modifier != LHAT_DEF_PLAIN) {
                continue;
            }
            if (node_name(c, entry->v.entry.key, &written, &written_length) &&
                written_length == length &&
                memcmp(written, name, length) == 0) {
                plain++;
                break;  // one part writing it twice is 14.12's own error
            }
        }
        c->lexer = enclosing;
    }
    return plain > 1;
}

// 14.11: self^{ … } inside new makes the instance. The fields it names are
// filled from what it wrote; the rest come from the template's initialisers,
// which 14.11 makes expressions evaluated at each construction rather than
// values stored anywhere -- so they are compiled here, at the construction.
//
// 14.11 also settles what an initialiser can see: not self^, which does not
// exist yet, but class^, which does.
static void compile_self_table(Compiler *c, const LhatNode *node, uint8_t into)
{
    const DefChain *chain = NULL;
    for (Compiler *at = c; at != NULL; at = at->parent) {
        if (at->building != NULL) {
            chain = at->building;
            break;
        }
    }
    if (chain == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t definition = reserve(c);
    if (!resolve_name(c, "class^", 6, definition)) {
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    emit(c, lhat_encode_abc(LHAT_BC_NEWINSTANCE, into, definition, 0));
    c->next_register = mark;

    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        // 14.15: a field left for the composition to fill has no initializer
        // to run. Whichever part provides it writes one here instead.
        if (entry->v.entry.declared) {
            continue;
        }
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

    // 14.11: in the order they were written, base first, and a field new
    // named is not initialised twice -- producing a value to be overwritten
    // is not something an initialiser should be made to do.
    for (size_t i = 0; i < chain->count; i++) {
        const LhatNode *fields = template_of(chain->parts[i]);
        if (fields == NULL) {
            continue;
        }
        // The part may have been read from an earlier input, and its offsets
        // mean nothing against this one's text.
        const LhatLexer *enclosing_lexer = c->lexer;
        c->lexer = chain->lexers[i];
        for (const LhatNode *field = fields->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            // 14.15: nothing to initialise, and a later part of the chain
            // carries the one that does.
            if (field->v.entry.declared) {
                continue;
            }
            if (field->v.entry.key == NULL ||
                !node_name(c, field->v.entry.key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            if (entry_named(c, node->v.list.items, name, length)) {
                continue;
            }
            uint8_t at = c->next_register;
            uint8_t key = reserve(c);
            uint8_t value = reserve(c);
            load_string_bytes(c, key, name, length);
            compile_expression(c, field->v.entry.value, value);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, value));
            c->next_register = at;
        }
        c->lexer = enclosing_lexer;
    }
}

// 14.1 and 14.3: a definition is a table of the members every instance
// shares. The template is not among them -- it belongs to the instances, and
// 14.11 keeps it in the compiler as initialisers rather than as any value.
//
// The chain is flattened here, which is what 14.2 permits by settling
// delegation at the definition: a later part's member simply overwrites an
// earlier one's, which is what override^ (14.12) means.
static void compile_def(Compiler *c, const LhatNode *node, uint8_t into)
{
    DefChain chain;
    chain.count = 0;
    if (!def_chain_of(c, node, &chain)) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // 14.9: the same structure a table literal makes, marked as the one a
    // def^ made. Nothing about conformance reads that (11.3 keeps identity
    // structural) -- 14.17改 does, to tell whose the member names are.
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, into, 1, 0));

    // class^ names the definition (14.4), and binding it as an ordinary local
    // is what lets a method or an initialiser reach it -- through the capture
    // of 5.4, with nothing special added.
    size_t local_mark = c->local_count;
    if (c->local_count >= LHAT_MAX_LOCALS) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    // 01 の 8 章: the def^'s '{' opens a scope for '$^' to count, and class^
    // is a name of it -- the checker binds self^ and class^ into the Scope
    // it pushes here, so both sides count this one the same.
    c->scope_depth++;
    Local *local = &c->locals[c->local_count++];
    local->name = "class^";
    local->length = 6;
    local->reg = into;
    local->depth = c->scope_depth;
    local->width = 1;

    const DefChain *enclosing = c->building;
    c->building = &chain;

    bool has_new = false;
    for (size_t i = 0; i < chain.count; i++) {
        // 03 の 4.3: a part read from an earlier input carries offsets into
        // that input's text, so the lexer travels with it.
        const LhatLexer *enclosing_lexer = c->lexer;
        c->lexer = chain.lexers[i];
        for (const LhatNode *entry = chain.parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (entry->v.entry.key == NULL) {
                continue;  // the template; 14.11 handles it at construction
            }
            // 14.15: a declaration carries a type and no value, so there is
            // nothing to write. What fills it comes from a later part.
            if (entry->v.entry.declared) {
                continue;
            }
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, entry->v.entry.key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                break;
            }
            if (name_is(name, length, "new")) {
                has_new = true;
            }
            // 14.5改: nothing goes under a name the checker will not read.
            if (entry->v.entry.modifier == LHAT_DEF_PLAIN &&
                ambiguous_member(c, &chain, name, length)) {
                continue;
            }
            uint8_t at = c->next_register;
            size_t entry_mark = c->local_count;
            uint8_t key = reserve(c);
            uint8_t value = reserve(c);
            load_string_bytes(c, key, name, length);

            // 14.12改: super^ is what this entry is about to write over. The
            // parts are walked in order, so the table holds the earlier one
            // right now -- reading it here is reading it before the write.
            //
            // It is an ordinary local, so a body reaches it through the
            // capture of 5.4 the way it reaches class^. Bound before the
            // value is compiled, since that is when the capture is made.
            if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                c->local_count < LHAT_MAX_LOCALS) {
                uint8_t hidden = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, hidden, into, key));
                Local *previous = &c->locals[c->local_count++];
                previous->name = "super^";
                previous->length = 6;
                previous->reg = hidden;
                previous->depth = c->scope_depth;
                previous->width = 1;
            }

            compile_expression(c, entry->v.entry.value, value);
            // 14.12: overload^ keeps what was there and adds a way to call
            // it, so the two go under one name together. Which one a call
            // means is settled when it runs, since 14.12's ban on overlapping
            // signatures leaves at most one that fits.
            LhatOpcode write = LHAT_BC_SETINDEX;
            uint8_t operand = value;
            if (entry->v.entry.modifier == LHAT_DEF_OVERLOAD) {
                write = LHAT_BC_ADDOVERLOAD;
            } else if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                // 14.12: and an override^ over an overloaded name takes the
                // one arm it overlaps rather than the group.
                write = LHAT_BC_OVERRIDEINDEX;
                // 03 の 5.11c: the checker knows which arm that is, so the
                // group can keep its shape instead of carrying the replaced
                // arm behind the replacement. OVERRIDEARM reads the value at
                // key + 1, which is where it is -- but the fallback keeps
                // that from being a silent assumption.
                if (entry->checked_arm != 0 &&
                    entry->checked_arm - 1 <= 0xFF && value == key + 1) {
                    write = LHAT_BC_OVERRIDEARM;
                    operand = (uint8_t)(entry->checked_arm - 1);
                }
            }
            emit(c, lhat_encode_abc(write, into, key, operand));
            // The slot goes back to the pool for the next entry, so a body
            // that captured it has to stop sharing it first.
            if (c->local_count > entry_mark) {
                emit(c, lhat_encode_abc(LHAT_BC_CLOSE, at, 0, 0));
                c->local_count = entry_mark;
            }
            c->next_register = at;
        }
        c->lexer = enclosing_lexer;
    }

    // 14.11: without a new of its own, a definition gets one that takes no
    // arguments and returns what the template says. Written out it is
    // 'new := f^ { return^ self^{ } }', so that is what is compiled.
    if (!has_new && *c->status == LHAT_COMPILE_OK) {
        compile_default_new(c, node, into);
    }

    c->building = enclosing;
    c->local_count = local_mark;
    c->scope_depth--;
}

// The new of 14.11 that a definition gets when it declares none: a function
// of no arguments answering what the template says. It is compiled as a body
// of its own so that it is an ordinary member, callable like any other.
static void compile_default_new(Compiler *c, const LhatNode *node,
                                uint8_t definition)
{
    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    proto->is_function = true;

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

    uint8_t slot = reserve(&inner);
    // An empty self^{ … }: everything comes from the template.
    LhatNode empty;
    memset(&empty, 0, sizeof empty);
    empty.kind = LHAT_NODE_SELF_TABLE;
    empty.offset = node->offset;
    empty.line = node->line;
    empty.column = node->column;
    compile_self_table(&inner, &empty, slot);
    emit(&inner, lhat_encode_abc(LHAT_BC_RETURN, slot, 0, 0));

    uint8_t mark = c->next_register;
    uint8_t key = reserve(c);
    uint8_t value = reserve(c);
    load_string_bytes(c, key, "new", 3);
    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, value, (uint16_t)index));
    emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, definition, key, value));
    c->next_register = mark;
}

// 01 の 5.4: one piece of an interpolated string. A text run is its own
// bytes; a hole is its value written down -- 02 の 14.17's tostring, reached
// and called the way a program would write it, so a type carrying one of its
// own answers with it. A format written after ':' becomes tostring's second
// argument, which 14.17 gives to number^ alone.
//
// `at` is the first of three consecutive registers the caller reserved: 5.3
// lays a method call out as callee, receiver, then arguments, and the key is
// read out of the third before the argument is written over it.
static void compile_interp_part(Compiler *c, const LhatNode *part, uint8_t at)
{
    if (part->kind == LHAT_NODE_INTERP_TEXT) {
        load_string(c, at, part);
        return;
    }

    uint8_t receiver = (uint8_t)(at + 1);
    uint8_t argument = (uint8_t)(at + 2);
    compile_expression(c, part->v.hole.value, receiver);
    // 14.17改: the hat spelling is the one that reaches the built-in on every
    // value -- a hole may hold a plain table, where the bare name is the
    // writer's and may hold anything.
    load_string_bytes(c, argument, "tostring^", 9);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, at, receiver, argument));

    uint8_t given = 0;
    if (part->v.hole.format != NULL) {
        load_string(c, argument, part->v.hole.format);
        given = 1;
    }
    emit(c, lhat_encode_abc(LHAT_BC_CALLMETHOD, at, given, 0));
}

// 01 の 5.4: the pieces joined, left to right. Every piece is a string^ by
// the time it is here, so this is 11.2's '..' over them and nothing more --
// there is no separate way of building a string for interpolation to use.
static void compile_interp(Compiler *c, const LhatNode *node, uint8_t into)
{
    uint8_t mark = c->next_register;
    bool started = false;

    for (const LhatNode *part = node->v.list.items; part != NULL;
         part = part->next) {
        // A hole wants three slots of its own, and the first piece cannot
        // simply take `into` and the two above it -- what those hold belongs
        // to whoever reserved them.
        uint8_t piece = reserve(c);
        reserve(c);
        reserve(c);
        compile_interp_part(c, part, piece);
        if (started) {
            emit(c, lhat_encode_abc(LHAT_BC_CONCAT, into, into, piece));
        } else {
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, piece, 0));
            started = true;
        }
        c->next_register = mark;
    }

    if (!started) {
        load_string_bytes(c, into, "", 0);  // $"" says nothing, at length 0
    }
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

        if (entry->v.entry.computed) {
            // 14.6改: the key is an expression, evaluated here like any
            // other. 04 の 11.3 keeps nil^ and a NaN out of a key, which the
            // machine reports when it lands.
            compile_expression(c, entry->v.entry.key, key);
        } else if (entry->v.entry.key != NULL) {
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

    // 14.4: 'x.m()' hands x to a method as its self^, and 'm(x)' on the same
    // member does the same by hand. So the receiver is put in place here and
    // the machine passes it or skips it depending on what the callee takes.
    const LhatNode *target = node->v.access.target;
    bool method = target != NULL && target->kind == LHAT_NODE_MEMBER;
    // 14.12改: super^(…) is the bound form too. What it replaces is a member
    // of this same definition, so the receiver is the self^ the body already
    // holds -- laid out here the way a method call lays it out, and the
    // machine skips it when the hidden member turns out to take none.
    bool super_call = !method && is_super_ident(c, target);
    if (method) {
        // 05 の 8.9: a host value receiver takes its width of slots, and the
        // machine reads the member off its head tag -- so the receiver run
        // is kept whole below the arguments exactly as a one-slot one is.
        size_t receiver_width = width_of(target->v.access.target);
        uint8_t receiver = reserve_wide(c, receiver_width);
        compile_expression(c, target->v.access.target, receiver);
        uint8_t key = c->next_register;
        if (key >= LHAT_MAX_REGISTERS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        (void)reserve(c);
        compile_key(c, target, key);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, callee, receiver, key));
        c->next_register = (uint8_t)(receiver + receiver_width);
    } else if (super_call) {
        compile_expression(c, target, callee);
        uint8_t receiver = reserve(c);
        if (!resolve_name(c, "self^", 5, receiver)) {
            // A static member has no self^ to hand over, and 14.4 says its
            // replacement takes none either. The plain call form is right.
            c->next_register = (uint8_t)(callee + 1);
            super_call = false;
        }
    } else {
        compile_expression(c, target, callee);
    }
    method = method || super_call;

    // 04 の 11.4改 with 01 の 7.1 (S43): '?(' answers nil^ for an absent
    // callee instead of calling one. Placed here, after the callee is in
    // place and before any argument is compiled, so an absent callee
    // evaluates no argument -- the same short circuit '?.' makes over its
    // key. The receiver of a method form is already in its slot above; it
    // was going to be evaluated either way, since it is what the callee was
    // read out of.
    size_t past_call = SIZE_MAX;
    if (node->v.access.nil_safe) {
        uint8_t test = c->next_register;
        (void)reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, callee, 0));
        size_t to_call = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = test;
        emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
        past_call = emit_jump(c, LHAT_BC_JUMP, 0);
        lhat_chunk_patch_here(&c->proto->chunk, to_call);
    }

    size_t count = 0;
    bool spread = false;
    bool wide_args = false;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        // 05 の 8.9: a host value argument takes its width of consecutive
        // slots; the callee's parameter run was laid out by the same widths,
        // so the frame window still lines up with no copying.
        uint8_t slot = reserve_for(c, arg);
        wide_args = wide_args || width_of(arg) > 1;
        // 13.7: the table itself goes in the slot; C tells the machine the
        // last one is not an ordinary argument but something to unpack.
        if (arg->kind == LHAT_NODE_SPREAD) {
            compile_expression(c, arg->v.jump.value, slot);
            spread = true;
        } else {
            compile_expression(c, arg, slot);
        }
        count++;
    }
    if (count > 0xFF) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    // 05 の 8.9: a spread re-reads the argument run by value index, which a
    // wide argument would put out of step. The checker refuses a host value
    // into a variadic tail already; the mixed form is refused here.
    if (spread && wide_args) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // 03 の 5.11c: strict settled which candidate of an overloaded member this
    // call means, so the search 5.11改 would run is replaced by taking that
    // one. Emitted here rather than beside the callee so the one place that
    // knows the call is complete is the one place that decides -- the
    // arguments are above the callee and PICKARM touches nothing but it.
    if (node->checked_arm != 0) {
        emit(c, lhat_encode_abx(LHAT_BC_PICKARM, callee,
                                (uint16_t)(node->checked_arm - 1)));
    }
    // 05 の 8.9: a call that answers a host value has the machine write the
    // whole width at the callee slot, so the frame has to be at least that
    // wide there even when the arguments took less.
    while (c->next_register < callee + width_of(node)) {
        reserve(c);
    }
    emit(c, lhat_encode_abc(method ? LHAT_BC_CALLMETHOD : LHAT_BC_CALL, callee,
                            (uint8_t)count, spread ? 1 : 0));
    // The answer then moves to the destination the same way it was written.
    emit_move_wide(c, into, callee, width_of(node));
    // S43: where an absent callee's nil^ lands, past everything the call
    // itself does.
    if (past_call != SIZE_MAX) {
        lhat_chunk_patch_here(&c->proto->chunk, past_call);
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
    proto->yields = node->v.func.yields;

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
    //
    // 13.4, 03 の 5.3: v.param.fallback is skipped on purpose. A default is
    // written into a call site by completion or the visual editor as the call
    // is built, so by the time anything runs the argument is there like any
    // other -- there is no defaulting left for the callee to do. Contrast the
    // fields of an error kind (04 の 2.2), whose defaults do get compiled, at
    // the construction rather than here.
    bool wide_param = false;  // 05 の 8.9: any parameter wider than a slot
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        const char *name = NULL;
        size_t length = 0;
        // 13.7: '...' takes the name slot instead of a written name, and
        // what it names in the body is the collector CALL/CALLMETHOD builds
        // -- so it is still one local, the way any other parameter is.
        if (param->v.param.variadic) {
            name = "...";
            length = 3;
            proto->has_variadic = true;
        } else if (!node_name(&inner, param->v.param.name, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // 14.4: a first parameter written self^ is what marks a method. No
        // modifier says so; the shape of the signature does.
        //
        // 11.3改 (S39): written last it marks one too, and says the receiver
        // is the RIGHT operand -- check.c refuses that on anything but an
        // op^, so reading it here is reading a shape already judged.
        if (name_is(name, length, "self^")) {
            if (param == node->v.func.params) {
                proto->takes_self = true;
            } else if (param->next == NULL) {
                proto->takes_self = true;
                proto->self_last = true;
            }
        }
        if (inner.local_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        // 05 の 8.9: a host value parameter takes its registered width of
        // consecutive slots; the caller lays the argument out the same way,
        // so the windows agree without any copying.
        const LhatHostValueTag *param_hostvalue =
            resolve_hostvalue_type_tag(&inner, param->v.param.type);
        size_t param_width = param_hostvalue != NULL ? param_hostvalue->width
                                                     : 1;
        // The two places that still count arguments by value index rather
        // than by slot: a variadic collection, and a yielding body's copy
        // of its arguments into the coroutine's registers. Both are refused
        // with a wide parameter rather than silently read out of step.
        // ('...' comes last, so has_variadic is checked again after the
        // loop for the parameters that preceded it.)
        if (param_width > 1 &&
            (node->v.func.yields || param->v.param.variadic)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        wide_param = wide_param || param_width > 1;
        uint8_t slot = reserve_wide(&inner, param_width);
        Local *local = &inner.locals[inner.local_count++];
        local->name = name;
        local->length = length;
        local->reg = slot;
        local->depth = inner.scope_depth;  // a parameter belongs to the body
        local->width = (uint8_t)param_width;

        // 14.12: the search that resolves an overloaded call asks each
        // candidate what it takes, so each body carries that with it. For
        // '...' this is the element type, not the collector's own type --
        // fits_call reads it that way once has_variadic says to.
        struct LhatRuntimeType **types =
            (struct LhatRuntimeType **)lhat_realloc(proto->parameter_types,
                                               ((size_t)proto->parameters + 1) *
                                                   sizeof *types);
        if (types == NULL) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        proto->parameter_types = types;
        types[proto->parameters] = lower_type(c, param->v.param.type);
        proto->parameters++;
    }
    // 05 の 8.9: see the wide-parameter refusal inside the loop -- '...'
    // comes last, so the parameters before it are re-judged here.
    if (wide_param && proto->has_variadic) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // 02 の 14.16: kept the same way parameter_types is, for typeof^ to
    // reconstruct the signature without touching the checker's types (03 の
    // 4.2 -- what runs cannot depend on whether checking did).
    proto->result_type = lower_type(c, node->v.func.return_type);
    if (node->v.func.return_type == NULL && node->checked_type != NULL) {
        // Nothing was written, so lower_type had nothing to read -- but
        // infer_func (check.c) already settled what the body actually
        // answers (03 の 3.4). Reaching for that only when checking ran
        // keeps compiling without it unaffected (test_vm.c does this on
        // purpose), and an explicitly written any^ above still wins since
        // this only fires when return_type itself is absent.
        const LhatType *checked = (const LhatType *)node->checked_type;
        proto->result_type = rt_from_checked(&root_of(c)->proto->chunk.heap,
                                             checked->v.func.result, NULL);
    }

    // 15.2, 13.9 (S28): Y and R have no written form at all -- 03 の 5.11a's
    // checked_type is the only place either can come from, written or not.
    if (node->v.func.yields && node->checked_type != NULL) {
        const LhatType *checked = (const LhatType *)node->checked_type;
        LhatHeap *owner = &root_of(c)->proto->chunk.heap;
        proto->yield_produce_type = rt_from_checked(owner, checked->v.func.yield_produce, NULL);
        proto->yield_receive_type = rt_from_checked(owner, checked->v.func.yield_receive, NULL);
    }

    // 02 の 10.1: a p^ body is a block that may carry a finally^, which is
    // where resources are handled and so where it is most wanted.
    compile_block_in_scope(&inner, node->v.func.body);
    if (lhat_chunk_emit(&proto->chunk,
                        lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0),
                        inner.line) == SIZE_MAX) {
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
        case LHAT_OP_CONCAT:   *out = LHAT_BC_CONCAT; return true;
        case LHAT_OP_EQ:       *out = LHAT_BC_EQ;   return true;
        case LHAT_OP_IS:       *out = LHAT_BC_SAME; return true;
        case LHAT_OP_NE:       *out = LHAT_BC_NE;   return true;
        case LHAT_OP_LT:       *out = LHAT_BC_LT;   return true;
        case LHAT_OP_GT:       *out = LHAT_BC_GT;   return true;
        case LHAT_OP_LE:       *out = LHAT_BC_LE;   return true;
        case LHAT_OP_GE:       *out = LHAT_BC_GE;   return true;
        case LHAT_OP_SPACESHIP: *out = LHAT_BC_SPACESHIP; return true;  // 11.9
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
    if (op == LHAT_OP_ISA) {
        compile_isa(c, node, into);
        return;
    }

    // 14.5: '..' composes two definitions, and 14.2 lets the compiler settle
    // which ones. Anything else spelled '..' is not composition.
    if (op == LHAT_OP_CONCAT) {
        DefChain chain;
        chain.count = 0;
        if (def_chain_of(c, node, &chain)) {
            compile_def(c, node, into);
            return;
        }
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
    // 05 の 8.9: a host value operand takes its width; the machine reads
    // that width off the head, so the instruction still names one register
    // per operand. The answer may be wide too (a registered "+"), which the
    // machine writes whole at `into` -- reserved by this node's own caller.
    uint8_t mark = c->next_register;
    uint8_t left = reserve_for(c, node->v.binary.left);
    uint8_t right = reserve_for(c, node->v.binary.right);
    compile_expression(c, node->v.binary.left, left);
    compile_expression(c, node->v.binary.right, right);
    emit(c, lhat_encode_abc(opcode, into, left, right));
    c->next_register = mark;
}

// 02 の 11.5 の (5): 'a < b < c' means '(a < b) and^ (b < c)', with the
// operand two links share **evaluated once**. Compiling it as the and^ it
// stands for would read `b` twice, and a call written there would run twice --
// which is the whole reason the parser keeps a chain as one node.
//
// Every link writes its answer into `into`, and a false one jumps past the
// rest: the same shape compile_binary gives and^, laid out along the chain.
// So the register holds the link that settled it, and nothing after a false
// link is evaluated at all.
static void compile_compare_chain(Compiler *c, const LhatNode *node,
                                  uint8_t into)
{
    uint8_t mark = c->next_register;
    const LhatNode *operand = node->v.chain.operands;
    if (operand == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // 05 の 8.9: a host value operand takes its width here as anywhere.
    uint8_t left = reserve_for(c, operand);
    compile_expression(c, operand, left);

    size_t settled[LHAT_MAX_LOCALS];
    size_t settled_count = 0;
    for (const LhatNode *marker = node->v.chain.operators; marker != NULL;
         marker = marker->next) {
        operand = operand->next;
        if (operand == NULL || *c->status != LHAT_COMPILE_OK) {
            break;
        }
        // Every link but the last leaves a jump for a false answer to take.
        if (settled_count > 0 && settled_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            break;
        }
        if (marker != node->v.chain.operators) {
            settled[settled_count++] =
                emit_jump(c, LHAT_BC_JUMP_FALSE, into);
        }

        LhatOpKind op = marker->v.unary.op;
        // 13.11: an isa^ link takes a type, which is not an operand the next
        // link could compare against -- so what it tests is the value still
        // standing to its left, and that value stays where it is.
        if (op == LHAT_OP_ISA) {
            compile_isa_test(c, operand, left, into);
            continue;
        }

        LhatOpcode opcode;
        if (!binary_opcode(op, &opcode)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            break;
        }
        uint8_t right = reserve_for(c, operand);
        compile_expression(c, operand, right);
        emit(c, lhat_encode_abc(opcode, into, left, right));
        left = right;  // shared with the next link, and already evaluated
    }

    for (size_t i = 0; i < settled_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, settled[i]);
    }
    c->next_register = mark;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node == NULL || *c->status != LHAT_COMPILE_OK) {
        return;
    }
    c->line = node->line;

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

        case LHAT_NODE_INTERP:
            compile_interp(c, node, into);
            return;

        case LHAT_NODE_TABLE:
            compile_table(c, node, into);
            return;

        case LHAT_NODE_ERROR_NEW:
            compile_error_new(c, node, into);
            return;

        case LHAT_NODE_DEF:
            compile_def(c, node, into);
            return;

        // 17.2: the expression form of a match. 16.1 makes for^ the place a
        // focus is defined whichever form follows it, so this is the one that
        // answers a value rather than running statements.
        case LHAT_NODE_FOR:
            if (node->v.loop.kind != LHAT_FOR_IF &&
                node->v.loop.kind != LHAT_FOR_WHEN) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            compile_for_once(c, node, into, true);
            return;

        // 14.13: the two readings of self^{ … } are told apart by where it
        // stands. Reaching it as an expression means the one inside new;
        // compile_def takes the template before anything gets here.
        case LHAT_NODE_SELF_TABLE:
            compile_self_table(c, node, into);
            return;

        case LHAT_NODE_TRY:
            compile_try(c, node, into);
            return;

        // 02 の 14.16改 (S36): typeof^ answers the type the checker settled,
        // compiled in as a constant -- type information is a compile-time
        // thing, and the run manufactures none (3.5改 and 5.13改 are the
        // same posture). The operand still runs, for whatever it does along
        // the way.
        //
        // Two things fall to the tag instruction instead. A type that
        // mentions an error is one: the value's own kind is a pointer read
        // and the leaf answer (04 の 2.4), where the static name cannot even
        // be built here (mentions_error). No checker having run is the
        // other: the instruction answers from the value's tag alone -- the
        // dispatch information every value already carries -- never by
        // walking a structure.
        case LHAT_NODE_TYPEOF: {
            uint8_t mark = c->next_register;
            uint8_t value = reserve(c);
            compile_expression(c, node->v.jump.value, value);
            const LhatType *checked = (const LhatType *)node->checked_type;
            if (checked != NULL && !mentions_error(checked, NULL)) {
                // 13.7: any^ converts to no descriptor at all ("asks
                // nothing"), but as an ANSWER it is a value of its own.
                LhatRuntimeType *rt =
                    checked->kind == LHAT_TYPE_ANY ||
                            checked->kind == LHAT_TYPE_NONE
                        ? lhat_type_rt_new(&c->proto->chunk.heap,
                                           LHAT_TYPE_RT_ANY)
                        : rt_from_checked(&c->proto->chunk.heap, checked, NULL);
                c->next_register = mark;
                if (rt == NULL) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                load_constant(c, into, lhat_object((LhatObject *)rt));
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_TYPEOF, into, value, 0));
            c->next_register = mark;
            return;
        }

        // 11.6, S27: the operand lands in `into` and stays there -- as^
        // narrows the type the checker tracks, not the value, so there is
        // nothing to write back once the check passes. lower_type reads
        // the written type the same way an overload^ed parameter's does
        // (14.12), and lhat_value_satisfies (the same relation fits_call
        // already trusts) is the check LHAT_BC_ASCAST makes at run time.
        case LHAT_NODE_AS: {
            compile_expression(c, node->v.ascription.value, into);
            const LhatNode *asked = node->v.ascription.type;
            LhatRuntimeType *wanted = lower_type(c, asked);
            if (wanted == NULL) {
                // 13.7: any^ asks nothing, so there is nothing for
                // LHAT_BC_ASCAST to check. Anything else lower_type could
                // not settle is refused (5.13改) -- S27 promised as^ panics
                // on a mismatch, and a cast that silently checked nothing
                // would be that promise quietly broken.
                const char *name = NULL;
                size_t length = 0;
                if (node_name(c, asked, &name, &length) &&
                    name_is(name, length, "any^")) {
                    return;
                }
                fail(c, asked->kind == LHAT_NODE_TYPE_NAME ||
                             asked->kind == LHAT_NODE_MEMBER
                         ? LHAT_COMPILE_UNDEFINED
                         : LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t type_slot = reserve(c);
            load_constant(c, type_slot, lhat_object((LhatObject *)wanted));
            emit(c, lhat_encode_abc(LHAT_BC_ASCAST, into, type_slot, 0));
            c->next_register = mark;
            return;
        }

        // 02 の 15.8: delegation is the outer one driving the inner one. 03
        // の 5.7 writes the expansion out; the chain of coroutines is
        // registers rather than anything the machine holds.
        // 05 の 5 章: a unit is a body, so requiring it is making a closure
        // of it and calling that. 5.3's "once" is the guard the unit itself
        // begins with, which is why nothing is remembered here.
        // 05 の 8.7: what the host registered is already in L^.modules by the
        // time anything runs, so reaching it is a walk and no more.
        case LHAT_NODE_IMPORT:
        case LHAT_NODE_IMPORT_STMT: {
            uint8_t mark = c->next_register;
            uint8_t key = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
            load_string_bytes(c, key, "modules", 7);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
            compile_import_path(c, node->v.jump.value, into, key);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_REQUIRE:
        case LHAT_NODE_REQUIRE_STMT: {
            const LhatNode *path = node->v.jump.value;
            if (c->units == NULL || c->units->resolve == NULL || path == NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            size_t which = c->units->resolve(
                c->units->context, c->lexer->strings + path->v.string.offset,
                path->v.string.length, NULL);
            // Bx is 16 bits, so a program of more units than that cannot be
            // written down -- the same ceiling 5.2 puts on constants.
            if (which == LHAT_NO_UNIT || which > UINT16_MAX) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            emit(c, lhat_encode_abx(LHAT_BC_UNIT, into, (uint16_t)which));
            emit(c, lhat_encode_abc(LHAT_BC_CALL, into, 0, 0));
            return;
        }

        case LHAT_NODE_YIELD_ALL: {
            uint8_t mark = c->next_register;
            uint8_t co = reserve(c);
            uint8_t sent = reserve(c);
            uint8_t test = reserve(c);
            compile_expression(c, node->v.jump.value, co);
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, sent, 0, 0));

            size_t top = c->proto->chunk.count;
            emit(c, lhat_encode_abc(LHAT_BC_RESUME, sent, co, 0));
            emit(c, lhat_encode_abc(LHAT_BC_ISDONE, test, co, 0));
            size_t keep = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
            size_t done = emit_jump(c, LHAT_BC_JUMP, 0);

            lhat_chunk_patch_here(&c->proto->chunk, keep);
            // What the inner one yielded goes straight out, and what the
            // resume sends comes back to be passed in next time round.
            emit(c, lhat_encode_abc(LHAT_BC_YIELD, sent, 0, 0));
            size_t back = emit_jump(c, LHAT_BC_JUMP, 0);
            if (back != SIZE_MAX) {
                int32_t offset = (int32_t)top - (int32_t)back - 1;
                c->proto->chunk.code[back] =
                    lhat_encode_jump(LHAT_BC_JUMP, 0, offset);
            }

            lhat_chunk_patch_here(&c->proto->chunk, done);
            // 15.8: the value of the whole thing is the inner return value.
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, sent, 0));
            c->next_register = mark;
            return;
        }

        // 02 の 15.4: an expression, not a statement -- what it answers is
        // what the resume sent, so one register carries both directions.
        case LHAT_NODE_YIELD:
            if (node->v.jump.value == NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
            } else {
                compile_expression(c, node->v.jump.value, into);
            }
            // 15.11: '_yield^' still works out what it would have sent, since
            // that expression may do something. What it does not do is
            // suspend -- and with nobody resuming it, nothing comes back.
            if (node->v.jump.phantom) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_YIELD, into, 0, 0));
            return;

        // 04 の 11.3: 't.foo' is resolved statically and 't[k]' is not, but
        // the machine performs one lookup either way. 5.1 keeps the checker's
        // knowledge out of the instruction set until specialisation.
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX: {
            uint8_t mark = c->next_register;
            // 05 の 8.9: a host value target takes its width of slots, or
            // the key would land inside its bytes.
            uint8_t target = reserve_for(c, node->v.access.target);
            compile_expression(c, node->v.access.target, target);

            // 04 の 11.4改 with 01 の 7.1 (S43): '?.' and '?[' answer nil^
            // for a nil^ target instead of reaching into one. The key is
            // compiled inside the branch, so an absent target does not
            // evaluate it -- what a reader expects of a form written to skip
            // the access, and what the other optional-chaining languages do.
            size_t past = SIZE_MAX;
            if (node->v.access.nil_safe) {
                uint8_t test = c->next_register;
                (void)reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, target, 0));
                size_t to_access = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = (uint8_t)(target + 1);
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                past = emit_jump(c, LHAT_BC_JUMP, 0);
                lhat_chunk_patch_here(&c->proto->chunk, to_access);
            }

            uint8_t key = reserve(c);
            compile_key(c, node, key);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, target, key));
            if (past != SIZE_MAX) {
                lhat_chunk_patch_here(&c->proto->chunk, past);
            }
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
            // 01 の 2.3改 (S35): a stacked reach -- it^^ the enclosing focus,
            // self^^/class^^ the enclosing def^'s, this^^ the enclosing
            // subroutine. The parser only lets those four through. The first
            // three are ordinary bindings resolved past their inner shadows;
            // this^ is an instruction rather than a binding, so its stacked
            // form is a capture of its own (resolve_this).
            if (node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats > 1) {
                size_t levels = node->v.name.hats - 1;
                if (name_is(name, length, "this^")) {
                    // The body `levels` out has to exist, and be a body --
                    // the unit's top level is not this^-able (15.10).
                    Compiler *target = c;
                    for (size_t i = 0; i <= levels && target != NULL; i++) {
                        target = target->parent;
                    }
                    if (target == NULL) {
                        fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                        return;
                    }
                    size_t up = resolve_this(c, levels);
                    if (up == SIZE_MAX) {
                        fail(c, LHAT_COMPILE_TOO_COMPLEX);
                        return;
                    }
                    emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into,
                                            (uint8_t)up, 0));
                    return;
                }
                size_t skip = levels;
                const Local *outer = find_local_skipping(c, name, length,
                                                         &skip);
                if (outer != NULL) {
                    emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, outer->reg, 0));
                    return;
                }
                size_t up = find_upvalue_skipping(c, name, length, skip);
                if (up == SIZE_MAX) {
                    // Fewer bindings of the name than the hats count out.
                    fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                    return;
                }
                emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)up,
                                        0));
                return;
            }
            // 01 の 2.2: three hat identifiers are values in themselves.
            // Everything else with a hat is a name like any other -- 16.2's
            // it^ is a name the for^ made, so it is looked up rather than
            // recognised.
            if (node->kind == LHAT_NODE_HAT_IDENT) {
                if (name_is(name, length, "true^") ||
                    name_is(name, length, "false^")) {
                    emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, into,
                                            name_is(name, length, "true^") ? 1 : 0,
                                            0));
                    return;
                }
                if (name_is(name, length, "nil^")) {
                    emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                    return;
                }
                // 15.10: the subroutine running, which is how a body with no
                // name recurses. Only the hatted spelling means it, so an
                // ordinary name `this` stays an ordinary name.
                if (name_is(name, length, "this^")) {
                    if (c->parent == NULL) {
                        fail(c, LHAT_COMPILE_UNDEFINED);
                        return;
                    }
                    emit(c, lhat_encode_abc(LHAT_BC_THIS, into, 0, 0));
                    return;
                }
                // 05 の 8.6: the machine's own table, reachable from anywhere
                // without being imported.
                if (name_is(name, length, "L^")) {
                    emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
                    return;
                }
            }
            // 05 の 8.2: resolve_name asks the scopes first and the host's
            // initial bindings last, which is what lets a let^ of the same
            // spelling shadow one.
            if (!resolve_name(c, name, length, into)) {
                fail(c, LHAT_COMPILE_UNDEFINED);
            }
            return;
        }

        // 01 の 8 章: the same name, looked for from a scope further out.
        case LHAT_NODE_SCOPE: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t reg = 0;
            size_t upvalue = 0;
            switch (resolve_scoped(c, node, name, length, &reg, &upvalue)) {
                case SCOPED_REGISTER:
                    emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, reg, 0));
                    return;
                case SCOPED_UPVALUE:
                    emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into,
                                            (uint8_t)upvalue, 0));
                    return;
                case SCOPED_TOO_FAR:
                    fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                    return;
                case SCOPED_NONE:
                    fail(c, LHAT_COMPILE_UNDEFINED);
                    return;
            }
            return;
        }

        case LHAT_NODE_UNARY: {
            uint8_t mark = c->next_register;
            // 05 の 8.9: a host value operand keeps its width, as everywhere.
            uint8_t operand = reserve_for(c, node->v.unary.operand);
            compile_expression(c, node->v.unary.operand, operand);
            // 11.7改2 (S42): 'x?' is '!(x isa^ nil^)' written short, and the
            // two instructions it needs already exist. NOT reads its operand
            // before it writes, so into == into is safe.
            if (node->v.unary.op == LHAT_OP_PRESENT) {
                emit(c, lhat_encode_abc(LHAT_BC_ISNIL, into, operand, 0));
                emit(c, lhat_encode_abc(LHAT_BC_NOT, into, into, 0));
                c->next_register = mark;
                return;
            }
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

        case LHAT_NODE_COMPARE_CHAIN:
            compile_compare_chain(c, node, into);
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

// 8.8: 'let^ a.b.c = v' puts c into a table reached through a and b. Only the
// root is a name of a scope; the rest are members.
static bool define_target_is_path(const LhatNode *target)
{
    return define_target_name(target)->kind == LHAT_NODE_MEMBER;
}

static const LhatNode *define_target_root(const LhatNode *target)
{
    const LhatNode *node = define_target_name(target);
    while (node->kind == LHAT_NODE_MEMBER) {
        node = node->v.access.target;
    }
    return node;
}

// 05 の 8.6: L^ names the machine's own table. Only the hatted spelling means
// it, so an ordinary name `L` is untouched.
static bool is_environment(const LhatNode *node, const char *name,
                           size_t length)
{
    return node->kind == LHAT_NODE_HAT_IDENT && name_is(name, length, "L^");
}

// Makes a table in a place that holds nothing yet, and leaves alone one that
// does. 8.8 has two paths through one table meet rather than replace each
// other, and 11.3 spells "nothing there" nil^ -- so this is the whole test.
static void ensure_table(Compiler *c, uint8_t slot)
{
    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, slot, 0));
    size_t there = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, slot, 0, 0));
    lhat_chunk_patch_here(&c->proto->chunk, there);
    c->next_register = mark;
}

// The same for a segment reached through a table, writing the new one back
// into owner[key] -- but only when one was actually made.
//
// 05 の 8.6改 (M5): writing back what was already there used to cost one
// instruction and save a branch. It is a write like any other, so the
// machine's own tables refuse it, and a path through L^ would fail on its own
// second segment. The branch is the cheaper of the two now.
static void ensure_table_at(Compiler *c, uint8_t slot, uint8_t owner,
                            uint8_t key)
{
    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, slot, 0));
    size_t there = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, slot, 0, 0));
    emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, slot));
    lhat_chunk_patch_here(&c->proto->chunk, there);
    c->next_register = mark;
}

// Puts the table a path segment names into `into`, making the ones the path
// does not reach yet. The checker has already refused a segment that cannot
// hold a member (8.8), so what is found here is a table or nothing.
static void compile_path_prefix(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node->kind != LHAT_NODE_MEMBER) {
        // The root: a slot of this frame, or a place an enclosing one holds.
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, node, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // 05 の 8.6: L^ is a place too, and the one nothing has to hold.
        if (is_environment(node, name, length)) {
            emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
            return;
        }
        const Local *local = find_local(c, name, length);
        if (local != NULL) {
            ensure_table(c, local->reg);
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, local->reg, 0));
            return;
        }
        size_t upvalue = find_upvalue(c, name, length);
        if (upvalue == SIZE_MAX) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
        ensure_table(c, into);
        emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, into, (uint8_t)upvalue, 0));
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t owner = reserve(c);
    uint8_t key = reserve(c);
    compile_path_prefix(c, node->v.access.target, owner);
    compile_key(c, node, key);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, owner, key));
    ensure_table_at(c, into, owner, key);
    c->next_register = mark;
}

// 8.7: a let^ name is visible across the whole scope, written before its own
// definition or after it. So every slot in a block is made before anything is
// compiled into it -- which is what lets a body call itself, and two bodies
// call each other, with nothing declared ahead of them.
static void declare_names(Compiler *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        // 05 の 5.4改: the short form makes one name too -- the root of the
        // path the unit declared. The rest of the path is members of it.
        if (s->kind == LHAT_NODE_REQUIRE_STMT ||
            s->kind == LHAT_NODE_IMPORT_STMT) {
            const char *module_name = NULL;
            if (s->kind == LHAT_NODE_IMPORT_STMT) {
                // 05 の 8.7: the path is written here, so the root is read
                // off the tree rather than off the unit that was required.
                const LhatNode *root_node = s->v.jump.value;
                while (root_node != NULL &&
                       root_node->kind == LHAT_NODE_MEMBER) {
                    root_node = root_node->v.access.target;
                }
                size_t length = 0;
                if (!node_name(c, root_node, &module_name, &length)) {
                    continue;
                }
                if (find_local(c, module_name, length) != NULL) {
                    continue;
                }
                if (c->local_count >= LHAT_MAX_LOCALS) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                uint8_t slot = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
                Local *local = &c->locals[c->local_count++];
                local->name = module_name;
                local->length = length;
                local->reg = slot;
                local->depth = c->scope_depth;
                local->width = 1;
                continue;
            }
            module_name = required_module_name(c, s);
            if (module_name == NULL) {
                continue;  // reported when the statement is compiled
            }
            size_t root = strcspn(module_name, ".");
            if (find_local(c, module_name, root) != NULL) {
                continue;
            }
            if (c->local_count >= LHAT_MAX_LOCALS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            uint8_t slot = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
            Local *local = &c->locals[c->local_count++];
            local->name = module_name;
            local->length = root;
            local->reg = slot;
            local->depth = c->scope_depth;
            local->width = 1;
            continue;
        }
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        const LhatNode *bound_value = s->v.binding.values;
        // 13.10 with 05 の 8.9: a destructuring spreads one value across the
        // targets rather than pairing them off, and a table never holds a
        // host value -- so every name it binds is one slot wide, and the
        // pairing below has nothing to say about them.
        if (bound_value != NULL && bound_value->kind == LHAT_NODE_UNPACK) {
            bound_value = NULL;
        }
        for (const LhatNode *target = s->v.binding.targets; target != NULL;
             target = target->next) {
            const char *name = NULL;
            size_t length = 0;
            // 05 の 8.9: the width this name will hold. The checker's stamp
            // on the value is the usual channel; a written annotation covers
            // a compile whose value node carries none.
            size_t width = 1;
            if (bound_value != NULL) {
                width = width_of(bound_value);
            }
            if (width == 1 && target->kind == LHAT_NODE_PARAM) {
                const LhatHostValueTag *tag =
                    resolve_hostvalue_type_tag(c, target->v.param.type);
                if (tag != NULL) {
                    width = tag->width;
                }
            }
            if (bound_value != NULL) {
                bound_value = bound_value->next;
            }

            // 8.8: a path introduces a member, so the only name it can make
            // is its root -- and only when nothing already holds that.
            // Reaching an enclosing table is the point of the form.
            if (define_target_is_path(target)) {
                const LhatNode *root = define_target_root(target);
                if (!node_name(c, root, &name, &length)) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    return;
                }
                if (is_environment(root, name, length) ||
                    find_local(c, name, length) != NULL ||
                    find_upvalue(c, name, length) != SIZE_MAX) {
                    continue;
                }
            } else if (!node_name(c, define_target_name(target), &name,
                                  &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }

            // 03 の 4.3: at the top level of a session, a name written again
            // is the same place written again. Reusing the slot is what keeps
            // a prompt from running out of registers, and it leaves what is
            // there readable -- 'let^ x = x + 1' means the x that is there.
            if (c->session_top && find_local(c, name, length) != NULL) {
                continue;
            }

            if (c->local_count >= LHAT_MAX_LOCALS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }

            // 03 の 4.3 with 05 の 8.9: a session's slots are one wide and
            // stay; the checker refuses this first, this is the backstop.
            if (width > 1 && c->session_top) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t slot = reserve_wide(c, width);
            // The slot may still hold what an earlier block left in it, and
            // 8.7 lets the name be read from a body before its let^ has run.
            // Emptying it makes that nil^ rather than rubbish.
            for (size_t i = 0; i < width; i++) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL,
                                        (uint8_t)(slot + i), 0, 0));
            }

            Local *local = &c->locals[c->local_count++];
            local->name = name;
            local->length = length;
            local->reg = slot;
            local->depth = c->scope_depth;
            local->width = (uint8_t)width;
        }
    }
}

// 02 の 8.6: let^ makes a name, and the name is a slot that stays. The slot
// itself was made by declare_names; this fills it.
// 02 の 13.10: one value taken apart by position, which is what the mark on
// the value says and what tells this from a multiple definition. The targets
// are the ordinary list either way (13.10), paths included.
//
// The source is compiled once and read from, since it is one value being
// spread rather than one per target. Each position comes out the way 16.3's
// walk takes a pair apart (bind_targets below) -- the two are the same
// operation, and 13.10 says so.
static void compile_unpack_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *unpack = node->v.binding.values;
    uint8_t mark = c->next_register;
    uint8_t source = reserve(c);
    compile_expression(c, unpack->v.jump.value, source);

    // 03 の 5.11c's channel: the positions the checker accounted for need no
    // check of their own. Zero under relaxed and on an unchecked compile,
    // where every one of them is checked instead.
    uint32_t confirmed = unpack->v.jump.level;

    size_t position = 0;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        if (position > LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        uint8_t inner = c->next_register;
        uint8_t key = reserve(c);
        load_constant(c, key, lhat_integer((int64_t)position));

        // 8.8: the place is a member of a table the path reaches, exactly as
        // in the ordinary define below.
        if (define_target_is_path(target)) {
            const LhatNode *last = define_target_name(target);
            uint8_t owner = reserve(c);
            uint8_t place = reserve(c);
            uint8_t slot = reserve(c);
            compile_path_prefix(c, last->v.access.target, owner);
            compile_key(c, last, place);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, slot, source, key));
            if (position > confirmed) {
                emit(c, lhat_encode_abc(LHAT_BC_CHECKPOS, slot,
                                        (uint8_t)position, 0));
            }
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, place, slot));
            c->next_register = inner;
            continue;
        }

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
        // 05 の 8.9: a table never holds a host value, so a position taken
        // out of one is never wide. The backstop is here rather than trusted.
        if (local->width > 1) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // 03 の 4.3, as in the ordinary define: a name an earlier input bound
        // stops sharing its place before this let^ writes it.
        if ((size_t)(local - c->locals) < c->session_locals) {
            emit(c, lhat_encode_abc(LHAT_BC_CLOSEONE, local->reg, 0, 0));
        }
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, local->reg, source, key));
        if (position > confirmed) {
            emit(c, lhat_encode_abc(LHAT_BC_CHECKPOS, local->reg,
                                    (uint8_t)position, 0));
        }
        c->next_register = inner;
    }
    c->next_register = mark;
}

static void compile_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    // 13.10: the mark is on the value, so this is where the two forms part.
    if (value != NULL && value->kind == LHAT_NODE_UNPACK) {
        compile_unpack_define(c, node);
        return;
    }
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        // 8.8: the place is a member of a table the path reaches. Everything
        // before the last segment is made where it is not there yet.
        if (define_target_is_path(target)) {
            if (value == NULL) {
                continue;
            }
            // 05 の 8.9: a path lands in a table, and a table never holds a
            // host value; the checker refused this first.
            if (width_of(value) > 1) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            const LhatNode *last = define_target_name(target);
            uint8_t mark = c->next_register;
            uint8_t owner = reserve(c);
            uint8_t key = reserve(c);
            uint8_t slot = reserve(c);
            compile_path_prefix(c, last->v.access.target, owner);
            compile_key(c, last, key);
            compile_expression(c, value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, slot));
            c->next_register = mark;
            value = value->next;
            continue;
        }

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
        // 03 の 4.3: a name an earlier input bound, written again. The slot
        // is reused (declare_names, so a prompt does not run out of them),
        // which makes this the one let^ that would otherwise be seen by a
        // closure that captured the earlier binding -- and 5.4's sharing is
        // for one binding, not for whatever later takes its place. Severing
        // it here is what keeps a redefinition to another type from making
        // an earlier closure's result type a lie; ':=' writes the same
        // binding and so goes on sharing it.
        if ((size_t)(local - c->locals) < c->session_locals) {
            emit(c, lhat_encode_abc(LHAT_BC_CLOSEONE, local->reg, 0, 0));
        }
        if (value != NULL) {
            compile_expression(c, value, local->reg);
            value = value->next;
        }
    }
}

// One target of a multiple assignment, carried from the pass that reads to the
// pass that writes. A member target holds the two registers its place was
// evaluated into, so that 7.4改's "the target is evaluated once" survives the
// split -- the write reaches the same owner and key the read did.
typedef struct {
    const LhatNode *target;
    uint8_t owner;
    uint8_t key;
    uint8_t value;
    bool indexed;
} PendingWrite;

// 13.8: reads everything, then writes everything. Only reached with more than
// one target -- see compile_reassign.
static void compile_reassign_parallel(Compiler *c, const LhatNode *node)
{
    PendingWrite pending[LHAT_MAX_LOCALS];
    size_t count = 0;
    uint8_t mark = c->next_register;
    const LhatNode *value = node->v.binding.values;

    // 13.10: one value spread across every target rather than one for each.
    // The source is compiled here, once, and each target reads its position
    // out of it below -- which is also why 13.8's read-everything before
    // write-everything needs nothing extra of this form: there is one read.
    //
    // Every target kind the write pass below handles comes for free that way
    // -- a name, a '$' specifier, an upvalue, a member of a table.
    const LhatNode *unpack =
        value != NULL && value->kind == LHAT_NODE_UNPACK ? value : NULL;
    uint8_t source = 0;
    uint32_t confirmed = 0;  // 03 の 5.11c, as in compile_unpack_define
    if (unpack != NULL) {
        source = reserve(c);
        compile_expression(c, unpack->v.jump.value, source);
        confirmed = unpack->v.jump.level;
    }

    size_t position = 0;
    for (const LhatNode *target = node->v.binding.targets;
         target != NULL && (unpack != NULL || value != NULL);
         target = target->next) {
        if (count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        position++;
        PendingWrite *w = &pending[count++];
        w->target = target;
        w->indexed = target->kind == LHAT_NODE_MEMBER ||
                     target->kind == LHAT_NODE_INDEX;
        w->owner = 0;
        w->key = 0;

        if (w->indexed) {
            // 05 の 8.9: a host value owner keeps its width, as in the
            // single-target path. (Writing a field through the copy here is
            // still the copy's -- the checker's escape rules never let a
            // host value be a table member, so an indexed target with a
            // wide owner only ever reads.)
            w->owner = reserve_for(c, target->v.access.target);
            w->key = reserve(c);
            compile_expression(c, target->v.access.target, w->owner);
            compile_key(c, target, w->key);

            // 7.4改, as in the single-target path: the current value comes
            // back through the owner and key already evaluated rather than
            // from compiling the target again. Never a destructuring: 13.10's
            // mark stands where a value would, so there is no operator on it.
            if (unpack == NULL && node->v.binding.has_compound_op &&
                value->kind == LHAT_NODE_BINARY) {
                LhatOpcode opcode;
                if (!binary_opcode(value->v.binary.op, &opcode)) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    return;
                }
                uint8_t current = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, current, w->owner,
                                        w->key));
                uint8_t rhs = reserve(c);
                compile_expression(c, value->v.binary.right, rhs);
                w->value = reserve(c);
                emit(c, lhat_encode_abc(opcode, w->value, current, rhs));
                value = value->next;
                continue;
            }
        }

        // 13.10: the position this target takes, read the way 16.3's walk
        // takes a pair apart.
        if (unpack != NULL) {
            uint8_t key = reserve(c);
            load_constant(c, key, lhat_integer((int64_t)position));
            w->value = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, w->value, source, key));
            if (position > confirmed) {
                emit(c, lhat_encode_abc(LHAT_BC_CHECKPOS, w->value,
                                        (uint8_t)position, 0));
            }
            continue;
        }

        // A name has no place to evaluate, so a compound value is compiled
        // whole: its left is the target node itself, which reads the register
        // the name already lives in.
        // 05 の 8.9: sized by the checker's stamp, so a host value rides the
        // two-pass exchange whole.
        w->value = reserve_for(c, value);
        compile_expression(c, value, w->value);
        value = value->next;
    }

    for (size_t i = 0; i < count; i++) {
        const PendingWrite *w = &pending[i];
        if (w->indexed) {
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, w->owner, w->key,
                                    w->value));
            continue;
        }

        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, w->target, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }

        if (w->target->kind == LHAT_NODE_SCOPE) {
            uint8_t reg = 0;
            size_t at = 0;
            ScopedKind found =
                resolve_scoped(c, w->target, name, length, &reg, &at);
            if (found == SCOPED_TOO_FAR) {
                fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                return;
            }
            if (found == SCOPED_NONE) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            if (found == SCOPED_REGISTER) {
                emit(c, lhat_encode_abc(LHAT_BC_MOVE, reg, w->value, 0));
            } else {
                emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, w->value,
                                        (uint8_t)at, 0));
            }
            continue;
        }

        const Local *local = find_local(c, name, length);
        if (local != NULL) {
            // 05 の 8.9: as wide as the name is.
            emit_move_wide(c, local->reg, w->value,
                           local->width > 1 ? local->width : 1);
            continue;
        }

        size_t upvalue = find_upvalue(c, name, length);
        if (upvalue == SIZE_MAX) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, w->value, (uint8_t)upvalue,
                                0));
    }

    c->next_register = mark;
}

static void compile_reassign(Compiler *c, const LhatNode *node)
{
    // 13.8's table offers 'a, b := b, a' as what replaces multiple return
    // values, and it exchanges anything only if nothing is stored until every
    // value has been read. So more than one target is compiled in two passes,
    // through a slot each.
    //
    // One target keeps the direct path below, which writes straight into the
    // place it names. There is nothing for it to interleave with, and the
    // common assignment should not pay a move for a promise about a form it
    // is not using.
    //
    // 13.10's destructuring goes the same way whatever the target count: the
    // two-pass path already writes every kind of place a target can be, and
    // reading one value by position is what its read pass does there.
    const LhatNode *values = node->v.binding.values;
    if ((node->v.binding.targets != NULL &&
         node->v.binding.targets->next != NULL) ||
        (values != NULL && values->kind == LHAT_NODE_UNPACK)) {
        compile_reassign_parallel(c, node);
        return;
    }

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
            // 05 の 8.9: 'v.x := n' writes v's own bytes, so the owner has
            // to be the local's registers themselves -- a copy would take
            // the write with it when the scratch is released.
            const Local *hv_owner = NULL;
            {
                const char *owner_name = NULL;
                size_t owner_length = 0;
                if (node_name(c, target->v.access.target, &owner_name,
                              &owner_length)) {
                    const Local *found =
                        find_local(c, owner_name, owner_length);
                    if (found != NULL && found->width > 1) {
                        hv_owner = found;
                    }
                }
            }
            uint8_t mark = c->next_register;
            uint8_t into = hv_owner != NULL
                               ? hv_owner->reg
                               : reserve_for(c, target->v.access.target);
            uint8_t key = reserve(c);
            if (hv_owner == NULL) {
                compile_expression(c, target->v.access.target, into);
            }
            compile_key(c, target, key);

            // 7.4改: owner and key were just evaluated once, above. A
            // compound assignment reads the current value back out through
            // them rather than compiling the target a second time, which
            // would run owner/key again and defeat the point of writing
            // 'target op= value' over 'target := target op value'.
            if (node->v.binding.has_compound_op &&
                value->kind == LHAT_NODE_BINARY) {
                LhatOpcode opcode;
                if (!binary_opcode(value->v.binary.op, &opcode)) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    c->next_register = mark;
                    return;
                }
                uint8_t current = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, current, into, key));
                uint8_t rhs = reserve(c);
                compile_expression(c, value->v.binary.right, rhs);
                uint8_t result = reserve(c);
                emit(c, lhat_encode_abc(opcode, result, current, rhs));
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, result));
            } else {
                // 05 の 8.9: a table never holds a host value; the checker
                // refused this first and this is the backstop.
                if (width_of(value) > 1) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    c->next_register = mark;
                    return;
                }
                uint8_t slot = reserve(c);
                compile_expression(c, value, slot);
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, slot));
            }
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

        // 01 の 8 章: a specifier says which binding is being written, and
        // it has to name the same one reading it would -- so the write goes
        // through the same resolution the read does.
        if (target->kind == LHAT_NODE_SCOPE) {
            uint8_t reg = 0;
            size_t at = 0;
            ScopedKind found =
                resolve_scoped(c, target, name, length, &reg, &at);
            if (found == SCOPED_TOO_FAR) {
                fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                return;
            }
            if (found == SCOPED_NONE) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            if (value != NULL) {
                if (found == SCOPED_REGISTER) {
                    compile_expression(c, value, reg);
                } else {
                    uint8_t mark = c->next_register;
                    uint8_t slot = reserve(c);
                    compile_expression(c, value, slot);
                    emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, slot,
                                            (uint8_t)at, 0));
                    c->next_register = mark;
                }
                value = value->next;
            }
            continue;
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
            // 05 の 8.9: an upvalue cell is one slot, and a host value is
            // never captured -- the checker refused this first.
            if (width_of(value) > 1) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
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
    declare_defs(c, statements);
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

// 03 の 4.3: the top level of a session is not a block that ends. Its names
// keep their slots for the next input, so nothing is rolled back and no
// CLOSE is written -- 5.4's shared places go on being shared, which is what
// lets a closure made in one input see a later one reassign what it captured.
static void compile_session_statements(Compiler *c, const LhatNode *statements)
{
    declare_errors(c, statements);
    // Only this list is the session's top level; the blocks inside it are
    // blocks like any other.
    c->session_top = true;
    declare_names(c, statements);
    c->session_top = false;
    declare_defs(c, statements);

    // 03 の 4.3: an input answers with the value of its last statement, when
    // that statement is an expression. 02 の 8.2 makes a call one and gives
    // an interactive top level the bare ones too, so this is what a prompt
    // has to show -- and doing it here rather than with a return^ keeps a
    // call of a procedure that answers nothing from being refused.
    const LhatNode *last = NULL;
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        last = s;
    }

    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s == last && s->kind == LHAT_NODE_CALL_STMT) {
            uint8_t into = reserve(c);
            compile_expression(c, s->v.jump.value, into);
            emit(c, lhat_encode_abc(LHAT_BC_RETURN, into, 0, 0));
            return;
        }
        compile_statement(c, s);
    }
}

// 5.5: a cleanup is a stretch of code the frame remembers and runs on the way
// out. The body is emitted after the block it belongs to, so the instruction
// naming it is written first and filled in once the body has an address.
static size_t emit_cleanup_push(Compiler *c)
{
    size_t at = lhat_chunk_emit(&c->proto->chunk,
                                lhat_encode_abx(LHAT_BC_PUSHCLEANUP, 0, 0),
                                c->line);
    if (at == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }
    c->cleanup_depth++;
    return at;
}

// Unlike a jump, this names an instruction outright rather than a distance,
// since the frame keeps it and runs it from wherever it happens to be.
static void patch_cleanup_here(Compiler *c, size_t at)
{
    LhatChunk *chunk = &c->proto->chunk;
    if (at == SIZE_MAX || at >= chunk->count) {
        return;
    }
    if (chunk->count > 0xFFFF) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    chunk->code[at] = lhat_encode_abx(LHAT_BC_PUSHCLEANUP, 0,
                                      (uint16_t)chunk->count);
}

static void emit_cleanup_drain(Compiler *c, size_t down_to)
{
    if (c->cleanup_depth > down_to) {
        emit(c, lhat_encode_abc(LHAT_BC_POPCLEANUP, (uint8_t)down_to, 0, 0));
    }
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
    // 8.6改: to^/downto^'s focus is LHAT_NODE_REASSIGN now when it reaches
    // an existing name (no let^ written) rather than always LHAT_NODE_DEFINE
    // -- both share the same v.binding shape, so define_target_name below
    // reads either the same way.
    if (focus == NULL || focus->next != NULL ||
        (focus->kind != LHAT_NODE_DEFINE && focus->kind != LHAT_NODE_REASSIGN)) {
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

// 02 の 10.1: finally^ belongs to blocks in general, not to loops. A block
// that has one remembers it on the way in and runs it on every way out --
// which 10.2 wants and 5.5 says is the frame's job rather than the compiler's.
// The block's statements in the scope that is already open. 01 の 8 章: a
// subroutine's body is written with a '{', but the scope it opens is the one
// its parameters are already in -- the checker puts both in the one Scope
// infer_func pushes, so counting it again here would put '$^' one step
// behind on this side.
static void compile_block_in_scope(Compiler *c, const LhatNode *block)
{
    if (block == NULL) {
        return;
    }
    const LhatNode *cleanup = clause_of(block, LHAT_CLAUSE_FINALLY);
    if (cleanup == NULL) {
        compile_statements(c, block->v.list.items);
        return;
    }

    size_t entry = c->cleanup_depth;
    size_t push = emit_cleanup_push(c);

    compile_statements(c, block->v.list.items);

    emit_cleanup_drain(c, entry);
    c->cleanup_depth = entry;
    size_t over = emit_jump(c, LHAT_BC_JUMP, 0);

    patch_cleanup_here(c, push);
    bool enclosing = c->in_cleanup;
    c->in_cleanup = true;
    compile_statements(c, cleanup);
    c->in_cleanup = enclosing;
    emit(c, lhat_encode_abc(LHAT_BC_ENDCLEANUP, 0, 0, 0));

    lhat_chunk_patch_here(&c->proto->chunk, over);
}

// The same, opening the scope the block's '{' stands for. 01 の 8 章 counts
// it, and its clauses sit inside it -- the checker keeps them in the block's
// own Scope too, so both sides count this the same.
static void compile_block(Compiler *c, const LhatNode *block)
{
    c->scope_depth++;
    compile_block_in_scope(c, block);
    c->scope_depth--;
}

// 02 の 12 章: with^ names a resource and the block's end disposes of it.
// 12.3 gives dispose() the same strength as finally^, so it is the same
// mechanism -- the cleanup body is just a call written by the compiler.
//
// 12.2 wants the disposals in reverse order of definition, which the drain
// gives for nothing: the cleanups are a stack.
//
// 12.4 wants finally^ before dispose(). That falls out too, since the block's
// own finally^ is pushed after these and so is drained first.
static void compile_with(Compiler *c, const LhatNode *node)
{
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;
    size_t entry = c->cleanup_depth;

    size_t pushes[LHAT_MAX_LOCALS];
    uint8_t held[LHAT_MAX_LOCALS];
    size_t count = 0;

    declare_names(c, node->v.list.items);
    for (const LhatNode *binding = node->v.list.items; binding != NULL;
         binding = binding->next) {
        compile_statement(c, binding);

        const char *name = NULL;
        size_t length = 0;
        const Local *local = NULL;
        if (node_name(c, define_target_name(binding->v.binding.targets), &name,
                      &length)) {
            local = find_local(c, name, length);
        }
        if (local == NULL || count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        held[count] = local->reg;
        pushes[count] = emit_cleanup_push(c);
        count++;
    }

    compile_block(c, node->v.list.extra);

    emit_cleanup_drain(c, entry);
    c->cleanup_depth = entry;
    size_t over = emit_jump(c, LHAT_BC_JUMP, 0);

    // One body per binding: read dispose, call it, and hand control back.
    // 12.7 makes dispose() unable to fail and 12.5 makes its absence a matter
    // for the checker, so nothing here has to cope with either.
    for (size_t i = 0; i < count; i++) {
        patch_cleanup_here(c, pushes[i]);
        uint8_t mark = c->next_register;
        uint8_t callee = reserve(c);
        uint8_t key = reserve(c);
        load_string_bytes(c, key, "dispose", 7);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, callee, held[i], key));
        emit(c, lhat_encode_abc(LHAT_BC_CALL, callee, 0, 0));
        c->next_register = mark;
        emit(c, lhat_encode_abc(LHAT_BC_ENDCLEANUP, 0, 0, 0));
    }

    lhat_chunk_patch_here(&c->proto->chunk, over);

    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }
    c->local_count = local_mark;
    c->next_register = register_mark;
}

// The name one element of an in^ focus binds. 16.2's wrapping of a lone
// unnamed focus reads it as a value, which is right for to^ but not here --
// with in^ the focus is what each turn binds. The wrapping is undone.
static const LhatNode *target_of(const LhatNode *element)
{
    if (element->kind == LHAT_NODE_DEFINE &&
        element->v.binding.targets != NULL &&
        element->v.binding.targets->kind == LHAT_NODE_FOCUS) {
        return element->v.binding.values;
    }
    return define_target_name(element);
}

static void declare_targets(Compiler *c, const LhatNode *focus)
{
    for (const LhatNode *element = focus; element != NULL;
         element = element->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, target_of(element), &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        if (c->local_count >= LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        uint8_t slot = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
        Local *local = &c->locals[c->local_count++];
        local->name = name;
        local->length = length;
        local->reg = slot;
        local->depth = c->scope_depth;
        local->width = 1;
    }
}

// 13.10: one name takes the value whole, and several take it apart by
// position. `in^` is the marker that says which, so no unpack^ is written
// (16.3). 13.8 makes what an iterator yields a table when it is a group.
static void bind_targets(Compiler *c, const LhatNode *focus, size_t local_mark,
                         size_t count, uint8_t from)
{
    if (count == 1) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, c->locals[local_mark].reg, from,
                                0));
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t mark = c->next_register;
        uint8_t key = reserve(c);
        load_constant(c, key, lhat_integer((int64_t)i + 1));
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX,
                                c->locals[local_mark + i].reg, from, key));
        c->next_register = mark;
    }
    (void)focus;
}

// The two forms of for^ that 16.1 says do not repeat: the if^ clause of 16.3
// and the pattern match of 17 章. Both introduce the focus, use it once, and
// let it go -- the do^ block 16.3 writes them out as.
//
// 17.9 makes a match sugar over an if-chain, and the parser has already
// written that chain, so there is nothing here to tell the two apart. Only
// the subject's binding is left, which is 17.2's "evaluated once and named".
static void compile_for_once(Compiler *c, const LhatNode *node, uint8_t into,
                             bool as_expression)
{
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    declare_names(c, node->v.loop.focus);
    compile_in_scope(c, node->v.loop.focus);
    if (as_expression) {
        compile_expression(c, node->v.loop.body, into);
    } else {
        compile_statement(c, node->v.loop.body);
    }

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
    const LhatNode *prolog = clause_of(body, LHAT_CLAUSE_PROLOG);
    const LhatNode *pre = clause_of(body, LHAT_CLAUSE_PRE);
    const LhatNode *first = clause_of(body, LHAT_CLAUSE_FIRST);
    const LhatNode *last = clause_of(body, LHAT_CLAUSE_LAST);
    const LhatNode *epilog = clause_of(body, LHAT_CLAUSE_EPILOG);
    const LhatNode *cleanup = clause_of(body, LHAT_CLAUSE_FINALLY);

    // 16.7 and 9.4: the focus and the names of prolog^ and first^ last for
    // the whole loop, so they are made in a scope outside it.
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;

    // 16.3: with in^ the focus is what each turn binds, not a value to
    // evaluate. Everything else evaluates its focus here and once.
    if (kind == LHAT_FOR_IN) {
        declare_targets(c, focus);
    } else {
        declare_names(c, focus);
        compile_in_scope(c, focus);
    }
    size_t focus_locals = c->local_count - local_mark;

    // 16.3: `in^ e` asks e for the coroutine to walk -- e.iterate(). A table
    // and a coroutine answer it themselves; anything else answers by having
    // a member of that name, which 11.3's structural judgement already knows
    // how to ask for.
    uint8_t walk = 0;
    uint8_t taken = 0;
    if (kind == LHAT_FOR_IN) {
        walk = reserve(c);
        uint8_t receiver = reserve(c);
        uint8_t key = reserve(c);
        compile_expression(c, bound, receiver);
        load_string_bytes(c, key, "iterate^", 8);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, walk, receiver, key));
        emit(c, lhat_encode_abc(LHAT_BC_CALLMETHOD, walk, 0, 0));
        c->next_register = (uint8_t)(walk + 1);
        taken = reserve(c);
    }

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

    // 9.2 puts finally^ last of all, and 10.2 makes it run on every way out.
    // Remembering it here covers the ways out that 9.8 keeps apart from a
    // normal end: a return^ or a try^ from inside the body.
    size_t cleanup_entry = c->cleanup_depth;
    size_t cleanup_push = SIZE_MAX;
    if (cleanup != NULL) {
        cleanup_push = emit_cleanup_push(c);
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
    context.cleanup_depth = c->cleanup_depth;
    c->loop = &context;

    size_t top = c->proto->chunk.count;
    size_t leaving = SIZE_MAX;

    // 9.10: pre^ runs at the head of every turn, before the condition is
    // tested -- so it runs at least once however the condition comes out,
    // which is the shape C spells do ... while. It does not mark the loop
    // entered: 9.1 keeps first^ and last^ for turns the condition accepted.
    compile_in_scope(c, pre);

    if (kind == LHAT_FOR_TO || kind == LHAT_FOR_DOWNTO) {
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        compile_numeric_test(c, node, numeric, numeric_bound, test);
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
    } else if (kind == LHAT_FOR_IN) {
        // 13.9: what a resume answers is the union of what the coroutine
        // yields and what it returns, so "is it finished" is the question
        // that tells the two apart.
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, taken, 0, 0));
        emit(c, lhat_encode_abc(LHAT_BC_RESUME, taken, walk, 0));
        emit(c, lhat_encode_abc(LHAT_BC_ISDONE, test, walk, 0));
        emit(c, lhat_encode_abc(LHAT_BC_NOT, test, test, 0));
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
        bind_targets(c, focus, local_mark, focus_locals, taken);
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
    // 01 の 8 章 counts that scope: the body's '{' is one for '$^' to walk.
    c->scope_depth++;
    compile_statements(c, body != NULL ? body->v.list.items : NULL);
    c->scope_depth--;

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

    // 9.2 and 10.9: epilog^ is the last of the loop's own clauses, and
    // finally^ comes after it whichever way the loop ended.
    if (cleanup != NULL) {
        emit_cleanup_drain(c, cleanup_entry);
        c->cleanup_depth = cleanup_entry;
        size_t over = emit_jump(c, LHAT_BC_JUMP, 0);

        patch_cleanup_here(c, cleanup_push);
        bool enclosing = c->in_cleanup;
        c->in_cleanup = true;
        compile_statements(c, cleanup);
        c->in_cleanup = enclosing;
        emit(c, lhat_encode_abc(LHAT_BC_ENDCLEANUP, 0, 0, 0));

        lhat_chunk_patch_here(&c->proto->chunk, over);
    }

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
    c->line = node->line;

    switch (node->kind) {
        case LHAT_NODE_DEFINE:
            compile_define(c, node);
            return;

        case LHAT_NODE_REASSIGN:
            compile_reassign(c, node);
            return;

        case LHAT_NODE_BLOCK:
            compile_block(c, node);
            return;

        case LHAT_NODE_WITH:
            compile_with(c, node);
            return;

        case LHAT_NODE_RETURN: {
            // 02 の 10.5: a finally^ cannot replace the answer, so it cannot
            // return one. Java allows it and it is a known trap; C# refuses.
            if (c->in_cleanup) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            // The frame drains what it has pending on the way out (5.5), so
            // nothing has to be emitted here for the blocks being left.
            if (node->v.jump.value == NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));
                return;
            }
            uint8_t mark = c->next_register;
            // 05 の 8.9: a returned host value needs its whole width here;
            // the machine reads that width off the head when the frame pops.
            uint8_t slot = reserve_for(c, node->v.jump.value);
            compile_expression(c, node->v.jump.value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_RETURN, slot, 0, 0));
            c->next_register = mark;
            return;
        }

        // 04 の 11.6改: unlike return^, this does not answer anything a
        // finally^ could be seen as replacing, so 02 の 10.5's restriction
        // does not apply here.
        case LHAT_NODE_PANIC: {
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->v.jump.value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_PANIC, slot, 0, 0));
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

        // 05 の 8.7: the same shape, with the path written rather than read
        // off the unit -- so 8.8's own walk does the binding.
        case LHAT_NODE_IMPORT_STMT: {
            const LhatNode *path = node->v.jump.value;
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node, slot);
            if (path != NULL && path->kind == LHAT_NODE_MEMBER) {
                uint8_t owner = reserve(c);
                uint8_t key = reserve(c);
                compile_path_prefix(c, path->v.access.target, owner);
                compile_key(c, path, key);
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, slot));
            } else {
                const char *name = NULL;
                size_t length = 0;
                const Local *local =
                    node_name(c, path, &name, &length)
                        ? find_local(c, name, length) : NULL;
                if (local == NULL) {
                    fail(c, LHAT_COMPILE_UNDEFINED);
                    return;
                }
                emit(c, lhat_encode_abc(LHAT_BC_MOVE, local->reg, slot, 0));
            }
            c->next_register = mark;
            return;
        }

        // 05 の 5.4改: bring the unit in, then put it where the path it
        // declared says. 8.8 makes the tables on the way, here as there.
        case LHAT_NODE_REQUIRE_STMT: {
            const char *module_name = required_module_name(c, node);
            if (module_name == NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node, slot);
            compile_bind_path(c, module_name, slot);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_CALL_STMT:
        case LHAT_NODE_YIELD_ALL:
        case LHAT_NODE_YIELD: {
            // 02 の 8.2: a call may stand alone, and its value is discarded.
            // A yield^ written for its effect alone is the same shape.
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node->kind == LHAT_NODE_CALL_STMT
                                      ? node->v.jump.value
                                      : node,
                               slot);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_FOR:
            // 16.1: for^ introduces a value; whether it repeats is up to the
            // clause after it, and if^ is the clause that does not.
            if (node->v.loop.kind == LHAT_FOR_IF ||
                node->v.loop.kind == LHAT_FOR_WHEN) {
                compile_for_once(c, node, 0, false);
            } else {
                compile_loop(c, node);
            }
            return;

        case LHAT_NODE_REPEAT:
            compile_loop(c, node);
            return;

        case LHAT_NODE_BREAK: {
            // 9.8's label form, or a level written two ways at once. Nothing
            // labels a loop yet, so neither has an answer here -- and unlike
            // the count below, these really are waiting on the compiler.
            if (node->v.jump.value != NULL || node->v.jump.level == 0) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            // 9.8: the loop being left is the one the level names, counting
            // the innermost as 1. Asking for more loops than stand here is
            // written down wrong rather than clamped to the outermost, and
            // a break^ outside every loop is the same mistake with nothing
            // to count from.
            LoopContext *target = c->loop;
            for (uint32_t out = 1; out < node->v.jump.level; out++) {
                if (target == NULL) {
                    break;
                }
                target = target->enclosing;
            }
            if (target == NULL) {
                fail(c, LHAT_COMPILE_BREAK_TOO_FAR);
                return;
            }
            if (target->count >= LHAT_MAX_BREAKS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);  // 5.2's limits, not 9.8's
                return;
            }
            // 9.8: break^ is a normal end for the loop it names, so the jump
            // lands where that loop's own last^ and epilog^ run. The loops
            // it passes through are left rather than ended -- their clauses
            // sit in the code being jumped over, and what has to run anyway
            // is their finally^, which draining the cleanups down to the
            // target's depth is exactly.
            emit_cleanup_drain(c, target->cleanup_depth);
            target->jumps[target->count++] = emit_jump(c, LHAT_BC_JUMP, 0);
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

// 03 の 4.3: the top-level names of the inputs already compiled, with the
// slots they were given. The names are copies -- the lexer each one was read
// from goes when that input does, and a Local points into source text.
struct LhatCompileSession {
    struct {
        char *name;
        size_t length;
        uint8_t reg;
    } names[LHAT_MAX_LOCALS];
    size_t count;
    uint8_t next_register;

    // 14.2 and 04 の 2.4: what a def^ composes onto and what an errordef^
    // declared are worked out while compiling and never reach the machine, so
    // an input that needs them needs what an earlier one found.
    //
    // Both point into the input that made them -- a DefChain at its syntax
    // tree, an ErrorDecl at the kind objects on its proto -- so a session's
    // inputs have to outlive it. The tree was already needed for that reason;
    // this only makes the requirement reach further back.
    struct DefDecl *defs;
    size_t def_count;
    size_t def_capacity;

    struct ErrorDecl *errors;
    size_t error_count;
    size_t error_capacity;

    // 05 の 8.2: the names the host bound to members of L^. A file gets these
    // through LhatUnits; a prompt has no program to carry them.
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // 04 の 12.4 and 05 の 8.8: what a host's lhat_register_error_kind and
    // lhat_register_hostdata_type registered, so that isa^ against either
    // compiles at a prompt as it does in a file. The other half of LhatUnits
    // a session carries; NULL/0 when the host registered none.
    const LhatHostErrorKind *host_errors;
    size_t host_error_count;
    const LhatHostTypeEntry *host_types;
    size_t host_type_count;
};

void lhat_compile_session_bind(LhatCompileSession *session,
                               const char *const *names,
                               const char *const *members, size_t count)
{
    if (session == NULL) {
        return;
    }
    session->initial_names = names;
    session->initial_members = members;
    session->initial_count = count;
}

void lhat_compile_session_hosted(LhatCompileSession *session,
                                 const LhatHostErrorKind *errors,
                                 size_t error_count,
                                 const LhatHostTypeEntry *types,
                                 size_t type_count)
{
    if (session == NULL) {
        return;
    }
    session->host_errors = errors;
    session->host_error_count = error_count;
    session->host_types = types;
    session->host_type_count = type_count;
}

LhatCompileSession *lhat_compile_session_new(void)
{
    return (LhatCompileSession *)lhat_calloc(1, sizeof(LhatCompileSession));
}

void lhat_compile_session_dispose(LhatCompileSession *session)
{
    if (session == NULL) {
        return;
    }
    for (size_t i = 0; i < session->count; i++) {
        lhat_free(session->names[i].name);
    }
    // The kind objects belong to the protos; only the lists of them are ours.
    for (size_t i = 0; i < session->error_count; i++) {
        lhat_free((void *)session->errors[i].kinds);
    }
    lhat_free(session->errors);
    lhat_free(session->defs);
    lhat_free(session);
}

// 05 の 8.7: reads a written path off L^.modules, from the root outwards.
// `into` already holds the table the first segment is looked up in.
static void compile_import_path(Compiler *c, const LhatNode *path, uint8_t into,
                                uint8_t key)
{
    if (path == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }
    if (path->kind == LHAT_NODE_MEMBER) {
        compile_import_path(c, path->v.access.target, into, key);
        compile_key(c, path, key);
    } else {
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, path, &name, &length)) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        load_string_bytes(c, key, name, length);
    }
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
}

// 05 の 5.4改: the path a require^ standing alone brings a unit in under, or
// NULL when there is no such unit or it declared none.
static const char *required_module_name(Compiler *c, const LhatNode *node)
{
    const LhatNode *path = node->v.jump.value;
    if (c->units == NULL || c->units->resolve == NULL || path == NULL) {
        return NULL;
    }
    const char *module_name = NULL;
    size_t which = c->units->resolve(
        c->units->context, c->lexer->strings + path->v.string.offset,
        path->v.string.length, &module_name);
    return which == LHAT_NO_UNIT ? NULL : module_name;
}

// Puts `value` where a dotted path says, making the tables on the way. 02 の
// 8.8 is the written form of this; here the path is a string rather than a
// tree, since it came from what the required unit declared.
static void compile_bind_path(Compiler *c, const char *path, uint8_t value)
{
    size_t length = strcspn(path, ".");
    const Local *root = find_local(c, path, length);
    if (root == NULL) {
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    // One segment names the place itself, so there is nothing to reach into.
    if (path[length] == '\0') {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, root->reg, value, 0));
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t owner = reserve(c);
    uint8_t key = reserve(c);
    ensure_table(c, root->reg);
    emit(c, lhat_encode_abc(LHAT_BC_MOVE, owner, root->reg, 0));

    const char *segment = path + length + 1;
    length = strcspn(segment, ".");
    while (segment[length] == '.') {
        uint8_t next = reserve(c);
        load_string_bytes(c, key, segment, length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, next, owner, key));
        ensure_table_at(c, next, owner, key);
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, owner, next, 0));
        c->next_register = next;
        segment += length + 1;
        length = strcspn(segment, ".");
    }

    load_string_bytes(c, key, segment, length);
    emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, value));
    c->next_register = mark;
}

// 05 の 5.3: a unit answers what an earlier require^ of it registered, and
// runs its body only when there is nothing there. 04 の 11.3 spells "not
// there" nil^, so a walk of the path with a nil test at each step is the
// whole of it -- the guard the desugaring in 8.6 writes as an if^.
static void compile_module_guard(Compiler *c, const char *path)
{
    uint8_t mark = c->next_register;
    uint8_t into = reserve(c);
    uint8_t test = reserve(c);
    uint8_t key = reserve(c);

    emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
    load_string_bytes(c, key, "modules", 7);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));

    // Every miss lands on the body, which is emitted after this.
    size_t misses[LHAT_MAX_LOCALS];
    size_t miss_count = 0;

    for (const char *segment = path;; ) {
        size_t length = strcspn(segment, ".");
        // Reading through what is not a table would fault (5.1), so each
        // step is guarded rather than only the last.
        emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, into, 0));
        emit(c, lhat_encode_abc(LHAT_BC_NOT, test, test, 0));
        if (miss_count < LHAT_MAX_LOCALS) {
            misses[miss_count++] = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        }
        load_string_bytes(c, key, segment, length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
        if (segment[length] != '.') {
            break;
        }
        segment += length + 1;
    }

    // Something there is what an earlier require^ registered, and is the
    // answer without the body running again.
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, into, 0));
    emit(c, lhat_encode_abc(LHAT_BC_NOT, test, test, 0));
    if (miss_count < LHAT_MAX_LOCALS) {
        misses[miss_count++] = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    }
    emit(c, lhat_encode_abc(LHAT_BC_RETURN, into, 0, 0));

    for (size_t i = 0; i < miss_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, misses[i]);
    }
    c->next_register = mark;
}

// 05 の 4 章: what the unit publishes. check.c's collect_exports reads the
// same public^ marks off the same tree to build the type, so the two are one
// rule read twice rather than two lists to keep level.
static void compile_exports(Compiler *c, const LhatNode *statements,
                            uint8_t into)
{
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, into, 0, 0));

    uint8_t mark = c->next_register;
    uint8_t key = reserve(c);
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        const LhatNode *named = NULL;
        if (s->kind == LHAT_NODE_DEFINE && s->v.binding.exported) {
            named = s->v.binding.targets;
        } else if (s->kind == LHAT_NODE_ERRORDEF && s->v.named.exported) {
            named = s->v.named.name;
        } else {
            continue;
        }
        for (; named != NULL; named = named->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, define_target_name(named), &name, &length)) {
                continue;
            }
            const Local *local = find_local(c, name, length);
            if (local == NULL) {
                continue;
            }
            load_string_bytes(c, key, name, length);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, local->reg));
        }
    }
    c->next_register = mark;
}

// The other half of the guard: what the body worked out goes into the
// registry under the declared path, and is the unit's answer. 02 の 8.8's
// rule for the tables on the way holds here too, which is why this reads
// like the code that form compiles to.
static void compile_module_register(Compiler *c, const LhatNode *statements,
                                    const char *path)
{
    uint8_t mark = c->next_register;
    uint8_t exports = reserve(c);
    uint8_t owner = reserve(c);
    uint8_t key = reserve(c);

    compile_exports(c, statements, exports);
    // 05 の 8.6改 (M5): everything it holds is in, so nothing else writes to
    // it. Before the registry gets it, since what goes into the registry is
    // the same object every requirer is handed.
    emit(c, lhat_encode_abc(LHAT_BC_SEAL, exports, 0, 0));

    emit(c, lhat_encode_abc(LHAT_BC_ENV, owner, 0, 0));
    load_string_bytes(c, key, "modules", 7);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, owner, owner, key));

    const char *segment = path;
    size_t length = strcspn(segment, ".");
    while (segment[length] == '.') {
        uint8_t next = reserve(c);
        load_string_bytes(c, key, segment, length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, next, owner, key));
        ensure_table_at(c, next, owner, key);
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, owner, next, 0));
        c->next_register = next;
        segment += length + 1;
        length = strcspn(segment, ".");
    }

    load_string_bytes(c, key, segment, length);
    emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, exports));
    emit(c, lhat_encode_abc(LHAT_BC_RETURN, exports, 0, 0));
    c->next_register = mark;
}

static LhatCompileStatus compile_unit(LhatCompileSession *session,
                                      const LhatNode *unit,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out)
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

    // 03 の 4.3: what earlier inputs left is already in scope and already in
    // registers, so this one names it where it stands and numbers its own
    // from above.
    if (session != NULL) {
        for (size_t i = 0; i < session->count; i++) {
            c.locals[c.local_count].name = session->names[i].name;
            c.locals[c.local_count].length = session->names[i].length;
            c.locals[c.local_count].reg = session->names[i].reg;
            // 03 の 4.3: the session's top level is this input's top level,
            // which 01 の 8 章 makes what '$' names.
            c.locals[c.local_count].depth = 0;
            c.local_count++;
        }
        c.session_locals = c.local_count;
        c.next_register = session->next_register;
        proto->reserved = session->next_register;
        if (proto->chunk.registers < session->next_register) {
            proto->chunk.registers = session->next_register;
        }

        // 14.2 and 04 の 2.4: taken over rather than copied. What the entries
        // point at belongs to the input that made it, which the session keeps
        // -- so the session hands the arrays across and this input grows them.
        c.defs = session->defs;
        c.def_count = session->def_count;
        c.def_capacity = session->def_capacity;
        c.errors = session->errors;
        c.error_count = session->error_count;
        c.error_capacity = session->error_capacity;
        session->defs = NULL;
        session->def_count = 0;
        session->def_capacity = 0;
        session->errors = NULL;
        session->error_count = 0;
        session->error_capacity = 0;
    }

    c.units = units;
    const char *module_name = units != NULL ? units->module_name : NULL;

    // 05 の 5.3: what an earlier require^ registered is the answer, and the
    // body below runs only when there is none.
    if (module_name != NULL) {
        compile_module_guard(&c, module_name);
    }

    // The unit is a scope like any other, so 8.7 applies to it too -- unless
    // it is one input of a session, where the top level outlives the input.
    if (session != NULL) {
        compile_session_statements(&c, unit->v.list.items);
    } else if (module_name != NULL) {
        // The scope ends with the unit, so nothing is handed back to the
        // pool -- 05 の 4 章 reads the exports off the slots the body left.
        declare_errors(&c, unit->v.list.items);
        declare_names(&c, unit->v.list.items);
        declare_defs(&c, unit->v.list.items);
        for (const LhatNode *s = unit->v.list.items; s != NULL; s = s->next) {
            compile_statement(&c, s);
        }
        compile_module_register(&c, unit->v.list.items, module_name);
    } else {
        compile_statements(&c, unit->v.list.items);
    }
    emit(&c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));

    // A session hands the registries back so the next input has them; without
    // one they were the compiler's, and the kind objects they point at belong
    // to the chunk and stay either way.
    if (session != NULL && status == LHAT_COMPILE_OK) {
        session->defs = c.defs;
        session->def_count = c.def_count;
        session->def_capacity = c.def_capacity;
        session->errors = c.errors;
        session->error_count = c.error_count;
        session->error_capacity = c.error_capacity;
    } else {
        for (size_t i = 0; i < c.error_count; i++) {
            lhat_free((void *)c.errors[i].kinds);
        }
        lhat_free(c.errors);
        lhat_free(c.defs);
    }

    if (status != LHAT_COMPILE_OK) {
        lhat_proto_free(proto);
        return status;
    }

    // What this input declared joins the session, copied out of the source it
    // was read from. 03 の 4.3: a name written again shadows rather than
    // redefines, so the newer slot takes the name and the older one keeps
    // whatever it holds -- a closure that captured it goes on reading it.
    if (session != NULL) {
        for (size_t i = session->count; i < c.local_count; i++) {
            size_t at = session->count;
            for (size_t seen = 0; seen < session->count; seen++) {
                if (session->names[seen].length == c.locals[i].length &&
                    memcmp(session->names[seen].name, c.locals[i].name,
                           c.locals[i].length) == 0) {
                    at = seen;
                    break;
                }
            }
            if (at == session->count) {
                if (session->count >= LHAT_MAX_LOCALS) {
                    break;
                }
                char *kept = (char *)lhat_alloc(c.locals[i].length + 1);
                if (kept == NULL) {
                    lhat_proto_free(proto);
                    return LHAT_COMPILE_TOO_COMPLEX;
                }
                memcpy(kept, c.locals[i].name, c.locals[i].length);
                kept[c.locals[i].length] = '\0';
                session->names[at].name = kept;
                session->names[at].length = c.locals[i].length;
                session->count++;
            }
            session->names[at].reg = c.locals[i].reg;
        }
        session->next_register = c.next_register > session->next_register
                                     ? c.next_register
                                     : session->next_register;
        // What the next input will find already filled is exactly what this
        // one has to leave sharable when its frame goes.
        proto->kept = session->next_register;
    }

    *out = proto;
    return status;
}

LhatCompileStatus lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out)
{
    return compile_unit(NULL, unit, lexer, NULL, out);
}

LhatCompileStatus lhat_compile_module(const LhatNode *unit,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out)
{
    return compile_unit(NULL, unit, lexer, units, out);
}

LhatCompileStatus lhat_compile_next(LhatCompileSession *session,
                                    const LhatNode *unit,
                                    const LhatLexer *lexer, LhatProto **out)
{
    // 05 の 8.2: the session carries the host's bindings where a file has a
    // program to, and 8.8/04 の 12.4's registries alongside them. What is left
    // of LhatUnits does not apply -- 5.3 gives a require^ at a prompt nowhere
    // to go, which a NULL resolver already says, and import^ needs nothing
    // here at all (compile_import_path reads L^.modules at run time).
    LhatUnits units;
    memset(&units, 0, sizeof units);
    units.initial_names = session->initial_names;
    units.initial_members = session->initial_members;
    units.initial_count = session->initial_count;
    units.host_errors = session->host_errors;
    units.host_error_count = session->host_error_count;
    units.host_types = session->host_types;
    units.host_type_count = session->host_type_count;
    return compile_unit(session, unit, lexer, &units, out);
}

const char *lhat_compile_status_message(LhatCompileStatus status)
{
    switch (status) {
        case LHAT_COMPILE_OK:          return "compiled";
        case LHAT_COMPILE_UNSUPPORTED: return "this form does not compile yet";
        case LHAT_COMPILE_TOO_COMPLEX: return "too many registers or constants";
        case LHAT_COMPILE_UNDEFINED:   return "no such name";
        case LHAT_COMPILE_BREAK_TOO_FAR:
            return "this break^ leaves more loops than there are around it";
        case LHAT_COMPILE_SCOPE_TOO_FAR:
            return "this reaches out past more scopes than are open here";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

// 02 の 11.8: an operator is a member whose name is the operator itself, and
// this is the spelling call_operator looks a candidate up by. Also reused by
// finish() to name a panicking instruction (04 の 11.6) for a host, which is
// where LHAT_BC_ASCAST answers too even though 11.6's as^ is not one of
// 11.8's overloadable operators and never reaches call_operator itself.
// NULL for every other instruction.
static const char *operator_name(LhatOpcode op, size_t *length)
{
    switch (op) {
        case LHAT_BC_SPACESHIP: *length = 3; return "<=>";  // 02 の 11.9
        case LHAT_BC_CONCAT: *length = 2; return "..";
        case LHAT_BC_ADD:    *length = 1; return "+";
        case LHAT_BC_SUB:    *length = 1; return "-";
        case LHAT_BC_MUL:    *length = 1; return "*";
        case LHAT_BC_DIV:    *length = 1; return "/";
        case LHAT_BC_IDIV:   *length = 2; return "//";
        case LHAT_BC_MOD:    *length = 1; return "%";
        case LHAT_BC_POW:    *length = 2; return "**";
        case LHAT_BC_ASCAST: *length = 3; return "as^";
        default:             *length = 0; return NULL;
    }
}

// 02 の 14.8改: the three that can leave the integers. Answers false when the
// result does not fit, and then the caller redoes it in reals.
//
// Computed through the unsigned type: signed overflow is undefined in C11, so
// the wrapped value a check would have to look at cannot be obtained by
// letting it wrap. Unsigned wraps by definition, and converting back is
// implementation-defined rather than undefined -- every target this runs on
// is two's complement, which C23 now requires outright.
static bool add_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x + (uint64_t)y;
    int64_t sum = (int64_t)wrapped;
    // Overflow is exactly the case where both operands share a sign and the
    // result does not.
    if (((x ^ sum) & (y ^ sum)) < 0) {
        return false;
    }
    *out = sum;
    return true;
}

static bool subtract_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x - (uint64_t)y;
    int64_t difference = (int64_t)wrapped;
    if (((x ^ y) & (x ^ difference)) < 0) {
        return false;
    }
    *out = difference;
    return true;
}

static bool multiply_exact(int64_t x, int64_t y, int64_t *out)
{
    if (x == 0 || y == 0) {
        *out = 0;
        return true;
    }
    uint64_t wrapped = (uint64_t)x * (uint64_t)y;
    int64_t product = (int64_t)wrapped;
    // Dividing back is the check that needs no wider type. The extra guard is
    // INT64_MIN / -1, which is the one division that overflows.
    if (x == -1 && y == INT64_MIN) {
        return false;
    }
    if (product / x != y) {
        return false;
    }
    *out = product;
    return true;
}

// 5.1: the generic form checks. 02 の 14.8 makes number^ one type with two
// representations, so an operation stays in integers when both sides are and
// widens when one of them is real -- or when the integers no longer hold the
// answer (14.8改).
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
        case LHAT_BC_ADD: {
            int64_t whole = 0;
            *out = exact && add_exact(lhat_as_integer(left),
                                      lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a + b);
            return true;
        }
        case LHAT_BC_SUB: {
            int64_t whole = 0;
            *out = exact && subtract_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a - b);
            return true;
        }
        case LHAT_BC_MUL: {
            int64_t whole = 0;
            *out = exact && multiply_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a * b);
            return true;
        }
        case LHAT_BC_DIV:
            // 04 の 11.2: real division, so a zero divisor gives inf rather
            // than failing. That is what keeps ordinary arithmetic out of the
            // unions.
            *out = lhat_real(a / b);
            return true;
        case LHAT_BC_IDIV:
        case LHAT_BC_MOD: {
            // 04 の 11.2改: a zero divisor no longer fails -- like an
            // overflow (14.8改), it widens to real arithmetic instead,
            // which already answers inf/nan for this the same way '/'
            // does, so ordinary arithmetic stays out of a union either way.
            if (exact && b != 0) {
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
            // floor() rather than a hand-rolled truncate-and-adjust: the
            // quotient can be inf or nan here (b == 0), and casting either
            // to int64_t, which the hand-rolled form used to do, is
            // undefined.
            double floored = floor(a / b);
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

// 02 の 11.9 (S40): which of the two comes first, for the types that order
// their own. Negative, zero or positive -- the shape every '<=>' answers in,
// and what the four orderings read against zero. False when neither number^
// nor string^ is what is being asked about, which sends the question to the
// member 11.8 names.
static bool three_way(LhatValue left, LhatValue right, int *out)
{
    if (lhat_is_number(left) && lhat_is_number(right)) {
        // 14.8: one type over integers and reals, so the comparison is the
        // one lhat_value_equal already makes across the two representations.
        if (lhat_value_equal(left, right)) {
            *out = 0;
            return true;
        }
        *out = lhat_number_as_real(left) < lhat_number_as_real(right) ? -1 : 1;
        return true;
    }
    if (lhat_is_object_kind(left, LHAT_OBJECT_STRING) &&
        lhat_is_object_kind(right, LHAT_OBJECT_STRING)) {
        // The bytes, in order. 01 の 1 章 fixes the encoding at UTF-8, which
        // orders code points the same way their bytes order.
        const LhatString *a = (const LhatString *)lhat_as_object(left);
        const LhatString *b = (const LhatString *)lhat_as_object(right);
        size_t shorter = a->length < b->length ? a->length : b->length;
        int by_bytes = shorter > 0 ? memcmp(a->text, b->text, shorter) : 0;
        if (by_bytes != 0) {
            *out = by_bytes < 0 ? -1 : 1;
        } else if (a->length == b->length) {
            *out = 0;
        } else {
            *out = a->length < b->length ? -1 : 1;
        }
        return true;
    }
    return false;
}

// 11.9 (S40): every ordering is read off a three-way answer, whether the
// answer came from a type that orders its own or from a written op^<=>. False
// when neither side is one of those, which is what sends the question on.
static bool ordering(LhatOpcode op, LhatValue left, LhatValue right,
                     bool *out, LhatRunStatus *status)
{
    int outcome = 0;
    if (!three_way(left, right, &outcome)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }
    switch (op) {
        case LHAT_BC_LT: *out = outcome <  0; return true;
        case LHAT_BC_LE: *out = outcome <= 0; return true;
        case LHAT_BC_GT: *out = outcome >  0; return true;
        case LHAT_BC_GE: *out = outcome >= 0; return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

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

// ---------------------------------------------------------------------------
// 05 の 8.9: host values at run time
// ---------------------------------------------------------------------------

// The members table the machine built for a host value type at install, or
// NULL when the registration never reached this machine.
static LhatTable *hostvalue_members_of(Machine *m, const LhatHostValueTag *tag)
{
    return tag != NULL && tag->index < m->hostvalue_member_count
               ? m->hostvalue_members[tag->index]
               : NULL;
}

// Value equality is byte equality under the same tag -- a host value has no
// identity to fall back on, which is half of what makes it a value. Every
// making zeroes the data run's unused tail, so whole slots compare exactly.
static bool hostvalue_equal(LhatSlots slots, size_t left, size_t right)
{
    const LhatHostValueTag *tag = slots.values[left].hostvalue;
    if (tag == NULL || slots.tags[right] != LHAT_VALUE_HOSTVALUE ||
        slots.values[right].hostvalue != tag) {
        return false;
    }
    return memcmp(slots.values + left + 1, slots.values + right + 1,
                  (tag->width - 1) * sizeof(LhatValueUnion)) == 0;
}

// A host answered with a head-shaped run -- an argument passed through, or
// the scratch lhat_make_hostvalue filled -- written out whole into the slots
// at `at`. False when the run is not one.
static bool place_hostvalue_answer(Machine *m, size_t at, LhatValue answered)
{
    const LhatValueUnion *run = answered.as.hostvalue_run;
    const LhatHostValueTag *tag = run != NULL ? run[0].hostvalue : NULL;
    if (tag == NULL || at + tag->width > LHAT_STACK_SLOTS) {
        return false;
    }
    // Ascending, which survives the one overlapping case there is: a host
    // echoing an argument back, whose run sits above the answer slot.
    m->slots.values[at] = run[0];
    m->slots.tags[at] = (uint8_t)LHAT_VALUE_HOSTVALUE;
    for (size_t i = 1; i < tag->width; i++) {
        m->slots.values[at + i] = run[i];
        m->slots.tags[at + i] = (uint8_t)LHAT_VALUE_CONT;
    }
    return true;
}

// The reverse: the argument whose head sits at `slot`, gathered for a host
// as a value aiming back into the stack. Stable for the call's duration --
// the stack is a fixed array of the machine's.
static LhatValue hostvalue_argument(LhatSlots slots, size_t slot)
{
    LhatValue v;
    v.tag = LHAT_VALUE_HOSTVALUE;
    v.as.hostvalue_run = slots.values + slot;
    return v;
}

// The registered field a string key names, or NULL.
static const LhatHostValueField *hostvalue_field_named(
    const LhatHostValueTag *tag, LhatValue key)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return NULL;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    for (size_t i = 0; i < tag->field_count; i++) {
        if (strlen(tag->fields[i].name) == name->length &&
            memcmp(tag->fields[i].name, name->text, name->length) == 0) {
            return &tag->fields[i];
        }
    }
    return NULL;
}

// 'v.x' as a number^, off the data run that follows a head. Offsets were
// checked against the registered size when the field was.
static LhatValue hostvalue_field_get(const LhatValueUnion *data,
                                     const LhatHostValueField *field)
{
    const uint8_t *bytes = (const uint8_t *)data + field->offset;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f;
            memcpy(&f, bytes, sizeof f);
            return lhat_real((double)f);
        }
        case LHAT_HVFIELD_F64: {
            double d;
            memcpy(&d, bytes, sizeof d);
            return lhat_real(d);
        }
        case LHAT_HVFIELD_I8: {
            int8_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I16: {
            int16_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I32: {
            int32_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I64: {
            int64_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_U8: {
            uint8_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
    }
    return lhat_nil();
}

// And the write. False when the value is not a number^ -- 14.8's one type,
// either representation.
static bool hostvalue_field_set(LhatValueUnion *data,
                                const LhatHostValueField *field,
                                LhatValue value)
{
    if (!lhat_is_number(value)) {
        return false;
    }
    uint8_t *bytes = (uint8_t *)data + field->offset;
    double real = lhat_number_as_real(value);
    int64_t integer = lhat_is_integer(value) ? lhat_as_integer(value)
                                             : (int64_t)real;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f = (float)real;
            memcpy(bytes, &f, sizeof f);
            return true;
        }
        case LHAT_HVFIELD_F64:
            memcpy(bytes, &real, sizeof real);
            return true;
        case LHAT_HVFIELD_I8: {
            int8_t i = (int8_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I16: {
            int16_t i = (int16_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I32: {
            int32_t i = (int32_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I64:
            memcpy(bytes, &integer, sizeof integer);
            return true;
        case LHAT_HVFIELD_U8: {
            uint8_t u = (uint8_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u = (uint16_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u = (uint32_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
    }
    return false;
}

// 13.7: the i-th effective argument of a call, whether it sits in an
// ordinary register or is a position of the table a spread ('expr...')
// unpacked -- the two are not contiguous in memory, so nothing beyond this
// reads registers directly once a spread is in play.
static LhatValue call_arg(LhatSlots regs, size_t rbase, uint8_t a, size_t skip,
                          const LhatTable *spread, size_t before_spread,
                          size_t i)
{
    if (spread != NULL && i >= before_spread) {
        return lhat_slots_get(spread->array, i - before_spread);
    }
    return lhat_slots_get(regs, rbase + a + skip + i);
}

// The same question for a read. 05 の 8.8: what a host value carries is the
// registered type's table, so 't.width()' finds the member there -- but the
// table belongs to the type and not to the value, so a write must not reach
// it. That is why this is separate from table_of rather than part of it.
static const LhatTable *readable_table(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return ((const LhatHostData *)lhat_as_object(value))->members;
    }
    return table_of(value);
}

// 02 の 14.16改 (S36): what typeof^ answers where no checked type was
// compiled in -- the value's TAG, the dispatch information every value
// already carries, read in O(1). It never walks a structure: a table is
// table^ whatever it holds (the deep answer is the checker's to give, at
// compile time), and a subroutine or coroutine answers from the types its
// proto already carries, which were made at compile time too (03 の 5.11a).
// The one deep-looking case, an overload's arms, is bounded by the arm
// count rather than by any value's size.
static LhatRuntimeType *tag_type(LhatHeap *heap, LhatValue value)
{
    if (lhat_is_nil(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_NIL);
    }
    if (lhat_is_bool(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_BOOL);
    }
    if (lhat_is_number(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_NUMBER);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_STRING);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_COROUTINE)) {
        // 13.9, S28: R and Y have no written form, so wherever they are
        // known at all it is through 03 の 5.11a's checked_type, already
        // converted onto the originating proto by compile_subroutine.
        // Reused directly (not copied) the same way the SUBROUTINE branch
        // below already reuses proto->result_type -- the chunk that owns
        // it outlives every machine that ever reflects one of its values.
        const LhatCoroutine *coroutine = (const LhatCoroutine *)lhat_as_object(value);
        const LhatProto *proto =
            coroutine->closure != NULL ? coroutine->closure->proto : NULL;
        LhatRuntimeType *type = lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
        if (type != NULL && proto != NULL) {
            type->receive = proto->yield_receive_type;
            type->produce = proto->yield_produce_type;
            type->result = proto->result_type;
            // 15.3改: which kind of body this came from, which is what
            // decides who may advance it (15.6改).
            type->is_function = proto->is_function;
        }
        return type;
    }
    // 04 の 2.4: an error's identity is the declaration, so what typeof^
    // answers with is the kind object itself -- 05 の 7 の 7.4's IOError.NotFound.
    if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
        const LhatError *error = (const LhatError *)lhat_as_object(value);
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR_KIND);
        if (type != NULL) {
            type->error_kind = error->kind;
        }
        return type;
    }
    // 14.5, 14.12: a multi-dispatched member is callable every way its arms
    // list, which is what '&' means -- 14.12 prints one the same way.
    if (lhat_is_object_kind(value, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *overload =
            (const LhatOverload *)lhat_as_object(value);
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_INTERSECT);
        if (type == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < overload->count; i++) {
            LhatRuntimeType *arm = tag_type(heap, overload->candidates[i]);
            if (arm == NULL || !lhat_type_rt_add_part(type, arm)) {
                return NULL;
            }
        }
        return type;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        const LhatClosure *closure = (const LhatClosure *)lhat_as_object(value);
        const LhatProto *proto = closure->proto;
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_SUBROUTINE);
        if (type == NULL) {
            return NULL;
        }
        type->is_function = proto->is_function;
        type->takes_self = proto->takes_self;
        // 13.7: the last slot collects rather than taking one argument for
        // itself, so it is kept apart from `parts` here the same way
        // v.func.variadic is kept apart from a checked type's own params.
        uint8_t fixed_end = proto->has_variadic ? proto->parameters - 1
                                                : proto->parameters;
        // 14.4: self^ is not a parameter of the signature -- it is the
        // receiver, written out only where 14.4 already says so. 11.3改: the
        // slot it occupies is the last one when it was written there.
        uint8_t first = proto->takes_self && !proto->self_last ? 1 : 0;
        if (proto->takes_self && proto->self_last && fixed_end > 0) {
            fixed_end--;
        }
        for (uint8_t i = first; i < fixed_end; i++) {
            LhatRuntimeType *param = proto->parameter_types != NULL
                                         ? proto->parameter_types[i]
                                         : NULL;
            if (param == NULL) {
                // 13.7: nothing written asks for the top type, not nothing.
                param = lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
            }
            if (param == NULL || !lhat_type_rt_add_part(type, param)) {
                return NULL;
            }
        }
        if (proto->has_variadic) {
            LhatRuntimeType *element =
                proto->parameter_types != NULL
                    ? proto->parameter_types[fixed_end]
                    : NULL;
            type->variadic = element != NULL
                                 ? element
                                 : lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
            if (type->variadic == NULL) {
                return NULL;
            }
        }
        type->result = proto->result_type;
        return type;
    }
    // 14.16改: a table answers 13.7's unstructured top of tables, whatever
    // it holds -- deep shape is the checker's answer, given at compile time,
    // and the run does not build one out of data.
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_TABLE);
    }
    // 05 の 8.8: a host value's identity is its tag, another pointer read.
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTDATA);
        if (type != NULL) {
            type->hostdata_tag =
                ((const LhatHostData *)lhat_as_object(value))->tag;
        }
        return type;
    }
    // 05 の 8.9: the same off the head slot's tag.
    if (lhat_is_hostvalue(value)) {
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE);
        if (type != NULL) {
            type->hostvalue_tag = lhat_as_hostvalue_tag(value);
        }
        return type;
    }
    // A type-info value itself, a runtime operation, … -- nothing more to
    // say than that a value is there.
    return lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
}

// The operations 02 の 12.6 and 15.6 give a coroutine, and 14.17's tostring,
// which every value carries. The rest of the standard library is M2 and will
// not go through here.
//
// 14.17改 and 16.3改: the two a table carries are written with a hat as well.
// `hatted` says which spelling reached here, which is what builtin_member
// needs -- the bare one is a name a writer may mean for something else.
static bool native_named(LhatValue key, LhatNativeKind *out, bool *hatted)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    *hatted = false;
    if (name->length == 5 && memcmp(name->text, "start", 5) == 0) {
        *out = LHAT_NATIVE_START;
        return true;
    }
    if (name->length == 6 && memcmp(name->text, "resume", 6) == 0) {
        *out = LHAT_NATIVE_RESUME;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "dispose", 7) == 0) {
        *out = LHAT_NATIVE_DISPOSE;
        return true;
    }
    if (name->length == 4 && memcmp(name->text, "done", 4) == 0) {
        *out = LHAT_NATIVE_DONE;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "started", 7) == 0) {
        *out = LHAT_NATIVE_STARTED;
        return true;
    }
    if ((name->length == 7 && memcmp(name->text, "iterate", 7) == 0) ||
        (name->length == 8 && memcmp(name->text, "iterate^", 8) == 0)) {
        *out = LHAT_NATIVE_ITERATE;
        *hatted = name->length == 8;
        return true;
    }
    if ((name->length == 8 && memcmp(name->text, "tostring", 8) == 0) ||
        (name->length == 9 && memcmp(name->text, "tostring^", 9) == 0)) {
        *out = LHAT_NATIVE_TOSTRING;
        *hatted = name->length == 9;
        return true;
    }
    if ((name->length == 8 && memcmp(name->text, "tonumber", 8) == 0) ||
        (name->length == 9 && memcmp(name->text, "tonumber^", 9) == 0)) {
        *out = LHAT_NATIVE_TONUMBER;
        *hatted = name->length == 9;
        return true;
    }
    return false;
}

// 14.9: a table nobody made with a def^. Every name on one is the writer's,
// which is what 14.17改 turns on -- a definition and an instance of it carry
// names 14 章 reserved, and a table literal carries none.
static bool plain_table(LhatValue on)
{
    if (!lhat_is_object_kind(on, LHAT_OBJECT_TABLE)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(on);
    return table->definition == NULL && !table->is_definition;
}

// 02 の 16.3: `in^ e` asks e for the coroutine to walk. A table answers with
// one over its keys and a coroutine answers with itself, and both are built
// in -- the same footing 12.6 gives dispose(). A member of that name written
// by hand wins, since lhat_table_get is asked first.
//
// 14.17改 and 16.3改: on a plain table the bare spelling is not one of these
// at all. 14.11 already writes new with a hat, and the reason is the same
// one: every name on a table the writer wrote is the writer's, so a built-in
// sitting on `tostring` there is the implementation taking a word it has no
// claim to. A def^ is the other way round -- 14 章 reserves new, tostring and
// dispose on one, which is what the hat spelling is saying -- and a value
// with no members of its own (a number^, a coroutine, an error) has no names
// to take. Those keep both spellings.
static bool builtin_member(LhatValue on, LhatValue key, LhatNativeKind *out)
{
    bool hatted = false;
    if (!native_named(key, out, &hatted)) {
        return false;
    }
    if (!hatted && plain_table(on)) {
        return false;
    }
    // 02 の 14.17: whatever the value is, it can be written down.
    if (*out == LHAT_NATIVE_TOSTRING) {
        return true;
    }
    // 14.17改2: only a string^ can be read as a number^.
    if (*out == LHAT_NATIVE_TONUMBER) {
        return lhat_is_object_kind(on, LHAT_OBJECT_STRING);
    }
    if (lhat_is_object_kind(on, LHAT_OBJECT_COROUTINE)) {
        return true;  // every one of them applies to a coroutine
    }
    return *out == LHAT_NATIVE_ITERATE &&
           (lhat_is_object_kind(on, LHAT_OBJECT_TABLE) ||
            lhat_is_object_kind(on, LHAT_OBJECT_ERROR));
}

// 02 の 14.12: whether this candidate takes what the call is handing over.
// The receiver is not asked about -- 14.12 keeps self^ out of the judgement
// for the same reason it keeps it out of override^'s.
static bool fits_call(LhatValue candidate, const LhatValue *at, uint8_t given,
                      bool method, size_t *skip)
{
    if (!lhat_is_object_kind(candidate, LHAT_OBJECT_SUBROUTINE)) {
        return false;
    }
    const LhatProto *proto =
        ((const LhatClosure *)lhat_as_object(candidate))->proto;
    if (proto == NULL) {
        return false;
    }

    size_t passed = given;
    size_t first = 1;
    size_t declared = 0;
    if (method) {
        // 5.3 lays a method call out as callee, receiver, then arguments, so
        // what was given starts at 2 either way. 14.4's self^ is the first
        // parameter and is not asked about -- the receiver is what it is.
        first = 2;
        if (proto->takes_self) {
            passed = (size_t)given + 1;
            declared = 1;
        }
    }
    if (passed != proto->parameters) {
        return false;
    }

    // 11.3改 (S39): the receiver occupies the last slot instead, so the
    // arguments are the ones before it and the walk stops one short.
    size_t stop = proto->parameters;
    if (method && proto->takes_self && proto->self_last) {
        declared = 0;
        stop = proto->parameters > 0 ? proto->parameters - 1 : 0;
    }
    for (size_t i = declared; i < stop; i++) {
        const struct LhatRuntimeType *wanted =
            proto->parameter_types != NULL ? proto->parameter_types[i] : NULL;
        if (!lhat_value_satisfies(at[first + i - declared], wanted)) {
            return false;
        }
    }
    *skip = method && !proto->takes_self ? 2 : 1;
    return true;
}

// The body a value carries, or NULL when it is not a subroutine at all.
static const LhatProto *proto_of(LhatValue value)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    return ((const LhatClosure *)lhat_as_object(value))->proto;
}

// 11.3改 (S39): how one side answered the operator it was asked for.
typedef enum {
    OPERATOR_PICKED,       // a candidate was found and takes the other operand
    OPERATOR_ABSENT,       // this side carries no such member
    OPERATOR_NO_CANDIDATE, // it carries a group, and none of it takes this
    OPERATOR_NOT_CALLABLE, // the member is there and is not a subroutine
    OPERATOR_NO_MEMORY
} OperatorLookup;

// The operator `name` as one side carries it, when that side is the receiver.
//
// `self_last` says which spelling is being looked for: the side standing on
// the left writes its self^ first (14.4), the side standing on the right
// writes it last. One written the other way round describes the other order,
// so it is not an answer here and the search passes over it.
//
// The right side is a fallback and has to qualify to be chosen, so its lone
// candidate is asked whether it takes the other operand -- the left's is not,
// which keeps the ordinary path exactly as it was (the checker is what judges
// a written one, and 03 の 3.1's relaxed leaves it to the body).
static OperatorLookup operator_candidate(Machine *m, LhatValue side,
                                         const char *name, size_t length,
                                         LhatValue receiver, LhatValue argument,
                                         bool self_last, LhatValue *picked)
{
    *picked = lhat_nil();
    const LhatTable *carrier = table_of(side);
    // 05 の 8.9: a host value's operators live in the members table the
    // machine bound for its type -- the value has no heap half of its own.
    if (carrier == NULL && lhat_is_hostvalue(side)) {
        carrier = hostvalue_members_of(m, lhat_as_hostvalue_tag(side));
    }
    if (name == NULL || carrier == NULL) {
        return OPERATOR_ABSENT;
    }
    LhatString *key = lhat_string_new(&m->objects, name, length);
    if (key == NULL) {
        return OPERATOR_NO_MEMORY;
    }
    LhatValue found = lhat_table_get(carrier, lhat_object((LhatObject *)key));
    if (lhat_is_nil(found)) {
        return OPERATOR_ABSENT;
    }

    // 14.12: a type may answer one operator for several right-hand types, and
    // then the member is a group. The search is the same one a call makes --
    // at most one candidate fits, so it ends at the first. 14.4's layout for
    // a method call is callee, receiver, arguments.
    LhatValue shaped[3];
    shaped[1] = receiver;
    shaped[2] = argument;
    if (lhat_is_object_kind(found, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *group = (const LhatOverload *)lhat_as_object(found);
        for (size_t i = 0; i < group->count; i++) {
            if (proto_of(group->candidates[i]) == NULL ||
                proto_of(group->candidates[i])->self_last != self_last) {
                continue;
            }
            size_t skip = 1;
            shaped[0] = group->candidates[i];
            if (fits_call(group->candidates[i], shaped, 1, true, &skip)) {
                *picked = group->candidates[i];
                return OPERATOR_PICKED;
            }
        }
        return OPERATOR_NO_CANDIDATE;
    }

    // 05 の 8.9: a host value's operator is a host function -- registered,
    // never written in L^. It carries no self_last (11.3改 is a written
    // shape), so only the left-operand ask takes it.
    if (lhat_is_object_kind(found, LHAT_OBJECT_HOST)) {
        if (self_last) {
            return OPERATOR_ABSENT;
        }
        *picked = found;
        return OPERATOR_PICKED;
    }
    if (!lhat_is_object_kind(found, LHAT_OBJECT_SUBROUTINE)) {
        return OPERATOR_NOT_CALLABLE;
    }
    const LhatProto *proto = proto_of(found);
    if (proto == NULL || proto->self_last != self_last) {
        return OPERATOR_ABSENT;  // it answers the other order
    }
    if (self_last) {
        size_t skip = 1;
        shaped[0] = found;
        if (!fits_call(found, shaped, 1, true, &skip)) {
            return OPERATOR_NO_CANDIDATE;
        }
    }
    *picked = found;
    return OPERATOR_PICKED;
}

// 15.4 with 5.4: the inverse of the move a yield^ makes. The frame is back
// on the stack at `base` -- always as the new top frame, so its slots are
// the highest addresses in use -- and every capture that traveled with the
// saved registers points back into the live slots and rejoins the machine's
// open list at its head. The coroutine's own list is kept in ascending slot
// order, so prepending one by one lands them in the descending order the
// machine's list keeps.
static void reattach_upvalues(Machine *m, LhatCoroutine *co, size_t base)
{
    while (co->open != NULL) {
        LhatUpvalue *up = co->open;
        co->open = up->next_open;
        size_t offset = (size_t)(up->location.value - co->registers.values);
        up->location = lhat_slots_ref(m->slots, base + offset);
        up->suspended_in = NULL;
        up->next_open = m->open;
        m->open = up;
    }
}

// 5.11 with 02 の 10.7: puts a suspended coroutine's one saved frame back on
// the stack so that what it still owes can be run. The caller has already
// made room (LHAT_MAX_FRAMES and the stack's own end) and picked where the
// frame goes: `next_base` is its first register and `result` the slot in the
// caller's frame the answer lands in.
//
// The frame comes back marked `disposing`, which 10.7 needs -- a yield^ from
// inside a cleanup has nothing to suspend into -- and set to drain rather
// than to run: every cleanup, innermost first, and then the coroutine is
// finished. What the caller does with `frame`/`registers`/`chunk`/`pc` after
// this is jump to the drain.
static void enter_disposal_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result,
                                 Frame **frame, size_t *rbase,
                                 const LhatChunk **chunk, size_t *pc)
{
    for (size_t i = 0; i < co->register_count; i++) {
        lhat_slots_set(m->slots, next_base + i,
                       lhat_slots_get(co->registers, i));
    }
    reattach_upvalues(m, co, next_base);
    Frame *called = &m->frames[m->frame_count++];
    called->closure = co->closure;
    called->pc = co->pc;
    called->base = next_base;
    called->result = result;
    called->coroutine = co;
    called->disposing = true;
    called->returning = true;
    called->drain_target = 0;
    called->answer = lhat_nil();
    called->cleanup_count = co->cleanup_count;
    for (size_t i = 0; i < co->cleanup_count; i++) {
        called->cleanups[i] = co->cleanups[i];
    }
    co->state = LHAT_COROUTINE_RUNNING;

    *frame = called;
    *rbase = called->base;
    *chunk = &co->closure->proto->chunk;
    *pc = called->pc;
}

// 5.4: one place per slot, so two closures capturing the same name share it.
// The machine's list only ever holds captures of live stack slots, so the
// payload pointer alone orders it -- the tag pointer travels alongside.
static LhatUpvalue *capture(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL && (*link)->location.value > place) {
        link = &(*link)->next_open;
    }
    if (*link != NULL && (*link)->location.value == place) {
        return *link;
    }

    LhatUpvalue *upvalue =
        (LhatUpvalue *)lhat_object_alloc(&m->objects, sizeof *upvalue,
                                        LHAT_OBJECT_UPVALUE);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = lhat_slots_ref(m->slots, slot);
    upvalue->next_open = *link;
    *link = upvalue;
    return upvalue;
}

// 5.12: everything in this file that writes into a table goes through here.
// A table is written over and over, so the barrier it takes is the backward
// one -- the table goes back on the collector's list, to be looked at once
// the writing has settled, rather than every value being marked as it
// arrives. Both the key and the value are stored, so both are asked about.
//
// One place for the barrier to be missing from rather than nine. A table
// made in this same instruction is white and the barrier does nothing, so
// there is no call site that has to know which kind it has.
static bool set_key(Machine *m, LhatTable *table, LhatValue key,
                    LhatValue value, bool *refused)
{
    if (!lhat_table_set(table, key, value, refused)) {
        return false;
    }
    lhat_gc_barrier_back(m, (LhatObject *)table, key);
    lhat_gc_barrier_back(m, (LhatObject *)table, value);
    return true;
}

// Carrying a shared place into the upvalue itself: the value moves into the
// closed cell and location aims there, the same dereference as before.
static void close_into_cell(Machine *m, LhatUpvalue *upvalue)
{
    LhatValue held = lhat_ref_get(upvalue->location);
    upvalue->closed_value = held.as;
    upvalue->closed_tag = (uint8_t)held.tag;
    upvalue->location = lhat_upvalue_closed_ref(upvalue);
    // 5.12: what was a stack slot the roots covered is now a place the
    // upvalue itself holds, so a black upvalue has just gained a reference
    // the collector has not seen.
    lhat_gc_barrier(m, (LhatObject *)upvalue, held);
}

// The frame is going, so anything still pointing into it carries its value
// away. Without this a closure returned from a subroutine would read a slot
// that has been reused.
static void close_upvalues(Machine *m, size_t above)
{
    const LhatValueUnion *floor = m->slots.values + above;
    while (m->open != NULL && m->open->location.value >= floor) {
        LhatUpvalue *upvalue = m->open;
        m->open = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

// The same for one place rather than a frame's worth. 03 の 4.3: a session's
// top level puts every let^ of a name back in the one slot, so severing that
// binding's sharing must leave the names above it -- other bindings, still
// live -- shared as they were.
static void close_one_upvalue(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL) {
        LhatUpvalue *upvalue = *link;
        if (upvalue->location.value != place) {
            link = &upvalue->next_open;
            continue;
        }
        *link = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

static LhatRunResult finish(Machine *m, const LhatChunk *chunk,
                            LhatRunStatus status, LhatValue value, size_t at)
{
    // 03 の 4.3: what the program allocated belongs to the machine, not to
    // the run -- the answer may be a table or a string, and a REPL's next
    // input has to be able to reach it. So a result owns nothing and the
    // answer is good for as long as the machine is.
    LhatRunResult result;
    result.status = status;
    result.value = value;
    result.at = at;
    // 04 の 11 章: named from the chunk's own line table (03 の 5.11a-style
    // parallel array) and 02 の 11.8's operator names -- both silently
    // answer nothing usable when `at` is out of range, which only happens
    // for the one fault raised before the first instruction runs.
    result.line = at < chunk->count ? chunk->lines[at] : 0;
    result.op_name =
        at < chunk->count ? operator_name(lhat_op(chunk->code[at]),
                                          &result.op_name_length)
                          : NULL;
    if (result.op_name == NULL) {
        result.op_name_length = 0;
    }
    result.collected = m->collected;
    result.live = m->objects.count;

    // 5.4: an upvalue points into the stack, and the frames are about to go
    // -- but the list is not emptied here. 03 の 4.3: a session's top-level
    // slots outlive the run, and what points into them has to stay listed as
    // open, or the next input could neither find it to close (a let^ writing
    // that name again) nor find it to share (a second closure over the same
    // name). What the run above left pointing into slots that do not survive
    // is closed by the next lhat_run, before it clears them.
    return result;
}

// 02 の 16.3: a table's walk has no body to enter, so one step is the whole
// of resuming it. Both the opcode the loops emit and the start()/resume() a
// program writes by hand take this step -- 16.3 puts a table's iterate() on
// the same footing as any other coroutine, so what the two do has to agree.
typedef enum {
    WALK_TOOK,      // a pair came out; the walk is suspended again
    WALK_ENDED,     // nothing left, so the walk is finished
    WALK_NO_MEMORY
} WalkStep;

static WalkStep step_table_walk(Machine *m, LhatCoroutine *co, LhatValue *out)
{
    LhatValue key, value;
    if (!lhat_table_walk(co, &key, &value)) {
        co->state = LHAT_COROUTINE_DONE;
        *out = lhat_nil();
        return WALK_ENDED;
    }
    // 13.8 has no tuples, so the pair a walk yields is a table.
    LhatTable *pair = lhat_table_new(&m->objects);
    bool refused = false;
    if (pair == NULL ||
        !set_key(m, pair, lhat_integer(1), key, &refused) ||
        !set_key(m, pair, lhat_integer(2), value, &refused)) {
        return WALK_NO_MEMORY;
    }
    co->state = LHAT_COROUTINE_SUSPENDED;
    *out = lhat_object((LhatObject *)pair);
    return WALK_TOOK;
}

// 05 の 8.6: L^ is the one name that is there without being imported, so what
// it answers is made with the machine. A member is added here and its type in
// check.c's environment_type -- the two lists have to say the same thing.
static bool set_member(Machine *m, LhatTable *table, const char *name,
                       LhatValue value)
{
    LhatString *key = lhat_string_new(&m->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    bool refused = false;
    return set_key(m, table, lhat_object((LhatObject *)key), value, &refused) &&
           !refused;
}

static bool build_environment(Machine *m)
{
    m->environment = lhat_table_new(&m->objects);
    LhatTable *modules = lhat_table_new(&m->objects);
    // 05 の 5.3: the registry a unit is loaded into once. Empty until
    // something is loaded, and grown the way 8.8 grows any table.
    LhatNative *collect_now =
        lhat_native_new(&m->objects, LHAT_NATIVE_COLLECTGARBAGE, lhat_nil());
    if (m->environment == NULL || modules == NULL || collect_now == NULL) {
        return false;
    }
    if (!set_member(m, m->environment, "modules",
                    lhat_object((LhatObject *)modules)) ||
        !set_member(m, m->environment, "collectgarbage",
                    lhat_object((LhatObject *)collect_now))) {
        return false;
    }
    // 05 の 8.6改 (M5): sealed once built, not before -- the two members above
    // are the machine writing its own table, which is exactly what the mark
    // goes on to refuse from an instruction.
    //
    // The registry inside it is not marked here. 5.3 has a unit register
    // itself, and what it emits for that is an ordinary instruction writing
    // into L^.modules -- the mark would refuse the machine's own bookkeeping.
    // check.c refuses what a writer spells there, which is the half that can
    // be told apart by looking at the source.
    m->environment->sealed = true;
    return true;
}

LhatMachine *lhat_machine_new(void)
{
    // A whole stack and a frame array, so the heap is where it belongs --
    // a static one could serve only one caller and never nest.
    // calloc rather than malloc, and nothing more: 03 の 2.2 numbers
    // LHAT_VALUE_NIL first and gives it a zero payload, so zeroed memory is
    // already a stack full of nil^.
    Machine *m = (Machine *)lhat_calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    // 2.2改: the one view every register read goes through, fixed for the
    // machine's whole life. Tag zero is nil^, so the zeroed runs above are
    // already a stack full of it.
    m->slots.values = m->stack_values;
    m->slots.tags = m->stack_tags;
    m->threshold = LHAT_GC_INITIAL_THRESHOLD;
    if (!build_environment(m)) {
        lhat_machine_dispose(m);
        return NULL;
    }
    return m;
}

void lhat_machine_set_modules(LhatMachine *machine, const LhatModule *modules,
                              size_t count)
{
    if (machine == NULL) {
        return;
    }
    machine->modules = modules;
    machine->module_count = count;
}

bool lhat_machine_make_table(LhatMachine *machine, LhatValue *out)
{
    LhatTable *table = lhat_table_new(&machine->objects);
    if (table == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)table);
    return true;
}

bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters,
                            bool has_variadic, bool takes_self, LhatValue *out)
{
    LhatHost *host = lhat_host_new(&machine->objects, call, context, parameters,
                                   has_variadic, takes_self);
    if (host == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)host);
    return true;
}

// 05 の 8.7: the same walk the unit prologue compiles to, done in C because
// nothing is being compiled here. 02 の 8.8's rule holds: a table is made
// where the path does not reach one, and what is there is left alone.
static LhatTable *reach_table(Machine *m, LhatTable *owner, const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        LhatString *key = lhat_string_new(&m->objects, segment, length);
        if (key == NULL) {
            return NULL;
        }
        LhatValue found = lhat_table_get(owner, lhat_object((LhatObject *)key));
        LhatTable *next = table_of(found);
        if (next == NULL) {
            if (!lhat_is_nil(found)) {
                return NULL;  // something that is not a table is there
            }
            next = lhat_table_new(&m->objects);
            bool refused = false;
            if (next == NULL ||
                !set_key(m, owner, lhat_object((LhatObject *)key),
                         lhat_object((LhatObject *)next), &refused) ||
                refused) {
                return NULL;
            }
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

bool lhat_machine_make_hostdata(LhatMachine *machine, const LhatHostDataTag *tag,
                            void *pointer, LhatValue *out)
{
    if (machine == NULL || tag == NULL) {
        return false;
    }
    // The members live where the registration put them, which is under the
    // type's own name in L^.modules -- so what answers a call on this value
    // is the same table every other value of the type answers through.
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *registry = table_of(lhat_table_get(
        machine->environment, lhat_object((LhatObject *)modules_key)));
    if (registry == NULL) {
        return false;
    }
    LhatTable *module = reach_table(machine, registry, tag->module);
    LhatTable *members =
        module != NULL ? reach_table(machine, module, tag->name) : NULL;
    if (members == NULL) {
        return false;
    }

    LhatHostData *data = lhat_hostdata_new(&machine->objects, tag, pointer, members);
    if (data == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)data);
    return true;
}

void *lhat_hostdata_pointer(LhatValue value, const LhatHostDataTag *tag)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return NULL;
    }
    const LhatHostData *data = (const LhatHostData *)lhat_as_object(value);
    return data->tag == tag ? data->pointer : NULL;
}

void *lhat_hostvalue_data(LhatValue argument, const LhatHostValueTag *tag)
{
    if (!lhat_is_hostvalue(argument) || tag == NULL) {
        return NULL;
    }
    const LhatValueUnion *run = argument.as.hostvalue_run;
    if (run == NULL || run[0].hostvalue != tag) {
        return NULL;
    }
    // The head carries the tag; the bytes start one slot later. The cast
    // drops const on purpose: the run is a scratch copy of the caller's,
    // and 8.9 lets a host read or write it freely for the call's duration.
    return (void *)(run + 1);
}

bool lhat_make_hostvalue(LhatMachine *machine, const LhatHostValueTag *tag,
                         const void *bytes, LhatValue *out)
{
    if (machine == NULL || tag == NULL || bytes == NULL || out == NULL ||
        tag->size > LHAT_HOSTVALUE_MAX_BYTES) {
        return false;
    }
    LhatValueUnion *run = machine->hostvalue_scratch;
    // The data run's tail is zeroed so equality can compare whole slots.
    memset(run, 0, tag->width * sizeof *run);
    run[0].hostvalue = tag;
    memcpy(run + 1, bytes, tag->size);
    out->tag = LHAT_VALUE_HOSTVALUE;
    out->as.hostvalue_run = run;
    return true;
}

bool lhat_machine_bind_hostvalues(LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count)
{
    if (machine == NULL || entries == NULL || count == 0) {
        return false;
    }
    LhatTable **tables =
        (LhatTable **)lhat_calloc(count, sizeof *tables);
    if (tables == NULL) {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    LhatTable *registry =
        modules_key != NULL
            ? table_of(lhat_table_get(machine->environment,
                                      lhat_object((LhatObject *)modules_key)))
            : NULL;
    if (registry == NULL) {
        lhat_free(tables);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        // The same walk lhat_machine_make_hostdata does for a hostdata
        // type's members: the table registration put under the type's own
        // name is the type's members table.
        LhatTable *module = reach_table(machine, registry, entries[i].tag->module);
        LhatTable *members =
            module != NULL ? reach_table(machine, module, entries[i].tag->name)
                           : NULL;
        if (members == NULL || entries[i].tag->index >= count) {
            lhat_free(tables);
            return false;
        }
        tables[entries[i].tag->index] = members;
    }
    lhat_free(machine->hostvalue_members);  // a second install replaces
    machine->hostvalue_members = tables;
    machine->hostvalue_member_count = count;
    return true;
}

bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name, LhatValue value)
{
    if (machine == NULL || machine->environment == NULL) {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *owner = table_of(lhat_table_get(
        machine->environment, lhat_object((LhatObject *)modules_key)));
    if (owner == NULL) {
        return false;
    }

    owner = reach_table(machine, owner, module);
    if (owner != NULL && type != NULL) {
        owner = reach_table(machine, owner, type);
    }
    if (owner == NULL) {
        return false;
    }

    LhatString *key = lhat_string_new(&machine->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    bool refused = false;
    return set_key(machine, owner, lhat_object((LhatObject *)key), value,
                   &refused) && !refused;
}

bool lhat_machine_set_global(LhatMachine *machine, const char *name,
                             LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->environment == NULL || name == NULL) {
        return false;
    }
    // 05 の 8.6改: set_key rather than an instruction, so the seal on L^ is
    // not in the way -- what it refuses is what a program writes, and this is
    // the host writing what the program will read.
    return set_member(m, m->environment, name, value);
}

void lhat_machine_dispose(LhatMachine *machine)
{
    if (machine == NULL) {
        return;
    }
    // 05 の 8.8: what the host made goes back before anything is freed, so a
    // release may still read the value it is given. Reachability is not asked
    // -- the machine is going, so everything on it is.
    for (LhatObject *object = machine->objects.objects; object != NULL;
         object = object->next) {
        lhat_hostdata_release(object, machine);
    }
    lhat_object_free_all(&machine->objects);
    // 05 の 8.9: the tables were the heap's (freed just above); the array
    // alone is the machine's.
    lhat_free(machine->hostvalue_members);
    lhat_free(machine);
}

// The run loop itself, shared by lhat_run (base_depth == 0, a fresh unit
// entered through its own wrapper closure) and lhat_machine_call
// (base_depth == m->frame_count at the time of the call, a value already
// callable pushed as one more frame). "the run is over" means the frame
// count has drained back down to base_depth, not to zero -- 0 stays right
// for lhat_run because nothing was on the machine before it pushed frame 0.
// 2.2改: the frame's registers, read and written through the machine's two
// parallel runs. `m` and `rbase` are run_frames' own locals; nothing outside
// it may use these.
#define R(i) lhat_slots_get(m->slots, rbase + (size_t)(i))
#define SET_R(i, v) lhat_slots_set(m->slots, rbase + (size_t)(i), (v))

static LhatRunResult run_frames(Machine *m, size_t base_depth)
{
    Frame *frame = &m->frames[m->frame_count - 1];
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t rbase = frame->base;
    size_t pc = frame->pc;

    // 02 の 10.7: the last collection of the run, and what it found. See the
    // drain, which is where both are decided. `at` is hoisted out of the
    // loop only so that `ending` can jump to the drain without stepping over
    // its initialiser.
    bool end_swept = false;
    bool ending = false;
    size_t at = 0;

    while (pc < chunk->count) {
        // Between instructions, where every live value is in a register, a
        // frame or the open list. Inside one there is a half-built object the
        // roots do not name yet -- which is also why a write made in the same
        // instruction that made the object it goes into needs no barrier.
        //
        // 5.12: a step, not a collection. What this costs is bounded by
        // LHAT_GC_STEP_WORK whatever the heap has grown to.
        if (m->objects.count >= m->threshold) {
            frame->pc = pc;
            lhat_gc_step(m);
        }

        // 02 の 10.7: and here is where what the collector held back gets to
        // run. The heap is whole again and this is an ordinary place to call
        // L^ code, which is exactly what a finally^ body is. One coroutine
        // per boundary -- the rest keep, so a long list never becomes one
        // long pause.
        //
        // Never on top of a disposal already under way (`disposing`): a
        // cleanup is 10.7's own unwinding and another coroutine's cleanups
        // cutting into it would interleave two unwindings that have nothing
        // to do with each other. The queue waits; it is in no hurry.
        if (m->pending_dispose != NULL && !frame->disposing) {
            if (m->frame_count >= LHAT_MAX_FRAMES) {
                return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            // Just past what this frame uses, so nothing live is written
            // over. Disposal answers nil^ and no one is asking, but the
            // return still lands somewhere and this is somewhere harmless.
            uint8_t into = chunk->registers;
            size_t next_base = rbase + (into) + 1;
            if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            LhatCoroutine *co = lhat_gc_take_pending(m);
            frame->pc = pc;
            enter_disposal_frame(m, co, next_base, into, &frame, &rbase,
                                 &chunk, &pc);
            // 15.4: same as an explicit dispose -- the slot a yield^ answers
            // into gets nil^, since nothing sent anything in.
            SET_R(co->sent_into, lhat_nil());
            goto drain;
        }

        // 02 の 10.7: the run already ended and only the queue was holding
        // it open. It is empty now, so back to the frame that was leaving.
        // Only once that frame is on top again -- while a cleanup of its
        // own is running there is a frame above it with instructions left.
        if (ending && m->frame_count == base_depth + 1) {
            goto drain;
        }

        LhatInstruction instruction = chunk->code[pc];
        at = pc++;

        uint8_t a = lhat_a(instruction);
        uint8_t b = lhat_b(instruction);
        uint8_t cc = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);
        // 02 の 11.9 (S40): the comparison that sent control to call_operator,
        // when one did. `op` becomes SPACESHIP there -- that is the member to
        // look for -- and this is what the answer gets read against zero with.
        LhatOpcode derive_from = LHAT_FRAME_NO_DERIVE;

        switch (op) {
            case LHAT_BC_LOADK:
                SET_R(a, chunk->constants[lhat_bx(instruction)]);
                break;
            case LHAT_BC_LOADNIL:
                SET_R(a, lhat_nil());
                break;
            case LHAT_BC_LOADBOOL:
                SET_R(a, lhat_bool(b != 0));
                break;
            case LHAT_BC_MOVE:
                SET_R(a, R(b));
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
                if (arithmetic(op, R(b), R(cc), &out,
                               &status)) {
                    SET_R(a, out);
                    break;
                }
                // 02 の 11.3: numbers answer built in; anything else answers
                // with the member 11.8 names, or not at all.
                //
                // 11.3改 (S39): the right operand is reason enough to go on.
                // '1 + v' is the case the rule exists for -- number^ carries
                // the arithmetic and takes only its own kind, so the answer
                // can only be on the other side.
                // 05 の 8.9: a host value carries its registered operators
                // the same way a table carries 11.8's members.
                if (status != LHAT_RUN_TYPE_ERROR ||
                    (table_of(R(b)) == NULL && table_of(R(cc)) == NULL &&
                     !lhat_is_hostvalue(R(b)) && !lhat_is_hostvalue(R(cc)))) {
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                goto call_operator;
            }

            case LHAT_BC_NEG: {
                if (!lhat_is_number(R(b))) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 14.8改: INT64_MIN is the one integer whose negation is not
                // an integer, so it widens like any other overflow.
                int64_t negated = 0;
                SET_R(a,
                    lhat_is_integer(R(b)) &&
                            subtract_exact(0, lhat_as_integer(R(b)),
                                           &negated)
                        ? lhat_integer(negated)
                        : lhat_real(-lhat_number_as_real(R(b))));
                break;
            }

            case LHAT_BC_NOT: {
                // 02 の 5.4's condition rule: only a bool is a truth value,
                // so this refuses anything else rather than inventing one.
                if (!lhat_is_bool(R(b))) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(!lhat_as_bool(R(b))));
                break;
            }

            case LHAT_BC_TYPEOF: {
                LhatRuntimeType *type = tag_type(&m->objects, R(b));
                if (type == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)type));
                break;
            }

            // 11.9 (S40): a type saying how it compares says what equality
            // means as well, so a table is asked before 14.2's own answer is
            // taken. Nothing carrying a '<=>' falls back to that answer, which
            // is what leaves every other table exactly as it was.
            case LHAT_BC_EQ:
            case LHAT_BC_NE:
                // 05 の 8.9: byte equality under the same tag. Asked before
                // the raw compare below, which would read the heads alone
                // and call two same-typed values equal whatever their bytes.
                if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    bool equal = lhat_is_hostvalue(R(b)) &&
                                 hostvalue_equal(m->slots, rbase + b,
                                                 rbase + cc);
                    SET_R(a, lhat_bool(equal == (op == LHAT_BC_EQ)));
                    break;
                }
                if (table_of(R(b)) != NULL || table_of(R(cc)) != NULL) {
                    derive_from = op;
                    op = LHAT_BC_SPACESHIP;
                    goto call_operator;
                }
                SET_R(a, lhat_bool(lhat_value_equal(R(b), R(cc)) ==
                                   (op == LHAT_BC_EQ)));
                break;
            case LHAT_BC_SAME:
                // 05 の 8.9: a value type has no identity apart from its
                // bytes, so "the same" is the equality above.
                if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    SET_R(a, lhat_bool(lhat_is_hostvalue(R(b)) &&
                                       hostvalue_equal(m->slots, rbase + b,
                                                       rbase + cc)));
                    break;
                }
                SET_R(a, lhat_bool(
                    lhat_value_same(R(b), R(cc))));
                break;

            case LHAT_BC_LT:
            case LHAT_BC_LE:
            case LHAT_BC_GT:
            case LHAT_BC_GE: {
                bool out = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (ordering(op, R(b), R(cc), &out, &status)) {
                    SET_R(a, lhat_bool(out));
                    break;
                }
                // 11.9 (S40): numbers order themselves; anything else says
                // how it orders with a '<=>', and this reads the answer.
                derive_from = op;
                op = LHAT_BC_SPACESHIP;
                goto call_operator;
            }

            // 11.9 (S40): written out. number^ and string^ each order their
            // own; anything else answers with the member 11.8 names.
            case LHAT_BC_SPACESHIP: {
                int outcome = 0;
                if (!three_way(R(b), R(cc), &outcome)) {
                    goto call_operator;
                }
                SET_R(a, lhat_integer(outcome));
                break;
            }

            case LHAT_BC_JUMP:
                pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                break;

            case LHAT_BC_JUMP_FALSE:
                if (!lhat_is_bool(R(a))) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (!lhat_as_bool(R(a))) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;

            case LHAT_BC_CLOSURE: {
                const LhatProto *nested =
                    frame->closure->proto->protos[lhat_bx(instruction)];
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = nested;
                closure->upvalue_count = nested->upvalue_count;
                if (nested->upvalue_count > 0) {
                    closure->upvalues = (LhatUpvalue **)lhat_calloc(
                        nested->upvalue_count, sizeof *closure->upvalues);
                    if (closure->upvalues == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                // 5.4: a register of this frame, one of its own upvalues
                // when the name came from further out, or -- 15.10改's
                // this^^ -- this frame's own closure, boxed closed on the
                // spot: nothing on the stack holds it, so there is nothing
                // to keep open.
                for (size_t i = 0; i < nested->upvalue_count; i++) {
                    const LhatUpvalueDesc *desc = &nested->upvalues[i];
                    LhatUpvalue *made = NULL;
                    switch (desc->source) {
                        case LHAT_UPVALUE_REGISTER:
                            made = capture(m, rbase + desc->index);
                            break;
                        case LHAT_UPVALUE_OUTER:
                            made = frame->closure->upvalues[desc->index];
                            break;
                        case LHAT_UPVALUE_THIS:
                            made = (LhatUpvalue *)lhat_object_alloc(
                                &m->objects, sizeof *made,
                                LHAT_OBJECT_UPVALUE);
                            if (made != NULL) {
                                lhat_ref_set(
                                    lhat_upvalue_closed_ref(made),
                                    lhat_object(
                                        (LhatObject *)frame->closure));
                                made->location =
                                    lhat_upvalue_closed_ref(made);
                                made->next_open = NULL;
                            }
                            break;
                    }
                    closure->upvalues[i] = made;
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            // 03 の 5.11c: strict already found the one arm that fits (14.12
            // leaves at most one), so the call ahead takes it instead of
            // asking every candidate again. Nothing else is touched: a value
            // that is not a group reached here some way the checker did not
            // read, and the ordinary call is still right for it.
            case LHAT_BC_PICKARM: {
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t which = lhat_bx(instruction);
                    if (which < group->count) {
                        SET_R(a, group->candidates[which]);
                    }
                }
                break;
            }

            case LHAT_BC_GETUPVAL:
                SET_R(a, lhat_ref_get(frame->closure->upvalues[b]->location));
                break;

            case LHAT_BC_SETUPVAL: {
                // 5.12: an open upvalue's place is a stack slot and the
                // stack is a root, so only a closed one gains anything the
                // collector has not seen. The barrier asks which it is by
                // asking whether the upvalue is black.
                LhatUpvalue *upvalue = frame->closure->upvalues[b];
                lhat_ref_set(upvalue->location, R(a));
                lhat_gc_barrier(m, (LhatObject *)upvalue, R(a));
                break;
            }

            // 02 の 11.2: '..' is concatenation in general, and strings are
            // the case that is settled. 11.3 leaves the rest to the
            // operator's own definition, which needs op^.
            case LHAT_BC_CONCAT: {
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_STRING) &&
                    lhat_is_object_kind(R(cc), LHAT_OBJECT_STRING)) {
                    const LhatString *left =
                        (const LhatString *)lhat_as_object(R(b));
                    const LhatString *right =
                        (const LhatString *)lhat_as_object(R(cc));
                    LhatString *joined =
                        lhat_string_concat(&m->objects, left, right);
                    if (joined == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)joined));
                    break;
                }

                // 02 の 11.3: a string answers above, built in. Anything else
                // answers with the member 11.8 names, or not at all -- and
                // 11.3改 (S39) reads the right operand for one too.
                if (table_of(R(b)) == NULL && table_of(R(cc)) == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                goto call_operator;
            }

            case LHAT_BC_CLOSE:
                close_upvalues(m, rbase + a);
                break;

            case LHAT_BC_CLOSEONE:
                close_one_upvalue(m, rbase + a);
                break;

            // 02 の 15.10: the frame already holds it, so naming it costs a
            // move rather than a capture.
            case LHAT_BC_THIS:
                SET_R(a,
                    lhat_object((LhatObject *)(void *)frame->closure));
                break;

            // 05 の 8.6: one table per machine, so naming it is a move too.
            case LHAT_BC_ENV:
                SET_R(a, lhat_object((LhatObject *)m->environment));
                break;

            // 05 の 5.3: a unit is a body like any other, so requiring it is
            // making a closure of it and calling that. What makes it load
            // once is the guard the unit itself begins with, not this.
            case LHAT_BC_UNIT: {
                size_t which = lhat_bx(instruction);
                if (which >= m->module_count) {
                    return finish(m, chunk, LHAT_RUN_NO_SUCH_UNIT, lhat_nil(), at);
                }
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = m->modules[which].proto;
                closure->upvalues = NULL;
                closure->upvalue_count = 0;
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            case LHAT_BC_NEWTABLE: {
                LhatTable *table = lhat_table_new(&m->objects);
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                table->is_definition = b != 0;  // 14.9
                SET_R(a, lhat_object((LhatObject *)table));
                break;
            }

            // 05 の 8.6改 (M5): what require^ answers is the machine's record
            // of what a unit published. Sealed once it is built, so that a
            // requiring unit cannot add to it or write over it -- not even by
            // passing it to a p^ whose parameter is written t^{ … }, which is
            // the one path check.c cannot name.
            case LHAT_BC_SEAL: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                table->sealed = true;
                break;
            }

            // 04 の 11.3: a key that is not there answers nil^, so the only
            // way this fails is being asked of something that is not a table.
            // An error answers from its fields: 2.3 gives every kind message
            // and cause, and they are reached the same way as a member.
            case LHAT_BC_GETINDEX: {
                // 02 の 12.6 and 15.6: a coroutine answers the operations the
                // runtime provides, bound to what they came through.
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    LhatNativeKind which;
                    bool hatted = false;
                    if (!native_named(R(cc), &which, &hatted)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    LhatNative *native =
                        lhat_native_new(&m->objects, which, R(b));
                    if (native == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)native));
                    break;
                }
                // 02 の 14.16: a type-info value carries exactly one member.
                // It is not a table, so the ordinary lookup below never sees
                // it -- this is what makes '.signature' answer something.
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_TYPE)) {
                    if (!lhat_is_object_kind(R(cc), LHAT_OBJECT_STRING)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    const LhatString *key =
                        (const LhatString *)lhat_as_object(R(cc));
                    if (key->length != 9 ||
                        memcmp(key->text, "signature", 9) != 0) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    const LhatRuntimeType *type =
                        (const LhatRuntimeType *)lhat_as_object(R(b));
                    size_t needed = lhat_runtime_type_write(type, NULL, 0);
                    char *text = (char *)lhat_alloc(needed + 1);
                    if (text == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    lhat_runtime_type_write(type, text, needed + 1);
                    LhatString *written =
                        lhat_string_new(&m->objects, text, needed);
                    lhat_free(text);
                    if (written == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    SET_R(a, lhat_object((LhatObject *)written));
                    break;
                }
                // 05 の 8.9: a host value answers a registered field off its
                // own bytes -- no host call -- and a registered member out
                // of the type's table the machine bound at install.
                if (lhat_is_hostvalue(R(b))) {
                    const LhatHostValueTag *hv_tag = lhat_as_hostvalue_tag(R(b));
                    const LhatHostValueField *field =
                        hostvalue_field_named(hv_tag, R(cc));
                    if (field != NULL) {
                        SET_R(a, hostvalue_field_get(
                                     m->slots.values + rbase + b + 1, field));
                        break;
                    }
                    const LhatTable *hv_members =
                        hostvalue_members_of(m, hv_tag);
                    if (hv_members == NULL) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    SET_R(a, lhat_table_get(hv_members, R(cc)));
                    break;
                }
                const LhatTable *table = readable_table(R(b));
                if (table == NULL) {
                    // 02 の 14.17: nil^, bool^, number^ and string^ hold no
                    // members of their own, but every value can be written
                    // down, and 14.17改2 reads a number^ back out of a
                    // string^. Nothing else reaches a value that is not a
                    // table, so these two are the whole of what one answers
                    // -- iterate stays the table path's, which is where a
                    // value carrying fields comes through.
                    LhatNativeKind bare;
                    if (builtin_member(R(b), R(cc), &bare) &&
                        (bare == LHAT_NATIVE_TOSTRING ||
                         bare == LHAT_NATIVE_TONUMBER)) {
                        LhatNative *native =
                            lhat_native_new(&m->objects, bare, R(b));
                        if (native == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)native));
                        break;
                    }
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                SET_R(a, lhat_table_get(table, R(cc)));

                // 16.3: a table has an iterate of its own, but only where
                // nothing was written under that name. 14.17 gives tostring
                // the same rule, which this order is already the whole of.
                LhatNativeKind which;
                if (lhat_is_nil(R(a)) &&
                    builtin_member(R(b), R(cc), &which)) {
                    LhatNative *native =
                        lhat_native_new(&m->objects, which, R(b));
                    if (native == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)native));
                }
                break;
            }

            // 02 の 13.10: the position a destructuring bind just read.
            // 04 の 11.3 spells absence nil^, so a position answering nil^
            // is a position the value does not have, and 13.10 makes a count
            // that does not agree an error rather than Lua's list
            // adjustment. Under strict the checker has already said so and
            // the compiler leaves this out; this is where relaxed and an
            // unchecked compile land.
            case LHAT_BC_CHECKPOS: {
                if (lhat_is_nil(R(a))) {
                    return finish(m, chunk, LHAT_RUN_UNPACK_SHORT, lhat_nil(),
                                  at);
                }
                break;
            }

            case LHAT_BC_SETINDEX: {
            set_index:;
                // 05 の 8.9: 'v.x := n' writes the field's bytes in place --
                // the owner register IS the value, so the write lands in the
                // very slots the name holds. Only a registered field takes a
                // write; the members are the host's (8.8's rule holds).
                if (lhat_is_hostvalue(R(a))) {
                    const LhatHostValueField *field = hostvalue_field_named(
                        lhat_as_hostvalue_tag(R(a)), R(b));
                    if (field == NULL ||
                        !hostvalue_field_set(
                            m->slots.values + rbase + a + 1, field, R(cc))) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    break;
                }
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 05 の 8.6改 (M5): the machine's own tables are written by
                // the host, not by an instruction. check.c refuses the ones it
                // can name, but a t^{ … } parameter carries no mark of this,
                // so the same question is asked once more where the write
                // actually happens.
                if (table->sealed) {
                    return finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!set_key(m, table, R(b), R(cc), &refused)) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // nil^ is how 11.3 spells "not there", so it cannot also be
                // a key. Neither can a NaN, which is not equal to itself.
                if (refused) {
                    return finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            // 14.12: the name keeps what it had and gains another way to be
            // called. What was there may already be a group, or the first of
            // two, or nothing when the base did not define it.
            case LHAT_BC_ADDOVERLOAD: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                LhatOverload *group = NULL;
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    group = (LhatOverload *)lhat_as_object(held);
                } else {
                    group = lhat_overload_new(&m->objects);
                    if (group == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    if (!lhat_is_nil(held) && !lhat_overload_add(group, held)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)group), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                if (!lhat_overload_add(group, R(cc))) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // 5.12: `group` may be one already in the table, black since
                // an earlier step. A group gains arms one at a time, so the
                // forward barrier is the cheaper of the two.
                lhat_gc_barrier(m, (LhatObject *)group, R(cc));
                break;
            }

            // 14.12: an override^ replaces the one arm it overlaps, and the
            // arms an overload^ put there are otherwise untouched. A plain
            // write would take the whole group with them.
            case LHAT_BC_OVERRIDEINDEX: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_with_first(
                        &m->objects, group, R(cc));
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                goto set_index;
            }

            // 03 の 5.11c: the same write with the arm named. What it replaces
            // is dropped rather than shadowed, so the arms left are the arms
            // the checker's type says the name carries -- which is what makes
            // an arm index mean the same thing on both sides. super^ is
            // unaffected: it was bound from the old group before this ran.
            case LHAT_BC_OVERRIDEARM: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_replacing(
                        &m->objects, group, cc, R(b + 1));
                    if (made == NULL) {
                        // No such arm. The group is not the one the checker
                        // read, so the honest answer is 14.12's own: put the
                        // replacement in front and let the search sort it out.
                        made = lhat_overload_with_first(&m->objects, group,
                                                        R(b + 1));
                    }
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                // Nothing overloaded under the name, so this is a plain write.
                // set_index cannot be jumped to for it: that reads the value
                // from C, which here is the arm.
                if (table->sealed) {
                    return finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!set_key(m, table, R(b), R(b + 1), &refused)) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                if (refused) {
                    return finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            case LHAT_BC_NEWERROR: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_ERROR_KIND)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatErrorKind *kind =
                    (const LhatErrorKind *)lhat_as_object(R(b));
                LhatError *error = lhat_error_new(&m->objects, kind);
                if (error == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)error));
                break;
            }

            // 04 の 2.6: an error satisfies no type but an error's, so asking
            // whether a value is one is a question about the value alone.
            case LHAT_BC_ISERROR:
                SET_R(a,
                    lhat_bool(lhat_is_object_kind(R(b),
                                                  LHAT_OBJECT_ERROR)));
                break;

            // 02 の 13.11 with 03 の 5.13改: R[C] is always a type the
            // compiler lowered -- a written type, never a runtime value --
            // and lhat_value_satisfies answers, the same relation ASCAST and
            // fits_call already trust. (A fallback that read a shape off a
            // definition table at run time was withdrawn: a value that only
            // arrives while the program runs carries no type to ask about.)
            case LHAT_BC_ISA: {
                LhatValue wanted = R(cc);
                if (!lhat_is_object_kind(wanted, LHAT_OBJECT_TYPE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatRuntimeType *type =
                    (const LhatRuntimeType *)lhat_as_object(wanted);
                SET_R(a,
                    lhat_bool(lhat_value_satisfies(R(b), type)));
                break;
            }

            case LHAT_BC_ISNIL:
                SET_R(a, lhat_bool(lhat_is_nil(R(b))));
                break;

            // 14.3 and 14.7: the instance holds its own fields and reads the
            // shared members through this link. 14.2 fixes it here and gives
            // no way to change it afterwards.
            case LHAT_BC_NEWINSTANCE: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_TABLE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatTable *instance = lhat_table_new(&m->objects);
                if (instance == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                instance->definition =
                    (const LhatTable *)lhat_as_object(R(b));
                SET_R(a, lhat_object((LhatObject *)instance));
                break;
            }

            case LHAT_BC_CALL:
            case LHAT_BC_CALLMETHOD: {
                // 05 の 8.7: the host wrote this one in C. 13.1 settled how
                // many arguments there are before anything ran, so they are
                // handed over as they lie rather than pushed one by one.
                // 04 の 12.8 makes an error a value, so what comes back is
                // one -- there is no unwinding to arrange.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_HOST)) {
                    LhatHost *host = (LhatHost *)lhat_as_object(R(a));
                    size_t skip = op == LHAT_BC_CALLMETHOD ? 2 : 1;

                    // 13.7: 'expr...' wrote a table in the last slot instead
                    // of an ordinary argument. The closure path below unpacks
                    // one into the frame it is about to push; there is no
                    // frame to push here, so it is unpacked into an array of
                    // its own further down.
                    const LhatTable *spread_table = NULL;
                    size_t written = b;
                    if (cc != 0) {
                        if (b == 0) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        spread_table = table_of(R(a + skip + b - 1));
                        if (spread_table == NULL) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        written = (size_t)b - 1 + spread_table->array_count;
                    }

                    // 13.4 keeps self^ out of the parameter list, so what the
                    // call wrote is compared against the list and the
                    // receiver is handed over besides it. 13.7 makes that
                    // comparison a floor once the signature ended in '...'.
                    if (host->has_variadic ? written < host->parameters
                                           : written != host->parameters) {
                        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }

                    // 14.4: the receiver comes first, and sits just below the
                    // arguments the call wrote.
                    bool receiver_first =
                        host->takes_self && op == LHAT_BC_CALLMETHOD;
                    size_t given = written + (receiver_first ? 1 : 0);
                    // 05 の 8.9: a host value receiver holds its width of
                    // slots, so a skipped receiver is skipped whole.
                    size_t receiver_width =
                        op == LHAT_BC_CALLMETHOD && lhat_is_hostvalue(R(a + 1))
                            ? lhat_as_hostvalue_tag(R(a + 1))->width
                            : 1;
                    // 2.2改: the stack holds no LhatValue run any more, so
                    // the arguments a host receives are gathered into one.
                    // 05 の 8.9: walked by slot rather than by value -- a
                    // host value argument is one argument, its head handed
                    // over aiming into the stack, and its width of slots
                    // stepped past.
                    LhatValue gathered[LHAT_MAX_REGISTERS + 2];
                    size_t slot = receiver_first
                                      ? (size_t)a + 1
                                      : (size_t)a +
                                            (op == LHAT_BC_CALLMETHOD
                                                 ? 1 + receiver_width
                                                 : 1);
                    for (size_t i = 0; i < given; i++) {
                        LhatValue held = R(slot);
                        if (lhat_is_hostvalue(held)) {
                            gathered[i] =
                                hostvalue_argument(m->slots, rbase + slot);
                            slot += lhat_as_hostvalue_tag(held)->width;
                        } else {
                            gathered[i] = held;
                            slot += 1;
                        }
                    }
                    const LhatValue *arguments = gathered;

                    // Packed only where a spread broke the contiguity the
                    // registers otherwise have. An allocation rather than the
                    // stack above this frame, because a host function may call
                    // back in and lhat_machine_call starts its frame exactly
                    // there. Nothing in it needs rooting: every value is also
                    // a position of `spread_table` or a register of this
                    // frame, and both stay reachable for as long as the call
                    // does.
                    LhatValue *packed = NULL;
                    if (spread_table != NULL && given > 0) {
                        packed = (LhatValue *)lhat_alloc(given * sizeof *packed);
                        if (packed == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        size_t into = 0;
                        if (receiver_first) {
                            packed[into++] = R(a + 1);
                        }
                        for (size_t i = 0; i < written; i++) {
                            packed[into++] = call_arg(m->slots, rbase, a, skip,
                                                      spread_table,
                                                      (size_t)b - 1, i);
                        }
                        arguments = packed;
                    }

                    // 05 の 8.8: a dispose^ written by hand is the same
                    // giving-back the collection would do, so it is marked
                    // here and 10.7 keeps the sweep from doing it again.
                    if (host->takes_self && given > 0 &&
                        lhat_is_object_kind(arguments[0],
                                            LHAT_OBJECT_HOSTDATA)) {
                        LhatHostData *data =
                            (LhatHostData *)lhat_as_object(arguments[0]);
                        if (data->tag != NULL &&
                            data->tag->release == host->call) {
                            data->released = true;
                        }
                    }
                    LhatValue answered =
                        host->call(m, host->context, arguments, given);
                    lhat_free(packed);
                    // 05 の 8.9: a host value answer arrives as a
                    // head-shaped run and is written out whole on the spot,
                    // before anything else can touch the scratch it may live
                    // in.
                    if (lhat_is_hostvalue(answered)) {
                        if (!place_hostvalue_answer(m, rbase + a, answered)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        break;
                    }
                    SET_R(a, answered);
                    break;
                }

                // 02 の 12.6 and 15.6: resume and dispose are the runtime's,
                // not the program's, so they are performed rather than called.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_NATIVE)) {
                    const LhatNative *native =
                        (const LhatNative *)lhat_as_object(R(a));
                    size_t first = a + (op == LHAT_BC_CALLMETHOD ? 2 : 1);
                    LhatValue sent = b > 0 ? R(first) : lhat_nil();

                    // 05 の 8.6: the one thing a program cannot arrange for
                    // itself. It takes nothing and answers nothing.
                    if (native->kind == LHAT_NATIVE_COLLECTGARBAGE) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                        }
                        lhat_gc_collect(m);
                        SET_R(a, lhat_nil());
                        break;
                    }

                    // 02 の 14.17: the value written down. Takes nothing, or
                    // a format when what it is bound to is a number^ -- the
                    // two signatures 14.12 makes an intersection of, told
                    // apart here by how many arguments arrived.
                    if (native->kind == LHAT_NATIVE_TOSTRING) {
                        if (b > 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        size_t needed;
                        char *text;
                        if (b == 1) {
                            if (!lhat_is_number(native->bound)) {
                                // Only a number^ carries the second
                                // signature, so this call was never one of
                                // the two ways of writing this value.
                                return finish(m, chunk, LHAT_RUN_ARITY,
                                              lhat_nil(), at);
                            }
                            LhatValue fmt = sent;
                            if (!lhat_is_object_kind(fmt, LHAT_OBJECT_STRING)) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            const LhatString *spelt =
                                (const LhatString *)lhat_as_object(fmt);
                            if (!lhat_number_format(native->bound, spelt->text,
                                                    spelt->length, NULL, 0,
                                                    &needed)) {
                                return finish(m, chunk, LHAT_RUN_BAD_FORMAT,
                                              lhat_nil(), at);
                            }
                            text = (char *)lhat_alloc(needed + 1);
                            if (text == NULL) {
                                return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            lhat_number_format(native->bound, spelt->text,
                                               spelt->length, text, needed + 1,
                                               &needed);
                        } else {
                            needed = lhat_value_text(native->bound, NULL, 0);
                            text = (char *)lhat_alloc(needed + 1);
                            if (text == NULL) {
                                return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            lhat_value_text(native->bound, text, needed + 1);
                        }
                        LhatString *written =
                            lhat_string_new(&m->objects, text, needed);
                        lhat_free(text);
                        if (written == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)written));
                        break;
                    }

                    // 02 の 14.17改2: the number^ a string^ names, or nil^
                    // where it names none. Takes nothing, or a format -- the
                    // same two signatures 14.17's takes, told apart the same
                    // way, by how many arguments arrived.
                    if (native->kind == LHAT_NATIVE_TONUMBER) {
                        if (b > 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        // builtin_member only ever binds this to a string^.
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        if (b == 0) {
                            // 01 の 10 章's own grammar, so what tonumber
                            // reads and what L^ reads cannot drift apart.
                            bool is_real = false;
                            int64_t whole = 0;
                            double real = 0.0;
                            if (!lhat_number_read(subject->text,
                                                  subject->length, &is_real,
                                                  &whole, &real)) {
                                SET_R(a, lhat_nil());
                                break;
                            }
                            SET_R(a, is_real ? lhat_real(real)
                                             : lhat_integer(whole));
                            break;
                        }
                        if (!lhat_is_object_kind(sent, LHAT_OBJECT_STRING)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        const LhatString *spelt =
                            (const LhatString *)lhat_as_object(sent);
                        LhatValue number = lhat_nil();
                        bool got = false;
                        if (!lhat_number_scan(subject->text, subject->length,
                                              spelt->text, spelt->length,
                                              &number, &got)) {
                            // 14.17 draws the line in the same place: the
                            // format is the writer's and a bad one is an
                            // error, where the text is data and a text that
                            // does not match is simply not a number^.
                            return finish(m, chunk, LHAT_RUN_BAD_FORMAT,
                                          lhat_nil(), at);
                        }
                        SET_R(a, got ? number : lhat_nil());
                        break;
                    }

                    // 16.3: what `in^` walks. A table answers with a walk of
                    // its keys; a coroutine is already one.
                    if (native->kind == LHAT_NATIVE_ITERATE) {
                        if (lhat_is_object_kind(native->bound,
                                                LHAT_OBJECT_COROUTINE)) {
                            SET_R(a, native->bound);
                            break;
                        }
                        const LhatTable *over = table_of(native->bound);
                        if (over == NULL) {
                            return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(),
                                          at);
                        }
                        LhatCoroutine *walk =
                            lhat_table_iterator(&m->objects, over);
                        if (walk == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                          at);
                        }
                        SET_R(a, lhat_object((LhatObject *)walk));
                        break;
                    }

                    if (!lhat_is_object_kind(native->bound,
                                             LHAT_OBJECT_COROUTINE)) {
                        return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                    }
                    LhatCoroutine *co =
                        (LhatCoroutine *)lhat_as_object(native->bound);

                    // 15.6改: the two questions, answered before any of the
                    // guards below. Neither runs the body, so both hold on a
                    // finished coroutine, where everything else faults, and
                    // on a fresh one, where resume does.
                    if (native->kind == LHAT_NATIVE_DONE ||
                        native->kind == LHAT_NATIVE_STARTED) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                        }
                        SET_R(a, lhat_bool(
                            native->kind == LHAT_NATIVE_DONE
                                ? co->state == LHAT_COROUTINE_DONE
                                : co->state != LHAT_COROUTINE_FRESH));
                        break;
                    }

                    // 15.2改: the machine holds this itself rather than
                    // trusting the checker to have (vm.h's opening comment).
                    // R is one fixed type now, so resume takes exactly one
                    // argument; start takes none, since nothing has been
                    // yield^ed yet to send a value to.
                    if (native->kind == LHAT_NATIVE_START && b != 0) {
                        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME && b != 1) {
                        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }

                    // 15.2改: start and resume now split what the first
                    // resume used to do silently -- each has to be called on
                    // the state that makes it meaningful.
                    if (native->kind == LHAT_NATIVE_START &&
                        co->state != LHAT_COROUTINE_FRESH) {
                        return finish(m, chunk, LHAT_RUN_COROUTINE_ALREADY_STARTED,
                                      lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME &&
                        co->state == LHAT_COROUTINE_FRESH) {
                        return finish(m, chunk, LHAT_RUN_COROUTINE_NOT_STARTED,
                                      lhat_nil(), at);
                    }

                    // 02 の 10.7: disposal runs what is still pending and
                    // never runs the same cleanup twice, so a coroutine that
                    // has finished simply has nothing left to do.
                    bool dispose = native->kind == LHAT_NATIVE_DISPOSE;
                    if (co->state == LHAT_COROUTINE_DONE ||
                        (dispose && co->state == LHAT_COROUTINE_FRESH)) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }
                    if (co->state == LHAT_COROUTINE_RUNNING) {
                        return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }

                    // 16.3: a walk of a table has no body, so nothing below
                    // applies to it -- no frame to enter and nothing pending
                    // to drain. The guards above leave start() on a fresh
                    // walk and resume() on a suspended one, and one step is
                    // the whole of either.
                    if (co->source == LHAT_COROUTINE_TABLE) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        LhatValue stepped = lhat_nil();
                        if (step_table_walk(m, co, &stepped) ==
                            WALK_NO_MEMORY) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, stepped);
                        break;
                    }

                    if (m->frame_count >= LHAT_MAX_FRAMES) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }

                    size_t next_base = rbase + (a) + 1;
                    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }
                    frame->pc = pc;

                    if (dispose) {
                        // 10.7: what is pending runs, innermost first, and
                        // then the coroutine is finished.
                        enter_disposal_frame(m, co, next_base, a, &frame,
                                                                  &rbase, &chunk, &pc);
                        // 15.4: the guards above leave only a suspended
                        // coroutine here, so the slot a yield^ answers into
                        // is written the way a resume writes it -- with
                        // nil^, since dispose sends nothing in.
                        SET_R(co->sent_into, sent);
                        goto drain;
                    }

                    // 5.11: one frame, put back where it left off.
                    for (size_t i = 0; i < co->register_count; i++) {
                        lhat_slots_set(m->slots, next_base + (i), lhat_slots_get(co->registers, i));
                    }
                    reattach_upvalues(m, co, next_base);
                    Frame *called = &m->frames[m->frame_count++];
                    called->closure = co->closure;
                    called->pc = co->pc;
                    called->base = next_base;
                    called->result = a;
                    called->coroutine = co;
                    called->disposing = false;
                    called->derive = LHAT_FRAME_NO_DERIVE;
                    called->returning = false;
                    called->cleanup_count = co->cleanup_count;
                    for (size_t i = 0; i < co->cleanup_count; i++) {
                        called->cleanups[i] = co->cleanups[i];
                    }

                    bool resuming = co->state == LHAT_COROUTINE_SUSPENDED;
                    co->state = LHAT_COROUTINE_RUNNING;

                    frame = called;
                    rbase = frame->base;
                    chunk = &co->closure->proto->chunk;
                    pc = frame->pc;

                    if (resuming) {
                        // 15.4: the value the resume sent arrives where the
                        // yield^ put the one it sent out.
                        SET_R(co->sent_into, sent);
                    }
                    break;
                }

                // 14.12: at most one candidate fits, so this is a search and
                // not a choice -- no ranking, no ambiguity to report. It ends
                // at the first that takes what it was given.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t skip = op == LHAT_BC_CALLMETHOD ? 2 : 1;
                    // 2.2改: fits_call reads the callee and arguments as one
                    // LhatValue run, which the stack no longer is -- so the
                    // slots are gathered once and every candidate reads the
                    // same copy.
                    LhatValue lineup[LHAT_MAX_REGISTERS + 2];
                    for (size_t i = 0; i <= (size_t)b + 1; i++) {
                        lineup[i] = R(a + i);
                    }
                    LhatValue chosen = lhat_nil();
                    for (size_t i = 0; i < group->count; i++) {
                        if (fits_call(group->candidates[i], lineup, b,
                                      op == LHAT_BC_CALLMETHOD, &skip)) {
                            chosen = group->candidates[i];
                            break;
                        }
                    }
                    if (lhat_is_nil(chosen)) {
                        return finish(m, chunk, LHAT_RUN_NO_CANDIDATE, lhat_nil(), at);
                    }
                    SET_R(a, chosen);
                }

                if (!lhat_is_object_kind(R(a), LHAT_OBJECT_SUBROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                const LhatClosure *callee =
                    (const LhatClosure *)lhat_as_object(R(a));
                if (callee->proto == NULL) {
                    return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }

                // 14.4: the receiver sits between the callee and the
                // arguments, and whether it is passed depends on the callee.
                // A member that takes no self^ is a static one (14.4), so the
                // frame simply starts after the receiver.
                size_t given = b;
                size_t skip = 1;
                if (op == LHAT_BC_CALLMETHOD) {
                    if (callee->proto->takes_self) {
                        given = (size_t)b + 1;
                    } else {
                        skip = 2;
                    }
                }
                // 13.7: 'expr...' put a table in the last slot instead of an
                // ordinary argument; its positions are unpacked in place of
                // it, which is why what a call owes is read through
                // call_arg() from here on rather than off registers directly.
                const LhatTable *spread_table = NULL;
                size_t before_spread = given;
                if (cc != 0) {
                    if (given == 0) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    spread_table =
                        table_of(R(a + skip + given - 1));
                    if (spread_table == NULL) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    before_spread = given - 1;
                    given = before_spread + spread_table->array_count;
                }
                // 13.7: the last slot of a variadic callee collects the rest
                // into a table rather than taking one argument for itself, so
                // a call owes at least the fixed count and not exactly the
                // slot count.
                size_t declared_slots = callee->proto->parameters;
                size_t required = callee->proto->has_variadic
                                      ? declared_slots - 1
                                      : declared_slots;
                // 13.4: nothing fills a missing argument in. A parameter's
                // default belongs to the editor that writes the call, so the
                // count owed here is the declared one either way.
                if (callee->proto->has_variadic ? given < required
                                                : given != declared_slots) {
                    return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                }
                // Built before either path below places it, since both read
                // the same arguments the same way.
                LhatValue collected_variadic = lhat_nil();
                if (callee->proto->has_variadic) {
                    LhatTable *collected = lhat_table_new(&m->objects);
                    if (collected == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    for (size_t i = required; i < given; i++) {
                        bool refused = false;
                        LhatValue value = call_arg(m->slots, rbase, a, skip,
                                                   spread_table, before_spread,
                                                   i);
                        if (!set_key(m, collected,
                                     lhat_integer((int64_t)(i - required + 1)),
                                     value, &refused)) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                    }
                    collected_variadic = lhat_object((LhatObject *)collected);
                }

                // 02 の 15.5: calling a yieldable procedure does not suspend
                // the caller. It answers a coroutine, and the body has not
                // started -- which is why the colouring of async/await never
                // arises here.
                if (callee->proto->yields) {
                    LhatCoroutine *co =
                        lhat_coroutine_new(&m->objects, callee,
                                           callee->proto->chunk.registers);
                    if (co == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    size_t fixed = callee->proto->has_variadic ? required : given;
                    for (size_t i = 0; i < fixed; i++) {
                        lhat_slots_set(co->registers, i,
                                       call_arg(m->slots, rbase, a, skip,
                                                spread_table, before_spread,
                                                i));
                    }
                    if (callee->proto->has_variadic) {
                        lhat_slots_set(co->registers, required, collected_variadic);
                    }
                    SET_R(a, lhat_object((LhatObject *)co));
                    break;
                }

                if (m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 5.3: the arguments already sit just above the callee, so
                // the new frame starts there and needs no shuffling -- unless
                // a spread broke the contiguity, or 13.7's collector has to
                // overwrite the slot after the fixed ones with the table just
                // built.
                size_t next_base = rbase + a + skip;
                if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                if (spread_table != NULL) {
                    size_t fixed = callee->proto->has_variadic ? required
                                                               : given;
                    for (size_t i = 0; i < fixed; i++) {
                        lhat_slots_set(m->slots, next_base + i,
                                       call_arg(m->slots, rbase, a, skip,
                                                spread_table, before_spread,
                                                i));
                    }
                }
                if (callee->proto->has_variadic) {
                    lhat_slots_set(m->slots, next_base + (required), collected_variadic);
                }

                frame->pc = pc;
                Frame *called = &m->frames[m->frame_count++];
                called->closure = callee;
                called->pc = 0;
                called->base = next_base;
                called->result = a;
                called->cleanup_count = 0;  // 5.5: pending cleanups are per frame
                called->returning = false;
                called->coroutine = NULL;
                called->disposing = false;
                called->derive = LHAT_FRAME_NO_DERIVE;

                frame = called;
                rbase = frame->base;
                chunk = &callee->proto->chunk;
                pc = 0;
                break;
            }

            case LHAT_BC_PUSHCLEANUP:
                if (frame->cleanup_count >= LHAT_MAX_CLEANUPS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                frame->cleanups[frame->cleanup_count++] = lhat_bx(instruction);
                break;

            // 10.2 and 12.3: leaving the block runs what it entered. The two
            // cases differ only in where control goes afterwards.
            case LHAT_BC_POPCLEANUP:
                frame->drain_target = a;
                frame->resume = pc;
                frame->returning = false;
                goto drain;

            case LHAT_BC_ENDCLEANUP:
                goto drain;

            // 10.4: leaving the procedure leaves every block inside it, so
            // the drain runs everything still pending before the frame goes.
            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL:
                frame->drain_target = 0;
                frame->returning = true;
                frame->answer = op == LHAT_BC_RETURN ? R(a) : lhat_nil();
                // 05 の 8.9: a host value answer moves into the frame's own
                // room before the drain -- the callee's window overlaps the
                // caller's scratch, so no register survives the cleanups.
                // The pop places it whole from there. A coroutine's own
                // return crossing a suspension was refused by the checker;
                // this is the backstop.
                if (op == LHAT_BC_RETURN && lhat_is_hostvalue(R(a))) {
                    if (frame->coroutine != NULL ||
                        m->frame_count <= base_depth + 1) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    const LhatHostValueTag *tag = lhat_as_hostvalue_tag(R(a));
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = m->slots.values[rbase + a + i];
                    }
                    frame->answer.as.hostvalue_run = frame->answer_run;
                }
                goto drain;

            // 04 の 11.6改: unlike a fault the machine itself raises, the
            // value is the program's own -- carried through to the host
            // exactly as lhat_run always has, rather than discarded like
            // every other finish() here discards its nil^.
            case LHAT_BC_PANIC:
                return finish(m, chunk, LHAT_RUN_PANIC, R(a), at);

            // 11.6, S27: 14.12's own runtime check (fits_call already
            // trusts it for overload^ resolution), just asked once instead
            // of per candidate. Compile-time disjointness (check.c) already
            // ruled out what could never hold; this is what it could not
            // rule out, checked against the actual value.
            case LHAT_BC_ASCAST: {
                const LhatRuntimeType *wanted =
                    (const LhatRuntimeType *)lhat_as_object(R(b));
                if (!lhat_value_satisfies(R(a), wanted)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                break;
            }

            // 02 の 15.4: the frame stops here and the value goes out. 5.11
            // keeps the one frame rather than a stack, which 15.5 is what
            // makes possible -- a yield^ is always in the body it suspends.
            case LHAT_BC_YIELD: {
                LhatCoroutine *co = frame->coroutine;
                // 10.7: nothing is waiting for a yield^ during disposal.
                if (co == NULL || frame->disposing) {
                    return finish(m, chunk, LHAT_RUN_YIELD_OUTSIDE, lhat_nil(), at);
                }
                LhatValue value = R(a);

                for (size_t i = 0; i < co->register_count; i++) {
                    lhat_slots_set(co->registers, i, R(i));
                    // 5.12: the coroutine has just taken a whole frame's
                    // worth of the stack into itself. The backward barrier,
                    // as for a table: one more visit to the coroutine costs
                    // less than marking every register as it is copied.
                    lhat_gc_barrier_back(m, (LhatObject *)co, R(i));
                }
                // 15.4 with 5.4: the captures of this frame's slots travel
                // with the registers, or a resume at another depth would
                // leave them reading whatever frame took these addresses
                // over. This frame is the top one, so its captures are the
                // head of the machine's descending open list; popped highest
                // first and prepended, the coroutine's own list comes out
                // ascending, which is what reattach_upvalues expects.
                const LhatValueUnion *suspending = m->slots.values + rbase;
                while (m->open != NULL &&
                       m->open->location.value >= suspending) {
                    LhatUpvalue *up = m->open;
                    m->open = up->next_open;
                    size_t offset = (size_t)(up->location.value - suspending);
                    up->location = lhat_slots_ref(co->registers, offset);
                    up->suspended_in = co;
                    up->next_open = co->open;
                    co->open = up;
                    // 5.12: the capture now keeps the coroutine alive
                    // (gc.c), which a black upvalue has to declare.
                    lhat_gc_barrier(m, (LhatObject *)up,
                                    lhat_object((LhatObject *)co));
                }
                co->pc = pc;
                co->sent_into = a;
                co->state = LHAT_COROUTINE_SUSPENDED;
                if (frame->cleanup_count > LHAT_COROUTINE_CLEANUPS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                co->cleanup_count = frame->cleanup_count;
                for (size_t i = 0; i < frame->cleanup_count; i++) {
                    co->cleanups[i] = frame->cleanups[i];
                }

                uint8_t into = frame->result;
                m->frame_count--;
                frame = &m->frames[m->frame_count - 1];
                rbase = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                SET_R(into, value);
                break;
            }

            // 02 の 15.8: the delegation loop asks whether the inner one is
            // finished, since 13.9 makes what a resume answers the union of
            // its yield type and its return type.
            case LHAT_BC_ISDONE: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatCoroutine *co =
                    (const LhatCoroutine *)lhat_as_object(R(b));
                SET_R(a, lhat_bool(co->state == LHAT_COROUTINE_DONE));
                break;
            }

            case LHAT_BC_RESUME: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatCoroutine *co =
                    (LhatCoroutine *)lhat_as_object(R(b));
                if (co->state == LHAT_COROUTINE_DONE ||
                    co->state == LHAT_COROUTINE_RUNNING) {
                    return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                }

                // 16.3: a table's walk has no body to enter, so resuming it
                // is one step and nothing more.
                if (co->source == LHAT_COROUTINE_TABLE) {
                    LhatValue stepped = lhat_nil();
                    if (step_table_walk(m, co, &stepped) ==
                        WALK_NO_MEMORY) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, stepped);
                    break;
                }
                if (m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                size_t next_base = rbase + (a) + 1;
                if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                LhatValue sent = R(a);
                bool resuming = co->state == LHAT_COROUTINE_SUSPENDED;
                for (size_t i = 0; i < co->register_count; i++) {
                    lhat_slots_set(m->slots, next_base + (i), lhat_slots_get(co->registers, i));
                }
                reattach_upvalues(m, co, next_base);
                frame->pc = pc;
                Frame *called = &m->frames[m->frame_count++];
                called->closure = co->closure;
                called->pc = co->pc;
                called->base = next_base;
                called->result = a;
                called->coroutine = co;
                called->disposing = false;
                called->derive = LHAT_FRAME_NO_DERIVE;
                called->returning = false;
                called->cleanup_count = co->cleanup_count;
                for (size_t i = 0; i < co->cleanup_count; i++) {
                    called->cleanups[i] = co->cleanups[i];
                }
                co->state = LHAT_COROUTINE_RUNNING;

                frame = called;
                rbase = frame->base;
                chunk = &co->closure->proto->chunk;
                pc = frame->pc;
                if (resuming) {
                    SET_R(co->sent_into, sent);
                }
                break;
            }

            case LHAT_BC_COUNT:
                return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        continue;

    // 02 の 11.1: an operator is a function the left operand carries, named
    // by 11.8 after the operator itself. The instructions above take their
    // own types directly and come here for everything else, which is why the
    // built-in cases pay nothing for this.
    call_operator: {
        size_t length = 0;
        const char *name = operator_name(op, &length);
        // 14.4 makes an operator a method: the left operand is the receiver
        // and the right one the single argument.
        LhatValue found = lhat_nil();
        OperatorLookup answer = operator_candidate(m, R(b), name, length, R(b),
                                                   R(cc), false, &found);
        // 11.3改 (S39): the left carries nothing that takes this right
        // operand, so the right one is asked whether it was written as the
        // receiver instead. This is what lets a value join an operation whose
        // left operand is a built-in, which can carry no answer for it.
        if (answer == OPERATOR_ABSENT || answer == OPERATOR_NO_CANDIDATE) {
            LhatValue other = lhat_nil();
            OperatorLookup right = operator_candidate(
                m, R(cc), name, length, R(cc), R(b), true, &other);
            if (right == OPERATOR_PICKED || right == OPERATOR_NO_MEMORY) {
                found = other;
                answer = right;
            }
        }
        if (answer == OPERATOR_NO_MEMORY) {
            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
        }
        // 11.9 (S40): equality is answered whether or not a '<=>' was
        // written -- 14.2 says what a table is the same as, and a type that
        // orders itself only refines that. An ordering has no such answer to
        // fall back on and faults the way it always did.
        if (answer != OPERATOR_PICKED &&
            (derive_from == LHAT_BC_EQ || derive_from == LHAT_BC_NE)) {
            SET_R(a, lhat_bool(lhat_value_equal(R(b), R(cc)) ==
                               (derive_from == LHAT_BC_EQ)));
            continue;  // the label sits in the loop, not in the switch
        }
        if (answer == OPERATOR_NO_CANDIDATE) {
            return finish(m, chunk, LHAT_RUN_NO_CANDIDATE, lhat_nil(), at);
        }
        if (answer != OPERATOR_PICKED) {
            return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        // 05 の 8.9: a host value's operator is C and answers on the spot --
        // no frame, no derive to carry: the receiver convention is 14.4's,
        // with each operand handed over as its head aimed into the stack.
        if (lhat_is_object_kind(found, LHAT_OBJECT_HOST)) {
            LhatHost *carried_host = (LhatHost *)lhat_as_object(found);
            LhatValue operands[2];
            operands[0] = lhat_is_hostvalue(R(b))
                              ? hostvalue_argument(m->slots, rbase + b)
                              : R(b);
            operands[1] = lhat_is_hostvalue(R(cc))
                              ? hostvalue_argument(m->slots, rbase + cc)
                              : R(cc);
            LhatValue answered =
                carried_host->call(m, carried_host->context, operands, 2);
            if (derive_from != LHAT_FRAME_NO_DERIVE) {
                // 11.9 (S40): what came back is read against zero.
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (derive_from == LHAT_BC_EQ || derive_from == LHAT_BC_NE) {
                    held = lhat_value_equal(answered, lhat_integer(0)) ==
                           (derive_from == LHAT_BC_EQ);
                } else if (!ordering(derive_from, answered, lhat_integer(0),
                                     &held, &status)) {
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(held));
                continue;
            }
            if (lhat_is_hostvalue(answered)) {
                if (!place_hostvalue_answer(m, rbase + a, answered)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
                continue;
            }
            SET_R(a, answered);
            continue;
        }
        const LhatClosure *carried =
            (const LhatClosure *)lhat_as_object(found);
        // 15.7改: an operator may not be yieldable. Not because it is an f^ --
        // 15.3改 lets one of those suspend -- but because 11.8's signature
        // answers T, and a yieldable subroutine answers a coroutine (15.5).
        if (carried->proto == NULL || carried->proto->yields) {
            return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        if (m->frame_count >= LHAT_MAX_FRAMES) {
            return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }

        // 5.3 wants the arguments in a contiguous run, and b and cc need not
        // be one. 14.4 puts the left operand in self^, so it leads: the frame
        // is laid out just past where the answer goes, the way a native call
        // lays one out.
        //
        // 11.3改 (S39): and a self^-last one needs nothing else here. Its
        // parameter list is written in operand order too -- the left operand
        // first, the self^ after it -- so the same two slots hold the same two
        // values whichever side the receiver is. Only which slot the body
        // calls self^ differs, and that is the body's own business.
        size_t next_base = rbase + (a) + 1;
        if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
            return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }
        lhat_slots_set(m->slots, next_base + (0), R(b));
        lhat_slots_set(m->slots, next_base + (1), R(cc));

        frame->pc = pc;
        Frame *entered = &m->frames[m->frame_count++];
        entered->closure = carried;
        entered->pc = 0;
        entered->base = next_base;
        entered->result = a;
        entered->cleanup_count = 0;
        entered->returning = false;
        entered->coroutine = NULL;
        entered->disposing = false;
        // 11.9 (S40): an ordering that reached for '<=>' wants the answer
        // read against zero, not handed over as it is.
        entered->derive = derive_from;

        frame = entered;
        rbase = frame->base;
        chunk = &carried->proto->chunk;
        pc = 0;
        continue;
    }

    drain:
        // Innermost first, one at a time: each body ends with ENDCLEANUP,
        // which comes back here for the next.
        if (frame->cleanup_count > frame->drain_target) {
            pc = frame->cleanups[--frame->cleanup_count];
            continue;
        }
        if (!frame->returning) {
            pc = frame->resume;
            continue;
        }

        // 5.4: whatever still points into this frame takes its value with it,
        // since the slots are about to be reused.
        {
            LhatValue value = frame->answer;
            // 5.11: the body is over, so the coroutine has nothing left to
            // resume and its cleanups have all run.
            if (frame->coroutine != NULL) {
                frame->coroutine->state = LHAT_COROUTINE_DONE;
                frame->coroutine->cleanup_count = 0;
            }

            // 03 の 4.3: a session's top-level slots outlive the input, so
            // what points into them goes on sharing them. Closing those here
            // would undo the CLOSE compile_session_statements deliberately
            // does not write, and a later input's ':=' through a closure
            // would land in a private copy instead of the slot the next
            // input reads. `kept` is zero everywhere else, which leaves an
            // ordinary frame closed whole the way it always was.
            const LhatProto *ran = frame->closure->proto;
            close_upvalues(m, frame->base + (ran != NULL ? ran->kept : 0));

            // 02 の 10.7: a coroutine dropped in the last few instructions
            // never met a collection, and without one here whether its
            // finally^ runs comes down to whether the heap happened to fill
            // up in time. The frame is still counted, which is what makes
            // this safe -- its registers and its answer are still roots, so
            // this asks the same question every other collection asks and
            // not a wider one. What the program is still holding when the
            // run ends is still held; the run ending is not the machine
            // ending, and lhat_run's own start is what lets the next one go.
            //
            // `ending` says the queue is the only thing keeping the run
            // open. The loop takes one coroutine per turn and comes straight
            // back here, and once nothing is left the frame goes for real.
            // Once only (`end_swept`): a cleanup that drops something has
            // already had its own chance, and the end of a run must not be
            // something a program can go on extending.
            if (m->frame_count == base_depth + 1 && !end_swept) {
                end_swept = true;
                lhat_gc_collect(m);
                if (m->pending_dispose != NULL) {
                    ending = true;
                    pc = at;
                    continue;
                }
            }
            m->frame_count--;

            if (m->frame_count == base_depth) {
                // 05 の 8.9: the checker refused a host value as the
                // program's answer; a relaxed run that got here anyway has
                // nothing meaningful to hand the host.
                if (lhat_is_hostvalue(value)) {
                    value = lhat_nil();
                }
                return finish(m, chunk, LHAT_RUN_OK, value, at);
            }

            uint8_t into = frame->result;
            // 02 の 11.9 (S40): the one frame whose answer is not the value of
            // the expression that made it. An ordering that reached for '<=>'
            // asked a number^ of it, and what was written asks which side of
            // zero that falls on.
            LhatOpcode derived = frame->derive;
            frame = &m->frames[m->frame_count - 1];
            rbase = frame->base;
            chunk = &frame->closure->proto->chunk;
            pc = frame->pc;
            if (derived != LHAT_FRAME_NO_DERIVE) {
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (derived == LHAT_BC_EQ || derived == LHAT_BC_NE) {
                    held = lhat_value_equal(value, lhat_integer(0)) ==
                           (derived == LHAT_BC_EQ);
                } else if (!ordering(derived, value, lhat_integer(0), &held,
                                     &status)) {
                    // 11.9: the shape rule asks an op^<=> for a number^, so
                    // this is a body that answered with something else.
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                value = lhat_bool(held);
            }
            // 05 の 8.9: a host value answer rides the frame's own room
            // through the drain (see the RETURN case), and is written out
            // whole here, into the caller's slots -- which are live again
            // now that the callee's window is gone.
            if (lhat_is_hostvalue(value)) {
                if (!place_hostvalue_answer(m, rbase + into, value)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
            } else {
                SET_R(into, value);
            }
        }
    }

    return finish(m, chunk, LHAT_RUN_OK, lhat_nil(), chunk->count);
}

LhatRunResult lhat_run(LhatMachine *m, const LhatProto *proto)
{
    const LhatChunk *chunk = &proto->chunk;

    // 5.4 and 5.2: the frames and the registers belong to the run, so each
    // starts with neither. What the heap holds is the machine's and stays.
    //
    // 03 の 4.3: except the registers the unit says are already spoken for.
    // A REPL's second input finds the top-level names of the first there,
    // and clearing them would be clearing the session.
    // 5.4: what an earlier run left sharing a slot this one is about to
    // clear takes its value away first. The slots below `reserved` are the
    // session's and survive, so what points into them stays open and shared
    // -- that is what lets a closure an earlier input made go on naming the
    // very place a later one reads and writes.
    m->frame_count = 0;
    close_upvalues(m, proto->reserved);
    for (size_t i = proto->reserved; i < LHAT_STACK_SLOTS; i++) {
        lhat_slots_set(m->slots, i, lhat_nil());
    }

    LhatClosure *entry =
        (LhatClosure *)lhat_object_alloc(&m->objects, sizeof *entry,
                                        LHAT_OBJECT_SUBROUTINE);
    if (entry == NULL) {
        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
    }
    entry->proto = proto;

    Frame *frame = &m->frames[m->frame_count++];
    frame->closure = entry;
    frame->pc = 0;
    frame->base = 0;
    frame->result = 0;
    frame->cleanup_count = 0;
    frame->returning = false;
    frame->coroutine = NULL;
    frame->disposing = false;
    frame->derive = LHAT_FRAME_NO_DERIVE;
    frame->answer = lhat_nil();

    return run_frames(m, 0);
}

// A LhatRunResult for a call that never got as far as pushing a frame -- no
// chunk exists yet to name a line or an operator from, so both come back
// empty the way finish() already leaves them for `at` out of range.
static LhatRunResult call_fault(Machine *m, LhatRunStatus status)
{
    LhatRunResult result;
    result.status = status;
    result.value = lhat_nil();
    result.at = 0;
    result.line = 0;
    result.op_name = NULL;
    result.op_name_length = 0;
    result.collected = m->collected;
    result.live = m->objects.count;
    return result;
}

LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count)
{
    Machine *m = (Machine *)machine;
    size_t base = m->frame_count;

    // 05 の 8.7's LhatHostFn has no shape for a receiver or a spread; a host
    // calling back in already has its arguments as a flat array, so this is
    // the plain-call subset of what LHAT_BC_CALL does (vm.c's CALL case).
    if (!lhat_is_object_kind(callee, LHAT_OBJECT_SUBROUTINE)) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    const LhatClosure *closure = (const LhatClosure *)lhat_as_object(callee);
    // 02 の 15.5: a yieldable subroutine answers a coroutine rather than
    // running -- there is nowhere here to hand that back as a distinct kind
    // of success, so a host reaching for one of these gets NOT_CALLABLE.
    if (closure->proto == NULL || closure->proto->yields) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }

    size_t declared_slots = closure->proto->parameters;
    size_t required =
        closure->proto->has_variadic ? declared_slots - 1 : declared_slots;
    if (closure->proto->has_variadic ? count < required
                                     : count != declared_slots) {
        return call_fault(m, LHAT_RUN_ARITY);
    }

    if (base >= LHAT_MAX_FRAMES) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }

    // Mirrors 6249's "just past what the caller's frame uses" -- a native
    // caller has no R(a) of its own to measure from, so the frame
    // already on top (if there is one) stands in for it. base == 0 is
    // lhat_run's own case: nothing is on the stack yet, so frame 0 starts at
    // its beginning.
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }

    for (size_t i = 0; i < required; i++) {
        lhat_slots_set(m->slots, next_base + (i), arguments[i]);
    }
    if (closure->proto->has_variadic) {
        LhatTable *collected = lhat_table_new(&m->objects);
        if (collected == NULL) {
            return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
        }
        for (size_t i = required; i < count; i++) {
            bool refused = false;
            if (!set_key(m, collected,
                         lhat_integer((int64_t)(i - required + 1)),
                         arguments[i], &refused)) {
                return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
            }
        }
        lhat_slots_set(m->slots, next_base + (required), lhat_object((LhatObject *)collected));
    }

    Frame *called = &m->frames[m->frame_count++];
    called->closure = closure;
    called->pc = 0;
    called->base = next_base;
    called->result = 0;  // never read: base_depth's drain returns instead
    called->cleanup_count = 0;
    called->returning = false;
    called->coroutine = NULL;
    called->disposing = false;
    called->derive = LHAT_FRAME_NO_DERIVE;
    called->answer = lhat_nil();

    return run_frames(m, base);
}

bool lhat_machine_make_string(LhatMachine *machine, const char *text,
                              size_t length, LhatValue *out)
{
    Machine *m = (Machine *)machine;
    LhatString *string = lhat_string_new(&m->objects, text, length);
    if (string == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)string);
    return true;
}

bool lhat_machine_make_closure(LhatMachine *machine, const LhatProto *proto,
                               LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || proto->upvalue_count != 0) {
        return false;
    }
    LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
        &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
    if (closure == NULL) {
        return false;
    }
    closure->proto = proto;
    // calloc 起源(lhat_object_alloc)なので upvalues/upvalue_count は
    // 既に NULL/0 -- proto->upvalue_count == 0 の前提と一致する。
    *out = lhat_object((LhatObject *)closure);
    return true;
}

void lhat_machine_modules(const LhatMachine *machine,
                          const LhatModule **out_modules, size_t *out_count)
{
    const Machine *m = (const Machine *)machine;
    if (m == NULL) {
        if (out_modules != NULL) *out_modules = NULL;
        if (out_count != NULL) *out_count = 0;
        return;
    }
    if (out_modules != NULL) *out_modules = m->modules;
    if (out_count != NULL) *out_count = m->module_count;
}

bool lhat_machine_make_error(LhatMachine *machine, const LhatErrorKind *kind,
                             const char *message, LhatValue cause,
                             LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || kind == NULL) {
        return false;
    }
    // Same shape LHAT_BC_NEWERROR builds, and the same field defaults
    // compile_error_new writes for what a construction left out (04 の
    // 2.3): every kind has message and cause without either being declared.
    LhatError *error = lhat_error_new(&m->objects, kind);
    if (error == NULL) {
        return false;
    }
    LhatString *message_key = lhat_string_new(&m->objects, "message", 7);
    LhatString *message_text = lhat_string_new(
        &m->objects, message != NULL ? message : "",
        message != NULL ? strlen(message) : 0);
    if (message_key == NULL || message_text == NULL) {
        return false;
    }
    bool refused = false;
    if (!set_key(m, error->fields, lhat_object((LhatObject *)message_key),
                 lhat_object((LhatObject *)message_text), &refused) ||
        refused) {
        return false;
    }
    if (!lhat_is_nil(cause)) {
        LhatString *cause_key = lhat_string_new(&m->objects, "cause", 5);
        if (cause_key == NULL) {
            return false;
        }
        if (!set_key(m, error->fields, lhat_object((LhatObject *)cause_key),
                     cause, &refused) ||
            refused) {
            return false;
        }
    }
    *out = lhat_object((LhatObject *)error);
    return true;
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_NOT_CALLABLE:    return "this is not a subroutine";
        case LHAT_RUN_ARITY:           return "the wrong number of arguments";
        case LHAT_RUN_STACK_OVERFLOW:  return "the calls went too deep";
        case LHAT_RUN_OUT_OF_MEMORY:   return "out of memory";
        case LHAT_RUN_BAD_KEY:         return "this cannot be a key";
        case LHAT_RUN_UNPACK_SHORT:
            return "this value has no such position to take apart";
        case LHAT_RUN_SEALED:
            return "this table belongs to the machine; what it holds is "
                   "written by the host, not from here";
        case LHAT_RUN_BAD_FORMAT:
            return "a number^ is written through one numeric conversion; "
                   "write '%d' or '%g' and no length of your own";
        case LHAT_RUN_DEAD_COROUTINE:  return "this coroutine has finished";
        case LHAT_RUN_NO_SUCH_UNIT:
            return "this machine was not given the unit this require^ asks for";
        case LHAT_RUN_YIELD_OUTSIDE:   return "nothing is waiting for this yield^";
        case LHAT_RUN_NO_CANDIDATE:    return "no way of calling this member takes these arguments";
        case LHAT_RUN_COROUTINE_NOT_STARTED:     return "resume needs start() first";
        case LHAT_RUN_COROUTINE_ALREADY_STARTED: return "start() only works before the first resume";
        // 04 の 11.6改: a placeholder for a caller that only wants this
        // status's own name -- the actual message is what the program
        // panicked with, in LhatRunResult.value, which this cannot see.
        case LHAT_RUN_PANIC:                     return "panic^";
    }
    return "unknown";
}
