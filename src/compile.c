// L^ (lhat) -- compiling a tree to bytecode.

#include "compile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/config.h"
#include "lhat/port.h"
#include "grow.h"
// 04 の 2.7: where localerror^.CastFailure's one object lives.
#include "registry.h"
#include "rttype.h"
#include "type.h"

// 9.8: break^ is a normal end for the loop it leaves, so its jump lands where
// last^ and epilog^ are, not past them. The chain is what a break^ written
// inside a nested block still finds.
typedef struct LoopContext {
    struct LoopContext *enclosing;
    size_t *jumps;
    size_t count;
    size_t capacity;
    // 9.11: and where a next^ lands -- the loop's own advance, which is what
    // the end of the body falls into. Kept apart from the jumps above
    // because the two are patched at different places: one past the loop,
    // one just short of the step that ends the turn.
    size_t *nexts;
    size_t next_count;
    size_t next_capacity;
    size_t cleanup_depth;  // what a break^ has to drain back down to
} LoopContext;

// 04 の 4.5: statements or a block with attached catch^ clauses (not a
// try^ block). Error propagation in the guarded statements leaves for these
// clauses rather than for the caller, which is the same
// shape as break^ above -- a jump to be patched, and the cleanups opened
// since to drain on the way. `caught` is where the error waits while the
// arms are chosen; it^ names that register inside each of them.
typedef struct TryContext {
    struct TryContext *enclosing;
    size_t *jumps;
    size_t count;
    size_t capacity;
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
    const LhatNode *declaration;
    // Composition maps each source def^ identity to this one emitted table.
    const struct DefChain *definition_chain;
    // 05 の 8.9: how many consecutive slots the name holds -- 1 for
    // everything but a host value, whose registered width the checker's
    // stamp (or the written annotation) decided at declaration. Reading or
    // writing the name moves this many slots.
    uint8_t width;
    // 09 の 4 章: its entry in the chunk's table of names, closed when the
    // scope is (release_locals).
    size_t table;
} Local;

typedef struct DefChain {
    const LhatNode *parts[LHAT_MAX_DEF_CHAIN];  // base first, derived last
    // 05 の 5.3: the unit a part was written in, when that is not this one.
    // Its top level is where a name free in the part is looked up, and its
    // module path is how what it published is reached while running. Both
    // NULL for a part written here, which needs neither.
    const LhatNode *scopes[LHAT_MAX_DEF_CHAIN];
    const char *modules[LHAT_MAX_DEF_CHAIN];
    // 03 の 4.3: a node carries an offset into the text it was read from, so
    // reading one through another input's lexer answers a different word.
    // A chain may cross inputs -- 'A .. def^{ … }' where A came from an
    // earlier one -- so the lexer belongs to the part and not to the chain.
    const LhatLexer *lexers[LHAT_MAX_DEF_CHAIN];
    size_t count;
} DefChain;

// One subroutine being compiled. Parent frames transport captures selected
// by semantic identity; they do not repeat lexical name resolution.
typedef struct Compiler {
    struct Compiler *parent;
    const LhatLexer *lexer;
    LhatProto *proto;
    const LhatNode *body; // Source function whose running closure this frame holds.
    // Shared, so the first failure sticks -- and carries where it was, which
    // is what a hole in the checker has to be found by (03 の 4.2).
    LhatCompileResult *result;

    Local locals[LHAT_MAX_LOCALS];
    size_t local_count;

    struct {
        const LhatNode *declaration;
        const LhatNode *this_body;
    } upvalue_bindings[LHAT_MAX_UPVALUES];

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

    // 04 の 11.6改: the label a traceback prints for the next body -- the
    // binding or member a FUNC value is being written under. Set just
    // before that value compiles, consumed (and cleared) by
    // compile_subroutine_as. Debug only; 14.9 keeps names out of what a
    // proto is.
    const char *pending_name;
    size_t pending_name_length;

    LoopContext *loop;  // the innermost loop being compiled, NULL outside one
    TryContext *trying;  // 04 の 4.5: and the innermost statements with arms

    // 03 の 4.3: the statements being declared are the top level of a
    // session's input, where a name written again keeps the slot it had.
    bool session_top;
    bool interactive_session;

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

    // 02 の 11.7改2: a guarded postfix run answers nil^ from wherever its
    // first '?' found one, so every guard in the run writes the same
    // destination and jumps to the same place -- the end of the run, which is
    // the node the parser marked. Opened by that node before it compiles
    // itself and closed after, so a run written inside an argument opens one
    // of its own.
    uint8_t chain_into;
    size_t chain_jumps[LHAT_MAX_NIL_CHAIN];
    size_t chain_jump_count;
    bool in_chain;
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

    // The definition being compiled, whose members the entries write and
    // whose prototype the template builds; def^ names it.
    const DefChain *building;

    // 02 の 14.11: this body is a written new's, and the slot is the copy it
    // is adjusting. Every return^ in it answers that slot -- construction
    // answers the copy, whatever the body wrote (15.12's sole expression
    // included).
    bool in_constructor;
    uint8_t constructor_self;

    // 05 の 5 章: where a require^ inside this unit leads. NULL when the unit
    // is being compiled on its own, and then a require^ has nowhere to go.
    const LhatUnits *units;

    // 05 の 5.3: set while a part written in another unit is being compiled.
    // A name free in it is that unit's, and emit_binding_read reaches it the only
    // way a body somewhere else can -- through L^.modules.
    const LhatNode *foreign_scope;
    const char *foreign_module;
} Compiler;

typedef struct ErrorDecl {
    const LhatNode *node;         // the errordef^, for the field defaults
    // 03 の 4.3: read the node through the lexer it came from. An earlier
    // input of a session is still where its offsets mean something.
    const LhatLexer *lexer;
    const LhatErrorKind **kinds;  // one per kind, in declaration order
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
//
// 8.9改: read through a union's arms, since 'Vector3|nil^' needs the
// Vector3's room reserved whichever arm turns up -- the nil^ writes the head
// slot and leaves the rest untouched, which is what makes one reservation
// serve both. The arms a union may carry beside a wide one are exactly the
// ones the head slot's tag tells apart (13.8改's family), so at most one arm
// is ever wide.
static const struct LhatHostValueTag *hostvalue_tag_of(const LhatType *type)
{
    const LhatType *arm = lhat_type_hostvalue_arm(type);
    return arm != NULL ? arm->v.table.hostvalue_tag : NULL;
}

static const struct LhatHostValueTag *hostvalue_of(const LhatNode *node)
{
    return hostvalue_tag_of(node != NULL ? (const LhatType *)node->checked_type
                                         : NULL);
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
static bool runs_nothing(Compiler *c, const LhatNode *node);

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
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
    }
}

static size_t emit_jump(Compiler *c, LhatOpcode op, uint8_t a)
{
    size_t at = lhat_chunk_emit(&c->proto->chunk, lhat_encode_jump(op, a, 0),
                                c->line);
    if (at == SIZE_MAX) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
    }
    return at;
}

// 8.6.4: '?op=' leaves an absent place as it is. The place has just been read
// into `current`; when the '?' spelling was written, everything after this --
// the right-hand side included -- is jumped over unless something is there.
// Answers where that jump waits to be patched, or SIZE_MAX for the plain
// spelling, which skips nothing.
//
// Shaped like 11.7改's '?.' (compile_access): the work is the branch the test
// takes when the place is not nil^, so the right-hand side is evaluated
// exactly as often as 'if^ a? { a op= b }' would evaluate it.
// `carrier` is a slot the skipped arm has to leave something in -- the two-pass
// path reserves one for the result before it branches, and both arms have to
// leave the collector a value it can read. Negative where there is none.
static size_t skip_when_absent(Compiler *c, bool asked, uint8_t current,
                               int carrier)
{
    if (!asked) {
        return SIZE_MAX;
    }
    uint8_t test = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, current, 0));
    size_t to_work = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    if (carrier >= 0) {
        emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, (uint8_t)carrier, 0, 0));
    }
    size_t past = emit_jump(c, LHAT_BC_JUMP, 0);
    lhat_chunk_patch_here(&c->proto->chunk, to_work);
    return past;
}

static void land_here(Compiler *c, size_t jump)
{
    if (jump != SIZE_MAX) {
        lhat_chunk_patch_here(&c->proto->chunk, jump);
    }
}

// 02 の 11.7改2: what a guarded link does instead of landing its own jump.
// Every '?' in the run answers the same nil^ from the same place, so the jump
// waits until the run's last access has been emitted.
typedef struct {
    uint8_t into;
    size_t jumps[LHAT_MAX_NIL_CHAIN];
    size_t count;
    bool outer;  // whether a run was already open around this one
    bool open;   // whether this node opened one, so this frame has to be put back
} ChainFrame;

// Only the node the parser marked opens one. A link inside the run leaves the
// frame alone -- saving and putting back around one would discard the guard it
// just pushed, since the guard belongs to the run and not to the link.
static void chain_open(Compiler *c, const LhatNode *node, uint8_t into,
                       ChainFrame *saved)
{
    saved->open = false;
    if (!node->v.access.nil_chain_end) {
        return;
    }
    saved->open = true;
    saved->into = c->chain_into;
    saved->count = c->chain_jump_count;
    saved->outer = c->in_chain;
    memcpy(saved->jumps, c->chain_jumps, sizeof saved->jumps);
    c->chain_into = into;
    c->chain_jump_count = 0;
    c->in_chain = true;
}

// Lands every guard the run left open, then puts back whatever run this one
// was written inside of.
static void chain_close(Compiler *c, ChainFrame *saved)
{
    if (!saved->open) {
        return;
    }
    for (size_t i = 0; i < c->chain_jump_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, c->chain_jumps[i]);
    }
    c->chain_into = saved->into;
    c->chain_jump_count = saved->count;
    c->in_chain = saved->outer;
    memcpy(c->chain_jumps, saved->jumps, sizeof saved->jumps);
}

// The guard one '?' emits: nil^ into the run's destination, then a jump that
// the run's end will land. Answers false when the run has no room left, which
// is a program past LHAT_MAX_NIL_CHAIN and is reported as too complex.
static bool chain_guard(Compiler *c, uint8_t target)
{
    if (!c->in_chain || c->chain_jump_count == LHAT_MAX_NIL_CHAIN) {
        return false;
    }
    uint8_t test = c->next_register;
    (void)reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ISNIL, test, target, 0));
    size_t to_access = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
    c->next_register = test;
    emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, c->chain_into, 0, 0));
    c->chain_jumps[c->chain_jump_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
    lhat_chunk_patch_here(&c->proto->chunk, to_access);
    return true;
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into);

// 8.6.4 for a place that is a name. There is no owner or key to run twice,
// so the place is read the way the compound value would read it -- one
// register read for a local, one GETUPVAL for a captured one -- and the test
// is made of that before anything else is evaluated. The slot is given back
// straight away: the branch below it is reached only after the test has been
// made, so nothing there can see the register change under it.
static size_t skip_name_when_absent(Compiler *c, bool asked,
                                    const LhatNode *target)
{
    if (!asked) {
        return SIZE_MAX;
    }
    uint8_t mark = c->next_register;
    uint8_t now = reserve(c);
    compile_expression(c, target, now);
    size_t past = skip_when_absent(c, true, now, -1);
    c->next_register = mark;
    return past;
}

static void load_constant(Compiler *c, uint8_t into, LhatValue value)
{
    size_t k = lhat_chunk_constant(&c->proto->chunk, value);
    if (k == SIZE_MAX) {
        fail(c, c->proto->chunk.constant_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

// 5.2: a name is a slot in the frame, declared here and nowhere else. The
// same declaration writes the chunk's table of names (09 の 4 章), which is
// where the debugger reads what a register was called. NULL, with the
// failure recorded, when the body has too many names.
static Local *declare_local(Compiler *c, const char *name, size_t length,
                            uint8_t reg, uint8_t width)
{
    size_t table = c->local_count < LHAT_MAX_LOCALS
                       ? lhat_chunk_add_local(&c->proto->chunk, name, length,
                                              reg, width)
                       : SIZE_MAX;
    if (table == SIZE_MAX) {
        fail(c, c->local_count >= LHAT_MAX_LOCALS
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return NULL;
    }
    Local *local = &c->locals[c->local_count++];
    local->name = name;
    local->length = length;
    local->reg = reg;
    local->width = width;
    local->table = table;
    local->declaration = NULL;
    local->definition_chain = NULL;
    return local;
}

// The names declared since `mark` go out of scope here: the next
// instruction is the first they are not live at.
static void release_locals(Compiler *c, size_t mark)
{
    LhatChunk *chunk = &c->proto->chunk;
    for (size_t i = mark; i < c->local_count; i++) {
        chunk->locals[c->locals[i].table].to = (uint32_t)chunk->count;
    }
    c->local_count = mark;
}

static const Local *local_for_binding(const Compiler *c, const LhatNode *binding)
{
    if (binding == NULL) return NULL;
    for (size_t i = c->local_count; i > 0; i--) {
        if (c->locals[i - 1].declaration == binding) return &c->locals[i - 1];
        const DefChain *chain = c->locals[i - 1].definition_chain;
        if (chain != NULL) {
            for (size_t j = 0; j < chain->count; j++) {
                if (chain->parts[j] == binding) return &c->locals[i - 1];
            }
        }
    }
    return NULL;
}

static size_t capture_binding(Compiler *c, const LhatNode *binding,
                               const char *name, size_t length)
{
    if (c->parent == NULL || binding == NULL) return SIZE_MAX;
    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_bindings[i].declaration == binding) return i;
    }
    const Local *local = local_for_binding(c->parent, binding);
    size_t index;
    LhatUpvalueSource source;
    if (local != NULL) {
        if (local->width > 1) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return SIZE_MAX;
        }
        source = LHAT_UPVALUE_REGISTER;
        index = local->reg;
    } else {
        source = LHAT_UPVALUE_OUTER;
        index = capture_binding(c->parent, binding, name, length);
        if (index == SIZE_MAX) return SIZE_MAX;
    }
    size_t added = lhat_proto_add_upvalue(c->proto, source, (uint8_t)index, name, length);
    if (added == SIZE_MAX) {
        fail(c, c->proto->upvalue_count >= LHAT_MAX_UPVALUES
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return SIZE_MAX;
    }
    c->upvalue_bindings[added].declaration = binding;
    return added;
}

// 15.10: capture the closure of the body selected by semantic analysis.
// No register ever holds an enclosing body's closure
// (BC_THIS reads the running frame), so the capture is the third upvalue
// source: at CLOSURE time the maker boxes its own closure, and the chain
// through any intermediate bodies is the ordinary one. Cache by body identity,
// independently of the written hat count and intermediate generated frames.
static size_t capture_this_body(Compiler *c, const LhatNode *body)
{
    if (c->parent == NULL || body == NULL) return SIZE_MAX;
    for (size_t i = 0; i < c->proto->upvalue_count; i++) {
        if (c->upvalue_bindings[i].this_body == body) {
            return i;
        }
    }

    size_t added;
    if (c->parent->body == body) {
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_THIS, 0, "this^",
                                       5);
    } else {
        size_t outer = capture_this_body(c->parent, body);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_OUTER,
                                       (uint8_t)outer, "this^", 5);
    }
    if (added == SIZE_MAX) {
        fail(c, c->proto->upvalue_count >= LHAT_MAX_UPVALUES
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return SIZE_MAX;
    }
    c->upvalue_bindings[added].this_body = body;
    return added;
}

// ---------------------------------------------------------------------------
// Storage for checked scope-qualified references (01 の 8 章)
// ---------------------------------------------------------------------------

static Compiler *root_of(Compiler *c);

// Where a checked reference is stored. The invalid-scope answer is retained
// from analysis, including when the low-level API emits code with diagnostics.
typedef enum {
    SCOPED_NONE,      // no resolved storage
    SCOPED_TOO_FAR,   // analysis rejected the scope specifier
    SCOPED_REGISTER,
    SCOPED_UPVALUE
} ScopedKind;

static ScopedKind binding_location(Compiler *c, const LhatNode *node,
                                 const char *name, size_t length,
                                 uint8_t *reg, size_t *upvalue)
{
    if (node->checked_binding != NULL) {
        const Local *local = local_for_binding(c, node->checked_binding);
        if (local != NULL) {
            *reg = local->reg;
            return SCOPED_REGISTER;
        }
        *upvalue = capture_binding(c, node->checked_binding, name, length);
        return *upvalue != SIZE_MAX ? SCOPED_UPVALUE : SCOPED_NONE;
    }
    return node->checked_scope_invalid ? SCOPED_TOO_FAR : SCOPED_NONE;
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
static void compile_yield_wide(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved);

// Lays a tuple answer into the run at `into`. Only called where is_run_source
// said yes -- or, for a yield^, where compile_define found several names
// binding one (13.8改: the resume's send comes back as a run there).
static void compile_run_source(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved)
{
    if (node->kind == LHAT_NODE_TRY) {
        compile_try_wide(c, node, into, reserved);
    } else if (node->kind == LHAT_NODE_YIELD) {
        compile_yield_wide(c, node, into, reserved);
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
static void compile_bind_path(Compiler *c, const LhatNode *node,
                               const char *path, uint8_t value);
static LhatRuntimeType *lower_type(Compiler *c, const LhatNode *node);
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

static const ErrorDecl *find_error_decl(Compiler *c, const LhatNode *node)
{
    const Compiler *root = root_of(c);
    for (size_t i = 0; i < root->error_count; i++) {
        const ErrorDecl *decl = &root->errors[i];
        if (decl->node == node) {
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
    if (find_error_decl(c, node) != NULL) {
        return;  // already registered; 8.7's pre-pass may reach it twice
    }

    Compiler *root = root_of(c);
    LhatChunk *chunk = &root->proto->chunk;

    if (root->error_count == root->error_capacity) {
        size_t grown = root->error_capacity ? root->error_capacity * 2 : 4;
        ErrorDecl *bigger =
            (ErrorDecl *)lhat_realloc(root->errors, grown * sizeof *bigger);
        if (bigger == NULL) {
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
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
        lhat_error_kind_new(&chunk->heap, NULL, node->v.named.local, group_name);
    if (group_name == NULL || group == NULL || kinds == NULL) {
        lhat_free(kinds);
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }

    size_t index = 0;
    if (node->checked_type != NULL) {
        ((LhatType *)node->checked_type)->v.error.runtime_kind = group;
    }
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
            text != NULL ? lhat_error_kind_new(&chunk->heap, group, false, text)
                         : NULL;
        if (kind == NULL) {
            lhat_free(kinds);
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
            return;
        }
        kinds[index] = kind;
        if (k->checked_type != NULL) {
            ((LhatType *)k->checked_type)->v.error.runtime_kind = kind;
        }
    }

    ErrorDecl *decl = &root->errors[root->error_count++];
    decl->node = node;
    decl->lexer = c->lexer;
    decl->kinds = kinds;
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

// Map the already-resolved error kind to its runtime identity and source.
// No spelling or qualification is interpreted by the emitter.
static const LhatErrorKind *compiled_error_kind(Compiler *c, const LhatType *type,
                                                const LhatLexer **lexer)
{
    *lexer = NULL;
    if (type == NULL || type->kind != LHAT_TYPE_ERROR_KIND) {
        return NULL;
    }
    if (type->v.error.declaration == NULL) return type->v.error.runtime_kind;
    Compiler *root = root_of(c);
    for (size_t i = 0; i < root->error_count; i++) {
        const ErrorDecl *decl = &root->errors[i];
        size_t index = 0;
        for (const LhatNode *k = decl->node->v.named.members; k != NULL; k = k->next, index++) {
            if (k == type->v.error.declaration) {
                *lexer = decl->lexer;
                return decl->kinds[index];
            }
        }
    }
    return NULL;
}

static void load_string_bytes(Compiler *c, uint8_t into, const char *text,
                              size_t length)
{
    size_t k = lhat_chunk_string(&c->proto->chunk, text, length);
    if (k == SIZE_MAX) {
        fail(c, c->proto->chunk.constant_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_LOADK, into, (uint16_t)k));
}

// STRING and NAME both hold a span of the lexer's decoded bytes, so the
// escapes of 01 の 5 章 are already resolved by the time the compiler sees
// them.
static void load_string(Compiler *c, uint8_t into, const LhatNode *node)
{
    // A unit whose only string is the empty one leaves the lexer with no
    // buffer at all, and offsetting a null pointer is undefined however
    // plainly zero the offset is.
    const char *bytes = c->lexer->strings != NULL
                            ? c->lexer->strings + node->v.string.offset
                            : "";
    load_string_bytes(c, into, bytes, node->v.string.length);
}

// The key of a member access or an index. 01 の 10.1 makes digits after a '.'
// an integer key, and a name after it a string key, so the two forms differ
// only in how the key was written.
// 03 の 5.1改: a cache for the member `node` reads, or SIZE_MAX where the
// site cannot have one -- an INDEX (the key changes), a name the compiler
// cannot spell, a chunk that has run out of the 256 a byte can name. The
// caller then emits the unspecialised read, which answers the same thing.
static size_t member_cache_named(Compiler *c, const char *name, size_t length)
{
    if (c->proto->chunk.member_cache_count >= 256) {
        return SIZE_MAX;
    }
    size_t k = lhat_chunk_string(&c->proto->chunk, name, length);
    if (k == SIZE_MAX) {
        return SIZE_MAX;
    }
    return lhat_chunk_member_cache(&c->proto->chunk, (uint16_t)k);
}

static size_t member_cache_for(Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (node->kind != LHAT_NODE_MEMBER ||
        !node_name(c, node->v.access.argument, &name, &length)) {
        return SIZE_MAX;
    }
    return member_cache_named(c, name, length);
}

// A member the runtime calls on its own account -- iterate^ for a walk,
// dispose for a cleanup -- read off R[callee + 1] into R[callee] for the
// method call emitted right after this. Fused (CALLMEMBER) so a built-in
// answer needs no receiver bound into it; past 256 sites it is the plain
// GETINDEX, the same answer made the long way.
static void emit_method_read(Compiler *c, uint8_t callee, const char *name,
                             size_t length)
{
    uint8_t receiver = (uint8_t)(callee + 1);
    size_t cache = member_cache_named(c, name, length);
    if (cache != SIZE_MAX) {
        emit(c, lhat_encode_abc(LHAT_BC_CALLMEMBER, callee, receiver,
                                (uint8_t)cache));
        return;
    }
    uint8_t key = reserve(c);
    load_string_bytes(c, key, name, length);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, callee, receiver, key));
}

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
        fail(c, c->proto->chunk.constant_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
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
    const LhatType *type = node->checked_type;
    const LhatLexer *declaring_lexer = NULL;
    const LhatErrorKind *kind =
        compiled_error_kind(c, type, &declaring_lexer);
    if (kind == NULL) {
        // Naming the declaration rather than one of its kinds leaves nothing
        // to construct: 2.3 makes it the union, not a type of its own.
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    const LhatNode *kind_node = type->v.error.declaration;

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
    // kind_node is NULL for a host-registered kind --
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
        if (!lhat_node_name(field->v.param.name, declaring_lexer->source->text,
                            declaring_lexer->strings, &name, &length)) {
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
        const LhatLexer *caller_lexer = c->lexer;
        c->lexer = declaring_lexer;
        compile_expression(c, fallback, value);
        c->lexer = caller_lexer;
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
// when nothing stands between here and the caller, and to the nearest catch^
// arms otherwise -- draining what was opened on the way, the same as break^
// leaving the loops it passes through.
static void emit_error_escape(Compiler *c, uint8_t from)
{
    TryContext *target = c->trying;
    if (target == NULL) {
        emit(c, lhat_encode_abc(LHAT_BC_RETURN, from, 0, 0));
        return;
    }
    if (target->count >= SIZE_MAX / sizeof *target->jumps / 2) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    LHAT_GROW(target->jumps, target->count, target->capacity, 16,
              { fail(c, LHAT_COMPILE_OUT_OF_MEMORY); return; });
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

    Local *binding = declare_local(c, "it^", 3, caught, 1);
    if (binding == NULL) return;
    binding->declaration = node;  // catch^ opens no brace of its own
    // 04 の 4.1改: the arm that does not come back. Nothing is written into
    // `into` and nothing needs to be -- what falls through to the join below
    // is the left having succeeded, and this side never reaches it. The same
    // holds for a run: the arm leaves the positions alone because it leaves.
    if (node->v.binary.right != NULL &&
        node->v.binary.right->kind == LHAT_NODE_PANIC) {
        compile_statement(c, node->v.binary.right);
    } else if (reserved > 1 && is_run_source(node->v.binary.right)) {
        compile_run_source(c, node->v.binary.right, into, reserved);
    } else {
        compile_expression(c, node->v.binary.right, into);
    }

    lhat_chunk_patch_here(&c->proto->chunk, past);
    release_locals(c, local_mark);
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

// 02 の 19 章: enum^ E { AAA, BBB = expr } builds its objects where the
// declaration stands. The identity rides an RT_ENUM descriptor made here
// and loaded as a constant -- the same object a fits^ against E compares
// (rt_from_checked stamps the same checker declaration). A member with no
// written value takes the running number: 0 to start, an integer literal
// resets the run to itself plus one, any other written value leaves the
// count where it was.
static void compile_enumdef(Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.named.name, &name, &length)) {
        return;
    }

    LhatHeap *owner_heap = &root_of(c)->proto->chunk.heap;
    LhatRuntimeType *decl_rt = lhat_type_rt_new(owner_heap, LHAT_TYPE_RT_ENUM);
    if (decl_rt == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    decl_rt->enum_decl = node->checked_type;  // NULL unchecked: fits^ needs
                                              // the checker anyway
    decl_rt->enum_name = lhat_string_new(owner_heap, name, length);
    if (decl_rt->enum_name == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }

    const Local *binding = local_for_binding(c, node->v.named.name->checked_binding);
    if (binding == NULL) {
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    uint8_t reg = binding->reg;
    size_t k = lhat_chunk_constant(&c->proto->chunk,
                                   lhat_object((LhatObject *)decl_rt));
    if (k == SIZE_MAX) {
        fail(c, c->proto->chunk.constant_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_NEWENUM, reg, (uint16_t)k));

    int64_t running = 0;
    uint8_t mark = c->next_register;
    uint8_t member_name = reserve(c);
    uint8_t member_value = reserve(c);
    for (const LhatNode *member = node->v.named.members; member != NULL;
         member = member->next) {
        const char *this_name = NULL;
        size_t this_length = 0;
        if (!node_name(c, member->v.named.name, &this_name, &this_length)) {
            continue;
        }
        load_string_bytes(c, member_name, this_name, this_length);
        const LhatNode *written = member->v.named.members;
        if (written == NULL) {
            load_constant(c, member_value, lhat_integer(running));
            running++;
        } else {
            compile_expression(c, written, member_value);
            if (written->kind == LHAT_NODE_INT) {
                running = written->v.integer.value + 1;
            }
        }
        emit(c, lhat_encode_abc(LHAT_BC_NEWENUMERATOR, reg, member_name,
                                member_value));
    }
    c->next_register = mark;
}

// 02 の 13.11: fits^ asks whether the left side may stand where the right side
// is written. Every spelling of the right side is that one question at run
// time, so there is one instruction for it: lower_type turns the written type
// into the descriptor LHAT_BC_FITS tests a value against, which is the same
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
// pure data, arbitrarily large).
//
// What lower_type settles it from is the words written, and where those run
// out it reads what the checker resolved the name to instead -- a name bound
// to a type or to a module is spelt nothing like what was registered, and the
// compiler settling the type never meant settling it from the spelling alone.
//
// So NULL from lower_type is answered by what was written: any^ makes the
// question empty (13.7; check.c reports the writing itself), a name that
// reached no type at all is UNDEFINED, and anything else is a written form
// nothing settles (UNSUPPORTED).
// The test itself, against a left operand already in a register. 11.5 の (5)
// shares an operand between two links of a chain and evaluates it once, so
// there the left is compiled by the caller.
// Lenient conformance forgives inference gaps. Those are not evidence for
// constant folding, including gaps nested in members or callable signatures.
// Keep any^ dynamic too: a named any^ field requires runtime presence, whereas
// ordinary static conformance also accepts nil^ as an any^ value.
typedef struct FitsTypeSeen {
    const LhatType *type;
    const struct FitsTypeSeen *outer;
} FitsTypeSeen;

static bool fits_type_settled(const LhatType *type, const FitsTypeSeen *seen)
{
    if (type == NULL) return true; // An absent optional signature component.
    if (type->kind == LHAT_TYPE_UNKNOWN || type->kind == LHAT_TYPE_PENDING ||
        type->kind == LHAT_TYPE_ANY)
        return false;
    for (const FitsTypeSeen *s = seen; s != NULL; s = s->outer) {
        if (s->type == type) return true;
    }
    FitsTypeSeen here = {type, seen};
    seen = &here;
    if (type->specialization_base != NULL || type->template_definition != NULL)
        return false; // Runtime descriptors erase specialization arguments.
    // 05 の 8.8: a registered type is its tag; its members are never asked
    // of a value, so their gaps are not this question's either.
    if (type->kind == LHAT_TYPE_TABLE && type->v.table.hostdata_tag != NULL)
        return true;
    switch (type->kind) {
        case LHAT_TYPE_ARGUMENT:
            return type->v.argument.bound != NULL &&
                   fits_type_settled(type->v.argument.bound, seen);
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
        case LHAT_TYPE_TUPLE:
            for (const LhatTypeList *a = type->v.composite.arms; a; a = a->next) {
                if (a->type == NULL || !fits_type_settled(a->type, seen)) return false;
            }
            return true;
        case LHAT_TYPE_TABLE:
        case LHAT_TYPE_HOSTVALUE:
        case LHAT_TYPE_HOSTVALUE_BOX:
            for (const LhatTypeMember *m = type->v.table.members; m; m = m->next) {
                if (m->type == NULL || !fits_type_settled(m->type, seen)) return false;
            }
            return fits_type_settled(type->v.table.base, seen) &&
                   fits_type_settled(type->v.table.delegate, seen) &&
                   fits_type_settled(type->v.table.instance, seen) &&
                   fits_type_settled(type->v.table.variadic, seen) &&
                   fits_type_settled(type->v.table.index_key, seen) &&
                   fits_type_settled(type->v.table.index_value, seen);
        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p; p = p->next) {
                if (p->type == NULL || !fits_type_settled(p->type, seen)) return false;
            }
            return fits_type_settled(type->v.func.variadic, seen) &&
                   fits_type_settled(lhat_type_call_answer(type), seen);
        case LHAT_TYPE_CORO:
            return fits_type_settled(type->v.coroutine.receive, seen) &&
                   fits_type_settled(type->v.coroutine.produce, seen) &&
                   fits_type_settled(type->v.coroutine.result, seen);
        default:
            return true;
    }
}

static bool fits_types_disjoint(const LhatType *actual, const LhatType *target)
{
    actual = lhat_type_argument_bound(actual);
    target = lhat_type_argument_bound(target);
    if (actual == NULL || target == NULL) return false;
    if (lhat_type_disjoint(actual, target)) return true;
    if (actual->kind == LHAT_TYPE_UNION || target->kind == LHAT_TYPE_UNION) {
        const LhatType *set = actual->kind == LHAT_TYPE_UNION ? actual : target;
        const LhatType *other = set == actual ? target : actual;
        for (const LhatTypeList *a = set->v.composite.arms; a; a = a->next) {
            if (!fits_types_disjoint(a->type, other)) return false;
        }
        return true;
    }
    // Static disjointness is deliberately conservative for overloads. A
    // callable's calling convention nevertheless cannot change. A p^ type
    // can contain an f^ value, so different effect bounds are not disjoint.
    if (actual->kind == LHAT_TYPE_FUNC && target->kind == LHAT_TYPE_FUNC) {
        if (actual->v.func.takes_self != target->v.func.takes_self ||
            actual->v.func.self_last != target->v.func.self_last ||
            (actual->v.func.variadic == NULL) != (target->v.func.variadic == NULL))
            return true;
        const LhatTypeList *a = actual->v.func.params, *b = target->v.func.params;
        while (a != NULL && b != NULL) { a = a->next; b = b->next; }
        if (a != NULL || b != NULL) return true;
        const LhatType *from = lhat_type_call_answer(actual);
        const LhatType *into = lhat_type_call_answer(target);
        if ((from == NULL) != (into == NULL)) return true;
        return from != NULL && lhat_type_disjoint(from, into);
    }
    return false;
}

// A union whose arms each settle on one side of the target, with nil^ alone on
// the other: the test is x? (or its negation). Answers whether nil^ fits, or
// -1 when the arms do not split that way.
static int fits_split_by_nil(const LhatType *actual, const LhatType *target)
{
    actual = lhat_type_argument_bound(actual);
    target = lhat_type_argument_bound(target);
    if (actual->kind != LHAT_TYPE_UNION) return -1;
    int nil_fits = -1, rest_fits = -1;
    for (const LhatTypeList *a = actual->v.composite.arms; a; a = a->next) {
        const LhatType *arm = lhat_type_argument_bound(a->type);
        int side = lhat_type_conforms(arm, target) ? 1
                 : fits_types_disjoint(arm, target) ? 0 : -1;
        int *seen = arm->kind == LHAT_TYPE_NIL ? &nil_fits : &rest_fits;
        if (side < 0 || (*seen >= 0 && *seen != side)) return -1;
        *seen = side;
    }
    return nil_fits >= 0 && rest_fits >= 0 && nil_fits != rest_fits ? nil_fits : -1;
}

// The answer when it is settled before the run: 1 or 0, and -1 when only
// the value can say.
static int fits_settled_answer(Compiler *c, const LhatNode *asked,
                               const LhatType *actual)
{
    const char *name = NULL;
    size_t length = 0;
    if (node_name(c, asked, &name, &length) && name_is(name, length, "any^")) {
        return 1;
    }
    const LhatType *target = asked->checked_type;
    if (actual != NULL && target != NULL &&
        fits_type_settled(actual, NULL) && fits_type_settled(target, NULL)) {
        if (lhat_type_conforms(actual, target)) return 1;
        if (fits_types_disjoint(actual, target)) return 0;
    }
    return -1;
}

static void compile_fits_test(Compiler *c, const LhatNode *asked,
                             const LhatType *actual, uint8_t value,
                             uint8_t into)
{
    int settled = fits_settled_answer(c, asked, actual);
    if (settled >= 0) {
        emit(c, lhat_encode_abc(LHAT_BC_LOADBOOL, into, (uint8_t)settled, 0));
        return;
    }

    const LhatType *target = asked->checked_type;
    if (actual != NULL && target != NULL &&
        fits_type_settled(actual, NULL) && fits_type_settled(target, NULL)) {
        int nil_fits = fits_split_by_nil(actual, target);
        if (nil_fits >= 0) {
            emit(c, lhat_encode_abc(LHAT_BC_ISNIL, into, value, 0));
            if (!nil_fits) emit(c, lhat_encode_abc(LHAT_BC_NOT, into, into, 0));
            return;
        }
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
    emit(c, lhat_encode_abc(LHAT_BC_FITS, into, value, holder));
    c->next_register = mark;
}

static void compile_fits(Compiler *c, const LhatNode *node, uint8_t into)
{
    // The left side runs whatever the answer turns out to be -- for an any^
    // it is still the reason compile_expression keeps typeof^'s operand.
    uint8_t mark = c->next_register;
    // 05 の 8.9: a host value operand keeps its width here as anywhere.
    uint8_t value = reserve_for(c, node->v.binary.left);
    compile_expression(c, node->v.binary.left, value);
    compile_fits_test(c, node->v.binary.right,
                      node->checked_fits_type, value, into);
    c->next_register = mark;
}

// All written types have been resolved by the common semantic pass. Code
// generation translates that result; it never interprets type syntax again.
static LhatRuntimeType *runtime_type(Compiler *c, const LhatType *type)
{
    while (type != NULL && type->kind == LHAT_TYPE_ARGUMENT) type = type->v.argument.bound;
    LhatRuntimeType *rt = lhat_rt_from_checked(&root_of(c)->proto->chunk.heap, type);
    if (rt == NULL && type != NULL && type->kind != LHAT_TYPE_NONE) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
    }
    return rt;
}

static LhatRuntimeType *lower_type(Compiler *c, const LhatNode *node)
{
    if (node == NULL) {
        return NULL;
    }
    if (node->checked_type == NULL) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return NULL;
    }
    return runtime_type(c, node->checked_type);
}

// ---------------------------------------------------------------------------
// 02 の 14 章: the object model
// ---------------------------------------------------------------------------

// L^.modules, then one key per segment of `path`, then `name` if there is
// one. The same walk an import^ makes, which is what makes it the only walk
// a body somewhere else can make: a registration is an object on the heap of
// the machine it was installed on, so nothing here is captured.
static void emit_modules_read(Compiler *c, const char *path, const char *name,
                              size_t name_length, uint8_t into)
{
    uint8_t mark = c->next_register;
    uint8_t key = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
    load_string_bytes(c, key, "modules", 7);
    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
    for (const char *segment = path; segment != NULL;) {
        size_t length = strcspn(segment, ".");
        load_string_bytes(c, key, segment, length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
        segment = segment[length] == '.' ? segment + length + 1 : NULL;
    }
    if (name != NULL) {
        load_string_bytes(c, key, name, name_length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
    }
    c->next_register = mark;
}

// A lexical use carries its declaration, independent of scope depth or name.
// Only storage placement and capture transport are decided here.
static bool emit_binding_read(Compiler *c, const LhatNode *binding,
                               const char *name, size_t length, uint8_t into)
{
    if (binding == NULL) return false;
    const Local *local = local_for_binding(c, binding);
    if (local != NULL) {
        emit_move_wide(c, into, local->reg, local->width);
        return true;
    }
    size_t upvalue = capture_binding(c, binding, name, length);
    if (upvalue != SIZE_MAX) {
        emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
        return true;
    }
    // A flattened foreign body has no frame for its module's top-level
    // locals. Map the resolved declaration to that module's export storage,
    // rather than resolving its spelling in this unit's environment again.
    Compiler *part = c;
    while (part != NULL && part->foreign_scope == NULL) part = part->parent;
    if (part == NULL || part->foreign_module == NULL) return false;
    for (const LhatNode *s = part->foreign_scope; s != NULL; s = s->next) {
        if (s->kind == LHAT_NODE_ENUMDEF && s->v.named.name == binding) {
            const char *key = NULL;
            size_t key_length = 0;
            if (!lhat_node_name(s->v.named.name, part->lexer->source->text, part->lexer->strings,
                                &key, &key_length)) return false;
            if (!s->v.named.exported) {
                fail_named(c, LHAT_COMPILE_NOT_PUBLISHED, key, key_length);
                return true;
            }
            emit_modules_read(c, part->foreign_module, key, key_length, into);
            return true;
        }
        if (s->kind != LHAT_NODE_DEFINE) continue;
        for (const LhatNode *target = s->v.binding.targets; target != NULL; target = target->next) {
            const LhatNode *declared = target->kind == LHAT_NODE_PARAM
                                          ? target->v.param.name : target;
            if (declared != binding) continue;
            const char *key = NULL;
            size_t key_length = 0;
            if (!lhat_node_name(declared, part->lexer->source->text, part->lexer->strings,
                                &key, &key_length)) return false;
            if (!s->v.binding.exported) {
                fail_named(c, LHAT_COMPILE_NOT_PUBLISHED, key, key_length);
                return true;
            }
            emit_modules_read(c, part->foreign_module, key, key_length, into);
            return true;
        }
    }
    return false;
}

static void compile_default_new(Compiler *c, const LhatNode *node,
                                uint8_t definition);

// Flatten the semantic composition tree. The emitter never follows source
// names, require expressions or exported spellings to rediscover its parts.
static bool append_definition(Compiler *c, const LhatDefinition *origin,
                               DefChain *out, size_t depth)
{
    if (origin == NULL || depth > LHAT_MAX_DEF_CHAIN) return false;
    if (origin->literal == NULL) {
        return append_definition(c, origin->left, out, depth + 1) &&
               append_definition(c, origin->right, out, depth + 1);
    }
    if (out->count >= LHAT_MAX_DEF_CHAIN) return false;
    Compiler *root = root_of(c);
    bool foreign = origin->lexer != root->lexer && !root->interactive_session;
    if (foreign && origin->module == NULL) return false;
    size_t at = out->count++;
    out->parts[at] = origin->literal;
    out->lexers[at] = origin->lexer;
    out->scopes[at] = foreign ? origin->statements : NULL;
    out->modules[at] = foreign ? origin->module : NULL;
    return true;
}

static bool def_chain_of(Compiler *c, const LhatNode *node, DefChain *out)
{
    return node != NULL && append_definition(c, node->checked_definition, out, 0);
}

// The template of one def^, or NULL. 14.13 allows one per definition, and it
// is the entry with no key.
static const LhatNode *template_of(const LhatNode *def)
{
    for (const LhatNode *entry = def->v.list.items; entry != NULL;
         entry = entry->next) {
        // 14.7改2: a delegate^ carries no key either. The template is the
        // entry with neither a key nor that marker.
        if (entry->v.entry.key == NULL &&
            entry->v.entry.modifier != LHAT_DEF_DELEGATE) {
            return entry->v.entry.value;
        }
    }
    return NULL;
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
    const LhatNode *enclosing_scope = c->foreign_scope;
    const char *enclosing_module = c->foreign_module;
        c->lexer = chain->lexers[i];
        c->foreign_scope = chain->scopes[i];
        c->foreign_module = chain->modules[i];
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
                c->foreign_scope = enclosing_scope;
                c->foreign_module = enclosing_module;
    }
    return plain > 1;
}

// 02 の 14.11: which body compile_subroutine_as is making. A written new is
// compiled twice -- once as the constructor the definition holds, whose
// frame the machine's construction opens, and once as the hook super^ names
// (14.12改), which runs the same body against a receiver it is handed
// instead of making one.
typedef enum {
    LHAT_BODY_ORDINARY,
    LHAT_BODY_CONSTRUCTOR,
    LHAT_BODY_NEW_HOOK
} BodyKind;

static void compile_subroutine_as(Compiler *c, const LhatNode *node,
                                  uint8_t into, BodyKind kind);

// 14.11: 'self^{ … }' writes the named fields onto the self^ in scope, one
// assignment per field, and stands for what it wrote. Construction is not
// here: the machine copies the definition's prototype (NEWINSTANCE), and
// this is how a written new adjusts the copy. The checker holds the notation
// to new bodies (SELF_TABLE_OUTSIDE_NEW); what compiles is general, the way
// every instruction is.
static void compile_self_assign(Compiler *c, const LhatNode *node,
                                uint8_t into)
{
    uint8_t mark = c->next_register;
    uint8_t self = reserve(c);
    // 14.11: outside a body that holds a receiver the spelling means nothing.
    if (!emit_binding_read(c, node->checked_receiver, "self^", 5, self)) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        // 14.15: a declaration carries no value to write.
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
        emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, self, key, value));
        c->next_register = at;
    }
    emit_move_wide(c, into, self, 1);
    c->next_register = mark;
}

// 14.12改: what super^ means inside an override^ new -- the hook of the new
// written before it, run against the same receiver. Every written new ahead
// of `stop_entry` in the chain is compiled once more as a hook (the same
// body without the construction). Each override identity is bound to its
// predecessor's hook. The chain starts on a hook that
// does nothing: the default new has nothing to run but the construction.
static void bind_new_hooks(Compiler *c, const DefChain *chain,
                           size_t stop_part, const LhatNode *stop_entry)
{
    LhatProto *idle = lhat_proto_new();
    if (idle == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    idle->is_function = true;
    idle->takes_self = true;
    idle->parameters = 1;
    idle->parameter_slots = 1;
    size_t index = lhat_proto_add(c->proto, idle);
    if (index == SIZE_MAX) {
        lhat_proto_free(idle);
        fail(c, c->proto->proto_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    if (lhat_chunk_emit(&idle->chunk,
                        lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0),
                        c->line) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    uint8_t hook = reserve(c);
    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, hook, (uint16_t)index));
    for (size_t i = 0; i <= stop_part && i < chain->count; i++) {
        const LhatLexer *enclosing_lexer = c->lexer;
        const LhatNode *enclosing_scope = c->foreign_scope;
        const char *enclosing_module = c->foreign_module;
        c->lexer = chain->lexers[i];
        c->foreign_scope = chain->scopes[i];
        c->foreign_module = chain->modules[i];
        for (const LhatNode *entry = chain->parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (i == stop_part && entry == stop_entry) {
                break;
            }
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key == NULL || entry->v.entry.declared ||
                !node_name(c, entry->v.entry.key, &name, &length) ||
                !name_is(name, length, "new") ||
                entry->v.entry.value == NULL ||
                entry->v.entry.value->kind != LHAT_NODE_FUNC) {
                continue;
            }
            Local *replaced = declare_local(c, "super^", 6, hook, 1);
            if (replaced == NULL) {
                break;
            }
            replaced->declaration = entry;
            hook = reserve(c);
            compile_subroutine_as(c, entry->v.entry.value, hook,
                                  LHAT_BODY_NEW_HOOK);
        }
        c->lexer = enclosing_lexer;
        c->foreign_scope = enclosing_scope;
        c->foreign_module = enclosing_module;
    }
    Local *replaced = declare_local(c, "super^", 6, hook, 1);
    if (replaced != NULL) replaced->declaration = stop_entry;
}

// 14.1 and 14.3: a definition is a table of the members every instance
// shares, plus the prototype its self^ member holds (14.11) -- the template,
// with every initialiser evaluated once, here at the definition. An instance
// is a copy of that prototype.
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

    // def^ names the definition (14.4), and binding it as an ordinary local
    // is what lets a method or an initialiser reach it -- through the capture
    // of 5.4, with nothing special added.
    size_t local_mark = c->local_count;
    Local *definition = declare_local(c, "def^", 4, into, 1);
    if (definition == NULL) {
        return;
    }
    definition->definition_chain = &chain;

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
        const LhatNode *enclosing_scope = c->foreign_scope;
        const char *enclosing_module = c->foreign_module;
        c->lexer = chain.lexers[i];
        c->foreign_scope = chain.scopes[i];
        c->foreign_module = chain.modules[i];
        for (const LhatNode *entry = chain.parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (entry->v.entry.key == NULL) {
                continue;  // the template; 14.11 handles it at construction
            }
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, entry->v.entry.key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                break;
            }
            // 14.17改: a def^ is not the writer's namespace, so the two
            // spellings of tostring and iterate are one member -- held under
            // the bare one, which is what the checker settled the name to.
            length = lhat_member_held_as(name, length);
            // 14.15: a declaration carries a type and no value; what it
            // leaves is the seat, so the definition shows the member before
            // a later part gives it. RESERVE lays one only where nothing
            // sits, so the parts may come in either order.
            if (entry->v.entry.declared) {
                uint8_t seat_mark = c->next_register;
                uint8_t seat_key = reserve(c);
                load_string_bytes(c, seat_key, name, length);
                emit(c, lhat_encode_abc(LHAT_BC_RESERVE, into, seat_key, 0));
                c->next_register = seat_mark;
                continue;
            }
            // 14.5改: nothing goes under a name the checker will not read.
            if (entry->v.entry.modifier == LHAT_DEF_PLAIN &&
                ambiguous_member(c, &chain, name, length)) {
                continue;
            }
            // 14.11: a member spelled new and written as a body is the
            // constructor. Compiled as one (compile_subroutine_as), so that
            // construction stays the machine's and the body only adjusts
            // the copy.
            bool constructor = name_is(name, length, "new") &&
                               entry->v.entry.value != NULL &&
                               entry->v.entry.value->kind == LHAT_NODE_FUNC;

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
            // capture of 5.4 the way it reaches def^. Bound before the
            // value is compiled, since that is when the capture is made.
            //
            // For new the name means the hook chain instead: what was under
            // the key is a constructor, and a constructor run from inside
            // one would make a second instance.
            if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE && constructor) {
                bind_new_hooks(c, &chain, i, entry);
            } else if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                uint8_t hidden = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, hidden, into, key));
                Local *replaced = declare_local(c, "super^", 6, hidden, 1);
                if (replaced == NULL) break;
                replaced->declaration = entry;
            }

            if (entry->v.entry.value->kind == LHAT_NODE_FUNC) {
                c->pending_name = name;
                c->pending_name_length = length;
            }
            if (constructor) {
                compile_subroutine_as(c, entry->v.entry.value, value,
                                      LHAT_BODY_CONSTRUCTOR);
            } else {
                compile_expression(c, entry->v.entry.value, value);
            }
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
                release_locals(c, entry_mark);
            }
            c->next_register = at;
        }
        c->lexer = enclosing_lexer;
        c->foreign_scope = enclosing_scope;
        c->foreign_module = enclosing_module;
    }

    // 14.11: the prototype, built last so an initialiser reads def^ with
    // every member in place. Base first, in the order the fields were
    // written; a later part's initialiser for a field the base also defaults
    // simply overwrites (14.5). A declared field (14.15) has no initialiser
    // and so no key here. SETPROTO then hangs the table under self^, sealed,
    // refusing any mutable value it holds.
    uint8_t proto_mark = c->next_register;
    uint8_t prototype = reserve(c);
    emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, prototype, 0, 0));
    for (size_t i = 0; i < chain.count; i++) {
        const LhatNode *fields = template_of(chain.parts[i]);
        if (fields == NULL) {
            continue;
        }
        // 03 の 4.3: the part may have been read from an earlier input, and
        // its offsets mean nothing against this one's text.
        const LhatLexer *enclosing_lexer = c->lexer;
        const LhatNode *enclosing_scope = c->foreign_scope;
        const char *enclosing_module = c->foreign_module;
        c->lexer = chain.lexers[i];
        c->foreign_scope = chain.scopes[i];
        c->foreign_module = chain.modules[i];
        for (const LhatNode *field = fields->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (field->v.entry.key == NULL ||
                !node_name(c, field->v.entry.key, &name, &length)) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                break;
            }
            uint8_t at = c->next_register;
            uint8_t key = reserve(c);
            // 14.15: a declaration carries no value; what it leaves is the
            // seat -- the key held with nothing under it, so a prototype
            // (and every clone) shows the field before anything gives it.
            // A part that already wrote the value keeps it: RESERVE lays a
            // seat only where nothing sits.
            if (field->v.entry.declared) {
                load_string_bytes(c, key, name, length);
                emit(c, lhat_encode_abc(LHAT_BC_RESERVE, prototype, key, 0));
                c->next_register = at;
                continue;
            }
            uint8_t value = reserve(c);
            load_string_bytes(c, key, name, length);
            compile_expression(c, field->v.entry.value, value);
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, prototype, key, value));
            c->next_register = at;
        }
        c->lexer = enclosing_lexer;
        c->foreign_scope = enclosing_scope;
        c->foreign_module = enclosing_module;
    }
    // 14.7改2: what this definition delegates to, put on it before the
    // prototype is sealed. The spelling travels with the name -- 'self^.x'
    // says to read it off the receiver, a bare 'x' off the definition -- so
    // nothing at run time has to work out which was meant.
    //
    // Walked over the chain's parts and not over `node`, which is the whole
    // composition and may be a '..' rather than a def^ at all (14.5). The
    // last part to declare one wins, the way a member written later does.
    const LhatNode *delegate = NULL;
    size_t delegate_part = 0;
    for (size_t i = 0; i < chain.count; i++) {
        for (const LhatNode *entry = chain.parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (entry->v.entry.modifier == LHAT_DEF_DELEGATE &&
                entry->v.entry.value != NULL) {
                delegate = entry->v.entry.value;
                delegate_part = i;
            }
        }
    }
    if (delegate != NULL) {
        bool through_self = delegate->kind == LHAT_NODE_MEMBER;
        const char *name = NULL;
        size_t length = 0;
        // 03 の 4.3, as the two loops above: the part this was written in may
        // be another unit's, and the offsets in it mean nothing against this
        // one's text. Only the reading of the name moves -- the bytes it
        // answers belong to that unit's lexer and outlive this, and
        // load_string_bytes copies them.
        const LhatLexer *enclosing_lexer = c->lexer;
        c->lexer = chain.lexers[delegate_part];
        bool named =
            node_name(c, through_self ? delegate->v.access.argument : delegate,
                      &name, &length);
        c->lexer = enclosing_lexer;
        if (!named) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        uint8_t mark = c->next_register;
        uint8_t key = reserve(c);
        load_string_bytes(c, key, name, length);
        emit(c, lhat_encode_abc(LHAT_BC_SETDELEGATE, into, key,
                                through_self ? 1 : 0));
        c->next_register = mark;
    }

    emit(c, lhat_encode_abc(LHAT_BC_SETPROTO, into, prototype, 0));
    c->next_register = proto_mark;

    c->building = enclosing;
    release_locals(c, local_mark);
}

// The new of 14.11 that a definition gets when it declares none: a function
// of no arguments answering a fresh copy of the prototype -- which is all
// construction is (NEWINSTANCE does the copying), so the default has nothing
// to add. It is compiled as a body of its own so that it is an ordinary
// member, callable like any other.
static void compile_default_new(Compiler *c, const LhatNode *node,
                                uint8_t definition)
{
    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    proto->is_function = true;

    size_t index = lhat_proto_add(c->proto, proto);
    if (index == SIZE_MAX) {
        lhat_proto_free(proto);
        fail(c, c->proto->proto_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }

    Compiler inner;
    memset(&inner, 0, sizeof inner);
    inner.parent = c;
    inner.lexer = c->lexer;
    inner.proto = proto;
    inner.result = c->result;

    (void)node;
    uint8_t slot = reserve(&inner);
    uint8_t inner_mark = inner.next_register;
    uint8_t owner = reserve(&inner);
    size_t captured = c->building != NULL && c->building->count > 0
        ? capture_binding(&inner, c->building->parts[0], "def^", 4) : SIZE_MAX;
    if (captured == SIZE_MAX) {
        fail(&inner, LHAT_COMPILE_UNDEFINED);
        return;
    }
    emit(&inner, lhat_encode_abc(LHAT_BC_GETUPVAL, owner, (uint8_t)captured, 0));
    emit(&inner, lhat_encode_abc(LHAT_BC_NEWINSTANCE, slot, owner, 0));
    inner.next_register = inner_mark;
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
// names its key; a positional one takes the next integer, counting from 0.
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
                if (entry->v.entry.value->kind == LHAT_NODE_FUNC) {
                    c->pending_name = name;
                    c->pending_name_length = length;
                }
            } else {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
        } else {
            load_constant(c, key, lhat_integer(position++));
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
    // 11.7改2: the run this call ends, if it ends one. Opened before anything
    // is compiled, since the receiver's guard belongs inside it.
    ChainFrame chain;
    chain_open(c, node, into, &chain);
    uint8_t callee = reserve(c);
    size_t fuse_cache = SIZE_MAX;  // 5.1改4, see below
    uint8_t fuse_receiver = 0;
    uint16_t selected_arm = node->checked_instance != NULL
        ? node->checked_instance->arm : node->checked_arm;
    if (node->checked_instance != NULL && selected_arm == 0) {
        fail(c, LHAT_COMPILE_UNSUPPORTED);
        return;
    }

    // 14.4: 'x.m()' hands x to a method as its self^, and 'm(x)' on the same
    // member does the same by hand. So the receiver is put in place here and
    // the machine passes it or skips it depending on what the callee takes.
    const LhatNode *target = node->v.access.target;
    bool method = target != NULL && target->kind == LHAT_NODE_MEMBER;
    // 14.12改: super^(…) is the bound form too. What it replaces is a member
    // of this same definition, so the receiver is the self^ the body already
    // holds -- laid out here the way a method call lays it out, and the
    // machine skips it when the hidden member turns out to take none.
    bool super_call = !method && node->checked_super_call;
    if (method) {
        // 05 の 8.9: a host value receiver takes its width of slots, and the
        // machine reads the member off its head tag -- so the receiver run
        // is kept whole below the arguments exactly as a one-slot one is.
        size_t receiver_width = width_of(target->v.access.target);
        uint8_t receiver = reserve_wide(c, receiver_width);
        compile_expression(c, target->v.access.target, receiver);
        // 11.7改2: the member node is absorbed here rather than compiled, so
        // its own '?' has to be emitted here too -- 'a?.b(x)' reads the member
        // off a, and a nil^ a never gets that far. The jump lands where the
        // call's does, which is what makes the whole run one guard.
        if (target->v.access.nil_safe && !chain_guard(c, receiver)) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        // 03 の 5.1改: 'x.m()' is where a member read is hottest, and the
        // name is written, so the site remembers where it found it.
        size_t cache = member_cache_for(c, target);
        // 03 の 5.1改4: when every argument runs nothing, the read moves
        // down to sit against the call and fuses with it (CALLMEMBER) --
        // nothing between them could have changed which member is called.
        // An argument that runs something keeps today's order: the member
        // is read before the arguments. '?(' needs the callee before the
        // arguments too, and PICKARM has to stand between the two, so both
        // keep the spelled-out pair.
        bool arms_pure = selected_arm == 0 && !node->v.access.nil_safe;
        for (const LhatNode *fuse_arg = node->v.access.argument;
             arms_pure && fuse_arg != NULL; fuse_arg = fuse_arg->next) {
            if (fuse_arg->kind == LHAT_NODE_SPREAD ||
                !runs_nothing(c, fuse_arg)) {
                arms_pure = false;
            }
        }
        if (cache != SIZE_MAX && arms_pure) {
            fuse_cache = cache;
            fuse_receiver = receiver;
        } else if (cache != SIZE_MAX) {
            emit(c, lhat_encode_abc(LHAT_BC_GETMETHOD, callee, receiver,
                                    (uint8_t)cache));
        } else {
            uint8_t key = c->next_register;
            if (key >= LHAT_MAX_REGISTERS) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            (void)reserve(c);
            compile_key(c, target, key);
            emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, callee, receiver, key));
        }
        c->next_register = (uint8_t)(receiver + receiver_width);
    } else if (super_call) {
        compile_expression(c, target, callee);
        uint8_t receiver = reserve(c);
        if (node->checked_receiver == NULL) {
            // A static member has no self^ to hand over, and 14.4 says its
            // replacement takes none either. The plain call form is right.
            c->next_register = (uint8_t)(callee + 1);
            super_call = false;
        } else if (!emit_binding_read(c, node->checked_receiver, "self^", 5, receiver)) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
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
    if (node->v.access.nil_safe && !chain_guard(c, callee)) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    size_t count = 0;
    bool spread = false;
    bool wide_args = false;
    for (const LhatNode *arg = node->v.access.argument; arg != NULL;
         arg = arg->next) {
        if (arg->kind == LHAT_NODE_SPREAD) {
            // 13.8改: a tuple's width is the checker's, so its positions are
            // ordinary arguments -- the run lands head first and the
            // positions move down over the head, into the slots written
            // arguments would have taken. Nothing is built to hand over, and
            // the machine sees a call like any other.
            size_t positions = tuple_width_of(arg->v.jump.value);
            if (positions > 1) {
                uint8_t head = reserve_wide(c, positions + 1);
                compile_run_source(c, arg->v.jump.value, head, positions + 1);
                emit_move_wide(c, head, (uint8_t)(head + 1), positions);
                c->next_register = (uint8_t)(head + positions);
                count += positions;
                continue;
            }
            // 13.7: a table spreads as the one value it is. C tells the
            // machine the last argument is something to unpack, so only the
            // last may be one -- the checker says so, and an unchecked
            // compile that could not tell a tuple from a table stops here.
            if (arg->next != NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t slot = reserve(c);
            compile_expression(c, arg->v.jump.value, slot);
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
    if (selected_arm != 0) {
        emit(c, lhat_encode_abx(LHAT_BC_PICKARM, callee,
                                (uint16_t)(selected_arm - 1)));
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
    // 05 の 8.9: a call that answers a host value says the width it made
    // room for, so the yield hand-back can tell this consumer from the
    // delegation loop's unreserved slot (both would otherwise read 0).
    size_t prepared = reserved;
    if (answer_width > 1 && width_of(node) > 1 && answer_width > prepared) {
        prepared = answer_width;
    }
    uint8_t operand = lhat_call_operand(spread, (unsigned)prepared);
    if (tail && drop) {
        operand |= LHAT_CALL_DROP;
    }
    if (fuse_cache != SIZE_MAX) {
        emit(c, lhat_encode_abc(LHAT_BC_CALLMEMBER, callee, fuse_receiver,
                                (uint8_t)fuse_cache));
    }
    emit(c, lhat_encode_abc(call_op, callee, (uint8_t)count, operand));
    // The answer then moves to the destination the same way it was written.
    emit_move_wide(c, into, callee, answer_width);
    // 11.7改2: where every '?' of the run lands, past everything the call
    // itself does. An absent one anywhere in the run left nil^ in `into`.
    chain_close(c, &chain);
    c->next_register = mark;
}

static void compile_call(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_call_wide(c, node, into, 0);
}

// 02 の 15 章: f^ and p^ are both compiled the same way here; the difference
// they carry is for the checker, and 5.1 keeps the machine out of it.
//
// 02 の 14.11: `kind` says whether this body is a written new. A constructor
// opens on the machine's construction -- NEWINSTANCE copies the definition's
// prototype -- binds the copy as self^, and answers it whatever the body
// does. A hook (14.12改's super^) is the same body run against a receiver it
// is handed instead, the way any method is. Neither may yield.
static void compile_subroutine_as(Compiler *c, const LhatNode *node,
                                  uint8_t into, BodyKind kind)
{
    LhatProto *proto = lhat_proto_new();
    if (proto == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    // 04 の 11.6改: the debug label, when the site said one. Cleared either
    // way, so a body written inside this one never inherits it.
    if (c->pending_name != NULL) {
        proto->debug_name = (char *)lhat_alloc(c->pending_name_length + 1);
        if (proto->debug_name == NULL) {
            lhat_proto_free(proto);
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
            return;
        }
        if (proto->debug_name != NULL) {
            memcpy(proto->debug_name, c->pending_name,
                   c->pending_name_length);
            proto->debug_name[c->pending_name_length] = '\0';
        }
    }
    c->pending_name = NULL;
    c->pending_name_length = 0;
    proto->is_function = node->v.func.is_function;
    proto->yields = node->v.func.yields;
    if (kind != LHAT_BODY_ORDINARY) {
        proto->is_function = true;  // 14.11: new is an f^
        if (node->v.func.yields) {
            lhat_proto_free(proto);
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
    }

    size_t index = lhat_proto_add(c->proto, proto);
    if (index == SIZE_MAX) {
        lhat_proto_free(proto);
        fail(c, c->proto->proto_count > 0xFFFF
                    ? LHAT_COMPILE_TOO_COMPLEX : LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }

    Compiler inner;
    memset(&inner, 0, sizeof inner);
    inner.parent = c;
    inner.lexer = c->lexer;
    inner.proto = proto;
    inner.result = c->result;

    inner.body = node;

    // 14.12改: a hook takes the instance under construction as its receiver,
    // the way any method does (14.4) -- the parameter is the machine's, not
    // one the body wrote, so it is laid down ahead of the written ones.
    uint8_t self_slot = 0;
    if (kind == LHAT_BODY_NEW_HOOK) {
        proto->takes_self = true;
        struct LhatRuntimeType **receiver_types =
            (struct LhatRuntimeType **)lhat_realloc(NULL,
                                                    sizeof *receiver_types);
        if (receiver_types == NULL) {
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
            return;
        }
        receiver_types[0] = NULL;
        proto->parameter_types = receiver_types;
        proto->parameters = 1;
        self_slot = reserve(&inner);
        Local *receiver = declare_local(&inner, "self^", 5, self_slot, 1);
        if (receiver == NULL) {
            return;
        }
        receiver->declaration = node;
    }

    // 5.3: the parameters are the frame's first registers, in order.
    //
    // 13.4, 03 の 5.3: v.param.fallback is skipped on purpose. A default is
    // written into a call site by completion or the visual editor as the call
    // is built, so by the time anything runs the argument is there like any
    // other -- there is no defaulting left for the callee to do. Contrast the
    // fields of an error kind (04 の 2.2), whose defaults do get compiled, at
    // the construction rather than here.
    bool wide_param = false;  // 05 の 8.9: any parameter wider than a slot

    // 05 の 8.9: the signature the checker settled, walked beside the written
    // parameters. It is where a parameter's width comes from -- a written
    // annotation only says the width when it is spelt out in full, since
    // resolve_hostvalue_type_tag matches the registry by the words used, and
    // an alias ('let^ Vector3 = vector3.Vector3') is not those words. The
    // checker resolved the name properly, so this asks it instead. It is
    // also the only thing that can answer for a parameter with no annotation
    // at all, whose type inference settled.
    //
    // 14.4: `self^` is written among the parameters but is not in the type's
    // list (type.h), so the walk steps over it. 13.7's '...' is kept apart
    // there too, in `variadic`.
    const LhatType *signature = (const LhatType *)node->checked_type;
    const LhatTypeList *settled =
        signature != NULL && signature->kind == LHAT_TYPE_FUNC
            ? signature->v.func.params
            : NULL;

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
        // modifier says so; the shape of the signature does. A new body has
        // no written receiver (14.11) -- the machine provides one.
        //
        // 11.3改: written last it marks one too, and says the receiver
        // is the RIGHT operand -- check.c refuses that on anything but an
        // op^, so reading it here is reading a shape already judged.
        bool is_receiver = false;
        if (kind == LHAT_BODY_ORDINARY && name_is(name, length, "self^")) {
            if (param == node->v.func.params) {
                proto->takes_self = true;
                is_receiver = true;
            } else if (param->next == NULL) {
                proto->takes_self = true;
                proto->self_last = true;
                is_receiver = true;
            }
        }
        // 05 の 8.9: a host value parameter takes its registered width of
        // consecutive slots; the caller lays the argument out the same way,
        // so the windows agree without any copying.
        const LhatType *settled_type = NULL;
        if (param->v.param.variadic) {
            settled_type = signature != NULL ? signature->v.func.variadic
                                             : NULL;
        } else if (is_receiver) {
            settled_type = NULL;  // 14.4: a receiver is one slot, and is not
                                  // in the list `settled` is walking
        } else if (settled != NULL) {
            settled_type = settled->type;
            settled = settled->next;
        }
        const LhatHostValueTag *param_hostvalue = hostvalue_tag_of(settled_type);
        if (param_hostvalue == NULL) {
            // 03 の 4.2: a compile that never checked has no settled
            // signature to read, so the written spelling is what is left.
            param_hostvalue =
                hostvalue_of(param->v.param.type);
        }
        size_t param_width = param_hostvalue != NULL ? param_hostvalue->width
                                                     : 1;
        // A variadic collection still counts arguments by value index, so a
        // wide parameter is refused beside one rather than silently read
        // out of step. (A yielding body's construction copy went slot-blind
        // -- vm.c's spread-free path -- so that half of the old refusal is
        // gone.) '...' comes last, so has_variadic is checked again after
        // the loop for the parameters that preceded it.
        if (param_width > 1 && param->v.param.variadic) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        wide_param = wide_param || param_width > 1;
        uint8_t slot = reserve_wide(&inner, param_width);
        // A parameter belongs to the body.
        Local *parameter = declare_local(&inner, name, length, slot, (uint8_t)param_width);
        if (parameter == NULL) {
            return;
        }
        parameter->declaration = is_receiver ? node : param->v.param.variadic
            ? param->checked_binding
            : (param->v.param.name != NULL ? param->v.param.name->checked_binding : NULL);

        // 14.12: the search that resolves an overloaded call asks each
        // candidate what it takes, so each body carries that with it. For
        // '...' this is the element type, not the collector's own type --
        // fits_call reads it that way once has_variadic says to.
        struct LhatRuntimeType **types =
            (struct LhatRuntimeType **)lhat_realloc(proto->parameter_types,
                                               ((size_t)proto->parameters + 1) *
                                                   sizeof *types);
        if (types == NULL) {
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
            return;
        }
        proto->parameter_types = types;
        types[proto->parameters] = runtime_type(c, settled_type);
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

    // 14.11: construction first -- the machine copies the definition's
    // prototype, and the body below only adjusts the copy. The slot is what
    // every self^ in the body names, and what the member answers whatever
    // the body does.
    if (kind == LHAT_BODY_CONSTRUCTOR) {
        self_slot = reserve(&inner);
        uint8_t owner_mark = inner.next_register;
        uint8_t owner = reserve(&inner);
        size_t captured = c->building != NULL && c->building->count > 0
            ? capture_binding(&inner, c->building->parts[0], "def^", 4) : SIZE_MAX;
        if (captured == SIZE_MAX) {
            fail(&inner, LHAT_COMPILE_UNDEFINED);
            return;
        }
        emit(&inner, lhat_encode_abc(LHAT_BC_GETUPVAL, owner, (uint8_t)captured, 0));
        emit(&inner, lhat_encode_abc(LHAT_BC_NEWINSTANCE, self_slot, owner, 0));
        inner.next_register = owner_mark;
        Local *receiver = declare_local(&inner, "self^", 5, self_slot, 1);
        if (receiver == NULL) {
            return;
        }
        receiver->declaration = node;
        inner.in_constructor = true;
        inner.constructor_self = self_slot;
    }

    // The resolved signature already preserves explicit any^ and inferred
    // types alike. A constructor hook does not return the constructed value.
    proto->result_type = kind != LHAT_BODY_NEW_HOOK && signature != NULL
        ? runtime_type(c, signature->v.func.result)
        : NULL;
    proto->signature = signature != NULL ? runtime_type(c, signature) : NULL;

    // 15.2, 13.9: Y and R have no written form at all -- 03 の 5.11a's
    // checked_type is the only place either can come from, written or not.
    if (node->v.func.yields && node->checked_type != NULL) {
        const LhatType *checked = (const LhatType *)node->checked_type;
        // 15.5: taken off the coroutine the checker assembled rather than off
        // the three fields, so the defaults 13.9 fills in (a body producing
        // nothing produces nil^; one that ends without a value ends with
        // nil^) reach a value reflected at run time. Otherwise typeof^ would
        // answer one thing where the checker resolved it and another where
        // 04 の 2.4 sends it to the instruction instead.
        const LhatType *made = lhat_type_call_answer(checked);
        if (made != NULL && made->kind == LHAT_TYPE_CORO) {
            proto->yield_produce_type =
                runtime_type(c, made->v.coroutine.produce);
            proto->yield_receive_type =
                runtime_type(c, made->v.coroutine.receive);
            // 13.9: an empty R means a resume of this takes no argument;
            // 13.8改 makes a tuple R that many arguments. An endless body's
            // empty T is a different absence from one that ends without a
            // value.
            proto->yield_receives_known = true;
            size_t receive_width =
                lhat_type_tuple_width(made->v.coroutine.receive);
            proto->yield_receive_count =
                receive_width > 0
                    ? (uint8_t)receive_width
                    : (made->v.coroutine.receive != NULL ? 1 : 0);
            proto->yield_endless = made->v.coroutine.endless;
            proto->result_type = runtime_type(c, made->v.coroutine.result);
        } else {
            proto->yield_produce_type =
                runtime_type(c, checked->v.func.yield_produce);
            proto->yield_receive_type =
                runtime_type(c, checked->v.func.yield_receive);
        }
    }

    // 5.3: which statement of this body ends it, so that a bare call there is
    // read as a tail call. Only a statement of the body itself -- one inside a
    // block, a loop or a branch has the statements after that block to come
    // back to. A body carrying a finally^ (10.1) has its cleanup after the
    // call, which cleanup_depth refuses at the statement itself. A new body
    // keeps its frame (14.11): the constructor still owes the instance after
    // its last statement, so nothing in it stands in tail position.
    if (kind == LHAT_BODY_ORDINARY && node->v.func.body != NULL &&
        node->v.func.body->kind == LHAT_NODE_BLOCK) {
        for (const LhatNode *s = node->v.func.body->v.list.items; s != NULL;
             s = s->next) {
            inner.tail_statement = s;
        }
    }

    // 02 の 10.1: a p^ body is a block that may carry a finally^, which is
    // where resources are handled and so where it is most wanted.
    compile_block_in_scope(&inner, node->v.func.body);
    // 14.11: a constructor answers the copy; nothing else has a last word.
    LhatInstruction last =
        kind == LHAT_BODY_CONSTRUCTOR
            ? lhat_encode_abc(LHAT_BC_RETURN, self_slot, 0, 0)
            : lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0);
    if (lhat_chunk_emit(&proto->chunk, last, inner.line) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
    }

    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, into, (uint16_t)index));
}

static void compile_subroutine(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node->v.func.is_template && node->checked_instances == NULL) {
        load_constant(c, into, lhat_nil());
        return;
    }
    if (node->checked_instances != NULL) {
        uint8_t saved = c->next_register;
        uint8_t table = reserve(c);
        uint8_t key = reserve(c);
        uint8_t value = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_NEWTABLE, table, 0, 0));
        load_constant(c, key, lhat_integer(0));
        uint16_t emitted = 0;
        const char *name = c->pending_name;
        size_t name_length = c->pending_name_length;
        for (const LhatFunctionInstance *i = node->checked_instances;
             i != NULL; i = i->next) {
            if (i->signature == NULL || i->arm == 0) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                break;
            }
            if (i->arm <= emitted) continue;
            emitted = i->arm;
            c->pending_name = name;
            c->pending_name_length = name_length;
            compile_subroutine_as(c, i->body, value, LHAT_BODY_ORDINARY);
            emit(c, lhat_encode_abc(LHAT_BC_ADDOVERLOAD, table, key, value));
        }
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, table, key));
        c->next_register = saved;
        return;
    }
    compile_subroutine_as(c, node, into, LHAT_BODY_ORDINARY);
}

static bool binary_opcode(LhatOpKind op, LhatOpcode *out)
{
    // The label is a token operator (01 の 7.1); the value is an opcode.
    switch (op) {
        case LHAT_OP_ADD:      *out = LHAT_BC_ADD;  return true;
        case LHAT_OP_SUB:      *out = LHAT_BC_SUB;  return true;
        case LHAT_OP_MUL:      *out = LHAT_BC_MUL;  return true;
        case LHAT_OP_CROSS:    *out = LHAT_BC_CROSS; return true;
        case LHAT_OP_DOT_PRODUCT: *out = LHAT_BC_DOT_PRODUCT; return true;
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

// Read an operand in place only when its semantic identity maps to a local.
// Missing metadata must not be repaired by a spelling-based optimization.
static const Local *forwardable_local(Compiler *c, const LhatNode *node)
{
    if (node != NULL && node->checked_this_body != NULL) return NULL;
    if (node != NULL && node->checked_import_global) return NULL;
    if (node != NULL && node->checked_host_member != NULL) return NULL;
    if (node != NULL && node->checked_binding != NULL) {
        return local_for_binding(c, node->checked_binding);
    }
    return NULL;
}

// Whether evaluating this node writes no local: a name or a literal. What
// makes it safe to read the left operand in place after it.
static bool runs_nothing(Compiler *c, const LhatNode *node)
{
    if (node == NULL) {
        return false;
    }
    if (node->kind == LHAT_NODE_INT || node->kind == LHAT_NODE_FLOAT ||
        node->kind == LHAT_NODE_STRING) {
        return true;
    }
    const char *name = NULL;
    size_t length = 0;
    return node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats == 1 &&
           node_name(c, node, &name, &length) &&
           (name_is(name, length, "true^") ||
            name_is(name, length, "false^") || name_is(name, length, "nil^"));
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
    if (op == LHAT_OP_FITS) {
        compile_fits(c, node, into);
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
        // A def^ written on the right says composition and nothing else --
        // 11.2's '..' is for strings and 11.8's for what carries the member,
        // and a definition is neither. So a left this could not follow to a
        // chain is a form 14.2 does not cover, and 03 の 4.2 makes that a
        // hole to report where it is rather than instructions that fault
        // where they run.
        if (node->v.binary.right != NULL &&
            node->v.binary.right->kind == LHAT_NODE_DEF) {
            fail(c, LHAT_COMPILE_UNSUPPORTED);
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
    //
    // 03 の 5.1: an operand that is a bare name is read where it lies
    // rather than MOVEd into scratch -- the staging copies were most of a
    // loop body's instructions. The right side always may (nothing runs
    // between its evaluation and the instruction); the left only when
    // evaluating the right can write no local -- a call reaches any of
    // them through a capture, so the left forwards only past a right that
    // runs nothing (a name, a literal).
    // 03 の 5.1: the four common operations with a numeric literal on the
    // right fold the constant into the instruction -- `i + 1` was a LOADK
    // re-run every turn of a loop. The operator fallback still works: the
    // machine's ADDK family carries the constant to call_operator itself.
    const LhatNode *right_node = node->v.binary.right;
    if ((opcode == LHAT_BC_ADD || opcode == LHAT_BC_SUB ||
         opcode == LHAT_BC_MUL || opcode == LHAT_BC_DIV) &&
        right_node != NULL &&
        (right_node->kind == LHAT_NODE_INT ||
         right_node->kind == LHAT_NODE_FLOAT)) {
        LhatValue constant =
            right_node->kind == LHAT_NODE_INT
                ? lhat_integer((int64_t)right_node->v.integer.value)
                : lhat_real(right_node->v.real);
        size_t k = lhat_chunk_constant(&c->proto->chunk, constant);
        if (k != SIZE_MAX && k <= 0xFF) {
            uint8_t kmark = c->next_register;
            const Local *home = forwardable_local(c, node->v.binary.left);
            uint8_t left_at = home != NULL
                                  ? home->reg
                                  : reserve_for(c, node->v.binary.left);
            if (home == NULL) {
                compile_expression(c, node->v.binary.left, left_at);
            }
            emit(c, lhat_encode_abc(
                        (LhatOpcode)(opcode - LHAT_BC_ADD + LHAT_BC_ADDK),
                        into, left_at, (uint8_t)k));
            c->next_register = kmark;
            return;
        }
    }

    uint8_t mark = c->next_register;
    const Local *left_home = forwardable_local(c, node->v.binary.left);
    const Local *right_home = forwardable_local(c, node->v.binary.right);
    if (left_home != NULL && right_home == NULL &&
        !runs_nothing(c, node->v.binary.right)) {
        left_home = NULL;
    }
    uint8_t left = left_home != NULL ? left_home->reg
                                     : reserve_for(c, node->v.binary.left);
    uint8_t right = right_home != NULL ? right_home->reg
                                       : reserve_for(c, node->v.binary.right);
    if (left_home == NULL) {
        compile_expression(c, node->v.binary.left, left);
    }
    if (right_home == NULL) {
        compile_expression(c, node->v.binary.right, right);
    }
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
        // 13.11: an fits^ link takes a type, which is not an operand the next
        // link could compare against -- so what it tests is the value still
        // standing to its left, and that value stays where it is.
        if (op == LHAT_OP_FITS) {
            compile_fits_test(c, operand, marker->checked_fits_type, left, into);
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

// 02 の 13.14: the descriptor is a constant, built from what the checker
// resolved the spelling to -- the fold typeof^ takes when its operand's
// type is known, with no operand to run. An unchecked compile has no
// resolution and loads unknown^, which 03 の 4.2 keeps runnable; a
// spelling reaching an error type is the same (a descriptor has no arm for
// one).
static void load_type_constant(Compiler *c, const LhatType *checked,
                               uint8_t into)
{
    LhatRuntimeType *rt =
        checked != NULL && checked->kind != LHAT_TYPE_UNKNOWN &&
                !lhat_rt_mentions_error(checked)
            ? lhat_rt_from_checked(&c->proto->chunk.heap, checked)
            : lhat_type_rt_new(&c->proto->chunk.heap, LHAT_TYPE_RT_UNKNOWN);
    if (rt == NULL) {
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
        return;
    }
    load_constant(c, into, lhat_object((LhatObject *)rt));
}

static void compile_expression(Compiler *c, const LhatNode *node, uint8_t into)
{
    if (node == NULL || c->result->status != LHAT_COMPILE_OK) {
        return;
    }
    c->line = node->line;
    c->offset = node->offset;
    c->column = node->column;

    if (node->descriptor_type != NULL) {
        load_type_constant(c, (const LhatType *)node->descriptor_type, into);
        return;
    }
    if (node->checked_this_body != NULL) {
        if (node->checked_this_body == c->body) {
            emit(c, lhat_encode_abc(LHAT_BC_THIS, into, 0, 0));
        } else {
            size_t upvalue = capture_this_body(c, node->checked_this_body);
            if (upvalue == SIZE_MAX) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, into, (uint8_t)upvalue, 0));
        }
        return;
    }
    if (node->checked_host_member != NULL) {
        const LhatTypeMember *member = node->checked_host_member;
        uint8_t mark = c->next_register;
        uint8_t key = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
        load_string_bytes(c, key, member->name, member->name_length);
        emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, into, key));
        c->next_register = mark;
        return;
    }
    if (node->checked_import_global && node->checked_module_root != NULL) {
        const LhatModuleRoot *root = node->checked_module_root;
        emit_modules_read(c, NULL, root->name, root->length, into);
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
            compile_self_assign(c, node, into);
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
                    fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
                    return;
                }
                load_constant(c, into, lhat_object((LhatObject *)rt));
                return;
            }
            emit(c, lhat_encode_abc(LHAT_BC_TYPEOF, into, value, 0));
            c->next_register = mark;
            return;
        }

        // 02 の 13.14: the descriptor is a constant (load_type_constant).
        case LHAT_NODE_TYPE_VALUE:
            load_type_constant(c, (const LhatType *)node->checked_type, into);
            return;

        // 11.6改3: the operand lands in `into` and stays there where it
        // fits -- as^ narrows the type the checker tracks, not the value,
        // so there is nothing to write back once the check passes. Where it
        // does not, ASCAST writes a localerror^.CastFailure over it, which
        // is the other arm of what the checker said this answers.
        //
        // lower_type reads the written type the same way an overload^ed
        // parameter's does (14.12), and lhat_value_satisfies (the same
        // relation fits_call already trusts) is the check ASCAST makes.
        case LHAT_NODE_AS: {
            compile_expression(c, node->v.ascription.value, into);
            const LhatNode *asked = node->v.ascription.type;
            LhatRuntimeType *wanted = lower_type(c, asked);
            if (wanted == NULL) {
                // 13.7: any^ asks nothing, so there is nothing for
                // LHAT_BC_ASCAST to check.
                const char *name = NULL;
                size_t length = 0;
                if (node_name(c, asked, &name, &length) &&
                    name_is(name, length, "any^")) {
                    return;
                }
                // 11.6改3: everything else lower_type could not settle is
                // refused (5.13). A cast that silently checked nothing would
                // answer the value where the checker promised a union, and
                // the failure arm would be one nothing could ever produce.
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
            const LhatUnits *units = root_of(c)->units;
            if (units == NULL || units->resolve == NULL || path == NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            size_t which = units->resolve(
                units->context, c->lexer->strings + path->v.string.offset,
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

        case LHAT_NODE_AWAIT: {
            uint8_t mark = c->next_register;
            uint8_t co = reserve(c);
            // 13.8改: what the outer resume sends may be a run -- the inner
            // R is the outer R (15.8), and its width is not known to an
            // unchecked compile -- so the send slot is wide enough for any
            // tuple. The head lands in `sent` and the positions after it.
            uint8_t sent = reserve_wide(c, LHAT_MAX_TUPLE + 1);
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
        // (The several-names binding of one goes through compile_yield_wide
        // instead, where the send comes back as a run.)
        case LHAT_NODE_YIELD:
            // 15.11: a _yield^ never runs -- not the suspension and not its
            // value either. It compiles to nil^ wherever it stands; the
            // statement and binding forms above it compile to nothing at
            // all, which is where the checker sends every written one.
            if (node->v.jump.phantom) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                return;
            }
            // 13.8改: 'yield^ a, b' answers a tuple -- the positions go in
            // consecutive slots and YIELD carries how many, the same shape
            // return^ uses. The head is the machine's, put down in the
            // resumer's frame.
            //
            // 15.4: what goes out and what comes back are sized apart -- R
            // and Y are their own seats (13.9) -- so a pair may go out and
            // one value come back. The send lands in the first position's
            // slot, which is why the run is scratch and not `into`: `into`
            // is a place a reassignment names, and the slots behind it
            // belong to whatever locals live there.
            if (node->v.jump.level > 1) {
                size_t positions = node->v.jump.level;
                if (positions > LHAT_MAX_TUPLE) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                // 05 の 8.9: the send may be a host value, and it arrives
                // whole -- the run holds the wider of the two.
                size_t receive = width_of(node);
                size_t need = positions > receive ? positions : receive;
                uint8_t mark = c->next_register;
                uint8_t first = reserve_wide(c, need);
                uint8_t at = first;
                for (const LhatNode *item = node->v.jump.value; item != NULL;
                     item = item->next) {
                    compile_expression(c, item, at);
                    at++;
                }
                emit(c, lhat_encode_abc(LHAT_BC_YIELD, first,
                                        (uint8_t)positions, 0));
                emit_move_wide(c, into, first, receive);
                c->next_register = mark;
                return;
            }
            if (node->v.jump.value == NULL) {
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
                emit(c, lhat_encode_abc(LHAT_BC_YIELD, into, 0, 0));
                return;
            }
            {
                // 05 の 8.9: a host value goes out whole and comes back
                // whole, so the slot takes the wider of the two widths --
                // the same reading compile_yield_wide's single case makes.
                size_t out_width = width_of(node->v.jump.value);
                size_t in_width = width_of(node);
                size_t need = out_width > in_width ? out_width : in_width;
                while (c->next_register < into + need) {
                    reserve(c);
                }
                compile_expression(c, node->v.jump.value, into);
                emit(c, lhat_encode_abc(
                            LHAT_BC_YIELD, into,
                            out_width > 1 ? LHAT_YIELD_HOSTVALUE : 0, 0));
            }
            return;

        // 04 の 11.3: 't.foo' is resolved statically and 't[k]' is not, but
        // the machine performs one lookup either way. 5.1 keeps the checker's
        // knowledge out of the instruction set until specialisation.
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX: {
            // 02 の 13.14改: X.ReturnType is a type spelling the checker
            // resolved, folded the way a written one is; nothing of X runs.
            // Unchecked, it stays a member read the machine answers.
            if (node->kind == LHAT_NODE_MEMBER && node->v.access.type_spelling) {
                load_type_constant(
                    c, (const LhatType *)node->v.access.argument->checked_type,
                    into);
                return;
            }
            // 02 の 14.8改2: number^.inf and number^.nan are constants, loaded
            // as such -- the checker already refused any other name there.
            const LhatNode *on = node->v.access.target;
            const char *on_name = NULL;
            size_t on_length = 0;
            if (node->kind == LHAT_NODE_MEMBER && on != NULL &&
                on->kind == LHAT_NODE_HAT_IDENT &&
                node_name(c, on, &on_name, &on_length) &&
                name_is(on_name, on_length, "number^")) {
                const char *constant = NULL;
                size_t constant_length = 0;
                const double *held =
                    node_name(c, node->v.access.argument, &constant,
                              &constant_length)
                        ? lhat_number_constant(constant, constant_length)
                        : NULL;
                if (held == NULL) {
                    fail(c, LHAT_COMPILE_UNDEFINED);
                    return;
                }
                load_constant(c, into, lhat_real(*held));
                return;
            }
            uint8_t mark = c->next_register;
            // 11.7改2: the run this access is the end of, if it is one. Opened
            // before the target is compiled, since the guards are inside it.
            ChainFrame chain;
            chain_open(c, node, into, &chain);

            // 05 の 8.9: a host value target takes its width of slots, or
            // the key would land inside its bytes.
            uint8_t target = reserve_for(c, node->v.access.target);
            compile_expression(c, node->v.access.target, target);

            // 04 の 11.4 with 01 の 7.1: '?.' and '?[' answer nil^
            // for a nil^ target instead of reaching into one. The key is
            // compiled inside the branch, so an absent target does not
            // evaluate it -- what a reader expects of a form written to skip
            // the access, and what the other optional-chaining languages do.
            //
            // 11.7改2: the jump waits for the end of the run rather than the
            // end of this access, so everything after the '?' is skipped too.
            if (node->v.access.nil_safe && !chain_guard(c, target)) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }

            // 03 の 5.1改: a written member name is the same key every time
            // this instruction runs, so the site can remember where it found
            // it.
            size_t cache = member_cache_for(c, node);
            if (cache != SIZE_MAX) {
                emit(c, lhat_encode_abc(LHAT_BC_GETMEMBER, into, target,
                                        (uint8_t)cache));
            } else {
                // 05 の 8.9: a host value key takes its width of slots, so the
                // bytes it asks by sit whole beside the head.
                uint8_t key = reserve_for(c, node->v.access.argument);
                compile_key(c, node, key);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, into, target, key));
            }
            chain_close(c, &chain);
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
            // 01 の 2.3: every stacked reference uses its checked identity.
            if (node->kind == LHAT_NODE_HAT_IDENT && node->v.name.hats > 1) {
                if (node->checked_binding != NULL) {
                    if (!emit_binding_read(c, node->checked_binding, name, length, into)) {
                        fail_named(c, LHAT_COMPILE_UNDEFINED, name, length);
                    }
                    return;
                }
                fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
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
                // A valid this^ was emitted from checked_this_body above.
                if (name_is(name, length, "this^")) {
                    fail(c, LHAT_COMPILE_UNDEFINED);
                    return;
                }
                // 05 の 8.6: the machine's own table, reachable from anywhere
                // without being imported.
                if (name_is(name, length, "L^")) {
                    emit(c, lhat_encode_abc(LHAT_BC_ENV, into, 0, 0));
                    return;
                }
            }
            // Focus, catch and variadic references, like lexical declarations,
            // must carry the identity chosen by analysis. Unresolved references
            // are not rebound by searching compiler scopes.
            if ((name_is(name, length, "it^") || name_is(name, length, "...") ||
                 name_is(name, length, "def^") || name_is(name, length, "self^") ||
                 name_is(name, length, "super^")) &&
                node->checked_binding == NULL) {
                fail_named(c, LHAT_COMPILE_UNDEFINED, name, length);
                return;
            }
            if (!emit_binding_read(c, node->checked_binding, name, length, into)) {
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
            if (node->checked_binding != NULL) {
                if (!emit_binding_read(c, node->checked_binding, name, length, into)) {
                    fail_named(c, LHAT_COMPILE_UNDEFINED, name, length);
                }
                return;
            }
            uint8_t reg = 0;
            size_t upvalue = 0;
            switch (binding_location(c, node, name, length, &reg, &upvalue)) {
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
            // 11.7改2: 'x?' is '!(x fits^ nil^)' written short, and the
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

        // 05 の 8.9: 'box^ expr' -- the host value laid out whole, then one
        // instruction to box it. The width is the type's, which only a
        // checked compile knows -- the line every wide form draws. 8.9改:
        // C says what BOX makes and takes -- bit 0 seals (constbox^), bit 1
        // says R[B] is a box to copy rather than a value laid out, which is
        // how constbox^ off a box compiles (the machine's kind check has
        // the last word on an unchecked run).
        case LHAT_NODE_BOX: {
            const LhatNode *held = node->v.jump.value;
            const LhatHostValueTag *tag = hostvalue_of(held);
            uint8_t seal = node->v.jump.sealing ? 1 : 0;
            uint8_t mark = c->next_register;
            if (tag == NULL) {
                if (!node->v.jump.sealing) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    return;
                }
                uint8_t slot = reserve(c);
                compile_expression(c, held, slot);
                emit(c, lhat_encode_abc(LHAT_BC_BOX, into, slot, seal | 2));
                c->next_register = mark;
                return;
            }
            uint8_t slot = reserve_wide(c, tag->width);
            compile_expression(c, held, slot);
            emit(c, lhat_encode_abc(LHAT_BC_BOX, into, slot, seal));
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
            bool defaulted = false;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    compile_expression(c, clause->v.clause.body, into);
                    defaulted = true;
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

            // 02 の 17.5: an expression match may leave other^ out when the
            // checker can show its arms exhaust the subject. What runs must
            // not depend on whether checking ran (03 の 4.2), so the tail
            // every arm missed is a panic rather than a silent nothing --
            // the checker's proof is what makes it unreachable.
            if (!defaulted) {
                uint8_t mark = c->next_register;
                uint8_t slot = reserve(c);
                static const char missed[] = "no arm fit the value";
                load_string_bytes(c, slot, missed, sizeof missed - 1);
                emit(c, lhat_encode_abc(LHAT_BC_PANIC, slot, 0, 0));
                c->next_register = mark;
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
    if (node->checked_import_global && node->checked_module_root != NULL) {
        const LhatModuleRoot *root = node->checked_module_root;
        emit_modules_read(c, NULL, root->name, root->length, into);
        return;
    }
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
        const Local *local = local_for_binding(c, node->checked_binding);
        if (local != NULL) {
            ensure_table(c, local->reg);
            emit(c, lhat_encode_abc(LHAT_BC_MOVE, into, local->reg, 0));
            return;
        }
        size_t upvalue = capture_binding(c, node->checked_binding, name, length);
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
    // 04 の 11 章: the whole preamble -- every name emptied to nil^ before
    // any statement runs -- belongs to the body's first line, not to line 0
    // and not to each name's own line. A debugger steps down these at the
    // first line and then moves on (09 の 2.1); giving each its written line
    // would walk the lines twice, once emptying and once assigning.
    if (statements != NULL) {
        c->line = statements->line;
    }
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        // 05 の 5.5: the short form makes one name too -- the root of the
        // path the unit declared. The rest of the path is members of it.
        if (s->kind == LHAT_NODE_REQUIRE_STMT ||
            s->kind == LHAT_NODE_IMPORT_STMT) {
            const LhatModuleRoot *root = s->checked_module_root;
            if (root == NULL || root->declaration != s ||
                local_for_binding(c, root->declaration) != NULL) continue;
            uint8_t slot = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
            Local *local = declare_local(c, root->name, root->length, slot, 1);
            if (local == NULL) return;
            local->declaration = root->declaration;
            continue;
        }
        if (s->kind == LHAT_NODE_ENUMDEF) {
            const LhatNode *name_node = s->v.named.name;
            const char *name = NULL;
            size_t length = 0;
            if (!node_name(c, name_node, &name, &length)) continue;
            if (local_for_binding(c, name_node->checked_binding) != NULL) continue;
            uint8_t slot = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
            Local *local = declare_local(c, name, length, slot, 1);
            if (local == NULL) return;
            local->declaration = name_node->checked_binding;
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
                    hostvalue_of(target->v.param.type);
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
                    root->checked_binding != root ||
                    local_for_binding(c, root->checked_binding) != NULL) {
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
            if (local_for_binding(c, define_target_name(target)->checked_binding) != NULL) {
                continue;
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

            Local *declared = declare_local(c, name, length, slot, (uint8_t)width);
            if (declared == NULL) {
                return;
            }
            declared->declaration = define_target_is_path(target)
                ? define_target_root(target)->checked_binding
                : define_target_name(target)->checked_binding;
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
// 15.4 with 13.8改: a yield^ several names take apart. What goes out is the
// one-slot form's value or its own run of positions, laid from `into`; what
// the resume sends comes back as a run -- head in `into`, positions in the
// slots the binding reserved after it (`reserved` sized them, and the
// CHECKRUN after this call is what refuses a mismatched send).
static void compile_yield_wide(Compiler *c, const LhatNode *node, uint8_t into,
                               size_t reserved)
{
    if (node->v.jump.level > 1) {
        size_t positions = node->v.jump.level;
        if (positions > LHAT_MAX_TUPLE) {
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        // The out run is contiguous with the binding's reservation --
        // reserve_wide grows the same run when the out side is the wider.
        if (positions > reserved) {
            reserve_wide(c, positions - reserved);
        }
        uint8_t at = into;
        for (const LhatNode *item = node->v.jump.value; item != NULL;
             item = item->next) {
            compile_expression(c, item, at);
            at++;
        }
        emit(c, lhat_encode_abc(LHAT_BC_YIELD, into, (uint8_t)positions, 0));
        return;
    }
    if (node->v.jump.value == NULL) {
        emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, into, 0, 0));
        emit(c, lhat_encode_abc(LHAT_BC_YIELD, into, 0, 0));
        return;
    }
    // 05 の 8.9: a host value goes out whole and comes back whole, so the
    // slot takes the wider of the two widths. `into` sits at the top of the
    // scratch wherever a yield^ compiles (reserve_for read the receive
    // width), so growing the reservation stays contiguous.
    size_t out_width = width_of(node->v.jump.value);
    size_t in_width = width_of(node);
    size_t need = out_width > in_width ? out_width : in_width;
    while (c->next_register < into + need) {
        reserve(c);
    }
    compile_expression(c, node->v.jump.value, into);
    emit(c, lhat_encode_abc(LHAT_BC_YIELD, into,
                            out_width > 1 ? LHAT_YIELD_HOSTVALUE : 0, 0));
}

static void compile_tuple_define(Compiler *c, const LhatNode *node,
                                 size_t positions)
{
    if (positions > LHAT_MAX_TUPLE) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    uint8_t mark = c->next_register;
    uint8_t head = reserve_wide(c, positions + 1);
    // 8.7改: the whole right side reads the old world.
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
        const Local *local = local_for_binding(c, define_target_name(target)->checked_binding);
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
    // 15.11: a _yield^ never runs -- the whole statement is the type's and
    // compiles to nothing, its value included. The checker held the left
    // side to _^, so no slot is waiting for anything here.
    if (value != NULL && value->next == NULL &&
        value->kind == LHAT_NODE_YIELD && value->v.jump.phantom) {
        return;
    }
    // 13.8改: several values on the right and several names on the left, with
    // no word between them -- what the type says is what tells this from
    // 8.6's multiple definition, and the parser already told them apart by
    // how many values were written. 15.4: a yield^ is the other run source
    // here -- what a resume sends comes back as the run these names take
    // apart.
    if (value != NULL && value->next == NULL &&
        (is_run_source(value) || value->kind == LHAT_NODE_YIELD) &&
        tuple_width_of(value) > 1 &&
        node->v.binding.targets != NULL &&
        node->v.binding.targets->next != NULL) {
        compile_tuple_define(c, node, tuple_width_of(value));
        return;
    }
    // 8.7改: the whole right side reads the old world -- every name this
    // statement binds is passed over while any of its values compiles.
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
        const Local *local = local_for_binding(c, define_target_name(target)->checked_binding);
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
        bool from_session = (size_t)(local - c->locals) < c->session_locals;
        if (from_session) {
            emit(c, lhat_encode_abc(LHAT_BC_CLOSEONE, local->reg, 0, 0));
        }
        if (value != NULL) {
            // Analysis has already selected the outer binding for reads in
            // an initializer, or the retained binding for a REPL redefinition.
            if (value->kind == LHAT_NODE_FUNC) {
                c->pending_name = name;
                c->pending_name_length = length;
            }
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
    // 8.6.4: what the place held when the read pass looked, kept so the
    // write pass can leave an absent one alone. The saved register rather
    // than the place: 8.6改3 reads every target before writing any, so a
    // write earlier in the second pass must not change what this one sees.
    uint8_t current;
    bool guarded;
} PendingWrite;

// 8.6.4: the place, read into a slot of its own for the test alone. An
// indexed target reads back through the owner and key already evaluated --
// compiling the target again would run both a second time, which is the very
// thing 'a is read once' keeps from happening.
static void read_place(Compiler *c, PendingWrite *w, const LhatNode *target)
{
    w->current = reserve(c);
    if (w->indexed) {
        emit(c,
             lhat_encode_abc(LHAT_BC_GETINDEX, w->current, w->owner, w->key));
        return;
    }
    compile_expression(c, target, w->current);
}

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
    // 15.4: a yield^ is a run source here exactly as at a define -- what the
    // resume sends comes back as the run these targets take apart.
    if (value != NULL && value->next == NULL &&
        (is_run_source(value) || value->kind == LHAT_NODE_YIELD) &&
        tuple_width_of(value) > 1 &&
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
        w->current = 0;
        // 8.6.4: what the '?' says, whichever of the nine spellings carried
        // it. The place is read for the test even where no operator wanted
        // it read, 13.10's destructuring included -- '?:=' asks the same
        // question of a target taking its value out of a tuple.
        w->guarded = node->v.binding.compound_nil_safe;

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
                w->current = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, w->current, w->owner,
                                        w->key));
                uint8_t rhs = reserve(c);
                w->value = reserve(c);
                // 8.6.4: reserved before the branch so both arms leave the
                // same slot behind. Nothing is written to an absent place,
                // but the slot is one the collector reads, so it holds nil^
                // rather than whatever stood there.
                size_t past =
                    skip_when_absent(c, w->guarded, w->current, (int)w->value);
                compile_expression(c, value->v.binary.right, rhs);
                emit(c, lhat_encode_abc(opcode, w->value, w->current, rhs));
                land_here(c, past);
                value = value->next;
                continue;
            }
        }

        // 13.8改: the position sits in the run already, so the write pass
        // reads it straight out -- no move of its own. There is nothing to
        // skip evaluating here (the one call answered every position at
        // once), so a '?:=' over a destructuring only reads its place and
        // lets the write pass ask.
        if (tuple_call != NULL) {
            if (position > tuple_positions) {
                fail(c, LHAT_COMPILE_TOO_COMPLEX);
                return;
            }
            if (w->guarded) {
                read_place(c, w, target);
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
        // 8.6.4: the place is read into a slot of its own so the write pass
        // can ask the question again without reaching it a second time. An
        // indexed target arrives here only from '?:=' -- the compound
        // spellings were answered above, where the operator needed the same
        // read.
        size_t past = SIZE_MAX;
        if (w->guarded) {
            read_place(c, w, target);
            past = skip_when_absent(c, true, w->current, (int)w->value);
        }
        compile_expression(c, value, w->value);
        land_here(c, past);
        value = value->next;
    }

    for (size_t i = 0; i < count; i++) {
        const PendingWrite *w = &pending[i];
        // 8.6.4: an absent place is left as it is, and only this pair is --
        // the other targets of the same statement are written whatever this
        // one answered.
        size_t past = skip_when_absent(c, w->guarded, w->current, -1);
        if (w->indexed) {
            emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, w->owner, w->key,
                                    w->value));
            land_here(c, past);
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
                binding_location(c, w->target, name, length, &reg, &at);
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
            land_here(c, past);
            continue;
        }

        const Local *local = local_for_binding(c, w->target->checked_binding);
        if (local != NULL) {
            // 05 の 8.9: as wide as the name is.
            emit_move_wide(c, local->reg, w->value,
                           local->width > 1 ? local->width : 1);
            land_here(c, past);
            continue;
        }

        size_t upvalue = capture_binding(c, w->target->checked_binding, name, length);
        if (upvalue == SIZE_MAX) {
            fail(c, LHAT_COMPILE_UNDEFINED);
            return;
        }
        emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, w->value, (uint8_t)upvalue,
                                0));
        land_here(c, past);
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
            const Local *found = local_for_binding(c, target->v.access.target->checked_binding);
            if (found != NULL && found->width > 1) hv_owner = found;
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
                size_t past = skip_when_absent(
                    c, node->v.binding.compound_nil_safe, current, -1);
                uint8_t rhs = reserve(c);
                compile_expression(c, value->v.binary.right, rhs);
                uint8_t result = reserve(c);
                emit(c, lhat_encode_abc(opcode, result, current, rhs));
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, result));
                land_here(c, past);
            } else {
                // 05 の 8.9: a table never holds a host value; the checker
                // refused this first and this is the backstop.
                if (width_of(value) > 1) {
                    fail(c, LHAT_COMPILE_UNSUPPORTED);
                    c->next_register = mark;
                    return;
                }
                // 8.6.4: '?:=' reads the place through the owner and key
                // just evaluated, for the test alone -- nothing here wants
                // the value itself, which is what makes it not a compound.
                size_t past = SIZE_MAX;
                if (node->v.binding.compound_nil_safe) {
                    uint8_t current = reserve(c);
                    emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, current, into,
                                            key));
                    past = skip_when_absent(c, true, current, -1);
                }
                uint8_t slot = reserve(c);
                compile_expression(c, value, slot);
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, into, key, slot));
                land_here(c, past);
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
                binding_location(c, target, name, length, &reg, &at);
            if (found == SCOPED_TOO_FAR) {
                fail(c, LHAT_COMPILE_SCOPE_TOO_FAR);
                return;
            }
            if (found == SCOPED_NONE) {
                fail(c, LHAT_COMPILE_UNDEFINED);
                return;
            }
            if (value != NULL) {
                size_t past = skip_name_when_absent(
                    c, node->v.binding.compound_nil_safe, target);
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
                land_here(c, past);
                value = value->next;
            }
            continue;
        }

        const Local *local = local_for_binding(c, target->checked_binding);
        if (local != NULL) {
            if (value != NULL) {
                size_t past = skip_name_when_absent(
                    c, node->v.binding.compound_nil_safe, target);
                compile_expression(c, value, local->reg);
                land_here(c, past);
                value = value->next;
            }
            continue;
        }

        // 8.6 is the reason 5.4 shares a place rather than a value: a ':='
        // written inside a nested subroutine has to reach the outer binding.
        size_t upvalue = capture_binding(c, target->checked_binding, name, length);
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
            size_t past = skip_name_when_absent(
                c, node->v.binding.compound_nil_safe, target);
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, value, slot);
            emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, slot, (uint8_t)upvalue, 0));
            c->next_register = mark;
            land_here(c, past);
            value = value->next;
        }
    }
}

// A list's statements with their names left declared, for what still reads
// them afterwards -- 02 の 10.1's finally^. end_statements ends them.
static void open_statements(Compiler *c, const LhatNode *statements)
{
    declare_errors(c, statements);
    declare_names(c, statements);
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        compile_statement(c, s);
    }
}

// A block's names end with it, and so do their slots -- everything declared
// since the marks were taken.
static void end_statements(Compiler *c, size_t local_mark,
                           uint8_t register_mark)
{
    // The slots go back to the pool, so anything sharing one has to stop
    // sharing it first -- otherwise a closure made inside the block would read
    // whatever the next block puts there.
    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }

    release_locals(c, local_mark);
    c->next_register = register_mark;
}

static void compile_statements(Compiler *c, const LhatNode *statements)
{
    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;
    open_statements(c, statements);
    end_statements(c, local_mark, register_mark);
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
        fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
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
    return local_for_binding(c, define_target_name(target)->checked_binding);
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

// 04 の 4.5: opens the place a try^ leaves for. `caught` is reserved here,
// before anything the guarded statements declare.
static void catch_begin(Compiler *c, TryContext *context)
{
    context->enclosing = c->trying;
    context->jumps = NULL;
    context->count = 0;
    context->capacity = 0;
    context->cleanup_depth = c->cleanup_depth;
    context->caught = reserve(c);
}

// 04 の 4.5: the arms, entered only through the jumps the try^ in what they
// guard wrote, with the error already in `caught`. Reaching the end of what
// they guard with nothing raised jumps past them.
//
// Analysis already excludes the guarded statements' bindings from the arms.
// Their slots remain available to a finally^ after the arms.
static void compile_arms(Compiler *c, TryContext *context,
                         const LhatNode *arms)
{
    c->trying = context->enclosing;
    uint8_t caught = context->caught;

    size_t no_error = emit_jump(c, LHAT_BC_JUMP, 0);
    for (size_t i = 0; i < context->count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, context->jumps[i]);
    }
    lhat_free(context->jumps);
    context->jumps = NULL;

    // 13.11's judgement, arm by arm. The bare one asks nothing, and 4.5 puts
    // it last, so what follows it is only the end.
    size_t *leaving = NULL;
    size_t leaving_count = 0;
    size_t leaving_capacity = 0;
    bool bare = false;
    for (const LhatNode *arm = arms; arm != NULL; arm = arm->next) {
        size_t next = SIZE_MAX;
        if (arm->v.clause.condition != NULL) {
            uint8_t inner = c->next_register;
            uint8_t test = reserve(c);
            compile_fits_test(c, arm->v.clause.condition, NULL, caught, test);
            next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
            c->next_register = inner;
        }

        // 4.2: it^ is the error, and the register it is already in.
        size_t local_mark = c->local_count;
        Local *binding = declare_local(c, "it^", 3, caught, 1);
        if (binding == NULL) {
            lhat_free(leaving);
            return;
        }
        binding->declaration = arm;
        compile_statement(c, arm->v.clause.body);
        release_locals(c, local_mark);

        if (next == SIZE_MAX) {
            bare = true;  // it takes everything left; the end is next
            break;
        }
        if (leaving_count >= SIZE_MAX / sizeof *leaving / 2) {
            fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
            lhat_free(leaving);
            return;
        }
        LHAT_GROW(leaving, leaving_count, leaving_capacity, 16,
                  { fail(c, LHAT_COMPILE_OUT_OF_MEMORY); lhat_free(leaving); return; });
        leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
        lhat_chunk_patch_here(&c->proto->chunk, next);
    }

    // 4.5: what no arm took leaves the way it would have without the arms
    // -- to outer ones, or out of the frame.
    if (!bare) {
        emit_error_escape(c, caught);
    }

    for (size_t i = 0; i < leaving_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
    }
    lhat_free(leaving);
    lhat_chunk_patch_here(&c->proto->chunk, no_error);
}

// 04 の 4.5: statements with the arms written after them, their names left
// declared (open_statements).
static void compile_caught(Compiler *c, const LhatNode *statements,
                           const LhatNode *arms)
{
    if (arms == NULL) {
        open_statements(c, statements);
        return;
    }
    TryContext context;
    catch_begin(c, &context);
    c->trying = &context;
    open_statements(c, statements);
    compile_arms(c, &context, arms);
}

// 02 の 10.1: finally^ belongs to blocks in general, not to loops. A block
// that has one remembers it on the way in (`push`, taken at cleanup depth
// `entry`) and runs it on every way out -- which 10.2 wants and 5.5 says is
// the frame's job rather than the compiler's. This is the normal way out,
// and the body the others reach.
static void compile_cleanup(Compiler *c, const LhatNode *cleanup, size_t entry,
                            size_t push)
{
    if (cleanup == NULL) {
        return;
    }
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

// The block's statements in the scope that is already open. 01 の 8 章: a
// subroutine's body is written with a '{', but the scope it opens is the one
// its parameters are already in -- the checker puts both in the one Scope
// infer_func pushes, so counting it again here would put '$^' one step
// behind on this side.
//
// The finally^ comes after the arms and reads what the statements bound, so
// their names end only after it (10.1).
static void compile_block_in_scope(Compiler *c, const LhatNode *block)
{
    if (block == NULL) {
        return;
    }
    const LhatNode *cleanup = clause_of(block, LHAT_CLAUSE_FINALLY);
    size_t entry = c->cleanup_depth;
    size_t push = cleanup != NULL ? emit_cleanup_push(c) : SIZE_MAX;

    size_t local_mark = c->local_count;
    uint8_t register_mark = c->next_register;
    compile_caught(c, block->v.list.items, block->v.list.arms);
    compile_cleanup(c, cleanup, entry, push);
    end_statements(c, local_mark, register_mark);
}

// The same, opening the scope the block's '{' stands for. 01 の 8 章 counts
// it, and its clauses sit inside it -- the checker keeps them in the block's
// own Scope too, so both sides count this the same.
static void compile_block(Compiler *c, const LhatNode *block)
{
    compile_block_in_scope(c, block);
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

        const Local *local = local_for_binding(c,
            define_target_name(binding->v.binding.targets)->checked_binding);
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
        // would be an argument short otherwise.
        uint8_t callee = reserve(c);
        uint8_t receiver = reserve(c);
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, receiver, held[i], 0));
        emit_method_read(c, callee, "dispose", 7);
        emit(c, lhat_encode_abc(LHAT_BC_CALLMETHOD, callee, 0, 0));
        c->next_register = mark;
        emit(c, lhat_encode_abc(LHAT_BC_ENDCLEANUP, 0, 0, 0));
    }

    lhat_chunk_patch_here(&c->proto->chunk, over);

    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }
    release_locals(c, local_mark);
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
        // An annotated focus ('for^ p:T in^ ...') wraps the name in a PARAM,
        // and define_target_name is the unwrap either way -- the same node
        // the checker stamps the element type on (check_focus).
        return define_target_name(element->v.binding.values);
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
        // 05 の 8.9: a host value focus holds its width of slots, sized by
        // the checker's stamp the way any binding's is (declare_names). An
        // unchecked compile sees width 1 and the walk faults at run time
        // rather than laying out a run nobody reserved.
        size_t width = width_of(target_of(element));
        uint8_t slot = reserve_wide(c, width);
        for (size_t i = 0; i < width; i++) {
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, (uint8_t)(slot + i), 0,
                                    0));
        }
        Local *binding = declare_local(c, name, length, slot, (uint8_t)width);
        if (binding == NULL) {
            return;
        }
        binding->declaration = target_of(element)->checked_binding;
    }
}

// 13.10: one name takes the value whole, and several take it apart by
// position. `in^` is the marker that says which, so no other mark is needed
// (16.3). 13.8 makes what an iterator yields a table when it is a group.
static void bind_targets(Compiler *c, const LhatNode *focus, size_t local_mark,
                         size_t count, uint8_t from)
{
    if (count == 1) {
        // 05 の 8.9: a host value focus moves as its width of slots, the
        // same slot-for-slot copy emit_binding_read makes.
        const Local *local = &c->locals[local_mark];
        emit_move_wide(c, local->reg, from,
                       local->width > 1 ? local->width : 1);
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

// 16.3: an annotated focus is a filter. An element its type does not take is
// skipped the way next^ skips the rest of a turn; index^ has already moved,
// so it still names the real position. Where the walk's own type settles the
// answer, nothing is emitted.
static void filter_targets(Compiler *c, const LhatNode *focus,
                           size_t local_mark, LoopContext *loop)
{
    size_t i = 0;
    for (const LhatNode *element = focus; element != NULL;
         element = element->next, i++) {
        const LhatNode *param =
            element->kind == LHAT_NODE_DEFINE &&
                    element->v.binding.targets != NULL &&
                    element->v.binding.targets->kind == LHAT_NODE_FOCUS
                ? element->v.binding.values
                : element;
        if (param == NULL || param->kind != LHAT_NODE_PARAM ||
            param->v.param.type == NULL ||
            fits_settled_answer(c, param->v.param.type,
                                param->checked_fits_type) == 1) {
            continue;
        }
        uint8_t mark = c->next_register;
        uint8_t test = reserve(c);
        compile_fits_test(c, param->v.param.type, param->checked_fits_type,
                          c->locals[local_mark + i].reg, test);
        LHAT_GROW(loop->nexts, loop->next_count, loop->next_capacity, 16,
                  { fail(c, LHAT_COMPILE_OUT_OF_MEMORY); return; });
        loop->nexts[loop->next_count++] =
            emit_jump(c, LHAT_BC_JUMP_FALSE, test);
        c->next_register = mark;
    }
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
    release_locals(c, local_mark);
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
        compile_expression(c, bound, receiver);
        emit_method_read(c, walk, "iterate^", 8);
        emit(c, lhat_encode_abc(LHAT_BC_CALLMETHOD, walk, 0, 0));
        c->next_register = (uint8_t)(walk + 1);
        // 13.8改: several names take a run -- one head slot plus a position
        // each -- and one name takes one value, so the answer's room is
        // sized by the count. The count is syntax, which is what lets an
        // unchecked compile reserve the same slots a checked one does.
        // 05 の 8.9: one name holding a host value takes its width instead,
        // which the focus local was just sized by.
        taken = focus_locals > 1
                    ? reserve_wide(c, focus_locals + 1)
                    : reserve_wide(c, focus_locals == 1
                                          ? c->locals[local_mark].width
                                          : 1);
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
            emit(c, lhat_encode_abc(LHAT_BC_CHECKSTEP, numeric_step, 0, 0));
        } else {
            load_constant(c, numeric_step, lhat_integer(1));
        }
    }

    // repeat^ n counts to a limit read once, for the same reason: 'n 回' says
    // how many times, so it cannot change under the loop.
    uint8_t counter = 0;
    uint8_t limit = 0;
    uint8_t count_step = 0;
    if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        // Laid as the fused triple (see LHAT_BC_FORPREP): the counter runs
        // 1..limit inclusive, the same number of turns the old
        // 0..limit-1 shape took.
        counter = reserve(c);
        limit = reserve(c);
        count_step = reserve(c);
        compile_expression(c, bound, limit);
        load_constant(c, counter, lhat_integer(1));
        load_constant(c, count_step, lhat_integer(1));
    }

    // 03 の 5.1改3: one instruction a turn, when the shape allows -- the
    // triple laid consecutively (a fresh focus; 8.6改 reuse of an existing
    // name lands elsewhere) and no pre^ (9.10 runs pre^ on the refusing turn
    // too, which a bottom-of-loop test cannot). Anything else keeps the
    // spelled-out form, which answers identically.
    bool fused_down = kind == LHAT_FOR_DOWNTO;
    bool fused_for = (kind == LHAT_FOR_TO || kind == LHAT_FOR_DOWNTO) &&
                     pre == NULL && numeric != NULL &&
                     (uint8_t)(numeric->reg + 1) == numeric_bound &&
                     (uint8_t)(numeric_bound + 1) == numeric_step;
    uint8_t fused_reg = fused_for ? numeric->reg : 0;
    bool fused_count = !is_for && bound != NULL && pre == NULL &&
                       node->v.repeat.kind == LHAT_REPEAT_COUNT;
    (void)count_step;

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
    // Keep the index above loop-persistent locals, so closing its capture
    // cell before the next element does not detach prolog^/first^ bindings.
    uint8_t array_index = 0;
    if (node->checked_array_index) {
        array_index = reserve(c);
        Local *index = declare_local(c, "index^", 6, array_index, 1);
        if (index == NULL) return;
        index->declaration = node;
        load_constant(c, array_index, lhat_integer(-1));
    }
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

    LoopContext context = {0};
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

    if (fused_for) {
        leaving = emit_jump(c, fused_down ? LHAT_BC_FORPREPD
                                          : LHAT_BC_FORPREP,
                            fused_reg);
    } else if (kind == LHAT_FOR_TO || kind == LHAT_FOR_DOWNTO) {
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
        // 05 の 8.9: a host value focus says its width under
        // LHAT_RESUME_WIDE, so the hand-back knows the whole value fits
        // here while the walk modes still read the step as one value.
        uint8_t one_step = 2;
        if (focus_locals == 1 && c->locals[local_mark].width > 1) {
            one_step = (uint8_t)(LHAT_RESUME_WIDE |
                                 c->locals[local_mark].width);
        }
        emit(c, lhat_encode_abc(LHAT_BC_RESUME, taken, walk,
                                focus_locals > 1 ? (uint8_t)(focus_locals + 1)
                                                 : one_step));
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
        if (node->checked_array_index) {
            // The built-in sequence walk visits 0, 1, ... without skipping
            // slots. Close the old cell before updating: an escaped closure
            // keeps this iteration's index, including across next^/break^.
            emit(c, lhat_encode_abc(LHAT_BC_CLOSE, array_index, 0, 0));
            uint8_t index_mark = c->next_register;
            uint8_t one = reserve(c);
            load_constant(c, one, lhat_integer(1));
            emit(c, lhat_encode_abc(LHAT_BC_ADD, array_index, array_index, one));
            c->next_register = index_mark;
        }
        filter_targets(c, focus, local_mark, &context);
    } else if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        if (fused_count) {
            leaving = emit_jump(c, LHAT_BC_FORPREP, counter);
        } else {
            // One-based now (see the setup above), so the spelled-out test
            // is <= where it was <.
            uint8_t mark = c->next_register;
            uint8_t test = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_LE, test, counter, limit));
            leaving = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
            c->next_register = mark;
        }
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
    // 04 の 4.5: the arms close main^, so they are one turn's too, and a
    // turn an arm took goes on to the next like any other.
    size_t main_locals = c->local_count;
    uint8_t main_registers = c->next_register;
    compile_caught(c, body != NULL ? body->v.list.items : NULL,
                   body != NULL ? body->v.list.arms : NULL);
    end_statements(c, main_locals, main_registers);

    // 9.11: where a next^ lands. The rest of the body is what it skipped;
    // the step below is not -- without it a counted loop would never move,
    // and 16.4's to^ is that step written for the writer.
    for (size_t i = 0; i < context.next_count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, context.nexts[i]);
    }

    if (fused_for || fused_count) {
        // The turn's whole machinery: advance, test, jump back to the
        // instruction after FORPREP. Emitted by hand because the jump aims
        // backwards, which lhat_chunk_patch_here cannot write.
        size_t looping = c->proto->chunk.count;
        int32_t offset = (int32_t)(top + 1) - (int32_t)looping - 1;
        LhatOpcode turn = fused_for && fused_down ? LHAT_BC_FORLOOPD
                                                  : LHAT_BC_FORLOOP;
        emit(c, lhat_encode_jump(turn, fused_for ? fused_reg : counter,
                                 offset));
    } else if (advance != NULL) {
        compile_in_scope(c, advance);
    } else if (numeric != NULL) {
        compile_numeric_advance(c, node, numeric, numeric_step);
    } else if (!is_for && node->v.repeat.kind == LHAT_REPEAT_COUNT) {
        size_t k = lhat_chunk_constant(&c->proto->chunk, lhat_integer(1));
        if (k != SIZE_MAX && k <= 0xFF) {
            emit(c, lhat_encode_abc(LHAT_BC_ADDK, counter, counter,
                                    (uint8_t)k));
        } else {
            uint8_t mark = c->next_register;
            uint8_t one = reserve(c);
            load_constant(c, one, lhat_integer(1));
            emit(c, lhat_encode_abc(LHAT_BC_ADD, counter, counter, one));
            c->next_register = mark;
        }
    }

    // Backwards, so lhat_chunk_patch_here -- which only ever aims at the end
    // of what has been emitted -- cannot write it.
    if (!fused_for && !fused_count) {
        size_t back = emit_jump(c, LHAT_BC_JUMP, 0);
        if (back != SIZE_MAX) {
            int32_t offset = (int32_t)top - (int32_t)back - 1;
            c->proto->chunk.code[back] =
                lhat_encode_jump(LHAT_BC_JUMP, 0, offset);
        }
    }

    if (leaving != SIZE_MAX) {
        lhat_chunk_patch_here(&c->proto->chunk, leaving);
    }
    for (size_t i = 0; i < context.count; i++) {
        lhat_chunk_patch_here(&c->proto->chunk, context.jumps[i]);
    }
    c->loop = context.enclosing;
    lhat_free(context.jumps);
    lhat_free(context.nexts);

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
    compile_cleanup(c, cleanup, cleanup_entry, cleanup_push);

    if (c->local_count > local_mark) {
        emit(c, lhat_encode_abc(LHAT_BC_CLOSE, register_mark, 0, 0));
    }
    release_locals(c, local_mark);
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
            // 14.11: a constructor answers the copy whatever its statements
            // say -- the value (15.12's sole expression included) still runs
            // for its effects, and a bare return^ is an early finish.
            if (c->in_constructor) {
                if (node->v.jump.value != NULL) {
                    uint8_t effect_mark = c->next_register;
                    for (const LhatNode *item = node->v.jump.value;
                         item != NULL; item = item->next) {
                        uint8_t slot = reserve_for(c, item);
                        compile_expression(c, item, slot);
                    }
                    c->next_register = effect_mark;
                }
                emit(c, lhat_encode_abc(LHAT_BC_RETURN, c->constructor_self,
                                        0, 0));
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
            // Forward a checked tuple through the same wide call protocol as
            // destructuring. Even a tail call may return here (host calls and
            // frames with cleanups), so retain all positions for RETURN.
            size_t positions = tuple_width_of(node->v.jump.value);
            if (positions > 1 && is_run_source(node->v.jump.value)) {
                if (positions > LHAT_MAX_TUPLE) {
                    fail(c, LHAT_COMPILE_TOO_COMPLEX);
                    return;
                }
                uint8_t head = reserve_wide(c, positions + 1);
                c->tail_call = node->v.jump.value->kind == LHAT_NODE_CALL &&
                               c->cleanup_depth == 0;
                compile_run_source(c, node->v.jump.value, head, positions + 1);
                c->tail_call = false;
                emit(c, lhat_encode_abc(LHAT_BC_CHECKRUN, head, (uint8_t)positions, 0));
                emit(c, lhat_encode_abc(LHAT_BC_RETURN, (uint8_t)(head + 1),
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

        // 04 の 4.5: with arms, the bodies are what they guard -- the
        // conditions stand outside the braces, and a try^ in one goes on out.
        // 02 の 10.1: a finally^ is the if^'s as a whole, in a scope of its
        // own as the checker keeps it.
        case LHAT_NODE_IF_STMT: {
            const LhatNode *cleanup = clause_of(node, LHAT_CLAUSE_FINALLY);
            size_t entry = c->cleanup_depth;
            size_t push = cleanup != NULL ? emit_cleanup_push(c) : SIZE_MAX;
            uint8_t register_mark = c->next_register;

            const LhatNode *arms = node->v.list.arms;
            TryContext context;
            if (arms != NULL) {
                catch_begin(c, &context);
            }
            TryContext *outer = c->trying;
            TryContext *bodies = arms != NULL ? &context : outer;

            size_t leaving[LHAT_MAX_LOCALS];
            size_t leaving_count = 0;

            for (const LhatNode *clause = node->v.list.items; clause != NULL;
                 clause = clause->next) {
                const LhatNode *condition = clause->v.clause.condition;
                if (condition == NULL) {
                    c->trying = bodies;
                    compile_statement(c, clause->v.clause.body);
                    c->trying = outer;
                    break;
                }

                uint8_t mark = c->next_register;
                uint8_t test = reserve(c);
                compile_expression(c, condition, test);
                size_t next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                c->next_register = mark;

                c->trying = bodies;
                compile_statement(c, clause->v.clause.body);
                c->trying = outer;
                if (clause->next != NULL && leaving_count < LHAT_MAX_LOCALS) {
                    leaving[leaving_count++] = emit_jump(c, LHAT_BC_JUMP, 0);
                }
                lhat_chunk_patch_here(&c->proto->chunk, next);
            }

            for (size_t i = 0; i < leaving_count; i++) {
                lhat_chunk_patch_here(&c->proto->chunk, leaving[i]);
            }
            if (arms != NULL) {
                compile_arms(c, &context, arms);
            }
            compile_cleanup(c, cleanup, entry, push);
            c->next_register = register_mark;
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
                uint8_t held = reserve(c);
                compile_path_prefix(c, path->v.access.target, owner);
                compile_key(c, path, key);
                // Where the parent was imported, its own table already holds
                // what this brought in -- and that table is the program's
                // (8.7改5), which nothing writes. So only a stand-in, or
                // nothing, is written over, as ensure_table_at does.
                emit(c, lhat_encode_abc(LHAT_BC_GETINDEX, held, owner, key));
                emit(c, lhat_encode_abc(LHAT_BC_SAME, held, held, slot));
                emit(c, lhat_encode_abc(LHAT_BC_NOT, held, held, 0));
                size_t there = emit_jump(c, LHAT_BC_JUMP_FALSE, held);
                emit(c, lhat_encode_abc(LHAT_BC_SETINDEX, owner, key, slot));
                lhat_chunk_patch_here(&c->proto->chunk, there);
            } else {
                const Local *local = local_for_binding(c, node->checked_binding);
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
            const char *module_name = node->checked_module_root != NULL
                                          ? node->checked_module_root->path : NULL;
            if (module_name == NULL) {
                fail(c, LHAT_COMPILE_UNSUPPORTED);
                return;
            }
            uint8_t mark = c->next_register;
            uint8_t slot = reserve(c);
            compile_expression(c, node, slot);
            compile_bind_path(c, node, module_name, slot);
            c->next_register = mark;
            return;
        }

        case LHAT_NODE_CALL_STMT:
        case LHAT_NODE_AWAIT:
        case LHAT_NODE_YIELD: {
            // 15.11: a _yield^ statement is the type's and compiles to
            // nothing, its value included.
            if (node->kind == LHAT_NODE_YIELD && node->v.jump.phantom) {
                return;
            }
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
            if (target->count >= SIZE_MAX / sizeof *target->jumps / 2) {
                fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
                return;
            }
            LHAT_GROW(target->jumps, target->count, target->capacity, 16,
                      { fail(c, LHAT_COMPILE_OUT_OF_MEMORY); return; });
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
            if (target->next_count >= SIZE_MAX / sizeof *target->nexts / 2) {
                fail(c, LHAT_COMPILE_OUT_OF_MEMORY);
                return;
            }
            LHAT_GROW(target->nexts, target->next_count, target->next_capacity, 16,
                      { fail(c, LHAT_COMPILE_OUT_OF_MEMORY); return; });
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

        case LHAT_NODE_ENUMDEF:
            // 02 の 19 章: unlike an errordef^, the members carry values, so
            // the declaration runs where it stands.
            compile_enumdef(c, node);
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
        const LhatNode *declaration;
    } names[LHAT_MAX_LOCALS];
    size_t count;
    uint8_t next_register;

    // Runtime error identities survive across inputs. Definition composition
    // is retained by the semantic session instead of a second name registry.
    struct ErrorDecl *errors;
    size_t error_count;
    size_t error_capacity;

    // 04 の 12.4 and 05 の 8.8: what a host's lhat_register_error_kind and
    // lhat_register_hostdata_type registered, so that fits^ against either
    // compiles at a prompt as it does in a file. The other half of LhatUnits
    // a session carries; NULL/0 when the host registered none.
    const LhatHostErrorKind *host_errors;
    size_t host_error_count;
    const LhatHostTypeEntry *host_types;
    size_t host_type_count;
};

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

bool lhat_compile_session_seed(LhatCompileSession *session, const char *name,
                               size_t length, uint8_t reg, const LhatNode *declaration)
{
    if (session == NULL || session->count >= LHAT_MAX_LOCALS) {
        return false;
    }
    // Copied, as every session name is: the text it came from (a chunk's
    // table, a lexer) need not outlive the session.
    char *copy = (char *)lhat_alloc(length + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, name, length);
    copy[length] = '\0';
    session->names[session->count].name = copy;
    session->names[session->count].length = length;
    session->names[session->count].reg = reg;
    session->names[session->count].declaration = declaration;
    session->count++;
    if (session->next_register <= reg) {
        session->next_register = (uint8_t)(reg + 1);
    }
    return true;
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

// Puts `value` where a dotted path says, making the tables on the way. 02 の
// 8.8 is the written form of this; here the path is a string rather than a
// tree, since it came from what the required unit declared.
static void compile_bind_path(Compiler *c, const LhatNode *node,
                               const char *path, uint8_t value)
{
    size_t length = strcspn(path, ".");
    const LhatModuleRoot *binding = node->checked_module_root;
    if (binding == NULL) {
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    const Local *root = local_for_binding(c, binding->declaration);
    size_t upvalue = root == NULL
        ? capture_binding(c, binding->declaration, binding->name, binding->length) : SIZE_MAX;
    if (root == NULL && upvalue == SIZE_MAX) {
        fail(c, LHAT_COMPILE_UNDEFINED);
        return;
    }
    // One segment names the place itself, so there is nothing to reach into.
    if (path[length] == '\0') {
        if (root != NULL) emit(c, lhat_encode_abc(LHAT_BC_MOVE, root->reg, value, 0));
        else emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, value, (uint8_t)upvalue, 0));
        return;
    }

    uint8_t mark = c->next_register;
    uint8_t owner = reserve(c);
    uint8_t key = reserve(c);
    if (root != NULL) {
        ensure_table(c, root->reg);
        emit(c, lhat_encode_abc(LHAT_BC_MOVE, owner, root->reg, 0));
    } else {
        emit(c, lhat_encode_abc(LHAT_BC_GETUPVAL, owner, (uint8_t)upvalue, 0));
        ensure_table(c, owner);
        emit(c, lhat_encode_abc(LHAT_BC_SETUPVAL, owner, (uint8_t)upvalue, 0));
    }

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
        } else if ((s->kind == LHAT_NODE_ERRORDEF ||
                    s->kind == LHAT_NODE_ENUMDEF) &&
                   s->v.named.exported) {
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
            const Local *local = local_for_binding(c, define_target_name(named)->checked_binding);
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
// `path` NULL (05 の 5.6: a loaded unit) builds and answers the table
// without putting it in the registry.
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
    if (path == NULL) {
        emit(c, lhat_encode_abc(LHAT_BC_RETURN, exports, 0, 0));
        c->next_register = mark;
        return;
    }

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
        result.status = LHAT_COMPILE_OUT_OF_MEMORY;
        return result;
    }

    Compiler c;
    memset(&c, 0, sizeof c);
    c.lexer = lexer;
    c.proto = proto;
    c.result = &result;
    c.interactive_session = session != NULL;

    // Publish retained state only after every allocation has succeeded. Old
    // kind objects belong to earlier protos; only the registry array is copied.
    LhatCompileSession *original_session = session;
    LhatCompileSession staged_session;
    size_t retained_errors = session != NULL ? session->error_count : 0;
    size_t retained_names = session != NULL ? session->count : 0;
    if (session != NULL) {
        staged_session = *session;
        staged_session.errors = NULL;
        staged_session.error_capacity = retained_errors;
        if (retained_errors != 0) {
            staged_session.errors = lhat_alloc(retained_errors * sizeof *session->errors);
            if (staged_session.errors == NULL) {
                lhat_proto_free(proto);
                result.status = LHAT_COMPILE_OUT_OF_MEMORY;
                return result;
            }
            memcpy(staged_session.errors, session->errors,
                   retained_errors * sizeof *session->errors);
        }
        session = &staged_session;
    }

    // 03 の 4.3: what earlier inputs left is already in scope and already in
    // registers, so this one names it where it stands and numbers its own
    // from above.
    if (session != NULL) {
        // 03 の 4.3: retain earlier bindings by identity. A session's slots
        // are one wide (declare_names).
        for (size_t i = 0; i < session->count; i++) {
            Local *local = declare_local(&c, session->names[i].name, session->names[i].length,
                                         session->names[i].reg, 1);
            if (local != NULL) local->declaration = session->names[i].declaration;
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
        c.errors = session->errors;
        c.error_count = session->error_count;
        c.error_capacity = session->error_capacity;
        session->errors = NULL;
        session->error_count = 0;
        session->error_capacity = 0;
    }

    c.units = units;
    proto->is_unit = true;
    const char *module_name = units != NULL ? units->module_name : NULL;
    bool registers = units != NULL && units->registers;

    // 05 の 5.3: what an earlier require^ registered is the answer, and the
    // body below runs only when there is none. 5.6: a loaded module^ unit
    // keeps no registry and has no guard -- every call runs it anew.
    if (module_name != NULL && registers) {
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
        for (const LhatNode *s = unit->v.list.items; s != NULL; s = s->next) {
            compile_statement(&c, s);
        }
        compile_module_register(&c, unit->v.list.items,
                                registers ? module_name : NULL);
    } else {
        // 02 の 13.7 with 05 の 3.2: a script's '...' is its one parameter,
        // register 0 -- laid down by whatever runs it, the way a body's is
        // (lhat_run builds the collector; a require^'s CALL collects).
        Local *arguments = declare_local(&c, "...", 3, reserve(&c), 1);
        if (arguments != NULL) arguments->declaration = unit->checked_binding;
        proto->parameters = 1;
        proto->parameter_slots = 1;
        proto->has_variadic = true;
        compile_statements(&c, unit->v.list.items);
    }
    emit(&c, lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0));

    // A session hands the registries back so the next input has them; without
    // one they were the compiler's, and the kind objects they point at belong
    // to the chunk and stay either way.
    if (session != NULL && result.status == LHAT_COMPILE_OK) {
        session->errors = c.errors;
        session->error_count = c.error_count;
        session->error_capacity = c.error_capacity;
    } else {
        for (size_t i = retained_errors; i < c.error_count; i++) {
            lhat_free((void *)c.errors[i].kinds);
        }
        lhat_free(c.errors);
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
                if (c.locals[i].declaration != NULL &&
                    session->names[seen].declaration == c.locals[i].declaration) {
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
                    for (size_t j = retained_names; j < session->count; j++) {
                        lhat_free(session->names[j].name);
                    }
                    for (size_t j = retained_errors; j < session->error_count; j++) {
                        lhat_free((void *)session->errors[j].kinds);
                    }
                    lhat_free(session->errors);
                    lhat_proto_free(proto);
                    result.status = LHAT_COMPILE_OUT_OF_MEMORY;
                    return result;
                }
                memcpy(kept, c.locals[i].name, c.locals[i].length);
                kept[c.locals[i].length] = '\0';
                session->names[at].name = kept;
                session->names[at].length = c.locals[i].length;
                session->count++;
            }
            session->names[at].reg = c.locals[i].reg;
            session->names[at].declaration = c.locals[i].declaration;
        }
        session->next_register = c.next_register > session->next_register
                                     ? c.next_register
                                     : session->next_register;
        // What the next input will find already filled is exactly what this
        // one has to leave sharable when its frame goes.
        proto->kept = session->next_register;
        lhat_free(original_session->errors);
        *original_session = *session;
    }

    *out = proto;
    return result;
}

LhatCompileResult lhat_compile(const LhatCheckResult *checked, const LhatLexer *lexer,
                               LhatProto **out)
{
    return compile_unit(NULL, checked != NULL ? checked->unit : NULL, lexer, NULL, out);
}

LhatCompileResult lhat_compile_module(const LhatCheckResult *checked,
                                      const LhatLexer *lexer,
                                      const LhatUnits *units, LhatProto **out)
{
    return compile_unit(NULL, checked != NULL ? checked->unit : NULL, lexer, units, out);
}

LhatCompileResult lhat_compile_next(LhatCompileSession *session,
                                    const LhatCheckResult *checked,
                                    const LhatLexer *lexer, LhatProto **out)
{
    // The session retains the remaining host registration metadata. Initial
    // names are resolved entirely by checking. 5.3 gives require^ nowhere
    // to go, which a NULL resolver already says, and import^ needs nothing
    // here at all (compile_import_path reads L^.modules at run time).
    LhatUnits units;
    memset(&units, 0, sizeof units);
    units.host_errors = session->host_errors;
    units.host_error_count = session->host_error_count;
    units.host_types = session->host_types;
    units.host_type_count = session->host_type_count;
    return compile_unit(session, checked != NULL ? checked->unit : NULL, lexer, &units, out);
}
