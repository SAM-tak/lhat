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

// 5.11: one suspended frame, which is all 02 の 15.5 leaves room for -- a
// yield^ is always written in the body of the procedure it suspends, so there
// is never more than one frame to keep.
typedef enum {
    LHAT_COROUTINE_FRESH,      // never resumed; the body has not started
    LHAT_COROUTINE_SUSPENDED,  // stopped at a yield^
    LHAT_COROUTINE_RUNNING,
    LHAT_COROUTINE_DONE
} LhatCoroutineState;

#define LHAT_COROUTINE_CLEANUPS 32

typedef struct LhatCoroutine {
    LhatObject header;
    const LhatClosure *closure;
    LhatCoroutineState state;

    LhatValue *registers;  // the saved frame, as wide as the body needs
    size_t register_count;
    size_t pc;
    uint8_t sent_into;     // the register a resume's value arrives in

    // 02 の 10.7: what a discarded coroutine still has to run.
    size_t cleanups[LHAT_COROUTINE_CLEANUPS];
    size_t cleanup_count;
} LhatCoroutine;

// An operation the runtime provides rather than the program. 02 の 12.6 and
// 15.6 give a coroutine two of them; the rest of the standard library is M2.
typedef enum {
    LHAT_NATIVE_RESUME,
    LHAT_NATIVE_DISPOSE
} LhatNativeKind;

typedef struct LhatNative {
    LhatObject header;
    LhatNativeKind kind;
    LhatValue bound;  // what it was reached through
} LhatNative;

// ---------------------------------------------------------------------------
// Making and freeing
// ---------------------------------------------------------------------------

// Both link the new object into *owner. Return NULL when out of memory.
LhatString *lhat_string_new(LhatObject **owner, const char *text, size_t length);
LhatTable *lhat_table_new(LhatObject **owner);

// `group` is NULL to make the object standing for a whole errordef^, and the
// group's object to make one of its kinds.
LhatErrorKind *lhat_error_kind_new(LhatObject **owner,
                                   const LhatErrorKind *group,
                                   const LhatString *name);

// Makes the error and the table its fields live in.
LhatError *lhat_error_new(LhatObject **owner, const LhatErrorKind *kind);

// `registers` is how wide the body's frame is, which 5.2 fixes at compile
// time -- so a coroutine's storage is known when it is made.
LhatCoroutine *lhat_coroutine_new(LhatObject **owner, const LhatClosure *closure,
                                  size_t registers);

LhatNative *lhat_native_new(LhatObject **owner, LhatNativeKind kind,
                            LhatValue bound);

// 04 の 2.6 and 6.1: whether `value` is an error of `kind`. A kind object
// standing for a whole errordef^ answers yes for any of its kinds, since 2.3
// makes the declaration the union of them.
bool lhat_error_is_kind(LhatValue value, const LhatErrorKind *kind);

// Frees one object and whatever it owns, but not what it refers to.
void lhat_object_free(LhatObject *object);

// Frees the whole list and empties it.
void lhat_object_free_all(LhatObject **owner);

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// 02 の 11.6: '=' on two strings asks whether they say the same thing. Which
// makes a table key work by what it spells rather than which copy it is, so
// t.foo and t["foo"] reach the same place even when they were compiled into
// separate constants.
bool lhat_string_equal(const LhatString *a, const LhatString *b);

uint32_t lhat_string_hash(const char *text, size_t length);

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
