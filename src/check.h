// L^ (lhat) -- the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". This is the third stage of [pipeline.stages.tree]: it walks the tree the
// parser produced, resolves written annotations into types, infers the rest,
// and reports what does not fit.
//
// It never rewrites the tree. [pipeline.repl.no-fork] requires the shape of the tree and
// the meaning at run time to be the same whatever the strictness, so the
// setting only decides diagnostic severity and whether execution may proceed.

#ifndef LHAT_CHECK_H
#define LHAT_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "lhat/error.h"  // [lsp.next]: the fixes a diagnostic carries
#include "lhat/lexer.h"
#include "hosted.h"
#include "type.h"

typedef enum {
    LHAT_CHECK_ERR_NONE,

    LHAT_CHECK_ERR_UNDEFINED,           // no such name in scope
    LHAT_CHECK_ERR_USED_BEFORE_DEFINED, // [syntax.stmt.visible]
    LHAT_CHECK_ERR_REDEFINED,           // [syntax.stmt.visible]: twice in one scope
    LHAT_CHECK_ERR_UNKNOWN_TYPE,        // an annotation names nothing, or
                                        // names a value -- [modules.env.one] gives a
                                        // type to a definition and an
                                        // errordef^, and to nothing else

    LHAT_CHECK_ERR_MISMATCH,            // [syntax.type.fits]: the value does not fit
    LHAT_CHECK_ERR_TEMPLATE_CONTEXT,
    LHAT_CHECK_ERR_UNINITIALIZED_TASK_CAPTURE,
    LHAT_CHECK_ERR_CAPTURE_INITIALIZER,
    LHAT_CHECK_ERR_SHAPE_REFUSED,       // [pipeline.infer.operators]: the body cannot
                                        // take this call's argument types
    LHAT_CHECK_ERR_NOT_NUMBER,          // unary '-' on something else. The
                                        // binary arithmetic goes through
                                        // [syntax.op.semantic]'s question instead ([syntax.op.define])
    LHAT_CHECK_ERR_NOT_BOOL,            // a condition, and^ / or^ / '!'
    LHAT_CHECK_ERR_BAD_STEP,            // step^ must be greater than zero
    LHAT_CHECK_ERR_MATCH_NOT_EXHAUSTIVE, // [syntax.match.other]: an expression match left
                                         // other^ out, and its arms do not
                                         // provably cover the subject (only
                                         // bool^ met both ways can)
    LHAT_CHECK_ERR_NOT_CALLABLE,
    LHAT_CHECK_ERR_CALL_UNION_NARROW,
    LHAT_CHECK_ERR_FUNCTION_CALLS_UNION,
    LHAT_CHECK_ERR_ACCESS_ON_MAYBE_NIL,
    LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE, // [syntax.sub.kinds]: f^ may call only f^;
                                              // this callee is a p^
    LHAT_CHECK_ERR_ARITY,               // too few or too many arguments
                                        // a call, and the two read differently
    LHAT_CHECK_ERR_ARITY_SPREAD,        // the same, with a spread tuple among
                                        // the arguments -- each of its
                                        // positions counts as one
    LHAT_CHECK_ERR_NOT_VARIADIC,        // [syntax.type.variadic]: an 'expr...' that is not a
                                        // tuple continues a variadic tail,
                                        // and this callee has none
    LHAT_CHECK_ERR_SPREAD_NOT_LAST,     // [syntax.type.variadic]: an 'expr...' that is not a
                                        // tuple with arguments after it --
                                        // its length is not known until it
                                        // runs
    LHAT_CHECK_ERR_SPREAD_NOT_SEQUENCE, // [syntax.type.variadic]: '...' of something that is
                                        // neither a tuple nor a table whose
                                        // type says what its sequence holds
    LHAT_CHECK_ERR_NO_MEMBER,           // [syntax.obj.structure]: the structure lacks it
    LHAT_CHECK_ERR_NO_RESULT_TYPE,      // [syntax.type.return-type]: X.ReturnType of a
                                        // signature answering no value ([syntax.type.empty])
    LHAT_CHECK_ERR_KIND_AS_VALUE,       // [errors.kind.made]: a kind of the errordef^
                                        // written where a value was wanted.
                                        // The declaration makes types and no
                                        // values, so [errors.kind.new]'s error^ is the only
                                        // way to one

    LHAT_CHECK_ERR_CANNOT_FAIL,         // [errors.catch.form]: catch^ on what cannot fail
    LHAT_CHECK_ERR_CANNOT_BE_NIL,       // [syntax.op.default], the same for '??'
    LHAT_CHECK_ERR_TRY_OUTSIDE,         // [errors.try.result]
    LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES, // [errors.kind.local-no-propagate]: a localerror^ on its
                                        // way out of the frame that raised it
    LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN, // [errors.kind.local-no-propagate]: localerror^ written where a
                                        // caller would receive it
    LHAT_CHECK_ERR_NEVER_RETURNS,       // [pipeline.infer]: every way out of the body
                                        // goes through the subroutine itself
    LHAT_CHECK_ERR_RESULT_UNDECIDED,    // [pipeline.strict.per-unit]: the body did not decide
                                        // the result type, so every caller
                                        // would be promised a gap
    LHAT_CHECK_ERR_PARAM_UNDECIDED,     // [pipeline.strict.per-unit]: nothing in the body
                                        // demanded anything of this parameter,
                                        // and nothing was written -- so its
                                        // place in the signature is a hole
    LHAT_CHECK_ERR_TYPE_UNDECIDED,      // [pipeline.strict.per-unit]: a gap reached a name
                                        // being bound, so what that name
                                        // holds was never decided
    LHAT_CHECK_ERR_FUNCTION_FALLS_OUT,  // [syntax.type.empty]: an f^ that can reach its end
    LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT, // the value-less exit does not fit the
                                        // result that was written
    LHAT_CHECK_ERR_THIS_OUTSIDE,        // [syntax.sub.this]: this^ where no body is running
    LHAT_CHECK_ERR_SUPER_OUTSIDE,       // [syntax.obj.super]: super^ outside an override^
    LHAT_CHECK_ERR_NOT_DISPOSABLE,      // [syntax.with.checked]: with^ needs a dispose()
    LHAT_CHECK_ERR_DISCARD_READ,        // [syntax.type.discard]: '_^' is not a name, so there
                                        // is nothing there to read back
    LHAT_CHECK_ERR_HAT_NAME_RESERVED,   // [lex.hat.repeat]: a hat name is the
                                        // language's, so a writer declares
                                        // none but tostring^ and iterate^
    LHAT_CHECK_ERR_ERROR_DROPPED,      // [errors.dropped.answer]: the answer carries a
                                        // failure and nothing was done with it

    LHAT_CHECK_ERR_MEMBER_EXISTS,       // [syntax.obj.override]: same name, no marker
    LHAT_CHECK_ERR_FIELD_METHOD_COLLISION, // field and shared method share a name
    LHAT_CHECK_ERR_COMPOSE_COLLIDES,    // [syntax.obj.compose]: and no marker can be written
    LHAT_CHECK_ERR_CONCAT_COLLIDES,     // [syntax.op.concat-table]: both tables carry the key,
                                        // so the concatenation holds no one
                                        // value for it
    LHAT_CHECK_ERR_AMBIGUOUS_MEMBER,    // [syntax.obj.compose-ambiguous]: both sides carry it, so the
                                        // composition reaches no one answer
    LHAT_CHECK_ERR_STILL_ABSTRACT,      // [syntax.obj.abstract]: new on a definition with a
                                        // member nothing has provided yet
    LHAT_CHECK_ERR_FIELD_UNPROVIDED,    // [syntax.obj.abstract-new]: the same for a template
                                        // field, which a written new may also
                                        // give a value to
    LHAT_CHECK_ERR_LENT_DOES_NOT_PROVIDE,  // [syntax.obj.abstract]: new where the declared
                                           // member is one the delegate^ lends
    LHAT_CHECK_ERR_LENT_IS_NOT_REPLACED,   // [syntax.obj.abstract-override]: new where a waiting
                                           // override^ has only a lent member
                                           // under it
    LHAT_CHECK_ERR_ALREADY_PROVIDED,    // [syntax.obj.abstract]: abstract^ over a member that
                                        // something in the chain provides
    LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE, // [syntax.obj.abstract-same]: abstract^ over one this
                                        // same def^ writes a value for
    LHAT_CHECK_ERR_MUTABLE_DEFAULT,     // [syntax.obj.new]: a template initialiser's
                                        // value sits on the prototype and is
                                        // shared by every instance, so it has
                                        // to be immutable -- a mutable one is
                                        // made inside new
    LHAT_CHECK_ERR_NEW_TARGET,         // new^ needs a constructible type
    LHAT_CHECK_ERR_NEW_CONSTRUCTOR,    // default new was overridden
    LHAT_CHECK_ERR_NEW_RETURNS,         // [syntax.obj.new]: construction answers the
                                        // copy, so a new body has no
                                        // return^ of its own
    LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW,  // [syntax.obj.new]: self^{ … } is the
                                        // construction notation, and only a
                                        // written new constructs
    LHAT_CHECK_ERR_PROTOTYPE_SEALED,    // [syntax.obj.new]: the prototype is read
                                        // through self^ and written by no one
    LHAT_CHECK_ERR_MUTABLE_KEY,         // [modules.hostvalue.union]: a key hashes its
                                        // bytes, so only a sealed box
                                        // (constbox^) may be one
    LHAT_CHECK_ERR_BOX_FIELD_WRITE,     // [modules.hostvalue.union]: a box's field is read
                                        // off the bytes; writing goes
                                        // through set()
    LHAT_CHECK_ERR_HOSTVALUE_TEMPORARY_WRITE, // [modules.hostvalue]: a temporary copy
                                             // has no storage to write back to
    LHAT_CHECK_ERR_NOT_BOXABLE,         // [modules.hostvalue]: box^ takes a host value
                                        // -- everything else already lives
                                        // on the heap and needs no box
    LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, // a marker with nothing under it
    LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,   // [syntax.obj.override]: override^ has to fit
    LHAT_CHECK_ERR_OVERLOAD_OVERLAPS,   // [syntax.obj.override]: signatures must stay apart
    LHAT_CHECK_ERR_OPERATOR_UNSETTLED,  // [pipeline.infer.expected]: several types here
                                        // carry it and nothing says which
    LHAT_CHECK_ERR_INCOMPARABLE,        // no value inhabits both sides
    LHAT_CHECK_ERR_NO_IDENTITY,         // value types have no object identity
    LHAT_CHECK_ERR_AS_IMPOSSIBLE,       // [syntax.op.table]: as^ between two types
                                        // no value inhabits both of
    LHAT_CHECK_ERR_BAD_KEY,             // [errors.runtime.index]: nil^ spells absence, so
                                        // it cannot also be a key
    LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR,
    LHAT_CHECK_ERR_TYPE_ARGUMENT_REFERENCE,
    LHAT_CHECK_ERR_TYPE_ATTRIBUTE,
    LHAT_CHECK_ERR_TYPE_SPECIALIZATION,
    LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME,
    LHAT_CHECK_ERR_NO_OPERATOR,         // [syntax.op.semantic]: an operator is answered by
                                        // the left operand, and this one
                                        // carries no answer for it
    LHAT_CHECK_ERR_BAD_OPERATOR,        // [syntax.op.define]: an op^ is an f^ taking self^
                                        // and one argument, and this is not
    LHAT_CHECK_ERR_ISA_ALWAYS_TRUE,     // [syntax.type.variadic]: any^ holds of every value, so
                                        // asking it of one asks nothing
    LHAT_CHECK_ERR_MISSING_FIELD,       // [errors.kind.new]: no default to fall back to
    LHAT_CHECK_ERR_REQUIRE_FAILED,      // [modules.deps]: the unit could not be had
    LHAT_CHECK_ERR_COROUTINE_DROPPED,   // [syntax.sub.await]: a call that makes a coroutine
                                        // and does nothing with it
    LHAT_CHECK_ERR_NOT_COROUTINE,       // [syntax.for.forms]: in^ walks whatever
                                        // iterate() answers with, and this
                                        // is what it says when that is not
                                        // a coroutine
    LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE, // [syntax.sub.await] with [syntax.sub.await-done]: what is awaited
                                        // is something that finishes, and
                                        // only a coroutine does -- which is
                                        // also the whole of what delegating
                                        // asks of it
    LHAT_CHECK_ERR_YIELD_AT_TOP,        // [syntax.sub.yieldable] with [modules.unit.optional]: only a
                                        // script's top level ('p^...') may
                                        // suspend; a module^ unit's and a
                                        // session's have nobody to resume
                                        // them
    LHAT_CHECK_ERR_COROUTINE_MISMATCH,  // [syntax.type.coroutine] with [syntax.sub.yieldable]: a c^{ … } written in
                                        // the result position says what a call
                                        // answers, and the yield^ sites say it
                                        // too. The two disagreed
    LHAT_CHECK_ERR_YIELDABLE_RESULT,    // [syntax.type.coroutine-result]: a yielding body's
                                        // call answers a coroutine, so its
                                        // written result has to be a c^{ … }

    LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION, // [syntax.sub.yieldable]: a yield^ that is bound
                                            // (directly, by a let^) needs a
                                            // written type there to fix R
    LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH,    // [syntax.sub.yieldable]: two yield^/await^ sites
    LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS,    // [syntax.sub.phantom-yield]: a _yield^ never runs, so
                                           // a name cannot take its answer --
                                           // only _^ stands on the left
    LHAT_CHECK_ERR_NOT_INDEXABLE,          // [errors.runtime.index]: only a table holds
                                           // keys -- a string^'s characters
                                           // answer at(i) instead
                                            // in one body disagree on Y or R

    LHAT_CHECK_ERR_PATH_NOT_TABLE,      // [modules.hostdata]: everything before the last
                                        // segment holds the one after it, so
                                        // it has to be a table
    LHAT_CHECK_ERR_PATH_IS_DEFINITION,  // [modules.hostdata]: [syntax.obj] fixes what an instance
                                        // of a def^ carries, so a member
                                        // cannot be added to one
    LHAT_CHECK_ERR_MODULE_UNNAMED,      // [modules.require.short]: the short form binds a
                                        // unit under the path it declared,
                                        // and this one declared none ([binary.codec.kinds])
    LHAT_CHECK_ERR_NOT_HOSTED,          // [modules.host.import]: import^ reaches what the
                                        // host registered, and nothing here
    LHAT_CHECK_ERR_SCOPE_TOO_FAR,       // [lex.scope]: '$^' naming more scopes
                                        // out than are open here
    LHAT_CHECK_ERR_SCOPE_ON_DEFINE,     // [lex.scope] with [modules.host.import]: a let^ makes a
                                        // name where it is written, so there
                                        // is no other scope for one to name
    LHAT_CHECK_ERR_FUNCTION_WRITES_OUT,  // [syntax.sub.kinds]: an f^ assigns to local
                                         // variables only, and this name was
                                         // bound outside its body
    LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE, // [syntax.sub.table-write]: the same rule for a table
                                           // -- this one is not one the body
                                           // made, so changing it is
                                           // observable from outside
    LHAT_CHECK_ERR_OWN_LOWERED,         // [syntax.sub.table-write]: this puts something the
                                        // body did not make where the body
                                        // changes things, written elsewhere
    LHAT_CHECK_ERR_FUNCTION_CLEANUP,    // [syntax.finally.not-in-f]: an f^ has
                                        // nothing to clean up, so finally^ and
                                        // with^ are a p^'s
    LHAT_CHECK_ERR_FUNCTION_REQUIRES,   // [modules.require.once]: require^ runs the unit,
                                        // a body of its own, which an f^ may
                                        // not have run on its behalf
    LHAT_CHECK_ERR_BAD_TOSTRING,        // [syntax.obj.tostring]: a member under tostring is
                                        // f^self^ -> string^, since a hole
                                        // calls it with no call written
    LHAT_CHECK_ERR_PATH_IS_OPAQUE,      // [modules.hostdata]: a host type carries what
                                        // the host registered, and [syntax.obj.typeof]'s
                                        // type info what made it; neither is
                                        // written over from L^
    LHAT_CHECK_ERR_ADVANCES_OUTSIDE,    // [syntax.sub.yieldable-f]: an f^ advancing a coroutine
                                        // its own body did not make, whose
                                        // progress the caller can see
    LHAT_CHECK_ERR_MUTATES_OUTSIDE,     // [syntax.sub.mutable-self]: an f^ calling a
                                        // mutable^self^ method on a table
                                        // its own body did not make
    LHAT_CHECK_ERR_ANSWER_NOT_FRESH,    // [syntax.sub.fresh]: a '-> fresh^T' body
                                        // returning something it did not
                                        // make on the spot
    LHAT_CHECK_ERR_COROUTINE_ESCAPES,   // [syntax.sub.yieldable-f]: an f^ coroutine reaching the
                                        // outside, where advancing it is what
                                        // makes the call observable
    LHAT_CHECK_ERR_TABLE_IS_SEALED,     // [modules.host.machine]: L^, its registry
                                        // and what require^/import^ answers
                                        // are the machine's own; the host
                                        // writes them through its API
    LHAT_CHECK_ERR_ASSIGN_TO_LET,       // [syntax.stmt.let]: the name was bound by a let^,
                                        // which a ':=' may not reach. Writing
                                        // var^ instead is open here
    LHAT_CHECK_ERR_ASSIGN_TO_FORM,      // [syntax.stmt.let] with [syntax.with.form] and [syntax.for.from]: bound by
                                        // the construct rather than a word --
                                        // a with^, or a for^ focus -- so
                                        // there is no var^ to offer
    LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE, // [modules.public] with [syntax.stmt.let]: a public^ var^
                                        // is a name another unit could see
                                        // change, which [lex.scope.no-globals] refused
    LHAT_CHECK_ERR_COMPARE_NOT_NUMBER,  // [syntax.op.compare]: an op^<=> answers a number^,
                                        // which is what the orderings read
    LHAT_CHECK_ERR_EQUAL_NOT_BOOL,      // [syntax.op.equal]: an op^= answers a bool^,
                                        // which '=' takes as it stands and
                                        // '≠' negates
    LHAT_CHECK_ERR_NOT_ORDERED,         // [syntax.op.compare]: an ordering is read off '<=>',
                                        // and neither side carries one taking
                                        // the other
    LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR, // [syntax.op.right]: a self^ written last says
                                           // the right operand is the
                                           // receiver, which only an op^ has
                                           // any use for ([syntax.obj.self])
    LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE,   // [syntax.type.self]: Self^ names the type literal
                                        // around it, and here there is none --
                                        // no t^ or def^ encloses it, or a
                                        // second hat counted past the
                                        // outermost one
    LHAT_CHECK_ERR_TUPLE_MISPLACED,     // [syntax.type.tuple]: a tuple is written in a
                                        // result and nowhere else -- not
                                        // bound to a name, passed as an
                                        // argument, held in a table, or
                                        // nested in another tuple. Confining
                                        // it there is what keeps 13.8's four
                                        // propagations from coming back. Use
                                        // pack^ to make a table of it
    LHAT_CHECK_ERR_TUPLE_UNION,         // [syntax.type.tuple] with [errors.fallible.union]: the only
                                        // thing a tuple may be written in a
                                        // union with is an error
    LHAT_CHECK_ERR_HOSTVALUE_UNION,     // [modules.hostvalue.union]: a host value takes
                                        // several slots, so what stands
                                        // beside it in a union has to be
                                        // told apart by the head slot --
                                        // the family [syntax.type.tuple] gives a tuple
    LHAT_CHECK_ERR_TUPLE_ARITY,         // [syntax.type.tuple]: the names taking the values
                                        // apart and the values answered are
                                        // not the same count. A tuple has exactly its
                                        // positions, since each one is a slot
                                        // the caller reserved -- [syntax.obj.structure]'s width
                                        // subtyping never applies here
    LHAT_CHECK_ERR_TUPLE_ERROR_POSITION,// [syntax.type.tuple] with [errors.dropped.around-value]: the error
                                        // goes around the values, never among
                                        // them. '(number^, IOError)' is the
                                        // shape Go's 'v, err := f()' needs --
                                        // one value to use and one to drop --
                                        // and [modules.host.initial] kept it unwritable by
                                        // having no second value at all.
                                        // Multi-value return brings the
                                        // second value back, so the rule has
                                        // to be said out loud instead. Write
                                        // '(A, B)|IOError'
    LHAT_CHECK_ERR_CATCHES_NOTHING,     // [errors.catch.clause]: arms after statements
                                        // that write no try^ have nothing to
                                        // catch, the same as [binary.net.api]'s catch^ on
                                        // an expression that cannot fail
    LHAT_CHECK_ERR_ARM_NEVER_RUNS,      // [errors.catch.clause]: nothing the try^
                                        // before this arm can find is left for
                                        // it to take
    LHAT_CHECK_ERR_HOSTVALUE_ESCAPES,   // [modules.hostvalue]: a host value lives in
                                        // stack slots and nowhere else, so a
                                        // table, a capture, an any^, a
                                        // tuple's position and the program's
                                        // answer all refuse one. [modules.hostvalue.union]: what
                                        // each of them refuses is the width,
                                        // so a union carrying one is refused
                                        // there too -- while the union
                                        // itself is written freely, beside
                                        // the arms the head slot tells apart
                                        // one -- box it into the hostdata
                                        // container its library provides.
                                        // Refused even under relaxed: a
                                        // runtime check cannot make room
                                        // where the representation has none
    LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL,  // [errors.runtime.index]: what a key reads out
                                        // of a table is 'T|nil^', and no
                                        // operator is on the nil^ side.
                                        // [syntax.op.right]'s rule sends the writer
                                        // looking for an operator to write
                                        // when what is missing is a narrowing
                                        // -- and [syntax.type.fits] does not narrow an
                                        // index where it stands
    // [syntax.annot.host]: an annotation the host did not register, one written where
    // its registration does not allow, and arguments that do not fit what it
    // declared. Three failures rather than one: what to change differs.
    LHAT_CHECK_ERR_NO_SUCH_ANNOTATION,
    LHAT_CHECK_ERR_ANNOTATION_MISPLACED,
    LHAT_CHECK_ERR_ANNOTATION_ARGUMENTS,
    LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE,// [syntax.annot.host.two-answers]: registered as one of two
                                        // answers to one question, and the
                                        // other is in this file already
    LHAT_CHECK_ERR_ANNOTATION_REQUISITE,// [syntax.annot.host.alone]: registered as half of
                                        // something, and the other half is
                                        // not on this declaration
    LHAT_CHECK_ERR_ANNOTATION_REPEATED, // [syntax.annot.host]: registered FILEUNIQUE, and
                                        // the file already had one
    LHAT_CHECK_ERR_BARE_TABLE_TYPE      // [syntax.obj.structure]: t^ is written with the
                                        // members it asks for, and the top of
                                        // tables is the one asking for none
                                        // -- 't^{}'. Bare, the '{' that
                                        // follows a signature would be read
                                        // as the member list rather than as
                                        // the body
} LhatCheckErrorCode;

typedef struct LhatCheckDiagnostic {
    LhatCheckErrorCode code;
    // A concrete-body failure is reported at its call, with the original
    // diagnostic retained in the semantic arena for all frontends.
    const struct LhatCheckDiagnostic *cause;
    const char *cause_path;
    bool relaxed_ok;  // This diagnostic is advisory under relaxed policy.
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // The name the diagnostic is about, for the codes that are about one --
    // "no such name in scope" is worth more when it says which. Borrowed from
    // the source the unit was read from, which outlives the result, and NULL
    // for the codes that name nothing.
    const char *name;
    uint32_t name_length;

    // Explicit source span when the mentioned name is not written here.
    // Zero retains the usual name_length span (or a single character).
    uint32_t highlight_length;

    // Frozen descriptions of the two types the message compares -- an
    // operator's operands, or a value and the type where it is written --
    // owned by the semantic arena.
    const char *left_type;
    const char *right_type;

    // [lsp.next]: the fixes this diagnostic knows how to make, read through
    // lhat_fix_slot_read. The stage that refused is the one that knows what
    // would have been right, so they are worked out here and not in a tool.
    LhatFixSlot fixes[LHAT_FIX_SLOTS];
} LhatCheckDiagnostic;

#if LHAT_WITH_RESOLUTIONS
// [lsp.hover]: where a name that was written got its meaning.
//
// Recorded as each use is resolved, so that a tool answering "what is this"
// reads what the checker decided rather than resolving names a second time.
// A second implementation of [lton.host]'s scoping would be one more thing to keep
// in step, and it would disagree in exactly the cases that are hard.
//
// **Nothing in the language reads this.** [pipeline.stages.tree]'s later stages compile
// and run the same tree whether it was recorded or not; it is here for the
// language server, and LHAT_WITH_RESOLUTIONS takes it out for a host that
// embeds the language without any tooling.
typedef struct {
    uint32_t use;      // where the name stands, as written
    uint32_t use_end;  // one past it, so a position within the name matches
    uint32_t definition;  // where the name it reaches was bound
    // False when there is no such place to point at: a member is looked up
    // in a type rather than in a scope ([syntax.obj.structure]), and the type may have come
    // from another unit or from a host registration, where nothing in this
    // source declares it. The type is still known, which is what a reader
    // asking "what is this" wants most.
    bool has_definition;
    // Which unit `definition` is an offset into: NULL for the one this
    // resolution belongs to, and a borrowed path (the member's declared_in,
    // type.h) for a name whose meaning was written somewhere else --
    // [modules.deps.static] publishes a type, and a member of it was written in the unit that
    // published it. Two more meanings are found the same way and recorded
    // the same way: [errors.kind.errordef]'s error kind and [syntax.enum]'s enum member carry
    // their written place on the type (type.h's v.error), and a require^'s
    // path names a whole unit, whose `definition` is 0 -- its start.
    const char *definition_path;
    // [syntax.obj.substring] and [syntax.obj.tostring-hat] with [syntax.sub.coroutine-members]: the language answered this member
    // itself. Nothing wrote it and no host registered it -- `length` on a
    // string, `resume` on a coroutine, `message` on an error -- so a reader
    // asking where it is gets nothing, and an editor drawing it wants to
    // draw it as the library's rather than as the program's.
    bool builtin;
    // [syntax.type.signature]: declared by a signature rather than bound by a let^ or var^.
    // Where a use stands says only that there is a name, and the type says
    // only what it holds -- which of the two declared it is something only
    // the checker knows.
    bool is_parameter;
    // [syntax.stmt.let]: bound by a let^ (or by a form that binds like one -- [syntax.with]'s
    // with^, [syntax.for.forms]'s walking focus), so nothing written after it reassigns
    // the name.
    bool immutable;
    // What the name holds there. Belongs to the result's type arena, so it
    // is valid for as long as the result is.
    LhatType *type;
    // [syntax.enum]: the member actually named, even when its value has the
    // wide enum type. Also records [errors.kind.errordef]'s named kind.
    const LhatType *named_kind;
    // The signature instantiated for this use as a callee, if any. Kept
    // separate from the value's type so declaration hovers stay general.
    const LhatType *call_signature;
} LhatResolution;

// [lsp.hover.try]: one place a try^ sends errors. Kept apart from LhatResolution
// because a try^ is no name, and may have several answers: what one arm does not
// take goes on to the next arm, the arms outside, or out of the body.
typedef enum {
    LHAT_TRY_TO_ARM,   // the catch^ arm `target`
    LHAT_TRY_TO_BODY,  // the result of the subroutine `target`
    LHAT_TRY_TO_UNIT,  // out of the unit, to what ran it; `target` is NULL
} LhatTryRouteKind;

typedef struct {
    uint32_t at;  // where the word try^ stands
    LhatTryRouteKind kind;
    const LhatNode *target;
    // What goes there, from the result's type arena. NULL when the try^ is over the
    // body's own call, whose result was still being inferred.
    const LhatType *errors;
} LhatTryRoute;

// [lsp.hover]: a place a member name may stand -- the '.' of an access, with
// the type of whatever stands to its left.
//
// Kept apart from LhatResolution because a resolution is keyed by a name's
// span, and this is wanted exactly when there is no name: the parser leaves
// a member with nothing under it for a dot at the end of what has been typed
// so far (parser.c), and a tool asked what may stand there needs the
// receiver rather than the answer. Working the receiver out from the names
// around it would be a second reading of [syntax.obj.structure]'s lookup, which is the thing
// this file exists to avoid.
typedef struct {
    uint32_t dot;  // where the '.' or '?.' stands: the member node's own
                   // offset, which is the operator rather than the whole
    uint32_t end;  // one past the member name, or one past the dot when
                   // nothing was written -- so a cursor inside a half-typed
                   // name finds the site the same way
    bool nil_safe;  // [syntax.op.present]: written '?.'
    // [syntax.obj.number-consts]: the receiver was written as the WORD number^, which is
    // no value of its own -- what stands after the dot there is one of the
    // constants and nothing else. `receiver` is number^ either way, so this
    // is the only thing that tells the two apart.
    bool number_word;
    // What stands to the left, as the checker settled it, with the nil^ arm
    // set aside where the lookup sets it aside -- so what a tool offers and
    // what a member access would accept are the one answer. Belongs to the
    // result's type arena, so it is valid for as long as the result is.
    LhatType *receiver;
} LhatMemberSite;

// [lsp.hover]: a name a scope held, and the source that scope covered.
//
// A resolution says where a name that was written got its meaning, which
// answers "what is this" and nothing else. This answers the other question a
// tool asks -- "what may I write here" -- for which no name has been written
// yet. Reading [lton.host] a second time to work it out is what this file exists to
// avoid, so the scope says what it held as it is disposed of.
//
// [syntax.stmt.visible] makes a name visible throughout the scope that binds it, so the span
// is the scope's own and not the part of it after the let^.
typedef struct {
    uint32_t from;  // where the construct the scope belongs to begins
    uint32_t to;    // one past its end
    // Borrowed from the source or from the lexer's string storage, both of
    // which outlive the result -- the same loan a Binding takes.
    const char *name;
    uint32_t name_length;
    // What the name holds. Belongs to the result's type arena, so it is
    // valid for as long as the result is. NULL where the scope closed before
    // anything settled it.
    LhatType *type;
    bool is_parameter;  // [syntax.type.signature]: declared by a signature rather than bound
    bool immutable;     // [syntax.stmt.let]: a let^ rather than a var^
    bool names_type;    // [syntax.type.as-expr]: the name stands for a type in annotations
    LhatType *named_type; // Meaning in type position (aliases differ from values).
} LhatBindingSite;
#endif  // LHAT_WITH_RESOLUTIONS

// [modules.host.import]: a host registers what it provides by writing the type out, so
// there has to be a way from written text to a type without a unit around
// it. `named` stands for the names a type may mention -- a table type whose
// members are what the host has registered so far, so that one registration
// can name what an earlier one made. NULL admits only the builtins.
//
// Answers NULL when the text is not a type, or names something `named` does
// not hold. The type belongs to `arena`.
//
// `self` is what [syntax.type.self]'s Self^ names in the text -- the type a member is
// being registered on ([modules.host.import]), so 'f^self^, Self^ -> Self^;' reads as
// the long spelling would. NULL leaves Self^ an error, as outside any
// structure.
LhatType *lhat_type_of_text(const char *text, size_t length,
                            LhatTypeArena *arena, LhatType *named,
                            LhatType *self);

// [modules.deps]: a unit's exports are types that the units requiring it hold on
// to, so the arena has to outlive any one result. The caller may pass its own
// and share it across a whole program; without one, the result keeps its own.
typedef struct {
    LhatTypeArena *types;
    LhatTypeArena owned;
    const LhatNode *unit;  // Set only after the semantic pass completes.
    bool strict;  // Diagnostic policy only; never changes inference.

    LhatCheckDiagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;

#if LHAT_WITH_RESOLUTIONS
    // Every name the walk resolved, ordered by `use` so that a lookup by
    // position can halve its way in rather than scan. The walk very nearly
    // gives that order for free, but not quite -- see chk_settle_resolutions
    // (check.c), which is what makes it hold.
    LhatResolution *resolutions;
    size_t resolution_count;
    size_t resolution_capacity;

    // Every '.' the walk met, ordered by `dot` for the same reason and by
    // the same settling.
    LhatMemberSite *member_sites;
    size_t member_site_count;
    size_t member_site_capacity;

    // Where every try^ sends what it finds, one record per place it goes. A try^
    // that was walked more than once keeps one record per place.
    LhatTryRoute *try_routes;
    size_t try_route_count;
    size_t try_route_capacity;

    // Every name every scope held, in the order the scopes closed -- which
    // is innermost first, since a scope is disposed of before the one around
    // it. lhat_check_bindings_at leans on that and nothing else, so no
    // settling is needed here.
    LhatBindingSite *binding_sites;
    size_t binding_site_count;
    size_t binding_site_capacity;
#endif

    // [modules.public]: the structure of what this unit publishes, or NULL when it
    // publishes nothing. What a require^ of it yields.
    LhatType *exports;

    // [modules.unit]: the path module^ declared, written out with its dots and
    // owned by the result. NULL when the unit declared none, which [modules.unit.optional]
    // allows. [modules.require.short] needs it to say what the short form of require^ binds.
    char *module_name;

    // [lsp.next]: the text of the fixes that write something the source does not
    // hold -- a name found near the one written. Each is its own allocation
    // on this chain, so what a diagnostic points at stays where it is however
    // many more are kept, and a diagnostic a later round took back leaves
    // its text here until the result goes.
    struct LhatFixText *fix_texts;
} LhatCheckResult;

size_t lhat_check_error_count(const LhatCheckResult *result);

#if LHAT_WITH_RESOLUTIONS
// The resolution covering `offset`, or NULL when no name was written there.
// `offset` may fall anywhere within the name, not only on its first byte.
const LhatResolution *lhat_check_resolution_at(const LhatCheckResult *result,
                                               uint32_t offset);

// [lsp.hover.try]: the next place the try^ at `offset` sends an error, or NULL when
// there are no more. `cursor` starts at 0 and carries the walk.
const LhatTryRoute *lhat_check_try_route_at(const LhatCheckResult *result,
                                            uint32_t offset, size_t *cursor);

// The member access whose '.' stands before `offset`, or NULL when none
// does. Open at the bottom and closed at the top: a cursor on the dot itself
// is still before it and answers nothing, and one just past it -- which is
// where it stands when the dot has only now been typed -- answers this one.
// For 'a.b.c' the two sites meet without overlapping, so a cursor between
// 'b' and the second dot finds the access that answered b and no other.
const LhatMemberSite *lhat_check_member_site_at(const LhatCheckResult *result,
                                                uint32_t offset);

#endif  // LHAT_WITH_RESOLUTIONS

// [lsp.hover] and [lsp.next]: the members `receiver` holds as written -- every one
// a lookup answers with, and no other. The search a member access uses is
// what decides, so a member a nearer type shadows and one a delegate does
// not lend ([syntax.obj.delegate]) are left out; so are [syntax.obj.compose-ambiguous]'s ambiguous ones, which
// reaching is refused, and [syntax.obj.structure-repeat]'s positions, which no name spells. What a
// tool offers after a '.' and what a fix offers instead of a misspelling
// are the one list.
typedef void (*LhatMemberSink)(void *context, const LhatTypeMember *member);
void lhat_check_written_members(const LhatType *receiver, LhatMemberSink sink,
                                void *context);

// [lsp.hover] with [syntax.obj.substring]: the built-in members `receiver` answers -- `length`
// on a string, `push^` on a plain table, `resume` on a coroutine. No member
// list holds them; they are the checker's own answers, and this asks the
// checker for each of them rather than describing them a second time.
//
// The sink gets the spelling that answered (bare or hatted -- [lex.hat.repeat] makes
// those different names) and the type the access would have. Written members
// are NOT included: those are the type's own, and
// lhat_check_written_members above is what lists them.
//
// Types are made in `result`'s arena as the walk would make them, so they
// live as long as the result does -- and the arena grows by one signature per
// member offered. Nothing is reported and nothing is recorded: the question
// is asked with no place for a diagnostic to stand.
typedef void (*LhatBuiltinSink)(void *context, const char *name, size_t length,
                                LhatType *type);
void lhat_check_builtin_members(LhatCheckResult *result, LhatType *receiver,
                                LhatBuiltinSink sink, void *context);

// [syntax.obj.number-consts]: the constants number^ carries, for a site written as the
// word (LhatMemberSite's number_word). Nothing else stands there, so this is
// the whole answer rather than an addition to the one above.
void lhat_check_number_constants(LhatCheckResult *result,
                                 LhatBuiltinSink sink, void *context);

#if LHAT_WITH_RESOLUTIONS

typedef void (*LhatBindingSink)(void *context, const LhatBindingSite *site);

// Every name in scope at `offset`, innermost first: a name that shadows
// another is passed on before the one it shadows, so a reader keeping the
// first of each name is left with what [lton.host]'s lookup would have found.
//
// The scopes a host or a session opened are not among them -- those cover no
// source and so contain no position. What a tool wants beside these is the
// names the language itself carries, which no scope holds either ([modules.host.machine]).
void lhat_check_bindings_at(const LhatCheckResult *result, uint32_t offset,
                            LhatBindingSink sink, void *context);

#endif

// [modules.require]. Asked for the unit at `path`, relative to whatever the resolver
// considers the requiring unit. Returns its export structure, or NULL when it
// could not be had -- a missing file or a cycle ([l10n.catalog.dump]), which the resolver
// reports in its own terms.
//
// `module_name` is filled with the path [spec.member-cache] had that unit declare, or left
// NULL when it declared none ([syntax.repl.steps]). [modules.require.short] binds the short form under it, so
// the text has to outlive the call -- the unit owns it.
//
// `unit_path` is filled with the path the resolver holds the unit under, or
// left NULL when it has none to give. [lsp.hover] records it against the
// written path, so that a reader standing on `"lib.lh"` can be sent to the
// file; it too has to outlive the call, and the unit owns it likewise.
typedef LhatType *(*LhatRequireResolver)(void *context, const char *path,
                                         size_t length,
                                         const char **module_name,
                                         const char **unit_path);

typedef struct {
    LhatRequireResolver resolve;
    void *context;
    // [modules.host.import]: what the host registered, as one nested table type keyed by
    // module path. import^ resolves against this alone, so its answer does
    // not depend on the order units happen to be checked in.
    LhatType *hosted;

    // [modules.host.machine]: what the host put in L^ itself, as a table type whose members
    // are those. NULL when it put nothing there. What [modules.host.machine] lists is carried by
    // the checker regardless; this is what a host added on top.
    LhatType *globals;

    // [modules.host.initial]: the names the host bound before anything ran, each to a
    // member of L^ ([modules.host.machine]). A name no scope holds is looked up here before it
    // is reported missing, and its type is the member's.
    //
    // [modules.host.nothing] is unchanged by this. The language hands out no names; a host does,
    // and one that binds none leaves the program seeing nothing. What a name
    // reaches stays readable as L^.<member> whatever a let^ later shadows.
    //
    // Two arrays rather than one of pairs, so that neither this header nor
    // vm.h has to know a type the other declares.
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // [syntax.annot.host]: the annotations the host registered, which are the only
    // ones a program may write. NULL/0 when it registered none, and then
    // every annotation written is one the checker refuses.
    const LhatAnnotationDecl *annotations;
    size_t annotation_count;
} LhatRequire;

// [pipeline.strict.per-unit]. `strict` is a setting of the compilation unit, not a dialect:
// [pipeline.strict.same-source] keeps the source text identical either way, and [pipeline.strict.relaxed] limits what
// relaxed defers to places where inference could not decide.
//
// `lexer` has to be the one the tree was parsed from: names are spans into
// its source and strings are spans into its decoded storage.
void lhat_check(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                LhatCheckResult *result);

// The same, for a unit that is part of a program. `arena` is shared so the
// types this unit publishes stay valid in the units that require it, and
// `require` is how those imports are answered. Either may be NULL.
void lhat_check_unit(const LhatNode *unit, const LhatLexer *lexer,
                     bool strict, LhatTypeArena *arena,
                     const LhatRequire *require, LhatCheckResult *result);

void lhat_check_result_dispose(LhatCheckResult *result);

// [pipeline.machine]: a REPL checks many inputs as one running program, so a name one
// input bound has to still have its type in the next. The session carries
// them, along with the arena their types live in -- the types outlive any one
// input, and so must the names, which are copied out of the source each was
// read from.
typedef struct LhatCheckSession LhatCheckSession;

LhatCheckSession *lhat_check_session_new(void);
// A debugger binding whose static type is unavailable.
// Returns a session-owned declaration identity for the copied frame binding.
const LhatNode *lhat_check_session_seed(LhatCheckSession *session, const char *name,
                             size_t length);

// Frees the session, the names it copied and every type in its arena. Nothing
// read out of a result checked in it stays valid.
void lhat_check_session_dispose(LhatCheckSession *session);

// [modules.host.initial]: the names the host bound to members of L^. The arrays belong to
// the caller and have to outlive the session; setting them again replaces
// what was there.
void lhat_check_session_bind(LhatCheckSession *session,
                             const char *const *names,
                             const char *const *members, size_t count);

// [modules.host.import]: the host registry a prompt's import^ resolves against, and [modules.host.machine]'s
// L^ members alongside it -- the LhatRequire fields a file gets from its
// LhatProgram. A prompt has no program of its own, so a host that wants
// import^ to answer at a prompt makes one, registers into it, and hands the
// two over. prompt.c's install_checks is that call written out.
//
// Both types belong to whatever arena made them, so that has to outlive the
// session -- a program registered into keeps its own and is the usual answer.
// `globals` may be NULL to leave what is there alone; anything else replaces
// it.
void lhat_check_session_hosted(LhatCheckSession *session, LhatType *hosted,
                               LhatType *globals);

// Checks `unit` as the next input of `session`. The names already bound in it
// are in scope, and the ones this input binds stay for the next. The result
// borrows the session's arena, so it has none of its own to dispose.
void lhat_check_next(LhatCheckSession *session, const LhatNode *unit,
                     const LhatLexer *lexer, bool strict,
                     LhatCheckResult *result);

// [syntax.obj.abstract] with [syntax.obj.new]: the first member of `type` that nothing has provided --
// declared by an abstract^, or left waiting by [syntax.obj.abstract-override]'s override^ -- or NULL
// when every one is filled. A definition still holding one cannot be
// instantiated, which is what makes it worth saying out loud: it is the
// difference between a type to compose onto and a type to make one of, and
// nothing in the written form says which.
//
// The rule is the checker's (this is the very predicate [syntax.obj.new] refuses new
// with), so it is asked here rather than read off the type a second time.
const LhatTypeMember *lhat_check_unimplemented_member(const LhatType *type);

// The text of a code, holes unfilled: a code only ever reported with a name
// has the hole for it here ([l10n.compose.holes]).
const char *lhat_check_error_message(LhatCheckErrorCode code);
// [l10n.id]: the stable ID of the same message; NULL for a code the table
// does not hold.
const char *lhat_check_error_id(LhatCheckErrorCode code);

// [l10n.choose.program]: a message is written in the language of the program it is about.
// lhat/program.h is where that is declared; naming it here asks for nothing
// else.
struct LhatProgram;

// The message for one diagnostic, which for the codes that are about a name
// says which -- "no such name in scope: nowhere". The name goes into the
// text's hole; a code reported both with a name and without one has a second
// text for the first, under an ID of its own.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it.
size_t lhat_check_message_write(const struct LhatProgram *program,
                                const LhatCheckDiagnostic *diagnostic,
                                char *out, size_t capacity);
// [l10n.id]: the ID of the text that message is drawn from; NULL for a code no
// table holds.
const char *lhat_check_message_id(const LhatCheckDiagnostic *diagnostic);

#endif  // LHAT_CHECK_H
