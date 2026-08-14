// L^ (lhat) -- types and the two relations the checker is built on.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "04", which means 04-errors.md.
//
// Identity is structural (11.3), so two types built independently with the
// same shape are the same type and nothing has to be interned for that to
// hold. Error kinds are the one exception: 04 の 2.4 makes their identity the
// declaration they came from, which here is the address of the set object.
//
// Types come from an arena and are never freed individually.

#ifndef LHAT_TYPE_H
#define LHAT_TYPE_H

#include <stdbool.h>
#include <stddef.h>

// 05 の 8.9: declared in object.h; the checker only ever compares the
// address, so the shape is not needed here.
struct LhatHostValueTag;

typedef enum {
    // Inference has not decided. Conforms to everything and nothing is
    // reported against it, so one failure does not produce a cascade. 03 の
    // 3.5 also lands here: under relaxed these become runtime checks.
    LHAT_TYPE_UNKNOWN,

    // 13.2: what an expression that produces no value has. A p^ whose
    // signature has no result answers this, which is a different thing from
    // UNKNOWN -- there is nothing here, rather than nothing known. Nothing
    // inhabits it, so it fits nowhere a value is wanted. 03 の 3.4 keeps it
    // out of nil^, and this is what keeps that distinction observable.
    LHAT_TYPE_NONE,

    LHAT_TYPE_ANY,         // any^ (13.7)
    LHAT_TYPE_NIL,
    LHAT_TYPE_BOOL,
    LHAT_TYPE_NUMBER,      // 14.8: one type over integers and reals
    LHAT_TYPE_STRING,

    LHAT_TYPE_TABLE,       // t^{ ... }: at least these members (14.10)
    LHAT_TYPE_FUNC,        // f^ and p^ (15 章)
    LHAT_TYPE_CORO,        // c^{ ... } (13.9)

    // 13.8改: (A, B) -- several values returned at once. A type, unlike the
    // value sequence 13.8 refused: it has a name and conforms like anything
    // else. What keeps 13.8's four propagations from coming back is where it
    // may be written rather than what it is -- a result position and nowhere
    // else, which the checker enforces off this kind. A separate kind from
    // TABLE on purpose: 14.10's width subtyping says a table may carry more
    // than its type lists, and a tuple may not, since every position of one
    // is a slot the caller reserved.
    //
    // The positions live in v.composite.arms, ordered, the way a union's arms
    // do. The two kinds never mix, so sharing the field costs nothing.
    LHAT_TYPE_TUPLE,

    // 05 の 8.9: a host-defined value type, held in consecutive stack slots
    // rather than on the heap. Its own kind rather than a nominal TABLE so
    // that every place a type may escape the stack (a table member, a
    // capture, an any^) has to say it accepts one -- a switch that does not
    // know the kind refuses, which is the right default for a type that
    // must never leave a frame. The payload shares v.table so the member
    // machinery (registration, lookup) reads it unchanged.
    LHAT_TYPE_HOSTVALUE,

    LHAT_TYPE_ERROR,       // error^ (04 の 2.3): the supertype of every kind
    LHAT_TYPE_ERROR_SET,   // what one errordef^ declares (04 の 2.2)
    LHAT_TYPE_ERROR_KIND,  // one kind within a set

    LHAT_TYPE_UNION,       // A | B  (13.5)
    LHAT_TYPE_INTERSECT,   // A & B  (14.5)

    // A gap in inference specifically: a seed placed before a signature or a
    // mutually recursive partner has been checked, waiting to be replaced
    // once that checking finishes. 03 の 3.1・3.5、P6: strict reports this
    // one if it survives to a place a concrete type was wanted, unlike
    // UNKNOWN above, which also covers table subtyping's silence, computed
    // keys, and other cases with nothing to report even under strict.
    LHAT_TYPE_PENDING,

    LHAT_TYPE_KIND_COUNT
} LhatTypeKind;

typedef struct LhatType LhatType;

// A named member of a table or the fields a kind declares. The name points
// into the source text, which therefore has to outlive the types.
typedef struct LhatTypeMember {
    const char *name;
    size_t name_length;
    LhatType *type;
    // 04 の 2.2: a field declared with a default need not be written at the
    // construction. Whether it has one is not part of the type -- two kinds
    // differing only there are still the same shape -- so nothing in the
    // relations reads this.
    bool optional;
    // 02 の 14.15: declared by an abstract^ and not yet provided. The shape
    // is the same either way -- what a caller may ask of the member does not
    // change -- so the relations do not read this. 14.11's new does: a
    // definition still holding one cannot be instantiated.
    bool abstract;
    // 02 の 14.15改: written with an override^ that has not met what it
    // replaces yet, so the member is here but super^ inside it points at
    // nothing. Composing onto something that provides the name settles it.
    bool pending;
    // 02 の 14.5改: two sides of a composition both carry it, and neither was
    // written against the other. The name reaches no one answer, so it is not
    // reachable through the composed type -- what each side wrote is still
    // reachable through that side.
    bool ambiguous;
    // 02 の 14.7改: put there by a def^'s first pass, from the signature the
    // member was written with, so a body checked before it can reach it. The
    // second pass writes the inferred type over it. 14.12's "a second member
    // of this name needs a marker" reads the members to find what was already
    // there, and one of these was not -- so that question skips them. The
    // relations do not read this, as with the three above.
    bool provisional;
    struct LhatTypeMember *next;
} LhatTypeMember;

// Types appear in more than one list (union arms, parameters), so the link
// lives in a separate node rather than on the type itself.
typedef struct LhatTypeList {
    LhatType *type;
    struct LhatTypeList *next;
} LhatTypeList;

struct LhatType {
    LhatTypeKind kind;

    union {
        struct {
            LhatTypeMember *members;
            // 14.1: def^ is the only way to make one, and 14.5 composes two
            // of them with '..' -- which 11.2's operator has to tell from a
            // call. Identity is still structural (11.3): this says what made
            // the structure, not what it is.
            bool is_definition;
            // 14.7改: what this definition's instances carry -- the template's
            // fields and the members that take a receiver (14.4). NULL on
            // everything that is not a definition, and on a definition the
            // checker did not build one for (05 の 8.8's host types).
            //
            // A definition and an instance of it do not carry the same
            // members: new and a static member are the definition's, and an
            // instance is not a thing to construct another from or to call a
            // receiverless member on. This is the link 13.13's Self^ means
            // inside the definition, and what the writer puts down as the
            // self^{ … } section of a written-out definition.
            LhatType *instance;
            // 8.8: what a def^ says its instances carry is fixed (14 章), so
            // a member cannot be added to one. Like `is_definition` this
            // records what made the structure and takes no part in
            // conformance -- 11.3 keeps identity structural.
            bool from_definition;
            // 05 の 8.8: identity is the declaration, not the shape. The one
            // place 11.3 gives way, and 7.3's reason is weaker than this
            // one: an opaque host type has no shape to compare, so structure
            // would make every one of them the same type and hand a pointer
            // to the C that expects another.
            bool nominal;
            // 05 の 8.6: a table the machine made rather than the program --
            // L^ itself, its module registry, and what require^ or import^
            // answers with. Nothing written in L^ may change one. The host
            // reaches these through its own API, which never goes through the
            // checker, so refusing every write written here is the whole rule
            // and there is nothing to spell in the source. Like the two above
            // it records where the structure came from and takes no part in
            // conformance (11.3).
            bool sealed;
            // 05 の 8.6: a table names are reached *through* rather than one
            // that holds a value -- L^ and its module registry, a segment of
            // that registry, what a require^ published, the tree a host
            // registered, and the root a dotted import^ or require^ path
            // stands on.
            //
            // Separate from `sealed`, which is next to it and looks like it
            // would do: that one says the program may not write here, and
            // the two are not the same question. The root of an import^ path
            // is a namespace and is *not* sealed, because the import itself
            // writes the segment it brought in there. Like the three above
            // it records what the structure is for and takes no part in
            // conformance (11.3).
            //
            // Nothing in the language reads it. It is here so that a tool
            // can tell a module apart from a table, which is a question the
            // shape alone cannot answer.
            bool is_module;
            // 13.7's variadic collector as a table type, and 14.10's
            // 't^{ ...:T }': the sequence half is unbounded, every position
            // of it T. NULL everywhere else -- mirrors func.variadic below,
            // which is the same idea for a parameter list instead of members.
            LhatType *variadic;
            // 05 の 8.9: set exactly when the kind is LHAT_TYPE_HOSTVALUE.
            // Identity is this pointer, for 8.8's reason sharpened by value
            // semantics: the C reading the bytes back must never read them
            // as another type's. The tag belongs to the program.
            const struct LhatHostValueTag *hostvalue_tag;
        } table;

        // 13.1. `result` is NULL when nothing is returned (13.2), and
        // `variadic` holds the element type of a trailing '...' (13.7).
        struct {
            LhatTypeList *params;
            LhatType *result;
            LhatType *variadic;
            bool is_function;  // f^ rather than p^ (15 章)
            // 14.4: a first parameter of self^ marks an instance method, and
            // 'x.m()' passes x there without writing it. The receiver is not
            // in `params`, so an ordinary call needs no special case.
            bool takes_self;
            // 11.3改: the self^ was written last instead, which only an
            // op^ may do -- it says the receiver is the RIGHT operand, so
            // 'op^+ = f^lhs:number^, self^' answers '1 + v'. The receiver is
            // out of `params` either way; this says which side it stands on.
            // Two signatures differing here are different types: what may be
            // written on the left of one cannot be written on the left of the
            // other.
            bool self_last;
            // 15.2: the body contains yield^ or yieldall^, so calling it answers
            // a coroutine rather than running anything (15.5)
            bool yields;
            // 15.13: written closed^ -- the body names nothing standing
            // outside it, so the value carries no captured place. A marked
            // one stands where an unmarked signature is written; the other
            // way round is what conforms_func refuses.
            bool closed;
            // 15.2: what the body's yield^/yieldall^ sites agree on. Both NULL
            // until inferred; meaningless unless `yields` is true.
            LhatType *yield_produce;
            LhatType *yield_receive;
            // 15.5: what a call to this answers. A body that yields is not
            // run by the call -- it makes a coroutine (13.9) -- so that is
            // what the caller receives, and this is it, assembled out of the
            // three fields above once they settle. NULL on everything else,
            // where `result` is the answer.
            //
            // 13.9's T stays in `result` either way: that is what the body
            // returns, which is what the compiler and 15.2 read. Everything
            // that asks what a *call* is worth -- the writers, conformance,
            // the runtime type -- reads this one first.
            LhatType *answers;
            // 03 の 3.4: the body has a way out that produces no value -- it
            // reaches its end, or a bare return^ takes it there. False means
            // every way out carries a value, or there is no way out at all.
            // 13.9 reads it to tell a coroutine that ends without one from
            // one that cannot end.
            bool ends_without_value;
        } func;

        struct {
            // 13.9. NULL means the slot is empty rather than unknown:
            // `receive` NULL is a coroutine nothing is sent to, whose resume
            // takes no argument; `result` NULL is a body that ends without a
            // value, where 15.6改's nil^ joins Y|T rather than standing here.
            LhatType *receive;
            LhatType *produce;
            LhatType *result;
            // 13.9: the body cannot end, so there is no last resume for a
            // result to be the type of and nothing joins Y. Written '-'. A
            // separate fact from `result` being NULL, and the two are
            // different types -- what a resume answers differs.
            bool endless;
            // 15.3改: the kind of the body this came from, which 13.9 writes
            // as the front half ('c^{ f^R -> Y;, T }'). Advancing a coroutine
            // runs its body, so start()/resume()/dispose() take this kind and
            // 15.1's calling rule does the rest -- an f^ reaching for a p^
            // coroutine is an f^ calling a p^, caught where every other one
            // is. Two of these differing here are different types: what may
            // be done with one is not what may be done with the other.
            bool is_function;
        } coroutine;

        // ERROR_SET and ERROR_KIND. A kind points back at the set that
        // declared it, and that pointer is the identity (04 の 2.4).
        struct {
            const char *name;
            size_t name_length;
            LhatType *set;           // ERROR_KIND only
            LhatTypeMember *fields;  // ERROR_KIND only; NULL when it declares none
            LhatTypeList *kinds;     // ERROR_SET only
        } error;

        struct {
            LhatTypeList *arms;
        } composite;
    } v;
};

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

typedef struct LhatTypeArenaBlock LhatTypeArenaBlock;

typedef struct {
    LhatTypeArenaBlock *blocks;
    size_t type_count;
} LhatTypeArena;

void lhat_type_arena_init(LhatTypeArena *arena);
void lhat_type_arena_dispose(LhatTypeArena *arena);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// The primitives and the two tops. Each call returns a fresh object; identity
// is structural, so callers never have to share one.
LhatType *lhat_type_simple(LhatTypeArena *arena, LhatTypeKind kind);

LhatType *lhat_type_table(LhatTypeArena *arena);
// 05 の 8.9: a host value type. Nominal like a hostdata type -- identity is
// the tag -- and additionally barred from every place that outlives a frame,
// which conformance and the checker enforce off the kind.
LhatType *lhat_type_hostvalue(LhatTypeArena *arena,
                              const struct LhatHostValueTag *tag);
LhatType *lhat_type_func(LhatTypeArena *arena, bool is_function);
// 15.3改: `is_function` is the kind of the body, which decides what may
// advance the coroutine and where it may be held. 13.9: a NULL `receive` or
// `result` is an empty slot, and `endless` is the third slot's other absence
// -- a body that cannot end, written '-'.
LhatType *lhat_type_coro(LhatTypeArena *arena, LhatType *receive,
                         LhatType *produce, LhatType *result, bool endless,
                         bool is_function);

// 04 の 2.2. The set is created first; each kind then points at it, which is
// what makes two identically written declarations different types.
LhatType *lhat_type_error_set(LhatTypeArena *arena, const char *name,
                              size_t name_length);
LhatType *lhat_type_error_kind(LhatTypeArena *arena, LhatType *set,
                               const char *name, size_t name_length);

// Appends and returns the member, or NULL when out of memory. Works on a
// TABLE and on an ERROR_KIND's fields.
LhatTypeMember *lhat_type_add_member(LhatTypeArena *arena, LhatType *owner,
                                     const char *name, size_t name_length,
                                     LhatType *type);

// 02 § 14 makes a table a sequence as well as a mapping, and the sequence
// half is described by members whose names are the indices. 01 § 6 spells
// a member name as an identifier, so digits are a name the program can never
// write -- these can only ever collide with each other.
LhatTypeMember *lhat_type_add_index_member(LhatTypeArena *arena,
                                           LhatType *owner, size_t index,
                                           LhatType *type);

// The member standing for a one-based position, or NULL when the table says
// nothing about it.
const LhatTypeMember *lhat_type_member_at(const LhatType *table, size_t index);

// Appends to a parameter list or to a set's kinds.
bool lhat_type_add_param(LhatTypeArena *arena, LhatType *func, LhatType *param);

// 13.8改: a tuple, built one position at a time. Positions are never merged
// the way lhat_type_union merges arms -- '(number^, number^)' has two of them
// and means it. A tuple with fewer than two positions is never built: 13.1
// writes one value as itself, and '(T)' is the grouping parentheses the type
// grammar already had.
LhatType *lhat_type_tuple(LhatTypeArena *arena);
bool lhat_type_add_position(LhatTypeArena *arena, LhatType *tuple,
                            LhatType *position);

// How many positions a tuple has, or 0 for anything else -- so a caller may
// ask without testing the kind first.
size_t lhat_type_tuple_width(const LhatType *type);

// The same, reaching through a union to the one tuple arm 13.8改 allows in
// it -- '(K, V)|nil^' answers 2. 0 when no arm is a tuple.
size_t lhat_type_tuple_arm_width(const LhatType *type);

// The type at a zero-based position, or NULL when there is none.
LhatType *lhat_type_tuple_at(const LhatType *type, size_t index);

// 15.5: what a call to this signature answers -- the coroutine a yielding
// body makes (13.9), or the result written on it. Anything asking what a call
// is worth goes through here; reading `result` on its own answers 13.9's T,
// which is a different question for a body that yields.
//
// NULL for a signature that answers nothing (13.2), and for anything that is
// not a signature at all.
LhatType *lhat_type_call_answer(const LhatType *type);

// Builds a union or an intersection. Arms that add nothing are dropped, so
// 'number^|number^' collapses and 'T|any^' becomes any^. A single surviving
// arm is returned on its own rather than wrapped.
LhatType *lhat_type_union(LhatTypeArena *arena, LhatType *a, LhatType *b);
LhatType *lhat_type_intersect(LhatTypeArena *arena, LhatType *a, LhatType *b);

// ---------------------------------------------------------------------------
// Relations
// ---------------------------------------------------------------------------

// 13.11: may a value of `value` stand where `target` is written? This is the
// question an annotation asks, and is^ asks the same one, which is why one
// type expression means one thing wherever it appears.
bool lhat_type_conforms(const LhatType *value, const LhatType *target);

// 03 の 3.1: is there a gap in inference here -- a pending^ the walk left
// behind, whether the type is one or carries one? A union arm counts (a
// mutually recursive partner answers 'bool^|pending^', which promises its
// callers a type it does not have), and so does a tuple position.
//
// A subroutine's parameters and a table's members are not walked into. A
// parameter left pending^ is a constraint nobody wrote, which 3.1 has always
// forgiven; a member is a question about that table, asked where the table is
// built. unknown^ is not a gap either: it covers spots with nothing to report
// (table subtyping's silence, a computed key) rather than inference that had
// somewhere left to run.
bool lhat_type_has_gap(const LhatType *type);

// 03 の 3.4改2: the same type with the gap arms of a union taken out, for a
// walk that is about to be run again from what the last one learned. The arm
// a mutually recursive call left behind would otherwise never leave -- the
// next walk reads it back and unions it in again -- so a walk seeded with
// 'bool^' answers what it can, and the walk after that either agrees or finds
// the arm was wider than that.
//
// Only for a seed. What a walk answers is built afresh each time, so nothing
// this drops is lost: an arm that really belongs comes back on the next walk.
// A type that is not a union, and one whose every arm is a gap (there being
// nothing left to say), is answered unchanged.
LhatType *lhat_type_without_gaps(LhatTypeArena *arena, LhatType *type);

// 03 の 3.1・3.5: the same question, asked the way strict asks it, and
// asymmetrically at that. A *value* with a gap in it (lhat_type_has_gap
// above) is not forgiven here the way it is under the lenient default; strict
// reports it instead of leaving it for a runtime check relaxed has not grown
// yet. A *target* left pending^ is a different thing -- a constraint nobody
// wrote (an omitted parameter or return annotation, a binding a
// multiple-assignment left short) -- and stays forgiven, the same as any^.
// unknown^ itself stays forgiven on either side even under strict. Checkers
// running relaxed keep using lhat_type_conforms.
bool lhat_type_conforms_strict(const LhatType *value, const LhatType *target);

// Mutual conformance. Used to drop redundant union arms.
bool lhat_type_equal(const LhatType *a, const LhatType *b);

// 14.12: is there no value at all that inhabits both? Overload resolution
// requires this of every pair of signatures, and narrowing reads it to decide
// which arms of a union survive a branch.
bool lhat_type_disjoint(const LhatType *a, const LhatType *b);

// Writes `type` in the notation 13 章 spells it with -- 'number^',
// 't^{ a : string^ }', 'f^number^ -> string^;' -- into `buffer`, always
// NUL-terminated when `size` is not zero.
//
// Follows lhat_report_write: answers how many bytes the whole thing wants,
// not counting the terminating NUL, and fills up to `size` including it. So
// measuring is a call with (NULL, 0). What does not fit is cut and ends in
// an ellipsis rather than being refused, so a buffer too small still holds
// something that reads as incomplete rather than as complete.
//
// This one also elides past LHAT_TYPE_WRITE_MAX_DEPTH and
// LHAT_TYPE_WRITE_MAX_ITEMS. Those are for a reader -- 07 の 4 章's hover,
// where a shorter answer says more -- and not what makes the walk finish:
// 14 章 lets a table hold itself, and what answers that is 13.13's Self^,
// written wherever the walk meets a structure it is already inside.
size_t lhat_type_write(const LhatType *type, char *buffer, size_t size);

// The same, eliding nothing but a cycle. For a caller that wants the type
// as text to keep -- 07 の 4 章's "copy the signature" -- rather than to
// read in a popup.
//
// Still not a promise of round-tripping, but the two things in the way are
// worth naming rather than leaving to be discovered. A type inference did
// not decide is written '?' (03 の 3.1), which nothing reads back; and a
// cycle is 'Self^', which 13.13 spells the same way but which only means
// this type inside the definition it belongs to.
size_t lhat_type_write_full(const LhatType *type, char *buffer, size_t size);

#endif  // LHAT_TYPE_H
