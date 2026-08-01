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
// makes while it runs. A collector replaces the second.

#ifndef LHAT_OBJECT_H
#define LHAT_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"

// Who an object belongs to. Two exist: a chunk, holding what its constants
// name, and the machine, holding what a program makes while it runs. Only the
// second is collected.
//
// The count is kept here rather than at the call sites so that every
// allocation is seen -- the collector decides when to run from it.
typedef struct {
    LhatObject *objects;
    size_t count;
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

// 02 の 15.11: a continuation. Calling a yieldable procedure runs the body,
// and the yield^ it reaches answers the call with one of these -- so there is
// no state before the body has started.
//
// 5.11: one suspended frame is all 02 の 15.5 leaves room for, since a yield^
// is always written in the body of the procedure it suspends. One body is one
// continuation, which is why a resume answers this same object again rather
// than a second one standing for the same frame.
typedef enum {
    LHAT_COROUTINE_SUSPENDED,  // stopped at a yield^
    LHAT_COROUTINE_RUNNING,
    LHAT_COROUTINE_DONE
} LhatCoroutineState;

#define LHAT_COROUTINE_CLEANUPS 32

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

    // 02 の 15.11: what the yield^ that made this continuation put out. The
    // caller reads it as `.result`, which is what makes a suspension usable
    // without resuming it.
    LhatValue result;

    LhatValue *registers;  // the saved frame, as wide as the body needs
    size_t register_count;
    size_t pc;
    uint8_t sent_into;     // the register a resume's value arrives in

    // 02 の 10.7: what a discarded coroutine still has to run.
    size_t cleanups[LHAT_COROUTINE_CLEANUPS];
    size_t cleanup_count;
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
    LHAT_TYPE_RT_UNION,      // 13.5
    LHAT_TYPE_RT_STRUCTURE   // 14.10: at least these members
} LhatRuntimeTypeKind;

typedef struct LhatRuntimeType {
    LhatObject header;
    LhatRuntimeTypeKind kind;

    const LhatErrorKind *error_kind;  // ERROR_KIND

    struct LhatRuntimeType **parts;   // UNION
    size_t part_count;

    // STRUCTURE. A member with no type asks only that the name is there.
    struct {
        const LhatString *name;
        struct LhatRuntimeType *type;
    } *members;
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
// 15.6 give a coroutine two of them; the rest of the standard library is M2.
typedef enum {
    LHAT_NATIVE_RESUME,
    LHAT_NATIVE_DISPOSE,
    LHAT_NATIVE_ITERATE   // 02 の 16.3: answers the coroutine `in^` walks
} LhatNativeKind;

typedef struct LhatNative {
    LhatObject header;
    LhatNativeKind kind;
    LhatValue bound;  // what it was reached through
} LhatNative;

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

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

LhatRuntimeType *lhat_type_rt_new(LhatHeap *heap, LhatRuntimeTypeKind kind);

// Both return false only when out of memory.
bool lhat_type_rt_add_part(LhatRuntimeType *type, LhatRuntimeType *part);
bool lhat_type_rt_add_member(LhatRuntimeType *type, const LhatString *name,
                             LhatRuntimeType *member);

// 13.11: whether the value may stand where the type is written. A NULL type
// asks nothing, which is what an unannotated parameter means.
bool lhat_value_satisfies(LhatValue value, const LhatRuntimeType *type);

LhatOverload *lhat_overload_new(LhatHeap *heap);
bool lhat_overload_add(LhatOverload *overload, LhatValue candidate);

// 04 の 2.6 and 6.1: whether `value` is an error of `kind`. A kind object
// standing for a whole errordef^ answers yes for any of its kinds, since 2.3
// makes the declaration the union of them.
bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind);

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------
//
// A mark and sweep that does not move anything, so nothing outside has to be
// told where a value went. 03 の 1.2 keeps Lua's incremental collector as
// something to borrow later; this is the working form it would replace.
//
// Marking uses an explicit list rather than recursion, so a deep structure
// cannot run the C stack out.

typedef struct {
    LhatObject **items;
    size_t count;
    size_t capacity;
} LhatGray;

void lhat_gray_dispose(LhatGray *gray);

// Marks the value if it is an unmarked object, and remembers it so that what
// it refers to is reached too. Returns false only when out of memory.
bool lhat_gc_reach(LhatGray *gray, LhatValue value);

// Marks everything the object refers to. Objects a chunk owns are reached
// like any other; the sweep simply never visits that list.
bool lhat_gc_children(LhatGray *gray, LhatObject *object);

// Frees what is not marked, and unmarks what is. Returns how many objects
// were freed.
size_t lhat_gc_sweep(LhatHeap *heap);

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
