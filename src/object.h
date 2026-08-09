// L^ (lhat) -- the heap values: strings and tables.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.
//
// 02 の 14 章 makes the table the one data structure, so this file carries
// most of what a program manipulates. value.h holds the representation of a
// value; this holds what an object actually is.
//
// Every object is linked into an owner's list when it is made, and the owner
// frees the whole list at once. Two owners exist: a chunk, which holds the
// strings its constants name, and the machine, which holds what a program
// makes while it runs. The second is collected as the program runs, which is
// gc.h's business -- what is here is making an object and giving one up.

#ifndef LHAT_OBJECT_H
#define LHAT_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"
#include "lhatconfig.h"

// Who an object belongs to. Two exist: a chunk, holding what its constants
// name, and the machine, holding what a program makes while it runs. Only the
// second is collected.
//
// The count is kept here rather than at the call sites so that every
// allocation is seen -- the collector decides when to run from it.
typedef struct {
    LhatObject *objects;
    size_t count;

    // 5.12: the colour a new object is born with. It is one of gc.h's two
    // whites, and the collector swaps it for the other one each cycle -- so
    // an object made while a sweep is under way wears the colour of the
    // living and is not taken by the sweep it was born into. Zero is
    // LHAT_GC_WHITE0, which is why a heap needs no initialiser: a chunk's
    // is never swept and so never swaps.
    uint8_t white;
} LhatHeap;

// The bytes are copied in and kept NUL-terminated, so a string can be handed
// to a C interface without another copy. `length` is the byte count and does
// not count the terminator -- 01 の 5 章 makes a string a byte sequence, not
// a sequence of code points.
typedef struct LhatString {
    LhatObject header;
    size_t length;
    uint32_t hash;
    char text[];
} LhatString;

typedef struct {
    LhatValue key;
    LhatValue value;
} LhatTableEntry;

// 02 の 14 章: one data structure, so it has to serve as both a sequence and
// a mapping. The dense part holds the keys 1..array_count and the rest go in
// an open-addressed hash part, which is the split 03 の 1.2 kept from Lua.
typedef struct LhatTable {
    LhatObject header;

    LhatValue *array;  // array[i] is the value at key i+1
    size_t array_count;
    size_t array_capacity;

    LhatTableEntry *entries;  // capacity is a power of two, or zero
    size_t entry_count;       // live entries, tombstones not counted
    size_t entry_capacity;

    // 02 の 14.3: the members a definition holds are shared, and the fields
    // its template declares are copied. An instance holds its own fields here
    // and reads the shared ones from `definition`.
    //
    // 14.2 fixes this at the definition, so it is set when the instance is
    // made and never afterwards. That is what separates it from Lua's
    // metatable, which 14.1 refuses.
    const struct LhatTable *definition;

    // 14.9: made by a def^ rather than written as a table literal. `definition`
    // above answers the same question for an instance, and this is the other
    // half -- the definition itself is a table nothing points at that way.
    // Neither takes part in conformance (11.3 keeps identity structural);
    // 14.17改 reads both, to know whether the member names are the writer's.
    bool is_definition;

    // 05 の 8.6改 (M5): the machine's own -- L^, its registry, and what
    // require^ or import^ answers with. The checker refuses what is written
    // against one directly, but a table reaches a p^ through a parameter
    // typed t^{ … }, which carries no such mark (the writer has no spelling
    // for it). So the machine asks as well: an instruction written in L^ may
    // not change one. The host is unaffected -- it reaches a table through
    // this file's own API rather than through an instruction.
    bool sealed;
} LhatTable;

// 04 の 2.4: what a kind is, is where it was declared. Two errordef^ bodies
// may spell NotFound identically and still be different kinds, so identity is
// this object rather than anything about its shape. One of these is made per
// kind and one per declaration; `group` is what tells them apart.
typedef struct LhatErrorKind {
    LhatObject header;
    const struct LhatErrorKind *group;  // NULL when this object is the group
    const LhatString *name;             // "IOError" or "IOError.NotFound"
} LhatErrorKind;

// 04 の 2.3: a kind, plus the fields the construction gave it. message and
// cause live in the same table as the declared fields; nothing separates
// them, because 2.3 gives every kind both without declaring either.
typedef struct LhatError {
    LhatObject header;
    const LhatErrorKind *kind;
    LhatTable *fields;
} LhatError;

// 5.11: one suspended frame, which is all 02 の 15.5 leaves room for -- a
// yield^ is always written in the body of the procedure it suspends, so there
// is never more than one frame to keep.
typedef enum {
    LHAT_COROUTINE_FRESH,      // never resumed; the body has not started
    LHAT_COROUTINE_SUSPENDED,  // stopped at a yield^
    LHAT_COROUTINE_RUNNING,
    LHAT_COROUTINE_DONE
} LhatCoroutineState;

// 02 の 16.3: what `in^` walks is a coroutine, and a table answers with one
// of its own. That one has no body to run -- resuming it reads the next pair
// -- so the machine tells the two apart here rather than inventing a second
// kind of iterator.
typedef enum {
    LHAT_COROUTINE_BODY,   // a yieldable p^ (15.5)
    LHAT_COROUTINE_TABLE   // walking a table's keys, built in
} LhatCoroutineSource;

typedef struct LhatCoroutine {
    LhatObject header;
    const LhatClosure *closure;
    LhatCoroutineState state;
    LhatCoroutineSource source;

    // LHAT_COROUTINE_TABLE only: where the walk has reached. The dense part
    // comes first in index order, then the rest.
    const LhatTable *walking;
    size_t at_array;
    size_t at_entry;

    LhatValue *registers;  // the saved frame, as wide as the body needs
    size_t register_count;
    size_t pc;
    uint8_t sent_into;     // the register a resume's value arrives in

    // 02 の 10.7: what a discarded coroutine still has to run.
    size_t cleanups[LHAT_COROUTINE_CLEANUPS];
    size_t cleanup_count;

    // 02 の 10.7: the collector is the last place a discarded coroutine's
    // cleanups can be reached from, so one found unreachable with some still
    // pending is put on the machine's list of them rather than freed. Only
    // meaningful while it is on that list -- the same intrusive link
    // LhatUpvalue.next_open is, and kept for the same reason: the list has
    // no bound worth fixing and the objects are already there to hold it.
    struct LhatCoroutine *next_pending;
} LhatCoroutine;

// 03 の 2.1 keeps a type tag on every value, which is what lets a written
// type be asked about one while the program runs. Three decided features want
// that: 02 の 13.11's is^, 03 の 3.3's relaxed checks, and 02 の 14.12's
// overloading. The descriptor is built by the compiler and owned by the chunk.
typedef enum {
    LHAT_TYPE_RT_ANY,        // 13.7, and an unannotated parameter
    LHAT_TYPE_RT_NIL,
    LHAT_TYPE_RT_BOOL,
    LHAT_TYPE_RT_NUMBER,     // 14.8: one type, two representations
    LHAT_TYPE_RT_STRING,
    LHAT_TYPE_RT_TABLE,
    LHAT_TYPE_RT_SUBROUTINE,
    LHAT_TYPE_RT_COROUTINE,
    LHAT_TYPE_RT_ERROR,      // 04 の 2.3: any kind
    LHAT_TYPE_RT_ERROR_KIND, // one kind, or one declaration's union of them
    // 05 の 8.8: a host type, whose identity is its tag and nothing else
    // (7.3's exception for an opaque value). Written like any other name, so
    // it belongs here rather than beside a question of its own.
    LHAT_TYPE_RT_HOSTDATA,
    LHAT_TYPE_RT_UNION,      // 13.5
    LHAT_TYPE_RT_INTERSECT,  // 14.5, 14.12: an overload^ed member's arms
    LHAT_TYPE_RT_STRUCTURE,  // 14.10: at least these members

    // 03 の 3.4: a place inference did not decide. Asks nothing of a value,
    // exactly as ANY does -- what differs is only how 14.16 writes it out.
    // 05 の 8.7 requires a signature to read back as a type expression, and
    // this one deliberately does not: there is no such hat identifier, so a
    // signature carrying it is visibly not a type, which is the honest
    // answer when part of it was never decided. Which member or parameter it
    // was is what the writer needs, and that is what this shows.
    LHAT_TYPE_RT_UNKNOWN
} LhatRuntimeTypeKind;

// STRUCTURE's named half. Given its own tag rather than left anonymous inside
// LhatRuntimeType, so a comparator (qsort, for 14.16's canonical order) has
// a type to take a pointer to.
typedef struct LhatRuntimeTypeMember {
    const LhatString *name;
    struct LhatRuntimeType *type;
} LhatRuntimeTypeMember;

typedef struct LhatRuntimeType {
    LhatObject header;
    LhatRuntimeTypeKind kind;

    const LhatErrorKind *error_kind;  // ERROR_KIND

    // HOSTDATA. Lives on the program rather than the heap, exactly as
    // LhatHostData.tag does, so the collector never follows it either. The
    // struct itself is written further down, next to the values it tags.
    const struct LhatHostDataTag *hostdata_tag;

    // UNION: the arms. SUBROUTINE: the parameter types, in order -- a
    // signature's parameters are a list the same way a union's arms are, and
    // the two kinds never mix, so reusing the field costs nothing.
    struct LhatRuntimeType **parts;
    size_t part_count;

    // SUBROUTINE only. 02 の 14.16's typeof^ is what first needed a
    // signature reconstructed in full; is^'s narrowing never asked for more
    // than "is this a subroutine" (LHAT_TYPE_RT_SUBROUTINE above), so these
    // are NULL/false wherever nothing built them.
    struct LhatRuntimeType *result;   // NULL when nothing is returned (13.2)
    bool is_function;                 // f^ rather than p^ (15 章)
    bool takes_self;                  // 14.4: the first parameter is self^

    // COROUTINE only (13.9). `result` above doubles as the third slot (T);
    // these are the other two. NULL/any^ wherever nothing built them (S28).
    struct LhatRuntimeType *receive;  // R
    struct LhatRuntimeType *produce;  // Y

    // 13.7's unbounded tail, one type throughout. STRUCTURE: the sequence
    // half beyond `parts`. SUBROUTINE: the element type of the last
    // parameter, kept apart from `parts` the way LhatType.v.func.variadic is
    // kept apart from `params` -- a call owes at least `part_count`, not
    // exactly it. NULL wherever nothing is variadic.
    struct LhatRuntimeType *variadic;

    // STRUCTURE. A member with no type asks only that the name is there.
    LhatRuntimeTypeMember *members;
    size_t member_count;
} LhatRuntimeType;

// 02 の 14.12: one name, several signatures. 14.12 forbids them overlapping,
// so at most one fits a call and the search stops at the first that does --
// no ranking, no ambiguity, no order to define.
typedef struct LhatOverload {
    LhatObject header;
    LhatValue *candidates;
    size_t count;
    size_t capacity;
} LhatOverload;

// An operation the runtime provides rather than the program. 02 の 12.6 and
// 15.6 give a coroutine these; the rest of the standard library is M2.
typedef enum {
    LHAT_NATIVE_START,    // 15.2改: runs a fresh coroutine from the top
    LHAT_NATIVE_RESUME,
    LHAT_NATIVE_DISPOSE,
    LHAT_NATIVE_ITERATE,  // 02 の 16.3: answers the coroutine `in^` walks
    // 15.6改: the two questions asked before choosing an operation. Neither
    // runs the body, so both answer whatever state the coroutine is in.
    LHAT_NATIVE_DONE,     // the body has run to its end
    LHAT_NATIVE_STARTED,  // start() has already run, so resume() is the way on
    // 05 の 8.6: what L^ carries. Reaching the collector by hand is the one
    // thing a program cannot arrange for itself.
    LHAT_NATIVE_COLLECTGARBAGE,
    // 02 の 14.17: every value carries this one, not just a coroutine.
    LHAT_NATIVE_TOSTRING
} LhatNativeKind;

typedef struct LhatNative {
    LhatObject header;
    LhatNativeKind kind;
    LhatValue bound;  // what it was reached through
} LhatNative;

// 05 の 8.7: a subroutine the host wrote in C. The arguments are handed over
// as an array rather than pushed one at a time -- 13.1 settles how many
// there are and what they are before anything runs, so the counting a
// dynamic language does at the boundary has nothing to do here.
//
// 04 の 12.8 makes an error a value, so this answers one. There is no
// unwinding to arrange and nothing to catch.
struct LhatMachine;

typedef LhatValue (*LhatHostFn)(struct LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count);

typedef struct LhatHost {
    LhatObject header;
    LhatHostFn call;
    void *context;   // what the registration handed over; the host owns it
    LhatValue bound;  // 14.4: the receiver, when reached as a member
    // 13.7: `parameters` is what a call owes at least, not exactly, once
    // has_variadic is set -- the same ">=" a variadic LhatProto asks for.
    // Unlike a proto's, the tail is not collected into a table: LhatHostFn is
    // handed its arguments as an array and a count already, so a variadic host
    // function reads the tail off the end of what it was given.
    uint8_t parameters;
    bool has_variadic;
    bool takes_self;
} LhatHost;

// 05 の 8.8: what tells one registered type from another while running.
// Compared by identity alone -- 7.3's rule made into an object, and the one
// thing standing between a Texture and the C code that expects a Sound. The
// names are for diagnostics and belong to the program.
typedef struct LhatHostDataTag {
    const char *module;
    const char *name;
    // 05 の 8.8: what the type registered as dispose^, or NULL when it
    // registered none and the host keeps the lifetime. Kept here rather than
    // on each value because it belongs to the type, and because the tag
    // outlives every machine that ever made one.
    LhatHostFn release;
    void *release_context;
} LhatHostDataTag;

// Something the host made. The pointer is the host's and the collector never
// looks into it; what is reachable from here is the table of members the
// registered type carries, which is where 't.width()' lands.
typedef struct LhatHostData {
    LhatObject header;
    const LhatHostDataTag *tag;
    void *pointer;
    LhatTable *members;
    // 02 の 10.7: what has already been given back is not given back again.
    bool released;
} LhatHostData;

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

// The one place an object comes into being: zeroed, given its kind and the
// heap's current white (5.12), and linked into the heap. Everything below
// goes through this, so there is one answer to what colour a new object is.
// Returns NULL when out of memory.
void *lhat_object_alloc(LhatHeap *heap, size_t size, LhatObjectKind kind);

// Both link the new object into the heap. Return NULL when out of memory.
LhatString *lhat_string_new(LhatHeap *heap, const char *text, size_t length);
LhatTable *lhat_table_new(LhatHeap *heap);

// `group` is NULL to make the object standing for a whole errordef^, and the
// group's object to make one of its kinds.
LhatErrorKind *lhat_error_kind_new(LhatHeap *heap,
                                   const LhatErrorKind *group,
                                   const LhatString *name);

// Makes the error and the table its fields live in.
LhatError *lhat_error_new(LhatHeap *heap, const LhatErrorKind *kind);

// `registers` is how wide the body's frame is, which 5.2 fixes at compile
// time -- so a coroutine's storage is known when it is made.
LhatCoroutine *lhat_coroutine_new(LhatHeap *heap, const LhatClosure *closure,
                                  size_t registers);

// 02 の 16.3: the coroutine a table answers with. It has no body; resuming it
// reads the next key and value.
LhatCoroutine *lhat_table_iterator(LhatHeap *heap, const LhatTable *table);

// Reads the next pair of a table walk, advancing it. Answers false when the
// walk is over.
bool lhat_table_walk(LhatCoroutine *walk, LhatValue *key, LhatValue *value);

LhatNative *lhat_native_new(LhatHeap *heap, LhatNativeKind kind,
                            LhatValue bound);

LhatHost *lhat_host_new(LhatHeap *heap, LhatHostFn call, void *context,
                        uint8_t parameters, bool has_variadic,
                        bool takes_self);

LhatHostData *lhat_hostdata_new(LhatHeap *heap, const LhatHostDataTag *tag,
                                void *pointer, LhatTable *members);

LhatRuntimeType *lhat_type_rt_new(LhatHeap *heap, LhatRuntimeTypeKind kind);

// Both return false only when out of memory.
bool lhat_type_rt_add_part(LhatRuntimeType *type, LhatRuntimeType *part);
bool lhat_type_rt_add_member(LhatRuntimeType *type, const LhatString *name,
                             LhatRuntimeType *member);

// 02 の 14.16: a table's own order is not writer-chosen and not stable
// across a rehash, so typeof^'s reflection puts the named members in a
// canonical (alphabetical) order once, here -- rather than leaving
// lhat_runtime_type_write's text or lhat_runtime_type_equal's comparison to
// depend on the hash table's internal layout.
void lhat_type_rt_sort_members(LhatRuntimeType *type);

// The printer for a runtime type, in 14.10's raw structural form -- what
// typeof^(x).signature answers with. Measured with (NULL, 0) first, filled
// once the caller has a buffer that size, the way lhat_report_write is.
size_t lhat_runtime_type_write(const LhatRuntimeType *type, char *out,
                               size_t capacity);

// 02 の 2811: typeof^(x) = typeof^(y) compares structurally (11.3, 14.9), not
// by the identity of the two LhatRuntimeType objects -- two calls to
// reflect_type build separate objects even for the same shape.
bool lhat_runtime_type_equal(const LhatRuntimeType *a,
                             const LhatRuntimeType *b);

// 13.11: whether the value may stand where the type is written. A NULL type
// asks nothing, which is what an unannotated parameter means.
bool lhat_value_satisfies(LhatValue value, const LhatRuntimeType *type);

LhatOverload *lhat_overload_new(LhatHeap *heap);
bool lhat_overload_add(LhatOverload *overload, LhatValue candidate);

// 02 の 14.12: a new group holding `candidate` first and then what `existing`
// held -- an override^ has to be reached before the arm it replaces, and the
// old group has to stay as it was for 14.12改's super^.
LhatOverload *lhat_overload_with_first(LhatHeap *heap,
                                       const LhatOverload *existing,
                                       LhatValue candidate);

// 04 の 2.6 and 6.1: whether `value` is an error of `kind`. A kind object
// standing for a whole errordef^ answers yes for any of its kinds, since 2.3
// makes the declaration the union of them.
bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind);

// ---------------------------------------------------------------------------
// Freeing
// ---------------------------------------------------------------------------
//
// gc.h holds the collector. What is here is what an object costs to give up,
// which the collector's sweep asks for and a chunk's own list asks for too.

// 05 の 8.8: hands a host value's pointer back to whoever registered its
// type, and answers whether it did. Does nothing for a type that registered
// no dispose^ (the host keeps the lifetime there) and nothing for one given
// back already -- 02 の 10.7's rule, which is the same rule.
//
// The release runs while the object is still whole, so it may read the
// pointer out of it. It must not reach into the L^ API: the sweep calls this
// from the middle of a collection, where the heap is half swept.
bool lhat_hostdata_release(LhatObject *object, struct LhatMachine *machine);

// Frees one object and whatever it owns, but not what it refers to.
void lhat_object_free(LhatObject *object);

// Frees the whole list and empties it.
void lhat_object_free_all(LhatHeap *heap);

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// 02 の 11.6: '=' on two strings asks whether they say the same thing. Which
// makes a table key work by what it spells rather than which copy it is, so
// t.foo and t["foo"] reach the same place even when they were compiled into
// separate constants.
bool lhat_string_equal(const LhatString *a, const LhatString *b);

uint32_t lhat_string_hash(const char *text, size_t length);

// 02 の 11.2: '..' on two strings. The bytes are copied into a new string;
// neither operand is touched.
LhatString *lhat_string_concat(LhatHeap *heap, const LhatString *left,
                               const LhatString *right);

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

// 04 の 11.3: a table is a mapping, so there is no such thing as out of
// range. A key that is not there answers nil^.
//
// 02 の 14.7: an instance sees its definition's members too, so what is not
// among the instance's own fields is looked for there.
LhatValue lhat_table_get(const LhatTable *table, LhatValue key);

// Storing nil^ removes the key, which is what keeps "absent" and "nil^" the
// one answer 11.3 describes. Returns false only when out of memory; a key
// that cannot be a key (nil^, or a NaN) is refused with `refused` set.
bool lhat_table_set(LhatTable *table, LhatValue key, LhatValue value,
                    bool *refused);

// How many keys 1, 2, 3 ... the table holds without a gap.
size_t lhat_table_length(const LhatTable *table);

#endif  // LHAT_OBJECT_H
