// L^ (lhat) -- the runtime representation of a value.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed with "02".
//
// 2.1: a value carries its own type. 2.2 makes that a tagged union rather
// than NaN boxing, so a 64-bit integer fits without being put on the heap.
//
// **Nothing outside this header touches the representation.** 2.2 keeps NaN
// boxing available as a later swap, and that only stays true while the rest
// of the runtime goes through the constructors and accessors below.

#ifndef LHAT_VALUE_H
#define LHAT_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 2.2. Integer and real are separate tags but one type: 02 の 14.8 makes
// number^ a single type whose representation may differ at run time.
typedef enum {
    LHAT_VALUE_NIL,
    LHAT_VALUE_BOOL,
    LHAT_VALUE_INTEGER,
    LHAT_VALUE_REAL,
    LHAT_VALUE_OBJECT
} LhatValueTag;

// What a heap value is. Kept on the object rather than in the tag so that a
// value stays one word plus a tag whatever gets added here.
typedef enum {
    LHAT_OBJECT_STRING,
    LHAT_OBJECT_TABLE,       // 02 の 10.7: the one data structure
    LHAT_OBJECT_SUBROUTINE,  // f^ and p^ (02 の 15 章)
    LHAT_OBJECT_COROUTINE,   // 02 の 13.9
    LHAT_OBJECT_ERROR,       // 04 の 2.3
    LHAT_OBJECT_ERROR_KIND,  // 04 の 2.4: the declaration a kind belongs to,
                             // which is what its identity is
    LHAT_OBJECT_NATIVE,      // an operation the runtime provides, bound to
                             // whatever it was reached through
    LHAT_OBJECT_TYPE,        // 02 の 14.12: a written type, kept so that a
                             // value can be tested against it while running
    LHAT_OBJECT_OVERLOAD,    // 02 の 14.12: the signatures one name carries
    LHAT_OBJECT_UPVALUE,     // 5.4: never a value, only reached through a
                             // closure -- but allocated and collected alike
    LHAT_OBJECT_HOST,        // 05 の 8.7: a subroutine the host wrote in C.
                             // 04 の 12.8 makes an error a value, so it
                             // answers one rather than unwinding
    LHAT_OBJECT_HOSTDATA     // 05 の 8.8: something the host made and holds,
                             // reached through a pointer the collector does
                             // not look into
} LhatObjectKind;

typedef struct LhatObject {
    LhatObjectKind kind;
    bool marked;              // reached in the collection now running
    struct LhatObject *next;  // every object, for the collector to walk
} LhatObject;


typedef struct {
    union {
        bool boolean;
        int64_t integer;
        double real;
        LhatObject *object;
    } as;
    LhatValueTag tag;
} LhatValue;

// 5.4: a place shared between a frame and the closures made inside it. While
// the frame lives, `location` points into it; once the frame goes, the value
// is carried into `closed` and `location` is aimed there. 02 の 8.6 is what
// requires the sharing -- a ':=' inside a nested subroutine has to reach the
// outer binding, which a copy would not.
typedef struct LhatUpvalue {
    LhatObject header;
    LhatValue *location;
    LhatValue closed;
    struct LhatUpvalue *next_open;  // the machine's open list, innermost first
} LhatUpvalue;

// A compiled body together with the places it captured. The proto is shared
// between every closure made from it; only the upvalues differ.
typedef struct LhatClosure {
    LhatObject header;
    const struct LhatProto *proto;
    LhatUpvalue **upvalues;
    size_t upvalue_count;
} LhatClosure;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static inline LhatValue lhat_nil(void)
{
    LhatValue v;
    v.tag = LHAT_VALUE_NIL;
    v.as.integer = 0;
    return v;
}

static inline LhatValue lhat_bool(bool b)
{
    LhatValue v;
    v.tag = LHAT_VALUE_BOOL;
    v.as.boolean = b;
    return v;
}

static inline LhatValue lhat_integer(int64_t i)
{
    LhatValue v;
    v.tag = LHAT_VALUE_INTEGER;
    v.as.integer = i;
    return v;
}

static inline LhatValue lhat_real(double d)
{
    LhatValue v;
    v.tag = LHAT_VALUE_REAL;
    v.as.real = d;
    return v;
}

static inline LhatValue lhat_object(LhatObject *o)
{
    LhatValue v;
    v.tag = LHAT_VALUE_OBJECT;
    v.as.object = o;
    return v;
}

// ---------------------------------------------------------------------------
// Questions
// ---------------------------------------------------------------------------

static inline bool lhat_is_nil(LhatValue v)     { return v.tag == LHAT_VALUE_NIL; }
static inline bool lhat_is_bool(LhatValue v)    { return v.tag == LHAT_VALUE_BOOL; }
static inline bool lhat_is_integer(LhatValue v) { return v.tag == LHAT_VALUE_INTEGER; }
static inline bool lhat_is_real(LhatValue v)    { return v.tag == LHAT_VALUE_REAL; }
static inline bool lhat_is_object(LhatValue v)  { return v.tag == LHAT_VALUE_OBJECT; }

// 02 の 14.8: one type, two representations. Code asking "is this a number^"
// has to accept either, and asking which representation is a separate
// question with its own predicate above.
static inline bool lhat_is_number(LhatValue v)
{
    return v.tag == LHAT_VALUE_INTEGER || v.tag == LHAT_VALUE_REAL;
}

static inline bool lhat_is_object_kind(LhatValue v, LhatObjectKind kind)
{
    return v.tag == LHAT_VALUE_OBJECT && v.as.object != NULL &&
           v.as.object->kind == kind;
}

// ---------------------------------------------------------------------------
// Extraction. Each is only valid when the matching question answered yes.
// ---------------------------------------------------------------------------

static inline bool lhat_as_bool(LhatValue v)       { return v.as.boolean; }
static inline int64_t lhat_as_integer(LhatValue v) { return v.as.integer; }
static inline double lhat_as_real(LhatValue v)     { return v.as.real; }
static inline LhatObject *lhat_as_object(LhatValue v) { return v.as.object; }

// The numeric value whichever representation it has. Reading an integer as a
// double loses exactness past 2^53, so this is for the places that genuinely
// want one number, not a shortcut around the two predicates.
static inline double lhat_number_as_real(LhatValue v)
{
    return v.tag == LHAT_VALUE_INTEGER ? (double)v.as.integer : v.as.real;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

// Equality as '=' means it (02 の 11.6). Two numbers compare as numbers even
// when their representations differ, since 14.8 makes them one type; an
// object compares by identity until the collector and the string table give
// a better answer.
bool lhat_value_equal(LhatValue a, LhatValue b);

// Identity as 'is^' means it (02 の 11.6改). Unlike lhat_value_equal, a
// string is never special-cased into a content comparison and a type object
// is never special-cased into a structural one -- every object answers only
// to itself, on purpose, so this stays the same once '=' moves to a real
// value comparison for tables and strings.
bool lhat_value_same(LhatValue a, LhatValue b);

const char *lhat_value_tag_name(LhatValueTag tag);

// 03 の 4 章: a prompt answers with a value, so something has to write one
// down. Writes `value` into `out` and answers how long the whole text is --
// snprintf's contract, so a caller may ask with a capacity of zero first.
// `out` may be NULL when `capacity` is zero. Always terminates the buffer
// when there is room for it.
//
// A string is written quoted, so that 1 and "1" read differently at a prompt.
// A table is written the way 14 章 has one written: its sequence half in
// order, then its keyed half. Nesting stops at a depth of its own, which is
// what keeps a table holding itself from being followed for ever.
size_t lhat_value_write(LhatValue value, char *out, size_t capacity);
const char *lhat_object_kind_name(LhatObjectKind kind);

#endif  // LHAT_VALUE_H
