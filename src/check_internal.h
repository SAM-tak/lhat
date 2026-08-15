// L^ (lhat) -- the checker's own insides, shared by check.c, check_expr.c
// and check_stmt.c and included by nothing else. The chk_ prefix marks a
// symbol that crosses those three files and must not collide with anything
// at link time.

#ifndef LHAT_CHECK_INTERNAL_H
#define LHAT_CHECK_INTERNAL_H

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "environment.h"
#include "lhat/config.h"
#include "grow.h"
#include "lhat/port.h"

// 05 の 8.7: a host writes a type out as text, so the checker reads the type
// grammar of 13 章 back through the parser it came from.
#include "parser.h"
#include "lhat/source.h"

// A name bound in a scope. `offset` is where its let^ stands, which 8.7 needs
// to tell a use before the definition from one after it.
typedef struct Binding {
    const char *name;
    size_t name_length;
    LhatType *type;
    // 03 の 3.4改2: what the walk before this one left under the name, which
    // is what this walk started from. NULL until a second walk is run, and
    // read only to tell a walk that learned something from one that answered
    // what the last one already had.
    LhatType *seed;
    uint32_t offset;
    bool reached;  // its let^ has been walked past
    // 15.1改: bound to a table this scope's own code made -- a literal or a
    // 14.11 new -- rather than to one that arrived from somewhere else. An
    // f^ may change the first and not the second, and reading it off the
    // initialiser's shape is what keeps that decidable without following
    // aliases: 'let^ v = t' binds a name, not a new table, so it is false.
    bool fresh;
    // 03 の 4.3: bound by an earlier input of a session. Writing the name
    // again is a redefinition rather than the clash 8.7 makes of two let^ in
    // one scope -- it is the same place, written again.
    bool from_session;
    // 8.9: introduced by a let^ rather than a var^, so nothing written after
    // it may reassign the name. 12 章's with^ and 16.3's in^ focus bind this
    // way too -- the first because the block disposes of whatever the name
    // still holds, the second because each turn binds it afresh.
    //
    // This says nothing about the value: a table a let^ holds is still the
    // table it was, and 8.8's members of it are still written through 15.1改
    // and 05 の 8.6. What it refuses is the name coming to mean something
    // else.
    bool immutable;
    // 8.9 with 12.1 and 16.3改2: the construct bound it, not a word the writer
    // chose -- a with^, or the focus of a counted or walking for^. Writing
    // var^ instead is not open to them, so the diagnostic must not offer it.
    bool bound_by_form;
    // 05 の 8.7: the root an import^ bound. A name under it is read off
    // L^.modules wherever it is written, so naming one captures nothing --
    // which is what 15.13 has to know to let a closed^ body write it. A
    // require^ landing on the same root clears this, the same way
    // compile.c's Local does.
    bool import_root;
    // 13.1: declared by a signature rather than bound by a let^ or var^.
    // Nothing in the checker reads it -- 07 の 4 章 does, since where a use
    // stands says only that there is a name, and the type says only what it
    // holds. Which of the two declared it is known here and nowhere else.
    bool is_parameter;
    struct Binding *next;
} Binding;

typedef struct Scope {
    Binding *bindings;
    Binding *tail;
    struct Scope *parent;
    // 02 の 9.4 with 01 の 8 章: a layer that holds names without being a '{'
    // anyone wrote. The loop clauses lasting the whole loop (prolog^, first^)
    // live in one of these, around the layer main^ binds into -- so a name
    // outlives the iteration without '$^' finding an extra step on the way
    // out. compile.c counts the same one scope for a loop body.
    bool transparent;
} Scope;

// 13.11. What a branch knows about a value that the binding does not: inside
// 'if^ r is^ T', r is only the arms of its union that conform to T. Held as
// the path it was written with rather than as a binding, since 13.11 admits
// a dot path as well as a plain name.
typedef struct Narrowing {
    const LhatNode *path;
    LhatType *type;
    struct Narrowing *next;
} Narrowing;

// 03 の 3.4: a parameter written without a type, while the body that decides
// it is being read. `slot` is the pending^ object infer_func made, and the
// binding and the signature both hold that very pointer -- settling one writes
// through to both, which is what keeps this to a single walk of the body.
//
// A member reached off such a parameter gets one of these too, so that
// 'x.foo(1)' can demand t^{ foo : p^number^; } rather than only the name.
// Its slot is the member's type inside the demand its parent is collecting.
typedef struct ParamVar {
    LhatType *slot;
    // Where the parameter was written, so 03 の 3.1 can say which one nothing
    // decided. NULL where there is no such place to point at.
    const LhatNode *node;
    // What positions that always run have asked of it, and what the ones
    // under a branch have. Kept apart because 3.4 settles them differently:
    // the first is a requirement, the second is only added when it does not
    // contradict.
    LhatType *demanded;
    LhatType *conditional;
    // Two demands from under branches ruled each other out, so the branches
    // disagree and nothing here is decided (3.4 leaves that unknown^ rather
    // than reporting -- the uses report for themselves).
    bool conditional_dropped;
    // Reassigned, self-referential, or two unconditional demands ruled each
    // other out. Collection stops and the answer is unknown^.
    bool closed;
    struct ParamVar *next;
} ParamVar;

// 03 の 3.4改2: the bookkeeping of a walk that is one iteration of a least
// fixpoint. Two walks are run that way -- a def^'s entries and a statement
// list -- and what they share is only this: where to roll back to, how many
// walks are allowed, and whether the last one changed any answer.
//
// What each of them seeds the next walk with, and what has to be put back
// before it, is their own -- see chk_infer_def and chk_check_statements.
typedef struct {
    // 07 の 4 章: both channels are arrays that only ever grow, so a count is
    // the whole of a mark. What a walk said is dropped when the next one
    // starts: it walks the same ground and says all of it again.
    size_t diagnostics;
#if LHAT_WITH_RESOLUTIONS
    size_t resolutions;
#endif
    size_t round;
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
    LhatCheckResult *result;
    bool strict;

    // 05 の 6.1: how an import is answered. Absent when a unit is checked on
    // its own, in which case a require^ cannot be followed.
    LhatRequire require;

    Scope *scope;
    // 05 の 2.2: one environment. A name means a value, a type, or both.

    // Innermost first. A branch pushes and pops around its body, so the list
    // is a stack rather than something scopes own.
    Narrowing *narrowings;

    // 13.11: the target of the write being checked, if any. What a branch
    // narrowed *it* to says nothing about what may be written -- the write
    // is what ends that claim -- so the type is asked of the name itself.
    // Set around that one reading only: the value on the right runs before
    // the write, where the claim still holds ('x := x + 1').
    const LhatNode *writing_to;

    // 13.11 with 9.2: what a loop condition established. It holds in the
    // clauses that run after the test -- main^, which is the block's own
    // statements (9.3), and first^ -- and nowhere else: prolog^ and pre^ run
    // before the test, last^ and epilog^ once the loop is over, where for a
    // while^ the condition is what ended it. Set by the loop around the walk
    // of its body and taken by that block on entry, so a block written inside
    // the body does not read it.
    struct LoopTest {
        Narrowing *before;  // where the list is cut back to for the rest
    } *loop_test;

    // 13.11: reading something that was read already, which is what
    // chk_narrow_from does to the path a condition tested. Nothing reports
    // while this stands, so one mistake stays one diagnostic. A counter
    // rather than a flag, for the same reason `deferred` is one.
    size_t rereading;

    // 03 の 3.4: the parameters whose types the bodies now open are deciding.
    // Innermost first, and a nested body's are pushed in front of the enclosing
    // one's -- a demand made inside one still reaches an outer parameter, since
    // the value it names came from out there.
    ParamVar *param_vars;

    // 8.7: inside a subroutine body nothing runs where it is written, so the
    // ordering rule does not apply. A counter rather than a flag, since
    // bodies nest.
    size_t deferred;

    // inside an if^ clause, a for^ body or a repeat^ body, nothing
    // guarantees this specific piece of code runs at all before some later
    // point reads what it wrote -- unlike `deferred` above, this still runs
    // immediately if it runs, so 8.7's own rule does not read this one.
    // A counter, the same way and for the same reason as `deferred`.
    size_t conditional;

    // 8.6改2: the place a '?op=' reads. It answers without its nil^ arm --
    // the form is written to say the write is skipped when the place is
    // absent, so what the operator is asked of is what is there. One node
    // rather than a depth: the very same shape standing in the right-hand
    // side is nobody's place, and goes on carrying its nil^.
    const LhatNode *nil_safe_place;

    // The result type of the subroutine being checked, for 04 の 5.3 and for
    // collecting return^ when 03 の 3.4 has to infer one.
    LhatType *declared_result;
    LhatType *inferred_result;
    bool saw_self_call;
    // 03 の 3.4: a return^ whose value goes through the subroutine itself.
    // Its type is the one being worked out, so it is left out of the union.
    bool recursive_return;
    // 03 の 3.4 counts every exit. A bare return^ is one that produces no
    // value, exactly like reaching the end of the body -- and it leaves the
    // same nil^ behind at run time.
    bool valueless_return;

    // 13.8改: whether the type about to be resolved may be a tuple. Consumed
    // by resolve_type on entry, so every nested resolve refuses one unless it
    // deliberately sets this again -- which only a signature's result, a
    // coroutine's Y and T, and the arms of a union do. Refusing everywhere
    // else is the whole of what confines a tuple to a result position, and
    // that confinement is what keeps 13.8's four propagations from coming
    // back: an argument cannot hold one, so 13.7's expansion rule has nothing
    // to expand; a name cannot, so there is nothing to compose further.
    bool tuple_allowed;

    // 02 の 15.10: the type of the subroutine whose body is being checked,
    // which is what this^ names. NULL outside any body.
    LhatType *this_type;

    // 15.10: the bodies enclosing this one, outermost last -- what
    // this^^ walks. Links live on the C stack of the infer that pushed them,
    // the same way the saved this_type does.
    struct ThisLink {
        LhatType *type;
        struct ThisLink *outer;
    } *this_link;

    // 13.13: the written type literals enclosing this point, innermost
    // first -- what Self^ names, and what a second hat counts outwards. Only
    // t^{ … } and def^{ … } bind one: a signature and a coroutine type are
    // written around a type rather than being a structure of their own, so
    // 'f^Self^ -> Self^;' inside a def^ still names the definition's instance,
    // which is what makes a method able to say its own type at all.
    //
    // A def^ binds the instance (14.7), the same object self^ is bound to --
    // self^ is one value of that type, Self^ is the type. Links live on the C
    // stack of the resolve/infer that pushed them, like this_link above.
    struct SelfLink {
        LhatType *type;
        struct SelfLink *outer;
    } *self_link;

    // 14.9 with 8.7: the definitions being built and the names they are on
    // their way into. A def^ writing its own name inside itself reached the
    // pending^ seed the collecting pass leaves for every let^, and pending^
    // conforms to everything -- so 'x:T' inside T asked nothing at all, while
    // the same annotation outside asked for the whole structure.
    //
    // Only resolve_type reads this. The *value* is genuinely not assigned yet
    // at that point, which is 8.7's own business and stays as it was; what a
    // name means as a type is settled by the def^ itself (14.7's instance),
    // and that exists from the first line of infer_def onwards.
    struct DefLink {
        const Binding *binding;
        const LhatNode *node;    // the def^ this name is being bound to
        LhatType *instance;      // NULL until infer_def has made it
        struct DefLink *outer;
    } *def_link;

    // 02 の 15.1: f^ may call only f^, never p^. True inside an f^ body
    // (and nested f^ literals within it), false everywhere else -- outside
    // any body (top level) and inside a p^ both allow either kind, so
    // false is the right default with no body open at all.
    bool in_function;

    // 15.1's other half: an f^ assigns to local variables only. This is the
    // scope its parameters were bound in, which is the outermost one its body
    // made -- a name found at or inside it is the body's own, and one found
    // past it belongs to whoever called. NULL outside any body. Saved and
    // restored around a nested one the same way `in_function` is, so an f^
    // written inside another measures against its own.
    Scope *body_scope;

    // 15.13: the same boundary for the innermost closed^ body, which a name
    // found past may not be. NULL where no such body stands around this one
    // -- and it is not restored the way `body_scope` is at every literal:
    // a body written inside a closed^ one is inside it too, so the mark
    // reaches through.
    Scope *closed_scope;

    // 04 の 4.5: the try^{ } being checked, if any. A try^ written inside one
    // hands its errors here rather than to the subroutine's result -- what
    // no arm takes is what goes on out (check_try_block). A subroutine body
    // written inside the block clears it: a try^ there belongs to that body.
    struct CatchFrame {
        LhatType *caught;
        struct CatchFrame *outer;
    } *catch_frame;

    // 02 の 14.12改: what an override^ is writing over, which is what super^
    // names. NULL anywhere else, so 14.12's marker is what makes it a name.
    LhatType *super_type;

    // 03 の 3.4改2: something answered before it was inferred -- a def^ member
    // read before its own body was walked (14.7改2's seed), or a name whose
    // let^ this walk has not reached yet (8.7). Both say the same thing: this
    // walk read ahead, so walking again from what it learned may answer
    // better. Set by the member search and by the name search, read by the
    // two round loops below.
    bool read_provisional;

    // 03 の 3.4改2: this is a second or later walk over the same statements.
    // What the first walk bound is already bound, so the statements that bind
    // (import^) or grow a table (8.8's path) would otherwise report their own
    // first walk as a redefinition.
    bool rewalking;

    // 03 の 3.4改: the signature a subroutine literal is expected to have, or
    // NULL where nothing expects one. Set around the one chk_infer() that
    // reaches the literal, the way super_type and yield_bound_type are, and
    // read by chk_infer_func for the parameters nothing was written on.
    // What it supplies stands exactly where a written annotation would.
    LhatType *expected_func;

    // 03 の 4.3: the statement whose value the input answers with, when the
    // input is one of a session. NULL for a file, where nothing answers.
    const LhatNode *answering;

    // 03 の 4.3 with 05 の 8.9: checking one input of a session. A session's
    // top-level names keep their one-slot places across inputs, which a
    // host value's width does not fit -- so the top level of a prompt
    // refuses one where a unit's top level (one ordinary frame) does not.
    bool session;

    // 05 の 8.6: the type L^ answers. One per check, since 8.8 lets a path
    // add to it and every mention has to see the same table. A session hands
    // its own in, so what one input registers is there for the next.
    LhatType *environment;

    // 02 の 14.16: what every typeof^(...) answers with. Nominal (8.8's
    // reason applies the same way: what it carries is fixed and comparing two
    // by shape would say nothing), so every mention has to be the very same
    // object -- kept here and handed forward by a session the way
    // `environment` is.
    LhatType *typeinfo_type;

    // The name the subroutine currently being checked is being bound to, so
    // that a call to it inside its own body can be spotted (03 の 3.4).
    const char *defining_name;
    size_t defining_length;

    // 15.2: what the yield^/yieldall^ sites seen so far in this body agree
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
    enum YieldContext {
        YIELD_CTX_NONE,
        YIELD_CTX_DISCARD,
        YIELD_CTX_BOUND
    } yield_context;
    LhatType *yield_bound_type;  // YIELD_CTX_BOUND only; NULL means "no annotation"
} Checker;


void chk_report(Checker *c, const LhatNode *at, LhatCheckErrorCode code);
#if LHAT_WITH_RESOLUTIONS
// 07 の 4 章: for the language server, not for the language -- see check.h's
// LhatResolution. Every call site carries this same guard.
void chk_record_resolution(Checker *c, const LhatNode *at, const Binding *b);
void chk_record_narrowed_resolution(Checker *c, const LhatNode *at,
                                    const Binding *b, LhatType *type);
void chk_record_typed_resolution(Checker *c, const LhatNode *at,
                                 LhatType *type);
void chk_settle_resolutions(LhatCheckResult *result);
#endif
void chk_report_named(Checker *c, const LhatNode *at,
                      LhatCheckErrorCode code, const char *name,
                      size_t length);
bool chk_node_name(const Checker *c, const LhatNode *node,
                   const char **text, size_t *length);
bool chk_name_is(const char *text, size_t length, const char *literal);
bool chk_is_super_name(Checker *c, const LhatNode *node);
bool chk_is_discard(const Checker *c, const LhatNode *node);
Binding *chk_scope_find_local(Scope *scope, const char *name, size_t length);
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found);
Binding *chk_scope_find_skipping(Scope *scope, const char *name,
                                 size_t length, size_t skip);
Scope *chk_scope_from(Scope *scope, const LhatNode *node);
Binding *chk_scope_add(Scope *scope, const char *name, size_t length,
                       LhatType *type, uint32_t offset);
void chk_scope_dispose(Scope *scope);
LhatType *chk_simple(Checker *c, LhatTypeKind kind);
int chk_self_marker_at(const Checker *c, const LhatNode *params,
                       const LhatNode *param);
LhatType *chk_resolve_func_type(Checker *c, const LhatNode *node);

// 14.7改 with 14.4: whether an instance carries a member of this type. What
// may be reached through one is what is handed a receiver, which its signature
// says; new, a static member and a value are the definition's alone.
bool chk_takes_receiver(const LhatType *type);

// 04 の 5.3 with 3.4: an error leaving the body being checked, through a try^
// or through the arms of a try^{ } that did not take it.
void chk_error_leaves(Checker *c, const LhatNode *at, LhatType *escaping);

bool chk_may_stand_beside_tuple(const LhatType *type);
bool chk_contains_error(const LhatType *type);

// 13.8改: what every position of a tuple has to satisfy, wherever the tuple
// came from -- 'return^ a, b', 'yield^ a, b', or the literal '(a, b)'. An
// error among the values rather than around them (04 の 8.2), a position
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
void chk_check_operator_shape(Checker *c, const LhatNode *at,
                              const LhatType *type, const char *name,
                              size_t length);
void chk_refuse_self_last(Checker *c, const LhatNode *at,
                          const LhatType *type);
LhatType *chk_builtin_operator(Checker *c, LhatTypeKind carrier,
                               const char *name, size_t length);
LhatType *chk_operator_member(Checker *c, const LhatType *type,
                              const char *name, size_t length);
bool chk_operator_undecided(const LhatType *type);
bool chk_narrowable(const LhatNode *node);
LhatType *chk_narrowed_type(Checker *c, const LhatNode *path);
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
void chk_rounds_begin(Checker *c, Rounds *r, size_t count);
bool chk_rounds_next(Checker *c, Rounds *r);
void chk_rounds_end(Checker *c, Rounds *r);
void chk_expect(Checker *c, const LhatNode *at, LhatType *value,
                LhatType *target, LhatCheckErrorCode code);
LhatType *chk_infer_name(Checker *c, const LhatNode *node);
LhatType *chk_infer_binary(Checker *c, const LhatNode *node);
bool chk_signature_accepts(const LhatType *func, LhatType *const *args,
                           size_t count, bool through_member);
LhatType *chk_infer_call(Checker *c, const LhatNode *node);
LhatType *chk_table_walk_tuple(Checker *c, const LhatType *over);
LhatType *chk_table_element_type(Checker *c, const LhatType *over);
LhatType *chk_without_nil_arm(Checker *c, LhatType *target);
LhatType *chk_infer_member(Checker *c, const LhatNode *node);
void chk_unify_yield(Checker *c, const LhatNode *at, LhatType **slot,
                     LhatType *candidate);
LhatType *chk_infer_func(Checker *c, const LhatNode *node);
const LhatTypeMember *chk_members_search(const LhatTypeMember *members,
                                         const char *name, size_t length);
const LhatTypeMember *chk_find_member(const LhatType *table,
                                      const char *name, size_t length);
LhatType *chk_instance_of(const LhatType *definition);
const LhatTypeMember *chk_unimplemented_member(const LhatType *definition);
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right);
LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base);
bool chk_is_hostvalue(const LhatType *type);
LhatType *chk_infer(Checker *c, const LhatNode *node);
LhatType *chk_environment_type(Checker *c);
LhatType *chk_typeinfo_type(Checker *c);
void chk_check_define(Checker *c, const LhatNode *node);
LhatType *chk_module_root_table(Checker *c);
void chk_register_module_type(Checker *c, const char *module_name,
                              LhatType *exports);
LhatType *chk_hosted_module(Checker *c, const LhatNode *path);
bool chk_value_is_fresh(const Checker *c, const LhatNode *value,
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

#endif  // LHAT_CHECK_INTERNAL_H
