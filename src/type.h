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

typedef enum {
    // Inference has not decided. Conforms to everything and nothing is
    // reported against it, so one failure does not produce a cascade. 03 の
    // 3.5 also lands here: under relaxed these become runtime checks.
    LHAT_TYPE_UNKNOWN,

    LHAT_TYPE_ANY,         // any^ (13.7)
    LHAT_TYPE_NIL,
    LHAT_TYPE_BOOL,
    LHAT_TYPE_NUMBER,      // 14.8: one type over integers and reals
    LHAT_TYPE_STRING,

    LHAT_TYPE_TABLE,       // t^{ ... }: at least these members (14.10)
    LHAT_TYPE_FUNC,        // f^ and p^ (15 章)
    LHAT_TYPE_CORO,        // c^{ ... } (13.9)

    LHAT_TYPE_ERROR,       // error^ (04 の 2.3): the supertype of every kind
    LHAT_TYPE_ERROR_SET,   // what one errordef^ declares (04 の 2.2)
    LHAT_TYPE_ERROR_KIND,  // one kind within a set

    LHAT_TYPE_UNION,       // A | B  (13.5)
    LHAT_TYPE_INTERSECT,   // A & B  (14.5)

    LHAT_TYPE_KIND_COUNT
} LhatTypeKind;

typedef struct LhatType LhatType;

// A named member of a table or the fields a kind declares. The name points
// into the source text, which therefore has to outlive the types.
typedef struct LhatTypeMember {
    const char *name;
    size_t name_length;
    LhatType *type;
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

    // For display and diagnostics only. 14.9: a name never takes part in
    // identity, so nothing below reads this. Error kinds are the exception
    // and keep their name in `error` instead.
    const char *label;
    size_t label_length;

    union {
        struct {
            LhatTypeMember *members;
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
        } func;

        struct {
            LhatType *receive;
            LhatType *produce;
            LhatType *result;
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
LhatType *lhat_type_func(LhatTypeArena *arena, bool is_function);
LhatType *lhat_type_coro(LhatTypeArena *arena, LhatType *receive,
                         LhatType *produce, LhatType *result);

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

// Appends to a parameter list or to a set's kinds.
bool lhat_type_add_param(LhatTypeArena *arena, LhatType *func, LhatType *param);

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

// Mutual conformance. Used to drop redundant union arms.
bool lhat_type_equal(const LhatType *a, const LhatType *b);

// 14.12: is there no value at all that inhabits both? Overload resolution
// requires this of every pair of signatures, and narrowing reads it to decide
// which arms of a union survive a branch.
bool lhat_type_disjoint(const LhatType *a, const LhatType *b);

const char *lhat_type_kind_name(LhatTypeKind kind);

#endif  // LHAT_TYPE_H
