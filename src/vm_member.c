// L^ (lhat) -- member lookup, overload selection and runtime reflection.

#include "vm_internal.h"
#include <string.h>
#include "lhat/token.h"
#include "operators.h"
#include "type.h"
#include "lhat/port.h"

// 02 の 11.8: an operator is a member whose name is the operator itself, and
// this is the spelling call_operator looks a candidate up by. Also reused by
// vm_finish() to name a panicking instruction (04 の 11.6) for a host, which is
// where LHAT_BC_ASCAST answers too even though 11.6's as^ is not one of
// 11.8's overloadable operators and never reaches call_operator itself.
// 11.6改3 left it here: a mismatch is an answer now rather than a fault, but
// the instruction can still fault for want of room to build that answer in.
// NULL for every other instruction.
const char *vm_operator_name(LhatOpcode op, size_t *length)
{
    switch (op) {
#define LHAT_OPERATOR_CASE(opk, bc, spelling, len) \
    case LHAT_BC_##bc:                             \
        *length = (len);                           \
        return spelling;
        LHAT_OPERATOR_MEMBERS(LHAT_OPERATOR_CASE)
#undef LHAT_OPERATOR_CASE
        // 02 の 11.8改: the unary '-' is the same member name as the binary
        // one, told apart by taking no argument. The table above is keyed by
        // instruction, and NEG is not in it -- SUB holds that spelling.
        case LHAT_BC_NEG:
            *length = 1;
            return "-";
        case LHAT_BC_ASCAST:
            *length = 3;
            return "as^";
        default:
            *length = 0;
            return NULL;
    }
}
// The same question for a read. 05 の 8.8: what a host value carries is the
// registered type's table, so 't.width()' finds the member there -- but the
// table belongs to the type and not to the value, so a write must not reach
// it. That is why this is separate from vm_table_of rather than part of it.
const LhatTable *vm_readable_table(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return ((const LhatHostData *)lhat_as_object(value))->members;
    }
    return vm_table_of(value);
}
// 02 の 14.16: what typeof^ answers where no checked type was
// compiled in -- the value's TAG, the dispatch information every value
// already carries, read in O(1). It never walks a structure: a table is
// t^ whatever it holds (the deep answer is the checker's to give, at
// compile time), and a subroutine or coroutine answers from the types its
// proto already carries, which were made at compile time too (03 の 5.11a).
// The one deep-looking case, an overload's arms, is bounded by the arm
// count rather than by any value's size.
LhatRuntimeType *vm_tag_type(LhatHeap *heap, LhatValue value)
{
    if (lhat_is_nil(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_NIL);
    }
    if (lhat_is_bool(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_BOOL);
    }
    if (lhat_is_number(value)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_NUMBER);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_STRING);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_COROUTINE)) {
        // 13.9: R and Y have no written form, so wherever they are
        // known at all it is through 03 の 5.11a's checked_type, already
        // converted onto the originating proto by compile_subroutine.
        // Reused directly (not copied) the same way the SUBROUTINE branch
        // below already reuses proto->result_type -- the chunk that owns
        // it outlives every machine that ever reflects one of its values.
        const LhatCoroutine *coroutine = (const LhatCoroutine *)lhat_as_object(value);
        const LhatProto *proto =
            coroutine->closure != NULL ? coroutine->closure->proto : NULL;
        LhatRuntimeType *type = lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
        if (type != NULL && proto != NULL) {
            type->receive = proto->yield_receive_type;
            type->produce = proto->yield_produce_type;
            type->result = proto->result_type;
            type->endless = proto->yield_endless;  // 13.9
            // 15.3改: which kind of body this came from, which is what
            // decides who may advance it (15.6改).
            type->is_function = proto->is_function;
        }
        return type;
    }
    // 02 の 19 章: an enumerator's identity is its declaration and place.
    if (lhat_is_object_kind(value, LHAT_OBJECT_ENUMERATOR)) {
        const LhatEnumerator *e = (const LhatEnumerator *)lhat_as_object(value);
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_ENUM_MEMBER);
        if (type != NULL && e->owner != NULL && e->owner->decl != NULL) {
            type->enum_decl = e->owner->decl->enum_decl;
            type->enum_member_index = e->index;
            type->enum_name = e->name;
            type->enum_owner_name = e->owner->name;
        }
        return type;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_ENUM)) {
        const LhatEnum *made = (const LhatEnum *)lhat_as_object(value);
        LhatRuntimeType *type = lhat_type_rt_new(heap, LHAT_TYPE_RT_ENUM);
        if (type != NULL && made->decl != NULL) {
            type->enum_decl = made->decl->enum_decl;
            type->enum_name = made->name;
        }
        return type;
    }
    // 04 の 2.4: an error's identity is the declaration, so what typeof^
    // answers with is the kind object itself -- 05 の 7 の 7.4's IOError.NotFound.
    if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
        const LhatError *error = (const LhatError *)lhat_as_object(value);
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR_KIND);
        if (type != NULL) {
            type->error_kind = error->kind;
        }
        return type;
    }
    // 14.5, 14.12: a multi-dispatched member is callable every way its arms
    // list, which is what '&' means -- 14.12 prints one the same way.
    if (lhat_is_object_kind(value, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *overload =
            (const LhatOverload *)lhat_as_object(value);
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_INTERSECT);
        if (type == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < overload->count; i++) {
            LhatRuntimeType *arm = vm_tag_type(heap, overload->candidates[i]);
            if (arm == NULL || !lhat_type_rt_add_part(type, arm)) {
                return NULL;
            }
        }
        return type;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        const LhatClosure *closure = (const LhatClosure *)lhat_as_object(value);
        const LhatProto *proto = closure->proto;
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_SUBROUTINE);
        if (type == NULL) {
            return NULL;
        }
        type->is_function = proto->is_function;
        type->takes_self = proto->takes_self;
        // 13.7: the last slot collects rather than taking one argument for
        // itself, so it is kept apart from `parts` here the same way
        // v.func.variadic is kept apart from a checked type's own params.
        uint8_t fixed_end = proto->has_variadic ? proto->parameters - 1
                                                : proto->parameters;
        // 14.4: self^ is not a parameter of the signature -- it is the
        // receiver, written out only where 14.4 already says so. 11.3改: the
        // slot it occupies is the last one when it was written there.
        uint8_t first = proto->takes_self && !proto->self_last ? 1 : 0;
        if (proto->takes_self && proto->self_last && fixed_end > 0) {
            fixed_end--;
        }
        for (uint8_t i = first; i < fixed_end; i++) {
            LhatRuntimeType *param = proto->parameter_types != NULL
                                         ? proto->parameter_types[i]
                                         : NULL;
            if (param == NULL) {
                // 13.7: nothing written asks for the top type, not nothing.
                param = lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
            }
            if (param == NULL || !lhat_type_rt_add_part(type, param)) {
                return NULL;
            }
        }
        if (proto->has_variadic) {
            LhatRuntimeType *element =
                proto->parameter_types != NULL
                    ? proto->parameter_types[fixed_end]
                    : NULL;
            type->variadic = element != NULL
                                 ? element
                                 : lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
            if (type->variadic == NULL) {
                return NULL;
            }
        }
        // 15.5: a call to a yielding body does not run it -- it makes the
        // coroutine 13.9 describes, and that is what the caller receives, so
        // that is this signature's result. The same three slots the branch
        // above hands back for the coroutine itself.
        if (proto->yields) {
            LhatRuntimeType *made =
                lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
            if (made == NULL) {
                return NULL;
            }
            made->receive = proto->yield_receive_type;
            made->produce = proto->yield_produce_type;
            made->result = proto->result_type;
            made->endless = proto->yield_endless;  // 13.9
            made->is_function = proto->is_function;
            type->result = made;
            return type;
        }
        type->result = proto->result_type;
        return type;
    }
    // 14.16: a table answers 13.7's unstructured top of tables, whatever
    // it holds -- deep shape is the checker's answer, given at compile time,
    // and the run does not build one out of data.
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return lhat_type_rt_new(heap, LHAT_TYPE_RT_TABLE);
    }
    // 05 の 8.8: a host value's identity is its tag, another pointer read.
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTDATA);
        if (type != NULL) {
            type->hostdata_tag =
                ((const LhatHostData *)lhat_as_object(value))->tag;
        }
        return type;
    }
    // 05 の 8.9: the same off the head slot's tag.
    if (lhat_is_hostvalue(value)) {
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE);
        if (type != NULL) {
            type->hostvalue_tag = lhat_as_hostvalue_tag(value);
        }
        return type;
    }
    // 05 の 8.9: and the box, off its own head slot.
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTVALUE_BOX)) {
        LhatRuntimeType *type =
            lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE_BOX);
        if (type != NULL) {
            type->hostvalue_tag = lhat_hostvalue_box_tag(
                (const LhatHostValueBox *)lhat_as_object(value));
        }
        return type;
    }
    // A type-info value itself, a runtime operation, … -- nothing more to
    // say than that a value is there.
    return lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);
}

// 05 の 8.9: which of the box's two members a key names. get answers the
// value whole; set writes one of the same tag over the bytes.
static bool box_member_named(LhatValue key, LhatNativeKind *out)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    if (name->length == 3 && memcmp(name->text, "get", 3) == 0) {
        *out = LHAT_NATIVE_BOX_GET;
        return true;
    }
    if (name->length == 3 && memcmp(name->text, "set", 3) == 0) {
        *out = LHAT_NATIVE_BOX_SET;
        return true;
    }
    return false;
}

// The operations 02 の 12.6 and 15.6 give a coroutine, and 14.17's tostring,
// which every value carries. The rest of the standard library is M2 and will
// not go through here.
//
// 14.17改 and 16.3改: the two a table carries are written with a hat as well.
// `hatted` says which spelling reached here, which is what builtin_member
// needs -- the bare one is a name a writer may mean for something else.
static bool native_named(LhatValue key, LhatNativeKind *out, bool *hatted)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return false;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    // Hats are accepted only for names that can also occur on tables.
    // builtin_member decides whether the receiver requires that spelling.
    static const struct {
        const char *word;
        size_t length;
        LhatNativeKind kind;
        bool accepts_hat;
    } words[] = {
        { "start", 5, LHAT_NATIVE_START, false },
        { "resume", 6, LHAT_NATIVE_RESUME, false },
        { "dispose", 7, LHAT_NATIVE_DISPOSE, false },
        { "done", 4, LHAT_NATIVE_DONE, false },
        { "started", 7, LHAT_NATIVE_STARTED, false },
        { "iterate", 7, LHAT_NATIVE_ITERATE, true },
        { "tostring", 8, LHAT_NATIVE_TOSTRING, true },
        { "tonumber", 8, LHAT_NATIVE_TONUMBER, false },
        { "eq", 2, LHAT_NATIVE_EQ, false },
        { "floor", 5, LHAT_NATIVE_FLOOR, false },
        { "ceil", 4, LHAT_NATIVE_CEIL, false },
        { "round", 5, LHAT_NATIVE_ROUND, false },
        { "abs", 3, LHAT_NATIVE_ABS, false },
        { "sign", 4, LHAT_NATIVE_SIGN, false },
        { "clamp", 5, LHAT_NATIVE_CLAMP, false },
        { "substring", 9, LHAT_NATIVE_SUBSTRING, false },
        { "substr", 6, LHAT_NATIVE_SUBSTRING, false },
        { "sub", 3, LHAT_NATIVE_SUBSTRING, false },
        { "at", 2, LHAT_NATIVE_AT, false },
        { "find", 4, LHAT_NATIVE_FIND, false },
        { "findall", 7, LHAT_NATIVE_FINDALL, false },
        { "replace", 7, LHAT_NATIVE_REPLACE, false },
        { "split", 5, LHAT_NATIVE_SPLIT, false },
        { "toupper", 7, LHAT_NATIVE_TOUPPER, false },
        { "tolower", 7, LHAT_NATIVE_TOLOWER, false },
        { "keys", 4, LHAT_NATIVE_KEYS, true },
        { "values", 6, LHAT_NATIVE_VALUES, true },
        { "join", 4, LHAT_NATIVE_JOIN, true },
        { "indexof", 7, LHAT_NATIVE_INDEXOF, true },
        { "contains", 8, LHAT_NATIVE_CONTAINS, true },
        { "slice", 5, LHAT_NATIVE_SLICE, true },
        { "clone", 5, LHAT_NATIVE_CLONE, true },
        { "insert", 6, LHAT_NATIVE_INSERT, true },
        { "push", 4, LHAT_NATIVE_PUSH, true },
        { "extend", 6, LHAT_NATIVE_EXTEND, true },
        { "remove", 6, LHAT_NATIVE_REMOVE, true },
        { "pop", 3, LHAT_NATIVE_POP, true },
        { "sort", 4, LHAT_NATIVE_SORT, true },
        { "stablesort", 10, LHAT_NATIVE_STABLESORT, true },
        { "move", 4, LHAT_NATIVE_MOVE, true },
        { "reverse", 7, LHAT_NATIVE_REVERSE, true },
        { "clear", 5, LHAT_NATIVE_CLEAR, true },
    };
    *hatted = name->length > 0 && name->text[name->length - 1] == '^';
    size_t length = name->length - (*hatted ? 1 : 0);
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        if (length == words[i].length &&
            (!*hatted || words[i].accepts_hat) &&
            memcmp(name->text, words[i].word, length) == 0) {
            *out = words[i].kind;
            return true;
        }
    }
    return false;
}

// 02 の 14.18: the three that are not operations at all -- a value's own
// shape, answered as a number with no call written. Kept out of native_named
// on purpose: that is the table of words a LhatNative is built for, and
// nothing is built for these.
//
// `hatted` says which spelling reached here, the way native_named answers it
// and for the same reason: on a table the bare word is the writer's, so only
// the hat spelling is this. A string^ takes either -- nothing can be written
// on one for the implementation to take.
typedef enum {
    COUNTED_NONE,
    COUNTED_LENGTH,  // the run: a table's dense half, a string's code points
    COUNTED_COUNT,   // a table altogether
    COUNTED_SIZE     // a string's bytes
} CountedKind;

static CountedKind counted_named(LhatValue key, bool *hatted)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return COUNTED_NONE;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    static const struct {
        const char *word;
        size_t length;
        CountedKind kind;
    } words[] = {
        { "length", 6, COUNTED_LENGTH },
        { "len", 3, COUNTED_LENGTH },
        { "count", 5, COUNTED_COUNT },
        { "size", 4, COUNTED_SIZE },
    };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        size_t n = words[i].length;
        if (name->length == n + 1 && name->text[n] == '^' &&
            memcmp(name->text, words[i].word, n) == 0) {
            *hatted = true;
            return words[i].kind;
        }
        if (name->length == n && memcmp(name->text, words[i].word, n) == 0) {
            *hatted = false;
            return words[i].kind;
        }
    }
    return COUNTED_NONE;
}
// 14.9: a table nobody made with a def^. Every name on one is the writer's,
// which is what 14.17改 turns on -- a definition and an instance of it carry
// names 14 章 reserved, and a table literal carries none.
bool vm_plain_table(LhatValue on)
{
    if (!lhat_is_object_kind(on, LHAT_OBJECT_TABLE)) {
        return false;
    }
    const LhatTable *table = (const LhatTable *)lhat_as_object(on);
    return table->definition == NULL && !table->is_definition;
}

// 02 の 16.3: `in^ e` asks e for the coroutine to walk. A table answers with
// one over its keys and a coroutine answers with itself, and both are built
// in -- the same footing 12.6 gives dispose(). A member of that name written
// by hand wins, since lhat_table_get is asked first.
//
// 14.17改 and 16.3改: on a plain table the bare spelling is not one of these
// at all. 14.11 already writes new with a hat, and the reason is the same
// one: every name on a table the writer wrote is the writer's, so a built-in
// sitting on `tostring` there is the implementation taking a word it has no
// claim to. A def^ is the other way round -- 14 章 reserves new, tostring and
// dispose on one, which is what the hat spelling is saying -- and a value
// with no members of its own (a number^, a coroutine, an error) has no names
// to take. Those keep both spellings.
static bool builtin_member(LhatValue on, LhatValue key, LhatNativeKind *out)
{
    bool hatted = false;
    if (!native_named(key, out, &hatted)) {
        return false;
    }
    // 16.3改2 with 14.18: these two want the hat on every table, not only a
    // plain one -- `keys` and `values` are words a writer reaches for, and a
    // def^ carrying its own is the ordinary case.
    if (*out == LHAT_NATIVE_KEYS || *out == LHAT_NATIVE_VALUES) {
        return hatted && lhat_is_object_kind(on, LHAT_OBJECT_TABLE);
    }
    if (!hatted && vm_plain_table(on)) {
        return false;
    }
    // 02 の 14.22: the table's own operations belong to a plain table alone
    // -- a def^'s names are the writer's, a host type's the library's. The
    // hat rule just above already refused the bare spelling there.
    if (*out >= LHAT_NATIVE_JOIN && *out <= LHAT_NATIVE_CLEAR) {
        return vm_plain_table(on);
    }
    // 02 の 14.17: whatever the value is, it can be written down.
    if (*out == LHAT_NATIVE_TOSTRING) {
        return true;
    }
    // 14.17改2: only a string^ can be read as a number^. 14.19 and 14.19改:
    // and only one has characters to take a run or a single one of.
    if (*out == LHAT_NATIVE_TONUMBER || *out == LHAT_NATIVE_SUBSTRING ||
        *out == LHAT_NATIVE_AT || *out == LHAT_NATIVE_FIND ||
        *out == LHAT_NATIVE_FINDALL || *out == LHAT_NATIVE_REPLACE ||
        *out == LHAT_NATIVE_SPLIT || *out == LHAT_NATIVE_TOUPPER ||
        *out == LHAT_NATIVE_TOLOWER) {
        return lhat_is_object_kind(on, LHAT_OBJECT_STRING);
    }
    // 14.20: and only a number^ has an error term to say anything about.
    // 14.21: nor has anything else a whole number below or above it.
    if (*out == LHAT_NATIVE_EQ || *out == LHAT_NATIVE_FLOOR ||
        *out == LHAT_NATIVE_CEIL || *out == LHAT_NATIVE_ROUND ||
        *out == LHAT_NATIVE_ABS || *out == LHAT_NATIVE_SIGN ||
        *out == LHAT_NATIVE_CLAMP) {
        return lhat_is_number(on);
    }
    if (lhat_is_object_kind(on, LHAT_OBJECT_COROUTINE)) {
        return true;  // every one of them applies to a coroutine
    }
    return *out == LHAT_NATIVE_ITERATE &&
           (lhat_is_object_kind(on, LHAT_OBJECT_TABLE) ||
            lhat_is_object_kind(on, LHAT_OBJECT_ERROR));
}

// 02 の 14.17 with 01 の 5.4: what was written answers before the built-in
// does, and an interpolation hole asks for the hat spelling -- the one
// 14.17改 keeps a plain table from taking off the writer. Everywhere else
// there is no writer's namespace to protect: 14 章 reserves these names on a
// def^, and every name on a host type is the library's (05 の 8.8, 8.9). So
// on those the two spellings name one member, and a written bare `tostring`
// answers a hole the way 14.17 says it does.
//
// The spelling that was asked for is looked for first; the two words that
// have two spellings at all (tostring and iterate -- keys^ and values^ are
// hat-only on every table, 16.3改2 with 14.18) are then looked for under the
// other one. Answers nil^ where neither is written, which is the path the
// built-in answers on.
static LhatValue member_written(Machine *m, LhatValue on, LhatValue key,
                                const LhatTable *members)
{
    LhatValue found = lhat_table_get(members, key);
    if (!lhat_is_nil(found) || vm_plain_table(on)) {
        return found;
    }
    LhatNativeKind which;
    bool hatted = false;
    if (!native_named(key, &which, &hatted) ||
        (which != LHAT_NATIVE_ITERATE && which != LHAT_NATIVE_TOSTRING)) {
        return found;
    }
    const char *other = which == LHAT_NATIVE_ITERATE
                            ? (hatted ? "iterate" : "iterate^")
                            : (hatted ? "tostring" : "tostring^");
    LhatString *spelt = lhat_string_new(&m->objects, other, strlen(other));
    if (spelt == NULL) {
        return found;  // the built-in is still an answer; nothing is lost here
    }
    return lhat_table_get(members, lhat_object((LhatObject *)spelt));
}

// 02 の 14.12: whether this candidate takes what the call is handing over.
// The receiver is not asked about -- 14.12 keeps self^ out of the judgement
// for the same reason it keeps it out of override^'s.
bool vm_fits_call(LhatValue candidate, const LhatValue *at, uint8_t given,
                      bool method, size_t *skip)
{
    // 02 の 14.12 with 05 の 8.7: a registered function is a candidate the
    // same way a written one is. 13.4 keeps self^ out of a host's count --
    // unlike a proto's, which includes it -- so what a call wrote is compared
    // as it stands, and 11.3改's trailing self^ needs no adjustment either:
    // the receiver was never in the list to move.
    if (lhat_is_object_kind(candidate, LHAT_OBJECT_HOST)) {
        const LhatHost *host = (const LhatHost *)lhat_as_object(candidate);
        if (host->has_variadic ? (size_t)given < host->parameters
                               : (size_t)given != host->parameters) {
            return false;
        }
        size_t first = method ? 2 : 1;
        for (size_t i = 0; i < host->parameters; i++) {
            const struct LhatRuntimeType *wanted =
                host->parameter_types != NULL ? host->parameter_types[i] : NULL;
            if (!lhat_value_satisfies(at[first + i], wanted)) {
                return false;
            }
        }
        *skip = method && !host->takes_self ? 2 : 1;
        return true;
    }
    if (!lhat_is_object_kind(candidate, LHAT_OBJECT_SUBROUTINE)) {
        return false;
    }
    const LhatProto *proto =
        ((const LhatClosure *)lhat_as_object(candidate))->proto;
    if (proto == NULL) {
        return false;
    }

    size_t passed = given;
    size_t first = 1;
    size_t declared = 0;
    if (method) {
        // 5.3 lays a method call out as callee, receiver, then arguments, so
        // what was given starts at 2 either way. 14.4's self^ is the first
        // parameter and is not asked about -- the receiver is what it is.
        first = 2;
        if (proto->takes_self) {
            passed = (size_t)given + 1;
            declared = 1;
        }
    }
    if (passed != proto->parameters) {
        return false;
    }

    // 11.3改: the receiver occupies the last slot instead, so the
    // arguments are the ones before it and the walk stops one short.
    size_t stop = proto->parameters;
    if (method && proto->takes_self && proto->self_last) {
        declared = 0;
        stop = proto->parameters > 0 ? proto->parameters - 1 : 0;
    }
    for (size_t i = declared; i < stop; i++) {
        const struct LhatRuntimeType *wanted =
            proto->parameter_types != NULL ? proto->parameter_types[i] : NULL;
        if (!lhat_value_satisfies(at[first + i - declared], wanted)) {
            return false;
        }
    }
    *skip = method && !proto->takes_self ? 2 : 1;
    return true;
}

// The body a value carries, or NULL when it is not a subroutine at all.
static const LhatProto *proto_of(LhatValue value)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    return ((const LhatClosure *)lhat_as_object(value))->proto;
}

// 02 の 11.3改: whether a candidate says the receiver is the operand `wanted`
// says. A written one carries the flag on its proto and a registered one on
// the host (05 の 8.7); anything that is neither answers no order at all.
static bool candidate_self_last(LhatValue candidate, bool wanted)
{
    if (lhat_is_object_kind(candidate, LHAT_OBJECT_HOST)) {
        return ((const LhatHost *)lhat_as_object(candidate))->self_last ==
               wanted;
    }
    const LhatProto *proto = proto_of(candidate);
    return proto != NULL && proto->self_last == wanted;
}
// The operator `name` as one side carries it, when that side is the receiver.
//
// `self_last` says which spelling is being looked for: the side standing on
// the left writes its self^ first (14.4), the side standing on the right
// writes it last. One written the other way round describes the other order,
// so it is not an answer here and the search passes over it.
//
// The right side is a fallback and has to qualify to be chosen, so its lone
// candidate is asked whether it takes the other operand -- the left's is not,
// which keeps the ordinary path exactly as it was (the checker is what judges
// a written one, and 03 の 3.1's relaxed leaves it to the body).
//
// 02 の 11.8改: `given` is 1 for a binary operator and 0 for the unary '-',
// which share the one member name and are told apart by the count. A lone
// candidate of the wrong count is asked here even on the left, since the two
// shapes cannot stand in for each other the way a mistyped operand can.
OperatorLookup vm_operator_candidate(Machine *m, LhatValue side,
                                         const char *name, size_t length,
                                         LhatValue receiver, LhatValue argument,
                                         uint8_t given, bool self_last,
                                         LhatValue *picked)
{
    *picked = lhat_nil();
    const LhatTable *carrier = vm_table_of(side);
    // 05 の 8.9: a host value's operators live in the members table the
    // machine bound for its type -- the value has no heap half of its own.
    if (carrier == NULL && lhat_is_hostvalue(side)) {
        carrier = vm_hostvalue_members_of(m, lhat_as_hostvalue_tag(side));
    }
    if (name == NULL || carrier == NULL) {
        return OPERATOR_ABSENT;
    }
    LhatString *key = lhat_string_new(&m->objects, name, length);
    if (key == NULL) {
        return OPERATOR_NO_MEMORY;
    }
    LhatValue found = lhat_table_get(carrier, lhat_object((LhatObject *)key));
    if (lhat_is_nil(found)) {
        return OPERATOR_ABSENT;
    }

    // 14.12: a type may answer one operator for several right-hand types, and
    // then the member is a group. The search is the same one a call makes --
    // at most one candidate fits, so it ends at the first. 14.4's layout for
    // a method call is callee, receiver, arguments.
    LhatValue shaped[3];
    shaped[1] = receiver;
    shaped[2] = argument;
    if (lhat_is_object_kind(found, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *group = (const LhatOverload *)lhat_as_object(found);
        for (size_t i = 0; i < group->count; i++) {
            // 11.3改: which operand the receiver is has to match, whoever
            // wrote the arm. A registered one carries the flag on the host
            // rather than on a proto (05 の 8.7); everything else about the
            // two is the same question, which vm_fits_call asks.
            if (!candidate_self_last(group->candidates[i], self_last)) {
                continue;
            }
            size_t skip = 1;
            shaped[0] = group->candidates[i];
            if (vm_fits_call(group->candidates[i], shaped, given, true, &skip)) {
                *picked = group->candidates[i];
                return OPERATOR_PICKED;
            }
        }
        return OPERATOR_NO_CANDIDATE;
    }

    // 05 の 8.9: a host value's operator is a host function -- registered,
    // never written in L^. 11.3改's trailing self^ is written in the
    // signature it registered with, so it answers whichever side that said.
    if (lhat_is_object_kind(found, LHAT_OBJECT_HOST)) {
        if (!candidate_self_last(found, self_last)) {
            return OPERATOR_ABSENT;  // it answers the other order
        }
        // 11.8改: the counts have to agree here too. A registration written
        // for 'a - b' is handed one operand by '-a' otherwise, and a host
        // function reached with fewer arguments than it registered for is
        // exactly what the call path refuses. 13.4 keeps self^ out of
        // `parameters`, so the operand count is compared as it stands.
        const LhatHost *host = (const LhatHost *)lhat_as_object(found);
        if (host->has_variadic ? given < host->parameters
                               : given != host->parameters) {
            return OPERATOR_NO_CANDIDATE;
        }
        *picked = found;
        return OPERATOR_PICKED;
    }
    if (!lhat_is_object_kind(found, LHAT_OBJECT_SUBROUTINE)) {
        return OPERATOR_NOT_CALLABLE;
    }
    const LhatProto *proto = proto_of(found);
    if (proto == NULL || proto->self_last != self_last) {
        return OPERATOR_ABSENT;  // it answers the other order
    }
    // 11.8改: the counts have to agree even for a lone candidate. A '-'
    // written with self^ alone is the unary one and holds no answer for
    // 'a - b'; one that takes an argument holds none for '-a'.
    if (proto->parameters != (size_t)given + (proto->takes_self ? 1 : 0)) {
        return OPERATOR_NO_CANDIDATE;
    }
    if (self_last) {
        size_t skip = 1;
        shaped[0] = found;
        if (!vm_fits_call(found, shaped, given, true, &skip)) {
            return OPERATOR_NO_CANDIDATE;
        }
    }
    *picked = found;
    return OPERATOR_PICKED;
}
// 02 の 14.16: typeof^ from C. One reading for both sides, so a host and a
// program never learn different things about one value -- which is why
// this is vm_tag_type itself and not a second walk beside it.
const LhatRuntimeType *lhat_value_type(LhatMachine *machine,
                                       LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return NULL;
    }
    return vm_tag_type(&m->objects, value);
}

// Bind a built-in only after user-written members have been considered.
static LhatRunStatus bind_native(Machine *m, size_t into,
                                 LhatNativeKind kind, LhatValue receiver)
{
    LhatNative *native = lhat_native_new(&m->objects, kind, receiver);
    if (native == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    lhat_slots_set(m->slots, into, lhat_object((LhatObject *)native));
    return LHAT_RUN_OK;
}

LhatRunStatus vm_get_member(Machine *m, size_t into, size_t receiver,
                        size_t key_slot, LhatValue member_key,
                        LhatMemberCache *filling)
{
    LhatValue on = lhat_slots_get(m->slots, receiver);
    // 02 の 19 章: a member answers what its declaration wrote
    // -- value and enum -- and the runtime's own tostring^.
    if (lhat_is_object_kind(on, LHAT_OBJECT_ENUMERATOR)) {
        const LhatEnumerator *e =
            (const LhatEnumerator *)lhat_as_object(on);
        if (lhat_is_object_kind(member_key, LHAT_OBJECT_STRING)) {
            const LhatString *asked =
                (const LhatString *)lhat_as_object(member_key);
            // `value` alone: nothing user-written can stand
            // here to collide with, and enum^ pairs with the
            // declaring word where value^ pairs with nothing.
            if (asked->length == 5 &&
                memcmp(asked->text, "value", 5) == 0) {
                lhat_slots_set(m->slots, into, e->value);
                return LHAT_RUN_OK;
            }
            if ((asked->length == 4 &&
                 memcmp(asked->text, "enum", 4) == 0) ||
                (asked->length == 5 &&
                 memcmp(asked->text, "enum^", 5) == 0)) {
                lhat_slots_set(m->slots, into, lhat_object((LhatObject *)(void *)
                                         e->owner));
                return LHAT_RUN_OK;
            }
        }
        LhatNativeKind which;
        bool hatted = false;
        if (!native_named(member_key, &which, &hatted) ||
            which != LHAT_NATIVE_TOSTRING) {
            return LHAT_RUN_TYPE_ERROR;
        }
        return bind_native(m, into, which, on);
    }
    // 02 の 12.6 and 15.6: a coroutine answers the operations the
    // runtime provides, bound to what they came through.
    if (lhat_is_object_kind(on, LHAT_OBJECT_COROUTINE)) {
        LhatNativeKind which;
        bool hatted = false;
        if (!native_named(member_key, &which, &hatted)) {
            return LHAT_RUN_TYPE_ERROR;
        }
        return bind_native(m, into, which, on);
    }
    // 02 の 13.14改: a closure answers its ReturnType -- what a
    // call of it answers, off the reflection typeof^ makes (a
    // yielding body's is the coroutine). Only a compile that
    // never checked reaches here: the checker folds the access
    // into a constant. Any other name falls through to what a
    // closure answered before.
    if (lhat_is_object_kind(on, LHAT_OBJECT_SUBROUTINE) &&
        lhat_is_object_kind(member_key, LHAT_OBJECT_STRING) &&
        ((const LhatString *)lhat_as_object(member_key))->length ==
            10 &&
        memcmp(((const LhatString *)lhat_as_object(member_key))
                   ->text,
               "ReturnType", 10) == 0) {
        LhatRuntimeType *signature = vm_tag_type(&m->objects, on);
        if (signature == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        if (signature->result == NULL) {
            return LHAT_RUN_TYPE_ERROR;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)signature->result));
        return LHAT_RUN_OK;
    }
    // 02 の 14.16: a type-info value carries exactly one member.
    // It is not a table, so the ordinary lookup below never sees
    // it -- this is what makes '.signature' answer something.
    if (lhat_is_object_kind(on, LHAT_OBJECT_TYPE)) {
        if (!lhat_is_object_kind(member_key, LHAT_OBJECT_STRING)) {
            return LHAT_RUN_TYPE_ERROR;
        }
        const LhatString *asked =
            (const LhatString *)lhat_as_object(member_key);
        if (asked->length != 9 ||
            memcmp(asked->text, "signature", 9) != 0) {
            return LHAT_RUN_TYPE_ERROR;
        }
        const LhatRuntimeType *type =
            (const LhatRuntimeType *)lhat_as_object(on);
        size_t needed = lhat_runtime_type_write(type, NULL, 0);
        char *text = (char *)lhat_alloc(needed + 1);
        if (text == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_runtime_type_write(type, text, needed + 1);
        LhatString *written =
            lhat_string_new(&m->objects, text, needed);
        lhat_free(text);
        if (written == NULL) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
        lhat_slots_set(m->slots, into, lhat_object((LhatObject *)written));
        return LHAT_RUN_OK;
    }
    // 05 の 8.9: a host value answers a registered field off its
    // own bytes -- no host call -- and a registered member out
    // of the type's table the machine bound at install.
    if (lhat_is_hostvalue(on)) {
        const LhatHostValueTag *hv_tag = lhat_as_hostvalue_tag(on);
        const LhatHostValueField *field =
            vm_hostvalue_field_named(hv_tag, member_key);
        if (field != NULL) {
            lhat_slots_set(m->slots, into, lhat_hostvalue_field_value(
                         m->slots.values + receiver + 1, field));
            return LHAT_RUN_OK;
        }
        const LhatTable *hv_members =
            vm_hostvalue_members_of(m, hv_tag);
        if (hv_members == NULL) {
            return LHAT_RUN_TYPE_ERROR;
        }
        lhat_slots_set(m->slots, into, member_written(m, on, member_key, hv_members));
    on = lhat_slots_get(m->slots, receiver);
        // 02 の 14.17: and where the library registered none, the
        // built-in writes the value down -- a host value has no
        // spelling of its own, so what it answers is its type's
        // name (value.c's write_value).
        LhatNativeKind hv_which;
        if (lhat_is_nil(lhat_slots_get(m->slots, into)) &&
            builtin_member(on, member_key, &hv_which)) {
            return bind_native(m, into, hv_which, on);
        }
        return LHAT_RUN_OK;
    }
    // 05 の 8.9: a box answers its two members, bound the way
    // 14.17's tostring is; any other name falls through to the
    // built-ins every value answers. 8.9改: a registered field
    // reads straight off the box's bytes first, as it does off
    // a stack value's.
    if (lhat_is_object_kind(on, LHAT_OBJECT_HOSTVALUE_BOX)) {
        const LhatHostValueBox *box =
            (const LhatHostValueBox *)lhat_as_object(on);
        const LhatHostValueField *field = vm_hostvalue_field_named(
            lhat_hostvalue_box_tag(box), member_key);
        if (field != NULL) {
            lhat_slots_set(m->slots, into, lhat_hostvalue_field_value(box->run + 1, field));
            return LHAT_RUN_OK;
        }
        LhatNativeKind which;
        if (box_member_named(member_key, &which)) {
            return bind_native(m, into, which, on);
        }
    }
    const LhatTable *table = vm_readable_table(on);
    if (table == NULL) {
        // 02 の 14.17: nil^, bool^, number^ and string^ hold no
        // members of their own, but every value can be written
        // down, and 14.17改2 reads a number^ back out of a
        // string^. Nothing else reaches a value that is not a
        // table, so these two are the whole of what one answers
        // -- iterate stays the table path's, which is where a
        // value carrying fields comes through.
        LhatNativeKind bare;
        if (builtin_member(on, member_key, &bare) &&
            (bare == LHAT_NATIVE_TOSTRING ||
             bare == LHAT_NATIVE_TONUMBER ||
             bare == LHAT_NATIVE_SUBSTRING ||
             bare == LHAT_NATIVE_AT ||
             bare == LHAT_NATIVE_FIND ||
             bare == LHAT_NATIVE_FINDALL ||
             bare == LHAT_NATIVE_REPLACE ||
             bare == LHAT_NATIVE_SPLIT ||
             bare == LHAT_NATIVE_TOUPPER ||
             bare == LHAT_NATIVE_TOLOWER ||
             bare == LHAT_NATIVE_EQ ||
             bare == LHAT_NATIVE_FLOOR ||
             bare == LHAT_NATIVE_CEIL ||
             bare == LHAT_NATIVE_ROUND ||
             bare == LHAT_NATIVE_ABS ||
             bare == LHAT_NATIVE_SIGN ||
             bare == LHAT_NATIVE_CLAMP)) {
            return bind_native(m, into, bare, on);
        }
        // 02 の 14.18: and a string^ answers how long it is,
        // without a call being written. Two readings of the same
        // bytes: the code points they spell, and how many there
        // are. 14.18改: the bare word alone, since a string^ has
        // no names of its own for a hat to be keeping this off.
        bool counted_hatted = false;
        CountedKind counted = counted_named(member_key, &counted_hatted);
        if (counted != COUNTED_NONE && !counted_hatted &&
            lhat_is_object_kind(on, LHAT_OBJECT_STRING)) {
            const LhatString *text =
                (const LhatString *)lhat_as_object(on);
            if (counted == COUNTED_COUNT) {
                // A string is not a collection of elements.
                return LHAT_RUN_TYPE_ERROR;
            }
            size_t held = counted == COUNTED_SIZE
                              ? text->length
                              : lhat_string_characters(text);
            lhat_slots_set(m->slots, into, lhat_integer((int64_t)held));
            return LHAT_RUN_OK;
        }
        return LHAT_RUN_TYPE_ERROR;
    }
    // 05 の 8.9改: a bare host value asks by its bytes of the
    // moment -- the stored keys are sealed boxes, and content
    // is what a box member_key means.
    if (lhat_is_hostvalue(member_key)) {
        lhat_slots_set(m->slots, into, lhat_table_get_by_value(
                     table, lhat_as_hostvalue_tag(member_key),
                     m->slots.values + key_slot + 1));
        return LHAT_RUN_OK;
    }
    // 03 の 5.1改: the one path a cache is about -- what a table
    // holds under a written name. Everything else this
    // instruction answers (a coroutine's operations, a host
    // value's fields, the built-ins every value carries) is made
    // rather than found, so there is no place to remember.
    {
        const LhatTable *found_in = NULL;
        uint32_t found_at = 0;
        bool inherited = false;
        LhatValue through = lhat_nil();
        LhatValue got = lhat_table_locate(
            table, member_key, &found_in, &found_at, &inherited,
            &through);
        // 14.7改2: found through a delegate^, so the receiver a
        // call has to pass is the delegate and not what stands
        // before the dot -- the member is the delegate's own.
        // Put where CALLMETHOD reads it.
        //
        // Written BEFORE the answer, since a site reading into
        // the register it read from ('into, into, key') means to
        // replace the receiver either way and wants the answer to
        // be what stands there.
        bool plain = vm_plain_table(on);
        if (!lhat_is_nil(through)) {
            lhat_slots_set(m->slots, receiver, through);
        }
        lhat_slots_set(m->slots, into, got);
    on = lhat_slots_get(m->slots, receiver);
        // 03 の 5.1改: only where the walk found it in a hash
        // entry, and -- for the inherited case -- where the
        // receiver itself has not been structurally written,
        // since that is what a hit will be trusting. A delegated
        // answer reports no place (object.c), so it lands here
        // as "nothing to remember".
        if (filling != NULL) {
            if (found_in != NULL &&
                (!inherited || table->version == 0)) {
                filling->answered = found_in;
                filling->version = found_in->version;
                filling->index = found_at;
                filling->from_definition = inherited;
            } else {
                filling->answered = NULL;
            }
        }
        if (!lhat_is_nil(got) || plain) {
            goto member_answered;
        }
    }
    lhat_slots_set(m->slots, into, member_written(m, on, member_key, table));
    on = lhat_slots_get(m->slots, receiver);
member_answered:;

    // 16.3: a table has an iterate of its own, but only where
    // nothing was written under that name. 14.17 gives tostring
    // the same rule, which this order is already the whole of.
    LhatNativeKind which;
    if (lhat_is_nil(lhat_slots_get(m->slots, into)) &&
        builtin_member(on, member_key, &which)) {
        return bind_native(m, into, which, on);
    }

    // 02 の 14.18: how long the run is, and how much the table
    // holds altogether. Same order again -- a written one wins,
    // and 14.18 answers only where nothing was. The hat spelling
    // alone: on a table the bare word is the writer's, whatever
    // kind of table it is.
    bool counting_hatted = false;
    CountedKind counting = COUNTED_NONE;
    if (lhat_is_nil(lhat_slots_get(m->slots, into))) {
        counting = counted_named(member_key, &counting_hatted);
        if (!counting_hatted) {
            counting = COUNTED_NONE;
        }
    }
    if (counting == COUNTED_SIZE) {
        // Bytes are a reading of a string^, not of a table.
        return LHAT_RUN_TYPE_ERROR;
    }
    if (counting != COUNTED_NONE) {
        size_t held = counting == COUNTED_COUNT
                          ? lhat_table_count(table)
                          : lhat_table_length(table);
        lhat_slots_set(m->slots, into, lhat_integer((int64_t)held));
    }
    return LHAT_RUN_OK;
}
