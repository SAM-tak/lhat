// L^ (lhat) -- the runtime representation of a value.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed with "02".
//
// 2.1: a value carries its own type. 2.2 makes that a tagged union rather
// than NaN boxing, so a 64-bit integer fits without being put on the heap;
// 2.2 then split payloads from tags for bulk storage (LhatSlots below),
// which is also what 05 の 8.9's host values stand on -- consecutive payload
// slots are plain contiguous bytes.
//
// **Nothing outside this header touches the representation.** That only
// stays true while the rest of the runtime goes through the constructors
// and accessors below.

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
    LHAT_VALUE_OBJECT,
    // 05 の 8.9: the head slot of a host value -- a host-defined value type
    // living in consecutive stack slots rather than on the heap. The payload
    // is the registered LhatHostValueTag, which knows how many continuation
    // slots follow; the raw bytes live in those. Only ever a stack register:
    // the checker keeps one out of every place that outlives a frame.
    LHAT_VALUE_HOSTVALUE,
    // 05 の 8.9: a continuation slot of a host value. The payload is raw
    // bytes belonging to the head before it; no reader gives one a meaning
    // of its own, and every bulk copy moves it exactly as any other slot.
    LHAT_VALUE_CONT,
    // 02 の 13.8改: the head slot of a tuple -- the several values a
    // subroutine answers with, laid in the consecutive stack slots the caller
    // reserved instead of in a table on the heap. The payload is the number
    // of positions, which follow in the next slots.
    //
    // Not LHAT_VALUE_CONT for those positions: a host value's continuation
    // slots are bytes with no meaning of their own, while a tuple's positions
    // are ordinary values, each keeping its own tag so the collector reads
    // them as values. The head is what makes the width self-describing, which
    // is what lets '(A, B)|SomeError' reserve one run and tell the two arms
    // apart by the tag sitting in this slot.
    //
    // Never a value a name can hold: the machine puts one down and the next
    // few instructions take it apart. 13.8改 confines it to a result, and the
    // checker refuses every place it could escape to.
    LHAT_VALUE_RUN
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
    // 5.12: white, gray or black, and which of the two whites. gc.h says
    // what each means and what the invariant between them is.
    uint8_t color;
    struct LhatObject *next;  // every object, for the collector to walk

    // 5.12: the collector's own list of what it has reached but not yet
    // looked into. Kept on the object rather than in an array of its own so
    // that adding to the list cannot fail -- a collection that runs out of
    // memory halfway is a bad enough position while it is stop-the-world,
    // and an unaffordable one once it runs a step at a time. Lua's
    // 'gclist' is this field, put per type rather than in the header;
    // here the eight bytes a string does not need buy one rule instead of
    // twelve. Meaningful only while the object is on the list.
    struct LhatObject *gclist;
} LhatObject;


// 2.2: the payload on its own, so that bulk storage can keep payloads and
// tags in two parallel runs (LhatSlots below) instead of paying eight bytes
// of padding per value for a tag that needs three bits.
typedef union LhatValueUnion {
    bool boolean;
    int64_t integer;
    double real;
    LhatObject *object;
    // 05 の 8.9: what a LHAT_VALUE_HOSTVALUE head carries *in a stack slot*.
    // The tag lives on the program, exactly as LhatHostData.tag does, so the
    // collector never follows it.
    const struct LhatHostValueTag *hostvalue;
    // 05 の 8.9: what a LHAT_VALUE_HOSTVALUE value carries *at the host
    // boundary* -- an argument handed to a host function, or the value one
    // answers with. It aims at a head-shaped run: run[0].hostvalue is the
    // tag, run[1..] the bytes. The machine expands one back into slots the
    // moment the call answers, so nothing holds this form for longer than
    // the call.
    const union LhatValueUnion *hostvalue_run;
    // 05 の 8.9: a continuation slot's eight bytes, and the unit host-value
    // bytes are copied in and out by.
    uint8_t bytes[8];
} LhatValueUnion;

typedef struct {
    LhatValueUnion as;
    LhatValueTag tag;
} LhatValue;

// 2.2: a run of values stored as two parallel arrays -- the payloads
// aligned and dense, the tags one byte each. This is the shape every bulk
// store takes: a table's array part, the machine's stack, a coroutine's
// saved registers. LhatValue stays the currency handed around by value,
// and these two functions are the only bridge.
typedef struct {
    LhatValueUnion *values;
    uint8_t *tags;
} LhatSlots;

static inline LhatValue lhat_slots_get(LhatSlots slots, size_t i)
{
    LhatValue v;
    v.as = slots.values[i];
    v.tag = (LhatValueTag)slots.tags[i];
    return v;
}

static inline void lhat_slots_set(LhatSlots slots, size_t i, LhatValue v)
{
    slots.values[i] = v.as;
    slots.tags[i] = (uint8_t)v.tag;
}

// 2.2: one slot of a run, named so it can be held. What an upvalue's
// location is now -- the payload and tag pointers travel as a pair, and the
// pair may aim into the machine's stack, a coroutine's saved registers, or
// the upvalue's own closed cell, which is what 5.4's sharing and 15.4's
// suspension both need. Never a raw LhatValue pointer: bulk storage holds
// no LhatValue to point at.
typedef struct {
    LhatValueUnion *value;
    uint8_t *tag;
} LhatValueRef;

static inline LhatValueRef lhat_slots_ref(LhatSlots slots, size_t i)
{
    LhatValueRef r;
    r.value = slots.values + i;
    r.tag = slots.tags + i;
    return r;
}

static inline LhatValue lhat_ref_get(LhatValueRef r)
{
    LhatValue v;
    v.as = *r.value;
    v.tag = (LhatValueTag)*r.tag;
    return v;
}

static inline void lhat_ref_set(LhatValueRef r, LhatValue v)
{
    *r.value = v.as;
    *r.tag = (uint8_t)v.tag;
}

// 5.4: a place shared between a frame and the closures made inside it. While
// the frame lives, `location` aims into the machine's stack; once the frame
// goes, the value is carried into the closed cell and `location` is aimed
// there. 02 の 8.6 is what requires the sharing -- a ':=' inside a nested
// subroutine has to reach the outer binding, which a copy would not.
typedef struct LhatUpvalue {
    LhatObject header;
    LhatValueRef location;
    // The closed cell, one slot in the shape bulk storage keeps them --
    // location is a pair of pointers, and pointing it here needs the tag
    // to be the byte it points at, not an enum inside an LhatValue.
    LhatValueUnion closed_value;
    uint8_t closed_tag;
    struct LhatUpvalue *next_open;  // the machine's open list, innermost first

    // 15.4 with 5.4: while the frame that owns the slot is suspended,
    // `location` points into that coroutine's saved registers rather than
    // into the stack, and this says whose. The coroutine has to outlive the
    // capture (gc.c reaches it through this), and every resume clears it
    // when the slot moves back onto the stack. NULL everywhere else.
    struct LhatCoroutine *suspended_in;
} LhatUpvalue;

// The closed cell seen as a one-slot ref, which is what closing aims
// location at -- the same uniform dereference open and closed.
static inline LhatValueRef lhat_upvalue_closed_ref(LhatUpvalue *upvalue)
{
    LhatValueRef r;
    r.value = &upvalue->closed_value;
    r.tag = &upvalue->closed_tag;
    return r;
}

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

// 13.8改: the head slot of a tuple. The positions are in the slots after it
// and are ordinary values; this one only says how many.
static inline LhatValue lhat_run_head(size_t positions)
{
    LhatValue v;
    v.tag = LHAT_VALUE_RUN;
    v.as.integer = (int64_t)positions;
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
static inline bool lhat_is_hostvalue(LhatValue v) { return v.tag == LHAT_VALUE_HOSTVALUE; }
// 13.8改: the head of a tuple, whose payload is how many positions follow it.
static inline bool lhat_is_run(LhatValue v)     { return v.tag == LHAT_VALUE_RUN; }
static inline size_t lhat_run_width(LhatValue v)
{
    return v.tag == LHAT_VALUE_RUN ? (size_t)v.as.integer : 0;
}

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
static inline const struct LhatHostValueTag *lhat_as_hostvalue_tag(LhatValue v)
{
    return v.as.hostvalue;
}

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

// 02 の 14.8: how far apart two reals may be and still be one number. A
// division carries a few units in the last place away from the answer it
// names -- '8/(3-8/3)' lands 1.07e-14 short of 24 -- and a writer comparing
// what arithmetic produced should not have to say so at every comparison.
//
// Eight times what a double's mantissa can resolve (2^-52). Four times the
// worst relative error measured across ordinary expressions, and at the size
// of 24 about twelve units in the last place.
#define LHAT_NUMBER_TOLERANCE (8.0 * 2.220446049250313e-16)

// Equality as '=' means it (02 の 11.6). Two numbers compare as numbers even
// when their representations differ, since 14.8 makes them one type; an
// object compares by identity until the collector and the string table give
// a better answer.
//
// **Exact.** This is what a table key, a constant pool slot and 'is^' are
// matched by, none of which may admit an error term -- see lhat_value_close
// for the one '=' itself uses.
bool lhat_value_equal(LhatValue a, LhatValue b);

// 02 の 14.8: the same question '=' and the four orderings ask, which admits
// the error a real carries. Where both sides are numbers and either is a
// real, two are one number when
//
//     |a - b| <= tolerance * max(|a|, |b|, 1.0)
//
// and otherwise this is lhat_value_equal. The floor of 1.0 is what makes a
// difference that should have cancelled compare equal to zero; its cost is
// that everything well under 1.0 compares equal to everything else that is.
// A writer wanting the exact question has 'is^'; one wanting a different
// error term has number^'s own eq.
//
// Integers are left alone: two of them name themselves exactly, and a key or
// an index would not survive a band around it.
bool lhat_value_close(LhatValue a, LhatValue b, double tolerance);

// Identity as 'is^' means it (02 の 11.6改). Unlike lhat_value_equal, a
// string is never special-cased into a content comparison and a type object
// is never special-cased into a structural one -- every object answers only
// to itself, on purpose, so this stays the same once '=' moves to a real
// value comparison for tables and strings.
//
// 02 の 14.8 with 13.11: a number answers for its representation here and
// nowhere else. '=' reads 1 and 1.0 as one number and admits an error term
// besides; this reads them as what the machine is holding, so it is the one
// question with no error in it at all.
bool lhat_value_same(LhatValue a, LhatValue b);

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

// 02 の 14.17: what tostring() answers -- the text a reader reads, where
// lhat_value_write gives the spelling L^ would read back. They differ for
// the three that name themselves as literals: a string^ is its own bytes
// rather than a quoted spelling (03 の 4 章 quotes so that 1 and "1" read
// apart at a prompt, which turning a value into text has no need to do),
// and nil^ and bool^ are the words "nil", "true" and "false" without the
// hats that make them literals.
//
// Only the value handed over is read that way. One inside a table is
// written as the table spells it, since that is the table's spelling.
//
// Follows lhat_value_write's contract: asks with (NULL, 0) first.
size_t lhat_value_text(LhatValue value, char *out, size_t capacity);

// 02 の 14.17: a number^ written through the format a program wrote.
//
// The format is checked rather than handed to snprintf as it stands: it must
// carry exactly one conversion and that conversion must be a numeric one, or
// 'tostring(1, "%s")' would read the number as a pointer. The length
// modifier is the machine's to supply -- the value is an int64_t whatever
// this platform calls one, so a program spelling 'l' would be naming a width
// it cannot know. 14.8: the conversion decides how the number is read, so
// either representation answers either family.
//
// Answers false when the format is not one a number^ can be written through,
// and fills nothing then. Otherwise `*needed` is the length the whole text
// wants, the same as snprintf's answer.
bool lhat_number_format(LhatValue value, const char *format,
                        size_t format_length, char *out, size_t capacity,
                        size_t *needed);

// 02 の 14.17改2: the same format read backwards -- what lhat_number_format
// writes, this reads. snprintf on one side, sscanf on the other.
//
// The format is checked the same way and for the same reason, but against
// what sscanf spells rather than what snprintf does: the two differ over the
// length modifier a double wants, and scanf has no precision and no printf
// flags. '*' is refused there too, where it suppresses the assignment and
// would leave nothing written.
//
// Two answers, because two things can go wrong and they are not the same
// mistake. False: the format is not one a number^ can be read through -- the
// writer's error, and *read is untouched. True with *read false: the format
// was fine and the text did not match it -- data, which 14.17改2 answers
// nil^ for. True with *read true: *out holds the number^, an integer or a
// real as 14.8 says the conversion asked for.
bool lhat_number_scan(const char *text, size_t length, const char *format,
                      size_t format_length, LhatValue *out, bool *read);

const char *lhat_object_kind_name(LhatObjectKind kind);

#endif  // LHAT_VALUE_H
