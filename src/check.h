// L^ (lhat) -- the type checking stage.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "03" or "04". This is the third stage of 03 の 1.1: it walks the tree the
// parser produced, resolves written annotations into types, infers the rest,
// and reports what does not fit.
//
// It never rewrites the tree. 03 の 4.2 requires the shape of the tree and
// the meaning at run time to be the same whatever the strictness, so the
// setting only decides which failures are reported here rather than deferred
// to a runtime check.

#ifndef LHAT_CHECK_H
#define LHAT_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "lhat/lexer.h"
#include "hosted.h"
#include "type.h"

typedef enum {
    LHAT_CHECK_ERR_NONE,

    LHAT_CHECK_ERR_UNDEFINED,           // no such name in scope
    LHAT_CHECK_ERR_USED_BEFORE_DEFINED, // 8.7
    LHAT_CHECK_ERR_REDEFINED,           // 8.7: twice in one scope
    LHAT_CHECK_ERR_UNKNOWN_TYPE,        // an annotation names nothing, or
                                        // names a value -- 05 の 2.2 gives a
                                        // type to a definition and an
                                        // errordef^, and to nothing else

    LHAT_CHECK_ERR_MISMATCH,            // 13.11: the value does not fit
    LHAT_CHECK_ERR_NOT_NUMBER,          // unary '-' on something else. The
                                        // binary arithmetic goes through
                                        // 11.3's question instead (11.8)
    LHAT_CHECK_ERR_NOT_BOOL,            // a condition, and^ / or^ / '!'
    LHAT_CHECK_ERR_NOT_CALLABLE,
    LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE, // 15.1: f^ may call only f^;
                                              // this callee is a p^
    LHAT_CHECK_ERR_ARITY,               // too few or too many arguments
                                        // a call, and the two read differently
    LHAT_CHECK_ERR_NOT_VARIADIC,        // 13.7: 'expr...' spreads into a
                                        // variadic tail, and this callee has
                                        // none
    LHAT_CHECK_ERR_NO_MEMBER,           // 14.10: the structure lacks it
    LHAT_CHECK_ERR_KIND_AS_VALUE,       // 04 の 2.3: a kind of the errordef^
                                        // written where a value was wanted.
                                        // The declaration makes types and no
                                        // values, so 2.5's error^ is the only
                                        // way to one

    LHAT_CHECK_ERR_CANNOT_FAIL,         // 04 の 4.1: catch^ on what cannot fail
    LHAT_CHECK_ERR_CANNOT_BE_NIL,       // 11.7, the same for '??'
    LHAT_CHECK_ERR_TRY_OUTSIDE,         // 04 の 5.3
    LHAT_CHECK_ERR_NEVER_RETURNS,       // 03 の 3.4: every way out of the body
                                        // goes through the subroutine itself
    LHAT_CHECK_ERR_RESULT_UNDECIDED,    // 03 の 3.1: the body did not decide
                                        // the result type, so every caller
                                        // would be promised a gap
    LHAT_CHECK_ERR_PARAM_UNDECIDED,     // 03 の 3.1: nothing in the body
                                        // demanded anything of this parameter,
                                        // and nothing was written -- so its
                                        // place in the signature is a hole
    LHAT_CHECK_ERR_TYPE_UNDECIDED,      // 03 の 3.1: a gap reached a name
                                        // being bound, so what that name
                                        // holds was never decided
    LHAT_CHECK_ERR_FUNCTION_FALLS_OUT,  // 13.2: an f^ that can reach its end
    LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT, // the value-less exit does not fit the
                                        // result that was written
    LHAT_CHECK_ERR_THIS_OUTSIDE,        // 15.10: this^ where no body is running
    LHAT_CHECK_ERR_SUPER_OUTSIDE,       // 14.12改: super^ outside an override^
    LHAT_CHECK_ERR_NOT_DISPOSABLE,      // 12.5: with^ needs a dispose()
    LHAT_CHECK_ERR_DISCARD_READ,        // 13.12: '_^' is not a name, so there
                                        // is nothing there to read back

    LHAT_CHECK_ERR_MEMBER_EXISTS,       // 14.12: same name, no marker
    LHAT_CHECK_ERR_COMPOSE_COLLIDES,    // 14.5: and no marker can be written
    LHAT_CHECK_ERR_AMBIGUOUS_MEMBER,    // 14.5改: both sides carry it, so the
                                        // composition reaches no one answer
    LHAT_CHECK_ERR_STILL_ABSTRACT,      // 14.15: new on a definition with a
                                        // member nothing has provided yet
    LHAT_CHECK_ERR_FIELD_UNPROVIDED,    // 14.15改3: the same for a template
                                        // field, which a written new may also
                                        // give a value to
    LHAT_CHECK_ERR_ALREADY_PROVIDED,    // 14.15: abstract^ over a member that
                                        // something in the chain provides
    LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE, // 14.15改2: abstract^ over one this
                                        // same def^ writes a value for
    LHAT_CHECK_ERR_MUTABLE_DEFAULT,     // 14.11: a template initialiser's
                                        // value sits on the prototype and is
                                        // shared by every instance, so it has
                                        // to be immutable -- a mutable one is
                                        // made inside new
    LHAT_CHECK_ERR_NEW_RETURNS,         // 14.11: construction answers the
                                        // copy, so a new body has no
                                        // return^ of its own
    LHAT_CHECK_ERR_PROTOTYPE_SEALED,    // 14.11: the prototype is read
                                        // through self^ and written by no one
    LHAT_CHECK_ERR_NOT_BOXABLE,         // 05 の 8.9: box^ takes a host value
                                        // -- everything else already lives
                                        // on the heap and needs no box
    LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, // a marker with nothing under it
    LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,   // 14.12: override^ has to fit
    LHAT_CHECK_ERR_OVERLOAD_OVERLAPS,   // 14.12: signatures must stay apart
    LHAT_CHECK_ERR_OPERATOR_UNSETTLED,  // 03 の 3.4改: several types here
                                        // carry it and nothing says which
    LHAT_CHECK_ERR_INCOMPARABLE,        // no value inhabits both sides
    LHAT_CHECK_ERR_AS_IMPOSSIBLE,       // 11.6: as^ between two types
                                        // no value inhabits both of
    LHAT_CHECK_ERR_BAD_KEY,             // 04 の 11.3: nil^ spells absence, so
                                        // it cannot also be a key
    LHAT_CHECK_ERR_NO_OPERATOR,         // 11.3: an operator is answered by
                                        // the left operand, and this one
                                        // carries no answer for it
    LHAT_CHECK_ERR_BAD_OPERATOR,        // 11.8: an op^ is an f^ taking self^
                                        // and one argument, and this is not
    LHAT_CHECK_ERR_ISA_ALWAYS_TRUE,     // 13.7: any^ holds of every value, so
                                        // asking it of one asks nothing
    LHAT_CHECK_ERR_MISSING_FIELD,       // 04 の 2.5: no default to fall back to
    LHAT_CHECK_ERR_REQUIRE_FAILED,      // 05 の 6 章: the unit could not be had
    LHAT_CHECK_ERR_COROUTINE_DROPPED,   // 15.8: a call that makes a coroutine
                                        // and does nothing with it
    LHAT_CHECK_ERR_NOT_COROUTINE,       // 15.8: yieldall^ needs one, and
                                        // 16.3: so does in^, which walks
                                        // whatever iterate() answers with
    LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE, // 15.14: the same rule under the
                                        // other word. What is awaited is
                                        // something that finishes, and only
                                        // a coroutine does
    LHAT_CHECK_ERR_COROUTINE_MISMATCH,  // 13.9 with 15.2: a c^{ … } written in
                                        // the result position says what a call
                                        // answers, and the yield^ sites say it
                                        // too. The two disagreed

    LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION, // 15.2: a yield^ that is bound
                                            // (directly, by a let^) needs a
                                            // written type there to fix R
    LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH,    // 15.2: two yield^/yieldall^ sites
                                            // in one body disagree on Y or R

    LHAT_CHECK_ERR_PATH_NOT_TABLE,      // 8.8: everything before the last
                                        // segment holds the one after it, so
                                        // it has to be a table
    LHAT_CHECK_ERR_PATH_IS_DEFINITION,  // 8.8: 14 章 fixes what an instance
                                        // of a def^ carries, so a member
                                        // cannot be added to one
    LHAT_CHECK_ERR_MODULE_UNNAMED,      // 05 の 5.5: the short form binds a
                                        // unit under the path it declared,
                                        // and this one declared none (3.2)
    LHAT_CHECK_ERR_NOT_HOSTED,          // 05 の 8.7: import^ reaches what the
                                        // host registered, and nothing here
    LHAT_CHECK_ERR_SCOPE_TOO_FAR,       // 01 の 8 章: '$^' naming more scopes
                                        // out than are open here
    LHAT_CHECK_ERR_SCOPE_ON_DEFINE,     // 01 の 8 章 with 8.7: a let^ makes a
                                        // name where it is written, so there
                                        // is no other scope for one to name
    LHAT_CHECK_ERR_FUNCTION_WRITES_OUT,  // 15.1: an f^ assigns to local
                                         // variables only, and this name was
                                         // bound outside its body
    LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE, // 15.1改: the same rule for a table
                                           // -- this one is not one the body
                                           // made, so changing it is
                                           // observable from outside
    LHAT_CHECK_ERR_PATH_IS_OPAQUE,      // 05 の 8.8: a host type carries what
                                        // the host registered, and 14.16's
                                        // type info what made it; neither is
                                        // written over from L^
    LHAT_CHECK_ERR_ADVANCES_OUTSIDE,    // 15.3改: an f^ advancing a coroutine
                                        // its own body did not make, whose
                                        // progress the caller can see
    LHAT_CHECK_ERR_COROUTINE_ESCAPES,   // 15.3改: an f^ coroutine reaching the
                                        // outside, where advancing it is what
                                        // makes the call observable
    LHAT_CHECK_ERR_TABLE_IS_SEALED,     // 05 の 8.6: L^, its registry
                                        // and what require^/import^ answers
                                        // are the machine's own; the host
                                        // writes them through its API
    LHAT_CHECK_ERR_ASSIGN_TO_LET,       // 8.9: the name was bound by a let^,
                                        // which a ':=' may not reach. Writing
                                        // var^ instead is open here
    LHAT_CHECK_ERR_ASSIGN_TO_FORM,      // 8.9 with 12.1 and 16.3改2: bound by
                                        // the construct rather than a word --
                                        // a with^, or a for^ focus -- so
                                        // there is no var^ to offer
    LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE, // 05 の 4 章 with 8.9: a public^ var^
                                        // is a name another unit could see
                                        // change, which 01 の 8.3 refused
    LHAT_CHECK_ERR_COMPARE_NOT_NUMBER,  // 11.9: an op^<=> answers a number^,
                                        // which is what the orderings read
    LHAT_CHECK_ERR_EQUAL_NOT_BOOL,      // 11.9改: an op^= answers a bool^,
                                        // which '=' takes as it stands and
                                        // '≠' negates
    LHAT_CHECK_ERR_NOT_ORDERED,         // 11.9: an ordering is read off '<=>',
                                        // and neither side carries one taking
                                        // the other
    LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR, // 11.3改: a self^ written last says
                                           // the right operand is the
                                           // receiver, which only an op^ has
                                           // any use for (14.4)
    LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE,   // 13.13: Self^ names the type literal
                                        // around it, and here there is none --
                                        // no t^ or def^ encloses it, or a
                                        // second hat counted past the
                                        // outermost one
    LHAT_CHECK_ERR_TUPLE_MISPLACED,     // 13.8改: a tuple is written in a
                                        // result and nowhere else -- not
                                        // bound to a name, passed as an
                                        // argument, held in a table, or
                                        // nested in another tuple. Confining
                                        // it there is what keeps 13.8's four
                                        // propagations from coming back. Use
                                        // pack^ to make a table of it
    LHAT_CHECK_ERR_TUPLE_UNION,         // 13.8改 with 04 の 3.1: the only
                                        // thing a tuple may be written in a
                                        // union with is an error
    LHAT_CHECK_ERR_TUPLE_ARITY,         // 13.8改: the names taking the values
                                        // apart and the values answered are
                                        // not the same count. A tuple has exactly its
                                        // positions, since each one is a slot
                                        // the caller reserved -- 14.10's width
                                        // subtyping never applies here
    LHAT_CHECK_ERR_TUPLE_ERROR_POSITION,// 13.8改 with 04 の 8.2: the error
                                        // goes around the values, never among
                                        // them. '(number^, IOError)' is the
                                        // shape Go's 'v, err := f()' needs --
                                        // one value to use and one to drop --
                                        // and 8.2 kept it unwritable by
                                        // having no second value at all.
                                        // Multi-value return brings the
                                        // second value back, so the rule has
                                        // to be said out loud instead. Write
                                        // '(A, B)|IOError'
    LHAT_CHECK_ERR_CATCHES_NOTHING,     // 04 の 4.5: a try^{ } whose body
                                        // writes no try^ has nothing to
                                        // catch, the same as 4.1's catch^ on
                                        // an expression that cannot fail
    LHAT_CHECK_ERR_CLOSED_CAPTURES,     // 15.13: a closed^ body named
                                        // something standing outside it. The
                                        // mark is a promise that it carries
                                        // no captured place, which is what
                                        // lets the value be handed to a
                                        // machine of its own (05 の 8.7)
    LHAT_CHECK_ERR_HOSTVALUE_ESCAPES,   // 05 の 8.9: a host value lives in
                                        // stack slots and nowhere else, so a
                                        // table, a capture, an any^, a yield
                                        // and the program's answer all refuse
                                        // one -- box it into the hostdata
                                        // container its library provides.
                                        // Refused even under relaxed: a
                                        // runtime check cannot make room
                                        // where the representation has none
    LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL,  // 04 の 11.3: what a key reads out
                                        // of a table is 'T|nil^', and no
                                        // operator is on the nil^ side.
                                        // 11.3改's rule sends the writer
                                        // looking for an operator to write
                                        // when what is missing is a narrowing
                                        // -- and 13.11 does not narrow an
                                        // index where it stands
    // 02 の 18.5: an annotation the host did not register, one written where
    // its registration does not allow, and arguments that do not fit what it
    // declared. Three failures rather than one: what to change differs.
    LHAT_CHECK_ERR_NO_SUCH_ANNOTATION,
    LHAT_CHECK_ERR_ANNOTATION_MISPLACED,
    LHAT_CHECK_ERR_ANNOTATION_ARGUMENTS,
    LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE,// 18.5改: registered as one of two
                                        // answers to one question, and the
                                        // other is in this file already
    LHAT_CHECK_ERR_ANNOTATION_REPEATED, // 18.5: registered FILEUNIQUE, and
                                        // the file already had one
    LHAT_CHECK_ERR_BARE_TABLE_TYPE      // 14.10: t^ is written with the
                                        // members it asks for, and the top of
                                        // tables is the one asking for none
                                        // -- 't^{}'. Bare, the '{' that
                                        // follows a signature would be read
                                        // as the member list rather than as
                                        // the body
} LhatCheckErrorCode;

typedef struct {
    LhatCheckErrorCode code;
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // The name the diagnostic is about, for the codes that are about one --
    // "no such name in scope" is worth more when it says which. Borrowed from
    // the source the unit was read from, which outlives the result, and NULL
    // for the codes that name nothing.
    const char *name;
    uint32_t name_length;
} LhatCheckDiagnostic;

#if LHAT_WITH_RESOLUTIONS
// 07 の 4 章: where a name that was written got its meaning.
//
// Recorded as each use is resolved, so that a tool answering "what is this"
// reads what the checker decided rather than resolving names a second time.
// A second implementation of 8 章's scoping would be one more thing to keep
// in step, and it would disagree in exactly the cases that are hard.
//
// **Nothing in the language reads this.** 03 の 1.1's later stages compile
// and run the same tree whether it was recorded or not; it is here for the
// language server, and LHAT_WITH_RESOLUTIONS takes it out for a host that
// embeds the language without any tooling.
typedef struct {
    uint32_t use;      // where the name stands, as written
    uint32_t use_end;  // one past it, so a position within the name matches
    uint32_t definition;  // where the name it reaches was bound
    // False when there is no such place to point at: a member is looked up
    // in a type rather than in a scope (14.10), and the type may have come
    // from another unit or from a host registration, where nothing in this
    // source declares it. The type is still known, which is what a reader
    // asking "what is this" wants most.
    bool has_definition;
    // 13.1: declared by a signature rather than bound by a let^ or var^.
    // Where a use stands says only that there is a name, and the type says
    // only what it holds -- which of the two declared it is something only
    // the checker knows.
    bool is_parameter;
    // 8.9: bound by a let^ (or by a form that binds like one -- 12 章's
    // with^, 16.3's walking focus), so nothing written after it reassigns
    // the name.
    bool immutable;
    // What the name holds there. Belongs to the result's type arena, so it
    // is valid for as long as the result is.
    LhatType *type;
} LhatResolution;
#endif  // LHAT_WITH_RESOLUTIONS

// 05 の 8.7: a host registers what it provides by writing the type out, so
// there has to be a way from written text to a type without a unit around
// it. `named` stands for the names a type may mention -- a table type whose
// members are what the host has registered so far, so that one registration
// can name what an earlier one made. NULL admits only the builtins.
//
// Answers NULL when the text is not a type, or names something `named` does
// not hold. The type belongs to `arena`.
LhatType *lhat_type_of_text(const char *text, size_t length,
                            LhatTypeArena *arena, LhatType *named);

// 05 の 6 章: a unit's exports are types that the units requiring it hold on
// to, so the arena has to outlive any one result. The caller may pass its own
// and share it across a whole program; without one, the result keeps its own.
typedef struct {
    LhatTypeArena *types;
    LhatTypeArena owned;

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
#endif

    // 05 の 4 章: the structure of what this unit publishes, or NULL when it
    // publishes nothing. What a require^ of it yields.
    LhatType *exports;

    // 05 の 3 章: the path module^ declared, written out with its dots and
    // owned by the result. NULL when the unit declared none, which 3.2
    // allows. 5.5 needs it to say what the short form of require^ binds.
    char *module_name;
} LhatCheckResult;

#if LHAT_WITH_RESOLUTIONS
// The resolution covering `offset`, or NULL when no name was written there.
// `offset` may fall anywhere within the name, not only on its first byte.
const LhatResolution *lhat_check_resolution_at(const LhatCheckResult *result,
                                               uint32_t offset);
#endif

// 05 の 5 章. Asked for the unit at `path`, relative to whatever the resolver
// considers the requiring unit. Returns its export structure, or NULL when it
// could not be had -- a missing file or a cycle (6.3), which the resolver
// reports in its own terms.
//
// `module_name` is filled with the path 3 章 had that unit declare, or left
// NULL when it declared none (3.2). 5.5 binds the short form under it, so
// the text has to outlive the call -- the unit owns it.
typedef LhatType *(*LhatRequireResolver)(void *context, const char *path,
                                         size_t length,
                                         const char **module_name);

typedef struct {
    LhatRequireResolver resolve;
    void *context;
    // 05 の 8.7: what the host registered, as one nested table type keyed by
    // module path. import^ resolves against this alone, so its answer does
    // not depend on the order units happen to be checked in.
    LhatType *hosted;

    // 05 の 8.6: what the host put in L^ itself, as a table type whose members
    // are those. NULL when it put nothing there. What 8.6 lists is carried by
    // the checker regardless; this is what a host added on top.
    LhatType *globals;

    // 05 の 8.2: the names the host bound before anything ran, each to a
    // member of L^ (8.6). A name no scope holds is looked up here before it
    // is reported missing, and its type is the member's.
    //
    // 8.1 is unchanged by this. The language hands out no names; a host does,
    // and one that binds none leaves the program seeing nothing. What a name
    // reaches stays readable as L^.<member> whatever a let^ later shadows.
    //
    // Two arrays rather than one of pairs, so that neither this header nor
    // vm.h has to know a type the other declares.
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // 02 の 18.5: the annotations the host registered, which are the only
    // ones a program may write. NULL/0 when it registered none, and then
    // every annotation written is one the checker refuses.
    const LhatAnnotationDecl *annotations;
    size_t annotation_count;
} LhatRequire;

// 03 の 3.1. `strict` is a setting of the compilation unit, not a dialect:
// 3.2 keeps the source text identical either way, and 3.5 limits what
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

// 03 の 4.3: a REPL checks many inputs as one running program, so a name one
// input bound has to still have its type in the next. The session carries
// them, along with the arena their types live in -- the types outlive any one
// input, and so must the names, which are copied out of the source each was
// read from.
typedef struct LhatCheckSession LhatCheckSession;

LhatCheckSession *lhat_check_session_new(void);

// Frees the session, the names it copied and every type in its arena. Nothing
// read out of a result checked in it stays valid.
void lhat_check_session_dispose(LhatCheckSession *session);

// 05 の 8.6: a member of L^ the host provides, its type written in 13 章's
// grammar the way 8.7's registration writes one. A prompt has no LhatProgram
// to hold these, so the session does -- what a file gets through LhatRequire.
// Resolved in the session's own arena, so the caller keeps no type.
bool lhat_check_session_global(LhatCheckSession *session, const char *name,
                               const char *signature);

// 05 の 8.2: the names the host bound to members of L^. The arrays belong to
// the caller and have to outlive the session; setting them again replaces
// what was there.
void lhat_check_session_bind(LhatCheckSession *session,
                             const char *const *names,
                             const char *const *members, size_t count);

// 05 の 8.7: the host registry a prompt's import^ resolves against, and 8.6's
// L^ members alongside it -- the LhatRequire fields a file gets from its
// LhatProgram. A prompt has no program of its own, so a host that wants
// import^ to answer at a prompt makes one, registers into it, and hands the
// two over. program.h's lhat_program_install_checks is that call written out.
//
// Both types belong to whatever arena made them, so that has to outlive the
// session -- a program registered into keeps its own and is the usual answer.
// `globals` may be NULL to leave what lhat_check_session_global built alone;
// anything else replaces it.
void lhat_check_session_hosted(LhatCheckSession *session, LhatType *hosted,
                               LhatType *globals);

// Checks `unit` as the next input of `session`. The names already bound in it
// are in scope, and the ones this input binds stay for the next. The result
// borrows the session's arena, so it has none of its own to dispose.
void lhat_check_next(LhatCheckSession *session, const LhatNode *unit,
                     const LhatLexer *lexer, bool strict,
                     LhatCheckResult *result);

// 14.15 with 14.11: the first member of `type` that nothing has provided --
// declared by an abstract^, or left waiting by 14.15改's override^ -- or NULL
// when every one is filled. A definition still holding one cannot be
// instantiated, which is what makes it worth saying out loud: it is the
// difference between a type to compose onto and a type to make one of, and
// nothing in the written form says which.
//
// The rule is the checker's (this is the very predicate 14.11 refuses new
// with), so it is asked here rather than read off the type a second time.
const LhatTypeMember *lhat_check_unimplemented_member(const LhatType *type);

const char *lhat_check_error_message(LhatCheckErrorCode code);

// The message for one diagnostic, which for the codes that are about a name
// says which -- "no such name in scope: nowhere". Everything a code knows on
// its own is what lhat_check_error_message answers, so this only differs
// where the diagnostic carries something besides its code.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it.
size_t lhat_check_message_write(const LhatCheckDiagnostic *diagnostic,
                                char *out, size_t capacity);

#endif  // LHAT_CHECK_H
