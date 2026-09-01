// L^ (lhat) -- compiling a tree to bytecode.

#include "compile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat/config.h"
#include "lhat/port.h"
// 04 の 2.7: where localerror^.CastFailure's one object lives.
#include "registry.h"
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
    // 02 の 8.7改: its own let^'s value is being compiled right now. A name
    // read there -- a nested body's capture included -- resolves past this
    // local to whatever the name meant outside, mirroring the checker's
    // Binding.being_defined. Set and cleared around the one value by
    // compile_define/compile_tuple_define, never true between statements.
    bool being_defined;
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

    // 04 の 11.6改: the label a traceback prints for the next body -- the
    // binding or member a FUNC value is being written under. Set just
    // before that value compiles, consumed (and cleared) by
    // compile_subroutine_as. Debug only; 14.9 keeps names out of what a
    // proto is.
    const char *pending_name;
    size_t pending_name_length;

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

    // The def^ bound to each name, so that '..' and a definition's prototype
    // can be resolved without running anything (14.2).
    struct DefDecl *defs;
    size_t def_count;
    size_t def_capacity;

    // The definition being compiled, whose members the entries write and
    // whose prototype the template builds; def^ names it.
    const DefChain *building;

    // 02 の 14.11: this body is a written new's, and the slot is the copy it
    // is adjusting. Every return^ in it answers that slot -- construction
    // answers the copy, whatever the body wrote (15.12's sole expression
    // included).
    bool in_constructor;
    uint8_t constructor_self;

    // 14.9: the definitions lower_def_chain is inside right now, kept on the
    // root the way `defs` is. A def^ reached again while its own shape is
    // being built is answered by its name alone rather than descended into.
    // One chain per level of nesting, so the bound is the two multiplied.
    const LhatNode *lowering[LHAT_MAX_TYPE_NESTING * LHAT_MAX_DEF_CHAIN];
    size_t lowering_count;

    // 05 の 5 章: where a require^ inside this unit leads. NULL when the unit
    // is being compiled on its own, and then a require^ has nowhere to go.
    const LhatUnits *units;

    // 14.2: the unit's own top level, for a composition naming a definition
    // another unit published -- what a require^ brought in is found there.
    // Only the root holds it; a nested body asks through root_of.
    const LhatNode *statements;

    // 05 の 5.3: set while a part written in another unit is being compiled.
    // A name free in it is that unit's, and resolve_name reaches it the only
    // way a body somewhere else can -- through L^.modules.
    const LhatNode *foreign_scope;
    const char *foreign_module;
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

// 02 の 8.7改: the same search, for a name written as a value to read. A
// binding does not stand in its own initialiser -- anywhere in it, a nested
// body's capture included -- so this one runs past it and finds whatever the
// name meant outside, which is what the checker resolved the read to, and
// the two have to agree or the read lands in the wrong slot.
//
// Only reads. The other questions find_local answers -- which slot a
// definition writes into, whether a name is a module root, whether a path
// starts at a local -- are about the binding itself and want it found.
static const Local *find_local_to_read(const Compiler *c, const char *name,
                                       size_t length)
{
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->being_defined) {
            continue;
        }
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
//
// 02 の 8.7改: a being-defined local is passed over here too -- this is the
// road a nested body's capture takes (find_upvalue), and a body written in
// an initialiser must not capture the very binding being made; it captures
// what the name meant outside, as any other read there does.
static const Local *find_local_skipping(const Compiler *c, const char *name,
                                        size_t length, size_t *skip)
{
    for (size_t i = c->local_count; i > 0; i--) {
        const Local *local = &c->locals[i - 1];
        if (local->being_defined) {
            continue;
        }
        if (local->length == length && memcmp(local->name, name, length) == 0) {
            if (*skip == 0) {
                return local;
            }
            (*skip)--;
        }
    }
    return NULL;
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
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return NULL;
    }
    Local *local = &c->locals[c->local_count++];
    local->name = name;
    local->length = length;
    local->reg = reg;
    local->depth = c->scope_depth;
    local->width = width;
    local->import_root = false;
    local->being_defined = false;
    local->table = table;
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

    size_t added = lhat_proto_add_upvalue(c->proto, source, index, name, length);
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
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_THIS, 0, "this^",
                                       5);
    } else {
        size_t outer = resolve_this(c->parent, levels - 1);
        if (outer == SIZE_MAX) {
            return SIZE_MAX;
        }
        added = lhat_proto_add_upvalue(c->proto, LHAT_UPVALUE_OUTER,
                                       (uint8_t)outer, "this^", 5);
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
        index, name, length);
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
static const char *required_module_name(Compiler *c, const LhatNode *node);
static void compile_bind_path(Compiler *c, const char *path, uint8_t value);
static LhatRuntimeType *lower_type(Compiler *c, const LhatNode *node);
static bool resolve_name(Compiler *c, const char *name, size_t length,
                         uint8_t into);
static const LhatNode *template_of(const LhatNode *def);
static const LhatNode *unit_binding(const LhatNode *statements,
                                    const LhatLexer *lexer, const char *name,
                                    size_t length, bool exported_only);

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
        lhat_error_kind_new(&chunk->heap, NULL, node->v.named.local, group_name);
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
            text != NULL ? lhat_error_kind_new(&chunk->heap, group, false, text)
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
        // fits^ against the union, rather than one kind, asks for).
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

// 05 の 8.8 の fits^ 版: what lhat_register_hostdata_type (program.h) put
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
// 03 の 5.1改: a cache for the member `node` reads, or SIZE_MAX where the
// site cannot have one -- an INDEX (the key changes), a name the compiler
// cannot spell, a chunk that has run out of the 256 a byte can name. The
// caller then emits the unspecialised read, which answers the same thing.
static size_t member_cache_for(Compiler *c, const LhatNode *node)
{
    if (node->kind != LHAT_NODE_MEMBER ||
        c->proto->chunk.member_cache_count >= 256) {
        return SIZE_MAX;
    }
    const char *name = NULL;
    size_t length = 0;
    if (!node_name(c, node->v.access.argument, &name, &length)) {
        return SIZE_MAX;
    }
    size_t k = lhat_chunk_string(&c->proto->chunk, name, length);
    if (k == SIZE_MAX) {
        return SIZE_MAX;
    }
    return lhat_chunk_member_cache(&c->proto->chunk, (uint16_t)k);
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

    declare_local(c, "it^", 3, caught, 1);  // catch^ opens no brace of its own
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
// written value takes the running number: 1 to start, an integer literal
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
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    decl_rt->enum_decl = node->checked_type;  // NULL unchecked: fits^ needs
                                              // the checker anyway
    decl_rt->enum_name = lhat_string_new(owner_heap, name, length);

    uint8_t reg = reserve(c);
    if (declare_local(c, name, length, reg, 1) == NULL) {
        return;
    }
    size_t k = lhat_chunk_constant(&c->proto->chunk,
                                   lhat_object((LhatObject *)decl_rt));
    if (k == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    emit(c, lhat_encode_abx(LHAT_BC_NEWENUM, reg, (uint16_t)k));

    int64_t running = 1;
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
static void compile_fits_test(Compiler *c, const LhatNode *asked, uint8_t value,
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
    compile_fits_test(c, node->v.binary.right, value, into);
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
        lhat_type_rt_new(&root_of(c)->proto->chunk.heap, LHAT_TYPE_RT_TABLE);
    if (type == NULL) {
        return NULL;
    }

    const LhatLexer *enclosing = c->lexer;
    const LhatNode *enclosing_scope = c->foreign_scope;
    const char *enclosing_module = c->foreign_module;
    for (size_t i = 0; i < chain->count; i++) {
        // 03 の 4.3: a part read from an earlier input carries offsets into
        // that input's text, so the lexer travels with it -- the same swap
        // compile_def makes to read the same names.
        c->lexer = chain->lexers[i];
        c->foreign_scope = chain->scopes[i];
        c->foreign_module = chain->modules[i];
        for (const LhatNode *entry = chain->parts[i]->v.list.items;
             entry != NULL; entry = entry->next) {
            if (!add_shape_member(c, type, entry, false)) {
                c->lexer = enclosing;
                c->foreign_scope = enclosing_scope;
                c->foreign_module = enclosing_module;
                return NULL;
            }
        }
        const LhatNode *fields = template_of(chain->parts[i]);
        for (const LhatNode *field = fields != NULL ? fields->v.list.items : NULL;
             field != NULL; field = field->next) {
            if (!add_shape_member(c, type, field, true)) {
                c->lexer = enclosing;
                c->foreign_scope = enclosing_scope;
                c->foreign_module = enclosing_module;
                return NULL;
            }
        }
    }
    c->lexer = enclosing;
                c->foreign_scope = enclosing_scope;
                c->foreign_module = enclosing_module;

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
// 05 の 8.9 with 08: what the checker resolved this written type to, as a
// descriptor. The words a type is written with are what lower_type below
// matches against the registrations, and they run out at a name bound to a
// type or to a module ('let^ Vector3 = vector3.Vector3'). The checker
// resolved the name properly and left what it found here (check.c's
// chk_resolve_type), so this is where the words running out stops being the
// end of the question.
//
// NULL when nothing checked, which is a compile 03 の 4.2 allows and which
// then answers exactly as it did before.
static LhatRuntimeType *from_checked_type(Compiler *c, const LhatNode *node)
{
    const LhatType *checked =
        node != NULL ? (const LhatType *)node->checked_type : NULL;
    return checked != NULL
               ? lhat_rt_from_checked(&root_of(c)->proto->chunk.heap, checked)
               : NULL;
}

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
                lhat_type_rt_new(owner, LHAT_TYPE_RT_TABLE);
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
            // 05 の 8.9: the box under a host value type -- `T.Box^`. The
            // path in front names the value type; the word makes it the box.
            if (node->kind == LHAT_NODE_MEMBER) {
                const char *last = NULL;
                size_t last_length = 0;
                if (node_name(c, node->v.access.argument, &last,
                              &last_length) &&
                    name_is(last, last_length, "Box^")) {
                    const LhatHostValueTag *held =
                        resolve_hostvalue_type_tag(c, node->v.access.target);
                    if (held == NULL) {
                        // The words did not reach the type; the checker's
                        // answer might. Still NULL where nothing checked.
                        return from_checked_type(c, node);
                    }
                    LhatRuntimeType *type =
                        lhat_type_rt_new(owner, LHAT_TYPE_RT_HOSTVALUE_BOX);
                    if (type != NULL) {
                        type->hostvalue_tag = held;
                    }
                    return type;
                }
            }

            // 04 の 2.7: the kinds the language declares for itself live
            // under localerror^. Read before resolve_kind for the reason
            // the builtin spellings are read before def_chain_of: nothing
            // may bind a name over one of these, since there is no name to
            // bind -- 'localerror^' is a hat word and not a binding.
            if (node->kind == LHAT_NODE_MEMBER) {
                const char *outer = NULL;
                size_t outer_length = 0;
                const char *last = NULL;
                size_t last_length = 0;
                if (node_name(c, node->v.access.target, &outer, &outer_length) &&
                    name_is(outer, outer_length, "localerror^") &&
                    node_name(c, node->v.access.argument, &last, &last_length) &&
                    name_is(last, last_length, "CastFailure")) {
                    const LhatErrorKind *builtin = lhat_registry_cast_failure();
                    if (builtin == NULL) {
                        return NULL;
                    }
                    LhatRuntimeType *type =
                        lhat_type_rt_new(owner, LHAT_TYPE_RT_ERROR_KIND);
                    if (type != NULL) {
                        type->error_kind = builtin;
                    }
                    return type;
                }
            }

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
            // after it for the same reason compile_fits tries it last:
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
                // A qualified path none of the lookups above reached -- which
                // is what a module bound to a name comes to, since they all
                // match the registrations by the words. The checker's answer
                // is the one thing left that knows better.
                return from_checked_type(c, node);
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
            } else if (name_is(name, length, "error^") ||
                       name_is(name, length, "localerror^")) {
                // 04 の 2.7: two tops, told apart below by error_local.
                simple = LHAT_TYPE_RT_ERROR;
            } else if (name_is(name, length, "any^")) {
                return NULL;  // asks nothing
            } else {
                // 14.9: a definition's name stands for its shape, which 14.2
                // lets the compiler assemble without running anything.
                DefChain chain;
                chain.count = 0;
                if (!def_chain_of(c, node, &chain)) {
                    // Nothing the words reach: not a kind, not a registered
                    // type, not a builtin, not a definition. A name bound to
                    // one of those is exactly this case, so the checker's
                    // answer is read before giving up.
                    return from_checked_type(c, node);
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
            LhatRuntimeType *made = lhat_type_rt_new(owner, simple);
            // 04 の 2.7: which of the two tops was written.
            if (made != NULL && simple == LHAT_TYPE_RT_ERROR) {
                made->error_local = name_is(name, length, "localerror^");
            }
            return made;
        }

        case LHAT_NODE_TYPE_FUNC:
            return lhat_type_rt_new(owner, LHAT_TYPE_RT_SUBROUTINE);
        // 13.9 with 15.3改: a written 'c^{ f^R -> Y -> T }' names its three
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

// 05 の 8.7: an import^ that unit wrote. The root is what a name of it
// begins with, and reaching it is the same walk here as there.
static bool foreign_import_root(const LhatNode *statements,
                                const LhatLexer *lexer, const char *name,
                                size_t length)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_IMPORT_STMT) {
            continue;
        }
        const LhatNode *root = s->v.jump.value;
        while (root != NULL && root->kind == LHAT_NODE_MEMBER) {
            root = root->v.access.target;
        }
        const char *spelt = NULL;
        size_t spelt_length = 0;
        if (!lhat_node_name(root, lexer->source->text, lexer->strings, &spelt,
                            &spelt_length)) {
            continue;
        }
        if (spelt_length == length && memcmp(spelt, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// 05 の 5.3: a name free in a body flattened here out of another unit. What
// can be reached is what lives under L^.modules -- what that unit published,
// and the roots it imported. Its own top-level names are registers in a
// frame this body does not have, and saying so is worth more than "no such
// name" about a name that is plainly there in the other file.
static bool resolve_foreign_name(Compiler *c, const char *name, size_t length,
                                 uint8_t into)
{
    Compiler *part = c;
    while (part != NULL && part->foreign_scope == NULL) {
        part = part->parent;
    }
    if (part == NULL || part->foreign_module == NULL) {
        return false;
    }

    if (foreign_import_root(part->foreign_scope, part->lexer, name, length)) {
        emit_modules_read(c, NULL, name, length, into);
        return true;
    }
    if (unit_binding(part->foreign_scope, part->lexer, name, length, true) !=
        NULL) {
        emit_modules_read(c, part->foreign_module, name, length, into);
        return true;
    }
    if (unit_binding(part->foreign_scope, part->lexer, name, length, false) !=
        NULL) {
        fail_named(c, LHAT_COMPILE_NOT_PUBLISHED, name, length);
        return true;  // reported; nothing more to try
    }
    return false;
}

static bool resolve_name(Compiler *c, const char *name, size_t length,
                         uint8_t into)
{
    const Local *local = find_local_to_read(c, name, length);
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
    // 05 の 5.3: this body was written in another unit and flattened here
    // (14.2), so what is free in it is that unit's. Asked before the host's
    // initial names, since those are the last resort for a name written
    // here and this one was not.
    if (resolve_foreign_name(c, name, length, into)) {
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

// The value a unit's top level binds to `name`, read through the lexer that
// unit's names are spans into. `exported_only` asks 05 の 4 章's question:
// what another unit may name is what this one published.
static const LhatNode *unit_binding(const LhatNode *statements,
                                    const LhatLexer *lexer, const char *name,
                                    size_t length, bool exported_only)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        if (s->kind != LHAT_NODE_DEFINE ||
            (exported_only && !s->v.binding.exported)) {
            continue;
        }
        const LhatNode *target = s->v.binding.targets;
        const LhatNode *value = s->v.binding.values;
        if (target == NULL || target->next != NULL || value == NULL) {
            continue;
        }
        const char *spelt = NULL;
        size_t spelt_length = 0;
        if (!lhat_node_name(define_target_name(target), lexer->source->text,
                            lexer->strings, &spelt, &spelt_length)) {
            continue;
        }
        if (spelt_length == length && memcmp(spelt, name, length) == 0) {
            return value;
        }
    }
    return NULL;
}

// The same walk as def_chain_of, over another unit's tree. A name is looked
// up in that unit's own top level; a path from there into a third unit is
// not followed, since 5.1 resolves a require^ against the unit that wrote it
// and the resolver here answers for the unit being compiled.
//
// `depth` is what stops a name bound to itself: only a def^ literal grows
// the chain, so nothing else would.
static bool def_chain_foreign(const LhatNode *statements,
                              const LhatLexer *lexer, const char *module,
                              const LhatNode *node, DefChain *out, size_t depth)
{
    if (node == NULL || depth > LHAT_MAX_DEF_CHAIN) {
        return false;
    }
    if (node->kind == LHAT_NODE_DEF) {
        if (out->count >= LHAT_MAX_DEF_CHAIN) {
            return false;
        }
        out->lexers[out->count] = lexer;
        out->scopes[out->count] = statements;
        out->modules[out->count] = module;
        out->parts[out->count++] = node;
        return true;
    }
    if (node->kind == LHAT_NODE_BINARY && node->v.binary.op == LHAT_OP_CONCAT) {
        return def_chain_foreign(statements, lexer, module,
                                 node->v.binary.left, out, depth + 1) &&
               def_chain_foreign(statements, lexer, module,
                                 node->v.binary.right, out, depth + 1);
    }

    const char *name = NULL;
    size_t length = 0;
    if (!lhat_node_name(node, lexer->source->text, lexer->strings, &name,
                        &length)) {
        return false;
    }
    const LhatNode *value =
        unit_binding(statements, lexer, name, length, false);
    return def_chain_foreign(statements, lexer, module, value, out, depth + 1);
}

// 05 の 5.3: 'lib.Thing' where lib is what a require^ answered. The tree is
// what crosses, not the value -- so 14.2's chain is still fixed at the
// definition and the flattening is still a compile-time one.
static bool def_chain_across(Compiler *c, const LhatNode *node, DefChain *out)
{
    Compiler *root = root_of(c);
    if (c->units == NULL || c->units->resolve == NULL ||
        c->units->body == NULL || root->statements == NULL) {
        return false;
    }

    const char *root_name = NULL;
    size_t root_length = 0;
    if (!node_name(c, node->v.access.target, &root_name, &root_length)) {
        return false;
    }
    const LhatNode *required = unit_binding(root->statements, root->lexer,
                                            root_name, root_length, false);
    if (required == NULL || required->kind != LHAT_NODE_REQUIRE ||
        required->v.jump.value == NULL) {
        return false;
    }
    const LhatNode *path = required->v.jump.value;
    const char *module = NULL;
    size_t which = c->units->resolve(
        c->units->context, root->lexer->strings + path->v.string.offset,
        path->v.string.length, &module);
    // 05 の 5.5: a unit that declared no path publishes nowhere, so a body of
    // it flattened here would have no way back to what it named.
    if (which == LHAT_NO_UNIT || module == NULL) {
        return false;
    }

    const LhatNode *statements = NULL;
    const LhatLexer *lexer = NULL;
    if (!c->units->body(c->units->context, which, &statements, &lexer)) {
        return false;
    }

    // The member is written here, so its spelling is read here; what it is
    // compared against was written there.
    const char *member = NULL;
    size_t member_length = 0;
    if (!node_name(c, node->v.access.argument, &member, &member_length)) {
        return false;
    }
    const LhatNode *value =
        unit_binding(statements, lexer, member, member_length, true);
    return def_chain_foreign(statements, lexer, module, value, out, 0);
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
        out->scopes[out->count] = c->foreign_scope;
        out->modules[out->count] = c->foreign_module;
        out->parts[out->count++] = node;
        return true;
    }
    // 14.5: composition is '..', and the order matters -- the right side is
    // what may override.
    if (node->kind == LHAT_NODE_BINARY && node->v.binary.op == LHAT_OP_CONCAT) {
        return def_chain_of(c, node->v.binary.left, out) &&
               def_chain_of(c, node->v.binary.right, out);
    }
    if (node->kind == LHAT_NODE_MEMBER) {
        return def_chain_across(c, node, out);
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
        out->scopes[out->count] = decl->chain.scopes[i];
        out->modules[out->count] = decl->chain.modules[i];
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
    if (!resolve_name(c, "self^", 5, self)) {
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
// body without the construction), each bound over the one before it, so the
// name reads newest-first the way locals do. The chain starts on a hook that
// does nothing: the default new has nothing to run but the construction.
static void bind_new_hooks(Compiler *c, const DefChain *chain,
                           size_t stop_part, const LhatNode *stop_entry)
{
    LhatProto *idle = lhat_proto_new();
    if (idle == NULL) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    idle->is_function = true;
    idle->takes_self = true;
    idle->parameters = 1;
    idle->parameter_slots = 1;
    size_t index = lhat_proto_add(c->proto, idle);
    if (index == SIZE_MAX) {
        lhat_proto_free(idle);
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    if (lhat_chunk_emit(&idle->chunk,
                        lhat_encode_abc(LHAT_BC_RETURN_NIL, 0, 0, 0),
                        c->line) == SIZE_MAX) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    uint8_t hook = reserve(c);
    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, hook, (uint16_t)index));
    if (declare_local(c, "super^", 6, hook, 1) == NULL) {
        return;
    }

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
            hook = reserve(c);
            compile_subroutine_as(c, entry->v.entry.value, hook,
                                  LHAT_BODY_NEW_HOOK);
            if (declare_local(c, "super^", 6, hook, 1) == NULL) {
                break;
            }
        }
        c->lexer = enclosing_lexer;
        c->foreign_scope = enclosing_scope;
        c->foreign_module = enclosing_module;
    }
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
    // 01 の 8 章: the def^'s '{' opens a scope for '$^' to count, and def^
    // is a name of it -- the checker binds self^ and def^ into the Scope
    // it pushes here, so both sides count this one the same.
    c->scope_depth++;
    if (declare_local(c, "def^", 4, into, 1) == NULL) {
        c->scope_depth--;
        return;
    }

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
                declare_local(c, "super^", 6, hidden, 1);
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
    c->scope_depth--;
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

    (void)node;
    uint8_t slot = reserve(&inner);
    uint8_t inner_mark = inner.next_register;
    uint8_t owner = reserve(&inner);
    if (!resolve_name(&inner, "def^", 4, owner)) {
        fail(&inner, LHAT_COMPILE_UNDEFINED);
        return;
    }
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
                if (entry->v.entry.value->kind == LHAT_NODE_FUNC) {
                    c->pending_name = name;
                    c->pending_name_length = length;
                }
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
    // 11.7改2: the run this call ends, if it ends one. Opened before anything
    // is compiled, since the receiver's guard belongs inside it.
    ChainFrame chain;
    chain_open(c, node, into, &chain);
    uint8_t callee = reserve(c);
    size_t fuse_cache = SIZE_MAX;  // 5.1改4, see below
    uint8_t fuse_receiver = 0;

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
        bool arms_pure = node->checked_arm == 0 && !node->v.access.nil_safe;
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
            emit(c, lhat_encode_abc(LHAT_BC_GETMEMBER, callee, receiver,
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
    if (node->v.access.nil_safe && !chain_guard(c, callee)) {
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
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
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }
    // 04 の 11.6改: the debug label, when the site said one. Cleared either
    // way, so a body written inside this one never inherits it.
    if (c->pending_name != NULL) {
        proto->debug_name = (char *)lhat_alloc(c->pending_name_length + 1);
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
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
        return;
    }

    Compiler inner;
    memset(&inner, 0, sizeof inner);
    inner.parent = c;
    inner.lexer = c->lexer;
    inner.proto = proto;
    inner.result = c->result;

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
            fail(c, LHAT_COMPILE_TOO_COMPLEX);
            return;
        }
        receiver_types[0] = NULL;
        proto->parameter_types = receiver_types;
        proto->parameters = 1;
        self_slot = reserve(&inner);
        if (declare_local(&inner, "self^", 5, self_slot, 1) == NULL) {
            return;
        }
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
                resolve_hostvalue_type_tag(&inner, param->v.param.type);
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
        if (declare_local(&inner, name, length, slot, (uint8_t)param_width) ==
            NULL) {
            return;
        }

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
        if (param->v.param.type == NULL && settled_type != NULL) {
            // Nothing was written, so lower_type had nothing to read --
            // but the checker settled what the body actually asks of this
            // parameter (03 の 3.4's ParamVar), and `settled_type` above is
            // already that, stepped in time with the written list. The
            // result type does the same a few lines below and for the same
            // reasons: reaching for it only where nothing was written
            // leaves a compile that never checked exactly as it was
            // (03 の 4.2), and a written any^ still wins because this
            // fires only when the annotation is absent.
            //
            // 3.1③ is why this is not a guess in a unit that checked:
            // a parameter neither written nor settled is already an error
            // there (LHAT_CHECK_ERR_PARAM_UNDECIDED).
            types[proto->parameters] = lhat_rt_from_checked(
                &root_of(c)->proto->chunk.heap, settled_type);
        }
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
        if (!resolve_name(&inner, "def^", 4, owner)) {
            fail(&inner, LHAT_COMPILE_UNDEFINED);
            return;
        }
        emit(&inner, lhat_encode_abc(LHAT_BC_NEWINSTANCE, self_slot, owner, 0));
        inner.next_register = owner_mark;
        if (declare_local(&inner, "self^", 5, self_slot, 1) == NULL) {
            return;
        }
        inner.in_constructor = true;
        inner.constructor_self = self_slot;
    }

    // 02 の 14.16: kept the same way parameter_types is, for typeof^ to
    // reconstruct the signature without touching the checker's types (03 の
    // 4.2 -- what runs cannot depend on whether checking did).
    proto->result_type = lower_type(c, node->v.func.return_type);
    if (node->v.func.return_type == NULL && node->checked_type != NULL &&
        kind != LHAT_BODY_NEW_HOOK) {
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
        fail(c, LHAT_COMPILE_TOO_COMPLEX);
    }

    emit(c, lhat_encode_abx(LHAT_BC_CLOSURE, into, (uint16_t)index));
}

static void compile_subroutine(Compiler *c, const LhatNode *node, uint8_t into)
{
    compile_subroutine_as(c, node, into, LHAT_BODY_ORDINARY);
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

// The local a bare name would be read out of, when reading it is one MOVE
// and nothing else -- what lets a binary operand be read in place. The
// conditions mirror compile_expression's own path to resolve_name exactly:
// a plain ident (16.2's focus included), or a hat name with one hat that is
// not one of the five the case above recognises before it looks anything
// up. find_local_to_read is the same search resolve_name makes, so a hit
// here is precisely "the compile would have been emit_move_wide from this
// register" -- width and all, since the machine reads an operand's width
// off its head wherever it sits.
static const Local *forwardable_local(Compiler *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (node == NULL ||
        (node->kind != LHAT_NODE_IDENT && node->kind != LHAT_NODE_FOCUS &&
         node->kind != LHAT_NODE_HAT_IDENT) ||
        !node_name(c, node, &name, &length)) {
        return NULL;
    }
    if (node->kind == LHAT_NODE_HAT_IDENT &&
        (node->v.name.hats > 1 || name_is(name, length, "true^") ||
         name_is(name, length, "false^") || name_is(name, length, "nil^") ||
         name_is(name, length, "this^") || name_is(name, length, "L^"))) {
        return NULL;
    }
    return find_local_to_read(c, name, length);
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
            compile_fits_test(c, operand, left, into);
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
            // 02 の 14.8改2: number^.pi and the rest are constants, loaded
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
            // 01 の 2.3: a stacked reach -- it^^ the enclosing focus,
            // self^^/def^^ the enclosing def^'s, this^^ the enclosing
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
                uint8_t slot = reserve(c);
                emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
                Local *local = declare_local(c, module_name, length, slot, 1);
                if (local == NULL) {
                    return;
                }
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
            uint8_t slot = reserve(c);
            emit(c, lhat_encode_abc(LHAT_BC_LOADNIL, slot, 0, 0));
            if (declare_local(c, module_name, root, slot, 1) == NULL) {
                return;
            }
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

            if (declare_local(c, name, length, slot, (uint8_t)width) == NULL) {
                return;
            }
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
// 02 の 8.7改: marks (or unmarks) every scope name this define binds, so
// reads inside the value resolve past them to what the names meant outside
// -- the checker's Binding.being_defined, mirrored. A session's names stay
// readable (03 の 4.3) and a path target binds no scope name, so neither is
// marked.
static void mark_being_defined(Compiler *c, const LhatNode *targets, bool on)
{
    for (const LhatNode *target = targets; target != NULL;
         target = target->next) {
        if (define_target_is_path(target)) {
            continue;
        }
        const char *name = NULL;
        size_t length = 0;
        if (!node_name(c, define_target_name(target), &name, &length)) {
            continue;
        }
        Local *local = (Local *)find_local(c, name, length);
        if (local == NULL ||
            (size_t)(local - c->locals) < c->session_locals) {
            continue;
        }
        local->being_defined = on;
    }
}

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
    mark_being_defined(c, node->v.binding.targets, true);
    compile_run_source(c, node->v.binding.values, head, positions + 1);
    mark_being_defined(c, node->v.binding.targets, false);
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
    mark_being_defined(c, node->v.binding.targets, true);
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
                mark_being_defined(c, node->v.binding.targets, false);
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
            mark_being_defined(c, node->v.binding.targets, false);
            fail(c, LHAT_COMPILE_UNSUPPORTED);
            return;
        }
        const Local *local = find_local(c, name, length);
        if (local == NULL) {
            mark_being_defined(c, node->v.binding.targets, false);
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
            // 8.7改: found first, then hidden -- the destination is this
            // local, and the whole statement's names were marked
            // being_defined before this loop, so the value reads what they
            // meant outside.
            //
            // 03 の 4.3 is the exception, and the same one 8.7 already makes:
            // a name an earlier input bound, written again, is that place
            // written again rather than a new one beside it. 'var^ x = x + 1'
            // at a prompt reads what the slot holds, which is the whole of
            // what a prompt is for -- mark_being_defined leaves session
            // names alone.
            if (value->kind == LHAT_NODE_FUNC) {
                c->pending_name = name;
                c->pending_name_length = length;
            }
            compile_expression(c, value, local->reg);
            value = value->next;
        }
    }
    mark_being_defined(c, node->v.binding.targets, false);
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
            land_here(c, past);
            continue;
        }

        const Local *local = find_local(c, name, length);
        if (local != NULL) {
            // 05 の 8.9: as wide as the name is.
            emit_move_wide(c, local->reg, w->value,
                           local->width > 1 ? local->width : 1);
            land_here(c, past);
            continue;
        }

        size_t upvalue = find_upvalue(c, name, length);
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

        const Local *local = find_local(c, name, length);
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

    release_locals(c, local_mark);
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
        if (declare_local(c, name, length, slot, (uint8_t)width) == NULL) {
            return;
        }
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
        // same slot-for-slot copy resolve_name makes.
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
    c->scope_depth++;
    compile_statements(c, body != NULL ? body->v.list.items : NULL);
    c->scope_depth--;

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
                    compile_fits_test(c, arm->v.clause.condition, caught, test);
                    next = emit_jump(c, LHAT_BC_JUMP_FALSE, test);
                    c->next_register = inner;
                }

                // 4.2: it^ is the error, and the register it is already in.
                size_t local_mark = c->local_count;
                declare_local(c, "it^", 3, caught, 1);
                compile_statement(c, arm->v.clause.body);
                release_locals(c, local_mark);

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
    // lhat_register_hostdata_type registered, so that fits^ against either
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

bool lhat_compile_session_seed(LhatCompileSession *session, const char *name,
                               size_t length, uint8_t reg)
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
        // 03 の 4.3: the session's top level is this input's top level,
        // which 01 の 8 章 makes what '$' names -- scope_depth is 0 here. A
        // session's slots are one wide (declare_names).
        for (size_t i = 0; i < session->count; i++) {
            declare_local(&c, session->names[i].name, session->names[i].length,
                          session->names[i].reg, 1);
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
    c.statements = unit != NULL ? unit->v.list.items : NULL;
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
        declare_defs(&c, unit->v.list.items);
        for (const LhatNode *s = unit->v.list.items; s != NULL; s = s->next) {
            compile_statement(&c, s);
        }
        compile_module_register(&c, unit->v.list.items,
                                registers ? module_name : NULL);
    } else {
        // 02 の 13.7 with 05 の 3.2: a script's '...' is its one parameter,
        // register 0 -- laid down by whatever runs it, the way a body's is
        // (lhat_run builds the collector; a require^'s CALL collects).
        declare_local(&c, "...", 3, reserve(&c), 1);
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
        case LHAT_COMPILE_NOT_PUBLISHED:
            return "a definition composed from another unit may only use what "
                   "that unit published";
        case LHAT_COMPILE_SCOPE_TOO_FAR:
            return "this reaches out past more scopes than are open here";
    }
    return "unknown";
}
