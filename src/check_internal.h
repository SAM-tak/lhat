// L^ (lhat) -- the checker's own insides, shared by the check*.c files and
// included by nothing else. The chk_ prefix marks a symbol that crosses
// those files and must not collide with anything at link time.

#ifndef LHAT_CHECK_INTERNAL_H
#define LHAT_CHECK_INTERNAL_H

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "environment.h"
#include "lhat/config.h"
#include "grow.h"
#include "message.h"  // [lsp.next]: the titles the fixes are offered under
#include "operators.h"
#include "lhat/port.h"

// [modules.host.import]: a host writes a type out as text, so the checker reads the type
// grammar of [syntax.type] back through the parser it came from.
#include "parser.h"
#include "lhat/source.h"

// [syntax.sub.table-write]: how many levels of what a place holds the body being checked made.
// 0 is none -- it arrived from somewhere else; 1 is the table itself but not
// what it holds; LHAT_OWN_DEEP is everything it reaches. Changing what sits n
// levels down asks for more than n. Read off every value the place is given,
// never off where the walk stands, so a loop's way back round is no way round:
// `own` only goes down, and `relied` is the most any change so far needed.
// Lowering `own` beneath `relied` is the one report, made where it happens.
typedef struct ChkOwnership {
    uint8_t own;
    uint8_t relied;
} ChkOwnership;

// A name bound in a scope. `offset` is where its let^ stands, which [syntax.stmt.visible] needs
// to tell a use before the definition from one after it.
typedef struct Binding {
    const char *name;
    size_t name_length;
    LhatType *type;
    const LhatNode *declaration;
    const LhatDefinition *definition;
    const LhatModuleRoot *module_root;
    // [pipeline.infer.rounds]: what the walk before this one left under the name, which
    // is what this walk started from. NULL until a second walk is run, and
    // read only to tell a walk that learned something from one that answered
    // what the last one already had.
    LhatType *seed;
    uint32_t offset;
    bool reached;  // its let^ has been walked past
    // [syntax.sub.table-write]: what the name holds and how much of it the body made.
    ChkOwnership owned;
    // [syntax.sub.table-write]: the name was bound to something read out of what `alias` is the
    // ownership of, `alias_depth` levels down -- 'let^ r = rows[0]' or a for^
    // focus. Then `alias` is what a write leans on and what a store lowers;
    // `owned` above only caps it, and drops when the name is written again.
    ChkOwnership *alias;
    uint8_t alias_depth;
    // [pipeline.machine]: bound by an earlier input of a session. Writing the name
    // again is a redefinition rather than the clash [syntax.stmt.visible] makes of two let^ in
    // one scope -- it is the same place, written again.
    bool from_session;
    // [syntax.stmt.let]: introduced by a let^ rather than a var^, so nothing written after
    // it may reassign the name. [syntax.with]'s with^ and [syntax.for.forms]'s in^ focus bind this
    // way too -- the first because the block disposes of whatever the name
    // still holds, the second because each turn binds it afresh.
    //
    // This says nothing about the value: a table a let^ holds is still the
    // table it was, and [modules.hostdata]'s members of it are still written through [syntax.sub.table-write]
    // and [modules.host.machine]. What it refuses is the name coming to mean something
    // else.
    bool immutable;
    // [syntax.stmt.let] with [syntax.with.form] and [syntax.for.from]: the construct bound it, not a word the writer
    // chose -- a with^, or the focus of a counted or walking for^. Writing
    // var^ instead is not open to them, so the diagnostic must not offer it.
    bool bound_by_form;
    // [lsp.next]: where the let^ or var^ that introduced it stands, so a
    // diagnostic about a write to this name can offer the other word. Empty
    // for every binding no word of the source made -- a parameter, an
    // import^ root, a construct's own focus ([syntax.stmt.let] with [syntax.with.form] and [syntax.for.from]).
    LhatSpan keyword;
    // [modules.host.import]: the root an import^ bound. A name under it is read off
    // L^.modules wherever it is written, so naming one captures nothing. A
    // require^ landing on the same root clears this, the same way
    // compile.c's Local does.
    bool import_root;
    // [syntax.type.signature]: declared by a signature rather than bound by a let^ or var^.
    // Nothing in the checker reads it -- [lsp.hover] does, since where a use
    // stands says only that there is a name, and the type says only what it
    // holds. Which of the two declared it is known here and nowhere else.
    bool is_parameter;
    // [syntax.type.as-expr]: a let^ binding a written type or an existing alias
    // names that type in annotations. `named_type` is what it stands for
    // there; `type` above stays the value's own (the typeinfo table), so a
    // read of the name as a value keeps answering the descriptor.
    bool names_type;
    LhatType *named_type;
    LhatType *seed_named_type;  // The preceding inference round's alias target.
    // [pipeline.infer.call-shapes]: the f^ literal this let^ bound, when it bound one --
    // what lets a call site say which body its argument shapes belong to.
    // NULL for every other value, and for a var^, whose name may come to
    // mean some other subroutine.
    const LhatNode *value_node;
    // [syntax.stmt.old-world]: its own let^'s value is being walked right now. A name read
    // there -- anywhere there, a deferred body included -- resolves past
    // this binding to whatever the name meant outside; the binding itself
    // is unreadable from its own initialiser. Set and cleared around the
    // one walk by chk_check_define, never true between statements.
    bool being_defined;
    struct Binding *next;
} Binding;

typedef struct Scope {
    Binding *bindings;
    Binding *tail;
    struct Scope *parent;
    // [syntax.loop.scope] with [lex.scope]: a layer that holds names without being a '{'
    // anyone wrote. The loop clauses lasting the whole loop (prolog^, first^)
    // live in one of these, around the layer main^ binds into -- so a name
    // outlives the iteration without '$^' finding an extra step on the way
    // out. compile.c counts the same one scope for a loop body.
    bool transparent;
    // [lsp.hover]: the source the scope covers, taken from the construct it
    // belongs to. Both zero for a layer that is a lookup table rather than a
    // piece of a program -- a def^'s members while its entries are read, a
    // session's one scope across many inputs -- and nothing is recorded for
    // those. What is recorded is every binding, as the scope closes
    // (chk_scope_close): a tool asked what names may stand at a position
    // reads the record rather than reading [lton.host] a second time.
    uint32_t from;
    uint32_t to;
} Scope;

// [syntax.type.fits]. What a branch knows about a value that the binding does not: inside
// 'if^ r is^ T', r is only the arms of its union that conform to T. Held as
// the path it was written with rather than as a binding, since [syntax.type.fits] admits
// a dot path as well as a plain name.
typedef struct Narrowing {
    const LhatNode *path;
    LhatType *type;
    // [syntax.type.fits-range]: what a branch knows about a whole number the type does not --
    // 'for^ i from^ 1 to^ 9' and '1 <= d <= 9' both say which values reach
    // the body. Both ends are always held; INT64_MIN and INT64_MAX are how a
    // side that nothing bounded is written. A record carries this or a type,
    // and the two readings pass over each other's.
    LhatInteger lo;
    LhatInteger hi;
    bool has_bounds;
    struct Narrowing *next;
} Narrowing;

// [pipeline.infer]: a parameter written without a type, while the body that decides
// it is being read. `slot` is the pending^ object infer_func made, and the
// binding and the signature both hold that very pointer -- settling one writes
// through to both, which is what keeps this to a single walk of the body.
//
// A member reached off such a parameter gets one of these too, so that
// 'x.foo(1)' can demand t^{ foo : p^number^; } rather than only the name.
// Its slot is the member's type inside the demand its parent is collecting.
// [pipeline.infer.expected]: a top-level name this unit binds to a def^ that writes an op^.
// Held by name rather than by type, since a def^'s type is made afresh on
// every walk -- the name is looked up through the scope where the demand is
// wanted, which is also what makes a walk that reads it too early ask for
// another round.
typedef struct {
    const char *name;
    size_t name_length;
    // [modules.host.import]: a host registration is the other kind of carrier, and its
    // type is the program's rather than this walk's -- built once at
    // registration and never remade -- so it is held directly. NULL for a
    // def^ of this unit, which is looked up by the name above.
    const LhatType *type;
    uint16_t operators;  // one bit per LHAT_OPERATOR_MEMBERS entry, in order
} OperatorCarrier;

typedef struct ParamVar {
    LhatType *slot;
    // Where the parameter was written, so [pipeline.strict.per-unit] can say which one nothing
    // decided. NULL where there is no such place to point at.
    const LhatNode *node;
    // What positions that always run have asked of it, and what the ones
    // under a branch have. Kept apart because [pipeline.infer] settles them differently:
    // the first is a requirement, the second is only added when it does not
    // contradict.
    LhatType *demanded;
    LhatType *conditional;
    // Two demands from under branches ruled each other out, so the branches
    // disagree and nothing here is decided ([syntax.repl.same] leaves that unknown^ rather
    // than reporting -- the uses report for themselves).
    bool conditional_dropped;
    // Reassigned, self-referential, or two unconditional demands ruled each
    // other out. Collection stops and the answer is unknown^.
    bool closed;
    struct ParamVar *next;
} ParamVar;

// [pipeline.infer.call-shapes]: one call shape of a literal whose parameters nothing was
// written on -- the argument types one call site handed it. infer_func walks
// the body once under each shape recorded for it (the expectation path, so
// no demands are collected and nothing is reported undecided), keeps what
// the body settled to under this shape in `signature`, and marks `failed`
// where the walk refused it; the call site reads both back on the next
// round. Each record owns a separate concrete body; the binding retains the
// intersection of its signatures rather than joining parameter positions.
typedef struct Instantiation {
    const LhatNode *func;  // the literal these types were handed to
    LhatType *args[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t count;
    // What the body settled to under this shape. NULL until a walk of the
    // literal has seen the record -- a call site reading NULL asks for
    // another round rather than inventing an answer.
    LhatType *signature;
    bool failed;  // the body refused these types
    bool advisory;
    bool checking;
    const LhatCheckDiagnostic *failure;
    const char *failure_path;
    LhatFunctionInstance *concrete;
    uint16_t arm;
    struct Instantiation *next;
} Instantiation;

// [pipeline.infer.rounds]: the bookkeeping of a walk that is one iteration of a least
// fixpoint. Two walks are run that way -- a def^'s entries and a statement
// list -- and what they share is only this: where to roll back to, how many
// walks are allowed, and whether the last one changed any answer.
//
// What each of them seeds the next walk with, and what has to be put back
// before it, is their own -- see chk_infer_def and chk_check_statements.
typedef struct {
    // [lsp.hover]: both channels are arrays that only ever grow, so a count is
    // the whole of a mark. What a walk said is dropped when the next one
    // starts: it walks the same ground and says all of it again.
    size_t diagnostics;
#if LHAT_WITH_RESOLUTIONS
    size_t resolutions;
    size_t member_sites;
    size_t binding_sites;
#endif
    size_t round;
    // New call shapes require another walk even if inferred signatures have
    // not changed. The list only prepends records during checking.
    const Instantiation *instantiations;
    // One walk per element plus one: an element settles no later than the one
    // it reads ahead of itself, so a chain of them is done in as many walks
    // as there are elements, and one more finds nothing left to change.
    size_t cap;
    // Whether this walk answered anything differently from the last one.
    bool changed;
    // Saved across the loop, since a def^ or a statement list inside this one
    // runs its own walks and leaves the flag talking about itself.
    bool read_provisional_outside;
} Rounds;

typedef struct {
    // The tree points into both the source text and the lexer's decoded
    // string storage, so the lexer is what has to outlive the result.
    const LhatLexer *lexer;
    const LhatNode *unit;
    LhatCheckResult *result;

    // [modules.host.register]: resolving a registered host signature (lhat_type_of_text).
    // Such a type only describes what the host hands over, so the written
    // tuple-position escape is judged where L^ consumes it instead.
    bool hosted_signature;
    // Only the result of the innermost written signature may name ARGn.
    LhatType *argument_signature;
    bool receiver_type_expression;

    // [modules.deps.static]: how an import is answered. Absent when a unit is checked on
    // its own, in which case a require^ cannot be followed.
    LhatRequire require;

    // [syntax.annot.host]: which of the registered annotations this file has already
    // used, for the ones registered LHAT_ANNOTATION_FILEUNIQUE. One flag per
    // registration, indexed the way `require.annotations` is -- annotations
    // are checked at the unit's head, at each binding and inside each def^,
    // so counting them takes something that outlives one list. NULL when
    // nothing was registered, or when there was no room.
    // [syntax.annot.host]: which node claimed each file-unique annotation. A node rather
    // than a flag because [pipeline.infer.rounds]'s rounds walk the same statements again --
    // the same node seen twice is one writing, only a different node is a
    // second one.
    const LhatNode **annotation_seen;

    Scope *scope;
    // [modules.env.one]: one environment. A name means a value, a type, or both.

    // Innermost first. A branch pushes and pops around its body, so the list
    // is a stack rather than something scopes own.
    Narrowing *narrowings;

    // [syntax.type.fits]: the target of the write being checked, if any. What a branch
    // narrowed *it* to says nothing about what may be written -- the write
    // is what ends that claim -- so the type is asked of the name itself.
    // Set around that one reading only: the value on the right runs before
    // the write, where the claim still holds ('x := x + 1').
    const LhatNode *writing_to;

    // [syntax.type.fits] with [syntax.loop.order]: what a loop condition established. It holds in the
    // clauses that run after the test -- main^, which is the block's own
    // statements ([syntax.loop.body]), and first^ -- and nowhere else: prolog^ and pre^ run
    // before the test, last^ and epilog^ once the loop is over, where for a
    // while^ the condition is what ended it. Set by the loop around the walk
    // of its body and taken by that block on entry, so a block written inside
    // the body does not read it.
    struct LoopTest {
        Narrowing *before;  // where the list is cut back to for the rest
    } *loop_test;

    // [syntax.type.fits]: reading something that was read already, which is what
    // chk_narrow_from does to the path a condition tested. Nothing reports
    // while this stands, so one mistake stays one diagnostic. A counter
    // rather than a flag, for the same reason `deferred` is one.
    size_t rereading;

    // [pipeline.infer]: the parameters whose types the bodies now open are deciding.
    // Innermost first, and a nested body's are pushed in front of the enclosing
    // one's -- a demand made inside one still reaches an outer parameter, since
    // the value it names came from out there.
    ParamVar *param_vars;

    // [pipeline.infer.call-shapes]: the call shapes this unit's walks have recorded, one
    // list across every round -- the rounds roll back what a walk said, and
    // these are what it learned. `instantiating` is the literal whose shapes
    // are being walked right now, so the walk under one shape does not open
    // the shape loop again.
    Instantiation *instantiations;
    struct LhatTemplate *templates;
    size_t template_depth;
    const LhatNode *instantiating;

    // [syntax.type.as-expr]: inside the spelling of a type-as-value right now -- what
    // scopes the old-world rule to the alias: only there does a name
    // reaching its own being-defined binding refuse to mean it. Elsewhere a
    // type position keeps reading the collecting pass's seed, which [syntax.obj.name]'s
    // neighbours pin.
    bool in_type_value;

    // [pipeline.infer.expected]: what this unit writes an op^ for, so that an operator used
    // on a parameter demands the candidates rather than [syntax.op.define]'s built-in
    // alone. Read off the syntax before anything is walked -- [syntax.stmt.visible] makes a
    // name visible over the whole unit, so where the def^ stands relative to
    // the body using the operator cannot matter.
    //
    // The types are not kept: a def^'s is rebuilt on every walk (
    // [pipeline.infer.rounds]), so what is held is the name to look up and which operators it
    // writes, one bit per LHAT_OPERATOR_MEMBERS entry.
    OperatorCarrier *operator_carriers;
    size_t operator_carrier_count;
    size_t operator_carrier_capacity;
    uint16_t unit_operators;  // the union of the bits below, to test in one go

    // [syntax.stmt.visible]: inside a subroutine body nothing runs where it is written, so the
    // ordering rule does not apply. A counter rather than a flag, since
    // bodies nest.
    size_t deferred;

    // inside an if^ clause, a for^ body or a repeat^ body, nothing
    // guarantees this specific piece of code runs at all before some later
    // point reads what it wrote -- unlike `deferred` above, this still runs
    // immediately if it runs, so [syntax.stmt.visible]'s own rule does not read this one.
    // A counter, the same way and for the same reason as `deferred`.
    size_t conditional;

    // [syntax.stmt.nil-assign]: the place a '?op=' reads. It answers without its nil^ arm --
    // the form is written to say the write is skipped when the place is
    // absent, so what the operator is asked of is what is there. One node
    // rather than a depth: the very same shape standing in the right-hand
    // side is nobody's place, and goes on carrying its nil^.
    const LhatNode *nil_safe_place;

    // The result type of the subroutine being checked, for [errors.try.result] and for
    // collecting return^ when [pipeline.infer] has to infer one.
    LhatType *declared_result;
    LhatType *inferred_result;
    bool saw_self_call;
    // [pipeline.infer]: a return^ whose value goes through the subroutine itself.
    // Its type is the one being worked out, so it is left out of the union.
    bool recursive_return;
    // [pipeline.infer] counts every exit. A bare return^ is one that produces no
    // value, exactly like reaching the end of the body -- and it leaves the
    // same nil^ behind at run time.
    bool valueless_return;

    // [syntax.type.tuple]: whether the type about to be resolved may be a tuple. Consumed
    // by resolve_type on entry, so every nested resolve refuses one unless it
    // deliberately sets this again -- which only a signature's result, a
    // coroutine's Y and T, and the arms of a union do. Refusing everywhere
    // else is the whole of what confines a tuple to a result position, and
    // that confinement is what keeps 13.8's four propagations from coming
    // back: an argument cannot hold one, so [syntax.type.variadic]'s expansion rule has nothing
    // to expand; a name cannot, so there is nothing to compose further.
    bool tuple_allowed;

    // [syntax.sub.this]: the type of the subroutine whose body is being checked,
    // which is what this^ names. NULL outside any body.
    LhatType *this_type;

    // [syntax.sub.this]: the bodies enclosing this one, outermost last -- what
    // this^^ walks. Links live on the C stack of the infer that pushed them,
    // the same way the saved this_type does.
    struct ThisLink {
        LhatType *type;
        struct ThisLink *outer;
        const LhatNode *body;
    } *this_link;

    // [syntax.type.self]: the written type literals enclosing this point, innermost
    // first -- what Self^ names, and what a second hat counts outwards. Only
    // t^{ … } and def^{ … } bind one: a signature and a coroutine type are
    // written around a type rather than being a structure of their own, so
    // 'f^Self^ -> Self^;' inside a def^ still names the definition's instance,
    // which is what makes a method able to say its own type at all.
    //
    // A def^ binds the instance ([syntax.obj.instance]), the same object self^ is bound to --
    // self^ is one value of that type, Self^ is the type. Links live on the C
    // stack of the resolve/infer that pushed them, like this_link above.
    struct SelfLink {
        LhatType *type;
        struct SelfLink *outer;
    } *self_link;

    // [syntax.obj.name] with [syntax.stmt.visible]: the definitions being built and the names they are on
    // their way into. A def^ writing its own name inside itself reached the
    // pending^ seed the collecting pass leaves for every let^, and pending^
    // conforms to everything -- so 'x:T' inside T asked nothing at all, while
    // the same annotation outside asked for the whole structure.
    //
    // Only resolve_type reads this. The *value* is genuinely not assigned yet
    // at that point, which is [syntax.stmt.visible]'s own business and stays as it was; what a
    // name means as a type is settled by the def^ itself ([syntax.obj.instance]'s instance),
    // and that exists from the first line of infer_def onwards.
    struct DefLink {
        const Binding *binding;
        const LhatNode *node;    // the def^ this name is being bound to
        LhatType *instance;      // NULL until infer_def has made it
        struct DefLink *outer;
    } *def_link;

    // [syntax.sub.kinds]: f^ may call only f^, never p^. True inside an f^ body
    // (and nested f^ literals within it), false everywhere else -- outside
    // any body (top level) and inside a p^ both allow either kind, so
    // false is the right default with no body open at all.
    bool in_function;

    // [syntax.sub.kinds]'s other half: an f^ assigns to local variables only. This is the
    // scope its parameters were bound in, which is the outermost one its body
    // made -- a name found at or inside it is the body's own, and one found
    // past it belongs to whoever called. NULL outside any body. Saved and
    // restored around a nested one the same way `in_function` is, so an f^
    // written inside another measures against its own.
    Scope *body_scope;


    // [errors.catch.clause]: the statements with catch^ arms being checked, if any. A
    // try^ written in them hands its errors here rather than to the
    // subroutine's result -- what no arm takes is what goes on out
    // (check_arms). A subroutine body written inside them clears it: a try^
    // there belongs to that body.
    struct CatchFrame {
        LhatType *caught;
        struct CatchFrame *outer;
        // A try^ over the body's own call, whose result is still being inferred:
        // what these arms may find is not all known yet.
        bool undecided;
    } *catch_frame;

    // [syntax.obj.super]: what an override^ is writing over, which is what super^
    // names. NULL anywhere else, so [syntax.obj.override]'s marker is what makes it a name.
    LhatType *super_type;
    const LhatNode *super_binding; // Override entry introducing the replaced value.

    // [syntax.obj.new]: the written new whose literal chk_infer_def is walking.
    // chk_infer_func reads it to know the body it opens is a constructor's:
    // the machine makes the instance, the body only adjusts it.
    const LhatNode *new_func;
    // And true exactly while that body's own statements are walked -- a
    // literal nested inside it is an ordinary body again. return^ (refused
    // there) and [syntax.sub.kinds]'s write rules (self^ is the body's to write) read it.
    bool in_new_body;

    // [syntax.sub.mutable-self]: the body being walked is an f^ whose receiver is mutable, so
    // a write through self^ -- direct, or another mutable^self^ call on it
    // -- is what its own signature promised. Saved and restored around each
    // literal, as in_new_body above is.
    bool receiver_mutable;
    // [syntax.sub.table-write]: self^'s ownership while either of the two above holds -- whole
    // on entry, by the signature's word (or the copy the machine just made),
    // and lowered by what the body stores into it. Saved and restored with
    // them.
    ChkOwnership self_owned;

    // [syntax.sub.fresh]: the body being walked wrote '-> fresh^T', so every exit owes
    // a value this body made. Saved and restored the same way.
    bool must_answer_fresh;

    // [pipeline.infer.rounds]: something answered before it was inferred -- a def^ member
    // read before its own body was walked ([syntax.obj.delegate]'s seed), or a name whose
    // let^ this walk has not reached yet ([syntax.stmt.visible]). Both say the same thing: this
    // walk read ahead, so walking again from what it learned may answer
    // better. Set by the member search and by the name search, read by the
    // two round loops below.
    bool read_provisional;

    // [pipeline.infer.rounds]: this is a second or later walk over the same statements.
    // What the first walk bound is already bound, so the statements that bind
    // (import^) or grow a table ([syntax.stmt.member]'s path) would otherwise report their own
    // first walk as a redefinition.
    bool rewalking;

    // [pipeline.infer.expected]: the signature a subroutine literal is expected to have, or
    // NULL where nothing expects one. Set around the one chk_infer() that
    // reaches the literal, the way super_type and yield_bound_type are, and
    // read by chk_infer_func for the parameters nothing was written on.
    // What it supplies stands exactly where a written annotation would.
    LhatType *expected_func;

    // [pipeline.machine]: the statement whose value the input answers with, when the
    // input is one of a session. NULL for a file, where nothing answers.
    const LhatNode *answering;

    // [pipeline.machine] with [modules.hostvalue]: checking one input of a session. A session's
    // top-level names keep their one-slot places across inputs, which a
    // host value's width does not fit -- so the top level of a prompt
    // refuses one where a unit's top level (one ordinary frame) does not.
    bool session;

    // [modules.host.machine]: the type L^ answers. One per check, since [modules.hostdata] lets a path
    // add to it and every mention has to see the same table. A session hands
    // its own in, so what one input registers is there for the next.
    LhatType *environment;

    // [syntax.obj.typeof]: what every typeof^(...) answers with. Nominal ([syntax.stmt.member]'s
    // reason applies the same way: what it carries is fixed and comparing two
    // by shape would say nothing), so every mention has to be the very same
    // object -- kept here and handed forward by a session the way
    // `environment` is.
    LhatType *typeinfo_type;
    // [errors.kind.local]: localerror^.CastFailure, interned for the same reason.
    LhatType *cast_failure_type;

    // [syntax.sub.yieldable]: what the yield^/await^ sites seen so far in this body agree
    // on. infer_func saves and resets these around a nested body the same
    // way it does declared_result/inferred_result, so a nested p^{...} does
    // not pollute the enclosing one.
    LhatType *coroutine_produce;  // Y
    LhatType *coroutine_receive;  // R

    // How the yield^ chk_infer() is about to see is being used. check_define
    // sets BOUND with the let^ target's annotation just around inferring
    // that one value; check_statement sets DISCARD around a bare yield^
    // statement. Left at NONE everywhere else, including inside whatever
    // chk_infer() recurses into to produce a yield^'s own value -- that value is
    // never itself the direct target of a binding.
#if LHAT_WITH_RESOLUTIONS
    // [lsp.hover]: the member chk_infer_member answered from, for the record it
    // makes on the way back out. Held here rather than handed back: that
    // function returns from thirty-odd places and only two of them have a
    // member record in hand. Cleared as it is entered and read immediately
    // after it returns, so the one that mattered is the last one written --
    // the target of a member access is inferred before its own lookup runs.
    const LhatTypeMember *resolved_member;
    // The same, for [syntax.enum]'s enum member read in value position: what
    // is answered is the wide ENUM type, so the singleton the name reached
    // -- and with it the place it was written -- is left here instead.
    const LhatType *resolved_kind;
#endif
    enum YieldContext {
        YIELD_CTX_NONE,
        YIELD_CTX_DISCARD,
        YIELD_CTX_BOUND
    } yield_context;
    LhatType *yield_bound_type;  // YIELD_CTX_BOUND only; NULL means "no annotation"
} Checker;

typedef struct LhatTemplate {
    const LhatNode *node;
    const char *module_name;
    Checker environment;
    LhatType *signature;
    Instantiation *instances;
    struct LhatTemplate *next;
} LhatTemplate;

void chk_freeze_templates(Checker *c, const Scope *scope);
void chk_check_task_initialization(Checker *c);


void chk_report(Checker *c, const LhatNode *at, LhatCheckErrorCode code);

// [lsp.next]: the name nearest one that was written, among names offered one at a
// time -- a misspelling's likeliest meaning. Near is an edit distance
// (adjacent swaps count once, ASCII case not at all) of at most a third of
// what was written and at least one; of two equally near, the first offered
// wins, so an offerer walks innermost first. The spelling is copied, since
// what offers it may hold it only for the call.
#define CHK_NEAR_NAME 128

typedef struct {
    const char *written;
    size_t written_length;
    char best[CHK_NEAR_NAME];
    size_t best_length;  // 0 while nothing near enough has been offered
    size_t distance;
} ChkNearest;

// Answers whether looking is worth it: nothing is reported on a round that
// only re-reads (chk_report), so there a search would find what no one sees.
bool chk_nearest_start(Checker *c, ChkNearest *near, const char *written,
                       size_t length);
void chk_nearest_offer(ChkNearest *near, const char *name, size_t length);
// The sinks for the two member walks (check.h), offering what they are
// handed to the ChkNearest in `context`.
void chk_nearest_member(void *context, const LhatTypeMember *member);
void chk_nearest_builtin(void *context, const char *name, size_t length,
                         LhatType *type);
// Every name `c` can read from where it stands: its scopes innermost first,
// then what the host bound ([modules.host.initial]).
void chk_nearest_in_scope(Checker *c, ChkNearest *near);

// Reports as chk_report_named does, offering the nearest name as the fix --
// written over `spelt`, the node whose source is the misspelling -- when one
// was near enough. `spelt` NULL offers nothing.
void chk_report_near(Checker *c, const LhatNode *at, LhatCheckErrorCode code,
                     const char *name, size_t length, const LhatNode *spelt,
                     const ChkNearest *near);

// [lsp.next] with the above: the whole of a report -- the name the diagnostic is
// about (NULL for the codes that name none) and the fixes it knows how to
// make (NULL for none, at most LHAT_FIX_SLOTS of them). chk_report and
// chk_report_named are this with one or both left out.
void chk_report_fix(Checker *c, const LhatNode *at, LhatCheckErrorCode code,
                    const char *name, size_t name_length,
                    const LhatFixSlot *fixes, size_t count);
#if LHAT_WITH_RESOLUTIONS
// [lsp.hover]: for the language server, not for the language -- see check.h's
// LhatResolution. Every call site carries this same guard.
void chk_record_resolution(Checker *c, const LhatNode *at, const Binding *b);
void chk_record_narrowed_resolution(Checker *c, const LhatNode *at,
                                    const Binding *b, LhatType *type);
void chk_record_typed_resolution(Checker *c, const LhatNode *at,
                                 LhatType *type);
// The same, for a member: [syntax.obj.structure] finds one in a type rather than in a
// scope, and the type knows where it was written (type.h's declared_at).
// `member` may be NULL, which is the typed record above.
// `builtin` says the language answered rather than anything written or
// registered -- see check.h's LhatResolution.
void chk_record_member_resolution(Checker *c, const LhatNode *at,
                                  LhatType *type,
                                  const LhatTypeMember *member, bool builtin);
// The same, for [errors.kind.errordef]'s error kind and [syntax.enum]'s enum member: found in
// the set that declared them, and the type carries the place (type.h's
// v.error.declared_at). `type` is what the name answers there -- the kind
// itself in type position, the wide ENUM in value position.
void chk_record_kind_resolution(Checker *c, const LhatNode *at, LhatType *type,
                                const LhatType *kind);
// And for a require^'s written path: what it reaches is a whole unit, so the
// place is that unit's start. `unit_path` is the resolver's answer, or NULL
// when it gave none -- then the record has the exports and no place.
void chk_record_unit_resolution(Checker *c, const LhatNode *at,
                                LhatType *exports, const char *unit_path);

// [lsp.hover]: the '.' of a member access and what stands to its left.
// Made where the receiver is inferred rather than where the answer comes
// back -- see check.h's LhatMemberSite.
void chk_record_member_site(Checker *c, const LhatNode *node,
                            LhatType *receiver, bool number_word);

void chk_settle_resolutions(LhatCheckResult *result);
void chk_settle_member_sites(LhatCheckResult *result);
#endif
// Marks where `member` was written, for the record above to point at.
// Outside the guard and a no-op without it: the sites that call this hold a
// written node either way, and wrapping every one of them in its own #if
// would say the same thing a dozen times. A no-op without a member or a
// node too -- the sites that add a member the writer did not write (a host
// registration, a built-in) simply have none to pass.
void chk_member_declared_at(Checker *c, LhatTypeMember *member,
                           const LhatNode *at);
// The same mark on an error kind or an enum member (type.h's v.error).
void chk_kind_declared_at(Checker *c, LhatType *kind, const LhatNode *at);
void chk_report_named(Checker *c, const LhatNode *at,
                      LhatCheckErrorCode code, const char *name,
                      size_t length);
// Mention name in the message, but underline the supplied node's source span.
void chk_report_named_span(Checker *c, const LhatNode *at,
                           LhatCheckErrorCode code, const char *name,
                           size_t length);
bool chk_node_name(const Checker *c, const LhatNode *node,
                   const char **text, size_t *length);
bool chk_name_is(const char *text, size_t length, const char *literal);
bool chk_is_super_name(Checker *c, const LhatNode *node);
bool chk_is_discard(const Checker *c, const LhatNode *node);
void chk_refuse_hat_name(Checker *c, const LhatNode *node, bool member);
Binding *chk_scope_find_local(Scope *scope, const char *name, size_t length);
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found);
Binding *chk_scope_find_skipping(Scope *scope, const char *name,
                                 size_t length, size_t skip);
Scope *chk_scope_from(Scope *scope, const LhatNode *node);
Binding *chk_scope_add(Scope *scope, const char *name, size_t length,
                       LhatType *type, uint32_t offset);

// Opens a layer under `parent`, covering the source `node` was written
// across. `node` is NULL for a layer that is not a piece of a program -- the
// members of a def^ while its entries are read, a session's one scope, the
// names a host registered -- and such a layer records nothing.
void chk_scope_open(Scope *scope, Scope *parent, const LhatNode *node,
                    bool transparent);

// Closes it, recording what it held ([lsp.hover]) before freeing it.
void chk_scope_close(Checker *c, Scope *scope);
LhatType *chk_simple(Checker *c, LhatTypeKind kind);

// [errors.kind.local]: 'error^|localerror^' -- what the operators mean by "an error"
// now that there are two disjoint families. catch^ takes either, and so does
// a bare catch^: arm; only try^ tells them apart, by refusing one.
LhatType *chk_any_error(Checker *c);

// [errors.kind.local-no-propagate]: does this type touch localerror^ anywhere a caller could see
// it? Walks unions, tuple positions, t^{} members and a subroutine's result,
// and treats a nominal type (def^, an error kind, a host type) as an atom --
// see the comment on the definition for why the atom matters.
bool chk_type_touches_local(const LhatType *type, unsigned depth);

// [syntax.annot.host]: what a run of annotations is asked, where `target` is the one
// LhatAnnotationTarget the place they were written is.
void chk_check_annotations(Checker *c, const LhatNode *list, uint32_t target);
int chk_self_marker_at(const Checker *c, const LhatNode *params,
                       const LhatNode *param);
LhatType *chk_resolve_func_type(Checker *c, const LhatNode *node);

// [syntax.obj.members-order] with [syntax.obj.self]: whether an instance carries a member of this type. What
// may be reached through one is what is handed a receiver, which its signature
// says; new, a static member and a value are the definition's alone.
bool chk_takes_receiver(const LhatType *type);

// [errors.try.result] with [pipeline.infer]: an error leaving the body being checked, through a try^
// or through catch^ arms that did not take it.
void chk_error_leaves(Checker *c, const LhatNode *at, LhatType *escaping);

bool chk_may_stand_beside_tuple(const LhatType *type);
bool chk_contains_error(const LhatType *type);

// [syntax.type.tuple]: what every position of a tuple has to satisfy, wherever the tuple
// came from -- 'return^ a, b', 'yield^ a, b', or the literal '(a, b)'. An
// error among the values rather than around them ([errors.dropped.around-value]), a position
// that is itself a run, and a host value that would not fit one slot are all
// refused here so the three spellings cannot drift apart.
void chk_check_tuple_position(Checker *c, const LhatNode *at,
                              const LhatType *position);
bool chk_contains_tuple(const LhatType *type);
LhatType *chk_resolve_type(Checker *c, const LhatNode *node);
LhatType *chk_only(Checker *c, LhatType *type, LhatType *wanted);
LhatType *chk_without(Checker *c, LhatType *type, LhatType *unwanted);
bool chk_can_be(const LhatType *type, const LhatType *wanted);
LhatType *chk_require_value(Checker *c, const LhatNode *at, LhatType *type);
const char *chk_operator_name(LhatOpKind op, size_t *length);
bool chk_is_operator_name(const char *name, size_t length);
uint16_t chk_operator_bit(const char *name, size_t length);
void chk_collect_operator_carriers(Checker *c, const LhatNode *statements);
void chk_dispose_operator_carriers(Checker *c);
void chk_check_operator_shape(Checker *c, const LhatNode *at,
                              const LhatType *type, const char *name,
                              size_t length);
void chk_check_tostring_shape(Checker *c, const LhatNode *at, const LhatType *type,
                              const char *name, size_t length, bool both);
void chk_refuse_self_last(Checker *c, const LhatNode *at,
                          const LhatType *type);
LhatType *chk_builtin_operator(Checker *c, LhatTypeKind carrier,
                               const char *name, size_t length);
LhatType *chk_operator_member(Checker *c, const LhatType *type,
                              const char *name, size_t length);
bool chk_operator_undecided(const LhatType *type);
bool chk_narrowable(const LhatNode *node);
// [syntax.type.fits-range]: a whole number written down, which is what a bound is read off.
bool chk_whole_literal(const LhatNode *node, LhatInteger *value);
LhatType *chk_narrowed_type(Checker *c, const LhatNode *path);
// [syntax.type.fits-range]: the whole numbers this path may hold here, or false where nothing
// bounded it. Both ends come back, INT64_MIN and INT64_MAX standing for a
// side nothing said anything about.
bool chk_narrowed_bounds(Checker *c, const LhatNode *path, LhatInteger *lo,
                         LhatInteger *hi);
// The same asked of an expression: a written number, a bounded path, or one
// of those with a written number added or taken away.
bool chk_bounds_of(Checker *c, const LhatNode *node, LhatInteger *lo, LhatInteger *hi);
void chk_push_bounds(Checker *c, const LhatNode *path, LhatInteger lo, LhatInteger hi);
void chk_pop_narrowings(Checker *c, Narrowing *mark);
void chk_drop_narrowings_for(Checker *c, const LhatNode *target);
bool chk_always_exits(const LhatNode *node);
void chk_narrow_from(Checker *c, const LhatNode *condition, bool truth);
ParamVar *chk_param_var_for(Checker *c, const LhatType *type);
bool chk_mentions_function_coroutine(const LhatType *type, unsigned depth);
void chk_constrain(Checker *c, LhatType *value, LhatType *wanted);
void chk_close_param_var(Checker *c, const LhatType *value);
void chk_constrain_member(Checker *c, LhatType *target, const char *name,
                          size_t length);
ParamVar *chk_push_param_var(Checker *c, LhatType *slot, const LhatNode *node);
void chk_settle_param_vars(Checker *c, ParamVar *mark);

// [pipeline.infer.call-shapes]. Finds the record of this shape for this literal, adding one
// when it is new -- two shapes are the same when their types are ([syntax.op.semantic], so
// lhat_type_equal). NULL only when memory refuses.
Instantiation *chk_instantiation_add(Checker *c, const LhatNode *func,
                                     LhatType *const *args, size_t count);
bool chk_instantiations_exist(const Checker *c, const LhatNode *func);
void chk_detach_instantiations(Checker *c);
void chk_rounds_begin(Checker *c, Rounds *r, size_t count);
bool chk_rounds_next(Checker *c, Rounds *r);
void chk_rounds_end(Checker *c, Rounds *r);
void chk_expect(Checker *c, const LhatNode *at, LhatType *value,
                LhatType *target, LhatCheckErrorCode code);
LhatType *chk_infer_name(Checker *c, const LhatNode *node,
                         LhatType **named_type);
LhatType *chk_infer_binary(Checker *c, const LhatNode *node);
bool chk_signature_accepts(const LhatType *func, LhatType *const *args,
                           size_t count, bool through_member);
LhatType *chk_infer_call(Checker *c, const LhatNode *node);
LhatType *chk_table_walk_tuple(Checker *c, const LhatType *over);
LhatType *chk_table_element_type(Checker *c, const LhatType *over);
LhatType *chk_without_nil_arm(Checker *c, LhatType *target);
// [syntax.obj.substring]: the bare spellings the built-in cascade answers to, for the one
// caller that lists them rather than looking one up. See check_member.c, where
// it stands next to the cascade it belongs to.
extern const char *const chk_builtin_words[];
extern const size_t chk_builtin_word_count;

// [syntax.obj.structure] with [syntax.obj.substring]: what a type answers for a member of this name, asked
// rather than walked into. `node` NULL asks it quietly -- see check_member.c.
LhatType *chk_member_of(Checker *c, LhatType *target, const char *name,
                        size_t length, const LhatNode *node,
                        LhatType **named_type);
LhatType *chk_infer_member(Checker *c, const LhatNode *node,
                           LhatType **named_type);
void chk_unify_yield(Checker *c, const LhatNode *at, LhatType **slot,
                     LhatType *candidate);
LhatType *chk_infer_func(Checker *c, const LhatNode *node);
const LhatTypeMember *chk_members_search(const LhatTypeMember *members,
                                         const char *name, size_t length);
// [errors.kind.made]: the kind this errordef^ declares under `name`, or NULL. Asked
// from the type side, where it is the answer, and from the value side, where
// finding one is what tells a type written in the wrong place from a name
// nothing declares.
LhatType *chk_kind_of_set(const LhatType *set, const char *name,
                          size_t length);
const LhatTypeMember *chk_find_member(const LhatType *table,
                                      const char *name, size_t length);
LhatType *chk_instance_of(const LhatType *definition);
const LhatTypeMember *chk_unimplemented_member(const LhatType *definition);
const LhatTypeMember *chk_hole_of(const LhatType *definition, bool *field);
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right);
LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base);
bool chk_is_hostvalue(const LhatType *type);
LhatType *chk_infer(Checker *c, const LhatNode *node);
// Infer once, also returning the alias target of a spelling or name path.
LhatType *chk_infer_with_named_type(Checker *c, const LhatNode *node,
                                    LhatType **named_type);
LhatType *chk_environment_type(Checker *c);
LhatType *chk_typeinfo_type(Checker *c);
LhatType *chk_typeinfo_of(Checker *c, LhatType *represented);
// [syntax.sub.no-contagion] with [syntax.type.empty]: what a call of this signature answers -- the coroutine a
// yielding body makes, its result, or NONE where it answers no value.
LhatType *chk_call_answer(Checker *c, const LhatType *callee);
LhatType *chk_cast_failure_type(Checker *c);
void chk_check_define(Checker *c, const LhatNode *node);
LhatType *chk_module_root_table(Checker *c);
void chk_register_module_type(Checker *c, const char *module_name,
                              LhatType *exports);
LhatType *chk_hosted_module(Checker *c, const LhatNode *path);
// [syntax.sub.table-write]: what the body being checked owns (ChkOwnership).
uint8_t chk_made_depth(const Checker *c, const LhatNode *value,
                       const LhatType *type);
bool chk_lean_on(Checker *c, const LhatNode *place, bool whole);
void chk_store_into(Checker *c, const LhatNode *at, const LhatNode *place,
                    uint8_t made);
void chk_store_into_self(Checker *c, const LhatNode *at, uint8_t made);
void chk_bind_own(Checker *c, Binding *b, const LhatNode *value,
                  const LhatType *type);
bool chk_receiver_is_own_coroutine(Checker *c, const LhatNode *receiver);
bool chk_scope_within_body(Checker *c, const Scope *found_in);
bool chk_scope_within(Checker *c, const Scope *found_in, const Scope *boundary);
void chk_check_write_target(Checker *c, const LhatNode *target);
void chk_check_opaque_write(Checker *c, const LhatNode *target);
char *chk_read_module_name(const Checker *c, const LhatNode *statements);
LhatType *chk_collect_exports(Checker *c, const LhatNode *statements);
void chk_check_statements(Checker *c, const LhatNode *statements);
void chk_check_block_in_scope(Checker *c, const LhatNode *node);
void chk_check_statement(Checker *c, const LhatNode *node);

// What check_expr.c shares with the files split off it: check_member.c,
// check_object.c and check_func.c.
bool chk_runtime_has_unresolved_arguments(const LhatType *type);
LhatType *chk_access_target(Checker *c, const LhatNode *node, LhatType *type);
const LhatType *chk_operator_arm(const LhatType *carrier,
                                 LhatType *const *args, size_t count,
                                 bool self_last);
void chk_report_no_member(Checker *c, const LhatNode *node, LhatType *target,
                          const char *name, size_t length);
void chk_join_table_entry_types(Checker *c, const LhatType *table,
                                LhatType **keys, LhatType **values);
LhatType *chk_coroutine_made_by(Checker *c, const LhatType *func);
LhatType *chk_coroutine_answer(Checker *c, const LhatType *coro);
void chk_hostvalue_temporary_write(Checker *c, const LhatNode *at,
                                   const LhatNode *receiver,
                                   const LhatType *type);
void chk_retain_instance_failure(Checker *c, Instantiation *instance,
                                 size_t start);
LhatType *chk_infer_template(Checker *c, const LhatNode *node);
LhatType *chk_nil_propagated(Checker *c, const LhatNode *node,
                             LhatType *answer);
bool chk_run_of_names(const LhatNode *node, bool allow_scope);
bool chk_string_member_key(Checker *c, const LhatNode *key);
LhatType *chk_infer_index(Checker *c, const LhatNode *node);
LhatType *chk_infer_table(Checker *c, const LhatNode *node);
LhatType *chk_declared_signature(Checker *c, const LhatNode *node);

#endif  // LHAT_CHECK_INTERNAL_H
