// L^ (lhat) -- compiling a tree to bytecode.

#include "compile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/config.h"
#include "lhat/port.h"
#include "rttype.h"
#include "type.h"

// 9.8: break^ is a normal end for the loop it leaves, so its jump lands where
// last^ and epilog^ are, not past them. The chain is what a break^ written
// inside a nested block still finds.
typedef struct LoopContext {
    struct LoopContext *enclosing;
    size_t jumps[LHAT_MAX_BREAKS];
    size_t count;
    // 9.11: and where a next^ lands -- the loop's own advance, which is what
    // the end of the body falls into. Kept apart from the jumps above
    // because the two are patched at different places: one past the loop,
    // one just short of the step that ends the turn.
    size_t nexts[LHAT_MAX_BREAKS];
    size_t next_count;
    size_t cleanup_depth;  // what a break^ has to drain back down to
} LoopContext;

// 04 の 4.5: the try^{ } being compiled. A try^ inside its body leaves for
// the arms rather than for the caller, which is the same shape as break^
// above -- a jump to be patched, and the cleanups opened since to drain on
// the way. `caught` is where the error waits while the arms are chosen; it^
// names that register inside each of them.
typedef struct TryContext {
    struct TryContext *enclosing;
    size_t jumps[LHAT_MAX_BREAKS];
    size_t count;
    size_t cleanup_depth;
    uint8_t caught;
} TryContext;

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
    // 05 の 8.7: the root an import^ bound, which a nested body reads off
    // L^.modules rather than capturing. See resolve_name. A require^ root
    // wearing the same name clears this: a unit's value was made by running
    // it and is in no registry to read back.
    bool import_root;
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
    // Shared, so the first failure sticks -- and carries where it was, which
    // is what a hole in the checker has to be found by (03 の 4.2).
    LhatCompileResult *result;

    Local locals[LHAT_MAX_LOCALS];
    size_t local_count;

    struct {
        const char *name;
        size_t length;
        // 01 の 2.3: how many inner bindings of the name this capture
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
    // The other two are the rest of that node's position, kept for the same
    // reason one line down: a failure says where it was without the site
    // that reports it holding the node.
    uint32_t line;
    uint32_t offset;
    uint32_t column;

    LoopContext *loop;  // the innermost loop being compiled, NULL outside one
    TryContext *trying;  // 04 の 4.5: and the innermost try^{ }

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

    // 5.3: the one call about to be compiled stands where this frame has
    // nothing left to do, so it may run in this frame rather than one above
    // it. Read and cleared at the head of compile_call_wide -- an argument
    // that is itself a call is not the one in tail position.
    bool tail_call;
    bool tail_drop;  // and its answer is thrown away (a bare call statement)
    // The last statement of the body being compiled, when it is a bare call.
    // 5.3 reads a call there as a tail call, the way it reads the value of a
    // return^: what follows it is the end of the body.
    const LhatNode *tail_statement;

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

// Where the compiler stands: the statement or expression it is under, which
// compile_statement and compile_expression put here as they go. A failure
// takes its position from this, so the 100-odd fail() sites say where they
// are without every one of them carrying a node.
static void fail(Compiler *c, LhatCompileStatus status)
{
    if (c->result->status != LHAT_COMPILE_OK) {
        return;
    }
    c->result->status = status;
    c->result->offset = c->offset;
    c->result->line = c->line;
    c->result->column = c->column;
}

// The same, for a status that is about a name -- "no such name" is worth
// more when it says which. The name is a span of the source the unit was
// read from, which outlives the compile, exactly as the checker's is.
static void fail_named(Compiler *c, LhatCompileStatus status, const char *name,
                       size_t length)
{
    bool first = c->result->status == LHAT_COMPILE_OK;
    fail(c, status);
    if (first) {
        c->result->name = name;
        c->result->name_length = (uint32_t)length;
    }
}

// ast.c's canonical-name reading (01 の 2.3), against this unit's source --
// and its string storage, where 3.1's backticked names are spelled.
static bool node_name(const Compiler *c, const LhatNode *node,
                      const char **text, size_t *length)
{
    return lhat_node_name(node, c->lexer->source->text, c->lexer->strings,
                          text, length);
}

// 02 の 13.12: '_^' stands where a name would and binds nothing, so no local
// is made for it and nothing is written back into one. The value is still
// evaluated -- what is thrown away is the place to read it from.
static bool node_is_discard(const Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           node_name(c, node, &name, &length) &&
           lhat_name_is(name, length, "_^");
}

static bool name_is(const char *text, size_t length, const char *literal)
{
    return lhat_name_is(text, length, literal);
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

// 02 の 13.8改: how many values this expression answers with, or 0 when it is
// not a tuple. Read off the checker's stamp the way hostvalue_of reads a host
// value's -- an unchecked compile sees 0 and takes the ordinary one-slot
// path, which then faults at run time rather than laying out a run nobody
// reserved (03 の 4.2: what runs must not depend on whether checking did).
static size_t tuple_width_of(const LhatNode *node)
{
    if (node == NULL || node->checked_type == NULL) {
        return 0;
    }
    return lhat_type_tuple_width((const LhatType *)node->checked_type);
}

// 02 の 13.8改: the forms a tuple can arrive through -- a call, or a call
// with 04 の 5.1's try^ around it. try^ is transparent here because the head
// slot's tag is what tells the error arm from the value arm, so the same run
// serves both.
static bool is_run_source(const LhatNode *node)
{
    if (node == NULL) {
        return false;
    }
    // 13.8改: a catch^ or a '??' answers a run when both its arms do --
    // which the type settles, since the union of two tuples of the same
    // width folds into one tuple. 04 の 4.1 and 11.7 make the two the same
    // shape (drop one arm, put a value in its place), so they are the same
    // here too. A written '(a, b)' is a run outright.
    if (node->kind == LHAT_NODE_BINARY &&
        (node->v.binary.op == LHAT_OP_CATCH ||
         node->v.binary.op == LHAT_OP_NIL_ELSE)) {
        return true;
    }
    return node->kind == LHAT_NODE_CALL || node->kind == LHAT_NODE_TRY ||
           node->kind == LHAT_NODE_TUPLE;
}

static void compile_run_source(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved);

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
// `*skip` bindings of the name -- 01 の 2.3's stacked reach, where it^^
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

// 15.10: this^^ is the subroutine enclosing the one running --
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
// 02 の 13.8改: `reserved` is how many consecutive slots the answer is to be
// written into -- 0 and 1 both mean the ordinary one.
static void compile_call_wide(Compiler *c, const LhatNode *node, uint8_t into,
                              size_t reserved);
static void compile_try_wide(Compiler *c, const LhatNode *node, uint8_t into,
                             size_t reserved);
static void compile_catch_wide(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved);
static void compile_nil_else_wide(Compiler *c, const LhatNode *node,
                                  uint8_t into, size_t reserved);
static void compile_tuple_literal(Compiler *c, const LhatNode *node,
                                  uint8_t into, size_t positions);

// Lays a tuple answer into the run at `into`. Only called where is_run_source
// said yes.
static void compile_run_source(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved)
{
    if (node->kind == LHAT_NODE_TRY) {
        compile_try_wide(c, node, into, reserved);
    } else if (node->kind == LHAT_NODE_TUPLE) {
        compile_tuple_literal(c, node, into, reserved > 0 ? reserved - 1 : 0);
    } else if (node->kind == LHAT_NODE_BINARY) {
        // 04 の 4.1 and 11.7: the same shape, asking about a different
        // unwanted half -- an error for catch^, nil^ for .??..
        if (node->v.binary.op == LHAT_OP_NIL_ELSE) {
            compile_nil_else_wide(c, node, into, reserved);
        } else {
            compile_catch_wide(c, node, into, reserved);
        }
    } else {
        compile_call_wide(c, node, into, reserved);
    }
}
static void compile_statement(Compiler *c, const LhatNode *node);
static void compile_statements(Compiler *c, const LhatNode *statements);
static void emit_cleanup_drain(Compiler *c, size_t down_to);
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
        // 01 の 3.3: id^name is the name's spelling, which is a key like any
        // other written one -- the same bytes node_name answers for the name
        // written bare.
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_NAME: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            load_string_bytes(c, into, name, length);
            return;
        }
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
// 02 の 13.8改: `reserved` widens the answer the way it does for a call --
// '(A, B)|SomeError' reserves one head slot plus the positions, and the two
// arms are told apart by the tag that lands in the head. So ISERROR below
// reads exactly what it always read, and the error arm still travels as one
// value: the RETURN carrying it out is narrow (B stays 0).
// 04 の 5.1 with 4.5: where an error found by a try^ goes. Out of the frame
// when nothing stands between here and the caller, and to the arms of the
// try^{ } that does otherwise -- draining what it opened on the way, the
// same as break^ leaving the loops it passes through.
static void emit_error_escape(Compiler *c, uint8_t from)
{
    TryContext *target = c->trying;
    if (target == NULL) {
        emit(c, lhat_encode_abc(LHAT_BC_RETURN, from, 0, 0));
        return;
    }
    if (target->count >= LHAT_MAX_BREAKS) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    if (target->caught != from) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, target->caught, from, 0));
    }
    emit_cleanup_drain(c, target->cleanup_depth);
    target->jumps[target->count++] = emit_jump(c, LHAT_BC_JUMP, 0);
}

static void compile_try_wide(Compiler *c, const LhatNode *node, uint8_t into,
                             size_t reserved)
{
    const LhatNode *operand = node->v.jump.value;
    if (reserved > 1 && operand != NULL && operand->kind == LHAT_NODE_CALL) {
        compile_call_wide(c, operand, into, reserved);
    } else {
        compile_expression(c, operand, into);
    }

    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISERROR, test, into, 0));
    size_t past = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    c->next_register = mark;

    emit_error_escape(c, into);
    lhat_chunk_patch_here(&c->proto->chunk, past);
}

static void compile_try(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_try_wide(c, node, into, 0);
}

// 04 の 4 章: catch^ replaces the value on the spot, and 4.2 names the error
// it^ inside the right side -- the same word 02 の 16.2 uses for a focus.
// 13.8改: `reserved` is the run the caller laid out -- one head slot plus a
// position each. 0 and 1 both mean the ordinary one-slot answer, which is
// every catch^ written before tuples.
//
// Both arms write the same run. The left is laid out by compile_run_source,
// so the head slot holds either the error or the run's own head, and ISERROR
// reads it exactly as it always did -- the discrimination try^ already
// relies on. The right side then writes its own positions over the same
// slots, which is why nothing needs to be moved when the arms meet again.
static void compile_catch_wide(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved)
{
    if (reserved > 1) {
        compile_run_source(c, node->v.binary.left, into, reserved);
    } else {
        compile_expression(c, node->v.binary.left, into);
    }

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
        local->import_root = false;
    }
    if (reserved > 1 && is_run_source(node->v.binary.right)) {
        compile_run_source(c, node->v.binary.right, into, reserved);
    } else {
        compile_expression(c, node->v.binary.right, into);
    }

    lhat_chunk_patch_here(&c->proto->chunk, past);
    c->local_count = local_mark;
    c->next_register = register_mark;
}

static void compile_catch(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_catch_wide(c, node, into, 0);
}

// 13.8改: '(a, b)' written as a value. The positions go into the slots above
// the head, and MAKERUN puts the head down over them -- the one place a run
// is built without crossing a frame boundary.
static void compile_tuple_literal(Compiler *c, const LhatNode *node,
                                  uint8_t into, size_t positions)
{
    size_t written = lhat_node_list_length(node->v.list.items);
    if (positions == 0) {
        positions = written;
    }
    if (positions < 2 || positions != written || positions > LHAT_MAX_TUPLE) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }
    uint8_t at = (uint8_t)(into + 1);
    for (const LhatNode *item = node->v.list.items; item != NULL;
         item = item->next) {
        compile_expression(c, item, at);
        at++;
    }
    emit(c, lhat_encode_abc(LHAT_BC_MAKERUN, into, (uint8_t)positions, 0));
}

// 02 の 11.7: '??' is the same shape as catch^, asking about nil^ instead.
// 13.8改: `reserved` is the run the caller laid out, exactly as it is for
// compile_catch_wide -- 04 の 4.1 and 11.7 make the two the same shape, so
// they get the same treatment. Both arms write the same run, and ISNIL reads
// the head slot: a run's head is not nil^, so the left arm stands; a nil^
// there takes the right. The instruction is unchanged.
static void compile_nil_else_wide(Compiler *c, const LhatNode *node,
                                  uint8_t into, size_t reserved)
{
    if (reserved > 1) {
        compile_run_source(c, node->v.binary.left, into, reserved);
    } else {
        compile_expression(c, node->v.binary.left, into);
    }

    uint8_t mark = c->next_register;
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, into, 0));
    size_t to_default = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    c->next_register = mark;

    if (reserved > 1 && is_run_source(node->v.binary.right)) {
        compile_run_source(c, node->v.binary.right, into, reserved);
    } else {
        compile_expression(c, node->v.binary.right, into);
    }
    lhat_chunk_patch_here(&c->proto->chunk, to_default);
}

static void compile_nil_else(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_nil_else_wide(c, node, into, 0);
}

// 02 の 13.11: isa^ asks whether the left side may stand where the right side
// is written. Every spelling of the right side is that one question at run
// time, so there is one instruction for it: lower_type turns the written type
// into the descriptor LHAT_BC_ISA tests a value against, which is the same
// object 11.6's as^ hands LHAT_BC_ASCAST. An error kind (04 の 6.1) and a
// host type (05 の 8.8) are both ordinary results of that lowering, which is
// why neither needs a case here any more.
//
// 5.13: the right side is a type the COMPILER settles, always. A value that
// only arrives while the program runs -- a definition passed as a parameter,
// another unit's member -- carries no type to ask about: it is a plain table
// (13.7's t^{}), however it was made, and a program that receives one probes
// it the dynamic way, member by member, nil^ by nil^. There is no fallback
// that loads such a name as a value and has the machine read a shape off the
// table at run time: constructing type
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
// 14.7改 with 14.4: whether an instance carries this entry of a def^. What is
// reached through one is what is handed a receiver, which the signature says
// -- written out where 14.15 declares the member, and in the literal's own
// parameter list otherwise. 14.11's new and a static member are the
// definition's, and asking a value for one of those would ask for what an
// instance was never meant to answer.
static bool entry_takes_receiver(Compiler *c, const LhatNode *entry)
{
    const LhatNode *written = entry->v.entry.value;
    if (written == NULL || (written->kind != LHAT_NODE_FUNC &&
                            written->kind != LHAT_NODE_TYPE_FUNC)) {
        return false;
    }
    const LhatNode *params = written->v.func.params;
    for (const LhatNode *param = params; param != NULL; param = param->next) {
        // 11.3改: first or last, and nowhere else -- the same two places
        // compile_func reads a receiver from.
        if (param != params && param->next != NULL) {
            continue;
        }
        const LhatNode *marker = param->v.param.name != NULL
                                     ? param->v.param.name
                                     : param->v.param.type;
        const char *name = NULL;
        size_t length = 0;
        if (node_name(c, marker, &name, &length) &&
            name_is(name, length, "self^")) {
            return true;
        }
    }
    return false;
}

static bool add_shape_member(Compiler *c, LhatRuntimeType *into,
                             const LhatNode *entry, bool of_template)
{
    const char *name = NULL;
    size_t length = 0;
    // 14.14改: a computed key names no member, so it asks for nothing.
    if (entry->v.entry.computed || entry->v.entry.key == NULL ||
        !node_name(c, entry->v.entry.key, &name, &length)) {
        return true;
    }
    // 14.7改: the shape is what the definition promises about its instances,
    // so a member they do not carry is not part of it. A template field is
    // theirs whatever it holds.
    if (!of_template && !entry_takes_receiver(c, entry)) {
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
            if (!add_shape_member(c, type, entry, false)) {
                c->lexer = enclosing;
                return NULL;
            }
        }
        const LhatNode *fields = template_of(chain->parts[i]);
        for (const LhatNode *field = fields != NULL ? fields->v.list.items : NULL;
             field != NULL; field = field->next) {
            if (!add_shape_member(c, type, field, true)) {
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

        case LHAT_NODE_TYPE_TUPLE: {
            // 13.8改: the positions, in order, held the way a union's arms
            // are. A position that lowers to nothing takes the tuple with it,
            // for the reason the union above gives.
            LhatRuntimeType *type = lhat_type_rt_new(owner, LHAT_TYPE_RT_TUPLE);
            if (type == NULL) {
                return NULL;
            }
            for (const LhatNode *item = node->v.list.items; item != NULL;
                 item = item->next) {
                LhatRuntimeType *position = lower_type(c, item);
                if (position == NULL || !lhat_type_rt_add_part(type, position)) {
                    return NULL;
                }
            }
            return type;
        }

        // The section reads as a structure of its own, which is what it is --
        // 14.7改 has a definition carry what its instances are.
        case LHAT_NODE_SELF_TABLE:
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
                if (member->v.entry.value != NULL &&
                    member->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
                    type->instance = lower_type(c, member->v.entry.value);
                    continue;
                }
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
            // after it for the same reason compile_isa tries it last:
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
        // bare tag alone.
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

// Whether this name is a root an import^ bound in a body around this one.
// Its own body's is an ordinary local and is read as one; only the step out
// is what a capture would otherwise be.
static bool import_root_outside(const Compiler *c, const char *name,
                                size_t length)
{
    for (const Compiler *outer = c->parent; outer != NULL;
         outer = outer->parent) {
        const Local *local = find_local(outer, name, length);
        if (local != NULL) {
            return local->import_root;
        }
    }
    return false;
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
    // 05 の 8.7: an import^ root standing outside this body is read back off
    // L^.modules rather than captured. A registration is an object on the
    // heap of the machine it was installed on, so a capture would hand this
    // body the maker's -- which is what a body run on another machine
    // (std.thread's spawn) must not have, and is why that refuses a closure
    // with captures at all. Reading it here is the same walk the import^
    // itself made, on whichever machine is running.
    if (import_root_outside(c, name, length)) {
        uint8_t mark = c->next_register;
        uint8_t key = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
        load_string_bytes(c, key, "modules", 7);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
        load_string_bytes(c, key, name, length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
        c->next_register = mark;
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
    local->import_root = false;

    const DefChain *enclosing = c->building;
    c->building = &chain;

    // 14.11改: the new every definition has goes down first, so a written one
    // is 14.12's second member of that name -- an overload^ adds an arm to
    // this and an override^ replaces it. The checker seeds the same member in
    // the same place, and the marker it asks for is what keeps the two in
    // step.
    compile_default_new(c, node, into);

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
                previous->import_root = false;
            }

            compile_expression(c, entry->v.entry.value, value);
            // 14.12: overload^ keeps what was there and adds a way to call
            // it, so the two go under one name together. Which one a call
            // means is settled when it runs, since 14.12's ban on overlapping
            // signatures leaves at most one that fits.
            //
            // 14.12改2: an override^ new is the exception -- it replaces the
            // member whole rather than the arm it overlaps, since 14.11's new
            // is exempt from the substitutability that picks one. So it takes
            // the plain write below, which is what the checker's type says.
            LhatOpcode write = LHAT_BC_SETINDEX;
            uint8_t operand = value;
            if (entry->v.entry.modifier == LHAT_DEF_OVERLOAD) {
                write = LHAT_BC_ADDOVERLOAD;
            } else if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                       !name_is(name, length, "new")) {
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
    inner.result = c->result;

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
// `at` is the first of the consecutive registers the caller reserved: 5.3
// lays a method call out as callee, receiver, then arguments, and the key is
// read out of the one past the receiver before the argument is written over
// it. 05 の 8.9: a host value receiver holds its width of them, so what
// follows it is that much further up.
static void compile_interp_part(Compiler *c, const LhatNode *part, uint8_t at)
{
    if (part->kind == LHAT_NODE_INTERP_TEXT) {
        load_string(c, at, part);
        return;
    }

    uint8_t receiver = (uint8_t)(at + 1);
    uint8_t argument = (uint8_t)(receiver + width_of(part->v.hole.value));
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
        // A hole wants the piece, the receiver and the key above it, and the
        // first piece cannot simply take `into` and what follows -- those
        // belong to whoever reserved them. 05 の 8.9: the receiver is as wide
        // as the checker says it is.
        size_t held = part->kind == LHAT_NODE_INTERP_HOLE
                          ? 1 + width_of(part->v.hole.value) + 1
                          : 1;
        uint8_t piece = reserve_wide(c, held);
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
            // 14.14改: the key is an expression, evaluated here like any
            // other. 04 の 11.3 keeps nil^ and a NaN out of a key, which the
            // machine reports when it lands.
            compile_expression(c, entry->v.entry.key, key);
        } else if (entry->v.entry.key != NULL) {
            const LhatNode *named = entry->v.entry.key;
            const char *name = NULL;
            size_t length = 0;
            if (node_name(c, named, &name, &length)) {
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
// 02 の 13.8改: `reserved` is how many consecutive slots the caller wants the
// answer written into -- one head slot plus the positions. 0 and 1 both mean
// the ordinary one-slot answer, which is every call site but a destructuring
// bind. The count travels in CALL's C so the callee can be told what is
// expected of it; the two sides cannot agree statically (an unchecked
// compile, 03 の 4.3's session, 05 の 5.3's units, a callee that is only a
// value), so the machine catches a disagreement instead.
static void compile_call_wide(Compiler *c, const LhatNode *node, uint8_t into,
                              size_t reserved)
{
    // 5.3: taken here and cleared at once. What is compiled below -- the
    // callee, the receiver, every argument -- may hold calls of its own, and
    // none of those is the one standing in tail position.
    bool tail = c->tail_call;
    bool drop = c->tail_drop;
    c->tail_call = false;
    c->tail_drop = false;

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

    // 04 の 11.4 with 01 の 7.1: '?(' answers nil^ for an absent
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
        // 13.7: the value to unpack goes in the slot; C tells the machine the
        // last argument is not an ordinary one but something to spread.
        if (arg->kind == LHAT_NODE_SPREAD) {
            // 13.8改: a tuple spreads as the run it already is -- head slot
            // and positions -- so nothing is built to hand over. A table
            // spreads as the one value it is, the way it always did.
            size_t positions = tuple_width_of(arg->v.jump.value);
            if (positions > 1) {
                uint8_t head = reserve_wide(c, positions + 1);
                compile_run_source(c, arg->v.jump.value, head, positions + 1);
            } else {
                uint8_t slot = reserve(c);
                compile_expression(c, arg->v.jump.value, slot);
            }
            spread = true;
            count++;
            continue;
        }
        // 05 の 8.9: a host value argument takes its width of consecutive
        // slots; the callee's parameter run was laid out by the same widths,
        // so the frame window still lines up with no copying.
        uint8_t slot = reserve_for(c, arg);
        wide_args = wide_args || width_of(arg) > 1;
        compile_expression(c, arg, slot);
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
    // call means, so the search 5.11 would run is replaced by taking that
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
    // 13.8改: a tuple answer needs the same room -- a head slot and then the
    // positions -- so the wider of the two is what the frame has to hold.
    size_t answer_width = width_of(node);
    if (reserved > answer_width) {
        answer_width = reserved;
    }
    while (c->next_register < callee + answer_width) {
        reserve(c);
    }
    // 5.3: a tail call is the same call with permission to take this frame
    // over. The machine refuses the permission where the frame is not free to
    // go, so what is emitted after this stands either way.
    LhatOpcode call_op = method ? (tail ? LHAT_BC_TAILCALLMETHOD
                                        : LHAT_BC_CALLMETHOD)
                                : (tail ? LHAT_BC_TAILCALL : LHAT_BC_CALL);
    uint8_t operand = lhat_call_operand(spread, (unsigned)reserved);
    if (tail && drop) {
        operand |= LHAT_CALL_DROP;
    }
    emit(c, lhat_encode_abc(call_op, callee, (uint8_t)count, operand));
    // The answer then moves to the destination the same way it was written.
    emit_move_wide(c, into, callee, answer_width);
    // where an absent callee's nil^ lands, past everything the call
    // itself does.
    if (past_call != SIZE_MAX) {
        lhat_chunk_patch_here(&c->proto->chunk, past_call);
    }
    c->next_register = mark;
}

static void compile_call(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_call_wide(c, node, into, 0);
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
    inner.result = c->result;

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
        // 11.3改: written last it marks one too, and says the receiver
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

    // 05 の 8.9: the parameters are laid down first and each took its own
    // width, so where the reserving has reached is where they end -- which is
    // not the count once one of them is a host value. The machine reads it to
    // know where the frame's scratch begins.
    proto->parameter_slots = inner.next_register;

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
        proto->result_type = lhat_rt_from_checked(
            &root_of(c)->proto->chunk.heap, checked->v.func.result);
    }

    // 15.2, 13.9: Y and R have no written form at all -- 03 の 5.11a's
    // checked_type is the only place either can come from, written or not.
    if (node->v.func.yields && node->checked_type != NULL) {
        const LhatType *checked = (const LhatType *)node->checked_type;
        LhatHeap *owner = &root_of(c)->proto->chunk.heap;
        // 15.5: taken off the coroutine the checker assembled rather than off
        // the three fields, so the defaults 13.9 fills in (a body producing
        // nothing produces nil^; one that ends without a value ends with
        // nil^) reach a value reflected at run time. Otherwise typeof^ would
        // answer one thing where the checker resolved it and another where
        // 04 の 2.4 sends it to the instruction instead.
        const LhatType *made = lhat_type_call_answer(checked);
        if (made != NULL && made->kind == LHAT_TYPE_CORO) {
            proto->yield_produce_type =
                lhat_rt_from_checked(owner, made->v.coroutine.produce);
            proto->yield_receive_type =
                lhat_rt_from_checked(owner, made->v.coroutine.receive);
            // What lower_type made above can name an error kind (04 の 2.4)
            // and what is rebuilt from a checked type cannot, so anything
            // written is lowered from where it was written rather than
            // rebuilt. What is taken from the written form is 13.9's three
            // types, never the c^{ … } around them: tag_type (vm.c) puts that
            // back, so leaving it here would wrap the coroutine twice.
            const LhatNode *written = node->v.func.return_type;
            if (written == NULL) {
                proto->result_type =
                    lhat_rt_from_checked(owner, made->v.coroutine.result);
            } else if (written->kind == LHAT_NODE_TYPE_CORO) {
                proto->result_type = lower_type(c, written->v.coroutine.result);
                // Omitted R or Y is 13.9's nil^ default, which only the
                // assembled type has -- so those two are left as they were.
                if (written->v.coroutine.produce != NULL) {
                    proto->yield_produce_type =
                        lower_type(c, written->v.coroutine.produce);
                }
                if (written->v.coroutine.receive != NULL) {
                    proto->yield_receive_type =
                        lower_type(c, written->v.coroutine.receive);
                }
            }
        } else {
            proto->yield_produce_type =
                lhat_rt_from_checked(owner, checked->v.func.yield_produce);
            proto->yield_receive_type =
                lhat_rt_from_checked(owner, checked->v.func.yield_receive);
        }
    }

    // 5.3: which statement of this body ends it, so that a bare call there is
    // read as a tail call. Only a statement of the body itself -- one inside a
    // block, a loop or a branch has the statements after that block to come
    // back to. A body carrying a finally^ (10.1) has its cleanup after the
    // call, which cleanup_depth refuses at the statement itself.
    if (node->v.func.body != NULL &&
        node->v.func.body->kind == LHAT_NODE_BLOCK) {
        for (const LhatNode *s = node->v.func.body->v.list.items; s != NULL;
             s = s->next) {
            inner.tail_statement = s;
        }
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
        if (operand == NULL || c->result->status != LHAT_COMPILE_OK) {
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
    if (node == NULL || c->result->status != LHAT_COMPILE_OK) {
        return;
    }
    c->line = node->line;
    c->offset = node->offset;
    c->column = node->column;

    switch (node->kind) {
        case LHAT_NODE_INT:
            load_constant(c, into, lhat_integer((int64_t)node->v.integer.value));
            return;

        case LHAT_NODE_FLOAT:
            load_constant(c, into, lhat_real(node->v.real));
            return;

        case LHAT_NODE_STRING:
            load_string(c, into, node);
            return;

        // 01 の 3.3: id^name is that name's spelling, and a string is what it
        // answers -- the same bytes a key written bare compiles to.
        case LHAT_NODE_NAME: {
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, node, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            load_string_bytes(c, into, name, length);
            return;
        }

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
                node->v.loop.kind != LHAT_FOR_WHEN &&
                node->v.loop.kind != LHAT_FOR_ONCE) {
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

        // 02 の 14.16: typeof^ answers the type the checker settled,
        // compiled in as a constant -- type information is a compile-time
        // thing, and the run manufactures none (3.5 and 5.13 are the
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
            if (checked != NULL && !lhat_rt_mentions_error(checked)) {
                // 13.7: any^ converts to no descriptor at all ("asks
                // nothing"), but as an ANSWER it is a value of its own.
                LhatRuntimeType *rt =
                    checked->kind == LHAT_TYPE_ANY ||
                            checked->kind == LHAT_TYPE_NONE
                        ? lhat_type_rt_new(&c->proto->chunk.heap,
                                           LHAT_TYPE_RT_ANY)
                        : lhat_rt_from_checked(&c->proto->chunk.heap, checked);
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

        // 11.6: the operand lands in `into` and stays there -- as^
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
                // not settle is refused (5.13) -- 02 の 11.6改 promises as^ panics
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
            // 13.8改: 'yield^ a, b' answers a tuple -- the positions go in
            // consecutive slots and YIELD carries how many, the same shape
            // return^ uses. The head is the machine's, put down in the
            // resumer's frame. Only written as a statement, so what the
            // resume sends lands in the first position's slot and nothing
            // here reads it back.
            if (node->v.jump.level > 1) {
                size_t positions = node->v.jump.level;
                if (positions > LHAT_MAX_TUPLE) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                uint8_t mark = c->next_register;
                uint8_t first = reserve_wide(c, positions);
                uint8_t at = first;
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    compile_expression(c, item, at);
                    at++;
                }
                if (!node->v.jump.phantom) {  // 15.11
                    emit(c, lhat_encode_abc(LHAT_BC_YIELD, first,
                                            (uint8_t)positions, 0));
                }
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                c->next_register = mark;
                return;
            }
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

            // 04 の 11.4 with 01 の 7.1: '?.' and '?[' answer nil^
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
            // 01 の 2.3: a stacked reach -- it^^ the enclosing focus,
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
                fail_named(c, LHAT_COMPILE_UNDEFINED, name, length);
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
                    fail_named(c, LHAT_COMPILE_UNDEFINED, name, length);
                    return;
            }
            return;
        }

        case LHAT_NODE_UNARY: {
            uint8_t mark = c->next_register;
            // 05 の 8.9: a host value operand keeps its width, as everywhere.
            uint8_t operand = reserve_for(c, node->v.unary.operand);
            compile_expression(c, node->v.unary.operand, operand);
            // 11.7改2: 'x?' is '!(x isa^ nil^)' written short, and the
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

        // 13.8改: a tuple literal reached as an ordinary expression means it
        // was written where no run was reserved -- a name, an argument, a
        // table's element. The checker refuses every one of those by name;
        // this is the backstop for an unchecked compile.
        case LHAT_NODE_TUPLE:
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;

        // 13.8改: pack^ -- the run is laid down and turned into a table in
        // place. The one allocation here is the table the writer asked for.
        case LHAT_NODE_PACK: {
            const LhatNode *source = node->v.jump.value;
            size_t positions = tuple_width_of(source);
            if (positions < 2 || positions > LHAT_MAX_TUPLE ||
                !is_run_source(source)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t head = reserve_wide(c, positions + 1);
            compile_run_source(c, source, head, positions + 1);
            emit(c, lhat_encode_abc(LHAT_BC_CHECKRUN, head,
                                    (uint8_t)positions, 0));
            emit(c, lhat_encode_abc(LHAT_BC_PACK, head, (uint8_t)positions, 0));
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, head, 0));
            c->next_register = mark;
            return;
        }

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

// ast.c's target readings (8.8), shared with the checker.
static const LhatNode *define_target_name(const LhatNode *target)
{
    return lhat_define_target_name(target);
}

static bool define_target_is_path(const LhatNode *target)
{
    return lhat_define_target_is_path(target);
}

static const LhatNode *define_target_root(const LhatNode *target)
{
    return lhat_define_target_root(target);
}

// ast.c's L^ test (05 の 8.6), shared with the checker. The name the caller
// already cut is enough to answer from, so the source is not re-read.
static bool is_environment(const LhatNode *node, const char *name,
                           size_t length)
{
    return node->kind == LHAT_NODE_HAT_IDENT && lhat_name_is(name, length, "L^");
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
// 05 の 8.6: writing back what was already there is a write like any other,
// so the machine's own tables refuse it, and a path through L^ would fail on
// its own second segment. The branch this costs is cheaper than skipping it.
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
        // 05 の 5.5: the short form makes one name too -- the root of the
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
                // 05 の 8.7: what is under this root came out of L^.modules,
                // so a nested body can read it back rather than capture it.
                local->import_root = true;
                continue;
            }
            module_name = required_module_name(c, s);
            if (module_name == NULL) {
                continue;  // reported when the statement is compiled
            }
            size_t root = strcspn(module_name, ".");
            const Local *taken = find_local(c, module_name, root);
            if (taken != NULL) {
                // 05 の 5.5 landing under a root an import^ also made: what
                // this puts there is a unit's value and is in no registry, so
                // the root goes back to being captured like any other name.
                ((Local *)taken)->import_root = false;
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
            local->import_root = false;
            continue;
        }
        if (s->kind != LHAT_NODE_DEFINE) {
            continue;
        }
        const LhatNode *bound_value = s->v.binding.values;
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
            //
            // 02 の 13.12: and a '_^' is written as often as it likes, so the
            // second one takes the same slot rather than a fresh one. Nothing
            // reads it back, which is what makes sharing the place harmless.
            if ((c->session_top ||
                 node_is_discard(c, define_target_name(target))) &&
                find_local(c, name, length) != NULL) {
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
            local->import_root = false;
        }
    }
}

// 02 の 13.8改: 'var^ a, b = f()'. The call lays a head slot and the positions
// into the run reserved here, and every name takes its own out of it. No
// table is made anywhere along the way -- which is what 13.8 promised when it
// said the cost would be absorbed by the implementation, and never built.
//
// Reaching here at all means the checker settled the width (tuple_width_of
// reads its stamp), and the two sides are reconciled by the machine at the
// pop, where a callee compiled separately is caught.
static void compile_tuple_define(Compiler *c, const LhatNode *node,
                                 size_t positions)
{
    if (positions > LHAT_MAX_TUPLE) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    uint8_t mark = c->next_register;
    uint8_t head = reserve_wide(c, positions + 1);
    compile_run_source(c, node->v.binding.values, head, positions + 1);
    // 13.8改: what stands between a value that is not a run and the slots
    // after it being read as positions nobody wrote. The type settled this
    // wherever the checker ran; a separately compiled callee is what lands
    // here. A try^'s error arm never reaches it -- that RETURN already left.
    emit(c, lhat_encode_abc(LHAT_BC_CHECKRUN, head, (uint8_t)positions, 0));

    size_t position = 0;
    for (const LhatNode *target = node->v.binding.targets; target != NULL;
         target = target->next) {
        position++;
        if (position > positions || position > LHAT_MAX_LOCALS) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        uint8_t from = (uint8_t)(head + position);
        uint8_t inner = c->next_register;

        // 8.8: the place is a member of a table the path reaches, exactly as
        if (define_target_is_path(target)) {
            const LhatNode *last = define_target_name(target);
            uint8_t owner = reserve(c);
            uint8_t place = reserve(c);
            compile_path_prefix(c, last->v.access.target, owner);
            compile_key(c, last, place);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, place, from));
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
        // 05 の 8.9: a position is one slot, so a name taking one is never
        // wide. The checker refused a host value as a position; this is the
        // backstop rather than a trust.
        if (local->width > 1) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        // 03 の 4.3, as in the ordinary define: a name an earlier input bound
        // stops sharing its place before this let^ writes it.
        if ((size_t)(local - c->locals) < c->session_locals) {
            emit(c, lhat_encode_abc(LHAT_BC_CLOSEONE, local->reg, 0, 0));
        }
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, local->reg, from, 0));
        c->next_register = inner;
    }
    c->next_register = mark;
}

static void compile_define(Compiler *c, const LhatNode *node)
{
    const LhatNode *value = node->v.binding.values;
    // 13.8改: several values on the right and several names on the left, with
    // no word between them -- what the type says is what tells this from
    // 8.6's multiple definition, and the parser already told them apart by
    // how many values were written.
    if (value != NULL && value->next == NULL &&
        is_run_source(value) && tuple_width_of(value) > 1 &&
        node->v.binding.targets != NULL &&
        node->v.binding.targets->next != NULL) {
        compile_tuple_define(c, node, tuple_width_of(value));
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
// evaluated into, so that 7.4's "the target is evaluated once" survives the
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


    // 13.8改: several values from one call, laid down once here with each
    // target reading its own position out of the run. 13.8.s
    // read-everything-then-write-everything (8.6改3) needs nothing extra:
    // there is one read.
    //
    // Every target kind the write pass below handles comes for free that way
    // -- a name, a .$. specifier, an upvalue, a member of a table.
    const LhatNode *tuple_call = NULL;
    size_t tuple_positions = 0;
    uint8_t run_head = 0;
    if (value != NULL && value->next == NULL &&
        is_run_source(value) && tuple_width_of(value) > 1 &&
        node->v.binding.targets != NULL &&
        node->v.binding.targets->next != NULL) {
        tuple_positions = tuple_width_of(value);
        if (tuple_positions > LHAT_MAX_TUPLE) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        tuple_call = value;
        run_head = reserve_wide(c, tuple_positions + 1);
        compile_run_source(c, value, run_head, tuple_positions + 1);
        emit(c, lhat_encode_abc(LHAT_BC_CHECKRUN, run_head,
                                (uint8_t)tuple_positions, 0));  // 13.8改
    }

    size_t position = 0;
    for (const LhatNode *target = node->v.binding.targets;
         target != NULL &&
         (tuple_call != NULL || value != NULL);
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

            // 7.4, as in the single-target path: the current value comes
            // back through the owner and key already evaluated rather than
            // from compiling the target again. Never a destructuring: 13.10's
            // mark stands where a value would, so there is no operator on it.
            if (tuple_call == NULL &&
                node->v.binding.has_compound_op &&
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

        // 13.8改: the position sits in the run already, so the write pass
        // reads it straight out -- no move of its own.
        if (tuple_call != NULL) {
            if (position > tuple_positions) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            w->value = (uint8_t)(run_head + position);
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
    (void)values;
    if (node->v.binding.targets != NULL &&
        node->v.binding.targets->next != NULL) {
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

            // 7.4: owner and key were just evaluated once, above. A
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
        // 5.3 lays a method call out as callee, receiver, then arguments.
        // 14.4 puts the value in self^, so this is a method call and not a
        // plain one -- a dispose() written in a def^ declares the receiver and
        // would be an argument short otherwise. The key is read out of the
        // third register before anything would be written over it, the same
        // way compile_interp_part reads one.
        uint8_t callee = reserve(c);
        uint8_t receiver = reserve(c);
        uint8_t key = reserve(c);
        load_string_bytes(c, key, "dispose", 7);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, callee, held[i], key));
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, receiver, held[i], 0));
        emit(c, lhat_encode_abc(LHAT_BC_CALLMETHOD, callee, 0, 0));
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
        local->import_root = false;
    }
}

// 13.10: one name takes the value whole, and several take it apart by
// position. `in^` is the marker that says which, so no other mark is needed
// (16.3). 13.8 makes what an iterator yields a table when it is a group.
static void bind_targets(Compiler *c, const LhatNode *focus, size_t local_mark,
                         size_t count, uint8_t from)
{
    if (count == 1) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, c->locals[local_mark].reg, from,
                                0));
        return;
    }
    // 16.3 with 13.8改: several names read the run the walk laid at `from` --
    // head slot first, positions after it. The machine put every shape of
    // iterator into this form (a table walk directly, a tuple-yielding body
    // by the yield's own placement, a table-yielding one expanded on
    // landing), so the binds are the same three MOVEs whatever was walked.
    for (size_t i = 0; i < count; i++) {
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, c->locals[local_mark + i].reg,
                                (uint8_t)(from + 1 + i), 0));
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
        // 13.8改: several names take a run -- one head slot plus a position
        // each -- and one name takes one value, so the answer's room is
        // sized by the count. The count is syntax, which is what lets an
        // unchecked compile reserve the same slots a checked one does.
        taken = focus_locals > 1 ? reserve_wide(c, focus_locals + 1)
                                 : reserve(c);
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
    context.next_count = 0;
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
        // 16.3 with 13.8改: C carries the loop's word on what one step should
        // put down -- N+1 for N names (a run), 2 for one name (a table's
        // sequence values; a body's yield unchanged). 03 の 5.3's shape:
        // the two sides tell each other, and a mismatch faults rather than
        // being papered over.
        emit(c, lhat_encode_abc(LHAT_BC_RESUME, taken, walk,
                                focus_locals > 1 ? (uint8_t)(focus_locals + 1)
                                                 : 2));
        emit(c, lhat_encode_abc(LHAT_BC_ISDONE, test, walk, 0));
        emit(c, lhat_encode_abc(LHAT_BC_NOT, test, test, 0));
        leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
        // The guard for an unchecked iterator that answered some other
        // shape; the checker's own report came first wherever it ran.
        if (focus_locals > 1) {
            emit(c, lhat_encode_abc(LHAT_BC_CHECKRUN, taken,
                                    (uint8_t)focus_locals, 0));
        }
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

    // 9.11: where a next^ lands. The rest of the body is what it skipped;
    // the step below is not -- without it a counted loop would never move,
    // and 16.4's to^ is that step written for the writer.
    for (size_t i = 0; i < context.next_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, context.nexts[i]);
    }

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
    if (node == NULL || c->result->status != LHAT_COMPILE_OK) {
        return;
    }
    c->line = node->line;
    c->offset = node->offset;
    c->column = node->column;

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
            // 02 の 13.8改: 'return^ a, b' answers a tuple -- the positions go
            // in consecutive slots and RETURN carries how many. No head slot
            // is built here: the head belongs in the caller's frame, and the
            // machine puts it there.
            if (node->v.jump.level > 1) {
                size_t positions = node->v.jump.level;
                if (positions > LHAT_MAX_TUPLE) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                uint8_t first = reserve_wide(c, positions);
                uint8_t at = first;
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    compile_expression(c, item, at);
                    at++;
                }
                emit(c, lhat_encode_abc(LHAT_BC_RETURN, first,
                                        (uint8_t)positions, 0));
                c->next_register = mark;
                return;
            }
            // 05 の 8.9: a returned host value needs its whole width here;
            // the machine reads that width off the head when the frame pops.
            uint8_t slot = reserve_for(c, node->v.jump.value);
            // 5.3: 'return^ f(x)' is the call standing in tail position -- what
            // it answers is what this frame answers, so the frame is free to
            // go. Not where a cleanup is pending: 5.5 runs those after the
            // call, and a frame that has left cannot run them.
            if (node->v.jump.value->kind == LHAT_NODE_CALL &&
                c->cleanup_depth == 0) {
                c->tail_call = true;
            }
            compile_expression(c, node->v.jump.value, slot);
            c->tail_call = false;
            emit(c, lhat_encode_abc(LHAT_BC_RETURN, slot, 0, 0));
            c->next_register = mark;
            return;
        }

        // 04 の 11.6: unlike return^, this does not answer anything a
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

        // 04 の 4.5: the body runs with the arms as the place a try^ leaves
        // for. Reaching the end of it with nothing raised jumps past them --
        // the arms are only ever entered through one of those jumps, with the
        // error already in `caught`.
        case LHAT_NODE_TRY_BLOCK: {
            const LhatNode *body = node->v.list.items;
            if (body == NULL) {
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t caught = reserve(c);

            TryContext context;
            context.enclosing = c->trying;
            context.count = 0;
            context.cleanup_depth = c->cleanup_depth;
            context.caught = caught;
            c->trying = &context;
            compile_statement(c, body->v.clause.body);
            c->trying = context.enclosing;

            size_t no_error = emit_jump(c, LHAT_BC_JUMP, 0);
            for (size_t i = 0; i < context.count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, context.jumps[i]);
            }

            // 13.11's judgement, arm by arm. The bare one asks nothing, and
            // 4.5 puts it last, so what follows it is only the end.
            size_t leaving[LHAT_MAX_BREAKS];
            size_t leaving_count = 0;
            bool bare = false;
            for (const LhatNode *arm = body->next; arm != NULL;
                 arm = arm->next) {
                size_t next = SIZE_MAX;
                if (arm->v.clause.condition != NULL) {
                    uint8_t inner = c->next_register;
                    uint8_t test = reserve(c);
                    compile_isa_test(c, arm->v.clause.condition, caught, test);
                    next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                    c->next_register = inner;
                }

                // 4.2: it^ is the error, and the register it is already in.
                size_t local_mark = c->local_count;
                if (c->local_count < LHAT_MAX_LOCALS) {
                    Local *local = &c->locals[c->local_count++];
                    local->name = "it^";
                    local->length = 3;
                    local->reg = caught;
                    local->depth = c->scope_depth;
                    local->width = 1;
                    local->import_root = false;
                }
                compile_statement(c, arm->v.clause.body);
                c->local_count = local_mark;

                if (next == SIZE_MAX) {
                    bare = true;  // it takes everything left; the end is next
                    break;
                }
                if (leaving_count < LHAT_MAX_BREAKS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            // 4.5: what no arm took leaves the way it would have without the
            // block around it -- to an outer one, or out of the frame.
            if (!bare) {
                emit_error_escape(c, caught);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            lhat_chunk_patch_here(&c->proto->chunk, no_error);
            c->next_register = mark;
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

        // 05 の 5.5: bring the unit in, then put it where the path it
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
            const LhatNode *value = node->kind == LHAT_NODE_CALL_STMT
                                        ? node->v.jump.value
                                        : node;
            // 13.8改: a discarded call may still answer a tuple -- a walk's
            // resume, whose type is '(K, V)|nil^' -- and the run needs its
            // width in slots even with nobody reading it. The checker's
            // stamp sizes the reservation; unchecked, the call reserves one
            // slot and a tuple coming back is the machine's to refuse.
            size_t width =
                node->kind == LHAT_NODE_CALL_STMT && value->checked_type != NULL
                    ? lhat_type_tuple_arm_width(
                          (const LhatType *)value->checked_type)
                    : 0;
            if (width > 0 && is_run_source(value)) {
                uint8_t first = reserve_wide(c, width + 1);
                compile_run_source(c, value, first, width + 1);
            } else {
                uint8_t slot = reserve(c);
                // 5.3: a bare call standing last in a body is in tail
                // position too -- what follows it is the end of the body,
                // which answers nil^. So the frame is free to go, and what
                // the call answers is thrown away the way it is here.
                if (node == c->tail_statement && c->cleanup_depth == 0 &&
                    value->kind == LHAT_NODE_CALL) {
                    c->tail_call = true;
                    c->tail_drop = true;
                }
                compile_expression(c, value, slot);
                c->tail_call = false;
                c->tail_drop = false;
            }
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_FOR:
            // 16.1: for^ introduces a value; whether it repeats is up to the
            // clause after it, and if^ is the clause that does not.
            if (node->v.loop.kind == LHAT_FOR_IF ||
                node->v.loop.kind == LHAT_FOR_WHEN ||
                node->v.loop.kind == LHAT_FOR_ONCE) {
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

        // 9.11: the same count over the same chain, landing at the loop's
        // own step instead of past it. The loops it passes through are left
        // for good, so their cleanups drain exactly as break^ drains them.
        case LHAT_NODE_NEXT: {
            if (node->v.jump.value != NULL || node->v.jump.level == 0) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
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
            if (target->next_count >= LHAT_MAX_BREAKS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            emit_cleanup_drain(c, target->cleanup_depth);
            target->nexts[target->next_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
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

// 05 の 5.5: the path a require^ standing alone brings a unit in under, or
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
    // 05 の 8.6: everything it holds is in, so nothing else writes to
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

static LhatCompileResult compile_unit(LhatCompileSession *session,
                                      const LhatNode *unit,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out)
{
    LhatCompileResult result;
    memset(&result, 0, sizeof result);
    *out = NULL;
    if (unit == NULL || lexer == NULL) {
        result.status = LHAT_COMPILE_UNSUPPORTED;
        return result;
    }

    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        result.status = LHAT_COMPILE_TOO_COMPLEX;
        return result;
    }

    Compiler c;
    memset(&c, 0, sizeof c);
    c.lexer = lexer;
    c.proto = proto;
    c.result = &result;

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
    if (session != NULL && result.status == LHAT_COMPILE_OK) {
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

    if (result.status != LHAT_COMPILE_OK) {
        lhat_proto_free(proto);
        return result;
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
                    result.status = LHAT_COMPILE_TOO_COMPLEX;
                    return result;
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
    return result;
}

LhatCompileResult lhat_compile(const LhatNode *unit, const LhatLexer *lexer,
                               LhatProto **out)
{
    return compile_unit(NULL, unit, lexer, NULL, out);
}

LhatCompileResult lhat_compile_module(const LhatNode *unit,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out)
{
    return compile_unit(NULL, unit, lexer, units, out);
}

LhatCompileResult lhat_compile_next(LhatCompileSession *session,
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
            return "this break^ or next^ names more loops than there are "
                   "around it";
        case LHAT_COMPILE_SCOPE_TOO_FAR:
            return "this reaches out past more scopes than are open here";
    }
    return "unknown";
}
